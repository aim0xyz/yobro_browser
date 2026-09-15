import SwiftUI
import WebKit

struct SpaceProxySheet: View {
    @ObservedObject var model: BrowserModel
    let spaceName: String
    @Environment(\.dismiss) private var dismiss

    @State private var enabled: Bool = true
    @State private var label: String = ""
    @State private var type: SpaceProxyType = .socks5
    @State private var host: String = ""
    @State private var portString: String = "1080"
    @State private var username: String = ""
    @State private var password: String = ""
    @State private var excludedDomainsString: String = "localhost, 127.0.0.1, [::1]"
    @State private var errorMessage: String?
    @State private var isTesting = false

    init(model: BrowserModel, space: String) {
        self.model = model
        self.spaceName = space
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Image(systemName: "shield.lefthalf.filled").font(.system(size: 20)).foregroundStyle(moss)
                VStack(alignment: .leading, spacing: 2) {
                    Text(L("Proxy / VPN für Space „\(spaceName)“", "Proxy / VPN for Space \"\(spaceName)\""))
                        .font(.headline)
                    Text(L("Routet den gesamten Webverkehr dieses Space über einen SOCKS5- oder HTTP-Proxy.", "Routes all web traffic of this Space through a SOCKS5 or HTTP proxy."))
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button { dismiss() } label: { Image(systemName: "xmark") }.buttonStyle(YOBROButtonStyle())
            }

            Toggle(L("Proxy für diesen Space aktivieren", "Enable proxy for this Space"), isOn: $enabled)
                .font(.system(size: 13, weight: .medium))

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(L("Bezeichnung (optional)", "Label (optional)")).font(.caption).foregroundStyle(.secondary)
                        TextField(L("z. B. NordVPN US, Mullvad Frankfurt, Lokal", "e.g. NordVPN US, Mullvad Frankfurt, Local"), text: $label)
                    }

                    VStack(alignment: .leading, spacing: 6) {
                        Text(L("Protokoll-Typ", "Protocol type")).font(.caption).foregroundStyle(.secondary)
                        Picker("", selection: $type) {
                            ForEach(SpaceProxyType.allCases) { item in
                                Text(item.rawValue).tag(item)
                            }
                        }.pickerStyle(.segmented).labelsHidden()
                    }

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(L("Server Host / IP", "Server Host / IP")).font(.caption).foregroundStyle(.secondary)
                            TextField(L("z. B. nl.socks.nordhold.net oder 127.0.0.1", "e.g. nl.socks.nordhold.net or 127.0.0.1"), text: $host)
                        }
                        VStack(alignment: .leading, spacing: 6) {
                            Text(L("Port", "Port")).font(.caption).foregroundStyle(.secondary)
                            TextField("1080", text: $portString).frame(width: 80)
                        }
                    }

                    // Quick Presets
                    VStack(alignment: .leading, spacing: 6) {
                        Text(L("Schnell-Vorlagen", "Quick Presets")).font(.caption).foregroundStyle(.secondary)
                        HStack(spacing: 8) {
                            Button(L("NordVPN Niederlande · SOCKS5")) {
                                host = "nl.socks.nordhold.net"
                                portString = "1080"
                                type = .socks5
                                if label.isEmpty { label = "NordVPN" }
                            }.font(.system(size: 11)).buttonStyle(.bordered)

                            Button(L("Lokal SOCKS5 (127.0.0.1:1080)")) {
                                host = "127.0.0.1"
                                portString = "1080"
                                type = .socks5
                                if label.isEmpty { label = L("Lokaler SOCKS5", "Local SOCKS5") }
                            }.font(.system(size: 11)).buttonStyle(.bordered)

                            Button(L("Mullvad SOCKS5 (1080)")) {
                                host = "socks.mullvad.net"
                                portString = "1080"
                                type = .socks5
                                if label.isEmpty { label = "Mullvad" }
                            }.font(.system(size: 11)).buttonStyle(.bordered)
                        }
                    }

                    HStack(spacing: 12) {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(L("Benutzername (optional)", "Username (optional)")).font(.caption).foregroundStyle(.secondary)
                            TextField(L("Benutzername", "Username"), text: $username)
                        }
                        VStack(alignment: .leading, spacing: 6) {
                            Text(L("Passwort (optional)", "Password (optional)")).font(.caption).foregroundStyle(.secondary)
                            SecureField(L("Passwort", "Password"), text: $password)
                        }
                    }

                    VStack(alignment: .leading, spacing: 6) {
                        Text(L("Ausgenommene Domains (durch Komma getrennt)", "Excluded domains (comma separated)")).font(.caption).foregroundStyle(.secondary)
                        TextField("localhost, 127.0.0.1, [::1]", text: $excludedDomainsString)
                    }

                    if let errorMessage {
                        Text(errorMessage)
                            .font(.caption)
                            .foregroundStyle(.red)
                    }

                    Text(L("NordVPN veröffentlicht für SOCKS5 derzeit nur Niederlande, Schweden und USA. Für Norwegen und andere Länder verwende die NordVPN-Mac-App; YoBro übernimmt das systemweite VPN automatisch. Proxy-Passwörter liegen im macOS-Schlüsselbund.", "NordVPN currently publishes SOCKS5 endpoints only for the Netherlands, Sweden, and the US. For Norway and other countries, use the NordVPN Mac app; YoBro automatically uses the system-wide VPN. Proxy passwords are stored in the macOS Keychain."))
                        .font(.system(size: 11))
                        .foregroundStyle(.secondary)
                }
            }

            HStack {
                if model.proxies.config(for: spaceName) != nil {
                    Button(L("Proxy entfernen", "Remove proxy"), role: .destructive) {
                        do {
                            try model.proxies.set(nil, for: spaceName)
                            if model.space == spaceName { model.updateActiveProxy() }
                            dismiss()
                        } catch { errorMessage = error.localizedDescription }
                    }.buttonStyle(.bordered)
                }

                Spacer()

                Button(L("Abbrechen", "Cancel")) {
                    dismiss()
                }.buttonStyle(.bordered)

                Button(isTesting ? L("Verbindung wird geprüft …", "Testing connection …") : L("Speichern & Anwenden", "Save & Apply")) {
                    saveAndApply()
                }.buttonStyle(.borderedProminent).tint(moss).disabled(isTesting)
            }
        }
        .padding(20)
        .frame(width: 540, height: 500)
        .background(paper)
        .foregroundStyle(ink)
        .textFieldStyle(YOBROTextFieldStyle()).tint(moss)
        .onAppear {
            loadExisting()
        }
    }

    private func loadExisting() {
        if let config = model.proxies.config(for: spaceName) {
            enabled = config.enabled
            label = config.label
            type = config.type
            host = config.host
            portString = String(config.port)
            username = config.username
            password = (try? model.proxies.resolvedConfig(config, for: spaceName).password) ?? ""
            excludedDomainsString = config.excludedDomains.joined(separator: ", ")
        }
    }

    private func saveAndApply() {
        let portInt = Int(portString.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
        let excluded = excludedDomainsString.components(separatedBy: ",")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }

        let config = SpaceProxyConfig(
            enabled: enabled,
            label: label,
            type: type,
            host: host,
            port: portInt,
            username: username,
            password: password,
            matchDomains: [],
            excludedDomains: excluded
        )

        errorMessage = nil
        isTesting = true
        Task { @MainActor in
            defer { isTesting = false }
            do {
                if enabled { try await model.proxies.testConnection(config, for: spaceName) }
                try model.proxies.set(config, for: spaceName)
                if model.space == spaceName { model.updateActiveProxy() }
                dismiss()
            } catch {
                errorMessage = L("Proxy-Verbindung fehlgeschlagen: \(error.localizedDescription)", "Proxy connection failed: \(error.localizedDescription)")
            }
        }
    }
}

struct LegacySpaceProxySettingsView: View {
    @ObservedObject var model: BrowserModel
    @State private var configuringSpace: String?
    @State private var changingSpaces: Set<String> = []

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                YOBROSettingsHeading(icon: "shield.lefthalf.filled", title: L("Space-spezifische Proxies & VPN", "Space-specific Proxies & VPN"), detail: L("Jeder Space kann eine eigene isolierte Proxy-Verbindung nutzen. Ein systemweites VPN wird automatisch übernommen.", "Each Space can use its own isolated proxy connection. A system-wide VPN is used automatically."))

                LazyVStack(alignment: .leading, spacing: 12) {
                    ForEach(model.spaces, id: \.self) { space in
                        let config = model.proxies.config(for: space)
                        let isConfigured = config != nil
                        let isEnabled = config?.enabled == true

                        HStack(spacing: 12) {
                            Image(systemName: isEnabled ? "shield.lefthalf.filled" : "shield.slash")
                                .font(.system(size: 16))
                                .foregroundStyle(isEnabled ? moss : .secondary)

                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 6) {
                                    Text(space).font(.system(size: 13, weight: .semibold))
                                    if model.space == space {
                                        Text(L("Aktiver Space", "Active Space"))
                                            .font(.system(size: 9, weight: .bold))
                                            .padding(.horizontal, 5).padding(.vertical, 1)
                                            .background(moss.opacity(0.15), in: Capsule())
                                            .foregroundStyle(moss)
                                    }
                                }
                                if let config, isConfigured {
                                    Text("\(config.displayLabel) · \(config.type.rawValue)")
                                        .font(.caption).foregroundStyle(.secondary)
                                } else {
                                    Text(L("Kein Proxy eingerichtet", "No proxy configured"))
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }

                            Spacer()

                            if let config, isConfigured {
                                Toggle("", isOn: Binding(
                                    get: { config.enabled },
                                    set: { val in
                                        changeProxy(config, enabled: val, for: space)
                                    }
                                )).toggleStyle(.switch).labelsHidden().disabled(changingSpaces.contains(space))
                            }

                            Button(isConfigured ? L("Bearbeiten", "Edit") : L("Einrichten", "Configure")) {
                                configuringSpace = space
                            }.buttonStyle(.bordered).font(.system(size: 11))
                        }
                        .yobroCard(padding: 14)
                    }
                }

                Divider()

                if let error = model.proxies.activeError {
                    Text(error).font(.caption).foregroundStyle(.red)
                }

                VStack(alignment: .leading, spacing: 8) {
                    Text(L("Häufige Fragen zu VPN & Proxies in YoBro", "FAQ on VPN & Proxies in YoBro")).font(.subheadline.bold())
                    Label(L("NordVPN und Mullvad unterstützen SOCKS5 mit Zugangsdaten aus deinem Anbieter-Dashboard.", "NordVPN and Mullvad support SOCKS5 using credentials from your provider dashboard."), systemImage: "key.horizontal")
                    Label(L("Ein systemweites VPN schützt YoBro automatisch – ohne zusätzlichen Proxy-Eintrag.", "A system-wide VPN protects YoBro automatically, without an additional proxy entry."), systemImage: "network.badge.shield.half.filled")
                }.font(.system(size: 11)).foregroundStyle(.secondary).yobroCard(padding: 13)
            }
        }
        .sheet(item: Binding(
            get: { configuringSpace.map { IdentifiableSpace(name: $0) } },
            set: { configuringSpace = $0?.name }
        )) { item in
            SpaceProxySheet(model: model, space: item.name)
        }
    }

    private func changeProxy(_ config: SpaceProxyConfig, enabled: Bool, for space: String) {
        changingSpaces.insert(space)
        Task { @MainActor in
            defer { changingSpaces.remove(space) }
            do {
                var updated = config
                updated.enabled = enabled
                if enabled { try await model.proxies.testConnection(updated, for: space) }
                try model.proxies.set(updated, for: space)
                if model.space == space { model.updateActiveProxy() }
            } catch {
                model.proxies.activeError = L("Proxy-Verbindung fehlgeschlagen: \(error.localizedDescription)", "Proxy connection failed: \(error.localizedDescription)")
            }
        }
    }
}

private struct IdentifiableSpace: Identifiable {
    let name: String
    var id: String { name }
}

struct SpaceProxySettingsView: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject private var vpn: ManagedVPNStore

    init(model: BrowserModel) {
        self.model = model
        vpn = model.managedVPN
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                YOBROSettingsHeading(
                    icon: "network.badge.shield.half.filled",
                    title: L("Privater Standort", "Private location"),
                    detail: L("Wähle einen Ort. YoBro verbindet sich verschlüsselt – ohne Serveradressen, Ports oder Passwörter.", "Choose a location. YoBro connects securely without server addresses, ports, or passwords.")
                )

                VStack(spacing: 10) {
                    locationRow(nil)
                    ForEach(vpn.locations) { location in locationRow(location) }
                }

                statusCard

                Text(L("Nur der Webverkehr in YoBro nutzt diesen Standort. Andere Apps auf deinem Mac bleiben unverändert.", "Only web traffic in YoBro uses this location. Other apps on your Mac are unchanged."))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func locationRow(_ location: ManagedVPNLocation?) -> some View {
        let selected = location?.id == vpn.selectedLocationID || (location == nil && vpn.selectedLocationID == nil)
        Button {
            if let location {
                Task {
                    do { try await vpn.connect(location, to: model.websiteDataStore); model.active?.webView.reload() }
                    catch { model.notice = error.localizedDescription }
                }
            } else {
                vpn.disconnect(from: model.websiteDataStore)
                model.updateActiveProxy()
                model.active?.webView.reload()
            }
        } label: {
            HStack(spacing: 14) {
                Text(location?.flag ?? "○").font(.system(size: location == nil ? 22 : 28)).frame(width: 38)
                VStack(alignment: .leading, spacing: 3) {
                    Text(location?.title ?? L("Aus", "Off")).font(.system(size: 14, weight: .semibold))
                    Text(location?.subtitle ?? L("Normale Verbindung", "Normal connection")).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if selected { Image(systemName: "checkmark.circle.fill").font(.system(size: 19)).foregroundStyle(moss) }
                else { Image(systemName: "circle").font(.system(size: 19)).foregroundStyle(.secondary.opacity(0.5)) }
            }
            .padding(.horizontal, 16).frame(height: 66).contentShape(Rectangle())
            .background(selected ? moss.opacity(0.1) : YOBROTheme.surface.opacity(0.5), in: RoundedRectangle(cornerRadius: 13))
            .overlay(RoundedRectangle(cornerRadius: 13).stroke(selected ? moss.opacity(0.45) : ink.opacity(0.06)))
        }
        .buttonStyle(.plain)
        .disabled(vpn.status == .connecting)
    }

    @ViewBuilder
    private var statusCard: some View {
        switch vpn.status {
        case .disconnected:
            Label(L("VPN ist ausgeschaltet", "VPN is off"), systemImage: "shield.slash")
                .foregroundStyle(.secondary).yobroCard(padding: 14)
        case .connecting:
            HStack(spacing: 10) { ProgressView().controlSize(.small); Text(L("Sichere Verbindung wird aufgebaut …", "Establishing secure connection …")) }
                .foregroundStyle(moss).yobroCard(padding: 14)
        case .connected(let location):
            Label(L("Verbunden mit \(location.city) · kanadische IP bestätigt", "Connected to \(location.city) · Canadian IP verified"), systemImage: "checkmark.shield.fill")
                .foregroundStyle(moss).yobroCard(padding: 14)
        case .failed(let message):
            Label(message, systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.red).yobroCard(padding: 14)
        }
    }
}
