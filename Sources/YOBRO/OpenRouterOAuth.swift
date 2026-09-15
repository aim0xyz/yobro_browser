import Foundation
import AppKit
import CryptoKit
import Network
import Security

struct OpenRouterModel: Decodable, Identifiable, Hashable {
    struct Pricing: Decodable, Hashable {
        let prompt: String?
        let completion: String?
    }
    let id: String
    let name: String?
    let pricing: Pricing?

    var isFree: Bool {
        if id == "openrouter/free" || id.hasSuffix(":free") { return true }
        guard let pricing, let prompt = pricing.prompt, let completion = pricing.completion else { return false }
        return Double(prompt) == 0 && Double(completion) == 0
    }
}

enum OpenRouterOAuth {
    static let referer = "https://yobro.lol"
    static let title = "YoBro"
    static let keyLabel = "YoBro"

    static func base64URL(_ data: Data) -> String {
        data.base64EncodedString().replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
    }

    static func randomToken(bytes: Int = 32) throws -> String {
        var data = Data(count: bytes)
        let status = data.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, bytes, $0.baseAddress!) }
        guard status == errSecSuccess else { throw YOBROError.message(L("Sicherer Zufallswert konnte nicht erzeugt werden.", "Could not create a secure random value.")) }
        return base64URL(data)
    }

    static func connect() async throws -> String {
        let verifier = try randomToken()
        let challenge = base64URL(Data(SHA256.hash(data: Data(verifier.utf8))))
        let receiver = try OpenRouterCallbackReceiver()
        let callback = try await receiver.start()
        let authorizationURL = try authorizationURL(callback: callback, challenge: challenge)
        _ = await MainActor.run { NSWorkspace.shared.open(authorizationURL) }
        let code = try await receiver.waitForCode()
        var request = URLRequest(url: URL(string: "https://openrouter.ai/api/v1/auth/keys")!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "code": code, "code_verifier": verifier, "code_challenge_method": "S256"
        ])
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode),
              let json = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let key = json["key"] as? String, !key.isEmpty else {
            throw YOBROError.message(L("OpenRouter konnte keinen API-Schlüssel übergeben.", "OpenRouter did not return an API key."))
        }
        return key
    }

    static func authorizationURL(callback: URL, challenge: String) throws -> URL {
        var components = URLComponents(string: "https://openrouter.ai/auth")!
        components.queryItems = [
            URLQueryItem(name: "callback_url", value: callback.absoluteString),
            URLQueryItem(name: "code_challenge", value: challenge),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
            URLQueryItem(name: "key_label", value: keyLabel)
        ]
        guard let url = components.url else { throw YOBROError.message(L("OpenRouter-Anmeldeadresse ist ungültig.", "The OpenRouter sign-in URL is invalid.")) }
        return url
    }

    static func models(apiKey: String) async throws -> [OpenRouterModel] {
        var request = URLRequest(url: URL(string: "https://openrouter.ai/api/v1/models")!)
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue(referer, forHTTPHeaderField: "HTTP-Referer")
        request.setValue(title, forHTTPHeaderField: "X-OpenRouter-Title")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw YOBROError.message(L("OpenRouter-Modelle konnten nicht geladen werden.", "OpenRouter models could not be loaded."))
        }
        struct Response: Decodable { let data: [OpenRouterModel] }
        return try JSONDecoder().decode(Response.self, from: data).data.sorted {
            ($0.name ?? $0.id).localizedCaseInsensitiveCompare($1.name ?? $1.id) == .orderedAscending
        }
    }

    static func sortedModels(_ models: [OpenRouterModel], freeFirst: Bool) -> [OpenRouterModel] {
        models.sorted {
            if freeFirst, $0.isFree != $1.isFree { return $0.isFree }
            return ($0.name ?? $0.id).localizedCaseInsensitiveCompare($1.name ?? $1.id) == .orderedAscending
        }
    }
}

private final class OpenRouterCallbackReceiver: @unchecked Sendable {
    private let listener: NWListener
    private let queue = DispatchQueue(label: "yobro.openrouter.oauth")
    private var continuation: CheckedContinuation<String, Error>?
    private var received: Result<String, Error>?

    init() throws {
        listener = try NWListener(using: .tcp, on: .any)
    }

    func start() async throws -> URL {
        try await withCheckedThrowingContinuation { continuation in
            listener.stateUpdateHandler = { [weak self] status in
                guard let self else { return }
                switch status {
                case .ready:
                    guard let port = self.listener.port else { return }
                    continuation.resume(returning: URL(string: "http://127.0.0.1:\(port.rawValue)/openrouter/callback")!)
                case .failed(let error): continuation.resume(throwing: error)
                default: break
                }
            }
            listener.newConnectionHandler = { [weak self] in self?.receive($0) }
            listener.start(queue: queue)
        }
    }

    func waitForCode() async throws -> String {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                if let received = self.received { continuation.resume(with: received) }
                else { self.continuation = continuation }
            }
        }
    }

    private func receive(_ connection: NWConnection) {
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16_384) { [weak self] data, _, _, _ in
            guard let self, let data, let request = String(data: data, encoding: .utf8),
                  let line = request.components(separatedBy: "\r\n").first,
                  let target = line.split(separator: " ").dropFirst().first,
                  let components = URLComponents(string: "http://127.0.0.1\(target)") else { connection.cancel(); return }
            let values = Dictionary(uniqueKeysWithValues: (components.queryItems ?? []).map { ($0.name, $0.value ?? "") })
            let code = values["code"]
            let html = code == nil ? "OpenRouter connection failed. You can close this tab." : "OpenRouter is connected to YoBro. You can close this tab."
            let body = Data(html.utf8)
            let response = Data("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n".utf8) + body
            connection.send(content: response, completion: .contentProcessed { _ in connection.cancel() })
            self.listener.cancel()
            let result: Result<String, Error> = code.map(Result.success) ?? .failure(YOBROError.message(L("Ungültige OpenRouter-Antwort.", "Invalid OpenRouter response.")))
            if let continuation = self.continuation { continuation.resume(with: result) }
            else { self.received = result }
            self.continuation = nil
        }
    }
}
