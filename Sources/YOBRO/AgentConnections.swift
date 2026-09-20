import SwiftUI
import AppKit

struct AgentAccessPermission: Codable {
    var allowed: Bool
    static func load(home: URL) -> Self {
        guard let data = try? Data(contentsOf: home.appendingPathComponent("agent-access.json")),
              let permission = try? JSONDecoder().decode(Self.self, from: data) else { return Self(allowed: false) }
        return permission
    }
}

enum AgentClient: String, CaseIterable, Identifiable {
    case claude, codex, chatgpt
    var id: String { rawValue }
    var title: String { switch self { case .claude: "Claude Desktop"; case .codex: "Codex"; case .chatgpt: "ChatGPT (Remote)" } }
}

struct AgentConnectionBinding: Codable, Equatable {
    let socketName: String
    static func file(client: AgentClient, root: URL) -> URL { root.appendingPathComponent("AgentConnections/\(client.rawValue).json") }
    static func read(client: AgentClient, root: URL) -> Self? {
        guard let data = try? Data(contentsOf: file(client: client, root: root)) else { return nil }
        return try? JSONDecoder().decode(Self.self, from: data)
    }
    static func save(client: AgentClient, socket: URL) throws {
        let target = file(client: client, root: socket.deletingLastPathComponent())
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try JSONEncoder().encode(Self(socketName: socket.lastPathComponent)).write(to: target, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: target.path)
    }
}

@MainActor
enum AgentInstaller {
    static var integrations: URL? { Bundle.main.resourceURL?.appendingPathComponent("AgentIntegrations") }
    static var helper: URL { Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/yobro-mcp") }
    static func requireInstalledApp() throws {
        let path = Bundle.main.bundleURL.path
        guard !path.hasPrefix("/Volumes/"), !path.contains("/AppTranslocation/") else {
            throw YOBROError.message(L("Ziehe YoBro zuerst nach Programme und starte es von dort. Danach kannst du deinen Agenten verbinden.", "First drag YoBro into Applications and launch it there. Then connect your agent."))
        }
    }
    static func exportClaude() throws -> Bool {
        try requireInstalledApp()
        guard let source = integrations?.appendingPathComponent("YOBRO.mcpb"), FileManager.default.fileExists(atPath: source.path) else {
            throw YOBROError.message(L("Die Erweiterung fehlt. Verwende die vollständige YoBro-App aus der DMG.", "The extension is missing. Use the complete YoBro app from the DMG."))
        }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "YOBRO.mcpb"
        panel.message = L("Speichere die Erweiterung und installiere sie danach in Claude unter Einstellungen → Extensions.", "Save the extension, then install it in Claude under Settings → Extensions.")
        guard panel.runModal() == .OK, let destination = panel.url else { return false }
        try Data(contentsOf: source).write(to: destination, options: .atomic)
        NSWorkspace.shared.activateFileViewerSelecting([destination])
        return true
    }
    static func codexExecutable() -> URL? {
        let paths = ["Codex", "ChatGPT"].flatMap { name in
            ["/Applications/\(name).app/Contents/Resources/codex", FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/\(name).app/Contents/Resources/codex").path]
        } + ["/opt/homebrew/bin/codex", "/usr/local/bin/codex"]
        return paths.first(where: { FileManager.default.isExecutableFile(atPath: $0) }).map { URL(fileURLWithPath: $0) }
    }
    // No shell and no private CLI diagnostics copied into the UI or logs.
    static func run(_ executable: URL, _ arguments: [String]) async throws -> Int32 {
        try await withCheckedThrowingContinuation { continuation in
            let process = Process(); process.executableURL = executable; process.arguments = arguments
            process.standardInput = FileHandle.nullDevice
            process.standardOutput = FileHandle.nullDevice; process.standardError = FileHandle.nullDevice
            process.terminationHandler = { continuation.resume(returning: $0.terminationStatus) }
            do { try process.run() }
            catch { process.terminationHandler = nil; continuation.resume(throwing: error) }
            DispatchQueue.global().asyncAfter(deadline: .now() + 60) { if process.isRunning { process.terminate() } }
        }
    }
    static func installCodex() async throws {
        try requireInstalledApp()
        guard let executable = codexExecutable() else { throw YOBROError.message(L("Installiere zuerst Codex bzw. die ChatGPT-Desktop-App mit Codex-Unterstützung.", "Install Codex or the ChatGPT desktop app with Codex support first.")) }
        guard let marketplace = integrations?.appendingPathComponent("Codex"), FileManager.default.fileExists(atPath: marketplace.appendingPathComponent(".agents/plugins/marketplace.json").path) else {
            throw YOBROError.message(L("Die Plugin-Dateien fehlen in dieser App.", "Plugin files are missing from this app."))
        }
        guard try await run(executable, ["plugin", "marketplace", "add", marketplace.path]) == 0 else {
            throw YOBROError.message(L("Der Plugin-Katalog konnte nicht eingerichtet werden. Prüfe die Codex-Version und Plugin-Berechtigungen.", "Could not set up the plugin catalog. Check the Codex version and plugin permissions."))
        }
        guard try await run(executable, ["plugin", "add", "yobro-browser@yobro-bundled"]) == 0 else {
            throw YOBROError.message(L("Das Plugin konnte nicht installiert werden. Prüfe die Plugin-Einstellungen in Codex.", "Could not install the plugin. Check plugin settings in Codex."))
        }
    }
}

struct AgentConnectionsView: View {
    @ObservedObject var model: BrowserModel
    @State private var client: AgentClient = .claude
    @State private var message = ""
    @State private var busy = false
    @State private var revision = 0
    private var root: URL { model.controlSocketURL.deletingLastPathComponent() }
    private var bound: Bool { AgentConnectionBinding.read(client: client, root: root)?.socketName == model.controlSocketURL.lastPathComponent }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text(L("Verbinde deinen Agenten", "Connect your agent")).font(.title2)
                Text(L("Die Verbindung bleibt bei diesem Browserprofil. Ein Profilwechsel gibt dem Agenten nicht automatisch andere Daten frei.", "The connection stays with this browser profile. Switching profiles does not automatically give the agent access to other data."))
                    .font(.callout).foregroundStyle(.secondary)
                Picker(L("Agent", "Agent"), selection: $client) { ForEach(AgentClient.allCases) { Text($0.title).tag($0) } }
                    .pickerStyle(.segmented).disabled(busy)
                if client == .chatgpt {
                    Label(L("Öffentliche Verbindung noch nicht verfügbar", "Public connection not available yet"), systemImage: "network")
                    Text(L("Remote-Chats benötigen einen gesonderten Verbindungsdienst. YoBro öffnet hier keine Netzwerkports und lädt keine Browserdaten hoch. Nutzt deine Desktop-App lokale Codex-Plugins, wähle Codex.", "Remote chats require a separate connection service. YoBro does not open network ports or upload browser data here. If your desktop app supports local Codex plugins, choose Codex."))
                        .font(.callout).foregroundStyle(.secondary)
                } else {
                    Label(model.profileName, systemImage: "person.crop.circle")
                    Text(bound ? L("Mit diesem Profil verknüpft", "Linked to this profile") : L("Noch nicht verknüpft", "Not linked yet"))
                        .font(.caption).foregroundStyle(.secondary).id(revision)
                    Button(L("Dieses Profil verbinden", "Connect this profile")) {
                        do {
                            try AgentConnectionBinding.save(client: client, socket: model.controlSocketURL); revision += 1
                            message = L("Profil gespeichert. Installiere die Erweiterung und aktiviere den Zugriff, wenn du bereit bist.", "Profile saved. Install the extension and enable access when you are ready.")
                        } catch { message = L("Die Verknüpfung konnte nicht gespeichert werden.", "Could not save the connection.") }
                    }.disabled(busy)
                    if bound {
                        HStack {
                            Button(client == .claude ? L("Claude-Erweiterung speichern …", "Save Claude extension …") : L("Codex-Plugin installieren", "Install Codex plugin")) { install() }.disabled(busy)
                            if busy { ProgressView().controlSize(.small) }
                        }
                        Toggle(L("Agent-Zugriff für dieses Profil erlauben", "Allow agent access to this profile"), isOn: $model.agentEnabled)
                        Text(L("Gelesene Seiteninhalte gehen an deinen Agent-Anbieter. Website-Logins werden innerhalb dieses Profils verwendet. Die Installation kopiert keine Passwörter, Cookies oder VPN-Schlüssel in die Erweiterung.", "Read page content is sent to your agent provider. Website logins are used within this profile. Installation does not copy passwords, cookies, or VPN keys into the extension."))
                            .font(.caption).foregroundStyle(.secondary)
                        HStack {
                            Button(L("Lokale Verbindung prüfen", "Check local connection")) { check() }.disabled(busy)
                            Button(L("Verknüpfung entfernen", "Remove connection")) {
                                do {
                                    try FileManager.default.removeItem(at: AgentConnectionBinding.file(client: client, root: root))
                                    model.pauseAgentWorkspace(); revision += 1
                                    message = L("Verknüpfung entfernt und Zugriff pausiert. Die Erweiterung kannst du zusätzlich in deinem Agenten deinstallieren.", "Connection removed and access paused. You can also uninstall the extension in your agent.")
                                } catch { message = L("Die Verknüpfung konnte nicht entfernt werden.", "Could not remove the connection.") }
                            }.disabled(busy)
                        }
                    }
                }
                if !message.isEmpty { Text(message).font(.callout).fixedSize(horizontal: false, vertical: true) }
            }.padding(2)
        }.onChange(of: client) { _, _ in message = "" }
    }
    private func install() {
        busy = true
        Task { @MainActor in
            defer { busy = false }
            do {
                if client == .claude {
                    if try AgentInstaller.exportClaude() { message = L("Öffne Claude → Einstellungen → Extensions → Erweiterung installieren und wähle YOBRO.mcpb. Bestätige dort die Installation.", "Open Claude → Settings → Extensions → Install Extension and select YOBRO.mcpb. Confirm installation there.") }
                } else {
                    try await AgentInstaller.installCodex()
                    message = L("Plugin installiert. Starte einen neuen Codex-Chat mit aktiviertem YOBRO-Plugin.", "Plugin installed. Start a new Codex chat with the YOBRO plugin enabled.")
                }
            } catch { message = error.localizedDescription }
        }
    }
    private func check() {
        busy = true
        Task { @MainActor in
            defer { busy = false }
            do {
                let code = try await AgentInstaller.run(AgentInstaller.helper, ["--connection", client.rawValue, "--check"])
                message = code == 0
                    ? L("Lokale Verbindung bereit. Bitte teste im Agenten: „Prüfe den YOBRO-Status“. Erst das bestätigt die gesamte Verbindung.", "Local connection ready. Ask your agent to check YOBRO status to confirm the complete connection.")
                    : L("Nicht bereit. Aktiviere den Zugriff und öffne das verknüpfte Profil.", "Not ready. Enable access and open the linked profile.")
            } catch { message = L("Der Verbindungshelfer konnte nicht gestartet werden.", "Could not start the connection helper.") }
        }
    }
}
