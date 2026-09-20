import SwiftUI

enum ExtensionCatalog {
    static let internalURL = "yobro://extensions"
    static let storeURL = "https://chromewebstore.google.com/category/extensions"
    static let safariInformationURL = "https://developer.apple.com/safari/extensions/"

    static func isInternal(_ value: String) -> Bool {
        value == internalURL || value == internalURL + "/"
    }

    static func searchURL(_ text: String) -> URL? {
        let query = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else { return nil }
        var components = URLComponents(string: "https://chromewebstore.google.com")!
        components.percentEncodedPath = "/search/" + (query.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "")
        return components.url
    }
}

extension BrowserModel {
    func openExtensionsHub() {
        showSettings = false
        if let existing = tabs.first(where: { $0.space == space && $0.isExtensionsHub && !agentTabIDs.contains($0.id) }) {
            select(existing.id)
        } else { newTab(url: ExtensionCatalog.internalURL) }
    }
}

struct ExtensionHubPage: View {
    @ObservedObject var model: BrowserModel
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                HStack(spacing: 10) {
                    YOBROMark(size: 26)
                    Text("YOBRO EXTRAS").font(.system(size: 11, weight: .bold, design: .rounded)).tracking(2)
                    Spacer()
                    Label(L("Direkt im Browser", "Inside your browser"), systemImage: "globe")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ExtensionSettings(store: model.extensions, model: model)
            }.padding(32).frame(maxWidth: 940)
                .frame(maxWidth: .infinity, alignment: .top)
        }.background(paper)
            .accessibilityIdentifier("extensions-hub")
    }
}

struct IncludedBrowserFeatures: View {
    @ObservedObject var store: ExtensionStore
    @ObservedObject var appearance: WebAppearance
    private var blocker: InstalledExtension? { store.entries.first { $0.id == ExtensionStore.bundledBlockerID } }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(L("Schon an Bord", "Already included")).font(.system(size: 15, weight: .semibold))
            HStack(alignment: .top, spacing: 12) {
                VStack(alignment: .leading, spacing: 10) {
                    Label(L("Weniger Werbung", "Less advertising"), systemImage: "shield.lefthalf.filled").font(.headline)
                    Text("uBlock Origin Lite").font(.caption).foregroundStyle(.secondary)
                    if let blocker {
                        Toggle(L("Aktiv", "Enabled"), isOn: Binding(get: { blocker.enabled }, set: { _ in Task { await store.toggle(blocker) } }))
                            .toggleStyle(.switch).disabled(store.busy || store.pending != nil)
                        if let error = store.errors[blocker.id] { Text(error).font(.caption).foregroundStyle(.red) }
                    } else if #available(macOS 15.6, *) {
                        Button(L("Enthaltenen Blocker aktivieren", "Enable included blocker")) {
                            Task { await store.restoreBundledBlocker() }
                        }.disabled(store.busy || store.pending != nil)
                    } else {
                        Text(L("Benötigt macOS 15.6+", "Requires macOS 15.6+")).font(.caption)
                    }
                }.frame(maxWidth: .infinity, alignment: .leading).yobroCard(padding: 16)
                VStack(alignment: .leading, spacing: 10) {
                    Label(L("Angenehm im Dunkeln", "Easy on the eyes"), systemImage: "moon.stars.fill").font(.headline)
                    Text(L("Webseiten-Darkmode · integriert", "Website dark mode · built in")).font(.caption).foregroundStyle(.secondary)
                    Toggle(L("Helle Seiten abdunkeln", "Darken bright pages"), isOn: Binding(get: { appearance.preferences.enabled }, set: { appearance.setEnabled($0) }))
                        .toggleStyle(.switch)
                    Text(L("Folgt dem dunklen Systemdesign. Keine zusätzliche Erweiterung nötig.", "Follows your system's dark appearance. No extra extension needed."))
                        .font(.caption).foregroundStyle(.secondary)
                }.frame(maxWidth: .infinity, alignment: .leading).yobroCard(padding: 16)
            }
        }
    }
}
