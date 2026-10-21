import SwiftUI
import AppKit

@main
struct YOBROApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var profiles = BrowserProfiles()
    private var browser: BrowserModel { profiles.browser }

    var body: some Scene {
        WindowGroup("YoBro", id: "main") {
            BrowserShell(model: browser)
                .id(profiles.registry.activeID)
                .frame(minWidth: 960, minHeight: 640)
                .tint(moss)
                .onAppear {
                    browser.startBridge()
                    browser.updates.start()
                    browser.showOnboarding = !OnboardingProgress.isComplete(home: browser.home)
                    delegate.browser = browser
                }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in profiles.persistAll() }
                .onDisappear {
                    browser.pauseAllMediaPlayback()
                    browser.persistSession()
                }
                .onOpenURL { url in
                    guard ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return }
                    browser.newTab(url: url.absoluteString)
                    NSApp.activate(ignoringOtherApps: true)
                }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1360, height: 880)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button(L("Neuer Tab")) { browser.openCommandBar(.newTab) }.keyboardShortcut("t")
                Button(L("Neuer privater Tab", "New private tab")) { browser.requestPrivateTab() }.keyboardShortcut("t", modifiers: [.command, .shift])
                Button(L("Tab schließen")) { browser.closeSidebarTab(browser.activeID) }.keyboardShortcut("w")
                Button(L("Geschlossenen Tab wieder öffnen")) { browser.reopenTab() }.disabled(browser.closedTabs.isEmpty)
            }
            CommandGroup(replacing: .appSettings) {
                Button(L("Einstellungen …")) { browser.showSettings = true }.keyboardShortcut(",")
            }
            CommandMenu("Browser") {
                Button(L("Einrichtung erneut öffnen …")) { browser.showOnboarding = true }
                Button(L("Erweiterungen und Datenimport …")) { browser.showSettings = true }
                Button(browser.showSidebar ? L("Seitenleiste einklappen") : L("Seitenleiste einblenden")) { browser.showSidebar.toggle() }.keyboardShortcut("s", modifiers: [.command])
                Button(browser.tabsOnTop ? L("Tabs in der Seitenleiste", "Tabs in the sidebar") : L("Tabs oben anzeigen", "Show tabs on top")) { browser.tabsOnTop.toggle() }.keyboardShortcut("s", modifiers: [.command, .option])
                Button(L("Adresse öffnen")) { browser.openCommandBar(.current) }.keyboardShortcut("l")
                Button(L("Schnellsuche")) { browser.openCommandBar(.newTab) }.keyboardShortcut("k")
                Button(L("Nach Updates suchen …", "Check for Updates…")) { browser.updates.installAvailableUpdate() }
                    .disabled(!browser.updates.isConfigured)
                Button(L("Auf Seite suchen")) { browser.active?.showFind = true }.keyboardShortcut("f").disabled(browser.active?.url.isEmpty != false)
                Button(L("Nächster Treffer")) { if let tab = browser.active { Task { await tab.find(tab.findQuery) } } }.keyboardShortcut("g")
                Button(L("Vorheriger Treffer")) { if let tab = browser.active { Task { await tab.find(tab.findQuery, backwards: true) } } }.keyboardShortcut("g", modifiers: [.command, .shift])
                Button(L("Verlauf")) { browser.librarySection = .history }.keyboardShortcut("y")
                Button("Downloads") { browser.librarySection = .downloads }.keyboardShortcut("j", modifiers: [.command, .shift])
                Button(L("Neu laden")) { browser.active?.webView.reload() }.keyboardShortcut("r")
                Button(L("Zurück")) { browser.active?.webView.goBack() }.keyboardShortcut("[", modifiers: .command)
                Button(L("Vorwärts")) { browser.active?.webView.goForward() }.keyboardShortcut("]", modifiers: .command)
                Divider()
                Button(L("Seite vergrößern", "Zoom in")) { browser.changeZoom(1) }
                    .keyboardShortcut("+", modifiers: .command).disabled(browser.active == nil)
                Button(L("Seite verkleinern", "Zoom out")) { browser.changeZoom(-1) }
                    .keyboardShortcut("-", modifiers: .command).disabled(browser.active == nil)
                Button(L("Originalgröße", "Actual size")) { browser.changeZoom(0) }
                    .keyboardShortcut("0", modifiers: .command).disabled(browser.active == nil)
                Button(L("Seite drucken …", "Print page …")) { browser.printActivePage() }
                    .keyboardShortcut("p", modifiers: .command).disabled(browser.active?.url.isEmpty != false)
                Button(L("Seite merken", "Bookmark page")) { browser.bookmarkActivePage() }
                    .keyboardShortcut("d", modifiers: .command).disabled(browser.active?.url.isEmpty != false)
                Button(L("Lesezeichen", "Bookmarks")) { browser.librarySection = .bookmarks }
                    .keyboardShortcut("b", modifiers: [.command, .option])
                Button(L("Assistent", "Assistant")) { browser.showAgent.toggle() }.keyboardShortcut("a", modifiers: [.command, .shift])
                Button(L("Geteilte Ansicht")) { browser.toggleSplit() }.keyboardShortcut("s", modifiers: [.command, .shift])
                Divider()
                // ⌘1–⌘9 for switching tabs was missing entirely.
                ForEach(1...9, id: \.self) { number in
                    // Interpolated text cannot be looked up in English.json, so
                    // both languages are given explicitly.
                    Button(number < 9 ? L("Tab \(number)", "Tab \(number)") : L("Letzter Tab", "Last tab")) {
                        browser.selectVisibleTab(at: number - 1)
                    }
                    .keyboardShortcut(KeyEquivalent(Character("\(number)")), modifiers: .command)
                }
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var browser: BrowserModel?
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Test only: never change the user's global macOS appearance.
        if ProcessInfo.processInfo.environment["YOBRO_DEV_CAPTURE"] == "1",
           let mode = ProcessInfo.processInfo.environment["YOBRO_TEST_APPEARANCE"] {
            NSApp.appearance = NSAppearance(named: mode == "dark" ? .darkAqua : .aqua)
        }
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationWillTerminate(_ notification: Notification) {
        // Session and history writes are debounced, so quitting inside that
        // window would otherwise discard the most recent change.
        browser?.persistSession()
        browser?.flushLibrary(waitForCompletion: true)
    }
    func applicationDidResignActive(_ notification: Notification) {
        browser?.updateAgentWorkspacePresentation(appIsActive: false)
    }
    func applicationDidBecomeActive(_ notification: Notification) {
        browser?.updateAgentWorkspacePresentation(appIsActive: true)
    }
    // Closing the browser window should behave like other Mac browsers: the app
    // remains available in the Dock and can restore its window on the next click.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag, let window = sender.windows.first(where: { $0.canBecomeMain }) {
            window.makeKeyAndOrderFront(nil)
            // We handled the reopen. Returning true also asks SwiftUI to open
            // an untitled WindowGroup, potentially recreating the view graph.
            return false
        }
        return true
    }
}
