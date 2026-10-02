import XCTest
@testable import YOBRO

final class BroSpeechTextTests: XCTestCase {
    func testMarkdownMailAnswerBecomesNaturalSpeech() {
        let spoken = BroSpeechText.prepare("**Absender:** Max\n- *Hallo* Laura\n[Nachricht öffnen](https://example.com)")
        XCTAssertEqual(spoken, "Absender: Max\nHallo Laura\nNachricht öffnen")
        XCTAssertFalse(spoken.contains("*"))
    }

    func testLongLaughterIsShortenedAndIsolatedFromAnswer() {
        let spoken = BroSpeechText.prepare("Hahahahahahaha 😂😂😂 Ich antworte dir gleich.")
        XCTAssertEqual(BroSpeechText.utterances(in: spoken), ["Haha.", "Ich antworte dir gleich."])
        XCTAssertEqual(BroSpeechText.laughAudioLimit, 4)
    }

    func testEmailAddressRemainsAvailableForLocalSpeechProtection() {
        let spoken = BroSpeechText.prepare("**An:** laura@example.com")
        XCTAssertEqual(BroPrivacySession().segments(in: spoken).filter(\.sensitive).map(\.text), ["laura@example.com"])
    }
}
