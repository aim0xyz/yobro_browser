import XCTest
import SwiftUI
@testable import YOBRO

final class OnboardingTests: XCTestCase {
    @MainActor
    func testCompletionPersistsAndWelcomeLayout() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString:$0) }; setenv("YOBRO_HOME",home.path,1)
        defer { if let previous { setenv("YOBRO_HOME",previous,1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at:home) }
        let model = BrowserModel()
        XCTAssertFalse(OnboardingProgress.isComplete(home:home))
        model.showOnboarding = true
        for step in [0,1,2] {
            let view = NSHostingView(rootView:OnboardingView(model:model,step:step))
            view.frame = NSRect(x:0,y:0,width:770,height:680)
            let window = NSWindow(contentRect:view.frame,styleMask:[.titled],backing:.buffered,defer:false)
            window.isReleasedWhenClosed = false; window.contentView = view; window.orderFront(nil)
            try await Task.sleep(nanoseconds:200_000_000)
            view.layoutSubtreeIfNeeded()
            let bitmap = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in:view.bounds)); view.cacheDisplay(in:view.bounds,to:bitmap)
            try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).write(to:URL(fileURLWithPath:"/tmp/YOBRO-onboarding-\(step).png"))
            window.close()
        }
        model.finishOnboarding()
        XCTAssertFalse(model.showOnboarding)
        XCTAssertEqual(model.tabs.count, 1)
        XCTAssertEqual(model.active?.url, "")
        XCTAssertTrue(OnboardingProgress.isComplete(home:home))
    }
}
