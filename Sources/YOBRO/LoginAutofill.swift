import SwiftUI
import WebKit

struct PendingLogin: Identifiable {
    let id = UUID()
    let entry: ImportPassword
}

@MainActor
final class LoginAutofill: NSObject, ObservableObject, WKScriptMessageHandler {
    static let world = WKContentWorld.world(name: "YOBRO.LoginAutofill")
    @Published var pending: PendingLogin?
    @Published var message: String?
    @Published private(set) var suggestionRequest: UUID?
    weak var tab: BrowserTab?
    private var stagedUsername: (origin: String, username: String, date: Date)?
    private var loginFrames: [(document: String, origin: String, frame: WKFrameInfo)] = []

    nonisolated static func origin(_ url: URL?) -> String? {
        guard let url, url.scheme?.lowercased() == "https", let host = url.host?.lowercased(), !host.isEmpty else { return nil }
        var parts = URLComponents()
        parts.scheme = "https"; parts.host = host
        if let port = url.port, port != 443 { parts.port = port }
        return parts.string
    }

    /// Apple signs users into App Store Connect in an Apple-owned frame whose
    /// host differs from the page (`idmsa.apple.com` inside
    /// `appstoreconnect.apple.com`). Keep the default exact-origin boundary and
    /// only admit this small, explicit set of Apple identity hosts.
    nonisolated static func permitsLoginFrame(origin frameOrigin: String, in pageOrigin: String) -> Bool {
        if frameOrigin == pageOrigin { return true }
        guard let frameHost = URL(string: frameOrigin)?.host?.lowercased(),
              let pageHost = URL(string: pageOrigin)?.host?.lowercased(),
              pageHost == "apple.com" || pageHost.hasSuffix(".apple.com") else { return false }
        return ["idmsa.apple.com", "appleid.apple.com", "account.apple.com"].contains(frameHost)
    }

    func install(on controller: WKUserContentController) {
        guard let url = YOBROResources.bundle.url(forResource: "LoginAutofill", withExtension: "js"), let source = try? String(contentsOf: url) else { return }
        controller.add(self, contentWorld: Self.world, name: "yobroLogin")
        // Apple and other identity providers render their form in a same-origin
        // iframe. Install in every frame; the native origin checks below still
        // reject third-party embedded login frames.
        controller.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: Self.world))
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        Self.target(for: message.webView, owner: self)?.receive(message)
    }

    /// Popups share their opener's content controller, so their messages
    /// arrive at the opener's handler. Route them to the popup tab's own
    /// autofill; private tabs never collect logins.
    static func target(for webView: WKWebView?, owner: LoginAutofill) -> LoginAutofill? {
        guard let tab = (webView as? AppearanceWebView)?.tab, tab !== owner.tab else { return owner }
        return tab.isPrivate ? nil : tab.logins
    }

    private func receive(_ message: WKScriptMessage) {
        guard let tab, message.webView === tab.webView, let owner = tab.owner,
              owner.activeID == tab.id, !owner.agentTabIDs.contains(tab.id),
              let origin = Self.origin(message.frameInfo.request.url),
              let pageOrigin = Self.origin(tab.webView.url),
              Self.permitsLoginFrame(origin: origin, in: pageOrigin),
              let body = message.body as? [String: String] else { return }
        if body["event"] == "ready", let document = body["document"], !document.isEmpty {
            loginFrames.removeAll { $0.document == document }
            loginFrames.append((document, origin, message.frameInfo))
            if loginFrames.count > 20 { loginFrames.removeFirst(loginFrames.count - 20) }
            return
        }
        if body["event"] == "focus" {
            suggestionRequest = UUID()
            self.message = nil
            return
        }
        guard let user = body["username"], let password = body["password"],
              user.count <= 1024, password.count <= 4096 else { return }
        if !user.isEmpty { stagedUsername = (origin, user, Date()) }
        guard !password.isEmpty else { return }
        let username = !user.isEmpty ? user : stagedUsername.flatMap { $0.origin == origin && Date().timeIntervalSince($0.date) < 300 ? $0.username : nil } ?? ""
        guard !username.isEmpty else { return }
        stagedUsername = nil
        considerForSaving(ImportPassword(url: origin, username: username, password: password), home: owner.home)
    }

    /// Suppress the prompt when the submitted credentials already match the
    /// Keychain. A different password for the same account still offers an
    /// update, and a later login attempt replaces an unconfirmed earlier one.
    func considerForSaving(_ entry: ImportPassword, home: URL) {
        if LoginNeverList.contains(entry.url, home: home) {
            pending = nil
            return
        }
        do {
            if try PasswordVault.entries(home: home, origin: entry.url).contains(where: {
                $0.username == entry.username && $0.password == entry.password
            }) {
                pending = nil
                message = nil
                return
            }
        } catch {
            // Saving may still succeed even when a read was temporarily denied;
            // keep the explicit user choice available in that case.
        }
        pending = PendingLogin(entry: entry)
        message = nil
        let id = pending?.id
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 120_000_000_000)
            if self?.pending?.id == id { self?.pending = nil }
        }
    }

    func save() {
        guard let pending, let home = tab?.owner?.home else { return }
        do {
            _ = try PasswordVault.store(pending.entry, home: home, replace: true)
            self.pending = nil
            message = nil
        } catch { message = error.localizedDescription }
    }

    /// Stops offering to save logins for the pending login's website.
    func never() {
        guard let pending, let home = tab?.owner?.home else { return }
        do {
            try LoginNeverList.add(pending.entry.url, home: home)
            self.pending = nil
            message = nil
        } catch { message = error.localizedDescription }
    }

    /// Focusing a login field asks for suggestions on every page. Only a
    /// click on the key explains that nothing is saved or that reading failed.
    nonisolated static func shouldPresent(automatic: Bool, entries: [ImportPassword], error: String?) -> Bool {
        !automatic || (error == nil && !entries.isEmpty)
    }

    func accounts() throws -> [ImportPassword] {
        guard let tab, let origin = Self.origin(tab.webView.url), let home = tab.owner?.home else { return [] }
        return try PasswordVault.entries(home: home, origin: origin)
    }

    static func call(_ webView: WKWebView, script: String, arguments: [String: Any] = [:], frame: WKFrameInfo? = nil) async throws -> Any {
        try await withCheckedThrowingContinuation { continuation in
            webView.callAsyncJavaScript(script, arguments: arguments, in: frame, in: Self.world) {
                continuation.resume(with: $0)
            }
        }
    }

    func fill(_ entry: ImportPassword) async throws {
        guard let tab, tab.owner?.activeID == tab.id, tab.owner?.agentTabIDs.contains(tab.id) != true,
              let origin = Self.origin(tab.webView.url), origin == Self.origin(URL(string: entry.url)) else { throw YOBROError.message(L("Die Website hat gewechselt. Bitte erneut auswählen.", "The website changed. Please select the account again.")) }
        let arguments: [String: Any] = ["origin": origin, "username": entry.username, "password": entry.password]
        // Prefer the most recently loaded matching frame, then fall back to the
        // main document for sessions created before frame discovery was added.
        for candidate in loginFrames.reversed() where candidate.origin == origin {
            do {
                let result = try await Self.call(tab.webView,
                    script: "return window.yobroLogin?.fill(origin, documentID, username, password) ?? false;",
                    arguments: arguments.merging(["documentID": candidate.document]) { _, value in value },
                    frame: candidate.frame)
                if result as? Bool == true { return }
            } catch { continue }
        }
        let document = try await Self.call(tab.webView, script: "return window.yobroLogin?.documentID ?? '';") as? String ?? ""
        if !document.isEmpty {
            let result = try await Self.call(tab.webView,
                script: "return window.yobroLogin?.fill(origin, documentID, username, password) ?? false;",
                arguments: arguments.merging(["documentID": document]) { _, value in value })
            if result as? Bool == true { return }
        }
        throw YOBROError.message(L("Kein passendes Loginformular gefunden. Die Seite wurde möglicherweise gewechselt.", "No matching login form found. The page may have changed."))
    }
}

struct LoginAutofillButton: View {
    @ObservedObject var login: LoginAutofill
    @State private var presented = false
    @State private var entries: [ImportPassword] = []
    @State private var message: String?

    private func showSuggestions(automatic: Bool = false) {
        var loaded: [ImportPassword] = []
        var failure: String?
        do { loaded = try login.accounts() } catch { failure = error.localizedDescription }
        guard LoginAutofill.shouldPresent(automatic: automatic, entries: loaded, error: failure) else { return }
        entries = loaded
        message = failure
        presented = true
    }

    var body: some View {
        Button {
            showSuggestions()
        } label: { Image(systemName: "key").font(.system(size: 13)).frame(width: 28, height: 32) }
        .buttonStyle(YOBROButtonStyle(minimumSize: 28))
        .yobroHelp(L("Gespeicherte Logins ausfüllen", "Fill saved logins"))
        .accessibilityLabel(L("Gespeicherte Logins ausfüllen", "Fill saved logins"))
        .popover(isPresented: $presented, arrowEdge: .top) {
            VStack(alignment: .leading, spacing: 12) {
                Label(L("Gespeicherte Logins", "Saved logins"), systemImage: "key.fill").font(.headline)
                Text(LoginAutofill.origin(login.tab?.webView.url) ?? L("Keine sichere Website geöffnet", "No secure website open")).font(.caption).foregroundStyle(.secondary)
                if let message { Text(message).font(.caption).foregroundStyle(.secondary) }
                if entries.isEmpty { Text(L("Noch kein Login für diese Website gespeichert. Nach dem Absenden eines Loginformulars bietet YoBro das Speichern an.", "No login saved for this website. YoBro offers to save it after you submit a login form.")).font(.callout) }
                ForEach(Array(entries.enumerated()), id: \.offset) { _, entry in
                    Button(entry.username) {
                        Task {
                            do { try await login.fill(entry); presented = false; entries = [] }
                            catch { message = error.localizedDescription }
                        }
                    }.buttonStyle(.bordered)
                }
            }.padding(18).frame(width: 300).background(paper)
        }
        .onChange(of: login.suggestionRequest) { _, request in
            if request != nil { showSuggestions(automatic: true) }
        }
        .onChange(of: presented) { _, value in if !value { entries = []; message = nil } }
        .onDisappear { entries = [] }
    }
}

struct LoginSavePrompt: View {
    @ObservedObject var login: LoginAutofill
    var body: some View {
        if let pending = login.pending {
            VStack(alignment: .leading, spacing: 10) {
                Label(L("Login in YoBro speichern?", "Save login in YoBro?"), systemImage: "key.fill").font(.headline)
                Text(pending.entry.url).font(.caption).foregroundStyle(.secondary)
                Text(pending.entry.username).font(.callout).lineLimit(2)
                Text(L("E-Mail und Passwort im macOS-Schlüsselbund speichern oder aktualisieren.", "Save or update email and password in macOS Keychain.")).font(.caption).foregroundStyle(.secondary)
                if let message = login.message { Text(message).font(.caption).foregroundStyle(.red) }
                HStack {
                    Button(L("Nicht jetzt", "Not now")) { login.pending = nil }
                    Button(L("Nie für diese Website", "Never for this website")) { login.never() }
                        .buttonStyle(.plain).font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button(L("Speichern", "Save")) { login.save() }.buttonStyle(.borderedProminent)
                }
            }.padding(16).frame(maxWidth: 300).background(paper, in: RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(ink.opacity(0.15)))
                .shadow(color: .black.opacity(0.2), radius: 16, y: 6).padding(12)
        }
    }
}

/// Websites the user excluded from the save prompt, stored per profile as
/// sorted `LoginAutofill.origin` strings.
enum LoginNeverList {
    static func file(home: URL) -> URL { home.appendingPathComponent("login-never.json") }

    static func all(home: URL) -> [String] {
        guard let data = try? Data(contentsOf: file(home: home)),
              let origins = try? JSONDecoder().decode([String].self, from: data) else { return [] }
        return origins
    }

    static func contains(_ origin: String, home: URL) -> Bool { all(home: home).contains(origin) }

    static func add(_ origin: String, home: URL) throws { try write(Set(all(home: home)).union([origin]), home: home) }

    static func remove(_ origin: String, home: URL) throws { try write(Set(all(home: home)).subtracting([origin]), home: home) }

    private static func write(_ origins: Set<String>, home: URL) throws {
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try JSONEncoder().encode(origins.sorted()).write(to: file(home: home), options: [.atomic, .completeFileProtection])
    }
}
