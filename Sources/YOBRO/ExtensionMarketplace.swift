import SwiftUI

struct ChromeStore {
    static func identifier(_ input: String) -> String? {
        let value = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if value.range(of: "^[a-p]{32}$", options: .regularExpression) != nil { return value }
        guard let url = URL(string: value), url.scheme == "https", ["chromewebstore.google.com", "chrome.google.com"].contains(url.host ?? "") else { return nil }
        let parts = url.pathComponents
        guard parts.contains("detail") else { return nil }
        return parts.last(where: { $0.range(of: "^[a-p]{32}$", options: .regularExpression) != nil })
    }
    static func downloadURL(id: String, version: String) -> URL {
        var url = URLComponents(string: "https://clients2.google.com/service/update2/crx")!
        url.queryItems = [URLQueryItem(name: "response", value: "redirect"), URLQueryItem(name: "prodversion", value: version), URLQueryItem(name: "acceptformat", value: "crx2,crx3"), URLQueryItem(name: "x", value: "id=\(id)&installsource=ondemand&uc")]
        return url.url!
    }
    static func download(id: String) async throws -> URL {
        guard identifier(id) == id else { throw YOBROError.message(L("Ungültige Erweiterungs-ID.")) }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 30; configuration.timeoutIntervalForResource = 120
        let session = URLSession(configuration: configuration, delegate: StoreRedirects(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        var version = Bundle(url: URL(fileURLWithPath: "/Applications/Google Chrome.app"))?.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        if let url = URL(string: "https://versionhistory.googleapis.com/v1/chrome/platforms/mac_arm64/channels/stable/versions?pageSize=1"),
           let (data, _) = try? await session.data(from: url),
           let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
           let row = (value["versions"] as? [[String: Any]])?.first,
           let current = row["version"] as? String { version = current }
        guard let version else { throw YOBROError.message(L("Der Marktplatz ist gerade nicht erreichbar. Bitte später erneut versuchen.")) }
        let (stream, response) = try await session.bytes(from: downloadURL(id: id, version: version))
        guard let http = response as? HTTPURLResponse, http.statusCode == 200, response.url?.scheme == "https" else { throw YOBROError.message(L("Der Chrome Web Store stellt für diese Erweiterung kein herunterladbares Paket bereit.")) }
        let maximum = 100 * 1024 * 1024
        guard response.expectedContentLength <= maximum else { throw YOBROError.message(L("Das Erweiterungspaket ist größer als 100 MB.")) }
        var data = Data()
        for try await byte in stream {
            data.append(byte)
            if data.count > maximum { throw YOBROError.message(L("Das Erweiterungspaket ist größer als 100 MB.")) }
        }
        _ = try ExtensionPackage.zipPayload(data)
        let file = FileManager.default.temporaryDirectory.appendingPathComponent("yobro-store-\(UUID().uuidString).crx")
        try data.write(to: file, options: .atomic)
        return file
    }
    private final class StoreRedirects: NSObject, URLSessionTaskDelegate {
        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
            guard let url = request.url, url.scheme == "https", let host = url.host,
                  ["google.com", "googleusercontent.com", "gvt1.com", "gvt2.com", "googleapis.com"].contains(where: { host == $0 || host.hasSuffix("." + $0) }) else { completionHandler(nil); return }
            completionHandler(request)
        }
    }
}

extension ExtensionStore {
    func prepareFromStore(_ input: String) async {
        guard supported, !busy, pending == nil else { return }
        guard let id = ChromeStore.identifier(input) else { message = L("Bitte den Link einer Erweiterung aus dem Chrome Web Store eingeben."); return }
        if entries.contains(where: { $0.storeID == id }) { message = L("Diese Erweiterung ist bereits installiert."); return }
        busy = true; message = L("Paket wird vom Chrome Web Store geladen …")
        do {
            let file = try await ChromeStore.download(id: id)
            defer { try? FileManager.default.removeItem(at: file) }
            busy = false
            await prepare(file)
            if pending != nil { pending?.storeID = id; message = L("Vom Chrome Web Store geladen. Bitte die Berechtigungen vor der Installation prüfen.") }
        } catch { busy = false; message = error.localizedDescription }
    }
}

struct MarketplaceInstallButton: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var tab: BrowserTab
    @ObservedObject var store: ExtensionStore
    var body: some View {
        if ChromeStore.identifier(tab.url) != nil {
            Button {
                let address = tab.url
                model.openExtensionsHub()
                Task { await store.prepareFromStore(address) }
            } label: { Label(L("In YoBro installieren"), systemImage: "puzzlepiece.extension").font(.system(size: 11, weight: .medium)).padding(.horizontal, 8) }
                .buttonStyle(.borderedProminent).tint(moss).disabled(store.busy || !store.supported || store.pending != nil)
        }
    }
}

struct ExtensionSuggestions: View {
    @ObservedObject var store: ExtensionStore
    private let suggestions = [
        ("Dark Reader", "eimadpbcbfnmbkopoojfekhnkhdbieeh"),
        ("Bitwarden", "nngceckbapebfimnlniiiahkandclblb")
    ]
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(L("Weitere Erweiterungen", "More extensions")).font(.headline)
            ForEach(suggestions, id: \.1) { name, identifier in
                HStack {
                    Text(name)
                    Spacer()
                    if let entry = store.entries.first(where: { $0.storeID == identifier }) {
                        Toggle(L("Aktiv", "Enabled"), isOn: Binding(get: { entry.enabled }, set: { _ in Task { await store.toggle(entry) } }))
                            .toggleStyle(.switch)
                    } else {
                        Button(L("Laden", "Download")) { Task { await store.prepareFromStore(identifier) } }
                    }
                }
            }
            Text(L("Vor der Installation werden die Berechtigungen angezeigt. WebKit-Kompatibilität hängt von der jeweiligen Erweiterung ab. Dark Reader ist alternativ bereits in der Seitendarstellung integriert.", "Permissions are shown before installation. WebKit compatibility depends on each extension. Dark Reader is also already integrated in page appearance."))
                .font(.caption).foregroundStyle(.secondary)
        }
        .disabled(store.busy || !store.supported || store.pending != nil)
        .yobroCard(padding: 15)
    }
}
