import XCTest
@testable import YOBRO

final class BrowserSyncTests: XCTestCase {
    private func snapshot() -> BrowserSyncSnapshot {
        let profile = UUID(uuidString: "00000000-0000-0000-0000-000000000010")!
        let tab = StoredTab(id: UUID(), title: "Example", url: "https://example.com", space: "Work", pinned: true,
                            faviconData: Data([1, 2, 3]))
        return BrowserSyncSnapshot(profileID: profile, modifiedAt: Date(timeIntervalSince1970: 100), tabs: [tab], activeID: tab.id,
                                   currentSpace: "Work", closedTabs: [], spaces: ["Work"], folders: [], splitPairs: [],
                                   bookmarks: [BookmarkEntry(title: "Example", url: "https://example.com", folder: "Saved")],
                                   history: [HistoryEntry(title: "Account", url: "https://accounts.google.com/signin?secret=yes", date: Date(timeIntervalSince1970: 90), visits: 1)],
                                   spaceIcons: ["Work": SpaceIcon.work.rawValue])
    }

    func testSnapshotStripsLocalOnlyDataAndSanitizesSensitiveLoginURLs() {
        let value = snapshot()
        XCTAssertNil(value.tabs.first?.faviconData)
        XCTAssertEqual(value.history.first?.url, "https://accounts.google.com/")
    }

    func testEncryptedSnapshotRoundTripAndWrongKeyFails() throws {
        let value = snapshot()
        let key = BrowserSyncCipher.makeKey()
        let sealed = try BrowserSyncCipher.seal(value, keyData: key)
        XCTAssertNotEqual(sealed.ciphertext, try JSONEncoder().encode(value))
        XCTAssertEqual(try BrowserSyncCipher.open(sealed, keyData: key), value)
        XCTAssertEqual(try BrowserSyncCipher.open(sealed, keyData: key).spaceIcons?["Work"], SpaceIcon.work.rawValue)
        XCTAssertThrowsError(try BrowserSyncCipher.open(sealed, keyData: BrowserSyncCipher.makeKey()))
    }

    func testInvalidKeyLengthIsRejected() {
        XCTAssertThrowsError(try BrowserSyncCipher.seal(snapshot(), keyData: Data([1, 2, 3])))
    }

    func testHistoryMergeKeepsBothSidesAndStaysIdempotent() {
        let older = Date(timeIntervalSince1970: 1_000)
        let newer = Date(timeIntervalSince1970: 2_000)
        let local = [
            HistoryEntry(title: "Shared old title", url: "https://example.com/page", date: older, visits: 3),
            HistoryEntry(title: "Only local", url: "https://local.example/a", date: older, visits: 1)
        ]
        let remote = [
            HistoryEntry(title: "Shared new title", url: "https://example.com/page", date: newer, visits: 5),
            HistoryEntry(title: "Only remote", url: "https://remote.example/b", date: newer, visits: 2)
        ]

        let merged = BrowserModel.mergedHistory(local, remote)

        // Neither side loses an entry: replacing instead of merging is what let
        // one device erase the other's history.
        XCTAssertEqual(Set(merged.map(\.url)), [
            "https://example.com/page", "https://local.example/a", "https://remote.example/b"
        ])
        let shared = merged.first { $0.url == "https://example.com/page" }
        XCTAssertEqual(shared?.title, "Shared new title", "the newer visit should win the title")
        XCTAssertEqual(shared?.visits, 5, "visit counts take the maximum, not the sum")

        // The merged result gets pushed back, so merging it again must be a no-op.
        let twice = BrowserModel.mergedHistory(merged, remote)
        XCTAssertEqual(twice.count, merged.count)
        XCTAssertEqual(twice.first { $0.url == "https://example.com/page" }?.visits, 5,
                       "repeated merges must not inflate visit counts")
    }

    func testRemoteWithAClockInTheFutureDoesNotWin() {
        let now = Date(timeIntervalSince1970: 10_000)
        func snapshot(modifiedAt: Date) -> BrowserSyncSnapshot {
            BrowserSyncSnapshot(profileID: UUID(), modifiedAt: modifiedAt, tabs: [], activeID: nil, currentSpace: "Work",
                                closedTabs: [], spaces: ["Work"], folders: [], splitPairs: [], bookmarks: [], history: [])
        }
        let local = snapshot(modifiedAt: now.addingTimeInterval(-60))

        XCTAssertTrue(BrowserSyncStore.remoteIsNewer(snapshot(modifiedAt: now), than: local, now: now))
        XCTAssertFalse(BrowserSyncStore.remoteIsNewer(snapshot(modifiedAt: now.addingTimeInterval(-600)), than: local, now: now))
        // A device whose clock runs far ahead would otherwise win every
        // comparison forever.
        XCTAssertFalse(BrowserSyncStore.remoteIsNewer(snapshot(modifiedAt: now.addingTimeInterval(86_400)), than: local, now: now))
    }

    func testCertificateExceptionsOnlyApplyToLocalHosts() {
        for host in ["localhost", "dev.local", "myapp.test", "127.0.0.1", "192.168.1.10", "10.0.0.5", "172.20.3.4", "169.254.1.1"] {
            XCTAssertTrue(CertificateTrustStore.isLocal(host), "\(host) should be eligible")
        }
        for host in ["example.com", "bank.de", "172.32.0.1", "8.8.8.8", "local.example.com", "notlocalhost.com"] {
            XCTAssertFalse(CertificateTrustStore.isLocal(host), "\(host) must never be excepted")
        }
    }
}
