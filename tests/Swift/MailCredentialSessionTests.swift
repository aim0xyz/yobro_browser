import XCTest
import Security
@testable import YOBRO
final class MailCredentialSessionTests: XCTestCase {
    func testMailPollingUsesFiveMinuteInterval() {
        XCTAssertEqual(MailStore.pollingIntervalNanoseconds, 300_000_000_000)
    }

    @MainActor
    func testRemoteImagesDefaultToEnabledAndPreferencePersists() throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: home) }
        let initial = MailStore(home: home)
        XCTAssertTrue(initial.remoteImagesEnabled)
        initial.setRemoteImages(false)
        XCTAssertFalse(MailStore(home: home).remoteImagesEnabled)
    }

    func testBackgroundKeychainReadSuppressesUIAndRestoresPolicy() throws {
        var before: DarwinBoolean = true
        XCTAssertEqual(SecKeychainGetUserInteractionAllowed(&before), errSecSuccess)
        XCTAssertThrowsError(try MailSecrets.withoutAuthenticationUI {
            var during: DarwinBoolean = true
            XCTAssertEqual(SecKeychainGetUserInteractionAllowed(&during), errSecSuccess)
            XCTAssertFalse(during.boolValue)
            throw MailSecretError.accessRequired
        })
        var after: DarwinBoolean = false
        XCTAssertEqual(SecKeychainGetUserInteractionAllowed(&after), errSecSuccess)
        XCTAssertEqual(after.boolValue, before.boolValue)
    }
    @MainActor
    func testLockedMailboxIsPresentedForExplicitUnlock() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at:home,withIntermediateDirectories:true)
        defer { try? FileManager.default.removeItem(at:home) }
        let account = MailAccount(address:"test@example.invalid")
        let store = MailStore(home:home) { _,_,_ in throw MailSecretError.accessRequired }
        store.accounts = [account]
        await store.refresh()
        XCTAssertEqual(store.lockedAccounts, [account.id])
        XCTAssertTrue(store.errors[account.id]?.contains(L("Mail entsperren")) == true)
        XCTAssertNil(store.unlockingAccount)
    }
    func testCredentialsReadOnlyOnceAndReplacedAfterEdit() throws {
        let session = MailCredentialSession(), id = UUID()
        var reads = 0
        for _ in 0..<5 { XCTAssertEqual(try session.read(id) { reads += 1; return "fixture" }, "fixture") }
        XCTAssertEqual(reads, 1)
        session.set("updated", id:id)
        XCTAssertEqual(try session.read(id) { XCTFail("Must use updated value"); return "" }, "updated")
        session.remove(id)
        _ = try session.read(id) { reads += 1; return "new" }
        XCTAssertEqual(reads, 2)
    }
    func testDeniedAccessIsNotRepeatedByPolling() throws {
        let session = MailCredentialSession(), id = UUID()
        var prompts = 0
        for _ in 0..<5 { XCTAssertThrowsError(try session.read(id) { prompts += 1; throw YOBROError.message("Denied fixture") }) }
        XCTAssertEqual(prompts, 1)
        session.retryFailures()
        XCTAssertEqual(try session.read(id) { prompts += 1; return "authorized fixture" }, "authorized fixture")
        XCTAssertEqual(prompts, 2)
    }
}
