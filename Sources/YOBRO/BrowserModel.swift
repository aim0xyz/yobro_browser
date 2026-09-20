import SwiftUI
import WebKit
import AppKit

struct StoredTab: Codable, Equatable {
    var id: UUID
    var title: String
    var customTitle: String? = nil
    var url: String
    var space: String
    var pinned: Bool
    var folderID: UUID? = nil
    var faviconData: Data? = nil
    var closedIndex: Int? = nil
    /// `nil` keeps sessions written by older YOBRO versions compatible.
    var suspended: Bool? = nil
    /// Private tabs are never written to disk. Optional for decoding older sessions.
    var isPrivate: Bool? = nil
    /// WebKit's opaque session state: back-forward list, scroll offset and form
    /// contents. Device-local, so `withoutInteractionState` strips it before the
    /// session is uploaded.
    var interactionState: Data? = nil
    /// Optional keeps sessions written before workspace notes compatible.
    var kind: TabKind? = nil
    var noteContent: String? = nil
    var noteRTF: Data? = nil

    var withoutInteractionState: Self {
        var copy = self
        copy.interactionState = nil
        return copy
    }
}

enum TabKind: String, Codable {
    case web, note
}

struct StoredSession: Codable, Equatable {
    var tabs: [StoredTab]
    var activeID: UUID?
    var space: String
    var closedTabs: [StoredTab]?
    var spaces: [String]? = nil
    var spaceIcons: [String: String]? = nil
    var folders: [TabFolder]? = nil
    var splitPairs: [StoredSplit]? = nil
}

/// Time spans offered when clearing website data.
enum WebsiteDataRange: String, CaseIterable, Identifiable {
    case hour, day, week, everything

    var id: String { rawValue }

    var title: String {
        switch self {
        case .hour: return L("Letzte Stunde", "Last hour")
        case .day: return L("Letzter Tag", "Last day")
        case .week: return L("Letzte Woche", "Last week")
        case .everything: return L("Alles", "Everything")
        }
    }

    var start: Date {
        switch self {
        case .hour: return Date().addingTimeInterval(-3_600)
        case .day: return Date().addingTimeInterval(-86_400)
        case .week: return Date().addingTimeInterval(-604_800)
        case .everything: return .distantPast
        }
    }
}

/// Persisted policy for the local control socket. See `bridgeLibraryAccess`.
struct BridgePolicy: Codable {
    var allowsLibraryAccess = false
}

struct AgentEvent: Identifiable {
    let id = UUID()
    let date = Date()
    let action: String
    let detail: String
}

struct WebPageDialog: Identifiable {
    enum Kind: Equatable {
        case alert, confirm
        /// `window.prompt`: a single text field prefilled with the page's default.
        case prompt
        /// HTTP Basic/Digest authentication: user name plus password.
        case credentials
    }

    /// Everything a dialog can return. Unused members stay empty, which keeps
    /// the four kinds on one code path instead of four parallel completions.
    struct Response {
        var accepted: Bool
        var text: String = ""
        var password: String = ""
        static let dismissed = Response(accepted: false)
    }

    let id = UUID()
    let tabID: UUID
    let title: String
    let message: String
    let kind: Kind
    var defaultText: String = ""
    /// Styles the confirm button as a warning. Used for the certificate override.
    var destructive = false
    let completion: (Response) -> Void
}

/// A navigation failure prepared for display. WebKit's `localizedDescription`
/// alone leaves the user with a bare error number, so the common causes get a
/// headline and an icon of their own.
struct PageFailure: Equatable {
    var title: String
    var detail: String
    var symbol: String
    var canRetry: Bool

    init(title: String, detail: String, symbol: String = "exclamationmark.triangle", canRetry: Bool = true) {
        self.title = title
        self.detail = detail
        self.symbol = symbol
        self.canRetry = canRetry
    }

    /// Used for messages assigned without classification, e.g. from the agent.
    static func generic(_ detail: String) -> Self {
        Self(title: L("Diese Seite ist gerade nicht erreichbar."), detail: detail, symbol: "wifi.exclamationmark")
    }

    static func classify(_ error: Error) -> Self {
        let native = error as NSError
        let detail = error.localizedDescription
        guard native.domain == NSURLErrorDomain else { return .generic(detail) }
        switch native.code {
        case NSURLErrorNotConnectedToInternet, NSURLErrorNetworkConnectionLost, NSURLErrorDataNotAllowed:
            return Self(title: L("Keine Internetverbindung.", "No internet connection."),
                        detail: L("Prüfe WLAN oder Kabel und versuche es erneut.", "Check Wi-Fi or your cable, then try again."),
                        symbol: "wifi.slash")
        case NSURLErrorCannotFindHost, NSURLErrorDNSLookupFailed:
            return Self(title: L("Diese Adresse gibt es nicht.", "This address does not exist."),
                        detail: L("Der Servername konnte nicht aufgelöst werden. Vielleicht ein Tippfehler?", "The server name could not be resolved. Perhaps a typo?"),
                        symbol: "magnifyingglass")
        case NSURLErrorTimedOut:
            return Self(title: L("Die Seite hat zu lange gebraucht.", "The page took too long."),
                        detail: L("Der Server hat nicht rechtzeitig geantwortet.", "The server did not respond in time."),
                        symbol: "clock.badge.exclamationmark")
        case NSURLErrorCannotConnectToHost:
            return Self(title: L("Der Server nimmt keine Verbindung an.", "The server refused the connection."),
                        detail: detail, symbol: "bolt.horizontal.circle")
        case NSURLErrorServerCertificateUntrusted, NSURLErrorServerCertificateHasBadDate,
             NSURLErrorServerCertificateHasUnknownRoot, NSURLErrorServerCertificateNotYetValid,
             NSURLErrorSecureConnectionFailed:
            return Self(title: L("Die Verbindung ist nicht sicher.", "The connection is not secure."),
                        detail: L("Das Zertifikat dieser Seite ist ungültig. YoBro hat die Verbindung deshalb abgebrochen.", "This site's certificate is invalid, so YoBro stopped the connection."),
                        symbol: "lock.trianglebadge.exclamationmark", canRetry: false)
        case NSURLErrorUserAuthenticationRequired:
            return Self(title: L("Anmeldung erforderlich.", "Sign-in required."),
                        detail: L("Diese Seite verlangt einen Benutzernamen und ein Passwort.", "This page requires a user name and password."),
                        symbol: "person.badge.key")
        default:
            return .generic(detail)
        }
    }
}

@MainActor
final class BrowserTab: NSObject, ObservableObject, Identifiable, WKNavigationDelegate, WKUIDelegate {
    let id: UUID
    let logins = LoginAutofill()
    let webView: WKWebView
    @Published var title: String
    @Published var customTitle: String?
    @Published var url: String
    @Published var space: String
    @Published var pinned: Bool {
        didSet {
            // The sidebar filters the owner's tab array, so it must observe pin changes too.
            if oldValue != pinned { owner?.objectWillChange.send() }
        }
    }
    @Published var folderID: UUID?
    let kind: TabKind
    @Published var noteContent: String
    @Published var noteRTF: Data?
    @Published var favicon: NSImage?
    @Published var loading = false
    @Published var progress = 0.0
    @Published var failure: PageFailure?
    /// The failure's message. Kept as a plain string because the control bridge
    /// and the agent both read and clear it; assigning here loses the
    /// classification and falls back to the generic headline.
    var error: String? {
        get { failure?.detail }
        set { failure = newValue.map(PageFailure.generic) }
    }
    @Published var canGoBack = false
    @Published var canGoForward = false
    @Published var showFind = false
    @Published var findQuery = ""
    @Published var findFound: Bool?
    @Published var findError: String?
    @Published private(set) var isSuspended: Bool
    var lastActiveAt: Date = Date()
    let isPrivate: Bool
    weak var owner: BrowserModel?
    private var observers: [NSKeyValueObservation] = []
    private var persistedFaviconData: Data?
    /// Guards against reload loops when the web content process keeps crashing.
    private var rendererRestarts = 0
    /// Session state from disk, consumed by the first load of this tab.
    private var restorableInteractionState: Data?
    /// Session state is a few kilobytes per tab; refuse pathological payloads so
    /// a single tab cannot bloat session.json.
    private static let interactionStateLimit = 512 * 1024

    init(saved: StoredTab, owner: BrowserModel, deferLoading: Bool = false, webViewConfiguration: WKWebViewConfiguration? = nil, useConfigurationDirectly: Bool = false) {
        id = saved.id; title = saved.title; customTitle = saved.customTitle; url = saved.url; space = saved.space; pinned = saved.pinned; folderID = saved.folderID
        kind = saved.kind ?? .web
        noteContent = saved.noteContent ?? ""
        noteRTF = saved.noteRTF
        isSuspended = saved.suspended ?? false
        isPrivate = saved.isPrivate ?? false
        persistedFaviconData = saved.faviconData
        favicon = saved.faviconData.flatMap(NSImage.init(data:))
        restorableInteractionState = saved.interactionState
        self.owner = owner
        // WKWebExtensionContext returns a reusable configuration. Its
        // WKUserContentController must not be shared between BrowserTabs:
        // LoginAutofill installs a tab-owned message handler and WebKit raises
        // an Objective-C exception when the same handler name already exists.
        let config: WKWebViewConfiguration
        if useConfigurationDirectly, let webViewConfiguration {
            // WKUIDelegate requires the exact configuration supplied to
            // createWebViewWith. Using a copy breaks window.opener, which OAuth
            // popups need to deliver their result to the originating page.
            config = webViewConfiguration
        } else {
            config = (webViewConfiguration?.copy() as? WKWebViewConfiguration) ?? WKWebViewConfiguration()
        }
        if webViewConfiguration == nil {
            config.websiteDataStore = isPrivate ? .nonPersistent() : owner.websiteDataStore
        } else if !useConfigurationDirectly {
            config.userContentController = WKUserContentController()
        }
        if !useConfigurationDirectly {
            BrowserIdentity.configure(config)
            // Ad blocking is provided by the bundled uBlock Origin Lite WebExtension.
            owner.webAppearance.install(on: config.userContentController)
            if !isPrivate { owner.extensions.configure(config) }
        }
        webView = AppearanceWebView(frame: .zero, configuration: config)
        super.init()
        logins.tab = self
        if !useConfigurationDirectly && !isPrivate { logins.install(on: config.userContentController) }
        if let appWebView = webView as? AppearanceWebView {
            appWebView.tab = self
            appWebView.appearanceChanged = { [weak self] in
                guard let self else { return }; self.owner?.webAppearance.update(self.webView)
            }
        }
        webView.navigationDelegate = self
        webView.uiDelegate = self
        webView.allowsBackForwardNavigationGestures = true
        webView.isInspectable = true
        observers = [
            webView.observe(\.title, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in
                    guard let self else { return }
                    guard !self.isSuspended, !self.isExtensionsHub, view.url?.scheme != "about" else { return }
                    self.title = view.title?.isEmpty == false ? view.title! : L("Neue Seite")
                    if #available(macOS 15.4, *) { self.owner?.extensions.runtime.controller.didChangeTabProperties(.title, for: self) }
                    self.owner?.updateVisitTitle(self)
                    self.owner?.save()
                }
            },
            webView.observe(\.url, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in
                    guard let self else { return }
                    guard !self.isSuspended, !self.isExtensionsHub, view.url?.scheme != "about" else { return }
                    self.url = view.url?.absoluteString ?? ""
                    if #available(macOS 15.4, *) { self.owner?.extensions.runtime.controller.didChangeTabProperties(.URL, for: self) }
                    self.owner?.save()
                }
            },
            webView.observe(\.isLoading, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in
                    guard let self, !self.isExtensionsHub else { return }; self.loading = view.isLoading
                    if #available(macOS 15.4, *) { self.owner?.extensions.runtime.controller.didChangeTabProperties(.loading, for: self) }
                }
            },
            webView.observe(\.estimatedProgress, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in self?.progress = view.estimatedProgress }
            },
            webView.observe(\.canGoBack, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in self?.canGoBack = self?.isExtensionsHub == true ? false : view.canGoBack }
            },
            webView.observe(\.canGoForward, options: [.new]) { [weak self] view, _ in
                Task { @MainActor in self?.canGoForward = self?.isExtensionsHub == true ? false : view.canGoForward }
            }
        ]
        if !deferLoading, !isSuspended { loadPersistedContent() }
    }

    var stored: StoredTab {
        StoredTab(id: id, title: title, customTitle: customTitle, url: url, space: space, pinned: pinned, folderID: folderID,
                  faviconData: persistedFaviconData, suspended: isSuspended, isPrivate: isPrivate,
                  interactionState: capturedInteractionState, kind: kind == .web ? nil : kind,
                  noteContent: kind == .note ? noteContent : nil, noteRTF: kind == .note ? noteRTF : nil)
    }

    var isNote: Bool { kind == .note }
    var isExtensionsHub: Bool { ExtensionCatalog.isInternal(url) }

    /// A suspended tab's WebView points at a blank document, so its live state is
    /// worthless; keep whatever was captured before it was suspended.
    private var capturedInteractionState: Data? {
        if isExtensionsHub { return nil }
        if isPrivate { return nil }
        if isSuspended { return restorableInteractionState }
        guard let live = webView.interactionState as? Data else { return restorableInteractionState }
        return live.count <= Self.interactionStateLimit ? live : nil
    }

    /// Applies the stored zoom for the current host. Called on every commit so a
    /// remembered level survives navigation within the same site.
    func applyStoredZoom() {
        guard let owner else { return }
        let level = owner.pageZoom.level(for: webView.url)
        if webView.pageZoom != level { webView.pageZoom = level }
    }

    /// - Parameter direction: `+1` larger, `-1` smaller, `0` back to 100 %.
    func changeZoom(_ direction: Int) {
        guard let owner else { return }
        let applied = direction == 0
            ? owner.pageZoom.reset(for: webView.url)
            : owner.pageZoom.step(direction, for: webView.url)
        // A start page or a document without a host has nothing to remember, so
        // adjust the view directly instead of dropping the request.
        webView.pageZoom = applied ?? (direction == 0 ? PageZoomStore.standard : webView.pageZoom)
    }

    var zoomLevel: Double { webView.pageZoom }

    /// Hands the rendered page to the standard print panel. ⌘P did nothing
    /// before because no print support existed.
    func printPage() {
        let operation = webView.printOperation(with: NSPrintInfo.shared)
        operation.view?.frame = webView.bounds
        operation.showsPrintPanel = true
        operation.showsProgressPanel = true
        if let window = webView.window {
            operation.runModal(for: window, delegate: nil, didRun: nil, contextInfo: nil)
        } else {
            operation.run()
        }
    }

    /// Brings the tab's page back. A persisted interaction state also restores
    /// the back-forward list, scroll position and form contents; without it only
    /// the URL is loaded, which is what YOBRO did for every tab before.
    func loadPersistedContent() {
        guard !url.isEmpty, !isExtensionsHub else { return }
        webView.customUserAgent = BrowserIdentity.catalogUserAgent(for: URL(string: url))
        if let state = restorableInteractionState {
            restorableInteractionState = nil
            webView.interactionState = state
            // If WebKit rejected the state there is no current entry, so fall
            // through to a plain load rather than leaving the tab blank.
            if webView.backForwardList.currentItem != nil { return }
        }
        guard let target = URL(string: url) else { return }
        webView.load(URLRequest(url: target))
    }

    /// Releases the live page while retaining the tab's identity, URL and
    /// favicon in its folder (or when hibernated for memory saving).
    /// Selecting it later reloads the retained URL.
    func suspend(forced: Bool = false) {
        guard (folderID != nil || forced), !isSuspended else { return }
        // Capture the live session before navigating away, so resuming restores
        // the history and scroll position instead of a bare URL.
        if let live = webView.interactionState as? Data, live.count <= Self.interactionStateLimit {
            restorableInteractionState = live
        }
        isSuspended = true
        webView.stopLoading()
        pauseMediaPlayback()
        webView.loadHTMLString("", baseURL: nil)
        owner?.save()
    }

    func resume() {
        guard isSuspended else { return }
        lastActiveAt = Date()
        isSuspended = false
        loadPersistedContent()
        owner?.save()
    }

    /// Checks whether the tab is actively playing audio/video or recording via camera/microphone.
    func isPlayingMedia() async -> Bool {
        if #available(macOS 12.0, *) {
            if webView.cameraCaptureState != .none || webView.microphoneCaptureState != .none {
                return true
            }
        }
        return await withCheckedContinuation { continuation in
            webView.requestMediaPlaybackState { state in
                continuation.resume(returning: state == .playing)
            }
        }
    }

    /// `stopLoading()` only cancels navigation; HTML media that has already
    /// started keeps playing in WebKit. Pause it explicitly whenever a tab is
    /// no longer presented, and navigate discarded tabs away from their page
    /// so scripts cannot restart playback after the tab leaves the model.
    func pauseMediaPlayback() {
        webView.pauseAllMediaPlayback(completionHandler: nil)
    }

    func discard() {
        webView.stopLoading()
        pauseMediaPlayback()
        webView.navigationDelegate = nil
        webView.uiDelegate = nil
        webView.loadHTMLString("", baseURL: nil)
        observers.removeAll()
    }

    var sidebarTitle: String {
        if let customTitle {
            let candidate = customTitle.trimmingCharacters(in: .whitespacesAndNewlines)
            if !candidate.isEmpty { return candidate }
        }
        if isNote {
            let candidate = title.trimmingCharacters(in: .whitespacesAndNewlines)
            return candidate.isEmpty ? L("Neue Notiz", "New note") : candidate
        }
        if url.isEmpty { return L("Neue Seite", "New page") }
        let candidate = title.trimmingCharacters(in: .whitespacesAndNewlines)
        if !candidate.isEmpty && !["Neue Seite", "New page", "New tab"].contains(candidate) { return candidate }
        return URL(string: url)?.host ?? url
    }

    func rename(_ value: String) {
        let candidate = value.trimmingCharacters(in: .whitespacesAndNewlines)
        customTitle = candidate.isEmpty ? nil : candidate
        owner?.save()
    }

    var sidebarHelp: String {
        if isNote { return L("Notiz: ", "Note: ") + sidebarTitle }
        guard let host = URL(string: url)?.host, !host.isEmpty, host != sidebarTitle else { return sidebarTitle }
        return sidebarTitle + "\n" + host
    }

    func restoreFaviconIfNeeded() {
        guard favicon == nil, let pageURL = URL(string: url), !url.isEmpty else { return }
        let expectedURL = url
        Task { [weak self] in
            guard let icon = await FaviconStore.shared.image(for: pageURL), self?.url == expectedURL else { return }
            self?.setFavicon(icon)
        }
    }

    private func setFavicon(_ icon: NSImage?) {
        favicon = icon
        persistedFaviconData = icon.flatMap(FaviconStore.persistedData)
        owner?.save()
    }

    func navigate(_ input: String) throws {
        lastActiveAt = Date()
        let text = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        if let callback = URL(string: text), SupabaseAuthClient.isAuthURL(callback) {
            if let owner, !owner.agentTabIDs.contains(id) {
                Task { await owner.sync.handleAuthURL(callback, model: owner) }
            }
            return
        }
        if ExtensionCatalog.isInternal(text) {
            webView.stopLoading()
            url = ExtensionCatalog.internalURL
            webView.customUserAgent = nil
            title = L("Erweiterungen · YoBro", "Extensions · YoBro")
            error = nil; failure = nil; loading = false; favicon = nil; persistedFaviconData = nil
            restorableInteractionState = nil
            canGoBack = false; canGoForward = false; showFind = false
            if webView.url != nil { webView.loadHTMLString("", baseURL: nil) }
            owner?.save()
            return
        }
        let candidate: String
        if text.contains("://") { candidate = text }
        else if !text.contains(" ") && (text.contains(".") || text.hasPrefix("localhost")) {
            candidate = (text.hasPrefix("localhost") || text.hasPrefix("127.0.0.1") ? "http://" : "https://") + text
        } else {
            var components = URLComponents(string: "https://duckduckgo.com/")!
            components.queryItems = [URLQueryItem(name: "q", value: text)]
            candidate = components.url!.absoluteString
        }
        guard let target = URL(string: candidate), ["https", "http"].contains(target.scheme?.lowercased() ?? ""), target.host != nil else {
            throw YOBROError.message(L("Nur gültige HTTP- und HTTPS-Adressen werden unterstützt."))
        }
        error = nil; url = candidate; favicon = nil; persistedFaviconData = nil
        webView.customUserAgent = BrowserIdentity.catalogUserAgent(for: target)
        webView.load(URLRequest(url: target))
        owner?.save()
    }

    /// WebKit reports a cancelled navigation as a failure whenever a policy
    /// decision stops a load, including our own download and tracking-parameter
    /// paths. Those are not user-visible errors.
    private func record(navigationFailure error: Error) {
        guard !isExtensionsHub else { return }
        let native = error as NSError
        guard native.code != NSURLErrorCancelled,
              !(native.domain == "WebKitErrorDomain" && native.code == 102),
              // A frame-load interruption after a policy change carries no
              // useful message and would replace an already rendered page.
              !(native.domain == "WebKitErrorDomain" && native.code == 101) else { return }
        failure = PageFailure.classify(error)
    }
    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        record(navigationFailure: error)
    }
    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        record(navigationFailure: error)
    }
    func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
        applyStoredZoom()
    }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        // Suspending a folder tab navigates its WebView to an internal blank
        // document. Never let that housekeeping navigation erase the retained
        // page's favicon/title/history.
        if isSuspended || isExtensionsHub || webView.url?.scheme == "about" { error = nil; return }
        if #available(macOS 15.4, *) { owner?.extensions.runtime.controller.didChangeTabProperties([.URL, .title, .loading], for: self) }
        let pageURL = webView.url
        Task { [weak self] in
            let icon = await FaviconStore.shared.image(for: webView)
            if self?.webView.url == pageURL {
                self?.setFavicon(icon)
            }
        }
        failure = nil; rendererRestarts = 0; findFound = nil; owner?.save(); owner?.recordVisit(self); owner?.webAppearance.update(webView) }

    /// A crashed web content process leaves a blank view behind. Reload once so
    /// a transient crash heals itself, but stop after that instead of looping.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        guard !isExtensionsHub else { return }
        guard rendererRestarts < 1, !isSuspended, !url.isEmpty else {
            failure = PageFailure(title: L("Die Seite wurde beendet.", "The page stopped responding."),
                                  detail: L("Der Seiteninhalt ist mehrfach abgestürzt. Lade sie neu oder öffne sie in einem neuen Tab.", "The page content crashed repeatedly. Reload it or open it in a new tab."),
                                  symbol: "exclamationmark.arrow.triangle.2.circlepath")
            return
        }
        rendererRestarts += 1
        webView.reload()
    }
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if let startup = owner?.webStartupTask,
           ["http", "https"].contains(navigationAction.request.url?.scheme?.lowercased() ?? "") {
            Task { @MainActor [weak self] in
                await startup.value
                guard let self else { decisionHandler(.cancel); return }
                self.decideNavigationPolicy(navigationAction, decisionHandler: decisionHandler)
            }
            return
        }
        decideNavigationPolicy(navigationAction, decisionHandler: decisionHandler)
    }

    private func decideNavigationPolicy(_ navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if let callback = navigationAction.request.url, SupabaseAuthClient.isAuthURL(callback) {
            decisionHandler(.cancel)
            if navigationAction.targetFrame?.isMainFrame != false, let owner, !owner.agentTabIDs.contains(id) {
                Task { await owner.sync.handleAuthURL(callback, model: owner) }
            }
            return
        }
        let scheme = navigationAction.request.url?.scheme?.lowercased() ?? ""
        // A space proxy or the managed VPN that cannot be applied must stop the
        // request, not quietly hand it to the direct connection.
        if ["http", "https"].contains(scheme), let reason = owner?.isolationFailure {
            decisionHandler(.cancel)
            failure = PageFailure(
                title: L("Verbindung angehalten.", "Connection on hold."),
                detail: reason + "\n" + L("YoBro lädt nichts, solange der Datenverkehr nicht wie eingestellt geschützt ist. Passe VPN oder Proxy in den Einstellungen an.",
                                          "YoBro loads nothing while traffic is not protected as configured. Adjust the VPN or proxy in settings."),
                symbol: "shield.lefthalf.filled.slash"
            )
            return
        }
        if navigationAction.targetFrame?.isMainFrame == true, ["http", "https"].contains(scheme) {
            let catalogAgent = BrowserIdentity.catalogUserAgent(for: navigationAction.request.url)
            if (webView.customUserAgent ?? "") != (catalogAgent ?? "") {
                webView.customUserAgent = catalogAgent
                // Reissue only safe reads so the request uses the new identity.
                // Never replay form submissions or alter subframe navigation.
                if (navigationAction.request.httpMethod ?? "GET") == "GET" {
                    decisionHandler(.cancel)
                    var request = navigationAction.request
                    request.setValue(nil, forHTTPHeaderField: "User-Agent")
                    webView.load(request)
                    return
                }
            }
        }
        if navigationAction.shouldPerformDownload, ["http", "https", "blob"].contains(scheme) { decisionHandler(.download); return }
        var extensionURL = false
        if #available(macOS 15.4, *), let url = navigationAction.request.url { extensionURL = owner?.extensions.runtime.controller.extensionContext(for: url) != nil }
        if ["http", "https", "about"].contains(scheme) || extensionURL { decisionHandler(.allow); return }
        decisionHandler(.cancel)
        openExternally(navigationAction)
    }

    /// Schemes that belong to a well-known system app and open without asking,
    /// matching what other Mac browsers do for these links.
    private static let trustedExternalSchemes: Set<String> = [
        "mailto", "tel", "sms", "facetime", "facetime-audio", "imessage", "maps", "webcal"
    ]

    /// WebKit renders web content only, so anything else has to be handed to the
    /// system. Previously these navigations were cancelled silently and a click
    /// on a `mailto:` link simply did nothing.
    private func openExternally(_ action: WKNavigationAction) {
        guard let owner, !owner.agentTabIDs.contains(id) else { return }
        guard let url = action.request.url, let scheme = url.scheme?.lowercased() else { return }
        // Only the user may launch another app. A script-initiated or redirected
        // navigation must not be able to do it on its own.
        guard action.navigationType == .linkActivated || action.navigationType == .formSubmitted else { return }
        // Local files and script URLs never go to the system opener.
        guard !["file", "javascript", "data", "blob", "about"].contains(scheme) else { return }

        if Self.trustedExternalSchemes.contains(scheme) {
            if !NSWorkspace.shared.open(url) { owner.notice = Self.missingHandlerNotice(url) }
            return
        }
        owner.presentWebPageDialog(
            tabID: id,
            title: L("Link in einer anderen App öffnen?", "Open link in another app?"),
            message: url.absoluteString,
            kind: .confirm
        ) { [weak owner] accepted in
            guard accepted else { return }
            if !NSWorkspace.shared.open(url) { owner?.notice = Self.missingHandlerNotice(url) }
        }
    }

    private static func missingHandlerNotice(_ url: URL) -> String {
        L("Für „\(url.scheme ?? "")“-Links ist auf diesem Mac keine App installiert.",
          "No app on this Mac handles “\(url.scheme ?? "")” links.")
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse, decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        let http = navigationResponse.response as? HTTPURLResponse
        let disposition = http?.value(forHTTPHeaderField: "Content-Disposition") ?? ""
        if !navigationResponse.canShowMIMEType || disposition.lowercased().hasPrefix("attachment") {
            decisionHandler(.download); return
        }
        // A server error without a body renders as a blank page. Only an
        // explicitly empty body qualifies; a length of -1 means "unknown" and
        // real error pages must keep rendering their own content.
        if navigationResponse.isForMainFrame, let http, http.statusCode >= 400,
           navigationResponse.response.expectedContentLength == 0 {
            failure = PageFailure(
                title: L("Der Server hat einen Fehler gemeldet.", "The server reported an error."),
                detail: "HTTP \(http.statusCode) · " + HTTPURLResponse.localizedString(forStatusCode: http.statusCode),
                symbol: "exclamationmark.octagon"
            )
            decisionHandler(.cancel); return
        }
        decisionHandler(.allow)
    }
    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { owner?.downloads.track(download); error = nil }
    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { owner?.downloads.track(download); error = nil }

    @discardableResult
    func find(_ query: String, backwards: Bool = false) async -> Bool {
        findQuery = query
        let configuration = WKFindConfiguration()
        configuration.backwards = backwards; configuration.wraps = true; configuration.caseSensitive = false
        do {
            let result = try await webView.find(query, configuration: configuration)
            if findQuery == query { findFound = query.isEmpty ? nil : result.matchFound; findError = nil }
            return result.matchFound
        } catch {
            findError = error.localizedDescription; findFound = nil
            return false
        }
    }
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        guard navigationAction.targetFrame == nil, let owner else { return nil }
        let popup: BrowserTab
        if owner.agentTabIDs.contains(id) {
            guard owner.agentEnabled && owner.isProfileActive else { return nil }
            popup = owner.newAgentTab(webViewConfiguration: configuration, useConfigurationDirectly: true)
        } else {
            popup = owner.newTab(space: space, isPrivate: isPrivate, webViewConfiguration: configuration, useConfigurationDirectly: true)
        }
        return popup.webView
    }
    func webViewDidClose(_ webView: WKWebView) { owner?.closeTab(id) }
    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
        if let owner, owner.agentTabIDs.contains(id) {
            error = L("Die Agentenseite hat einen Dialog angefordert. Pausiere den Agenten, um die Seite zu übernehmen.", "The agent page requested a dialog. Pause the agent to take over the page.")
            completionHandler(); return
        }
        guard let owner, webView.window != nil else { completionHandler(); return }
        owner.presentWebPageDialog(tabID: id, title: webView.url?.host ?? L("Webseite"), message: message, kind: .alert) { (_: Bool) in completionHandler() }
    }
    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        if let owner, owner.agentTabIDs.contains(id) {
            error = L("Die Agentenseite hat eine Bestätigung angefordert. Pausiere den Agenten, um die Seite zu übernehmen.", "The agent page requested confirmation. Pause the agent to take over the page.")
            completionHandler(false); return
        }
        guard let owner, webView.window != nil else { completionHandler(false); return }
        owner.presentWebPageDialog(tabID: id, title: webView.url?.host ?? L("Webseite"), message: message, kind: .confirm, completion: completionHandler)
    }
    func webView(_ webView: WKWebView, runJavaScriptTextInputPanelWithPrompt prompt: String, defaultText: String?, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (String?) -> Void) {
        if let owner, owner.agentTabIDs.contains(id) {
            failure = PageFailure.generic(L("Die Agentenseite hat eine Eingabe angefordert. Pausiere den Agenten, um die Seite zu übernehmen.", "The agent page requested input. Pause the agent to take over the page."))
            completionHandler(nil); return
        }
        guard let owner, webView.window != nil else { completionHandler(nil); return }
        owner.presentWebPageDialog(tabID: id, title: webView.url?.host ?? L("Webseite"), message: prompt, kind: .prompt, defaultText: defaultText ?? "") { response in
            completionHandler(response.accepted ? response.text : nil)
        }
    }
    /// Without this the "leave page?" confirmation never appeared and half
    /// finished forms were discarded silently.
    func webView(_ webView: WKWebView, runBeforeUnloadConfirmPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        guard let owner, webView.window != nil, !owner.agentTabIDs.contains(id) else { completionHandler(true); return }
        owner.presentWebPageDialog(
            tabID: id,
            title: L("Diese Seite verlassen?", "Leave this page?"),
            message: message.isEmpty ? L("Nicht gespeicherte Änderungen gehen dabei verloren.", "Any unsaved changes will be lost.") : message,
            kind: .confirm,
            completion: completionHandler
        )
    }
    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters, initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping ([URL]?) -> Void) {
        // The agent never gets a file picker; it must not reach local files.
        if let owner, owner.agentTabIDs.contains(id) {
            failure = PageFailure.generic(L("Die Agentenseite wollte eine Datei auswählen. Pausiere den Agenten, um die Seite zu übernehmen.", "The agent page tried to pick a file. Pause the agent to take over the page."))
            completionHandler(nil); return
        }
        guard let window = webView.window else { completionHandler(nil); return }
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = parameters.allowsDirectories
        panel.canChooseFiles = true
        panel.prompt = L("Auswählen", "Choose")
        panel.message = L("Diese Datei wird an \(webView.url?.host ?? L("die Website", "the website")) gesendet.",
                          "This file will be sent to \(webView.url?.host ?? "the website").")
        panel.beginSheetModal(for: window) { response in
            completionHandler(response == .OK ? panel.urls : nil)
        }
    }
    /// HTTP Basic/Digest logins used to fail without explanation because no
    /// challenge handler existed. Certificate validation stays with WebKit:
    /// there is deliberately no way to accept an untrusted certificate.
    func webView(_ webView: WKWebView, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        let method = challenge.protectionSpace.authenticationMethod
        if method == NSURLAuthenticationMethodServerTrust {
            resolveServerTrust(challenge, in: webView, completionHandler: completionHandler)
            return
        }
        guard [NSURLAuthenticationMethodHTTPBasic, NSURLAuthenticationMethodHTTPDigest, NSURLAuthenticationMethodNTLM].contains(method) else {
            completionHandler(.performDefaultHandling, nil); return
        }
        guard challenge.previousFailureCount == 0 else {
            failure = PageFailure(title: L("Anmeldung fehlgeschlagen.", "Sign-in failed."),
                                  detail: L("Benutzername oder Passwort wurden nicht akzeptiert.", "The user name or password was not accepted."),
                                  symbol: "person.badge.key")
            completionHandler(.cancelAuthenticationChallenge, nil); return
        }
        guard let owner, webView.window != nil, !owner.agentTabIDs.contains(id) else {
            completionHandler(.performDefaultHandling, nil); return
        }
        let space = challenge.protectionSpace
        let realm = space.realm ?? ""
        owner.presentWebPageDialog(
            tabID: id,
            title: L("Anmeldung für \(space.host)", "Sign in to \(space.host)"),
            message: realm.isEmpty ? L("Diese Seite ist passwortgeschützt.", "This site is password protected.") : realm,
            kind: .credentials
        ) { response in
            guard response.accepted, !response.text.isEmpty else {
                completionHandler(.cancelAuthenticationChallenge, nil); return
            }
            completionHandler(.useCredential, URLCredential(user: response.text, password: response.password, persistence: .forSession))
        }
    }
    /// Certificate handling.
    ///
    /// For any host that could be a public website this defers to WebKit, which
    /// means an invalid certificate ends the navigation and there is no way to
    /// click through. An exception is only offered for loopback, private and
    /// `.local`-style hosts, so a self-signed local dev server is reachable
    /// without opening that door for the rest of the web. The exception is bound
    /// to the exact certificate and is never written to disk.
    private func resolveServerTrust(_ challenge: URLAuthenticationChallenge, in webView: WKWebView, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        let host = challenge.protectionSpace.host.lowercased()
        guard let owner, let trust = challenge.protectionSpace.serverTrust,
              webView.window != nil, !owner.agentTabIDs.contains(id),
              CertificateTrustStore.isLocal(host) else {
            completionHandler(.performDefaultHandling, nil); return
        }
        // Only step in for a certificate that actually fails evaluation; a valid
        // one must go through the normal path.
        var evaluationError: CFError?
        guard !SecTrustEvaluateWithError(trust, &evaluationError) else {
            completionHandler(.performDefaultHandling, nil); return
        }
        guard let summary = CertificateTrustStore.summary(trust) else {
            completionHandler(.performDefaultHandling, nil); return
        }
        if owner.certificateTrust.isAccepted(host: host, fingerprint: summary.fingerprint) {
            completionHandler(.useCredential, URLCredential(trust: trust))
            return
        }
        let details = [
            L("Name: \(summary.subject)", "Name: \(summary.subject)"),
            L("Ausgestellt von: \(summary.issuer)", "Issued by: \(summary.issuer)"),
            L("Gültig: \(summary.validity)", "Valid: \(summary.validity)"),
            "SHA-256: \(summary.fingerprint)",
            L("Vergleiche den Fingerprint mit deinem Server. Die Ausnahme gilt nur für diesen Host und nur bis YoBro beendet wird.",
              "Compare the fingerprint with your server. The exception applies to this host only and lasts until YoBro quits.")
        ].joined(separator: "\n")
        owner.presentWebPageDialog(
            tabID: id,
            title: L("Zertifikat von \(host) ist ungültig", "Certificate for \(host) is invalid"),
            message: details,
            kind: .confirm,
            destructive: true
        ) { [weak owner] accepted in
            guard accepted else { completionHandler(.cancelAuthenticationChallenge, nil); return }
            owner?.certificateTrust.accept(host: host, fingerprint: summary.fingerprint)
            completionHandler(.useCredential, URLCredential(trust: trust))
        }
    }
    func webView(_ webView: WKWebView, requestMediaCapturePermissionFor origin: WKSecurityOrigin, initiatedByFrame frame: WKFrameInfo, type: WKMediaCaptureType, decisionHandler: @escaping (WKPermissionDecision) -> Void) {
        guard let owner, webView.window != nil, !owner.agentTabIDs.contains(id) else { decisionHandler(.deny); return }
        let device: String
        switch type {
        case .camera: device = L("die Kamera", "the camera")
        case .microphone: device = L("das Mikrofon", "the microphone")
        case .cameraAndMicrophone: device = L("Kamera und Mikrofon", "the camera and microphone")
        @unknown default: device = L("Kamera und Mikrofon", "the camera and microphone")
        }
        let host = origin.host.isEmpty ? L("Diese Seite", "This page") : origin.host
        owner.presentWebPageDialog(
            tabID: id,
            title: L("\(host) möchte \(device) verwenden.", "\(host) wants to use \(device)."),
            message: L("Die Freigabe gilt nur für diesen Seitenaufruf.", "Access applies to this page visit only."),
            kind: .confirm
        ) { decisionHandler($0 ? .grant : .deny) }
    }
}

enum YOBROError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

@MainActor
final class BrowserModel: ObservableObject {
    weak var sidebarScrollView: NSScrollView?
    @Published var tabs: [BrowserTab] = []
    @Published var activeID: UUID?
    @Published var space = L("Persönlich") { didSet { if oldValue != space { chat.cancel(); if agentSessionActive || !agentTabIDs.isEmpty { pauseAgentWorkspace() } } } }
    @Published var splitID: UUID?
    @Published var agentTabID: UUID?
    @Published var agentTabIDs: Set<UUID> = []
    @Published var agentSessionActive = false
    @Published var agentWorkspaceVisible = false
    @Published var agentAction: String?
    var agentGeneration = UUID()

    var agentUsageActive: Bool {
        agentEnabled && isProfileActive && (agentSessionActive || agentAction != nil || chat.running)
    }

    var agentTab: BrowserTab? { tabs.first { $0.id == agentTabID } }

    /// Detach the agent WebView while another application is in front. Keeping a
    /// live WKWebView in an occluded SwiftUI hierarchy can briefly surface its
    /// newly committed frame when WebKit swaps rendering surfaces.
    func updateAgentWorkspacePresentation(appIsActive: Bool) {
        agentWorkspaceVisible = appIsActive && agentTab != nil
    }

    func presentAgentWorkspaceIfAppropriate() {
        // Model-only tests and pre-window setup have no visible application
        // window. Preserve the normal eager presentation in that case.
        let hasVisibleWindow = NSApp.windows.contains { $0.isVisible && $0.contentView != nil }
        updateAgentWorkspacePresentation(appIsActive: NSApp.isActive || !hasVisibleWindow)
    }

    func pauseAgentWorkspace() {
        agentEnabled = false
        endAgentWorkspace()
    }

    func endAgentWorkspace() {
        agentSessionActive = false
        agentWorkspaceVisible = false
        agentGeneration = UUID()
        let ownedTabs = tabs.filter { agentTabIDs.contains($0.id) }
        // Detach WebKit views from the published UI tree before navigating or
        // destroying them. Doing both inside a Space change can deadlock
        // SwiftUI layout against WebKit's synchronous view callbacks.
        tabs.removeAll { agentTabIDs.contains($0.id) }
        agentTabIDs.removeAll()
        agentTabID = nil
        save()
        Task { @MainActor [weak self] in
            await Task.yield()
            for tab in ownedTabs {
                tab.discard()
                (tab.webView as? AppearanceWebView)?.agentControlled = false
                if #available(macOS 15.4, *), let self { self.extensions.runtime.controller.didCloseTab(tab) }
            }
        }
    }

    @discardableResult
    func newAgentTab(url: String = "", webViewConfiguration: WKWebViewConfiguration? = nil, useConfigurationDirectly: Bool = false) -> BrowserTab {
        let tab = BrowserTab(saved: StoredTab(id: UUID(), title: L("Agentenseite", "Agent page"), url: "", space: space, pinned: false), owner: self, webViewConfiguration: webViewConfiguration, useConfigurationDirectly: useConfigurationDirectly)
        (tab.webView as? AppearanceWebView)?.agentControlled = true
        agentSessionActive = true
        agentTabIDs.insert(tab.id)
        tabs.append(tab)
        agentTabID = tab.id
        presentAgentWorkspaceIfAppropriate()
        if #available(macOS 15.4, *) { extensions.runtime.controller.didOpenTab(tab) }
        if !url.isEmpty { do { try tab.navigate(url) } catch { tab.error = error.localizedDescription } }
        save()
        return tab
    }
    /// Whether the local control socket may hand out the history and download
    /// lists.
    ///
    /// Off by default. The socket is only protected by file permissions and a
    /// matching user id, so every process running as you can talk to it. Most
    /// commands at least need a tab and are visible on screen, but `history` and
    /// `downloads` return stored data in one request without any visible trace,
    /// which makes them the one part worth keeping shut unless asked for.
    @Published private(set) var bridgeLibraryAccess = false

    func setBridgeLibraryAccess(_ value: Bool) {
        guard bridgeLibraryAccess != value else { return }
        bridgeLibraryAccess = value
        do { try JSONEncoder().encode(BridgePolicy(allowsLibraryAccess: value)).write(to: bridgePolicyURL, options: .atomic) }
        catch { notice = error.localizedDescription }
    }

    var bridgePolicyURL: URL { home.appendingPathComponent("bridge-policy.json") }

    @Published var showAgent = false
    @Published var focusAddress = false
    @Published var agentEnabled = false {
        didSet {
            do {
                let file = home.appendingPathComponent("agent-access.json")
                try JSONEncoder().encode(AgentAccessPermission(allowed: agentEnabled)).write(to: file, options: .atomic)
                try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
            } catch { notice = L("Agent-Freigabe konnte nicht gespeichert werden.", "Could not save agent permission.") }
            if !agentEnabled { agentSessionActive = false; chat.cancel(); agentGeneration = UUID(); for tab in tabs where agentTabIDs.contains(tab.id) { tab.webView.stopLoading() } }
        }
    }
    @Published var bridgeStatus = L("Startet …")
    @Published var events: [AgentEvent] = []
    @Published var notice: String?
    @Published var webPageDialog: WebPageDialog?
    @Published var history: [HistoryEntry] = []
    @Published var closedTabs: [StoredTab] = []
    @Published private(set) var closedTabUndoID: UUID?
    @Published var draggingTabID: UUID?
    @Published var tabDropFeedback: TabDropFeedback?
    @Published var draggingFolderID: UUID?
    @Published var folderDropFeedback: TabDropFeedback?
    @Published private(set) var sidebarDragSessionID: UUID?
    @Published var librarySection: LibrarySection?
    @Published var showPalette = false
    @Published var showMail = false
    @Published var showSidebar = true {
        didSet {
            if showSidebar { sidebarOverlayVisible = false }
        }
    }
    @Published var sidebarAutoHide = UserDefaults.standard.bool(forKey: "YOBRO.sidebarAutoHide") {
        didSet {
            UserDefaults.standard.set(sidebarAutoHide, forKey: "YOBRO.sidebarAutoHide")
            if sidebarAutoHide { showSidebar = false }
            else { sidebarOverlayVisible = false }
        }
    }
    @Published var sidebarOverlayVisible = false
    @Published var showSettings = false
    @Published var showOnboarding = false
    @Published var bookmarks: [BookmarkEntry] = []
    @Published var spaces = [L("Persönlich"), "Studio"]
    @Published var spaceIcons: [String: String] = [:]
    @Published var folders: [TabFolder] = []
    /// A single shared owner prevents two delayed folder hovers from presenting
    /// overlapping popovers whose contents can be reused by SwiftUI.
    @Published var hoveredFolderID: UUID?
    /// AppKit dismisses WebKit action popovers when SwiftUI presents another
    /// popover, even when the WebKit popover is application-defined.
    @Published var extensionActionPopupPresented = false
    @Published var splitPairs: [StoredSplit] = []
    @Published var showProfileEditor = false
    @Published var creatingProfile = false
    @Published var settingsSection = L("Erweiterungen")
    @Published var profileName = L("Privat", "Personal")
    var isProfileActive = true { didSet { if !isProfileActive { chat.cancel(); pauseAgentWorkspace() } } }
    weak var profileSession: BrowserProfiles?
    let controlSocketURL: URL
    let home: URL
    static var defaultHome: URL {
        if let configured = ProcessInfo.processInfo.environment["YOBRO_HOME"] {
            return URL(fileURLWithPath: configured)
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let current = support.appendingPathComponent("YOBRO")
        let legacy = support.appendingPathComponent("Orbit")
        guard !FileManager.default.fileExists(atPath: current.path),
              FileManager.default.fileExists(atPath: legacy.path) else { return current }
        do {
            try FileManager.default.moveItem(at: legacy, to: current)
            return current
        } catch {
            // Never hide an existing browser profile just because the directory
            // could not be moved during the brand migration.
            return legacy
        }
    }
    let downloads: DownloadStore
    let mail: MailStore
    let chat: SpaceChatStore
    let proxies: SpaceProxyStore
    let managedVPN: ManagedVPNStore
    let webAppearance: WebAppearance
    let pageZoom: PageZoomStore
    let updates = UpdateService()
    let certificateTrust = CertificateTrustStore()
    let extensions: ExtensionStore
    let sync: BrowserSyncStore
    let websiteDataStore: WKWebsiteDataStore
    private var bridge: ControlBridge?
    private var saveTask: Task<Void, Never>?
    /// A fresh app launch restores the sidebar, but not a selected page. Keep
    /// this true through the initial sync so a remote snapshot cannot activate
    /// (and therefore load) a tab before the user chooses one.
    var isColdStartRestore = true
    var webStartupTask: Task<Void, Never>?
    /// Coalesces history writes; see `saveLibrary`.
    var libraryTask: Task<Void, Never>?
    var syncModifiedAt = Date()
    @Published var autoSuspendInactiveTabs: Bool = UserDefaults.standard.object(forKey: "YOBRO.autoSuspendInactiveTabs") as? Bool ?? true {
        didSet {
            UserDefaults.standard.set(autoSuspendInactiveTabs, forKey: "YOBRO.autoSuspendInactiveTabs")
        }
    }
    private var hibernationTask: Task<Void, Never>?
    private var memoryPressureSource: (any DispatchSourceMemoryPressure)?

    init(profile: LocalBrowserProfile? = nil, root: URL? = nil) {
        let root = root ?? Self.defaultHome
        home = profile?.home(in: root) ?? root
        let original = profile?.isOriginal ?? true
        let testing = ProcessInfo.processInfo.environment["YOBRO_HOME"] != nil
        controlSocketURL = root.appendingPathComponent(original ? "control.sock" : "p-\(profile!.id.uuidString).sock")
        downloads = DownloadStore(home: home, isolated: testing || !original)
        mail = MailStore(home: home)
        chat = SpaceChatStore(home: home)
        proxies = SpaceProxyStore(home: home)
        managedVPN = ManagedVPNStore(home: home)
        webAppearance = WebAppearance(home: home)
        pageZoom = PageZoomStore(home: home)
        if testing { websiteDataStore = .nonPersistent() }
        else if let profile, !profile.isOriginal { websiteDataStore = WKWebsiteDataStore(forIdentifier: profile.id) }
        else { websiteDataStore = .default() }
        extensions = ExtensionStore(home: home, websiteDataStore: websiteDataStore, profileIdentifier: original ? nil : profile?.id)
        sync = BrowserSyncStore(home: home, profileID: profile?.id ?? LocalBrowserProfile.originalID)
        showSidebar = !sidebarAutoHide
        profileName = profile?.name ?? L("Privat", "Personal")
        // New installs never grant local programs access to browser sessions implicitly.
        agentEnabled = AgentAccessPermission.load(home: home).allowed
        var startupProblems: [String] = []
        do { try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700]) }
        catch { startupProblems.append(error.localizedDescription) }
        let historyState = PersistedState.load([HistoryEntry].self, at: home.appendingPathComponent("history.json"))
        startupProblems.append(contentsOf: historyState.problem.map { [$0] } ?? [])
        if let saved = historyState.value {
            history = Self.sanitizedHistory(saved)
            if history != saved { try? JSONEncoder().encode(history).write(to: home.appendingPathComponent("history.json"), options: [.atomic, .completeFileProtection]) }
        }
        let sessionURL = home.appendingPathComponent("session.json")
        if let attributes = try? FileManager.default.attributesOfItem(atPath: sessionURL.path), let date = attributes[.modificationDate] as? Date { syncModifiedAt = date }
        let sessionState = PersistedState.load(StoredSession.self, at: sessionURL)
        startupProblems.append(contentsOf: sessionState.problem.map { [$0] } ?? [])
        if let saved = sessionState.value {
            if let names = saved.spaces, !names.isEmpty { spaces = Array(NSOrderedSet(array: names).array as? [String] ?? names) }
            spaceIcons = (saved.spaceIcons ?? [:]).filter { spaces.contains($0.key) && SpaceIcon(rawValue: $0.value) != nil }
            folders = saved.folders ?? []
            splitPairs = saved.splitPairs ?? []
            space = spaces.contains(saved.space) ? saved.space : spaces[0]
            closedTabs = saved.closedTabs ?? []
            tabs = saved.tabs.filter { spaces.contains($0.space) }.map { BrowserTab(saved: $0, owner: self, deferLoading: true) }
            // A full relaunch always opens on the branded start page. Restored
            // tabs stay visible in the sidebar and remain deferred until the
            // user deliberately selects one.
            activeID = nil
        }
        webAppearance.changed = { [weak self] in
            guard let self else { return }
            for tab in self.tabs { self.webAppearance.update(tab.webView) }
        }
        for tab in tabs { tab.restoreFaviconIfNeeded() }
        proxies.apply(to: websiteDataStore, for: space)
        let bookmarkState = PersistedState.load([BookmarkEntry].self, at: home.appendingPathComponent("bookmarks.json"))
        startupProblems.append(contentsOf: bookmarkState.problem.map { [$0] } ?? [])
        if let saved = bookmarkState.value { bookmarks = saved }
        if let policy = PersistedState.load(BridgePolicy.self, at: home.appendingPathComponent("bridge-policy.json")).value {
            bridgeLibraryAccess = policy.allowsLibraryAccess
        }
        if !startupProblems.isEmpty { notice = startupProblems.joined(separator: "\n") }
        webStartupTask = Task {
            // WKWebView locks in its data-store proxy when the first navigation
            // starts, so restore the managed VPN before loading deferred tabs.
            await managedVPN.restore(to: websiteDataStore)
            webStartupTask = nil
        }
        // Extension startup can involve loading a background worker and must
        // never keep ordinary browsing, restored tabs, or the address search
        // waiting behind it.
        Task { await extensions.start(owner: self) }
        let startup = webStartupTask
        Task {
            await startup?.value
            await sync.restoreAndSync(self)
            isColdStartRestore = false
            if !testing {
                startTabHibernationMonitoring()
            }
        }
    }

    deinit {
        hibernationTask?.cancel()
        memoryPressureSource?.cancel()
    }

    /// Automatically unloads tabs that have been inactive for longer than `timeout`,
    /// provided they are not active, pinned, loading, playing media, or notes.
    @discardableResult
    func suspendInactiveTabs(olderThan timeout: TimeInterval = 30 * 60) async -> [UUID] {
        guard autoSuspendInactiveTabs else { return [] }
        let now = Date()
        var suspendedIDs: [UUID] = []
        for tab in tabs {
            guard tab.id != activeID, tab.id != splitID else { continue }
            guard !agentTabIDs.contains(tab.id) else { continue }
            guard !tab.isNote, !tab.url.isEmpty, !tab.isSuspended, !tab.loading else { continue }
            guard now.timeIntervalSince(tab.lastActiveAt) >= timeout else { continue }
            let playing = await tab.isPlayingMedia()
            guard !playing else { continue }
            tab.suspend(forced: true)
            suspendedIDs.append(tab.id)
        }
        return suspendedIDs
    }

    func startTabHibernationMonitoring() {
        guard hibernationTask == nil else { return }
        hibernationTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 300 * 1_000_000_000)
                guard let self else { break }
                await self.suspendInactiveTabs()
            }
        }
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
        source.setEventHandler { [weak self] in
            guard let self else { return }
            Task { @MainActor in
                await self.suspendInactiveTabs(olderThan: 5 * 60)
            }
        }
        source.resume()
        memoryPressureSource = source
    }

    func stopTabHibernationMonitoring() {
        hibernationTask?.cancel()
        hibernationTask = nil
        memoryPressureSource?.cancel()
        memoryPressureSource = nil
    }


    var active: BrowserTab? { tabs.first { $0.id == activeID } }
    var visibleTabs: [BrowserTab] { tabs.filter { $0.space == space && !agentTabIDs.contains($0.id) } }

    /// Non-nil when the traffic should be tunnelled through the managed VPN or a
    /// space proxy but is not. Navigation is refused while this is set.
    var isolationFailure: String? { managedVPN.isolationFailure ?? proxies.isolationFailure }

    func autoScrollSidebarDuringDrag() {
        guard let scrollView = sidebarScrollView, let window = scrollView.window else { return }
        guard let document = scrollView.documentView else { return }
        let pointer = document.convert(window.mouseLocationOutsideOfEventStream, from: nil)
        let visible = scrollView.documentVisibleRect
        let edge: CGFloat = 54
        let delta: CGFloat
        if pointer.y < visible.minY + edge { delta = -max(4, 15 * (1 - max(0, pointer.y - visible.minY) / edge)) }
        else if pointer.y > visible.maxY - edge { delta = max(4, 15 * (1 - max(0, visible.maxY - pointer.y) / edge)) }
        else { return }
        let maximum = max(0, document.bounds.height - visible.height)
        let target = min(maximum, max(0, visible.minY + delta))
        scrollView.contentView.scroll(to: NSPoint(x: visible.minX, y: target))
        scrollView.reflectScrolledClipView(scrollView.contentView)
    }

    func beginSidebarTabDrag(_ id: UUID) {
        sidebarDragSessionID = UUID()
        draggingFolderID = nil
        folderDropFeedback = nil
        draggingTabID = id
        tabDropFeedback = nil
    }

    func beginSidebarFolderDrag(_ id: UUID) {
        sidebarDragSessionID = UUID()
        hoveredFolderID = nil
        draggingTabID = nil
        tabDropFeedback = nil
        draggingFolderID = id
        folderDropFeedback = nil
    }

    func finishSidebarDrag(sessionID: UUID? = nil) {
        if let sessionID, sessionID != sidebarDragSessionID { return }
        draggingTabID = nil
        draggingFolderID = nil
        tabDropFeedback = nil
        folderDropFeedback = nil
        sidebarDragSessionID = nil
    }


    func requestNewTab(space targetSpace: String? = nil) {
        _ = newTab(space: targetSpace ?? space)
    }

    func requestPrivateTab(space targetSpace: String? = nil) {
        _ = newTab(space: targetSpace ?? space, isPrivate: true)
    }

    @discardableResult
    func newNote(title: String = "", space targetSpace: String? = nil, folderID: UUID? = nil) -> BrowserTab {
        showMail = false
        let targetSpace = targetSpace ?? space
        let validFolder = folderID.flatMap { id in folders.contains { $0.id == id && $0.space == targetSpace } ? id : nil }
        let note = BrowserTab(saved: StoredTab(
            id: UUID(), title: title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? L("Neue Notiz", "New note") : title,
            url: "", space: targetSpace, pinned: false, folderID: validFolder, kind: .note, noteContent: "", noteRTF: nil
        ), owner: self)
        tabs.append(note)
        space = targetSpace; activeID = note.id; splitID = nil
        save()
        return note
    }

    func presentMail(_ presented: Bool = true) {
        showMail = presented
    }

    @discardableResult
    func newTab(url: String = "", space targetSpace: String? = nil, isPrivate: Bool = false, webViewConfiguration: WKWebViewConfiguration? = nil, useConfigurationDirectly: Bool = false) -> BrowserTab {
        showMail = false
        if let targetSpace, targetSpace != space {
            switchSpace(targetSpace)
        }
        let tab = BrowserTab(saved: StoredTab(id: UUID(), title: isPrivate ? L("Privater Tab", "Private tab") : L("Neue Seite"), url: "", space: targetSpace ?? space, pinned: false, isPrivate: isPrivate), owner: self, webViewConfiguration: webViewConfiguration, useConfigurationDirectly: useConfigurationDirectly)
        tabs.append(tab)
        if #available(macOS 15.4, *) { extensions.runtime.controller.didOpenTab(tab) }
        select(tab.id)
        if !url.isEmpty { do { try tab.navigate(url) } catch { notice = error.localizedDescription } }
        save(); return tab
    }
    func select(_ id: UUID) {
        showMail = false
        guard let tab = tabs.first(where: { $0.id == id }), !agentTabIDs.contains(id) else { return }
        tab.lastActiveAt = Date()
        if space != tab.space { splitID = nil }
        splitID = splitPairs.first(where: { $0.contains(id) })?.other(id)
        for pane in tabs where pane.id == id || pane.id == splitID {
            if !pane.isNote {
                pane.resume()
                if pane.webView.url == nil { pane.loadPersistedContent() }
            }
        }
        let previous = active
        activeID = id; space = tab.space; save()
        if #available(macOS 15.4, *) { extensions.runtime.controller.didActivateTab(tab, previousActiveTab: previous) }
    }
    func switchSpace(_ value: String) {
        showMail = false
        space = value; splitID = nil
        if managedVPN.isActive { managedVPN.reapply(to: websiteDataStore) }
        else { proxies.apply(to: websiteDataStore, for: space) }
        if let tab = visibleTabs.first { select(tab.id) }
        else { activeID = nil; requestNewTab(space: value) }
    }
    func updateActiveProxy() {
        if managedVPN.isActive { managedVPN.reapply(to: websiteDataStore) }
        else { proxies.apply(to: websiteDataStore, for: space) }
    }

    func presentWebPageDialog(tabID: UUID, title: String, message: String, kind: WebPageDialog.Kind, defaultText: String = "", destructive: Bool = false, completion: @escaping (WebPageDialog.Response) -> Void) {
        if let pending = webPageDialog { pending.completion(.dismissed) }
        webPageDialog = WebPageDialog(tabID: tabID, title: title, message: message, kind: kind, defaultText: defaultText, destructive: destructive, completion: completion)
    }
    /// Convenience for the alert and confirm cases, which only need the answer.
    func presentWebPageDialog(tabID: UUID, title: String, message: String, kind: WebPageDialog.Kind, destructive: Bool = false, completion: @escaping (Bool) -> Void) {
        presentWebPageDialog(tabID: tabID, title: title, message: message, kind: kind, destructive: destructive) { completion($0.accepted) }
    }
    func resolveWebPageDialog(_ accepted: Bool, text: String = "", password: String = "") {
        guard let dialog = webPageDialog else { return }
        webPageDialog = nil
        dialog.completion(WebPageDialog.Response(accepted: accepted, text: text, password: password))
    }
    func pauseAllMediaPlayback() {
        for tab in tabs { tab.pauseMediaPlayback() }
    }
    func closeTab(_ id: UUID?, revealStartPage: Bool = false) {
        guard let id else { return }
        if webPageDialog?.tabID == id { resolveWebPageDialog(false) }
        if var saved = tabs.first(where: { $0.id == id && !$0.isPrivate })?.stored {
            saved.closedIndex = tabs.firstIndex(where: { $0.id == id })
            closedTabs.append(saved)
            if closedTabs.count > 20 { closedTabs.removeFirst() }
            closedTabUndoID = UUID()
        }
        if #available(macOS 15.4, *), let tab = tabs.first(where: { $0.id == id }) { extensions.runtime.controller.didCloseTab(tab) }
        tabs.first(where: { $0.id == id })?.discard()
        tabs.removeAll { $0.id == id }
        splitPairs.removeAll { $0.contains(id) }
        if agentTabIDs.remove(id) != nil {
            if agentTabID == id { agentTabID = tabs.last(where: { agentTabIDs.contains($0.id) })?.id }
            if agentTabIDs.isEmpty { agentWorkspaceVisible = false; agentGeneration = UUID() }
        }
        if splitID == id { splitID = nil }
        if activeID == id {
            activeID = nil
            if !revealStartPage {
                if let replacement = visibleTabs.last { select(replacement.id) } else { requestNewTab() }
            }
        }
        save()
    }

    /// Folder tabs have an Arc-like two-step close lifecycle: first unload the
    /// page but retain its entry, then allow explicit deletion with the x.
    func closeSidebarTab(_ id: UUID?) {
        guard let id, let tab = tabs.first(where: { $0.id == id }) else { return }
        if tab.isNote { closeTab(id, revealStartPage: activeID == id); return }
        guard tab.folderID != nil, activeID == id, !tab.isSuspended else {
            closeTab(id, revealStartPage: activeID == id)
            return
        }
        if webPageDialog?.tabID == id { resolveWebPageDialog(false) }
        tab.suspend()
        splitPairs.removeAll { $0.contains(id) }
        if splitID == id { splitID = nil }
        if activeID == id {
            activeID = nil
        }
        save()
    }
    func dismissClosedTabUndo(_ id: UUID) {
        if closedTabUndoID == id { closedTabUndoID = nil }
    }
    func clearClosedTabUndo() { closedTabUndoID = nil }
    func toggleSplit() {
        if splitID != nil { separateSplit(); return }
        guard let current = activeID else { return }
        if let other = visibleTabs.last(where: { $0.id != current }) { pairTabs(other.id, with: current) }
        else { let added = newTab(); pairTabs(added.id, with: current) }
    }
    func navigate(_ text: String) {
        do { try active?.navigate(text) } catch { notice = error.localizedDescription }
    }

    /// ⌘1–⌘8 pick a tab by position, ⌘9 jumps to the last one, matching the
    /// convention in Safari and Chrome.
    func selectVisibleTab(at index: Int) {
        let list = visibleTabs
        guard !list.isEmpty else { return }
        let target = index >= 8 ? list.count - 1 : index
        guard list.indices.contains(target) else { return }
        select(list[target].id)
    }

    func changeZoom(_ direction: Int) { active?.changeZoom(direction) }
    func printActivePage() { active?.printPage() }

    /// Saves the current page. Reachable via ⌘D and the page actions; before,
    /// bookmarks could only be created from a button buried in settings.
    func bookmarkActivePage() {
        guard let tab = active, let url = tab.webView.url, ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return }
        let title = tab.title.trimmingCharacters(in: .whitespacesAndNewlines)
        let link = ImportLink(title: title.isEmpty ? (url.host ?? url.absoluteString) : title,
                              url: url.absoluteString,
                              folder: L("Meine Lesezeichen"))
        do {
            let added = try mergeImport(bookmarks: [link], history: []).0
            notice = added > 0
                ? L("Zu deinen Lesezeichen hinzugefügt.", "Added to your bookmarks.")
                : L("Diese Seite ist bereits in deinen Lesezeichen.", "This page is already in your bookmarks.")
        } catch { notice = error.localizedDescription }
    }

    var activePageIsBookmarked: Bool {
        guard let url = active?.webView.url?.absoluteString else { return false }
        return bookmarks.contains { $0.url == url }
    }

    /// Removes cookies, caches and local storage from this profile's data store.
    /// Saved passwords live in the keychain and are deliberately untouched.
    func clearWebsiteData(_ range: WebsiteDataRange, includingHistory: Bool) async {
        await websiteDataStore.removeData(ofTypes: WKWebsiteDataStore.allWebsiteDataTypes(), modifiedSince: range.start)
        if includingHistory {
            if range == .everything { clearHistory() }
            else {
                history.removeAll { $0.date >= range.start }
                flushLibrary()
            }
        }
        // Reload so open pages notice that their session is gone instead of
        // continuing to look signed in.
        for tab in tabs where !tab.url.isEmpty && !agentTabIDs.contains(tab.id) { tab.webView.reload() }
    }

    func removeBookmark(_ entry: BookmarkEntry) {
        let updated = bookmarks.filter { $0.id != entry.id }
        do {
            try JSONEncoder().encode(updated).write(to: home.appendingPathComponent("bookmarks.json"), options: .atomic)
            bookmarks = updated
            markSyncChanged()
        } catch { notice = error.localizedDescription }
    }
    func record(_ action: String, _ detail: String, in eventSpace: String? = nil) {
        events.insert(AgentEvent(action: action, detail: detail), at: 0)
        chat.append("action", action + ": " + detail, space: eventSpace ?? space)
        if events.count > 50 { events.removeLast() }
    }
    func save() {
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 200_000_000)
            guard !Task.isCancelled, let self else { return }
            persistSession()
        }
    }
    @discardableResult
    func persistSession(scheduleSync: Bool = true, updateTimestamp: Bool = true) -> Bool {
        saveTask?.cancel()
        do {
            let userTabs = tabs.filter { !agentTabIDs.contains($0.id) && !$0.isPrivate }
            let data = try JSONEncoder().encode(StoredSession(tabs: userTabs.map(\.stored), activeID: activeID, space: space, closedTabs: closedTabs, spaces: spaces, spaceIcons: spaceIcons, folders: folders, splitPairs: splitPairs))
            try data.write(to: home.appendingPathComponent("session.json"), options: .atomic)
            if updateTimestamp { syncModifiedAt = Date() }
            if scheduleSync { sync.schedule(self) }
            return true
        } catch { notice = "Sitzung konnte nicht gespeichert werden: \(error.localizedDescription)"; return false }
    }
    func markSyncChanged() {
        syncModifiedAt = Date()
        sync.schedule(self)
    }
    func startBridge() {
        guard bridge == nil else { return }
        do {
            let server = try ControlBridge(path: controlSocketURL.path) { [weak self] request in
                guard let self else { throw YOBROError.message("Browser geschlossen") }
                return try await self.handle(request)
            }
            bridge = server; bridgeStatus = L("Bereit")
        } catch { bridgeStatus = "Nicht verbunden"; notice = error.localizedDescription }
    }
}
