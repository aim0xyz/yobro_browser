import AppKit
import XCTest
@testable import YOBRO

@MainActor
final class MailTextColorTests: XCTestCase {
    func testMissingAndLegacyBlackColorsBecomeAutomatic() throws {
        let rich = NSMutableAttributedString(string: "ABC")
        rich.addAttribute(.foregroundColor, value: NSColor.black, range: NSRange(location: 1, length: 1))
        rich.addAttribute(.foregroundColor, value: NSColor(deviceWhite: 0, alpha: 1), range: NSRange(location: 2, length: 1))

        MailTextColor.applyAutomaticTextColor(to: rich)

        for index in 0..<rich.length {
            XCTAssertEqual(color(in: rich, at: index), NSColor.textColor)
        }
        let light = try XCTUnwrap(resolved(color(in: rich, at: 0), appearance: .aqua))
        let dark = try XCTUnwrap(resolved(color(in: rich, at: 0), appearance: .darkAqua))
        XCTAssertLessThan(light.redComponent, 0.25)
        XCTAssertGreaterThan(dark.redComponent, 0.75)
    }

    func testExplicitAccentsAndBackgroundContrastsArePreserved() {
        let rich = NSMutableAttributedString(string: "ABCD")
        rich.addAttribute(.foregroundColor, value: NSColor.systemRed, range: NSRange(location: 0, length: 1))
        rich.addAttributes([.foregroundColor: NSColor.white, .backgroundColor: NSColor.black], range: NSRange(location: 1, length: 1))
        rich.addAttributes([.foregroundColor: NSColor.black, .backgroundColor: NSColor.yellow], range: NSRange(location: 2, length: 1))
        rich.addAttribute(.backgroundColor, value: NSColor.yellow, range: NSRange(location: 3, length: 1))
        let original = NSAttributedString(attributedString: rich)

        MailTextColor.applyAutomaticTextColor(to: rich)

        XCTAssertEqual(rich, original)
    }

    func testTransparentBackgroundDoesNotPreventAutomaticText() {
        let rich = NSMutableAttributedString(string: "Neutral", attributes: [
            .foregroundColor: NSColor.black,
            .backgroundColor: NSColor.clear
        ])
        MailTextColor.applyAutomaticTextColor(to: rich)
        XCTAssertEqual(color(in: rich, at: 0), NSColor.textColor)
        XCTAssertEqual(rich.attribute(.backgroundColor, at: 0, effectiveRange: nil) as? NSColor, NSColor.clear)
    }

    func testParagraphsListsEmphasisAndLinksSurviveColorAdaptation() throws {
        let rich = formattedFixture()
        let original = NSAttributedString(attributedString: rich)
        let linkRange = range("Reference", in: rich)
        let emphasizedRange = range("Bold italic", in: rich)

        MailTextColor.applyAutomaticTextColor(to: rich)

        XCTAssertEqual(rich.string, original.string)
        for key: NSAttributedString.Key in [.font, .paragraphStyle, .underlineStyle, .link] {
            original.enumerateAttribute(key, in: fullRange(original)) { expected, range, _ in
                let actual = rich.attribute(key, at: range.location, effectiveRange: nil)
                XCTAssertEqual(actual as? NSObject, expected as? NSObject, "Changed \(key) at \(range)")
            }
        }
        XCTAssertEqual(color(in: rich, at: linkRange.location), NSColor.linkColor)
        let font = try XCTUnwrap(rich.attribute(.font, at: emphasizedRange.location, effectiveRange: nil) as? NSFont)
        XCTAssertTrue(NSFontManager.shared.traits(of: font).contains([.boldFontMask, .italicFontMask]))
        let style = try XCTUnwrap(rich.attribute(.paragraphStyle, at: emphasizedRange.location, effectiveRange: nil) as? NSParagraphStyle)
        XCTAssertEqual(style.textLists.count, 1)
        XCTAssertEqual(style.paragraphSpacing, 9)
        XCTAssertEqual(style.headIndent, 24)
    }

    func testTypingAttributesUseAutomaticColorAndKeepFormatting() {
        let font = NSFont.boldSystemFont(ofSize: 17)
        let original: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.black, .underlineStyle: 1]
        let adapted = MailTextColor.typingAttributes(original)
        XCTAssertEqual(adapted[.foregroundColor] as? NSColor, NSColor.textColor)
        XCTAssertEqual(adapted[.font] as? NSFont, font)
        XCTAssertEqual(adapted[.underlineStyle] as? Int, 1)
        XCTAssertEqual(MailTextColor.typingAttributes([:])[.foregroundColor] as? NSColor, NSColor.textColor)
        XCTAssertEqual(MailTextColor.typingAttributes([.foregroundColor: NSColor.systemRed])[.foregroundColor] as? NSColor, NSColor.systemRed)
        XCTAssertEqual(MailTextColor.typingAttributes([.foregroundColor: NSColor.white, .backgroundColor: NSColor.black])[.foregroundColor] as? NSColor, NSColor.white)
        let link: [NSAttributedString.Key: Any] = [.link: URL(string: "https://example.invalid")!]
        XCTAssertEqual(MailTextColor.typingAttributes(link)[.foregroundColor] as? NSColor, NSColor.linkColor)
    }

    func testExportRemovesAutomaticAndLegacyDefaultColorsWithoutChangingSource() throws {
        let rich = formattedFixture()
        MailTextColor.applyAutomaticTextColor(to: rich)
        let legacyRange = range("Plain", in: rich)
        rich.addAttribute(.foregroundColor, value: NSColor.black, range: legacyRange)
        let original = NSAttributedString(attributedString: rich)

        let exported = MailTextColor.contentForExport(rich)

        XCTAssertEqual(rich, original, "Export must not change the editor's dynamic display colors")
        XCTAssertEqual(exported.string, rich.string)
        XCTAssertNil(color(in: exported, at: legacyRange.location))
        XCTAssertNil(color(in: exported, at: range("Bold italic", in: rich).location))
        XCTAssertEqual(color(in: exported, at: range("Reference", in: rich).location), NSColor.linkColor)
        XCTAssertEqual(color(in: exported, at: range("Accent", in: rich).location), NSColor.systemRed)
        XCTAssertEqual(color(in: exported, at: range("Contrast", in: rich).location), NSColor.white)
        let emphasis = range("Bold italic", in: rich).location
        XCTAssertEqual(exported.attribute(.font, at: emphasis, effectiveRange: nil) as? NSFont, rich.attribute(.font, at: emphasis, effectiveRange: nil) as? NSFont)
        XCTAssertEqual(exported.attribute(.paragraphStyle, at: emphasis, effectiveRange: nil) as? NSParagraphStyle, rich.attribute(.paragraphStyle, at: emphasis, effectiveRange: nil) as? NSParagraphStyle)
    }

    func testNeutralHTMLDoesNotFreezeEitherAppearanceColor() throws {
        for appearance: NSAppearance.Name in [.aqua, .darkAqua] {
            let data = withAppearance(appearance) {
                let rich = NSMutableAttributedString(string: "Hello\n\nWorld", attributes: [.foregroundColor: NSColor.textColor, .font: NSFont.boldSystemFont(ofSize: 15)])
                let exported = MailTextColor.contentForExport(rich)
                return try? exported.data(from: fullRange(exported), documentAttributes: [.documentType: NSAttributedString.DocumentType.html])
            }
            let html = try XCTUnwrap(String(data: XCTUnwrap(data), encoding: .utf8))
            assertNoForegroundCSS(html)
            XCTAssertTrue(html.contains("Hello"))
            XCTAssertTrue(html.contains("World"))
        }
    }

    func testInstallingLegacyRTFDraftRestoresAutomaticTextAndEmphasis() throws {
        let rich = formattedFixture()
        rich.addAttribute(.foregroundColor, value: NSColor.black, range: range("Plain", in: rich))
        let data = try XCTUnwrap(rich.rtf(from: fullRange(rich), documentAttributes: [:]))
        let view = NSTextView(frame: .zero)
        let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: true) { _, _ in
            XCTFail("Loading a draft must not publish an edit")
        }

        coordinator.install(rtf: data, fallbackText: "Unused fallback", in: view)

        XCTAssertEqual(view.string, rich.string)
        XCTAssertEqual(color(in: view.attributedString(), at: 0), NSColor.textColor)
        XCTAssertEqual(color(in: view.attributedString(), at: range("Bold italic", in: rich).location), NSColor.textColor)
        XCTAssertEqual(coordinator.lastRTF, data)
        let font = try XCTUnwrap(view.attributedString().attribute(.font, at: range("Bold italic", in: rich).location, effectiveRange: nil) as? NSFont)
        XCTAssertTrue(NSFontManager.shared.traits(of: font).contains([.boldFontMask, .italicFontMask]))
    }

    func testFallbackDraftAndEmptyDraftGetAutomaticTypingColor() {
        for fallback in ["Recovered plain text", ""] {
            let view = NSTextView(frame: .zero)
            let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: true) { _, _ in }
            coordinator.install(rtf: Data("invalid RTF".utf8), fallbackText: fallback, in: view)
            XCTAssertEqual(view.string, fallback)
            XCTAssertEqual(view.typingAttributes[.foregroundColor] as? NSColor, NSColor.textColor)
            if !fallback.isEmpty {
                XCTAssertEqual(color(in: view.attributedString(), at: 0), NSColor.textColor)
            }
        }
    }

    func testTextChangePreservesCaretAndPublishesPortableDraftInDarkMode() throws {
        var saved: Data?
        var savedText = ""
        var changes = 0
        let view = NSTextView(frame: .zero)
        let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: true) { data, text in
            saved = data; savedText = text; changes += 1
        }
        let original = NSMutableAttributedString(string: "A pasted paragraph", attributes: [.font: NSFont.boldSystemFont(ofSize: 15)])
        view.textStorage?.setAttributedString(original)
        let caret = NSRange(location: 8, length: 0)
        view.setSelectedRange(caret)
        view.typingAttributes = [.font: NSFont.boldSystemFont(ofSize: 15), .foregroundColor: NSColor.black]

        withAppearance(.darkAqua) {
            coordinator.textDidChange(Notification(name: NSText.didChangeNotification, object: view))
        }

        XCTAssertEqual(changes, 1)
        XCTAssertEqual(savedText, original.string)
        XCTAssertEqual(view.selectedRange(), caret)
        XCTAssertEqual(color(in: view.attributedString(), at: 0), NSColor.textColor)
        XCTAssertEqual(view.typingAttributes[.foregroundColor] as? NSColor, NSColor.textColor)
        let data = try XCTUnwrap(saved)
        XCTAssertEqual(coordinator.lastRTF, data)
        let reopened = NSTextView(frame: .zero)
        RichTextEditor.Coordinator(adaptsEmailTextColor: true) { _, _ in }.install(rtf: data, fallbackText: "", in: reopened)
        XCTAssertEqual(color(in: reopened.attributedString(), at: 0), NSColor.textColor, "RTF saved in dark mode must reopen as automatic, not fixed white")
        let font = try XCTUnwrap(reopened.attributedString().attribute(.font, at: 0, effectiveRange: nil) as? NSFont)
        XCTAssertTrue(NSFontManager.shared.traits(of: font).contains(.boldFontMask))
        let restored = try XCTUnwrap(NSAttributedString(rtf: data, documentAttributes: nil))
        let exported = MailTextColor.contentForExport(restored)
        let htmlData = try exported.data(from: fullRange(exported), documentAttributes: [.documentType: NSAttributedString.DocumentType.html])
        assertNoForegroundCSS(try XCTUnwrap(String(data: htmlData, encoding: .utf8)))
    }

    func testNotesKeepTheirExistingExplicitColors() throws {
        let original = NSAttributedString(string: "Note", attributes: [.foregroundColor: NSColor.black])
        let data = try XCTUnwrap(original.rtf(from: fullRange(original), documentAttributes: [:]))
        let view = NSTextView(frame: .zero)
        let coordinator = RichTextEditor.Coordinator { _, _ in }
        coordinator.install(rtf: data, fallbackText: "", in: view)
        coordinator.textDidChange(Notification(name: NSText.didChangeNotification, object: view))
        let actual = try XCTUnwrap(resolved(color(in: view.attributedString(), at: 0), appearance: .darkAqua))
        XCTAssertLessThan(actual.redComponent, 0.01)
        XCTAssertNotEqual(color(in: view.attributedString(), at: 0), NSColor.textColor)
    }

    func testNativePasteImportsNeutralRTFAndHTMLThroughEditorDelegate() throws {
        let fixtures: [(NSPasteboard.PasteboardType, String)] = [
            (.rtf, #"{\rtf1\ansi\deff0{\fonttbl{\f0 Helvetica;}}\f0\fs30\b Bold paste\b0}"#),
            (.html, "<html><body><p><strong>Bold paste</strong></p></body></html>")
        ]
        for (type, fixture) in fixtures {
            // A unique named pasteboard exercises AppKit's native format importer
            // without reading or changing the user's general clipboard.
            let pasteboard = NSPasteboard(name: NSPasteboard.Name("YOBRO.MailTextColorTests.\(UUID().uuidString)"))
            defer { pasteboard.releaseGlobally() }
            pasteboard.declareTypes([type], owner: nil)
            XCTAssertTrue(pasteboard.setData(Data(fixture.utf8), forType: type))
            var saved: Data?
            var savedText = ""
            var changes = 0
            let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: true) { data, text in
                saved = data; savedText = text; changes += 1
            }
            let view = NSTextView(frame: .zero)
            view.isRichText = true
            view.allowsUndo = true
            view.delegate = coordinator
            coordinator.install(rtf: nil, fallbackText: "Existing ", in: view)
            view.setSelectedRange(NSRange(location: view.string.utf16.count, length: 0))

            withAppearance(.darkAqua) {
                XCTAssertTrue(view.readSelection(from: pasteboard), "AppKit rejected \(type.rawValue)")
                // readSelection is the lower-level importer. When it does not
                // emit a change notification, complete the edit via AppKit's
                // public didChangeText hook, as a paste action does. The real
                // delegate receives the notification; it is never called here.
                if changes == 0 { view.didChangeText() }
            }

            XCTAssertEqual(changes, 1)
            XCTAssertTrue(view.string.hasPrefix("Existing "))
            XCTAssertEqual(savedText, view.string)
            let inserted = range("Bold paste", in: view.attributedString())
            XCTAssertNotEqual(inserted.location, NSNotFound)
            guard inserted.location != NSNotFound else { continue }
            XCTAssertEqual(view.selectedRange(), NSRange(location: view.string.utf16.count, length: 0))
            XCTAssertEqual(color(in: view.attributedString(), at: 0), NSColor.textColor)
            XCTAssertEqual(color(in: view.attributedString(), at: inserted.location), NSColor.textColor)
            let font = try XCTUnwrap(view.attributedString().attribute(.font, at: inserted.location, effectiveRange: nil) as? NSFont)
            XCTAssertTrue(NSFontManager.shared.traits(of: font).contains(.boldFontMask))

            let data = try XCTUnwrap(saved)
            XCTAssertEqual(coordinator.lastRTF, data)
            let restored = try XCTUnwrap(NSAttributedString(rtf: data, documentAttributes: nil))
            XCTAssertEqual(restored.string, view.string)
            let exported = MailTextColor.contentForExport(restored)
            let htmlData = try exported.data(from: fullRange(exported), documentAttributes: [.documentType: NSAttributedString.DocumentType.html])
            assertNoForegroundCSS(try XCTUnwrap(String(data: htmlData, encoding: .utf8)))
            let reopened = NSTextView(frame: .zero)
            RichTextEditor.Coordinator(adaptsEmailTextColor: true) { _, _ in }.install(rtf: data, fallbackText: "", in: reopened)
            XCTAssertEqual(color(in: reopened.attributedString(), at: inserted.location), NSColor.textColor)
        }
    }

    func testSendUsesPortableHTMLForDraftSavedInDarkMode() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let account = MailAccount(label: "Test", address: "sender@example.invalid")
        var payload: [String: Any] = [:]
        let store = MailStore(home: home, request: { action, _, extra in
            XCTAssertEqual(action, "send")
            payload = extra
            return ["sent": true, "saved": true, "refused": []]
        })
        store.accounts = [account]
        store.draftAccount = account.id
        store.draftTo = "receiver@example.invalid"
        store.draftSubject = "Automatic text color"
        let view = NSTextView(frame: .zero)
        view.textStorage?.setAttributedString(NSAttributedString(string: "Hello\n\nRegards", attributes: [.font: NSFont.boldSystemFont(ofSize: 15)]))
        let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: true) { data, text in
            store.draftRTF = data; store.draftText = text
        }
        withAppearance(.darkAqua) {
            coordinator.textDidChange(Notification(name: NSText.didChangeNotification, object: view))
        }

        try await store.send()

        XCTAssertEqual(payload["text"] as? String, "Hello\n\nRegards")
        let html = try XCTUnwrap(payload["html"] as? String)
        assertNoForegroundCSS(html)
        XCTAssertTrue(html.contains("Hello"))
        XCTAssertTrue(html.contains("Regards"))
        XCTAssertTrue(html.contains("font-weight") || html.contains("<b>") || html.contains("<strong>"), "Sending must preserve emphasis")
        XCTAssertNil(store.draftRTF)
    }

    func testAttributedInsertionUndoRedoPreservesFormattingAndAutomaticColor() throws {
        let pasteboard = NSPasteboard(name: NSPasteboard.Name("YOBRO.MailTextColorTests.Undo.\(UUID().uuidString)"))
        defer { pasteboard.releaseGlobally() }
        pasteboard.declareTypes([.rtf], owner: nil)
        let rtf = #"{\rtf1\ansi\deff0{\fonttbl{\f0 Helvetica;}}\f0\fs30\b Bold paste\b0}"#
        XCTAssertTrue(pasteboard.setData(Data(rtf.utf8), forType: .rtf))
        let imported = try XCTUnwrap(NSAttributedString(rtf: XCTUnwrap(pasteboard.data(forType: .rtf)), documentAttributes: nil))
        var callbackSequences: [Bool: [String]] = [:]
        for adaptsColor in [false, true] {
            var published: [String] = []
            let coordinator = RichTextEditor.Coordinator(adaptsEmailTextColor: adaptsColor) { _, text in published.append(text) }
            let view = UndoableMailTextView(frame: .zero)
            view.isRichText = true
            view.allowsUndo = true
            view.delegate = coordinator
            coordinator.install(rtf: nil, fallbackText: "Before  After", in: view)
            let original = NSAttributedString(attributedString: view.attributedString())
            view.setSelectedRange(NSRange(location: 7, length: 0))
            let undo = view.localUndoManager
            undo.groupsByEvent = false
            undo.removeAllActions()

            withAppearance(.darkAqua) {
                undo.beginUndoGrouping()
                // The separate native-paste test covers readSelection. This
                // supplementary edit test uses the complete input entrypoint.
                view.insertText(imported, replacementRange: view.selectedRange())
                undo.endUndoGrouping()
            }

            let pasted = NSAttributedString(attributedString: view.attributedString())
            XCTAssertEqual(pasted.string, "Before Bold paste After")
            XCTAssertEqual(published, [pasted.string])
            if adaptsColor { XCTAssertEqual(color(in: pasted, at: 7), NSColor.textColor) }
            XCTAssertTrue(undo.canUndo, "Attributed text insertion must register an undo operation")
            guard undo.canUndo else { continue }

            undo.undo()

            XCTAssertEqual(view.attributedString(), original, "Undo must restore content, formatting, and colors")
            XCTAssertTrue(undo.canRedo)
            guard undo.canRedo else { continue }

            undo.redo()

            XCTAssertEqual(view.attributedString(), pasted, "Redo must restore content, formatting, and colors")
            let font = try XCTUnwrap(view.attributedString().attribute(.font, at: 7, effectiveRange: nil) as? NSFont)
            XCTAssertTrue(NSFontManager.shared.traits(of: font).contains(.boldFontMask))
            if adaptsColor {
                XCTAssertEqual(color(in: view.attributedString(), at: 7), NSColor.textColor)
                XCTAssertEqual(view.typingAttributes[.foregroundColor] as? NSColor, NSColor.textColor)
            }
            callbackSequences[adaptsColor] = published
        }
        // Windowless NSTextView + a private undo manager does not emit delegate
        // callbacks for undo/redo on the tested macOS version, even with color
        // adaptation disabled. Compare that baseline; do not fabricate callbacks.
        XCTAssertEqual(callbackSequences[true], callbackSequences[false])
    }

    private func formattedFixture() -> NSMutableAttributedString {
        let rich = NSMutableAttributedString(string: "Plain\n• Bold italic\nReference\nAccent\nContrast", attributes: [.font: NSFont.systemFont(ofSize: 15)])
        let font = NSFontManager.shared.convert(NSFont.boldSystemFont(ofSize: 15), toHaveTrait: .italicFontMask)
        let paragraph = NSMutableParagraphStyle()
        paragraph.textLists = [NSTextList(markerFormat: .disc, options: 0)]
        paragraph.paragraphSpacing = 9
        paragraph.headIndent = 24
        rich.addAttributes([.font: font, .paragraphStyle: paragraph, .underlineStyle: NSUnderlineStyle.single.rawValue], range: range("• Bold italic", in: rich))
        rich.addAttribute(.link, value: URL(string: "https://example.invalid/reference")!, range: range("Reference", in: rich))
        rich.addAttribute(.foregroundColor, value: NSColor.systemRed, range: range("Accent", in: rich))
        rich.addAttributes([.foregroundColor: NSColor.white, .backgroundColor: NSColor.black], range: range("Contrast", in: rich))
        return rich
    }

    private func range(_ substring: String, in rich: NSAttributedString) -> NSRange {
        (rich.string as NSString).range(of: substring)
    }

    private func fullRange(_ rich: NSAttributedString) -> NSRange {
        NSRange(location: 0, length: rich.length)
    }

    private func color(in rich: NSAttributedString, at index: Int) -> NSColor? {
        rich.attribute(.foregroundColor, at: index, effectiveRange: nil) as? NSColor
    }

    private func resolved(_ color: NSColor?, appearance: NSAppearance.Name) -> NSColor? {
        withAppearance(appearance) { color?.usingColorSpace(.sRGB) }
    }

    private func withAppearance<T>(_ name: NSAppearance.Name, _ body: () -> T) -> T {
        var result: T!
        NSAppearance(named: name)!.performAsCurrentDrawingAppearance { result = body() }
        return result
    }

    private func assertNoForegroundCSS(_ html: String, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertNil(html.range(of: #"(?i)(?<![\w-])color\s*:"#, options: .regularExpression), "Automatic text gained a fixed CSS foreground: \(html)", file: file, line: line)
        XCTAssertNil(html.range(of: #"(?i)<font\b[^>]*\bcolor\s*="#, options: .regularExpression), "Automatic text gained a legacy HTML font color", file: file, line: line)
    }
}

@MainActor
private final class UndoableMailTextView: NSTextView {
    let localUndoManager = UndoManager()
    override var undoManager: UndoManager? { localUndoManager }
}
