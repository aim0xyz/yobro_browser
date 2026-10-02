import XCTest
@testable import YOBRO

final class VoiceWakeTests: XCTestCase {
    func testWakeWordCanCarryCommand() {
        XCTAssertEqual(VoiceWake.command(in: "Yo Bro, fasse diese Seite zusammen!"), "fasse diese Seite zusammen")
        XCTAssertEqual(VoiceWake.command(in: "YoBro, öffne Spotify"), "öffne Spotify")
        XCTAssertEqual(VoiceWake.command(in: "Yo Bro"), "")
        XCTAssertEqual(VoiceWake.command(in: "Jo Bro, starte einen Timer"), "starte einen Timer")
        XCTAssertEqual(VoiceWake.command(in: "Jobro, öffne Mail"), "öffne Mail")
    }

    func testOrdinaryConversationDoesNotTriggerWakeWord() {
        XCTAssertNil(VoiceWake.command(in: "Öffne meinen Posteingang"))
        XCTAssertNil(VoiceWake.command(in: "Ich habe gestern Yo Bro gesagt"))
        XCTAssertNil(VoiceWake.command(in: "Hey Jeff, öffne meinen Posteingang"))
        XCTAssertNil(VoiceWake.command(in: "Hey Jev"))
        XCTAssertEqual(VoiceWake.window, 60)
    }

    func testOnlyWakeWordPlusStopCancels() {
        XCTAssertTrue(VoiceWake.isStopCommand("Yo Bro Stopp"))
        XCTAssertTrue(VoiceWake.isStopCommand("YoBro, stop!"))
        XCTAssertFalse(VoiceWake.isStopCommand("Stopp"))
        XCTAssertFalse(VoiceWake.isStopCommand("Hey Jeff Stopp"))
        XCTAssertFalse(VoiceWake.isStopCommand("Yo Bro starte einen Timer"))
    }

    func testFollowUpWindowEndsExactlyOneMinuteAfterWake() {
        let wake = Date(timeIntervalSince1970: 1_000)
        let deadline = wake.addingTimeInterval(VoiceWake.window)
        XCTAssertFalse(VoiceWake.acceptsFollowUp(until: nil, at: wake))
        XCTAssertTrue(VoiceWake.acceptsFollowUp(until: deadline, at: wake.addingTimeInterval(59)))
        XCTAssertFalse(VoiceWake.acceptsFollowUp(until: deadline, at: deadline))
        XCTAssertFalse(VoiceWake.acceptsFollowUp(until: deadline, at: deadline.addingTimeInterval(1)))
    }

    func testWakeWordAloneKeepsRecognitionOpenForTheNextSentence() {
        XCTAssertEqual(VoiceWake.silenceTimeout(for: "Yo Bro", wakeRecognized: true), 4)
        XCTAssertEqual(VoiceWake.silenceTimeout(for: "Yo Bro, öffne Mail", wakeRecognized: true), 2.5)
        XCTAssertEqual(VoiceWake.silenceTimeout(for: "öffne Mail", wakeRecognized: false), 2.5)
    }

    func testLongOptionDictationKeepsEarlierSegmentsWhenFinalResultIsMissing() {
        XCTAssertEqual(
            VoiceDictation.combine(segments: [" Erster Abschnitt. ", "Zweiter Abschnitt."], final: ""),
            "Erster Abschnitt. Zweiter Abschnitt."
        )
        XCTAssertEqual(
            VoiceDictation.combine(segments: ["Erster Abschnitt."], final: "Letzter Abschnitt."),
            "Erster Abschnitt. Letzter Abschnitt."
        )
    }

    func testOptionDictationUsesPartialResultSoonerButWaitsForInitialRecognition() {
        XCTAssertEqual(VoiceDictation.finalizationTimeout(hasTranscript: true), 1.5)
        XCTAssertEqual(VoiceDictation.finalizationTimeout(hasTranscript: false), 3)
    }
}
