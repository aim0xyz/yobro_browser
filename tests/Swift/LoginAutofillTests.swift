import XCTest
import WebKit
import Security
@testable import YOBRO

final class LoginAutofillTests: XCTestCase {
    @MainActor
    func testLoginScriptIsInstalledInFramesForAppleStyleSignIn() {
        let configuration = WKWebViewConfiguration()
        let login = LoginAutofill()
        login.install(on: configuration.userContentController)
        let scripts = configuration.userContentController.userScripts
        XCTAssertEqual(scripts.count, 1)
        XCTAssertFalse(scripts[0].isForMainFrameOnly)
        XCTAssertFalse(scripts[0].source.contains("window.top !== window"))
        XCTAssertTrue(scripts[0].source.contains("event: 'ready'"))
    }

    func testOriginMatchingRequiresHTTPSAndExactHostAndPort() {
        XCTAssertEqual(LoginAutofill.origin(URL(string: "https://example.com:443/login?token=secret")), "https://example.com")
        XCTAssertEqual(LoginAutofill.origin(URL(string: "https://example.com:8443/login")), "https://example.com:8443")
        XCTAssertNotEqual(LoginAutofill.origin(URL(string: "https://example.com")), LoginAutofill.origin(URL(string: "https://example.com.evil.test")))
        XCTAssertNotEqual(LoginAutofill.origin(URL(string: "https://example.com")), LoginAutofill.origin(URL(string: "https://accounts.example.com")))
        XCTAssertNil(LoginAutofill.origin(URL(string: "http://example.com/login")))
        XCTAssertNil(LoginAutofill.origin(URL(string: "file:///tmp/login.html")))
    }

    func testAppleIdentityFrameIsAllowedOnlyInsideApplePages() {
        XCTAssertTrue(LoginAutofill.permitsLoginFrame(origin: "https://idmsa.apple.com", in: "https://appstoreconnect.apple.com"))
        XCTAssertTrue(LoginAutofill.permitsLoginFrame(origin: "https://appleid.apple.com", in: "https://developer.apple.com"))
        XCTAssertTrue(LoginAutofill.permitsLoginFrame(origin: "https://example.com", in: "https://example.com"))
        XCTAssertFalse(LoginAutofill.permitsLoginFrame(origin: "https://idmsa.apple.com.evil.test", in: "https://appstoreconnect.apple.com"))
        XCTAssertFalse(LoginAutofill.permitsLoginFrame(origin: "https://idmsa.apple.com", in: "https://example.com"))
        XCTAssertFalse(LoginAutofill.permitsLoginFrame(origin: "https://evil.test", in: "https://appstoreconnect.apple.com"))
    }

    func testVaultOnlyReturnsMatchingOriginAndProfile() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: PasswordVault.service(home: home)] as CFDictionary) }
        for address in ["https://login.example.test/path", "https://login.example.test:8443", "https://other.example.test", "http://login.example.test"] {
            XCTAssertTrue(try PasswordVault.store(ImportPassword(url: address, username: "fixture", password: "fixture-only"), home: home, replace: false))
        }
        let entries = try PasswordVault.entries(home: home, origin: "https://login.example.test")
        XCTAssertEqual(entries.count, 1)
        XCTAssertEqual(entries.first?.url, "https://login.example.test/path")
        XCTAssertTrue(try PasswordVault.entries(home: home.appendingPathComponent("other-profile"), origin: "https://login.example.test").isEmpty)
    }

    @MainActor
    func testSaveOfferUsesLatestAttemptAndSkipsExactStoredLogin() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer {
            SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: PasswordVault.service(home: home)] as CFDictionary)
            try? FileManager.default.removeItem(at: home)
        }
        let login = LoginAutofill()
        let wrong = ImportPassword(url: "https://login.example.test", username: "person@example.test", password: "wrong")
        let correct = ImportPassword(url: "https://login.example.test", username: "person@example.test", password: "correct")

        login.considerForSaving(wrong, home: home)
        XCTAssertEqual(login.pending?.entry.password, "wrong")
        login.pending = nil // User chose “Not now”.
        login.considerForSaving(correct, home: home)
        XCTAssertEqual(login.pending?.entry.password, "correct")
        XCTAssertTrue(try PasswordVault.store(correct, home: home, replace: true))
        login.considerForSaving(correct, home: home)
        XCTAssertNil(login.pending, "An unchanged saved login must not prompt again")
    }

    @MainActor
    func testTrustedSubmitOffersSavingWithoutStoringAutomatically() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            SecItemDelete([kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: PasswordVault.service(home: home)] as CFDictionary)
            try? FileManager.default.removeItem(at: home)
        }
        let model = BrowserModel(root: home)
        let tab = model.newTab()
        let web = tab.webView
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = web; window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        web.loadHTMLString("<form onsubmit='event.preventDefault()'><input name='email' type='email' value='test@example.test'><input id='password' type='password' value='fixture-only'><button>Sign in</button></form>", baseURL: URL(string: "https://login.example.test/"))
        for _ in 0..<60 {
            if (try? await LoginAutofill.call(web, script: "return !!window.yobroLogin;")) as? Bool == true { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        // Programmatic submission must not collect credentials.
        _ = try await web.evaluateJavaScript("document.querySelector('form').requestSubmit()")
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertNil(tab.logins.pending)
        window.makeFirstResponder(web)
        _ = try await web.evaluateJavaScript("document.querySelector('#password').focus()")
        for type in [NSEvent.EventType.keyDown, .keyUp] {
            let event = try XCTUnwrap(NSEvent.keyEvent(with: type, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber, context: nil, characters: "\r", charactersIgnoringModifiers: "\r", isARepeat: false, keyCode: 36))
            window.sendEvent(event)
        }
        for _ in 0..<30 {
            if tab.logins.pending != nil { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertEqual(tab.logins.pending?.entry.username, "test@example.test")
        XCTAssertEqual(tab.logins.pending?.entry.url, "https://login.example.test")
        XCTAssertTrue(try PasswordVault.entries(home: home).isEmpty)
        tab.logins.save()
        XCTAssertNil(tab.logins.pending)
        let saved = try XCTUnwrap(tab.logins.accounts().first)
        _ = try await web.evaluateJavaScript("document.querySelectorAll('input').forEach(el => el.value = '')")
        try await tab.logins.fill(saved)
        let username = try await web.evaluateJavaScript("document.querySelector('[name=email]').value") as? String
        XCTAssertEqual(username, "test@example.test")
    }

    @MainActor
    func testFillUsesIsolatedWorldAndDoesNotSubmit() async throws {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        let login = LoginAutofill()
        login.install(on: configuration.userContentController)
        let web = WKWebView(frame: NSRect(x: 0, y: 0, width: 800, height: 600), configuration: configuration)
        let window = NSWindow(contentRect: web.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = web; window.orderFront(nil)
        defer { window.close() }
        web.loadHTMLString("<form onsubmit='window.submitted=true;return false'><input id='email' type='email'><input id='password' type='password'><input id='hidden' type='password' style='display:none'><button>Sign in</button></form>", baseURL: URL(string: "https://login.example.test/"))
        for _ in 0..<60 {
            if (try? await LoginAutofill.call(web, script: "return !!window.yobroLogin;")) as? Bool == true { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        let document = try await LoginAutofill.call(web, script: "return window.yobroLogin?.documentID ?? '';" ) as? String ?? ""
        XCTAssertFalse(document.isEmpty)
        guard !document.isEmpty else { return }
        let script = "return window.yobroLogin.fill(origin, documentID, username, password);"
        var arguments: [String: Any] = ["origin": "https://evil.test", "documentID": document, "username": "test@example.test", "password": "fixture-only"]
        let rejected = try await LoginAutofill.call(web, script: script, arguments: arguments)
        XCTAssertEqual(rejected as? Bool, false)
        arguments["origin"] = "https://login.example.test"
        arguments["documentID"] = "stale-document"
        let stale = try await LoginAutofill.call(web, script: script, arguments: arguments)
        XCTAssertEqual(stale as? Bool, false)
        arguments["documentID"] = document
        let filled = try await LoginAutofill.call(web, script: script, arguments: arguments)
        XCTAssertEqual(filled as? Bool, true)
        let values = try await web.evaluateJavaScript("[document.querySelector('#email').value, document.querySelector('#password').value, document.querySelector('#hidden').value, !!window.submitted, typeof window.yobroLogin]") as? [Any]
        XCTAssertEqual(values?[0] as? String, "test@example.test")
        XCTAssertEqual(values?[1] as? String, "fixture-only")
        XCTAssertEqual(values?[2] as? String, "")
        XCTAssertEqual(values?[3] as? Bool, false)
        XCTAssertEqual(values?[4] as? String, "undefined")
        XCTAssertNil(login.pending, "Filling must not trigger a save prompt")
        _ = try await web.evaluateJavaScript("document.querySelector('form').action = 'https://other.example.test/'")
        let crossOrigin = try await LoginAutofill.call(web, script: script, arguments: arguments)
        XCTAssertEqual(crossOrigin as? Bool, false)
        _ = try await web.evaluateJavaScript("document.querySelector('form').action = ''; document.querySelector('#password').autocomplete = 'new-password'")
        let signup = try await LoginAutofill.call(web, script: script, arguments: arguments)
        XCTAssertEqual(signup as? Bool, false)
    }
}
