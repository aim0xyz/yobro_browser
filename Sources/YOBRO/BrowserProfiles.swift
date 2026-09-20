import SwiftUI
import WebKit
import Combine

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
        browser.mail.stopPolling()
        for tab in browser.tabs { tab.webView.pauseAllMediaPlayback(completionHandler: nil) }
        let next = models[id] ?? BrowserModel(profile: profile, root: root)
        models[id] = next
        next.profileSession = self; next.isProfileActive = true
        next.showSettings = false; next.showProfileEditor = false
        browser = next
        observeBrowser()
    }
    private func observeBrowser() {
        browserChanges = browser.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
    }
    func persistAll() { for model in models.values { model.persistSession() } }
}

struct ProfileMenu: View {
    @ObservedObject var profiles: BrowserProfiles
    @ObservedObject var model: BrowserModel
    var compact = false
    @State private var hovering = false
    var body: some View {
        Menu {
            Text(L("Lokales Profil", "Local profile") + " · " + profiles.active.name)
            ForEach(profiles.registry.profiles) { profile in
                Button {
                    do { try profiles.switchTo(profile.id) } catch { model.notice = error.localizedDescription }
                } label: {
                    Label(profile.name, systemImage: profile.id == profiles.active.id ? "checkmark" : profile.symbol)
                }
            }
            Divider()
            Button(L("Neues Profil …", "New profile …")) { model.creatingProfile = true; model.showProfileEditor = true }
            Button(L("Profil bearbeiten …", "Edit profile …")) { model.creatingProfile = false; model.showProfileEditor = true }
            Divider()
            Button(L("Einstellungen …", "Settings …")) { model.settingsSection = L("Erweiterungen"); model.showSettings = true }
            Button(L("Browserdaten importieren …", "Import browser data …")) { model.settingsSection = L("Daten importieren"); model.showSettings = true }
        } label: {
            profileLabel.contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .accessibilityLabel(L("Profil & Einstellungen", "Profile & settings") + ": " + profiles.active.name)
        .frame(width: compact ? 42 : 214, height: compact ? 40 : 50)
        .overlay {
            if compact {
                RoundedRectangle(cornerRadius: 9)
                    .fill(ink.opacity(hovering ? 0.065 : 0))
                    .allowsHitTesting(false)
            }
        }
        .onHover { hovering = $0 }
        .yobroHelp(L("Profil: ", "Profile: ") + profiles.active.name + "\n" + L("Profil wechseln und Einstellungen öffnen", "Switch profiles and open settings"))
    }

    @ViewBuilder
    private var profileLabel: some View {
        if compact {
            ZStack(alignment: .bottomTrailing) {
                Image(systemName: profiles.active.symbol)
                    .font(.system(size: 18, weight: .medium))
                    .foregroundStyle(moss)
                    .frame(width: 42, height: 40)
                    .background(YOBROTheme.surface.opacity(0.58), in: RoundedRectangle(cornerRadius: 10))
                Circle()
                    .fill(moss)
                    .frame(width: 7, height: 7)
                    .overlay(Circle().stroke(YOBROTheme.chromeBottom, lineWidth: 2))
                    .padding(5)
            }
        } else {
            HStack(spacing: 10) {
                Image(systemName: profiles.active.symbol)
                    .font(.system(size: 17, weight: .medium))
                    .frame(width: 34, height: 34)
                    .background(moss.opacity(0.14), in: Circle())
                VStack(alignment: .leading, spacing: 2) {
                    Text(profiles.active.name).font(.system(size: 12, weight: .semibold))
                    Text(L("Profil & Einstellungen", "Profile & settings"))
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Image(systemName: "chevron.up.chevron.down")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 10)
            .frame(width: 214, height: 50)
            .background(YOBROTheme.surface.opacity(0.38), in: RoundedRectangle(cornerRadius: 12))
        }
    }
}

struct ProfileEditor: View {
    @ObservedObject var profiles: BrowserProfiles
    let creating: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var symbol = BrowserProfiles.symbols[0]
    @State private var error: String?
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
            .onAppear { if !creating { name = profiles.active.name; symbol = profiles.active.symbol } }
    }
}
