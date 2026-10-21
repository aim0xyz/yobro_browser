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
        for step in [0,1,2,3] {
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
        // Finishing opens the address field instead of an empty tab.
        XCTAssertTrue(model.tabs.isEmpty)
        XCTAssertEqual(model.commandBar, .newTab)
        XCTAssertTrue(OnboardingProgress.isComplete(home:home))
    }

    @MainActor
    func testFinalStepShowsFourFeatureTips() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString:$0) }; setenv("YOBRO_HOME",home.path,1)
        defer { if let previous { setenv("YOBRO_HOME",previous,1) } else { unsetenv("YOBRO_HOME") }; try? FileManager.default.removeItem(at:home) }
        let model = BrowserModel()
        let tips = OnboardingView.tips
        XCTAssertEqual(tips.count, 4)
        XCTAssertEqual(Set(tips.map(\.icon)).count, 4)
        for tip in tips {
            XCTAssertNotNil(NSImage(systemSymbolName:tip.icon,accessibilityDescription:nil), "Unknown symbol \(tip.icon)")
            XCTAssertFalse(tip.shortcut.isEmpty); XCTAssertFalse(tip.title.isEmpty)
        }
        let step = NSHostingView(rootView:OnboardingView(model:model,step:3))
        step.frame = NSRect(x:0,y:0,width:770,height:680)
        let window = NSWindow(contentRect:step.frame,styleMask:[.titled],backing:.buffered,defer:false)
        window.isReleasedWhenClosed = false; window.contentView = step; window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds:200_000_000)
        step.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(step.bitmapImageRepForCachingDisplay(in:step.bounds)); step.cacheDisplay(in:step.bounds,to:bitmap)
        XCTAssertGreaterThan(try XCTUnwrap(bitmap.representation(using:.png,properties:[:])).count, 10_000)
        // The step's content width: four tips need exactly two rows of two.
        func height(_ tips: [OnboardingTip]) -> CGFloat {
            let grid = NSHostingView(rootView:OnboardingTips(tips:tips))
            grid.frame.size.width = 714
            return grid.fittingSize.height
        }
        let row = height(Array(tips.prefix(2))), four = height(tips)
        XCTAssertGreaterThan(row, 30)
        XCTAssertGreaterThan(four, row * 1.8, "Four tips fill a second row")
        XCTAssertLessThan(four, row * 2.5, "Four tips need no third row")
    }
}
