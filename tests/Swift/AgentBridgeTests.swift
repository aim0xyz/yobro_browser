import XCTest
import WebKit
@testable import YOBRO

@MainActor
final class AgentBridgeTests: XCTestCase {
    private func fixture(_ html: String) async throws -> WKWebView {
        _ = NSApplication.shared
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let source = try String(contentsOf: root.appendingPathComponent("Sources/YOBRO/Resources/AgentBridge.js"))
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        config.userContentController.addUserScript(WKUserScript(source: source, injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: .defaultClient))
        let view = WKWebView(frame: NSRect(x: 0, y: 0, width: 800, height: 600), configuration: config)
        view.loadHTMLString("<!doctype html><body>" + html + "</body>", baseURL: URL(string: "https://agent-fixture.invalid"))
        for _ in 0..<100 {
            if (try? await js("Boolean(globalThis.__yobro)", view)) as? Bool == true { return view }
            try await Task.sleep(for: .milliseconds(50))
        }
        throw NSError(domain: "Fixture did not load", code: 1)
    }
    private func js(_ source: String, _ view: WKWebView) async throws -> Any {
        try await withCheckedThrowingContinuation { continuation in
            view.evaluateJavaScript(source, in: nil, in: .defaultClient) { continuation.resume(with: $0) }
        }
    }
    private func act(_ label: String, action: String, value: String = "", key: String = "Enter", view: WKWebView) async throws {
        let args = String(decoding: try JSONSerialization.data(withJSONObject: ["label": label, "action": action, "value": value, "key": key]), as: UTF8.self)
        _ = try await js("""
        (() => { const a = \(args); const p = __yobro.snapshot();
        return __yobro.act({...a, ref:p.elements.find(e => e.label === a.label).ref, document:p.document}); })()
        """, view)
    }
    private func rejects(_ source: String, _ view: WKWebView) async throws {
        let rejected = try await js("(() => { try { \(source); return false; } catch (_) { return true; } })()", view) as? Bool
        XCTAssertEqual(rejected, true)
    }
    func testInputWithTextboxRoleFillsNativeValueAndRespectsBeforeInput() async throws {
        let view = try await fixture("<input aria-label='Name' role='textbox' value='Original'>")
        try await act("Name", action: "fill", value: "Laura ☀", view: view)
        let value = try await js("document.querySelector('input').value", view) as? String
        XCTAssertEqual(value, "Laura ☀")
        _ = try await js("document.querySelector('input').addEventListener('beforeinput', e => e.preventDefault()); true", view)
        try await rejects("const p = __yobro.snapshot(); __yobro.act({action:'fill',ref:p.elements[0].ref,document:p.document,value:'Changed'})", view)
        let unchanged = try await js("document.querySelector('input').value", view) as? String
        XCTAssertEqual(unchanged, "Laura ☀")
    }
    func testStaleDocumentCannotActEvenWhenElementRefExists() async throws {
        let view = try await fixture("<input type='checkbox' aria-label='Remember'>")
        try await rejects("const p=__yobro.snapshot(); __yobro.act({action:'click',ref:p.elements[0].ref,document:'old-document'})", view)
        let checked = try await js("document.querySelector('input').checked", view) as? Bool
        XCTAssertEqual(checked, false)
        try await act("Remember", action: "click", view: view)
        let updated = try await js("document.querySelector('input').checked", view) as? Bool
        XCTAssertEqual(updated, true)
    }
    func testDisabledAncestorsAndCheckboxFillAreRejected() async throws {
        let view = try await fixture("<div aria-disabled='true'><button>Blocked</button></div><fieldset disabled><input aria-label='Disabled'></fieldset><input type='checkbox' aria-label='Check' value='original'>")
        let disabled = try await js("__yobro.snapshot().elements.filter(e=>e.disabled).length", view) as? Int
        XCTAssertEqual(disabled, 2)
        for index in 0..<3 {
            try await rejects("const p=__yobro.snapshot(); __yobro.act({action:'fill',ref:p.elements[\(index)].ref,document:p.document,value:'changed'})", view)
        }
        let value = try await js("document.querySelector('input[type=checkbox]').value", view) as? String
        XCTAssertEqual(value, "original")
    }
    func testSelectRejectsMissingDisabledAndAmbiguousOptionsWithoutChangingValue() async throws {
        let view = try await fixture("<select aria-label='Choice'><option value='a'>First</option><option value='A'>Upper</option><option disabled value='off'>Disabled</option><option value='b'>Same</option><option value='c'>Same</option></select>")
        for value in ["missing", "off", "Same"] {
            try await rejects("const p=__yobro.snapshot(); __yobro.act({action:'fill',ref:p.elements[0].ref,document:p.document,value:'\(value)'})", view)
            let unchanged = try await js("document.querySelector('select').value", view) as? String
            XCTAssertEqual(unchanged, "a")
        }
        try await act("Choice", action: "fill", value: "A", view: view)
        let selected = try await js("document.querySelector('select').value", view) as? String
        XCTAssertEqual(selected, "A")
    }
    func testPreventedEnterDoesNotSubmitAndTextAreaEnterDoesNotSubmit() async throws {
        let view = try await fixture("<form><input aria-label='Name'><textarea aria-label='Notes'></textarea></form>")
        _ = try await js("globalThis.submissions=0; document.querySelector('form').addEventListener('submit',e=>{e.preventDefault();submissions++}); globalThis.cancel=e=>e.preventDefault(); document.querySelector('input').addEventListener('keydown',cancel); true", view)
        try await act("Name", action: "press", view: view)
        try await act("Notes", action: "press", view: view)
        let prevented = try await js("submissions", view) as? Int
        XCTAssertEqual(prevented, 0)
        _ = try await js("document.querySelector('input').removeEventListener('keydown',cancel); true", view)
        try await act("Name", action: "press", view: view)
        let submitted = try await js("submissions", view) as? Int
        XCTAssertEqual(submitted, 1)
    }
}
