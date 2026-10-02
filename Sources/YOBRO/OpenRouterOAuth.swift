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
    let supportedParameters: [String]?

    enum CodingKeys: String, CodingKey {
        case id, name, pricing
        case supportedParameters = "supported_parameters"
    }

    var supportsTools: Bool { supportedParameters?.contains("tools") == true }

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

    static func callbackCode(from request: String) -> String? {
        guard let line = request.components(separatedBy: "\r\n").first else { return nil }
        let fields = line.split(separator: " ")
        guard fields.count == 3, fields[0] == "GET", fields[2] == "HTTP/1.1",
              let components = URLComponents(string: "http://127.0.0.1\(fields[1])"),
              components.path == "/openrouter/callback",
              let codes = components.queryItems?.filter({ $0.name == "code" }),
              codes.count == 1, let code = codes[0].value,
              !code.isEmpty, code.utf8.count <= 2048 else { return nil }
        return code
    }

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
    private var completed = false

    init() throws {
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
        listener = try NWListener(using: parameters, on: .any)
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
        readRequestLine(connection, data: Data())
    }

    private func readRequestLine(_ connection: NWConnection, data: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] chunk, _, complete, error in
            guard let self, error == nil else { connection.cancel(); return }
            var data = data
            if let chunk { data.append(chunk) }
            guard data.count <= 16_384 else { connection.cancel(); return }
            guard let lineEnd = data.range(of: Data("\r\n".utf8)) else {
                if complete { connection.cancel() }
                else { self.readRequestLine(connection, data: data) }
                return
            }
            guard let line = String(data: data[..<lineEnd.lowerBound], encoding: .utf8) else { connection.cancel(); return }
            let request = line + "\r\n"
            let code = OpenRouterOAuth.callbackCode(from: request)
            let html = code == nil ? "Invalid OpenRouter callback. You can close this tab." : "OpenRouter is connected to YoBro. You can close this tab."
            let body = Data(html.utf8)
            let status = code == nil ? "400 Bad Request" : "200 OK"
            let response = Data("HTTP/1.1 \(status)\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n".utf8) + body
            connection.send(content: response, completion: .contentProcessed { _ in connection.cancel() })
            guard let code, !self.completed else { return }
            self.completed = true
            self.listener.cancel()
            let result: Result<String, Error> = .success(code)
            if let continuation = self.continuation { continuation.resume(with: result) }
            else { self.received = result }
            self.continuation = nil
        }
    }
}
