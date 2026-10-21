import Foundation

struct BroTextSegment: Equatable {
    let text: String
    let sensitive: Bool
}

/// A per-conversation map. Originals stay in YoBro's memory; only generated
/// placeholders are sent to an external model.
final class BroPrivacySession {
    private var placeholders: [String: String] = [:]
    private var originals: [String: String] = [:]
    /// One expression for all stored secrets, built once per session. Every
    /// value needs word boundaries so that short passwords or usernames never
    /// match inside other words, IDs or protocol fields.
    private let knownSecrets: NSRegularExpression?
    private let nonce = UUID().uuidString.replacingOccurrences(of: "-", with: "")
    private static let email = try! NSRegularExpression(pattern: #"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b"#)
    private static let iban = try! NSRegularExpression(pattern: #"(?i)\b[A-Z]{2}\d{2}(?:[ \t]?[A-Z0-9]){11,30}\b"#)
    private static let postalAddress = try! NSRegularExpression(
        pattern: #"(?i)\b(?:[\p{L}.-]+\s+){1,6}\d{1,4}[a-z]?,?\s+\d{5}\s+[\p{L}.-]+(?:\s+[\p{L}.-]+){0,3}\b"#)
    private static let credential = try! NSRegularExpression(
        pattern: #"(?i)\b(?:passwort|password|kennwort|pin|api[-_ ]?key|token|secret|zugangscode|login|benutzername|username)\b\s*(?:ist|lautet|heißt|:|=)\s*[\"'“]?([^\s,;\"'”]{3,})"#)
    private static let cardNumber = try! NSRegularExpression(pattern: #"\b(?:\d[ -]?){12,18}\d\b"#)
    private static let detector = try? NSDataDetector(types:
        NSTextCheckingResult.CheckingType.address.rawValue |
        NSTextCheckingResult.CheckingType.phoneNumber.rawValue)

    init(knownSecrets: [String] = []) {
        let values = Set(knownSecrets.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { $0.count >= 2 })
        if values.isEmpty {
            self.knownSecrets = nil
        } else {
            // Longest first, so a secret containing another one is replaced whole.
            let alternatives = values.sorted { $0.count > $1.count }.map(NSRegularExpression.escapedPattern(for:))
            self.knownSecrets = try? NSRegularExpression(
                pattern: #"(?<![\p{L}\p{N}_])(?:"# + alternatives.joined(separator: "|") + #")(?![\p{L}\p{N}_])"#)
        }
    }

    /// Reading every stored password costs one Keychain call each. The chat
    /// needs this on every request, so keep the values briefly in memory.
    @MainActor private static var storedSecrets: [URL: (date: Date, values: [String])] = [:]

    @MainActor static func forBrowser(_ browser: BrowserModel) -> BroPrivacySession {
        let values: [String]
        if let cached = storedSecrets[browser.home], Date().timeIntervalSince(cached.date) < 120 {
            values = cached.values
        } else {
            let logins = (try? PasswordVault.entries(home: browser.home, skipUnreadable: true)) ?? []
            let stored = logins.map(\.password) + logins.map(\.username).filter { $0.count >= 4 }
            storedSecrets[browser.home] = (Date(), stored)
            values = stored
        }
        return BroPrivacySession(knownSecrets: values + [browser.chat.apiKey])
    }

    func segments(in text: String) -> [BroTextSegment] {
        let source = text as NSString
        let full = NSRange(location: 0, length: source.length)
        var ranges: [NSRange] = []
        for expression in [Self.email, Self.iban, Self.postalAddress] {
            ranges += expression.matches(in: text, range: full).map(\.range)
        }
        ranges += Self.credential.matches(in: text, range: full).compactMap { match in
            let value = match.range(at: 1)
            return value.location == NSNotFound ? nil : value
        }
        // Order and reference numbers look like cards too; only a valid
        // checksum marks a real one.
        ranges += Self.cardNumber.matches(in: text, range: full).map(\.range)
            .filter { Self.passesLuhn(source.substring(with: $0)) }
        // Phone numbers have at most 15 digits, so longer runs are left to
        // the card rule above.
        ranges += Self.detector?.matches(in: text, range: full).filter { match in
            match.resultType != .phoneNumber || source.substring(with: match.range).filter { $0.isASCII && $0.isWholeNumber }.count <= 15
        }.map(\.range) ?? []
        ranges += knownSecrets?.matches(in: text, range: full).map(\.range) ?? []
        let ordered = ranges.filter { $0.length > 0 }.sorted {
            $0.location == $1.location ? $0.length > $1.length : $0.location < $1.location
        }
        var merged: [NSRange] = []
        for range in ordered {
            if let last = merged.last, range.location <= NSMaxRange(last) {
                merged[merged.count - 1].length = max(NSMaxRange(last), NSMaxRange(range)) - last.location
            } else {
                merged.append(range)
            }
        }
        var segments: [BroTextSegment] = []
        var cursor = 0
        for range in merged {
            if range.location > cursor {
                segments.append(BroTextSegment(text: source.substring(with: NSRange(location: cursor, length: range.location - cursor)), sensitive: false))
            }
            segments.append(BroTextSegment(text: source.substring(with: range), sensitive: true))
            cursor = NSMaxRange(range)
        }
        if cursor < source.length {
            segments.append(BroTextSegment(text: source.substring(from: cursor), sensitive: false))
        }
        return segments.isEmpty ? [BroTextSegment(text: text, sensitive: false)] : segments
    }

    static func passesLuhn(_ candidate: String) -> Bool {
        let digits = candidate.compactMap { $0.isASCII ? $0.wholeNumberValue : nil }
        guard (13...19).contains(digits.count) else { return false }
        let sum = digits.reversed().enumerated().reduce(0) { total, item in
            let doubled = item.offset % 2 == 1 ? item.element * 2 : item.element
            return total + (doubled > 9 ? doubled - 9 : doubled)
        }
        return sum % 10 == 0
    }

    func redact(_ text: String) -> String {
        var changed = false
        return redact(text, changed: &changed)
    }

    private func redact(_ text: String, changed: inout Bool) -> String {
        segments(in: text).map { segment in
            guard segment.sensitive else { return segment.text }
            changed = true
            if let existing = placeholders[segment.text] { return existing }
            let marker = "[[YB_PRIVATE_\(nonce)_\(placeholders.count + 1)]]"
            placeholders[segment.text] = marker
            originals[marker] = segment.text
            return marker
        }.joined()
    }

    func restore(_ text: String) -> String {
        originals.reduce(text) { result, item in
            result.replacingOccurrences(of: item.key, with: item.value)
        }
    }

    func redactJSON(_ value: Any) -> Any {
        var changed = false
        return redactJSON(value, changed: &changed)
    }

    private func redactJSON(_ value: Any, changed: inout Bool) -> Any {
        if let text = value as? String {
            if let nested = transformEmbeddedJSON(text, restoring: false) {
                if nested != text { changed = true }
                return nested
            }
            return redact(text, changed: &changed)
        }
        if let array = value as? [Any] { return array.map { redactJSON($0, changed: &changed) } }
        if let object = value as? [String: Any] {
            return object.mapValues { redactJSON($0, changed: &changed) }
        }
        return value
    }

    /// Protects only what the model reads as text: message content and tool
    /// arguments. Roles, tool names and call IDs are protocol fields; replacing
    /// them would make the provider reject the request.
    func redactMessages(_ messages: [[String: Any]]) -> [[String: Any]] {
        messages.map { message in
            var protected = message
            if let content = message["content"] { protected["content"] = redactJSON(content) }
            if let calls = message["tool_calls"] as? [[String: Any]] {
                protected["tool_calls"] = calls.map { call -> [String: Any] in
                    guard var function = call["function"] as? [String: Any],
                          let arguments = function["arguments"] else { return call }
                    var protectedCall = call
                    function["arguments"] = redactJSON(arguments)
                    protectedCall["function"] = function
                    return protectedCall
                }
            }
            return protected
        }
    }

    func restoreJSON(_ value: Any) -> Any {
        if let text = value as? String {
            if let nested = transformEmbeddedJSON(text, restoring: true) { return nested }
            return restore(text)
        }
        if let array = value as? [Any] { return array.map(restoreJSON) }
        if let object = value as? [String: Any] {
            return object.mapValues { restoreJSON($0) }
        }
        return value
    }

    /// Re-serializes embedded JSON only when a value inside actually changes,
    /// so answers and page snapshots without private data keep their bytes.
    private func transformEmbeddedJSON(_ text: String, restoring: Bool) -> String? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix("{") || trimmed.hasPrefix("[") else { return nil }
        // Placeholders contain nothing JSON escapes, so a plain search is exact.
        if restoring, !originals.keys.contains(where: { text.contains($0) }) { return nil }
        guard let parsed = try? JSONSerialization.jsonObject(with: Data(text.utf8)) else { return nil }
        let transformed: Any
        if restoring {
            transformed = restoreJSON(parsed)
        } else {
            // Checked on the parsed strings: in the raw text an escaped "\n" or
            // "\/" sits right before a secret and hides it from the word-boundary
            // rules.
            var changed = false
            transformed = redactJSON(parsed, changed: &changed)
            if !changed { return text }
        }
        guard let data = try? JSONSerialization.data(withJSONObject: transformed, options: [.withoutEscapingSlashes]),
              let result = String(data: data, encoding: .utf8) else { return nil }
        return result
    }
}
