import Foundation
import WebKit

private struct AdBlockPreferences: Codable {
    var enabled: Bool
    var strictProtection: Bool?
}

@MainActor
final class AdBlocker: NSObject, ObservableObject, WKScriptMessageHandler {
    static let ruleListIdentifier = "local.yobro.browser.adblock.v3"
    static let world = WKContentWorld.world(name: "YOBRO.AdBlocker")

    static let blockedDomains = [
        "2mdn.net", "33across.com", "360yield.com", "4dsply.com", "ad.gt", "adform.net",
        "adition.com", "adnxs.com", "adroll.com", "adsafeprotected.com", "adsrvr.org",
        "ads-twitter.com", "adscale.de", "adservice.google.com", "amazon-adsystem.com",
        "amplitude.com", "app-measurement.com", "appsflyer.com", "atdmt.com", "bingads.microsoft.com",
        "bluekai.com", "branch.io", "casalemedia.com", "chartbeat.com", "clarity.ms",
        "contextweb.com", "criteo.com", "criteo.net", "demdex.net", "districtm.io",
        "doubleclick.net", "doubleverify.com", "everesttech.net", "exelator.com", "facebook.net",
        "flashtalking.com", "gemius.pl", "google-analytics.com", "googleadservices.com",
        "googlesyndication.com", "googletagmanager.com", "gumgum.com", "heapanalytics.com",
        "hotjar.com", "imrworldwide.com", "indexww.com", "innovid.com", "inmobi.com",
        "krxd.net", "lijit.com", "linkedin.com", "lkqd.net", "mathtag.com", "media.net",
        "mediavine.com", "mixpanel.com", "moatads.com", "mookie1.com", "newrelic.com",
        "nexac.com", "omtrdc.net", "onaudience.com", "openx.net", "optimizely.com",
        "outbrain.com", "pardot.com", "perfectaudience.com", "permutive.com", "postrelease.com",
        "pubmatic.com", "quantcast.com", "quantserve.com", "raygun.io", "revcontent.com",
        "rfihub.com", "rlcdn.com", "rubiconproject.com", "samba.tv", "scorecardresearch.com",
        "segment.com", "segment.io", "serving-sys.com", "sharethrough.com", "smartadserver.com",
        "snapads.com", "spotxchange.com", "stackadapt.com", "taboola.com", "tapad.com",
        "teads.tv", "thetradedesk.com", "triplelift.com", "turn.com", "undertone.com",
        "unrulymedia.com", "usebutton.com", "viglink.com", "weborama.com", "yieldmo.com",
        "zedo.com", "zemanta.com"
    ]

    static let thirdPartyPathPatterns = [
        "/ad[/?]", "/ads[/?]", "/adserver[/?]", "/advert[/?]", "/advertising[/?]",
        "/analytics[/?]", "/collect[/?]", "/pixel[/?]", "/tracking[/?]", "/telemetry[/?]",
        "/beacon[/?]", "/prebid[./?]", "/vast[/?]", "/pagead[/?]"
    ]

    private static let trackingParameters: Set<String> = [
        "fbclid", "gclid", "dclid", "gbraid", "wbraid", "msclkid", "twclid", "ttclid",
        "igshid", "mc_cid", "mc_eid", "mkt_tok", "vero_conv", "vero_id", "oly_anon_id",
        "oly_enc_id", "rb_clickid", "s_cid", "wickedid", "yclid", "_hsenc", "_hsmi"
    ]

    static func removingTrackingParameters(from url: URL) -> URL {
        guard var components = URLComponents(url: url, resolvingAgainstBaseURL: false),
              let items = components.queryItems, !items.isEmpty else { return url }
        let filtered = items.filter { item in
            let name = item.name.lowercased()
            return !name.hasPrefix("utm_") && !trackingParameters.contains(name)
        }
        guard filtered.count != items.count else { return url }
        components.queryItems = filtered.isEmpty ? nil : filtered
        return components.url ?? url
    }

    static var ruleJSON: String {
        var rules: [[String: Any]] = []
        
        // Block third-party ad/tracking domains
        for domain in blockedDomains {
            rules.append([
                "trigger": [
                    "url-filter": "^https?://([^/]+\\.)?\(NSRegularExpression.escapedPattern(for: domain))/",
                    "load-type": ["third-party"]
                ],
                "action": ["type": "block"]
            ])
        }
        
        // IMPORTANT: Do NOT block YouTube's own ad detection endpoints
        // Blocking these triggers YouTube's ad blocker detection
        // We handle ads via JavaScript injection instead
        
        // Block third-party ad paths
        for pattern in thirdPartyPathPatterns {
            rules.append([
                "trigger": [
                    "url-filter": "^https?://.+\(pattern)",
                    "load-type": ["third-party"],
                    "resource-type": ["image", "style-sheet", "script", "font", "media", "svg-document", "raw"]
                ],
                "action": ["type": "block"]
            ])
        }
        
        // CSS-based hiding (this doesn't trigger detection)
        rules.append([
            "trigger": ["url-filter": ".*"],
            "action": [
                "type": "css-display-none",
                "selector": "ins.adsbygoogle, iframe[id^='google_ads_'], iframe[src*='doubleclick.net'], [data-ad-client], [data-ad-slot], .ad-container, .ad-wrapper, .advertisement, .advertising, .sponsored-content, .taboola, .OUTBRAIN, [id^='taboola-'], [class*='ad-placement'], [class*='ad_slot'], ytd-ad-slot-renderer, ytd-in-feed-ad-layout-renderer, ytd-banner-promo-renderer, ytd-promoted-sparkles-web-renderer"
            ]
        ])
        
        // An empty string signals failure to `compiledRuleList`. Crashing here
        // would take down the first tab instead of just losing the filter.
        guard let data = try? JSONSerialization.data(withJSONObject: rules) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    @Published private(set) var enabled: Bool
    @Published private(set) var strictProtection: Bool
    @Published private(set) var ready = false
    @Published private(set) var error: String?

    private let preferencesURL: URL
    private var ruleList: WKContentRuleList?
    private var compilationTask: Task<WKContentRuleList, Error>?
    private static let scriptSource: String = {
        guard let url = Bundle.module.url(forResource: "AdBlocker", withExtension: "js") else { return "" }
        return (try? String(contentsOf: url)) ?? ""
    }()
    private static let pageScriptSource: String = {
        guard let url = Bundle.module.url(forResource: "AdBlockerPage", withExtension: "js") else { return "" }
        return (try? String(contentsOf: url)) ?? ""
    }()

    init(home: URL) {
        preferencesURL = home.appendingPathComponent("adblock.json")
        if let data = try? Data(contentsOf: preferencesURL),
           let preferences = try? JSONDecoder().decode(AdBlockPreferences.self, from: data) {
            enabled = preferences.enabled
            strictProtection = preferences.strictProtection ?? true
        } else {
            enabled = true
            strictProtection = true
        }
        super.init()
    }

    func attach(to controller: WKUserContentController) {
        controller.add(self, contentWorld: Self.world, name: "yobroAdBlock")
        let pageSource = Self.pageScriptSource + "\nwindow.__yobroAdBlockPage?.setEnabled(\(aggressiveVideoFilteringEnabled));"
        controller.addUserScript(WKUserScript(source: pageSource, injectionTime: .atDocumentStart, forMainFrameOnly: true, in: .page))
        controller.addUserScript(WKUserScript(source: Self.scriptSource, injectionTime: .atDocumentStart, forMainFrameOnly: true, in: Self.world))
        guard enabled else { return }
        Task { [weak self, weak controller] in
            guard let self, let controller else { return }
            do {
                let list = try await compiledRuleList()
                guard enabled else { return }
                controller.add(list)
                ready = true
                error = nil
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        guard message.frameInfo.isMainFrame,
              (message.body as? String) == "ready",
              let webView = message.webView else { return }
        webView.evaluateJavaScript("window.__yobroAdBlock?.configure(\(enabled), \(aggressiveVideoFilteringEnabled));", in: nil, in: Self.world) { _ in }
        webView.evaluateJavaScript("window.__yobroAdBlockPage?.setEnabled(\(aggressiveVideoFilteringEnabled));", in: nil, in: .page) { _ in }
    }

    func setEnabled(_ value: Bool, webViews: [WKWebView]) {
        guard enabled != value else { return }
        enabled = value
        persist()

        Task {
            do {
                let list = try await compiledRuleList()
                guard enabled == value else { return }
                for webView in webViews {
                    if value { webView.configuration.userContentController.add(list) }
                    else { webView.configuration.userContentController.remove(list) }
                    webView.evaluateJavaScript("window.__yobroAdBlock?.configure(\(enabled), \(aggressiveVideoFilteringEnabled));", in: nil, in: Self.world) { _ in }
                    webView.evaluateJavaScript("window.__yobroAdBlockPage?.setEnabled(\(aggressiveVideoFilteringEnabled));", in: nil, in: .page) { _ in }
                    if webView.url != nil { webView.reload() }
                }
                ready = value
                error = nil
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    func setStrictProtection(_ value: Bool, webViews: [WKWebView]) {
        guard strictProtection != value else { return }
        strictProtection = value
        persist()

        for webView in webViews {
            webView.evaluateJavaScript("window.__yobroAdBlock?.configure(\(enabled), \(aggressiveVideoFilteringEnabled));", in: nil, in: Self.world) { _ in }
            webView.evaluateJavaScript("window.__yobroAdBlockPage?.setEnabled(\(aggressiveVideoFilteringEnabled));", in: nil, in: .page) { _ in }
            if webView.url != nil { webView.reload() }
        }
    }

    func compiledRuleList() async throws -> WKContentRuleList {
        if let ruleList { return ruleList }
        if let compilationTask { return try await compilationTask.value }

        let encoded = Self.ruleJSON
        guard !encoded.isEmpty else { throw AdBlockError.compilationFailed }
        let task = Task<WKContentRuleList, Error> {
            try await withCheckedThrowingContinuation { continuation in
                WKContentRuleListStore.default().compileContentRuleList(
                    forIdentifier: Self.ruleListIdentifier,
                    encodedContentRuleList: encoded
                ) { list, error in
                    if let list { continuation.resume(returning: list) }
                    else { continuation.resume(throwing: error ?? AdBlockError.compilationFailed) }
                }
            }
        }
        compilationTask = task
        do {
            let list = try await task.value
            ruleList = list
            compilationTask = nil
            return list
        } catch {
            compilationTask = nil
            throw error
        }
    }

    private func persist() {
        do {
            let data = try JSONEncoder().encode(AdBlockPreferences(enabled: enabled, strictProtection: strictProtection))
            try data.write(to: preferencesURL, options: .atomic)
        } catch {
            self.error = error.localizedDescription
        }
    }

    private var aggressiveVideoFilteringEnabled: Bool {
        enabled && !strictProtection
    }
}

private enum AdBlockError: LocalizedError {
    case compilationFailed
    var errorDescription: String? { L("Die Adblock-Regeln konnten nicht geladen werden.", "The ad-blocking rules could not be loaded.") }
}
