import SwiftUI
import WebKit
import Combine
import Security

struct LocalBrowserProfile: Codable, Identifiable, Equatable {
    // The original profile keeps its existing files and WebKit store.
    static let originalID = UUID(uuidString: "00000000-0000-0000-0000-000000000001")!
    var id: UUID
    var name: String
    var symbol: String
    var isOriginal: Bool { id == Self.originalID }
    func home(in root: URL) -> URL { isOriginal ? root : root.appendingPathComponent("Profiles").appendingPathComponent(id.uuidString) }
}

@MainActor
final class BrowserProfiles: ObservableObject {
    struct Registry: Codable {
        var profiles: [LocalBrowserProfile]
        var activeID: UUID
    }
    @Published private(set) var registry: Registry
    @Published private(set) var browser: BrowserModel
    let root: URL
    private var browserChanges: AnyCancellable?
    private var registryLoadFailed = false
    private var models: [UUID: BrowserModel] = [:]
    private var file: URL { root.appendingPathComponent("profiles.json") }
    var active: LocalBrowserProfile { registry.profiles.first { $0.id == registry.activeID }! }
    static let symbols = ["person.crop.circle", "house.fill", "briefcase.fill", "flask.fill", "leaf.fill", "moon.fill", "sparkles", "heart.fill"]

    init(root: URL? = nil) {
        let root = root ?? BrowserModel.defaultHome
        self.root = root
        let original = LocalBrowserProfile(id: LocalBrowserProfile.originalID, name: L("Privat", "Personal"), symbol: "person.crop.circle")
        let fallback = Registry(profiles: [original], activeID: original.id)
        var registry = fallback
        var loadError: Error?
        let file = root.appendingPathComponent("profiles.json")
        if FileManager.default.fileExists(atPath: file.path) {
            do {
                let saved = try JSONDecoder().decode(Registry.self, from: Data(contentsOf: file))
                guard !saved.profiles.isEmpty, Set(saved.profiles.map(\.id)).count == saved.profiles.count,
                      saved.profiles.contains(where: { $0.id == saved.activeID }), saved.profiles.contains(where: \.isOriginal) else {
                    throw YOBROError.message(L("Die Profilliste ist ungültig.", "The profile list is invalid."))
                }
                registry = saved
            } catch { loadError = error }
        }
        self.registry = registry
        let active = registry.profiles.first { $0.id == registry.activeID }!
        browser = BrowserModel(profile: active, root: root)
        models[active.id] = browser
        browser.profileSession = self
        observeBrowser()
        registryLoadFailed = loadError != nil
        if let loadError { browser.notice = L("Die Profilliste konnte nicht geladen werden. Die vorhandene Datei bleibt erhalten: ", "Could not load the profile list. The existing file is preserved: ") + loadError.localizedDescription }
    }

    private func save(_ value: Registry) throws {
        guard !registryLoadFailed else { throw YOBROError.message(L("Die gespeicherte Profilliste muss zuerst wiederhergestellt werden.", "The saved profile list must be restored first.")) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try JSONEncoder().encode(value).write(to: file, options: .atomic)
        registry = value
    }
    private func checkedName(_ name: String, excluding id: UUID? = nil) throws -> String {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, name.count <= 40 else { throw YOBROError.message(L("Bitte einen Namen mit 1–40 Zeichen eingeben.", "Enter a name with 1–40 characters.")) }
        guard !registry.profiles.contains(where: { $0.id != id && $0.name.localizedCaseInsensitiveCompare(name) == .orderedSame }) else {
            throw YOBROError.message(L("Diesen Profilnamen gibt es bereits.", "This profile name already exists."))
        }
        return name
    }
    func create(name: String, symbol: String) throws {
        let profile = LocalBrowserProfile(id: UUID(), name: try checkedName(name), symbol: Self.symbols.contains(symbol) ? symbol : Self.symbols[0])
        var updated = registry; updated.profiles.append(profile)
        try save(updated)
        try switchTo(profile.id)
    }
    func edit(name: String, symbol: String) throws {
        let name = try checkedName(name, excluding: active.id)
        var updated = registry
        let index = updated.profiles.firstIndex { $0.id == active.id }!
        updated.profiles[index].name = name
        updated.profiles[index].symbol = Self.symbols.contains(symbol) ? symbol : Self.symbols[0]
        try save(updated)
        browser.profileName = name
    }
    func switchTo(_ id: UUID) throws {
        guard id != registry.activeID, let profile = registry.profiles.first(where: { $0.id == id }) else { return }
        var updated = registry; updated.activeID = id
        try save(updated)
        browser.persistSession()
        browser.isProfileActive = false
        for tab in browser.tabs { tab.webView.pauseAllMediaPlayback(completionHandler: nil) }
        let next = models[id] ?? BrowserModel(profile: profile, root: root)
        models[id] = next
        next.profileSession = self; next.isProfileActive = true
        next.showSettings = false; next.showProfileEditor = false
        // Layout preferences are app-wide, but a profile opened earlier still
        // holds the values it read when it was created.
        let defaults = UserDefaults.standard
        let tabsOnTop = defaults.bool(forKey: "YOBRO.tabsOnTop")
        let showSidebar = defaults.object(forKey: "YOBRO.showSidebar") as? Bool ?? true
        let autoSuspend = defaults.object(forKey: "YOBRO.autoSuspendInactiveTabs") as? Bool ?? true
        if next.tabsOnTop != tabsOnTop { next.tabsOnTop = tabsOnTop }
        if next.showSidebar != showSidebar { next.showSidebar = showSidebar }
        if next.autoSuspendInactiveTabs != autoSuspend { next.autoSuspendInactiveTabs = autoSuspend }
        browser = next
        observeBrowser()
    }
    /// Removes a profile with its tabs, bookmarks, website data, logins and
    /// agent links. The original profile cannot be deleted.
    func delete(_ id: UUID) throws {
        guard id != LocalBrowserProfile.originalID else {
            throw YOBROError.message(L("Das ursprüngliche Profil kann nicht gelöscht werden.", "The original profile cannot be deleted."))
        }
        guard let profile = registry.profiles.first(where: { $0.id == id }) else { return }
        if id == registry.activeID { try switchTo(LocalBrowserProfile.originalID) }
        var updated = registry
        updated.profiles.removeAll { $0.id == id }
        try save(updated)
        // Dropping the model releases its control bridge, which unlinks the socket.
        models.removeValue(forKey: id)?.isProfileActive = false
        let home = profile.home(in: root)
        for service in [PasswordVault.service(home: home), AgentAPIKeySecrets.service(home: home)] {
            SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service] as CFDictionary)
        }
        let socketName = "p-\(id.uuidString).sock"
        for client in AgentClient.allCases where AgentConnectionBinding.read(client: client, root: root)?.socketName == socketName {
            try? FileManager.default.removeItem(at: AgentConnectionBinding.file(client: client, root: root))
        }
        var failure: Error?
        if FileManager.default.fileExists(atPath: home.path) {
            do { try FileManager.default.removeItem(at: home) } catch { failure = error }
        }
        // WebKit frees the store only after the profile's last web view is gone.
        Task.detached {
            for attempt in 1...20 {
                do { try await WKWebsiteDataStore.remove(forIdentifier: id); return }
                catch { if attempt < 20 { try? await Task.sleep(nanoseconds: 250_000_000) } }
            }
        }
        if let failure { throw failure }
    }
    private func observeBrowser() {
        browserChanges = browser.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
    }
    func persistAll() { for model in models.values { model.persistSession() } }
}

struct ProfileEditor: View {
    @ObservedObject var profiles: BrowserProfiles
    let creating: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var symbol = BrowserProfiles.symbols[0]
    @State private var error: String?
    @State private var confirmingDelete = false
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text(creating ? L("Neues Profil", "New profile") : L("Profil bearbeiten", "Edit profile")).font(.title2)
            Text(L("Eigene Anmeldungen, Tabs, Spaces, Lesezeichen und Passwörter. Lokal auf deinem Mac, ohne Registrierung.", "Separate logins, tabs, spaces, bookmarks and passwords. Local to your Mac, no account required.")).font(.callout).foregroundStyle(.secondary)
            TextField(L("Profilname", "Profile name"), text: $name)
            HStack {
                ForEach(BrowserProfiles.symbols, id: \.self) { icon in
                    Button { symbol = icon } label: {
                        Image(systemName: icon).frame(width: 36, height: 36).background(symbol == icon ? moss.opacity(0.3) : .clear, in: Circle())
                    }.buttonStyle(.plain).accessibilityLabel(icon).accessibilityAddTraits(symbol == icon ? .isSelected : [])
                }
            }
            if creating {
                Text(L("Das Profil startet leer. Daten kannst du anschließend gezielt importieren. Agentenzugriff ist zunächst pausiert.", "The profile starts empty. You can import data afterwards. Agent access is initially paused.")).font(.caption).foregroundStyle(.secondary)
            }
            if let error { Text(error).font(.caption).foregroundStyle(.red) }
            HStack {
                Button(L("Abbrechen", "Cancel")) { dismiss() }.keyboardShortcut(.cancelAction)
                if !creating && !profiles.active.isOriginal {
                    Button(L("Profil löschen …", "Delete profile …"), role: .destructive) { confirmingDelete = true }
                        .foregroundStyle(.red)
                }
                Spacer()
                Button(creating ? L("Profil erstellen", "Create profile") : L("Speichern", "Save")) {
                    do {
                        if creating { try profiles.create(name: name, symbol: symbol) }
                        else { try profiles.edit(name: name, symbol: symbol) }
                        dismiss()
                    } catch { self.error = error.localizedDescription }
                }.buttonStyle(.borderedProminent).tint(moss).keyboardShortcut(.defaultAction)
            }
        }.padding(24).frame(width: 420).background(paper).foregroundStyle(ink)
            .textFieldStyle(YOBROTextFieldStyle()).tint(moss)
            .overlay {
                if confirmingDelete {
                    YOBRODialogOverlay(
                        icon: "trash.fill",
                        title: L("Profil „\(profiles.active.name)“ löschen?", "Delete profile “\(profiles.active.name)”?"),
                        message: L("Die Anmeldungen, Tabs, Lesezeichen und Passwörter dieses Profils werden von diesem Mac entfernt.", "This profile's logins, tabs, bookmarks and passwords will be removed from this Mac."),
                        confirmTitle: L("Löschen", "Delete"),
                        cancelTitle: L("Abbrechen", "Cancel"),
                        destructive: true,
                        confirm: {
                            let id = profiles.active.id
                            confirmingDelete = false
                            dismiss()
                            do { try profiles.delete(id) } catch { profiles.browser.notice = error.localizedDescription }
                        },
                        cancel: { confirmingDelete = false }
                    )
                }
            }
            .onAppear { if !creating { name = profiles.active.name; symbol = profiles.active.symbol } }
    }
}
