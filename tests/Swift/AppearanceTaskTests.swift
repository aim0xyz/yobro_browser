import XCTest
import SwiftUI
@testable import YOBRO

final class AppearanceTaskTests: XCTestCase {
    @MainActor
    func testRestartCancelsPreviousOperation() async {
        let lifetime = AppearanceTaskLifetime()
        let started = expectation(description: "First operation started")
        let cancelled = expectation(description: "First operation cancelled")
        let restarted = expectation(description: "Replacement operation started")
        lifetime.start {
            started.fulfill()
            do { try await Task.sleep(nanoseconds: 60_000_000_000) }
            catch { XCTAssertTrue(Task.isCancelled); cancelled.fulfill() }
        }
        await fulfillment(of: [started], timeout: 2)
        lifetime.start { restarted.fulfill() }
        await fulfillment(of: [cancelled, restarted], timeout: 2)
        lifetime.cancel()
    }

    @MainActor
    func testCancelBeforeScheduledOperationDoesNotRunIt() async {
        let lifetime = AppearanceTaskLifetime()
        var ran = false
        lifetime.start { ran = true }
        lifetime.cancel()
        await Task.yield()
        XCTAssertFalse(ran)
    }

    @MainActor
    func testRecreatedWindowStartsTaskAndIDChangeRestartsIt() async throws {
        // Exercise view destruction and recreation, as happens after closing
        // the last WindowGroup. No user browser or profile is touched.
        for _ in 0..<3 {
            let appeared = expectation(description: "Window appeared")
            let changed = expectation(description: "Task ID changed")
            let cancelled = expectation(description: "Window task cancelled")
            let disappeared = expectation(description: "Disappearing view cancelled task")
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 200),
                                  styleMask: [.titled, .closable], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            let host = NSHostingView(rootView: Color.clear.appearanceTask(id: 1) {
                appeared.fulfill()
                do { try await Task.sleep(nanoseconds: 60_000_000_000) }
                catch { XCTAssertTrue(Task.isCancelled); cancelled.fulfill() }
            })
            window.contentView = host
            window.orderFront(nil)
            await fulfillment(of: [appeared], timeout: 3)
            host.rootView = Color.clear.appearanceTask(id: 2) {
                changed.fulfill()
                do { try await Task.sleep(nanoseconds: 60_000_000_000) }
                catch { XCTAssertTrue(Task.isCancelled); disappeared.fulfill() }
            }
            await fulfillment(of: [cancelled, changed], timeout: 3)
            // Removing the hosting view must cancel its current task as well.
            // Keep the same id so cancellation cannot come from onChange.
            window.close()
            window.contentView = nil
            await fulfillment(of: [disappeared], timeout: 3)
        }
    }
}
