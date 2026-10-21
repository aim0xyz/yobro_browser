import XCTest
@testable import YOBRO

final class BroPrivacyTests: XCTestCase {
    func testEmailAndLabeledPasswordStayLocalAndRestore() {
        let privacy = BroPrivacySession()
        let original = "Schreibe an laura@example.com. Mein Passwort: TestSecret123"
        let redacted = privacy.redact(original)
        XCTAssertFalse(redacted.contains("laura@example.com"))
        XCTAssertFalse(redacted.contains("TestSecret123"))
        XCTAssertEqual(privacy.restore(redacted), original)
    }

    func testKnownSecretIsHiddenEvenWithoutLabel() {
        let privacy = BroPrivacySession(knownSecrets: ["VaultPassword9"])
        let redacted = privacy.redact("Nutze VaultPassword9 für das Login.")
        XCTAssertFalse(redacted.contains("VaultPassword9"))
        XCTAssertEqual(privacy.restore(redacted), "Nutze VaultPassword9 für das Login.")
    }

    func testShortKnownPINIsHiddenWithoutMaskingLongerNumbers() {
        let privacy = BroPrivacySession(knownSecrets: ["1234", "42"])
        let redacted = privacy.redact("PIN 1234, Code 42, Referenz 1427")
        XCTAssertFalse(redacted.contains("1234"))
        XCTAssertFalse(redacted.contains("Code 42"))
        XCTAssertTrue(redacted.contains("1427"))
        XCTAssertEqual(privacy.restore(redacted), "PIN 1234, Code 42, Referenz 1427")
    }

    func testSpeechSegmentsKeepSensitiveTextSeparate() {
        let segments = BroPrivacySession().segments(in: "Sende es an team@example.com, bitte.")
        XCTAssertEqual(segments.filter(\.sensitive).map(\.text), ["team@example.com"])
        XCTAssertEqual(segments.map(\.text).joined(), "Sende es an team@example.com, bitte.")
    }

    func testPostalAddressIsRedacted() {
        let privacy = BroPrivacySession()
        let original = "Treffen bei Unter den Linden 1, 10117 Berlin."
        let redacted = privacy.redact(original)
        XCTAssertFalse(redacted.contains("Unter den Linden 1"))
        XCTAssertEqual(privacy.restore(redacted), original)
    }

    func testNestedToolArgumentsAreProtectedAndRestored() throws {
        let privacy = BroPrivacySession()
        let arguments = #"{"recipient":"team@example.com","body":"Mein Passwort: TestSecret123"}"#
        let message: [String: Any] = ["role": "assistant", "tool_calls": [["function": ["name": "draft_mail", "arguments": arguments]]]]
        let protected = try XCTUnwrap(privacy.redactJSON(message) as? [String: Any])
        let serialized = String(decoding: try JSONSerialization.data(withJSONObject: protected), as: UTF8.self)
        XCTAssertFalse(serialized.contains("team@example.com"))
        XCTAssertFalse(serialized.contains("TestSecret123"))
        XCTAssertEqual(privacy.restoreJSON(protected) as? [String: Any] != nil, true)
        let restored = try XCTUnwrap(privacy.restoreJSON(protected) as? [String: Any])
        let call = try XCTUnwrap((restored["tool_calls"] as? [[String: Any]])?.first)
        let function = try XCTUnwrap(call["function"] as? [String: Any])
        let restoredArguments = try XCTUnwrap(function["arguments"] as? String)
        let parsed = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(restoredArguments.utf8)) as? [String: String])
        XCTAssertEqual(parsed["recipient"], "team@example.com")
        XCTAssertEqual(parsed["body"], "Mein Passwort: TestSecret123")
    }

    func testShortStoredSecretsNeverReplaceProtocolFieldsOrWordParts() throws {
        let privacy = BroPrivacySession(knownSecrets: ["user", "tool", "test"])
        let messages: [[String: Any]] = [
            ["role": "user", "content": "Mein Login-Name ist user, nicht testing."],
            ["role": "assistant", "tool_calls": [["id": "call", "type": "function", "function": ["name": "read_tab", "arguments": #"{"tab":"test"}"#]]]],
            ["role": "tool", "tool_call_id": "call", "content": "ok"]
        ]
        let protected = privacy.redactMessages(messages)
        XCTAssertEqual(protected.map { $0["role"] as? String }, ["user", "assistant", "tool"])
        let content = try XCTUnwrap(protected[0]["content"] as? String)
        XCTAssertFalse(content.contains(" user,"))
        XCTAssertTrue(content.contains("testing"))
        let function = try XCTUnwrap(((protected[1]["tool_calls"] as? [[String: Any]])?.first)?["function"] as? [String: Any])
        XCTAssertEqual(function["name"] as? String, "read_tab")
        XCTAssertFalse((function["arguments"] as? String ?? "").contains("test"))
        XCTAssertEqual(protected[2]["tool_call_id"] as? String, "call")
    }

    func testRestoringJSONWithoutPlaceholdersKeepsItsBytes() {
        let privacy = BroPrivacySession()
        _ = privacy.redact("Schreibe an team@example.com")
        let pretty = "{\n  \"url\" : \"https://example.com/a/b\",\n  \"price\" : 1.50\n}"
        XCTAssertEqual(privacy.restoreJSON(pretty) as? String, pretty)
        XCTAssertEqual(privacy.redactJSON(pretty) as? String, pretty)
    }

    func testRestoredJSONKeepsSlashesUnescaped() throws {
        let privacy = BroPrivacySession()
        let marker = privacy.redact("team@example.com")
        let json = "{\"to\":\"\(marker)\",\"url\":\"https://example.com/a/b\"}"
        let restored = try XCTUnwrap(privacy.restoreJSON(json) as? String)
        XCTAssertTrue(restored.contains("https://example.com/a/b"))
        XCTAssertFalse(restored.contains(#"\/"#))
        let parsed = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(restored.utf8)) as? [String: String])
        XCTAssertEqual(parsed["to"], "team@example.com")
    }

    func testSecretAfterEscapedLineBreakOrSlashIsRedactedInJSON() throws {
        let privacy = BroPrivacySession(knownSecrets: ["Zebra-Secret-4417"])
        // Default serialization escapes "/" as well, like many page snapshots.
        let data = try JSONSerialization.data(withJSONObject: ["text": "Intro\nZebra-Secret-4417 here", "path": "a/Zebra-Secret-4417"])
        let redacted = try XCTUnwrap(privacy.redactJSON(String(decoding: data, as: UTF8.self)) as? String)
        XCTAssertFalse(redacted.contains("Zebra-Secret-4417"))
        XCTAssertNotNil(try JSONSerialization.jsonObject(with: Data(redacted.utf8)))
    }

    func testCardNumbersNeedAValidChecksum() {
        let privacy = BroPrivacySession()
        let card = privacy.redact("Karte 4111 1111 1111 1111 bitte")
        XCTAssertFalse(card.contains("4111 1111 1111 1111"))
        XCTAssertEqual(privacy.restore(card), "Karte 4111 1111 1111 1111 bitte")
        XCTAssertFalse(privacy.redact("Karte 4111111111111111").contains("4111111111111111"))
        XCTAssertEqual(privacy.redact("Nummer 1234 5678 9012 3456 bitte"), "Nummer 1234 5678 9012 3456 bitte")
        XCTAssertTrue(BroPrivacySession.passesLuhn("4111 1111 1111 1111"))
        XCTAssertFalse(BroPrivacySession.passesLuhn("1234 5678 9012 3456"))
    }
}
