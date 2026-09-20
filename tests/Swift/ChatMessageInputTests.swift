import XCTest
import AppKit
import SwiftUI
@testable import YOBRO

@MainActor
final class ChatMessageInputTests: XCTestCase {
    private func key(_ flags: NSEvent.ModifierFlags = [], keypad: Bool = false, repeatKey: Bool = false) throws -> NSEvent {
        try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
                                      windowNumber: 0, context: nil, characters: "\r", charactersIgnoringModifiers: "\r",
                                      isARepeat: repeatKey, keyCode: keypad ? 76 : 36))
    }

    func testShiftEnterInsertsAtCursorAndEnterSendsWithoutChangingDraft() throws {
        _ = NSApplication.shared
        let editor = ChatMessageTextView(frame: NSRect(x: 0, y: 0, width: 300, height: 100))
        editor.isRichText = false
        editor.string = "Hello world"
        editor.setSelectedRange(NSRange(location: 5, length: 0))
        var sends = 0
        editor.onSend = { sends += 1 }
        editor.keyDown(with: try key(.shift))
        XCTAssertEqual(editor.string, "Hello\n world")
        XCTAssertEqual(editor.selectedRange().location, 6)
        XCTAssertEqual(sends, 0)
        editor.keyDown(with: try key())
        XCTAssertEqual(sends, 1)
        XCTAssertEqual(editor.string, "Hello\n world")
        editor.keyDown(with: try key(repeatKey: true))
        XCTAssertEqual(sends, 1, "Holding Return must not repeatedly submit")
    }

    func testShiftKeypadEnterReplacesSelectionWithNewline() throws {
        _ = NSApplication.shared
        let editor = ChatMessageTextView(frame: NSRect(x: 0, y: 0, width: 300, height: 100))
        editor.isRichText = false
        editor.string = "One replace Two"
        editor.setSelectedRange(NSRange(location: 3, length: 9))
        editor.keyDown(with: try key(.shift, keypad: true))
        XCTAssertEqual(editor.string, "One\nTwo")
    }
    func testHostedComposerHasEditableAreaAndUpdatesBinding() async throws {
        _ = NSApplication.shared
        var draft = "Hello world"
        let root = ChatMessageInput(text: Binding(get: { draft }, set: { draft = $0 }), onSend: {})
            .frame(width: 300)
        let host = NSHostingView(rootView: root)
        host.frame = NSRect(x: 0, y: 0, width: 300, height: 100)
        let window = NSWindow(contentRect: NSRect(x: -2000, y: -2000, width: 300, height: 100), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        defer { window.close() }
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(100))
        host.layoutSubtreeIfNeeded()
        func findEditor(_ view: NSView) -> ChatMessageTextView? {
            if let editor = view as? ChatMessageTextView { return editor }
            return view.subviews.lazy.compactMap(findEditor).first
        }
        let editor = try XCTUnwrap(findEditor(host))
        XCTAssertGreaterThan(editor.bounds.width, 200)
        XCTAssertGreaterThanOrEqual(editor.bounds.height, 40)
        XCTAssertEqual(editor.string, draft)
        editor.setSelectedRange(NSRange(location: 5, length: 0))
        editor.keyDown(with: try key(.shift))
        XCTAssertEqual(draft, "Hello\n world")
    }

}
