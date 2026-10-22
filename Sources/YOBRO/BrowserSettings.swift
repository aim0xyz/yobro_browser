import SwiftUI
import WebKit
import AppKit

/// File panels opened from a sheet attach to it, as a sheet of their own,
/// instead of running app-modal from inside the sheet's button action.
@MainActor
enum FilePanel {
    static func present(_ panel: NSSavePanel, completion: @escaping (URL?) -> Void) {
        // The frontmost sheet, such as Settings, whichever window holds the focus.
        var window = NSApp.keyWindow ?? NSApp.mainWindow
        while let sheet = window?.attachedSheet, !(sheet is NSSavePanel) { window = sheet }
        if let window, window.isVisible, window.attachedSheet == nil {
            panel.beginSheetModal(for: window) { response in completion(response == .OK ? panel.url : nil) }
        } else {
            completion(panel.runModal() == .OK ? panel.url : nil)
        }
    }
}

struct BrowserSettings: View {
    @ObservedObject var model: BrowserModel
    @State var section = L("Allgemein", "General")
    private var sections: [(String, String)] { [
        (L("Allgemein", "General"), "gearshape"),
        (L("Agenten", "Agents"), "cable.connector"),
        (L("Datenschutz & Werbung", "Privacy & ads"), "hand.raised"),
        (L("Downloads", "Downloads"), "arrow.down.circle"),
        (L("Erweiterungen"), "puzzlepiece.extension"),
        (L("Daten importieren"), "square.and.arrow.down"),
        (L("Lesezeichen"), "bookmark"),
        (L("Passwörter"), "key")
    ] }
    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: 10) {
                    YOBROMark(size: 28)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("YoBro").font(.system(size: 20, weight: .semibold, design: .rounded))
                        Text(L("EINSTELLUNGEN", "SETTINGS")).font(.system(size: 8, weight: .bold, design: .rounded)).tracking(1.4).foregroundStyle(moss)
                    }
                }.padding(.horizontal, 18).padding(.top, 25).padding(.bottom, 24)
                VStack(spacing: 5) {
                    ForEach(sections, id: \.0) { item in
                        Button { section = item.0; model.settingsSection = item.0 } label: {
                            HStack(spacing: 10) {
                                Image(systemName: item.1).frame(width: 18)
                                Text(item.0).font(.system(size: 12, weight: section == item.0 ? .semibold : .regular))
                                Spacer()
                            }
                            .padding(.horizontal, 11).frame(height: 39)
                            .foregroundStyle(section == item.0 ? ink : ink.opacity(0.68))
                            .background(section == item.0 ? YOBROTheme.surface.opacity(0.7) : .clear, in: RoundedRectangle(cornerRadius: 10))
                        }.buttonStyle(YOBROButtonStyle(minimumSize: 38))
                    }
                }.padding(.horizontal, 10)
                Spacer()
                Label(L("Daten bleiben auf diesem Mac", "Your data stays on this Mac"), systemImage: "internaldrive")
                    .font(.system(size: 9)).foregroundStyle(.secondary).padding(18)
            }
            .frame(width: 190).background(YOBROTheme.chromeBottom.opacity(0.5))

            Divider().opacity(0.5)

            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(section).font(.system(size: 23, weight: .semibold, design: .rounded))
                        Text(L("Passe YoBro an deinen Alltag an.", "Make YoBro fit the way you work.")).font(.system(size: 11)).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button { model.showSettings = false } label: {
                        Image(systemName: "xmark").font(.system(size: 11, weight: .semibold)).frame(width: 30, height: 30)
                            .background(YOBROTheme.surface.opacity(0.6), in: Circle())
                    }.buttonStyle(.plain).yobroHelp(L("Schließen", "Close")).keyboardShortcut(.cancelAction)
                }
                Divider().opacity(0.45)
                Group {
                    switch section {
                    case L("Allgemein", "General"): GeneralSettings(model: model)
                    case L("Agenten", "Agents"): AgentConnectionsView(model: model)
                    case L("Datenschutz & Werbung", "Privacy & ads"): AdBlockSettings(model: model)
                    case L("Downloads", "Downloads"): DownloadSettings(model: model)
                    case L("Erweiterungen"): ScrollView { ExtensionSettings(store: model.extensions, model: model) }
                    case L("Daten importieren"): BrowserImportView(model: model)
                    case L("Lesezeichen"): BookmarkSettings(model: model)
                    default: PasswordSettings(model: model)
                    }
                }.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }.padding(22)
        }
        .frame(width: 860, height: 650).background(paper).foregroundStyle(ink)
        .textFieldStyle(YOBROTextFieldStyle()).tint(moss)
        .onChange(of: model.settingsSection) { _, value in section = value }
    }
}

@MainActor
final class DefaultBrowserSettings: ObservableObject {
    @Published private(set) var isDefault = false
    @Published private(set) var isChanging = false
    @Published var error: String?

    init() { refresh() }

    func refresh() {
        guard let probe = URL(string: "https://example.com") else { return }
        let current = NSWorkspace.shared.urlForApplication(toOpen: probe)?.resolvingSymlinksInPath()
        isDefault = current == Bundle.main.bundleURL.resolvingSymlinksInPath()
    }

    func makeDefault() {
        guard !isChanging else { return }
        isChanging = true
        error = nil
        let application = Bundle.main.bundleURL
        let group = DispatchGroup()
        var errors: [Error] = []
        let lock = NSLock()

        for scheme in ["http", "https"] {
            group.enter()
            NSWorkspace.shared.setDefaultApplication(at: application, toOpenURLsWithScheme: scheme) { result in
                if let result {
                    lock.lock(); errors.append(result); lock.unlock()
                }
                group.leave()
            }
        }
        group.notify(queue: .main) { [weak self] in
            guard let self else { return }
            self.isChanging = false
            self.refresh()
            if let first = errors.first { self.error = first.localizedDescription }
        }
    }
}

struct GeneralSettings: View {
    @ObservedObject var model: BrowserModel
    @StateObject private var defaultBrowser = DefaultBrowserSettings()
    @AppStorage(SearchEngine.defaultsKey) private var searchEngine: SearchEngine = .duckDuckGo

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                YOBROSettingsHeading(
                    icon: "safari.fill",
                    title: L("YoBro als Standardbrowser", "YoBro as your default browser"),
                    detail: L("Öffne Weblinks aus Mail, Nachrichten und anderen Apps direkt in YoBro.", "Open web links from Mail, Messages, and other apps directly in YoBro.")
                )
                VStack(alignment: .leading, spacing: 13) {
                    HStack(spacing: 12) {
                        Image(systemName: defaultBrowser.isDefault ? "checkmark.circle.fill" : "circle.dashed")
                            .font(.system(size: 24)).foregroundStyle(defaultBrowser.isDefault ? moss : .secondary)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(defaultBrowser.isDefault ? L("YoBro ist dein Standardbrowser", "YoBro is your default browser") : L("Ein anderer Browser ist als Standard festgelegt", "Another browser is currently the default"))
                                .font(.system(size: 13, weight: .semibold))
                            Text(L("Diese Einstellung gilt systemweit für HTTP- und HTTPS-Links.", "This system-wide setting applies to HTTP and HTTPS links."))
                                .font(.system(size: 10)).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if !defaultBrowser.isDefault {
                            Button(L("Als Standard festlegen", "Make default")) { defaultBrowser.makeDefault() }
                                .buttonStyle(.borderedProminent).tint(moss).disabled(defaultBrowser.isChanging)
                        }
                    }
                    if defaultBrowser.isChanging { ProgressView().controlSize(.small) }
                    if let error = defaultBrowser.error {
                        Label(error, systemImage: "exclamationmark.triangle.fill")
                            .font(.system(size: 10)).foregroundStyle(.red)
                    }
                }.yobroCard(padding: 16, emphasized: true)

                YOBROSettingsHeading(
                    icon: "magnifyingglass",
                    title: L("Suchmaschine", "Search engine"),
                    detail: L("Was du in die Adresszeile tippst und keine Adresse ist, wird hier gesucht.", "Text you type in the address bar that is not an address is searched here.")
                )
                Picker(L("Suchen mit", "Search with"), selection: $searchEngine) {
                    ForEach(SearchEngine.allCases) { Text($0.name).tag($0) }
                }
                .pickerStyle(.menu).fixedSize()
                .frame(maxWidth: .infinity, alignment: .leading)
                .yobroCard(padding: 16)

                YOBROSettingsHeading(
                    icon: "bolt.badge.clock",
                    title: L("Leistung & Arbeitsspeicher", "Performance & Memory"),
                    detail: L("Schone den Arbeitsspeicher deines Mac durch automatisches Entlasten inaktiver Tabs.", "Save Mac memory by automatically suspending inactive tabs.")
                )
                Toggle(isOn: $model.autoSuspendInactiveTabs) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(L("Inaktive Tabs nach 30 Minuten schlafen legen", "Suspend inactive tabs after 30 minutes"))
                            .font(.system(size: 13, weight: .semibold))
                        Text(L("Gibt Arbeitsspeicher frei. Beim Anklicken wird der Tab an genau derselben Stelle wiederhergestellt. Tabs mit aktiver Medienwiedergabe bleiben geöffnet.", "Frees up RAM. Clicking the tab restores it right where you left off. Tabs actively playing media remain untouched."))
                            .font(.system(size: 10)).foregroundStyle(.secondary)
                    }
                }
                .toggleStyle(.switch)
                .tint(moss)
                .yobroCard(padding: 16)
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { defaultBrowser.refresh() }
    }
}

struct DownloadSettings: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject private var downloads: DownloadStore
    @State private var saved = false

    init(model: BrowserModel) {
        self.model = model
        downloads = model.downloads
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                YOBROSettingsHeading(
                    icon: "arrow.down.circle.fill",
                    title: L("Downloads speichern", "Save downloads"),
                    detail: L("Lege fest, in welchem Ordner neue Downloads gespeichert werden.", "Choose the folder where new downloads are saved.")
                )
                VStack(alignment: .leading, spacing: 12) {
                    Text(L("DOWNLOADORDNER", "DOWNLOAD FOLDER"))
                        .font(.system(size: 9, weight: .semibold)).tracking(1.2).foregroundStyle(.secondary)
                    HStack(spacing: 12) {
                        Image(systemName: "folder.fill").font(.system(size: 22)).foregroundStyle(moss)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(downloads.directory.lastPathComponent).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                            Text(downloads.directory.path).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(2).textSelection(.enabled)
                        }
                        Spacer(minLength: 8)
                        Button(L("Auswählen …", "Choose …")) { chooseDirectory() }
                            .buttonStyle(.borderedProminent).tint(moss)
                            .disabled(downloads.activeCount > 0)
                    }
                    Divider().opacity(0.4)
                    HStack {
                        Button { openDirectory() } label: {
                            Text(L("Im Finder öffnen", "Open in Finder"))
                                .padding(.horizontal, 11)
                        }
                        Button(L("Standard wiederherstellen", "Restore default")) { resetDirectory() }
                            .disabled(downloads.usesDefaultDirectory || downloads.activeCount > 0)
                        Spacer()
                    }.buttonStyle(YOBROButtonStyle()).font(.system(size: 11))
                    if downloads.activeCount > 0 {
                        Label(L("Der Ordner kann geändert werden, sobald alle Downloads beendet sind.", "You can change the folder after all downloads finish."), systemImage: "arrow.down.circle")
                            .font(.system(size: 10)).foregroundStyle(.secondary)
                    } else if saved {
                        Label(L("Gespeichert. Neue Downloads landen in diesem Ordner.", "Saved. New downloads go to this folder."), systemImage: "checkmark.circle.fill")
                            .font(.system(size: 10)).foregroundStyle(.secondary)
                    }
                }.yobroCard(padding: 16, emphasized: true)
            }.frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func chooseDirectory() {
        let panel = NSOpenPanel()
        panel.title = L("Downloadordner auswählen", "Choose download folder")
        panel.prompt = L("Auswählen", "Choose")
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        // The default folder may not exist yet; start beside it then.
        let current = downloads.directory
        panel.directoryURL = FileManager.default.fileExists(atPath: current.path) ? current : current.deletingLastPathComponent()
        saved = false
        FilePanel.present(panel) { url in
            guard let url else { return }
            apply { try downloads.setDirectory(url) }
        }
    }

    private func resetDirectory() {
        saved = false
        apply { try downloads.resetDirectory() }
    }

    private func apply(_ change: () throws -> Bool) {
        do {
            if try change() { saved = true }
            else { model.notice = L("Bitte warte, bis alle Downloads beendet sind.", "Please wait until all downloads finish.") }
        } catch { model.notice = error.localizedDescription }
    }

    private func openDirectory() {
        do {
            try FileManager.default.createDirectory(at: downloads.directory, withIntermediateDirectories: true)
            NSWorkspace.shared.open(downloads.directory)
        } catch { model.notice = error.localizedDescription }
    }
}

struct AdBlockSettings: View {
    @ObservedObject var model: BrowserModel
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                IncludedBrowserFeatures(store: model.extensions, appearance: model.webAppearance)
                Button(L("Erweiterungen verwalten …", "Manage extensions …"), systemImage: "puzzlepiece.extension") {
                    model.settingsSection = L("Erweiterungen")
                }
                .buttonStyle(YOBROButtonStyle()).font(.system(size: 11))
                WebsiteDataSettings(model: model)
                BridgeAccessSettings(model: model)
            }
        }
    }
}

struct BridgeAccessSettings: View {
    @ObservedObject var model: BrowserModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(L("Agentenschnittstelle", "Agent interface"), systemImage: "cable.connector")
                .font(.system(size: 12, weight: .semibold))
            Text(L("YoBro stellt lokal einen Socket bereit, über den Agenten den Browser steuern. Er ist auf deinen Benutzer beschränkt, aber jedes Programm, das als du läuft, kann ihn ansprechen.",
                   "YoBro provides a local socket that agents use to drive the browser. It is limited to your user account, but any program running as you can talk to it."))
                .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Text(model.controlSocketURL.path)
                .font(.system(size: 10, design: .monospaced)).foregroundStyle(moss).lineLimit(1).truncationMode(.middle)
            Divider().opacity(0.45)
            HStack(spacing: 14) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(L("Verlauf, Downloads und offene Tabs freigeben", "Allow history, downloads and open tabs"))
                        .font(.system(size: 13, weight: .semibold))
                    Text(model.bridgeLibraryAccess
                         ? L("Agenten können deinen Verlauf, deine Downloadpfade und die Titel und Adressen deiner Tabs auslesen. Private Tabs bleiben immer verborgen.", "Agents can read your history, download paths and the titles and addresses of your tabs. Private tabs always stay hidden.")
                         : L("Gesperrt. Alle anderen Befehle brauchen einen sichtbaren Agenten-Tab.", "Blocked. Every other command needs a visible agent tab."))
                        .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                Toggle("", isOn: Binding(
                    get: { model.bridgeLibraryAccess },
                    set: { model.setBridgeLibraryAccess($0) }
                ))
                .labelsHidden().toggleStyle(.switch)
            }
            Text(L("Den Zugriff insgesamt beendest du unter Einstellungen → Agenten.",
                   "To stop access entirely, turn it off in Settings → Agents."))
                .font(.system(size: 10)).foregroundStyle(.secondary)
        }
        .yobroCard(padding: 15)
    }
}

/// Clearing cookies, caches and local storage. YOBRO had no way to do this at
/// all: only the visited-pages list could be emptied, while the actual website
/// data stayed on disk forever.
struct WebsiteDataSettings: View {
    @ObservedObject var model: BrowserModel
    @State private var range: WebsiteDataRange = .everything
    @State private var includeHistory = false
    @State private var confirming = false
    @State private var working = false
    @State private var done: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(L("Browserdaten löschen", "Clear browsing data"), systemImage: "trash")
                .font(.system(size: 12, weight: .semibold))
            Text(L("Entfernt Cookies, Zwischenspeicher und lokal gespeicherte Websitedaten aus diesem Profil. Gespeicherte Passwörter bleiben erhalten.",
                   "Removes cookies, caches, and locally stored website data from this profile. Saved passwords are kept."))
                .font(.system(size: 11)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Picker("", selection: $range) {
                ForEach(WebsiteDataRange.allCases) { value in Text(value.title).tag(value) }
            }.labelsHidden().pickerStyle(.segmented)
            Toggle(L("Verlauf mitlöschen", "Clear history as well"), isOn: $includeHistory)
                .toggleStyle(.checkbox).font(.system(size: 11))
            HStack {
                if working { ProgressView().controlSize(.small) }
                if let done { Text(done).font(.system(size: 11)).foregroundStyle(moss) }
                Spacer()
                Button(L("Daten löschen …", "Clear data …")) { confirming = true }
                    .buttonStyle(YOBROButtonStyle()).font(.system(size: 11)).disabled(working)
            }
        }
        .yobroCard(padding: 15)
        .overlay {
            if confirming {
                YOBRODialogOverlay(
                    icon: "trash.fill",
                    title: L("Browserdaten löschen?", "Clear browsing data?"),
                    message: L("\(range.title): Cookies und Websitedaten werden entfernt. Du wirst auf betroffenen Seiten abgemeldet.",
                               "\(range.title): cookies and website data will be removed. You will be signed out of the affected sites."),
                    confirmTitle: L("Löschen", "Clear"),
                    cancelTitle: L("Abbrechen"),
                    destructive: true,
                    confirm: { confirming = false; clear() },
                    cancel: { confirming = false }
                )
            }
        }
    }

    private func clear() {
        working = true
        done = nil
        Task {
            await model.clearWebsiteData(range, includingHistory: includeHistory)
            working = false
            done = L("Erledigt.", "Done.")
        }
    }
}

struct ExtensionSettings: View {
    @ObservedObject var store: ExtensionStore
    @ObservedObject var model: BrowserModel
    @State private var removing: InstalledExtension?
    @State private var storeLink = ""
    @State private var extensionSearch = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            YOBROSettingsHeading(icon: "puzzlepiece.extension.fill", title: L("Erweiterungen für deinen Browser", "Extensions for your browser"), detail: store.supported ? L("Installiere kompatible WebExtensions lokal oder direkt über einen Link aus dem Chrome Web Store.", "Install compatible WebExtensions locally or from a Chrome Web Store link.") : L("Erweiterungen benötigen macOS 15.4 oder neuer.", "Extensions require macOS 15.4 or later."))
            IncludedBrowserFeatures(store: store, appearance: model.webAppearance)
            VStack(alignment: .leading, spacing: 12) {
                Text(L("Mehr für deinen Browser", "Make it yours")).font(.headline)
                Text(L("Entdecke WebExtensions im Chrome Web Store. YoBro prüft das Paket vor der Installation; manche Chrome-APIs sind in WebKit nicht verfügbar.", "Discover WebExtensions in the Chrome Web Store. YoBro checks the package before installation; some Chrome APIs are unavailable in WebKit."))
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    TextField(L("Erweiterungen suchen", "Search extensions"), text: $extensionSearch)
                        .onSubmit { searchExtensions() }
                    Button(L("Suchen", "Search"), systemImage: "magnifyingglass") { searchExtensions() }
                        .disabled(extensionSearch.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    Button(L("Katalog öffnen", "Browse catalog")) { model.showSettings = false; model.newTab(url: ExtensionCatalog.storeURL) }
                }
                HStack {
                    TextField(L("Chrome-Web-Store-Link oder Erweiterungs-ID", "Chrome Web Store link or extension ID"), text: $storeLink)
                    Button(L("Paket prüfen", "Review package")) { Task { await store.prepareFromStore(storeLink) } }
                        .disabled(storeLink.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || store.busy || !store.supported || store.pending != nil)
                    if store.busy { ProgressView().controlSize(.small) }
                }
                Button(L("Lokales Paket öffnen …", "Open local package …"), systemImage: "folder") { store.choose() }
                    .disabled(!store.supported || store.busy || store.pending != nil)
                DisclosureGroup(L("Was ist mit Safari-Erweiterungen?", "What about Safari extensions?")) {
                    VStack(alignment: .leading, spacing: 8) {
                        Text(L("Apps aus Apples Safari-Katalog werden in Safari installiert. Für YoBro brauchst du ein kompatibles WebExtension-Paket als Ordner, ZIP oder CRX. Unser Katalog-Link ist nicht an einen Länder-Store gebunden; die Verfügbarkeit beim Anbieter kann regional variieren.", "Apps from Apple's Safari catalog install in Safari. YoBro needs a compatible WebExtension folder, ZIP or CRX. Our catalog link is not tied to a country storefront; provider availability may vary by region."))
                        Button(L("Safari-Format bei Apple erklärt", "Apple's guide to Safari extensions")) { model.showSettings = false; model.newTab(url: ExtensionCatalog.safariInformationURL) }
                    }.font(.caption).foregroundStyle(.secondary).padding(.top, 6)
                }
            }.yobroCard(padding: 16)
            if let pending = store.pending {
                VStack(alignment: .leading, spacing: 10) {
                    Text("\(pending.name) · \(pending.version)").font(.headline)
                    ForEach(store.pendingWarnings, id: \.self) { Text($0).font(.caption).foregroundStyle(.orange) }
                    Text(L("Diese Erweiterung erhält Zugriff auf:")).font(.system(size: 12))
                    ScrollView {
                        Text((pending.permissions + pending.sites).isEmpty ? L("Keine angeforderten Berechtigungen") : (pending.permissions + pending.sites).joined(separator: "\n"))
                            .font(.system(size: 11, design: .monospaced)).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled)
                    }.frame(maxHeight: 90)
                    Text(L("Nur aus vertrauenswürdiger Quelle installieren. CRX-Dateien werden als lokale Pakete geladen; die Herausgebersignatur wird dabei nicht geprüft.")).font(.system(size: 11)).foregroundStyle(.secondary)
                    HStack {
                        Button(L("Abbrechen")) { store.cancelPending() }.disabled(store.busy)
                        Button(L("Zugriff erlauben und installieren")) { Task { await store.installPending() } }.disabled(store.busy).buttonStyle(.borderedProminent).tint(moss)
                    }
                }.yobroCard(padding: 15, emphasized: true)
            }
            if let message = store.message { Text(message).font(.system(size: 12)).textSelection(.enabled) }
            Text(L("Installiert", "Installed")).font(.headline)
            Group {
                LazyVStack(alignment: .leading, spacing: 12) {
                    if store.entries.isEmpty && store.pending == nil {
                        Label(L("Noch keine Erweiterungen installiert"), systemImage: "puzzlepiece.extension").foregroundStyle(.secondary).padding(.vertical, 40).frame(maxWidth: .infinity)
                    }
                    ForEach(store.entries) { entry in
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Image(systemName: "puzzlepiece.extension.fill").foregroundStyle(moss)
                                Text(entry.name).font(.headline)
                                Text(entry.version).font(.caption).foregroundStyle(.secondary)
                                Spacer()
                                Toggle(L("Aktiv"), isOn: Binding(get: { entry.enabled }, set: { _ in Task { await store.toggle(entry) } })).toggleStyle(.switch).disabled(store.busy || !store.supported)
                            }
                            Text((entry.permissions + entry.sites).joined(separator: " · ")).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(3)
                            if let error = store.errors[entry.id] { Text(error).font(.caption).foregroundStyle(.red) }
                            HStack {
                                Button(L("Öffnen")) { model.showSettings = false; store.perform(entry, tab: model.active) }.disabled(!entry.enabled || store.errors[entry.id] != nil)
                                Button(L("Einstellungen")) {
                                    if #available(macOS 15.4, *), let context = store.runtime.contexts[entry.id] {
                                        store.runtime.webExtensionController(store.runtime.controller, openOptionsPageFor: context) { error in if let error { store.message = error.localizedDescription } }
                                    }
                                }.disabled(!entry.enabled)
                                Spacer()
                                Button(L("Entfernen"), role: .destructive) { removing = entry }.disabled(store.busy)
                            }.buttonStyle(.bordered)
                        }.yobroCard(padding: 15)
                    }
                }
            }
            Text(L("Nicht verfügbar: native Begleitprogramme sowie Erweiterungszugriff auf YoBro-Lesezeichen, Verlauf, Downloads und Sitzungsverwaltung. Andere API-Unterschiede können einzelne Erweiterungen einschränken."))
                .font(.system(size: 11)).foregroundStyle(.secondary)
        }.overlay {
            if let entry = removing {
                YOBRODialogOverlay(icon: "puzzlepiece.extension.fill", title: L("Erweiterung entfernen?"), message: entry.name, confirmTitle: L("Entfernen"), cancelTitle: L("Abbrechen"), destructive: true, confirm: { store.remove(entry); removing = nil }, cancel: { removing = nil })
            }
        }
        .onDisappear { if !store.busy { store.cancelPending() } }
    }
    private func searchExtensions() {
        guard let url = ExtensionCatalog.searchURL(extensionSearch) else { return }
        model.showSettings = false
        model.newTab(url: url.absoluteString)
    }
}

struct BrowserImportView: View {
    @ObservedObject var model: BrowserModel
    @StateObject private var store = BrowserImportStore()
    @State private var confirming = false
    @State private var primaryPassword = ""
    var onImported: ((String) -> Void)?
    @MainActor init(model: BrowserModel, store: BrowserImportStore? = nil, onImported: ((String) -> Void)? = nil) {
        self.model = model; _store = StateObject(wrappedValue: store ?? BrowserImportStore()); self.onImported = onImported
    }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                YOBROSettingsHeading(icon: "square.and.arrow.down", title: L("Deine Daten ziehen mit um", "Bring your data with you"), detail: L("Wähle Browser, Profil und Datenarten. Erst „Auswahl importieren“ verändert dein YoBro-Profil.", "Choose a browser, profile, and data types. Nothing changes until you select Import."))
                HStack {
                    Picker("Browser", selection: $store.browser) { ForEach(ImportBrowser.all) { Text($0.name).tag($0.name) } }.frame(width: 190)
                    Picker(L("Profil"), selection: Binding(get: { store.profileID }, set: { store.selectProfile($0) })) {
                        Text(L("Profil wählen")).tag("")
                        ForEach(store.matchingProfiles) { Text($0.name).tag($0.id) }
                    }
                    Button(L("Ordner …")) { store.chooseFolder() }
                }.disabled(store.busy)
                ForEach(ImportKind.allCases) { kind in
                    HStack(alignment: .top, spacing: 12) {
                        Toggle(isOn: Binding(get: { store.selected.contains(kind) }, set: { if $0 { store.selected.insert(kind) } else { store.selected.remove(kind) } })) {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(kind.title).font(.system(size: 12, weight: .medium))
                                Text(store.preview.availability?[kind.rawValue] ?? kind.format).font(.system(size: 10)).foregroundStyle(.secondary)
                            }
                        }.toggleStyle(.checkbox)
                        Spacer()
                        Text(kind == .passwords && !store.passwordsLoaded && store.preview.passwords.isEmpty ? "—" : "\(store.preview.count(kind))").font(.system(size: 12, design: .monospaced)).foregroundStyle(moss).frame(width: 40)
                        Button(L("Datei …")) { store.chooseFile(kind) }.font(.system(size: 11))
                    }.yobroCard(padding: 12).disabled(store.busy)
                }
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Button(L(store.passwordsLoaded ? "Passwörter erneut entsperren" : "Passwörter entsperren", store.passwordsLoaded ? "Unlock passwords again" : "Unlock passwords")) {
                            Task { await store.unlockPasswords() }
                        }.disabled(store.busy || !store.canUnlockPasswords)
                        Text(L("macOS kann nach deiner Freigabe fragen.", "macOS may ask for your permission.")).font(.caption).foregroundStyle(.secondary)
                    }
                    if store.needsPrimaryPassword {
                        HStack {
                            SecureField(L("\(store.browser)-Hauptpasswort", "\(store.browser) Primary Password"), text: $primaryPassword)
                            Button(L("Entsperren", "Unlock")) {
                                let password = primaryPassword; primaryPassword = ""
                                Task { await store.unlockPasswords(primaryPassword: password) }
                            }.disabled(store.busy)
                        }
                    }
                    if let message = store.passwordMessage { Text(message).font(.caption).textSelection(.enabled) }
                    if store.browser == "Safari" {
                        Text(L("iCloud-Passwörter: In Apples App „Passwörter“ über Ablage → Alle Passwörter exportieren sichern und oben bei Passwörtern „Datei …“ wählen.", "iCloud passwords: In Apple’s Passwords app choose File → Export All Passwords, then select File … in the password row above.")).font(.caption).foregroundStyle(.secondary)
                    }
                }
                HStack {
                    Button(L("Erneut erkennen")) { Task { await store.readProfile() } }.disabled(store.profileID.isEmpty || store.busy)
                    Text(L("Die Profildaten werden aktualisiert; ergänzte Dateien bleiben erhalten.", "Profile data is refreshed; added files are kept.")).font(.system(size: 10)).foregroundStyle(.secondary)
                    if store.busy { ProgressView().controlSize(.small) }
                }
                if !store.preview.warnings.isEmpty {
                    VStack(alignment: .leading, spacing: 6) { ForEach(Array(store.preview.warnings.enumerated()), id: \.offset) { _, warning in Text(warning).font(.system(size: 11)).foregroundStyle(.orange) } }
                }
                if store.preview.count > 0 {
                    DisclosureGroup(L("Vorschau: \(store.preview.count) Einträge", "Preview: \(store.preview.count) items")) {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(Array((store.preview.bookmarks + store.preview.history + store.preview.tabs).prefix(15).enumerated()), id: \.offset) { _, entry in
                                Text("\(entry.title) · \(URL(string: entry.url)?.host ?? entry.url)").font(.system(size: 10)).lineLimit(1)
                            }
                            Text(L("Passwörter und Cookie-Werte werden hier nicht angezeigt.")).font(.system(size: 10)).foregroundStyle(.secondary)
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }
                    if store.selected.contains(.passwords), !store.preview.passwords.isEmpty {
                        Toggle(L("Bestehende YoBro-Passwörter für dieselben Konten ersetzen"), isOn: $store.replacePasswords).font(.system(size: 11))
                        Text(L("Speicherung im macOS-Schlüsselbund. Passwort-Exportdateien enthalten Klartext; entferne sie nach erfolgreichem Import, wenn du sie nicht mehr brauchst.")).font(.system(size: 10)).foregroundStyle(.secondary)
                    }
                    if store.selected.contains(.cookies), !store.preview.cookies.isEmpty {
                        Toggle(L("Vorhandene Cookies mit gleichem Namen und Pfad ersetzen"), isOn: $store.replaceCookies).font(.system(size: 11))
                    }
                }
                if let message = store.message { Text(message).font(.system(size: 12)).textSelection(.enabled) }
                Button(L("Auswahl importieren (\(store.selectedCount))", "Import selection (\(store.selectedCount))")) { confirming = true }.buttonStyle(.borderedProminent).tint(moss).disabled(store.selectedCount == 0 || store.busy)
            }
        }.frame(minHeight: 0, maxHeight: .infinity)
        .appearanceTask { await store.discover() }
        .onChange(of: store.browser) { _, _ in primaryPassword = ""; store.resetSource() }
        .onChange(of: store.profileID) { _, _ in primaryPassword = "" }
        .onDisappear { primaryPassword = "" }
        .overlay {
            if confirming {
                YOBRODialogOverlay(icon: "square.and.arrow.down", title: L("Ausgewählte Daten importieren?"), message: ImportKind.allCases.filter { store.selected.contains($0) }.map { "\($0.title): \(store.preview.count($0))" }.joined(separator: "\n") + L("\nTabs laden erst beim Öffnen. Cookies können den angemeldeten Account beeinflussen."), confirmTitle: L("Importieren"), cancelTitle: L("Abbrechen"), confirm: {
                    confirming = false
                    Task { await store.apply(to: model); if store.completed, let message = store.message { onImported?(message) } }
                }, cancel: { confirming = false })
            }
        }
    }
}

struct BookmarkSettings: View {
    @ObservedObject var model: BrowserModel
    @State private var query = ""
    @State private var error: String?
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            YOBROSettingsHeading(icon: "bookmark.fill", title: L("Deine Lesezeichen", "Your bookmarks"), detail: L("Finde gespeicherte Seiten wieder oder merke die aktuell geöffnete Seite.", "Find saved pages or bookmark the page you currently have open."))
            HStack {
                TextField(L("Lesezeichen suchen"), text: $query)
                Button(L("Aktuelle Seite merken")) {
                    guard let tab = model.active, let url = tab.webView.url, ["http", "https"].contains(url.scheme ?? "") else { return }
                    do { _ = try model.mergeImport(bookmarks: [ImportLink(title: tab.title, url: url.absoluteString, folder: L("Meine Lesezeichen"))], history: []) } catch { self.error = error.localizedDescription }
                }.disabled(model.active?.webView.url == nil)
            }
            if let error { Text(error).foregroundStyle(.red).font(.caption) }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 8) {
                    ForEach(model.bookmarks.filter { query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) || $0.url.localizedCaseInsensitiveContains(query) || $0.folder.localizedCaseInsensitiveContains(query) }) { entry in
                        HStack {
                            Button {
                                model.newTab(url: entry.url); model.showSettings = false
                            } label: {
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(entry.title).font(.system(size: 12, weight: .medium))
                                    Text("\(entry.folder) · \(entry.url)").font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
                                }.frame(maxWidth: .infinity, alignment: .leading)
                            }.buttonStyle(YOBROButtonStyle())
                            Button { remove(entry) } label: { Image(systemName: "trash") }.buttonStyle(YOBROButtonStyle()).yobroHelp(L("Lesezeichen entfernen"))
                        }.yobroCard(padding: 8)
                    }
                }
            }
            Text(L("\(model.bookmarks.count) Lesezeichen", "\(model.bookmarks.count) bookmarks")).font(.caption).foregroundStyle(.secondary)
        }
    }
    private func remove(_ entry: BookmarkEntry) { model.removeBookmark(entry) }
}

struct PasswordSettings: View {
    @ObservedObject var model: BrowserModel
    @State private var entries: [ImportPassword] = []
    @State private var message: String?
    @State private var loaded = false
    @State private var query = ""
    @State private var deleting: ImportPassword?
    @State private var neverSave: [String] = []

    private var visible: [ImportPassword] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return entries }
        return entries.filter { $0.host.localizedCaseInsensitiveContains(needle) || $0.username.localizedCaseInsensitiveContains(needle) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            PasskeySettings()
            YOBROSettingsHeading(icon: "key.fill", title: L("Passwörter im macOS-Schlüsselbund", "Passwords in macOS Keychain"), detail: L("Gespeicherte und importierte Passwörter liegen verschlüsselt im Schlüsselbund. Über den Schlüssel in der URL-Leiste kannst du sie auf der passenden Website ausfüllen.", "Saved and imported passwords are encrypted in Keychain. Use the key in the address bar to fill them on the matching website."))
            HStack {
                Button(L("Gespeicherte Konten laden")) { load() }
                if loaded && !entries.isEmpty {
                    TextField(L("Konten suchen", "Search accounts"), text: $query).frame(maxWidth: 260)
                }
            }
            if let message { Text(message).font(.caption) }
            ScrollView {
                LazyVStack(alignment: .leading) {
                    ForEach(visible, id: \.rowID) { entry in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(entry.host).font(.system(size: 12, weight: .medium))
                                Text(entry.username).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button(L("Benutzername kopieren")) { copy(entry.username) }
                            Button(L("Passwort kopieren")) { copy(entry.password) }
                            Button { deleting = entry } label: { Image(systemName: "trash") }
                                .buttonStyle(YOBROButtonStyle())
                                .yobroHelp(L("Login löschen", "Delete login"))
                                .accessibilityLabel(L("Login löschen", "Delete login"))
                        }.yobroCard(padding: 12)
                    }
                    if loaded && entries.isEmpty { Text(L("Noch keine Passwörter importiert.")).foregroundStyle(.secondary) }
                    if !entries.isEmpty && visible.isEmpty { Text(L("Keine passenden Konten.", "No matching accounts.")).foregroundStyle(.secondary) }
                    if !neverSave.isEmpty {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(L("Nie speichern für", "Never save for")).font(.system(size: 12, weight: .semibold))
                            ForEach(neverSave, id: \.self) { origin in
                                HStack {
                                    Text(URL(string: origin)?.host ?? origin).font(.system(size: 11))
                                    Spacer()
                                    Button { allowSaving(origin) } label: { Image(systemName: "xmark.circle") }
                                        .buttonStyle(YOBROButtonStyle())
                                        .yobroHelp(L("Wieder Speichern anbieten", "Offer saving again"))
                                        .accessibilityLabel(L("Wieder Speichern anbieten", "Offer saving again"))
                                }
                            }
                        }.yobroCard(padding: 12).padding(.top, 8)
                    }
                }
            }
        }
        .overlay {
            if let entry = deleting {
                YOBRODialogOverlay(
                    icon: "trash.fill",
                    title: L("Login löschen?", "Delete login?"),
                    message: "\(entry.host) · \(entry.username)",
                    confirmTitle: L("Löschen", "Delete"),
                    cancelTitle: L("Abbrechen"),
                    destructive: true,
                    confirm: { deleting = nil; delete(entry) },
                    cancel: { deleting = nil }
                )
            }
        }
        .onAppear { neverSave = LoginNeverList.all(home: model.home) }
        .onDisappear { entries = []; deleting = nil }
    }

    private func load() {
        do {
            entries = try PasswordVault.entries(home: model.home).sorted {
                ($0.host.lowercased(), $0.username.lowercased()) < ($1.host.lowercased(), $1.username.lowercased())
            }
            loaded = true; message = nil
        } catch { message = error.localizedDescription }
    }

    private func delete(_ entry: ImportPassword) {
        do {
            try PasswordVault.delete(entry, home: model.home)
            entries.removeAll { $0.rowID == entry.rowID }
        } catch { message = error.localizedDescription }
    }

    private func allowSaving(_ origin: String) {
        do {
            try LoginNeverList.remove(origin, home: model.home)
            neverSave = LoginNeverList.all(home: model.home)
        } catch { message = error.localizedDescription }
    }

    private func copy(_ value: String) {
        SecretPasteboard.copy(value)
        message = L("Kopiert. Die Zwischenablage wird nach 30 Sekunden geleert, sofern sie unverändert ist.")
    }
}

private extension ImportPassword {
    var host: String { URL(string: url)?.host ?? url }
    var rowID: String { url + "\n" + username }
}

/// Copies a secret so it stays on this Mac: no Universal Clipboard, and
/// marked concealed for clipboard managers. It is cleared after 30 seconds
/// and when YoBro quits, unless something else was copied since.
@MainActor
enum SecretPasteboard {
    private static var copiedChange: Int?
    private static var terminationObserver: NSObjectProtocol?

    static func copy(_ value: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.prepareForNewContents(with: .currentHostOnly)
        pasteboard.setString(value, forType: .string)
        pasteboard.setData(Data(), forType: .init("org.nspasteboard.ConcealedType"))
        let change = pasteboard.changeCount
        copiedChange = change
        if terminationObserver == nil {
            terminationObserver = NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { clearIfUnchanged() }
            }
        }
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 30_000_000_000)
            if pasteboard.changeCount == change { pasteboard.clearContents() }
        }
    }

    private static func clearIfUnchanged() {
        guard let copiedChange, NSPasteboard.general.changeCount == copiedChange else { return }
        NSPasteboard.general.clearContents()
    }
}
