import Foundation
import CryptoKit
import SwiftUI
import UIKit
import VisionKit

/// Preserves unknown desktop fields. Mobile only appends its own records; it never
/// replaces a desktop tab or propagates a deletion into the desktop workspace.
enum MobileSyncCodec {
    struct Envelope: Codable {
        var algorithm = "ChaChaPoly"
        var formatVersion = 1
        var ciphertext: Data
    }
    static func key(from text: String) throws -> Data {
        let code = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let base64 = code.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/") + String(repeating: "=", count: (4 - code.count % 4) % 4)
        guard let data = Data(base64Encoded: base64), data.count == 32 else { throw YOBROError.message("Der Wiederherstellungscode ist ungültig.") }
        return data
    }
    static func open(_ payload: String, key: Data, profileID: UUID) throws -> [String: Any] {
        guard key.count == 32, let data = Data(base64Encoded: payload) else { throw YOBROError.message("Ungültige Sync-Daten.") }
        let envelope = try JSONDecoder().decode(Envelope.self, from: data)
        guard envelope.algorithm == "ChaChaPoly", envelope.formatVersion == 1 else { throw YOBROError.message("Nicht unterstützte Sync-Version.") }
        let box = try ChaChaPoly.SealedBox(combined: envelope.ciphertext)
        let decoded = try ChaChaPoly.open(box, using: SymmetricKey(data: key))
        guard let snapshot = try JSONSerialization.jsonObject(with: decoded) as? [String: Any],
              snapshot["version"] as? Int == 1,
              (snapshot["profileID"] as? String).flatMap(UUID.init(uuidString:)) == profileID,
              snapshot["tabs"] is [[String: Any]] else { throw YOBROError.message("Das Sync-Profil passt nicht zu den verschlüsselten Daten.") }
        return snapshot
    }
    static func seal(_ snapshot: [String: Any], key: Data) throws -> String {
        guard key.count == 32 else { throw YOBROError.message("Ungültiger Sync-Schlüssel.") }
        let data = try JSONSerialization.data(withJSONObject: snapshot, options: [.sortedKeys])
        let box = try ChaChaPoly.seal(data, using: SymmetricKey(data: key))
        return try JSONEncoder().encode(Envelope(ciphertext: box.combined)).base64EncodedString()
    }
    static func merged(_ remote: [String: Any], local: MobileSnapshot, now: Date = Date()) -> [String: Any] {
        var snapshot = remote
        var tabs = remote["tabs"] as? [[String: Any]] ?? []
        let existing = Set(tabs.compactMap { ($0["id"] as? String)?.lowercased() })
        // A separate space makes mobile additions discoverable without switching the Mac's active space.
        for tab in local.tabs where tab.sourceID == nil && !existing.contains(tab.id.uuidString.lowercased()) {
            guard let url = URL(string: tab.url), ["http", "https"].contains(url.scheme ?? ""), url.host != nil else { continue }
            var record: [String: Any] = ["id": tab.id.uuidString, "title": tab.title, "url": tab.url, "space": tab.space ?? "Mobile", "pinned": false, "kind": "web"]
            if let folderID = tab.folderID { record["folderID"] = folderID.uuidString }
            tabs.append(record)
        }
        for note in local.notes where note.sourceID == nil && !existing.contains(note.id.uuidString.lowercased()) {
            tabs.append(["id": note.id.uuidString, "title": note.title, "url": "", "space": "Mobile", "pinned": false, "kind": "note", "noteContent": note.text])
        }
        snapshot["tabs"] = tabs
        var spaces = remote["spaces"] as? [String] ?? []
        if !spaces.contains("Mobile") { spaces.append("Mobile") }; snapshot["spaces"] = spaces
        var bookmarks = remote["bookmarks"] as? [[String: Any]] ?? []
        let urls = Set(bookmarks.compactMap { $0["url"] as? String })
        for entry in local.bookmarks ?? [] where !urls.contains(entry.url) {
            bookmarks.append(["id": entry.id.uuidString, "title": entry.title, "url": entry.url, "folder": "Mobile"])
        }
        snapshot["bookmarks"] = bookmarks
        snapshot["modifiedAt"] = now.timeIntervalSince1970 * 1000
        return snapshot
    }
    static func importing(_ remote: [String: Any], into local: MobileSnapshot) -> MobileSnapshot {
        var state = local
        var remoteSpaces = remote["spaces"] as? [String] ?? []
        // Tabs created by older mobile builds had no organizational metadata.
        // Keep them reachable in their own space when a newer sync arrives.
        if state.tabs.contains(where: { $0.space == nil }) {
            for index in state.tabs.indices where state.tabs[index].space == nil { state.tabs[index].space = "Mobile" }
            if !remoteSpaces.contains("Mobile") { remoteSpaces.append("Mobile") }
        }
        state.spaces = remoteSpaces
        if let current = remote["currentSpace"] as? String, remoteSpaces.contains(current), !remoteSpaces.contains(state.selectedSpace ?? "") {
            state.selectedSpace = current
        }
        let folders = (remote["folders"] as? [[String: Any]] ?? []).compactMap { record -> MobileFolder? in
            guard let idText = record["id"] as? String, let id = UUID(uuidString: idText),
                  let name = record["name"] as? String, let space = record["space"] as? String else { return nil }
            return MobileFolder(id: id, name: name, space: space, collapsed: record["collapsed"] as? Bool ?? false, color: record["color"] as? String)
        }
        state.folders = folders
        var known = Set(state.tabs.flatMap { [$0.id, $0.sourceID].compactMap { $0 } } + state.notes.flatMap { [$0.id, $0.sourceID].compactMap { $0 } })
        for record in remote["tabs"] as? [[String: Any]] ?? [] {
            guard let text = record["id"] as? String, let id = UUID(uuidString: text) else { continue }
            let title = record["customTitle"] as? String ?? record["title"] as? String ?? "Vom Desktop"
            let space = record["space"] as? String
            let folderID = (record["folderID"] as? String).flatMap(UUID.init(uuidString:))
            if let index = state.tabs.firstIndex(where: { $0.sourceID == id }) {
                state.tabs[index].title = title
                state.tabs[index].space = space
                state.tabs[index].folderID = folderID
                continue
            }
            guard !known.contains(id) else { continue }
            if record["kind"] as? String == "note" {
                state.notes.append(MobileNote(title: title, text: record["noteContent"] as? String ?? "", sourceID: id))
            } else if let value = record["url"] as? String, let url = URL(string: value), ["http", "https"].contains(url.scheme ?? ""), url.host != nil {
                state.tabs.append(MobileTab(title: title, url: value, sourceID: id,
                                            space: space, folderID: folderID))
            }
            known.insert(id)
        }
        var bookmarks = state.bookmarks ?? []
        var knownURLs = Set(bookmarks.map(\.url))
        for entry in remote["bookmarks"] as? [[String: Any]] ?? [] {
            guard let value = entry["url"] as? String, let url = URL(string: value), ["http", "https"].contains(url.scheme ?? ""), !knownURLs.contains(value) else { continue }
            bookmarks.append(MobileLink(title: entry["title"] as? String ?? value, url: value)); knownURLs.insert(value)
        }
        state.bookmarks = bookmarks
        return state
    }
}

struct MobileCloudProfile: Decodable, Identifiable {
    let profile_id: UUID
    var id: UUID { profile_id }
}
@MainActor final class MobileSync: ObservableObject {
    @Published var profiles: [MobileCloudProfile] = []
    @Published var selected: UUID?
    @Published var recoveryCode = ""
    @Published var status = "Noch nicht synchronisiert"
    @Published var busy = false
    private let account: MobileAccount
    private let browser: MobileBrowser
    init(account: MobileAccount, browser: MobileBrowser) { self.account = account; self.browser = browser }
    private struct Row: Decodable { let user_id: UUID; let profile_id: UUID; let revision: Int64; let payload: String }
    func loadProfiles() async {
        guard !busy else { return }; busy = true; defer { busy = false }
        do {
            let session = try await account.validSession()
            let data = try await request(path: "rest/v1/browser_sync", session: session, query: [URLQueryItem(name: "select", value: "profile_id"), URLQueryItem(name: "user_id", value: "eq.\(session.userID.uuidString)")])
            profiles = try JSONDecoder().decode([MobileCloudProfile].self, from: data)
            if !profiles.contains(where: { $0.id == selected }) { selected = profiles.first?.id }
            if profiles.isEmpty { status = "Bitte zuerst am Desktop Sync aktivieren. Es ist noch kein Cloud-Profil vorhanden." }
        } catch { status = error.localizedDescription }
    }
    func synchronize(upload: Bool) async {
        guard !busy, let profileID = selected else { return }
        busy = true; defer { busy = false }
        do {
            let session = try await account.validSession()
            guard session.userID == browser.userID else { throw YOBROError.message("Das Konto hat sich geändert.") }
            let keyAccount = "ios.sync.\(session.userID).\(profileID)"
            let key: Data
            if recoveryCode.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, let saved = BrowserSyncSecrets.read(account: keyAccount) { key = saved }
            else { key = try MobileSyncCodec.key(from: recoveryCode) }
            let data = try await request(path: "rest/v1/browser_sync", session: session, query: [URLQueryItem(name: "select", value: "user_id,profile_id,revision,payload"), URLQueryItem(name: "user_id", value: "eq.\(session.userID.uuidString)"), URLQueryItem(name: "profile_id", value: "eq.\(profileID.uuidString)")])
            guard let row = try JSONDecoder().decode([Row].self, from: data).first, row.user_id == session.userID, row.profile_id == profileID else { throw YOBROError.message("Cloud-Profil nicht gefunden.") }
            let remote = try MobileSyncCodec.open(row.payload, key: key, profileID: profileID)
            // Store only after successful authenticated decryption; never replace a good key with an unverified one.
            try BrowserSyncSecrets.store(key, account: keyAccount); recoveryCode = ""
            if upload {
                let merged = MobileSyncCodec.merged(remote, local: browser.state)
                let body: [String: Any] = ["p_profile_id": profileID.uuidString.lowercased(), "p_expected_revision": row.revision, "p_payload": try MobileSyncCodec.seal(merged, key: key)]
                _ = try await request(path: "rest/v1/rpc/push_browser_sync", session: session, body: body)
            }
            browser.state = MobileSyncCodec.importing(remote, into: browser.state); browser.save()
            status = upload ? "Neue Einträge zusammengeführt. Spaces, Ordner, Tabs, Notizen und Lesezeichen sind auf dem neuesten Stand." : "Desktop-Spaces, Ordner, Tabs, Notizen und Lesezeichen übernommen."
        } catch { status = "Sync fehlgeschlagen: " + error.localizedDescription }
    }
    private func request(path: String, session: SupabaseAuthSession, query: [URLQueryItem] = [], body: [String: Any]? = nil) async throws -> Data {
        var url = URLComponents(url: SupabaseAuthClient.projectURL.appendingPathComponent(path), resolvingAgainstBaseURL: false)!
        if !query.isEmpty { url.queryItems = query }
        var request = URLRequest(url: url.url!); request.timeoutInterval = 30
        request.setValue(SupabaseAuthClient.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("Bearer \(session.accessToken)", forHTTPHeaderField: "Authorization")
        if let body { request.httpMethod = "POST"; request.setValue("application/json", forHTTPHeaderField: "Content-Type"); request.httpBody = try JSONSerialization.data(withJSONObject: body) }
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw YOBROError.message("Cloud-Anfrage fehlgeschlagen (\((response as? HTTPURLResponse)?.statusCode ?? 0)). Bei gleichzeitigen Desktop-Änderungen bitte erneut synchronisieren.")
        }
        return data
    }
}
struct MobileSyncView: View {
    @StateObject private var sync: MobileSync
    init(account: MobileAccount, browser: MobileBrowser) { _sync = StateObject(wrappedValue: MobileSync(account: account, browser: browser)) }
    var body: some View {
        Form {
            Section("Desktop-Verbindung") {
                Text("Wähle dein Desktop-Profil. Den Wiederherstellungscode findest du am Mac unter Einstellungen → Sync. Er wird einmal benötigt und danach im Schlüsselbund gespeichert.").font(.callout)
                Picker("Profil", selection: $sync.selected) {
                    ForEach(sync.profiles) { profile in Text(profile.id.uuidString).tag(Optional(profile.id)) }
                }
                SecureField("Wiederherstellungscode", text: $sync.recoveryCode).textInputAutocapitalization(.never).autocorrectionDisabled()
                Button("Vom Desktop übernehmen") { Task { await sync.synchronize(upload: false) } }.disabled(sync.busy || sync.selected == nil)
                Button("Neue Einträge auch zum Desktop senden") { Task { await sync.synchronize(upload: true) } }.disabled(sync.busy || sync.selected == nil)
            }
            .listRowBackground(YOBROTheme.surface)
            Section {
                if sync.busy { ProgressView() }
                Text(sync.status)
                Text("Dieser Abgleich ergänzt neue Einträge. Änderungen und Löschungen bereits abgeglichener Einträge werden noch nicht übertragen. Website-Anmeldungen und VPN-Zugangsdaten bleiben auf dem jeweiligen Gerät.").font(.caption).foregroundStyle(.secondary)
            }
            .listRowBackground(YOBROTheme.surface)
        }.mobileSurface().navigationTitle("Desktop-Sync").task { await sync.loadProfiles() }
    }
}

/// Completes the one-time capability exchange started by the Mac QR code. The
/// account session is deliberately not part of the QR payload.
@MainActor final class MobilePairing: ObservableObject {
    @Published var code = ""
    @Published var status = ""
    @Published var busy = false
    func redeem(account: MobileAccount) async {
        guard !busy else { return }
        busy = true; defer { busy = false }
        do {
            let pairing = try DevicePairingCode.decode(code)
            let deviceSecret = DevicePairingCode.secret()
            var request = URLRequest(url: SupabaseAuthClient.projectURL.appendingPathComponent("rest/v1/rpc/redeem_browser_pairing"))
            request.httpMethod = "POST"; request.setValue(SupabaseAuthClient.publishableKey, forHTTPHeaderField: "apikey")
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: ["p_pairing_secret": pairing.bootstrapSecret, "p_device_hash": DevicePairingCode.hash(deviceSecret), "p_label": UIDevice.current.name, "p_platform": "ios"])
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode),
                  let text = String(data: data, encoding: .utf8)?.trimmingCharacters(in: CharacterSet(charactersIn: "\" \n")),
                  UUID(uuidString: text) == pairing.profileID else { throw YOBROError.message("Der QR-Code ist abgelaufen oder wurde bereits verwendet.") }
            try BrowserSyncSecrets.store(Data(deviceSecret.utf8), account: "ios.paired.device.\(pairing.profileID)")
            try BrowserSyncSecrets.store(Data(base64Encoded: pairing.syncKey)!, account: "ios.paired.key.\(pairing.profileID)")
            account.pair(profileID: pairing.profileID)
            status = "iPhone verbunden. Der erste Abgleich startet jetzt."
        } catch { status = error.localizedDescription }
    }
}

struct MobilePairingView: View {
    @EnvironmentObject private var account: MobileAccount
    @Environment(\.dismiss) private var dismiss
    @StateObject private var pairing = MobilePairing()
    @State private var scannerPresented = false
    var body: some View {
        Form {
            Section("Mit deinem Mac verbinden") {
                Text("Öffne auf dem Mac YoBro → Einstellungen → Sync → iPhone per QR verbinden.")
                Button("QR-Code scannen", systemImage: "qrcode.viewfinder") { scannerPresented = true }
                    .buttonStyle(.borderedProminent)
                TextField("QR-Code einfügen", text: $pairing.code, axis: .vertical).textInputAutocapitalization(.never).autocorrectionDisabled()
                Button("Code aus Zwischenablage einsetzen") { pairing.code = UIPasteboard.general.string ?? "" }
                Button("iPhone verbinden") { Task { await pairing.redeem(account: account); if account.isPairedDevice { dismiss() } } }
                    .buttonStyle(.borderedProminent).disabled(pairing.busy || pairing.code.isEmpty)
                if pairing.busy { ProgressView() }
                if !pairing.status.isEmpty { Text(pairing.status).foregroundStyle(.secondary) }
            }
        }.mobileSurface().navigationTitle("Mac verbinden")
        .sheet(isPresented: $scannerPresented) {
            MobilePairingScanner { value in pairing.code = value; scannerPresented = false }
        }
    }
}

@available(iOS 16.0, *)
private struct MobilePairingScanner: UIViewControllerRepresentable {
    let found: (String) -> Void
    func makeCoordinator() -> Coordinator { Coordinator(found: found) }
    func makeUIViewController(context: Context) -> DataScannerViewController {
        let scanner = DataScannerViewController(recognizedDataTypes: [.barcode(symbologies: [.qr])], qualityLevel: .accurate, recognizesMultipleItems: false, isHighFrameRateTrackingEnabled: true, isHighlightingEnabled: true)
        scanner.delegate = context.coordinator
        try? scanner.startScanning()
        return scanner
    }
    func updateUIViewController(_ uiViewController: DataScannerViewController, context: Context) {}
    final class Coordinator: NSObject, DataScannerViewControllerDelegate {
        let found: (String) -> Void
        init(found: @escaping (String) -> Void) { self.found = found }
        func dataScanner(_ dataScanner: DataScannerViewController, didTapOn item: RecognizedItem) {
            guard case .barcode(let code) = item, let value = code.payloadStringValue, value.hasPrefix("yobro-pair:") else { return }
            dataScanner.stopScanning(); found(value)
        }
        func dataScanner(_ dataScanner: DataScannerViewController, didAdd addedItems: [RecognizedItem], allItems: [RecognizedItem]) {
            guard let item = addedItems.first, case .barcode(let code) = item, let value = code.payloadStringValue, value.hasPrefix("yobro-pair:") else { return }
            dataScanner.stopScanning(); found(value)
        }
    }
}
