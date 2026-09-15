import SwiftUI
import WebKit

final class AppearanceWebView: WKWebView {
    var agentControlled = false
    override var acceptsFirstResponder: Bool { !agentControlled && super.acceptsFirstResponder }
    override func becomeFirstResponder() -> Bool { !agentControlled && super.becomeFirstResponder() }

    var appearanceChanged: (() -> Void)?
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        appearanceChanged?()
    }

    /// WebKit's context menu offers "Open Link in New Window". YOBRO turns every
    /// requested window into a tab in `createWebViewWith`, so the action was
    /// already right and only the wording was misleading.
    override func willOpenMenu(_ menu: NSMenu, with event: NSEvent) {
        super.willOpenMenu(menu, with: event)
        for item in menu.items {
            guard let identifier = item.identifier?.rawValue else { continue }
            switch identifier {
            case "WKMenuItemIdentifierOpenLinkInNewWindow":
                item.title = L("Link in neuem Tab öffnen", "Open Link in New Tab")
            case "WKMenuItemIdentifierOpenImageInNewWindow":
                item.title = L("Bild in neuem Tab öffnen", "Open Image in New Tab")
            case "WKMenuItemIdentifierOpenMediaInNewWindow":
                item.title = L("Video in neuem Tab öffnen", "Open Video in New Tab")
            case "WKMenuItemIdentifierOpenFrameInNewWindow":
                item.title = L("Frame in neuem Tab öffnen", "Open Frame in New Tab")
            default:
                break
            }
        }
    }
}

struct WebAppearancePreferences: Codable {
    var enabled = false
    var excludedHosts: Set<String> = []
}

@MainActor
final class WebAppearance: ObservableObject {
    static let world = WKContentWorld.world(name: "YOBRO.WebAppearance")
    @Published private(set) var preferences: WebAppearancePreferences
    private let file: URL
    var changed: (() -> Void)?
    init(home: URL) {
        file = home.appendingPathComponent("web-appearance.json")
        preferences = (try? JSONDecoder().decode(WebAppearancePreferences.self, from: Data(contentsOf: file))) ?? WebAppearancePreferences()
    }
    func setEnabled(_ enabled: Bool) { preferences.enabled = enabled; save() }
    func setExcluded(_ host: String, _ excluded: Bool) {
        if excluded { preferences.excludedHosts.insert(host.lowercased()) }
        else { preferences.excludedHosts.remove(host.lowercased()) }
        save()
    }
    private func save() {
        do { try JSONEncoder().encode(preferences).write(to: file, options: .atomic) }
        catch { NSLog("YoBro: Webseitendarstellung konnte nicht gespeichert werden: %@", error.localizedDescription) }
        changed?()
    }
    /// `String(decoding:as:)` cannot fail, unlike the force-unwrapped
    /// `String(data:encoding:)` this replaces.
    private var json: String {
        String(decoding: (try? JSONEncoder().encode(preferences)) ?? Data("{}".utf8), as: UTF8.self)
    }
    private static let source: String = {
        // A missing script resource must degrade to "no dark mode", not crash
        // the first tab the browser opens.
        ["DarkReader", "WebAppearance"].compactMap { name -> String? in
            guard let url = Bundle.main.url(forResource: name, withExtension: "js")
                ?? Bundle.module.url(forResource: name, withExtension: "js") else { return nil }
            return try? String(contentsOf: url)
        }.joined(separator: "\n")
    }()
    func install(on controller: WKUserContentController) {
        guard !Self.source.isEmpty else { return }
        controller.addUserScript(WKUserScript(source: Self.source + "\nwindow.__yobroAppearance?.configure(\(json));", injectionTime: .atDocumentEnd, forMainFrameOnly: false, in: Self.world))
    }
    func update(_ view: WKWebView) {
        let dark = view.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        view.evaluateJavaScript("window.__yobroAppearance?.configure({...\(json), systemDark: \(dark)});", in: nil, in: Self.world) { _ in }
    }
}

struct WebAppearanceButton: View {
    @ObservedObject var appearance: WebAppearance
    let host: String?
    @Environment(\.colorScheme) private var scheme
    @State private var presented = false
    var body: some View {
        Button { presented.toggle() } label: {
            Image(systemName: appearance.preferences.enabled && scheme == .dark ? "moon.fill" : "moon")
                .foregroundStyle(appearance.preferences.enabled ? moss : ink.opacity(0.5))
        }.buttonStyle(YOBROButtonStyle()).help(L("Webseiten-Darkmode")).popover(isPresented: $presented, arrowEdge: .bottom) {
            VStack(alignment: .leading, spacing: 17) {
                Text(L("Licht nach deinem Rhythmus.")).font(.system(size: 21, design: .serif))
                Label(scheme == .dark ? L("System ist im Dunkelmodus") : L("System ist im Hellmodus"), systemImage: scheme == .dark ? "moon" : "sun.max")
                    .font(.system(size: 11)).foregroundStyle(ink.opacity(0.55))
                Text(L("Websites erhalten automatisch die Systemdarstellung. Eine dort gespeicherte Auswahl kann Vorrang haben.")).font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                if let host, host == "youtube.com" || host.hasSuffix(".youtube.com") {
                    Text(L("YouTube: Profilmenü → Darstellung → Gerätedesign verwenden. Alternativ dort „Dunkles Design“ wählen.")).font(.system(size: 11)).fixedSize(horizontal: false, vertical: true)
                }
                Toggle(L("Helle Websites abdunkeln"), isOn: Binding(get: { appearance.preferences.enabled }, set: { appearance.setEnabled($0) })).toggleStyle(.switch)
                Text(L("Aktiv, sobald dein System dunkel ist. Bereits dunkle Seiten behalten ihre eigenen Farben."))
                    .font(.system(size: 11)).foregroundStyle(ink.opacity(0.6)).fixedSize(horizontal: false, vertical: true)
                if let host {
                    Divider()
                    Toggle(L("Auf dieser Website verwenden"), isOn: Binding(get: { !appearance.preferences.excludedHosts.contains(host.lowercased()) }, set: { appearance.setExcluded(host, !$0) })).toggleStyle(.switch).disabled(!appearance.preferences.enabled)
                    Text(host).font(.system(size: 10)).foregroundStyle(moss).lineLimit(1)
                }
                Text(L("YoBro selbst folgt immer dem Systemmodus.")).font(.system(size: 10)).foregroundStyle(ink.opacity(0.45))
            }.font(.system(size: 12)).padding(22).frame(width: 315).background(paper).foregroundStyle(ink)
        }
    }
}
