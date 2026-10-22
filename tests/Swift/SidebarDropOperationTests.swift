import XCTest
import SwiftUI
@testable import YOBRO

/// What AppKit hands a drop target during a drag, reduced to what the
/// sidebar's targets read.
private final class StubDrag: NSObject, NSDraggingInfo {
    var draggingDestinationWindow: NSWindow?
    var draggingSourceOperationMask: NSDragOperation
    var draggingLocation: NSPoint
    var draggedImageLocation: NSPoint { draggingLocation }
    var draggedImage: NSImage? { nil }
    var draggingPasteboard: NSPasteboard
    var draggingSource: Any?
    var draggingSequenceNumber = 1
    var draggingFormation: NSDraggingFormation = .default
    var animatesToDestination = false
    var numberOfValidItemsForDrop = 1
    var springLoadingHighlight: NSSpringLoadingHighlight { .none }
    init(window: NSWindow, location: NSPoint, pasteboard: NSPasteboard, mask: NSDragOperation) {
        draggingDestinationWindow = window
        draggingLocation = location
        draggingPasteboard = pasteboard
        draggingSourceOperationMask = mask
    }
    func slideDraggedImage(to screenPoint: NSPoint) {}
    override func namesOfPromisedFilesDropped(atDestination dropDestination: URL) -> [String]? { nil }
    func enumerateDraggingItems(options enumOpts: NSDraggingItemEnumerationOptions = [], for view: NSView?, classes classArray: [AnyClass],
                                searchOptions: [NSPasteboard.ReadingOptionKey: Any] = [:], using block: (NSDraggingItem, Int, UnsafeMutablePointer<ObjCBool>) -> Void) {}
    func resetSpringLoading() {}
}

final class SidebarDropOperationTests: XCTestCase {
    /// The source's callbacks only need some session to be handed.
    static let session = (NSDraggingSession.self as NSObject.Type).init() as! NSDraggingSession

    /// AppKit drops only with an operation the drag source offers. The
    /// model-level drop tests never see that check, and answering one the
    /// source does not offer makes every sidebar drop fail.
    @MainActor
    func testSidebarDropAnswersWithAnOperationTheTabDragOffers() async throws {
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let previous = getenv("YOBRO_HOME").map { String(cString: $0) }
        setenv("YOBRO_HOME", home.path, 1)
        defer {
            if let previous { setenv("YOBRO_HOME", previous, 1) } else { unsetenv("YOBRO_HOME") }
            try? FileManager.default.removeItem(at: home)
            UserDefaults.standard.removeObject(forKey: "YOBRO.showSidebar")
        }
        let model = BrowserModel()
        model.showAgent = false
        model.showSidebar = true
        let tabs = ["Alpha", "Bravo", "Charlie"].map { name -> BrowserTab in
            let tab = model.newTab()
            tab.url = "https://\(name.lowercased()).test/"
            tab.title = name
            return tab
        }
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700), styleMask: [.titled, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 600_000_000)

        // The mask the sidebar's drag source offers inside the app.
        let sourceMask = SidebarDragSource.shared.draggingSession(Self.session, sourceOperationMaskFor: .withinApplication)
        XCTAssertFalse(sourceMask.isEmpty)

        let pasteboard = NSPasteboard(name: .init("yobro.test.\(UUID().uuidString)"))
        defer { pasteboard.releaseGlobally() }
        pasteboard.clearContents()
        pasteboard.setString(tabs[0].id.uuidString, forType: .string)
        model.beginSidebarTabDrag(tabs[0].id)
        defer { model.finishSidebarDrag() }
        try await Task.sleep(nanoseconds: 300_000_000)

        // The front-most view registered for drags under the point, as AppKit picks it.
        func dropTarget(_ view: NSView, at point: NSPoint) -> NSView? {
            guard !view.isHidden, view.alphaValue > 0, view.convert(view.bounds, to: nil).contains(point) else { return nil }
            for subview in view.subviews.reversed() {
                if let found = dropTarget(subview, at: point) { return found }
            }
            return view.registeredDraggedTypes.isEmpty ? nil : view
        }
        var answered: NSDragOperation?
        for top in stride(from: 40.0, through: 500.0, by: 1.0) {
            let point = NSPoint(x: 110, y: host.bounds.height - top)
            guard let target = dropTarget(host, at: point) else { continue }
            let info = StubDrag(window: window, location: point, pasteboard: pasteboard, mask: sourceMask)
            _ = target.draggingEntered(info)
            let operation = target.draggingUpdated(info)
            await Task.yield()
            let feedback = model.tabDropFeedback
            target.draggingExited(info)
            if feedback?.targetID == tabs[2].id, feedback?.intent == .split {
                answered = operation
                break
            }
        }
        let operation = try XCTUnwrap(answered, "No split zone on the last tab's row")
        XCTAssertFalse(operation.intersection(sourceMask).isEmpty,
                       "The drop answers \(operation.rawValue), which the drag (\(sourceMask.rawValue)) does not offer")
    }

    /// In the app's window SwiftUI's `onDrag` never started a drag, so the
    /// sidebar starts one in AppKit from the press under way, and AppKit's
    /// end of the session, dropped or not, ends the drag in the model.
    @MainActor
    func testTabDragStartsFromThePressAndEndsWithTheSession() async throws {
        _ = NSApplication.shared
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home) }
        let model = BrowserModel(root: home)
        let tab = model.newTab()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 300, height: 200), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = NSView()
        defer { window.close() }
        let source = SidebarDragSource.shared
        var started: [String] = []
        source.startSession = { view, item, event in
            XCTAssertTrue(view === window.contentView)
            XCTAssertEqual(event.type, .leftMouseDragged)
            started.append((item.item as? NSString).map { String($0) } ?? "")
        }
        defer { source.startSession = { view, item, event in view.beginDraggingSession(with: [item], event: event, source: SidebarDragSource.shared) } }

        // The press under way is the app's current event.
        let drag = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDragged, location: NSPoint(x: 40, y: 40), modifierFlags: [], timestamp: 0,
                                                    windowNumber: window.windowNumber, context: nil, eventNumber: 1, clickCount: 1, pressure: 1))
        NSApp.postEvent(drag, atStart: true)
        _ = NSApp.nextEvent(matching: .leftMouseDragged, until: Date(), inMode: .default, dequeue: true)
        model.beginSidebarTabDrag(tab.id)
        let session = model.sidebarDragSessionID
        var ended = 0
        XCTAssertTrue(source.begin(tab.id.uuidString, preview: NSImage(size: NSSize(width: 200, height: 34))) {
            ended += 1
            model.finishSidebarDrag(sessionID: session)
        })
        XCTAssertEqual(started, [tab.id.uuidString], "The drag carries the tab's ID as text, as the drops read it")
        // Only the sidebar's drops, which answer move, take it, whatever
        // modifier is held: with copy, text fields took the ID as text.
        XCTAssertEqual(DropProposal.sidebarDrop.operation, .move)
        XCTAssertEqual(source.draggingSession(Self.session, sourceOperationMaskFor: .withinApplication), .move)
        XCTAssertTrue(source.draggingSession(Self.session, sourceOperationMaskFor: .outsideApplication).isEmpty)
        XCTAssertTrue(source.ignoreModifierKeys(for: Self.session))
        let page = AppearanceWebView()
        let pageDrag = StubDrag(window: window, location: NSPoint(x: 40, y: 40), pasteboard: NSPasteboard(name: .drag), mask: .move)
        pageDrag.draggingSource = source
        XCTAssertEqual(page.draggingEntered(pageDrag), [], "The page takes no tab as text")

        source.draggingSession(Self.session, endedAt: .zero, operation: [])
        XCTAssertEqual(ended, 0, "The drop target's own cleanup runs first")
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(ended, 1)
        XCTAssertNil(model.draggingTabID, "A drag nobody took still ends")
        source.draggingSession(Self.session, endedAt: .zero, operation: [])
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(ended, 1, "Each session ends once")

        // Escape ends the session while the button is still down: the same
        // press must not start another drag. The next press may.
        XCTAssertTrue(source.pressDragged)
        let press = try XCTUnwrap(NSEvent.mouseEvent(with: .leftMouseDown, location: NSPoint(x: 40, y: 40), modifierFlags: [], timestamp: 0,
                                                     windowNumber: window.windowNumber, context: nil, eventNumber: 2, clickCount: 1, pressure: 1))
        NSApp.sendEvent(press)
        XCTAssertFalse(source.pressDragged)
    }

    /// The card is drawn without a window, so it must take the sidebar's
    /// look along: in dark mode a white card with dark text stood out.
    @MainActor
    func testDragCardFollowsTheSidebarsColorScheme() throws {
        func brightness(_ scheme: ColorScheme) throws -> CGFloat {
            let image = SidebarDragSource.card("Alpha", icon: "globe", colorScheme: scheme)
            let bitmap = try XCTUnwrap(image.tiffRepresentation.flatMap(NSBitmapImageRep.init(data:)))
            // Inside the card, left of the icon.
            let color = try XCTUnwrap(bitmap.colorAt(x: bitmap.pixelsWide * 2 / 200, y: bitmap.pixelsHigh / 2)?.usingColorSpace(.sRGB))
            return color.brightnessComponent
        }
        XCTAssertLessThan(try brightness(.dark), 0.3)
        XCTAssertGreaterThan(try brightness(.light), 0.8)
    }

    /// The drag session takes the mouse-up, so SwiftUI still held the press
    /// after a drop and swallowed the next click in the sidebar.
    @MainActor
    func testTheClickAfterADragStillSelectsATab() async throws {
        _ = NSApplication.shared
        let home = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: home); UserDefaults.standard.removeObject(forKey: "YOBRO.showSidebar") }
        let model = BrowserModel(root: home)
        model.showAgent = false
        model.showSidebar = true
        let tabs = ["Alpha", "Bravo", "Charlie", "Delta"].map { name -> BrowserTab in
            let tab = model.newTab()
            tab.url = "https://\(name.lowercased()).test/"
            tab.title = name
            return tab
        }
        model.select(tabs[3].id)
        let host = NSHostingView(rootView: BrowserShell(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700), styleMask: [.titled, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { window.close() }
        try await Task.sleep(nanoseconds: 800_000_000)
        // Each tab row is a drop target, top to bottom.
        func dropTargets(_ view: NSView) -> [NSView] { (view.registeredDraggedTypes.isEmpty ? [] : [view]) + view.subviews.flatMap(dropTargets) }
        let rows = dropTargets(host).filter { !($0 is AppearanceWebView) }.map { $0.convert($0.bounds, to: nil) }
            .filter { abs($0.height - (Chrome.rowHeight + 5)) < 1 }.sorted { $0.minY > $1.minY }
        XCTAssertEqual(rows.count, tabs.count)
        let source = SidebarDragSource.shared
        var dragged: [String] = []
        source.startSession = { _, item, _ in dragged.append((item.item as? NSString).map { String($0) } ?? "") }
        defer { source.startSession = { view, item, event in view.beginDraggingSession(with: [item], event: event, source: SidebarDragSource.shared) } }
        var press = 100
        func send(_ type: NSEvent.EventType, _ point: NSPoint) {
            NSApp.sendEvent(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                               windowNumber: window.windowNumber, context: nil, eventNumber: press, clickCount: 1,
                                               pressure: type == .leftMouseUp ? 0 : 1)!)
        }
        func drag(_ row: NSRect) async throws {
            press += 1
            let start = NSPoint(x: row.minX + 60, y: row.midY)
            send(.leftMouseDown, start)
            for step in 1...10 {
                send(.leftMouseDragged, NSPoint(x: start.x + Double(step), y: start.y - Double(step) * 4))
                try await Task.sleep(nanoseconds: 15_000_000)
            }
            // The session takes the mouse-up.
            try await Task.sleep(nanoseconds: 150_000_000)
        }

        try await drag(rows[0])
        XCTAssertEqual(dragged, [tabs[0].id.uuidString])
        source.draggingSession(Self.session, endedAt: .zero, operation: [])
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertNil(model.draggingTabID)
        XCTAssertEqual(model.activeID, tabs[3].id, "Releasing the dragged row's press does not open it")

        press += 1
        let bravo = NSPoint(x: rows[1].minX + 60, y: rows[1].midY)
        send(.leftMouseDown, bravo)
        send(.leftMouseUp, bravo)
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(model.activeID, tabs[1].id, "The click after a drag selects the tab")

        try await drag(rows[2])
        XCTAssertEqual(dragged, [tabs[0].id.uuidString, tabs[2].id.uuidString], "And the next drag starts")
        source.draggingSession(Self.session, endedAt: .zero, operation: [])
        try await Task.sleep(nanoseconds: 200_000_000)
    }
}
