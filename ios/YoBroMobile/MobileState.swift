import SwiftUI
import WebKit

func L(_ german: String, _ english: String) -> String {
    Locale.preferredLanguages.first?.hasPrefix("de") == true ? german : english
}
enum YOBROError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let value) = self { return value }; return nil }
}

struct MobileTab: Codable, Identifiable {
    var id = UUID()
    var title = "Neuer Tab"
    var url = ""
    var sourceID: UUID?
    /// Desktop organization is retained locally so synced tabs can be grouped
    /// exactly as they are on the desktop browser.
    var space: String?
    var folderID: UUID?
}
struct MobileFolder: Codable, Identifiable, Equatable {
    var id: UUID
    var name: String
    var space: String
    var collapsed: Bool
    var color: String?
}
struct MobileNote: Codable, Identifiable {
    var id = UUID()
    var title = "Neue Notiz"
    var text = ""
    var source = ""
    var sourceID: UUID?
}
struct MobileSnapshot: Codable {
    var tabs = [MobileTab()]
    var selected: UUID?
    var notes: [MobileNote] = []
    var darkWebsites = true
    var blocking = true
    var bookmarks: [MobileLink]? = []
    var history: [MobileLink]? = []
    var closedTabs: [MobileTab]? = []
    var darkExcludedHosts: [String]? = []
    /// Optional fields keep snapshots written by earlier app versions readable.
    var spaces: [String]? = []
    var selectedSpace: String?
    var folders: [MobileFolder]? = []
}
struct MobileLink: Codable, Identifiable {
    var id = UUID()
    var title: String
    var url: String
    var date = Date()
}

enum MobileAddress {
    static func resolve(_ input: String) -> URL? {
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return nil }
        if !text.contains(where: { $0.isWhitespace }), text.range(of: #"^[^/:]+\.[^/:]+:[0-9]+(/|$)"#, options: .regularExpression) != nil { return URL(string: "https://" + text) }
        if let url = URL(string: text), let scheme = url.scheme {
            return ["http", "https"].contains(scheme.lowercased()) && url.host != nil ? url : nil
        }
        if !text.contains(where: { $0.isWhitespace }), text.contains("."),
           let url = URL(string: "https://" + text), url.host != nil { return url }
        var search = URLComponents(string: "https://duckduckgo.com/")!
        search.queryItems = [URLQueryItem(name: "q", value: text)]
        return search.url
    }
}

@MainActor final class MobileAccount: ObservableObject {
    /// Stable local-only identity: never uploaded, independent from signed-in profiles.
    static let guestProfileID = UUID(uuidString: "DA8BD18E-4570-4A63-9BED-78046DB56829")!
    @Published private(set) var pairedProfileID: UUID?
    var browserProfileID: UUID { session?.userID ?? pairedProfileID ?? Self.guestProfileID }
    var isGuest: Bool { session == nil && pairedProfileID == nil }
    var isPairedDevice: Bool { session == nil && pairedProfileID != nil }
    @Published private(set) var session: SupabaseAuthSession?
    @Published var busy = false
    @Published var message: String?
    @Published private(set) var otpRequested = false
    @Published private(set) var otpEmail = ""
    @Published var needsNewPassword = false
    @Published var error: String?
    private let auth: SupabaseAuthClient
    private var passwordRecoverySession: SupabaseAuthSession?
    @Published private(set) var recoveryEmail = ""
    @Published var passwordRecoveryCompleted = false
    var canUpdatePassword: Bool { passwordRecoverySession != nil && !busy }
    private let key = "ios.account.session"
    private var generation = 0
    private var refreshTask: Task<SupabaseAuthSession, Error>?
    init(auth: SupabaseAuthClient = SupabaseAuthClient()) {
        self.auth = auth
        if let data = BrowserSyncSecrets.read(account: key) {
            session = try? JSONDecoder().decode(SupabaseAuthSession.self, from: data)
        }
        pairedProfileID = UserDefaults.standard.string(forKey: "yobro.mobile.pairedProfile").flatMap(UUID.init(uuidString:))
    }
    func requestOTP(email: String) async {
        guard !busy else { return }
        busy = true; error = nil; defer { busy = false }
        let address = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !address.isEmpty else { error = "Gib deine E-Mail-Adresse ein."; return }
        do {
            try await auth.requestEmailOTP(email: address)
            otpEmail = address; otpRequested = true
            message = "Wir haben dir einen Einmalcode gesendet."
        }
        catch { self.error = error.localizedDescription }
    }

    func verifyOTP(_ code: String) async {
        guard !busy else { return }
        let token = code.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !otpEmail.isEmpty, !token.isEmpty else { error = "Gib den Code aus deiner E-Mail ein."; return }
        busy = true; error = nil; defer { busy = false }
        do {
            try save(try await auth.verifyEmailOTP(email: otpEmail, code: token))
            otpRequested = false; message = nil
        } catch { self.error = error.localizedDescription }
    }

    func cancelOTP() {
        guard !busy else { return }
        otpRequested = false; otpEmail = ""; message = nil; error = nil
    }
    func resetPassword(email: String) async {
        guard !busy else { return }; busy = true; error = nil; defer { busy = false }
        recoveryEmail = email.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !recoveryEmail.isEmpty else { error = "Gib deine E-Mail-Adresse ein."; return }
        do { try await auth.requestPasswordReset(email: recoveryEmail); message = "Falls das Konto existiert, wurde eine E-Mail zum Zurücksetzen gesendet." }
        catch { self.error = error.localizedDescription }
    }
    func handle(_ url: URL) async {
        guard SupabaseAuthClient.isAuthURL(url) else { return }
        let recoveryLink = SupabaseAuthClient.isRecoveryURL(url)
        if recoveryLink { passwordRecoverySession = nil; needsNewPassword = true }
        busy = true; error = nil; message = nil; defer { busy = false }
        do {
            let (value, recovery) = try await auth.session(fromAuthURL: url)
            if recovery {
                passwordRecoverySession = value; recoveryEmail = value.email; needsNewPassword = true
            } else { passwordRecoverySession = nil; try save(value); needsNewPassword = false }
        } catch {
            self.error = recoveryLink
                ? "Der Rücksetzlink ist ungültig oder abgelaufen. Fordere einen neuen Link an."
                : error.localizedDescription
        }
    }
    func cancelPasswordRecovery() {
        guard !busy else { return }
        passwordRecoverySession = nil; needsNewPassword = false; error = nil; message = nil
    }
    func changePassword(_ password: String) async {
        guard !busy, password.count >= 8, var recovery = passwordRecoverySession else { return }
        busy = true; error = nil; defer { busy = false }
        do {
            if recovery.expiresAt.timeIntervalSinceNow < 60 {
                recovery = try await auth.refresh(recovery)
                passwordRecoverySession = recovery
            }
            try await auth.updatePassword(password, accessToken: recovery.accessToken)
            if session?.userID == recovery.userID {
                generation += 1; refreshTask?.cancel(); refreshTask = nil
                BrowserSyncSecrets.remove(account: key); session = nil
            }
            passwordRecoverySession = nil; needsNewPassword = false
            message = "Passwort geändert. Melde dich mit deinem neuen Passwort an."
            passwordRecoveryCompleted = true
        } catch { self.error = error.localizedDescription }
    }
    func validSession() async throws -> SupabaseAuthSession {
        guard let previous = session else { throw YOBROError.message("Bitte erneut anmelden.") }
        if previous.expiresAt.timeIntervalSinceNow >= 120 { return previous }
        let expectedGeneration = generation
        if refreshTask == nil {
            let client = auth
            refreshTask = Task { try await client.refresh(previous) }
        }
        let task = refreshTask!
        do {
            let value = try await task.value
            guard generation == expectedGeneration, session?.userID == previous.userID else { throw CancellationError() }
            if session?.refreshToken == previous.refreshToken { try save(value) }
            refreshTask = nil
            guard let current = session else { throw CancellationError() }; return current
        } catch {
            if generation == expectedGeneration { refreshTask = nil }; throw error
        }
    }
    func refresh() async {
        guard !needsNewPassword, session != nil else { return }
        do { _ = try await validSession() }
        catch is CancellationError { }
        catch { self.error = "Konto konnte nicht aktualisiert werden. Offline-Daten bleiben verfügbar. " + error.localizedDescription }
    }
    func signOut() async {
        generation += 1; refreshTask?.cancel(); refreshTask = nil
        let token = session?.accessToken
        BrowserSyncSecrets.remove(account: key); passwordRecoverySession = nil; session = nil; needsNewPassword = false; otpRequested = false; otpEmail = ""; message = nil; error = nil
        if let token { await auth.signOut(accessToken: token) }
    }
    func pair(profileID: UUID) {
        pairedProfileID = profileID
        UserDefaults.standard.set(profileID.uuidString, forKey: "yobro.mobile.pairedProfile")
    }
    private func save(_ value: SupabaseAuthSession) throws {
        try BrowserSyncSecrets.store(JSONEncoder().encode(value), account: key)
        session = value
    }
}

@MainActor final class MobileBrowser: ObservableObject {
    var handleAuthURL: ((URL) -> Void)?
    @Published var state: MobileSnapshot
    @Published var error: String?
    @Published var blockerStatus = "Werbeschutz wird vorbereitet …"
    @Published var revision = 0
    @Published var dialog: MobileDialog?
    @Published var extensionPage: MobileExtensionPage?
    @Published var protectionBusy = false
    let downloads: MobileDownloads
    let userID: UUID
    private var extensionRuntime: AnyObject?
    @available(iOS 18.6, *) var extensions: MobileExtensions? { extensionRuntime as? MobileExtensions }
    private var observations: [UUID: [NSKeyValueObservation]] = [:]
    private var views: [UUID: WKWebView] = [:]
    private var delegates: [UUID: MobileNavigation] = [:]
    private var rules: WKContentRuleList?
    private var queuedNavigation: String?
    @Published private(set) var ready = false
    private let file: URL
    private let websiteData: WKWebsiteDataStore
    var selected: MobileTab? { state.tabs.first { $0.id == state.selected } }
    var active: WKWebView? { state.selected.flatMap { views[$0] } }
    var availableSpaces: [String] {
        Array(NSOrderedSet(array: (state.spaces ?? []) + state.tabs.compactMap(\.space) + (state.folders ?? []).map(\.space)).array as? [String] ?? [])
    }
    var currentSpace: String? {
        guard !availableSpaces.isEmpty else { return nil }
        return availableSpaces.contains(state.selectedSpace ?? "") ? state.selectedSpace : availableSpaces.first
    }
    var tabsInCurrentSpace: [MobileTab] {
        guard let currentSpace else { return state.tabs }
        return state.tabs.filter { $0.space == currentSpace }
    }
    var foldersInCurrentSpace: [MobileFolder] {
        guard let currentSpace else { return [] }
        return (state.folders ?? []).filter { $0.space == currentSpace }
    }

    init(userID: UUID, home: URL? = nil) {
        let home = home ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Mobile/" + userID.uuidString, isDirectory: true)
        self.userID = userID
        downloads = MobileDownloads(home: home)
        file = home.appendingPathComponent("browser.json")
        websiteData = WKWebsiteDataStore(forIdentifier: userID)
        let loaded = PersistedState.load(MobileSnapshot.self, at: file)
        state = loaded.value ?? MobileSnapshot()
        error = loaded.problem
        if state.tabs.isEmpty { state.tabs = [MobileTab()] }
        if !state.tabs.contains(where: { $0.id == state.selected }) { state.selected = state.tabs[0].id }
        if #available(iOS 18.6, *) {
            let runtime = MobileExtensions(userID: userID, dataStore: websiteData)
            extensionRuntime = runtime; runtime.browser = self
        }
        do { try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true) }
        catch { self.error = error.localizedDescription }
    }
    func prepare() async {
        guard !ready else { return }
        do {
            rules = try await WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "yobro.mobile.basic.v1", encodedContentRuleList: MobileProtection.rules)
            blockerStatus = "Basis-Werbeblocker bereit"
        } catch { blockerStatus = "Werbeblocker nicht verfügbar: " + error.localizedDescription }
        ready = true
        for view in views.values { configure(view) }
        if let queuedNavigation { self.queuedNavigation = nil; navigate(queuedNavigation) }
        revision += 1
        // Large extension rulesets initialize after the native baseline is ready.
        // Keep the browser and local tools usable during extension startup.
        await updateProtection()
        for view in views.values { configure(view) }
        revision += 1
    }
    func updateProtection() async {
        guard !protectionBusy else { return }
        protectionBusy = true; defer { protectionBusy = false }
        if #available(iOS 18.6, *), let extensions {
            do {
                if state.blocking {
                    try await extensions.start()
                    blockerStatus = "uBlock Origin Lite aktiv"
                } else { try extensions.stop(); blockerStatus = "Werbeschutz ausgeschaltet" }
            } catch { blockerStatus = "uBlock nicht verfügbar. " + (rules == nil ? "Kein Werbeschutz verfügbar: " : "Basis-Schutz aktiv: ") + error.localizedDescription }
        } else { blockerStatus = state.blocking ? (rules == nil ? "Kein Werbeschutz verfügbar" : "Basis-Schutz aktiv · uBlock Lite ab iOS 18.6") : "Werbeschutz ausgeschaltet" }
    }
    func existingView(_ id: UUID) -> WKWebView? { views[id] }
    func select(_ id: UUID) {
        guard state.tabs.contains(where: { $0.id == id }) else { return }
        let previous = state.selected
        if previous != id { active?.pauseAllMediaPlayback(completionHandler: nil); dismissDialog() }
        state.selected = id
        if #available(iOS 18.6, *) { extensions?.selected(id, previous: previous) }
        save()
    }
    func selectSpace(_ space: String) {
        guard availableSpaces.contains(space) else { return }
        state.selectedSpace = space
        save()
    }
    func toggleFolder(_ id: UUID) {
        guard let index = state.folders?.firstIndex(where: { $0.id == id }) else { return }
        state.folders?[index].collapsed.toggle()
        save()
    }
    func webView(for id: UUID) -> WKWebView {
        if let existing = views[id] { return existing }
        return createView(id: id, configuration: WKWebViewConfiguration(), loadStoredURL: true)
    }
    private func createView(id: UUID, configuration: WKWebViewConfiguration, loadStoredURL: Bool) -> WKWebView {
        configuration.websiteDataStore = websiteData
        if #available(iOS 18.6, *) { configuration.webExtensionController = extensions?.controller }
        configuration.allowsInlineMediaPlayback = true
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.allowsBackForwardNavigationGestures = true
        view.isFindInteractionEnabled = true
        let delegate = MobileNavigation(browser: self, id: id)
        view.navigationDelegate = delegate; view.uiDelegate = delegate
        views[id] = view; delegates[id] = delegate
        observations[id] = [view.observe(\.estimatedProgress, options: [.new]) { [weak self] _, _ in
            Task { @MainActor in self?.revision += 1 }
        }, view.observe(\.url, options: [.new]) { [weak self] view, _ in
            Task { @MainActor in self?.updated(id: id, view: view, addHistory: false) }
        }]
        if #available(iOS 18.6, *) { extensions?.opened(id) }
        configure(view)
        if loadStoredURL, let tab = state.tabs.first(where: { $0.id == id }), let url = MobileAddress.resolve(tab.url) {
            view.load(URLRequest(url: url))
        }
        return view
    }
    func configure(_ view: WKWebView) {
        let controller = view.configuration.userContentController
        controller.removeAllContentRuleLists()
        var needsFallback = true
        if #available(iOS 18.6, *) { needsFallback = extensions?.isReady != true }
        if state.blocking, needsFallback, let rules { controller.add(rules) }
        controller.removeAllUserScripts()
        if let path = Bundle.main.url(forResource: "DarkReader", withExtension: "js"), let source = try? String(contentsOf: path) {
            let script = source + "\n" + MobileProtection.darkScript(enabled: state.darkWebsites, excludedHosts: state.darkExcludedHosts ?? [])
            controller.addUserScript(WKUserScript(source: script, injectionTime: .atDocumentEnd, forMainFrameOnly: false, in: .defaultClient))
            view.evaluateJavaScript(script, in: nil, in: .defaultClient, completionHandler: nil)
        }
    }
    func settingsChanged() async {
        await updateProtection()
        for view in views.values { configure(view); view.reload() }; save()
    }
    func bookmarkCurrent() {
        guard let tab = selected, let url = URL(string: tab.url), ["http", "https"].contains(url.scheme ?? "") else { return }
        var bookmarks = state.bookmarks ?? []
        if !bookmarks.contains(where: { $0.url == tab.url }) { bookmarks.insert(MobileLink(title: tab.title, url: tab.url), at: 0) }
        state.bookmarks = bookmarks; save()
    }
    func reopenClosedTab() {
        guard let tab = state.closedTabs?.popLast() else { return }
        state.tabs.append(tab); select(tab.id)
    }
    func toggleDarkForCurrentSite() async {
        guard let host = active?.url?.host?.lowercased() else { return }
        var hosts = state.darkExcludedHosts ?? []
        if hosts.contains(host) { hosts.removeAll { $0 == host } } else { hosts.append(host) }
        state.darkExcludedHosts = hosts; await settingsChanged()
    }
    func dismissDialog() { let previous = dialog; dialog = nil; previous?.finish(nil) }
    func presentDialog(_ value: MobileDialog) {
        guard value.tabID == state.selected, dialog == nil else { value.finish(nil); return }
        dialog = value
    }
    func navigate(_ input: String) {
        if let callback = URL(string: input.trimmingCharacters(in: .whitespacesAndNewlines)), SupabaseAuthClient.isAuthURL(callback) { handleAuthURL?(callback); return }
        guard ready else { queuedNavigation = input; return }
        guard let id = state.selected, let url = MobileAddress.resolve(input) else {
            error = "Bitte eine Webadresse oder einen Suchbegriff eingeben."; return
        }
        webView(for: id).load(URLRequest(url: url))
        if let index = state.tabs.firstIndex(where: { $0.id == id }) { state.tabs[index].url = url.absoluteString }
        save(); revision += 1
    }
    @discardableResult func add(url: URL? = nil) -> UUID {
        if let url, SupabaseAuthClient.isAuthURL(url) { handleAuthURL?(url); return state.selected ?? add() }
        let tab = MobileTab(url: url?.absoluteString ?? "", space: currentSpace)
        state.tabs.append(tab); select(tab.id); return tab.id
    }
    @discardableResult func add(configuration: WKWebViewConfiguration, url: URL?) -> UUID {
        let id = add(url: url)
        let view = createView(id: id, configuration: configuration, loadStoredURL: false)
        if let url { view.load(URLRequest(url: url)) }
        return id
    }
    func popup(configuration: WKWebViewConfiguration) -> WKWebView {
        let id = add()
        return createView(id: id, configuration: configuration, loadStoredURL: false)
    }
    func close(_ id: UUID) {
        if dialog?.tabID == id { dismissDialog() }
        if let tab = state.tabs.first(where: { $0.id == id }), ["https", "http"].contains(URL(string: tab.url)?.scheme ?? "") {
            state.closedTabs = Array(((state.closedTabs ?? []) + [tab]).suffix(20))
        }
        if #available(iOS 18.6, *) { extensions?.closed(id) }
        observations[id] = nil
        views[id]?.stopLoading(); views[id]?.navigationDelegate = nil; views[id]?.uiDelegate = nil
        views.removeValue(forKey: id); delegates.removeValue(forKey: id)
        state.tabs.removeAll { $0.id == id }
        if state.tabs.isEmpty { state.tabs.append(MobileTab()) }
        if state.selected == id { select(state.tabs[0].id) }; save()
    }
    func updated(id: UUID, view: WKWebView, addHistory: Bool = true) {
        guard let index = state.tabs.firstIndex(where: { $0.id == id }) else { return }
        state.tabs[index].url = view.url?.absoluteString ?? state.tabs[index].url
        state.tabs[index].title = view.title ?? "Webseite"
        if addHistory, let url = view.url, ["https", "http"].contains(url.scheme ?? "") {
            var history = state.history ?? []
            history.removeAll { $0.url == url.absoluteString }
            history.insert(MobileLink(title: view.title ?? url.host ?? "Webseite", url: url.absoluteString), at: 0)
            state.history = Array(history.prefix(500))
        }
        if #available(iOS 18.6, *) { extensions?.changed(id) }
        revision += 1; save()
    }
    func save() {
        do {
            var stored = state
            stored.tabs.removeAll { !$0.url.isEmpty && !["http", "https"].contains(URL(string: $0.url)?.scheme ?? "") }
            if stored.tabs.isEmpty { stored.tabs = [MobileTab()] }
            if !stored.tabs.contains(where: { $0.id == stored.selected }) { stored.selected = stored.tabs.first?.id }
            try JSONEncoder().encode(stored).write(to: file, options: [.atomic, .completeFileProtection])
        }
        catch { self.error = "Speichern fehlgeschlagen: " + error.localizedDescription }
    }
    func suspend() { dismissDialog(); downloads.cancelAll(); for view in views.values { view.stopLoading(); view.pauseAllMediaPlayback(completionHandler: nil) }; save() }
}

@MainActor final class MobileNavigation: NSObject, WKNavigationDelegate, WKUIDelegate {
    weak var browser: MobileBrowser?
    let id: UUID
    init(browser: MobileBrowser, id: UUID) { self.browser = browser; self.id = id }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) { browser?.updated(id: id, view: webView) }
    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        if (error as NSError).code != NSURLErrorCancelled { browser?.error = error.localizedDescription }
    }
    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        if (error as NSError).code != NSURLErrorCancelled { browser?.error = error.localizedDescription }
    }
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if let callback = navigationAction.request.url, SupabaseAuthClient.isAuthURL(callback) {
            decisionHandler(.cancel)
            if navigationAction.targetFrame?.isMainFrame != false { browser?.handleAuthURL?(callback) }
            return
        }
        let scheme = navigationAction.request.url?.scheme?.lowercased() ?? ""
        var allowed = ["https", "http", "about", "blob"].contains(scheme)
        if #available(iOS 18.6, *), let context = browser?.extensions?.context,
           let url = navigationAction.request.url, url.scheme == context.baseURL.scheme, url.host == context.baseURL.host { allowed = true }
        decisionHandler(allowed ? (navigationAction.shouldPerformDownload ? .download : .allow) : .cancel)
    }
    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse, decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        let disposition = (navigationResponse.response as? HTTPURLResponse)?.value(forHTTPHeaderField: "Content-Disposition") ?? ""
        decisionHandler(navigationResponse.canShowMIMEType && !disposition.lowercased().hasPrefix("attachment") ? .allow : .download)
    }
    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { browser?.downloads.begin(download) }
    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { browser?.downloads.begin(download) }
    func webViewDidClose(_ webView: WKWebView) { browser?.close(id) }
    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
        guard let browser else { completionHandler(); return }
        browser.presentDialog(MobileDialog(tabID: id, origin: frame.securityOrigin.host, message: message, kind: .alert, completion: { _ in completionHandler() }))
    }
    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        guard let browser else { completionHandler(false); return }
        browser.presentDialog(MobileDialog(tabID: id, origin: frame.securityOrigin.host, message: message, kind: .confirm, completion: { completionHandler($0 != nil) }))
    }
    func webView(_ webView: WKWebView, runJavaScriptTextInputPanelWithPrompt prompt: String, defaultText: String?, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (String?) -> Void) {
        guard let browser else { completionHandler(nil); return }
        browser.presentDialog(MobileDialog(tabID: id, origin: frame.securityOrigin.host, message: prompt, kind: .prompt, defaultText: defaultText ?? "", completion: completionHandler))
    }
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        guard navigationAction.targetFrame == nil, let url = navigationAction.request.url,
              ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return nil }
        return browser?.popup(configuration: configuration)
    }
}
