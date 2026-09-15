import Foundation

struct SupabaseSyncConfiguration: Sendable {
    let projectURL: URL
    let publishableKey: String
    let userID: UUID
    let accessToken: String
}

/// Supabase REST adapter. Authentication is injected so account UI and token
/// storage stay independent from the sync engine.
struct SupabaseSyncService: BrowserSyncService {
    let configuration: SupabaseSyncConfiguration
    var session: URLSession = .shared

    private struct Row: Codable {
        var user_id: UUID
        var profile_id: UUID
        var revision: Int64
        var payload: String
        var updated_at: Date
    }

    func fetch(profileID: UUID) async throws -> RemoteSyncRecord? {
        var components = URLComponents(url: configuration.projectURL.appendingPathComponent("rest/v1/browser_sync"), resolvingAgainstBaseURL: false)!
        components.queryItems = [
            URLQueryItem(name: "select", value: "user_id,profile_id,revision,payload,updated_at"),
            URLQueryItem(name: "profile_id", value: "eq.\(profileID.uuidString.lowercased())")
        ]
        var request = try authorizedRequest(url: components.url!)
        request.httpMethod = "GET"
        let (data, response) = try await session.data(for: request)
        try validate(response: response, data: data)
        guard let row = try decoder.decode([Row].self, from: data).first else { return nil }
        return try record(row)
    }

    func push(profileID: UUID, expectedRevision: Int64?, payload: EncryptedSyncPayload) async throws -> RemoteSyncRecord {
        var components = URLComponents(url: configuration.projectURL.appendingPathComponent("rest/v1/rpc/push_browser_sync"), resolvingAgainstBaseURL: false)!
        components.queryItems = []
        var request = try authorizedRequest(url: components.url!)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "p_profile_id": profileID.uuidString.lowercased(),
            "p_expected_revision": expectedRevision.map(NSNumber.init(value:)) ?? NSNull(),
            "p_payload": try JSONEncoder().encode(payload).base64EncodedString()
        ])
        let (data, response) = try await session.data(for: request)
        try validate(response: response, data: data)
        if let row = try? decoder.decode(Row.self, from: data) { return try record(row) }
        guard let row = try decoder.decode([Row].self, from: data).first else {
            throw YOBROError.message(L("Leere Sync-Antwort.", "Empty sync response."))
        }
        return try record(row)
    }

    private func authorizedRequest(url: URL) throws -> URLRequest {
        // The access token may only go to the configured project's own host.
        // Matching the exact host is tighter than the previous `.supabase.co`
        // suffix check and keeps working when the project is overridden.
        guard url.scheme == "https", let host = url.host?.lowercased(),
              let expected = configuration.projectURL.host?.lowercased(), host == expected else {
            throw YOBROError.message(L("Ungültige Supabase-Adresse.", "Invalid Supabase URL."))
        }
        var request = URLRequest(url: url)
        request.setValue(configuration.publishableKey, forHTTPHeaderField: "apikey")
        request.setValue("Bearer \(configuration.accessToken)", forHTTPHeaderField: "Authorization")
        return request
    }

    private var decoder: JSONDecoder {
        let value = JSONDecoder()
        value.dateDecodingStrategy = .custom { decoder in
            let text = try decoder.singleValueContainer().decode(String.self)
            let fractional = ISO8601DateFormatter()
            fractional.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            if let date = fractional.date(from: text) { return date }
            let standard = ISO8601DateFormatter()
            guard let date = standard.date(from: text) else {
                throw DecodingError.dataCorruptedError(in: try decoder.singleValueContainer(), debugDescription: "Invalid ISO-8601 timestamp")
            }
            return date
        }
        return value
    }

    private func validate(response: URLResponse, data: Data) throws {
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            let detail = (try? JSONSerialization.jsonObject(with: data) as? [String: Any]).flatMap { $0["message"] as? String ?? $0["hint"] as? String }
            throw YOBROError.message(detail ?? L("Sync-Serverfehler (\(status)).", "Sync server error (\(status))."))
        }
    }

    private func record(_ row: Row) throws -> RemoteSyncRecord {
        guard row.user_id == configuration.userID, let envelope = Data(base64Encoded: row.payload) else {
            throw YOBROError.message(L("Ungültige Sync-Antwort.", "Invalid sync response."))
        }
        return RemoteSyncRecord(profileID: row.profile_id, revision: row.revision, modifiedAt: row.updated_at,
                                payload: try JSONDecoder().decode(EncryptedSyncPayload.self, from: envelope))
    }
}
