import WebKit
import SwiftUI

@available(iOS 18.6, *)
@MainActor final class MobileExtensions: NSObject, WKWebExtensionControllerDelegate, WKWebExtensionWindow {
    let controller: WKWebExtensionController
    weak var browser: MobileBrowser?
    private(set) var context: WKWebExtensionContext?
    private var tabsByID: [UUID: MobileExtensionTab] = [:]
    private(set) var readiness: [String: Any] = [:]
    var isReady: Bool { readiness["ready"] as? Bool == true }
    init(userID: UUID, dataStore: WKWebsiteDataStore) {
        let configuration = WKWebExtensionController.Configuration(identifier: userID)
        configuration.defaultWebsiteDataStore = dataStore
        let web = WKWebViewConfiguration(); web.websiteDataStore = dataStore
        configuration.webViewConfiguration = web
        controller = WKWebExtensionController(configuration: configuration)
        super.init(); controller.delegate = self
    }
    func start() async throws {
        guard context == nil else { return }
        guard let url = Bundle.main.url(forResource: "uBlockOriginLite.safari", withExtension: "zip") else {
            throw YOBROError.message("uBlock-Origin-Lite-Paket fehlt.")
        }
        let ext = try await WKWebExtension(resourceBaseURL: url)
        let ctx = WKWebExtensionContext(for: ext)
        ctx.uniqueIdentifier = "FC650123-334F-4B10-8C9B-017C5230B010"
        ctx.baseURL = URL(string: "webkit-extension://fc650123-334f-4b10-8c9b-017c5230b010/")!
        ctx.unsupportedAPIs = ["bookmarks", "history", "downloads", "runtime.connectNative", "runtime.sendNativeMessage", "sessions", "topSites"]
        for permission in ext.requestedPermissions { ctx.setPermissionStatus(.grantedExplicitly, for: permission) }
        for pattern in ext.allRequestedMatchPatterns { ctx.setPermissionStatus(.grantedExplicitly, for: pattern) }
        try controller.load(ctx); context = ctx
        controller.didOpenWindow(self)
        do {
            try await ctx.loadBackgroundContent()
            try await verifyReady(ctx)
        } catch { try? controller.unload(ctx); context = nil; throw error }
    }
    func stop() throws {
        if let context { try controller.unload(context) }
        context = nil; readiness = [:]
    }
    private func verifyReady(_ context: WKWebExtensionContext) async throws {
        guard let configuration = context.webViewConfiguration else { throw YOBROError.message("uBlock-Konfiguration fehlt.") }
        let view = WKWebView(frame: .zero, configuration: configuration)
        defer { view.stopLoading() }
        let url = context.baseURL.appendingPathComponent("web_accessible_resources/noop.html")
        view.load(URLRequest(url: url))
        let deadline = Date().addingTimeInterval(60)
        while Date() < deadline {
            try Task.checkCancellation()
            if !view.isLoading, view.url == url,
               (try? await view.evaluateJavaScript("typeof browser?.runtime?.sendMessage === 'function'")) as? Bool == true {
                let result = try? await view.callAsyncJavaScript("""
                    const { webextFlavor } = await import(browser.runtime.getURL('js/ext.js'));
                    if (webextFlavor !== 'safari') throw new Error('Wrong WebKit extension flavor');
                    return await Promise.race([
                        browser.runtime.sendMessage({what: 'getRegisteredContentScripts'}).then(async scripts => ({
                            ready: Array.isArray(scripts), scripts: scripts?.length ?? 0,
                            rulesets: (await browser.declarativeNetRequest.getEnabledRulesets()).length,
                            dynamicRules: (await browser.declarativeNetRequest.getDynamicRules()).length,
                            sessionRules: (await browser.declarativeNetRequest.getSessionRules()).length
                        })),
                        new Promise((_, reject) => setTimeout(() => reject(new Error('Filter initialization timed out')), 3000))
                    ]);
                    """, arguments: [:], in: nil, contentWorld: .page)
                if let summary = result as? [String: Any], summary["ready"] as? Bool == true,
                   (summary["rulesets"] as? Int ?? 0) + (summary["dynamicRules"] as? Int ?? 0) + (summary["sessionRules"] as? Int ?? 0) > 0 {
                    readiness = summary; return
                }
            }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw YOBROError.message("uBlock konnte nicht gestartet werden.")
    }
    func tab(_ id: UUID) -> MobileExtensionTab {
        if let tab = tabsByID[id] { return tab }
        let tab = MobileExtensionTab(id: id, runtime: self); tabsByID[id] = tab; return tab
    }
    func opened(_ id: UUID) { controller.didOpenTab(tab(id)) }
    func selected(_ id: UUID, previous: UUID?) { controller.didActivateTab(tab(id), previousActiveTab: previous.map(tab)) }
    func closed(_ id: UUID) { if let tab = tabsByID[id] { controller.didCloseTab(tab, windowIsClosing: false) }; tabsByID[id] = nil }
    func changed(_ id: UUID) { controller.didChangeTabProperties([.URL, .title, .loading], for: tab(id)) }
    func tabs(for context: WKWebExtensionContext) -> [any WKWebExtensionTab] { browser?.state.tabs.map { tab($0.id) } ?? [] }
    func activeTab(for context: WKWebExtensionContext) -> (any WKWebExtensionTab)? { browser?.state.selected.map(tab) }
    func webExtensionController(_ controller: WKWebExtensionController, openWindowsFor context: WKWebExtensionContext) -> [any WKWebExtensionWindow] { [self] }
    func webExtensionController(_ controller: WKWebExtensionController, focusedWindowFor context: WKWebExtensionContext) -> (any WKWebExtensionWindow)? { self }
    func optionsView() -> WKWebView? {
        guard let context, let url = context.optionsPageURL, let configuration = context.webViewConfiguration else { return nil }
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.load(URLRequest(url: url)); return view
    }
    func showOptions() {
        guard let view = optionsView() else { return }; browser?.extensionPage = MobileExtensionPage(title: "uBlock Origin Lite", view: view)
    }
    func performAction() {
        guard let context else { return }
        let active = browser?.state.selected.map(tab)
        if let active { context.userGesturePerformed(in: active) }
        context.performAction(for: active)
    }
    func webExtensionController(_ controller: WKWebExtensionController, presentActionPopup action: WKWebExtension.Action, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) {
        guard let view = action.popupWebView else { completionHandler(YOBROError.message("Kein uBlock-Popup verfügbar.")); return }
        browser?.extensionPage = MobileExtensionPage(title: "uBlock Origin Lite", view: view, onClose: { action.closePopup() })
        completionHandler(nil)
    }
    func webExtensionController(_ controller: WKWebExtensionController, openOptionsPageFor context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { showOptions(); completionHandler(nil) }
    func webExtensionController(_ controller: WKWebExtensionController, openNewTabUsing configuration: WKWebExtension.TabConfiguration, for context: WKWebExtensionContext, completionHandler: @escaping ((any WKWebExtensionTab)?, Error?) -> Void) {
        guard let browser, let url = configuration.url else { completionHandler(nil, YOBROError.message("Keine Adresse.")); return }
        if url.scheme == context.baseURL.scheme, url.host == context.baseURL.host, let config = context.webViewConfiguration {
            let id = browser.add(configuration: config, url: url)
            completionHandler(tab(id), nil)
        } else if ["https", "http"].contains(url.scheme ?? "") {
            let id = browser.add(url: url); completionHandler(tab(id), nil)
        } else { completionHandler(nil, YOBROError.message("Nicht unterstützte Adresse.")) }
    }
}

struct MobileExtensionPage: Identifiable {
    let id = UUID()
    let title: String
    let view: WKWebView
    var onClose: () -> Void = {}
}

@available(iOS 18.6, *)
@MainActor final class MobileExtensionTab: NSObject, WKWebExtensionTab {
    let id: UUID
    weak var runtime: MobileExtensions?
    init(id: UUID, runtime: MobileExtensions) { self.id = id; self.runtime = runtime }
    func window(for context: WKWebExtensionContext) -> (any WKWebExtensionWindow)? { runtime }
    func indexInWindow(for context: WKWebExtensionContext) -> Int { runtime?.browser?.state.tabs.firstIndex { $0.id == id } ?? 0 }
    func webView(for context: WKWebExtensionContext) -> WKWebView? { runtime?.browser?.existingView(id) }
    func title(for context: WKWebExtensionContext) -> String? { runtime?.browser?.state.tabs.first { $0.id == id }?.title }
    func url(for context: WKWebExtensionContext) -> URL? { webView(for: context)?.url }
    func isLoadingComplete(for context: WKWebExtensionContext) -> Bool { webView(for: context)?.isLoading == false }
    func isSelected(for context: WKWebExtensionContext) -> Bool { runtime?.browser?.state.selected == id }
    func size(for context: WKWebExtensionContext) -> CGSize { webView(for: context)?.bounds.size ?? .zero }
    func activate(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { runtime?.browser?.select(id); completionHandler(nil) }
    func close(for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { runtime?.browser?.close(id); completionHandler(nil) }
    func reload(fromOrigin: Bool, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) { webView(for: context)?.reload(); completionHandler(nil) }
    func loadURL(_ url: URL, for context: WKWebExtensionContext, completionHandler: @escaping (Error?) -> Void) {
        guard ["https", "http"].contains(url.scheme ?? "") || (url.scheme == context.baseURL.scheme && url.host == context.baseURL.host) else { completionHandler(YOBROError.message("Nicht unterstützte Adresse.")); return }
        webView(for: context)?.load(URLRequest(url: url)); completionHandler(nil)
    }
}
