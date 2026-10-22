import SwiftUI
import AppKit
import UserNotifications

@main
struct YOBROApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var profiles = BrowserProfiles()
    private var browser: BrowserModel { profiles.browser }

    var body: some Scene {
        // A single window: a WindowGroup opened a second, empty window for
        // links from other apps and offered macOS window tabs.
        Window("YoBro", id: "main") {
            BrowserShell(model: browser)
                .id(profiles.registry.activeID)
                .frame(minWidth: 960, minHeight: 640)
                .tint(moss)
                .onAppear {
                    browser.startBridge()
                    browser.mail.startPolling()
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
                Button(L("Neuer privater Tab", "New private tab")) { browser.requestPrivateTab() }.keyboardShortcut("n", modifiers: [.command, .shift])
                Button(L("Tab schließen")) {
                    // A Peek closes first; with no tab left, ⌘W closes the window.
                    if browser.peekTab != nil { browser.closePeek() }
                    else if browser.activeID == nil { NSApp.keyWindow?.performClose(nil) }
                    else { browser.closeSidebarTab(browser.activeID) }
                }.keyboardShortcut("w")
                Button(L("Geschlossenen Tab wieder öffnen")) { browser.reopenTab() }
                    .keyboardShortcut("t", modifiers: [.command, .shift]).disabled(browser.closedTabs.isEmpty)
            }
            // Replaces the system "Close" item, which also claimed ⌘W.
            CommandGroup(replacing: .saveItem) {
                Button(L("Fenster schließen", "Close window")) { NSApp.keyWindow?.performClose(nil) }.keyboardShortcut("w", modifiers: [.command, .shift])
            }
            CommandGroup(replacing: .appSettings) {
                Button(L("Einstellungen …")) { openSettings(L("Allgemein", "General")) }.keyboardShortcut(",")
            }
            CommandMenu("Browser") {
                Button(L("Einrichtung erneut öffnen …")) { browser.showOnboarding = true }
                Button(L("Erweiterungen …", "Extensions …")) { openSettings(L("Erweiterungen")) }
                Button(L("Browserdaten importieren …", "Import browser data …")) { openSettings(L("Daten importieren")) }
                Button(sidebarToggleTitle) { browser.showSidebar.toggle() }.keyboardShortcut("s", modifiers: [.command])
                Button(browser.tabsOnTop ? L("Tabs in der Seitenleiste", "Tabs in the sidebar") : L("Tabs oben anzeigen", "Show tabs on top")) { browser.tabsOnTop.toggle() }.keyboardShortcut("s", modifiers: [.command, .option])
                Button(L("Adresse öffnen")) { browser.openCommandBar(.current) }.keyboardShortcut("l")
                Button(L("Schnellsuche")) { browser.openCommandBar(.newTab) }.keyboardShortcut("k")
                Button(L("Nach Updates suchen …", "Check for Updates…")) { browser.updates.installAvailableUpdate() }
                    .disabled(!browser.updates.isConfigured)
                // Page commands act on the Peek while it is open.
                Button(L("Auf Seite suchen")) { browser.commandTarget?.requestFind() }.keyboardShortcut("f").disabled(browser.commandTarget?.url.isEmpty != false)
                Button(L("Nächster Treffer")) { if let tab = browser.commandTarget { Task { await tab.find(tab.findQuery) } } }
                    .keyboardShortcut("g").disabled(browser.commandTarget == nil)
                Button(L("Vorheriger Treffer")) { if let tab = browser.commandTarget { Task { await tab.find(tab.findQuery, backwards: true) } } }
                    .keyboardShortcut("g", modifiers: [.command, .shift]).disabled(browser.commandTarget == nil)
                Button(L("Verlauf")) { browser.librarySection = .history }.keyboardShortcut("y")
                Button("Downloads") { browser.librarySection = .downloads }.keyboardShortcut("j", modifiers: [.command, .shift])
                Button(L("Neu laden")) { browser.commandTarget?.webView.reload() }.keyboardShortcut("r").disabled(browser.commandTarget == nil)
                Button(L("Zurück")) { browser.commandTarget?.webView.goBack() }.keyboardShortcut("[", modifiers: .command).disabled(browser.commandTarget == nil)
                Button(L("Vorwärts")) { browser.commandTarget?.webView.goForward() }.keyboardShortcut("]", modifiers: .command).disabled(browser.commandTarget == nil)
                Button(L("Link kopieren", "Copy link")) { browser.copyLink(browser.commandTarget) }
                    .keyboardShortcut("c", modifiers: [.command, .shift]).disabled(browser.commandTarget?.url.isEmpty != false)
                Divider()
                Button(L("Seite vergrößern", "Zoom in")) { browser.changeZoom(1) }
                    .keyboardShortcut("+", modifiers: .command).disabled(browser.commandTarget == nil)
                Button(L("Seite verkleinern", "Zoom out")) { browser.changeZoom(-1) }
                    .keyboardShortcut("-", modifiers: .command).disabled(browser.commandTarget == nil)
                Button(L("Originalgröße", "Actual size")) { browser.changeZoom(0) }
                    .keyboardShortcut("0", modifiers: .command).disabled(browser.commandTarget == nil)
                Button(L("Seite drucken …", "Print page …")) { browser.printActivePage() }
                    .keyboardShortcut("p", modifiers: .command).disabled(browser.commandTarget?.url.isEmpty != false)
                Button(L("Seite merken", "Bookmark page")) { browser.bookmarkActivePage() }
                    .keyboardShortcut("d", modifiers: .command).disabled(browser.commandTarget?.url.isEmpty != false)
                Button(L("Lesezeichen", "Bookmarks")) { browser.librarySection = .bookmarks }
                    .keyboardShortcut("b", modifiers: [.command, .option])
                Button(L("Assistent", "Assistant")) { browser.showAgent.toggle() }.keyboardShortcut("a", modifiers: [.command, .shift])
                Button(L("Mail", "Mail")) { browser.presentMail(!browser.showMail) }.keyboardShortcut("m", modifiers: [.command, .shift])
                Button(L("Kontrollzentrum", "Control Center")) { browser.showControlCenter = true }.keyboardShortcut("p", modifiers: [.command, .option])
                Button(L("Splitview", "Split view")) { browser.toggleSplit() }.keyboardShortcut("s", modifiers: [.command, .shift])
                Divider()
                // ⌃⇥ and ⌃⇧⇥ do the same; see AppDelegate.
                Button(L("Nächster Tab", "Next tab")) { browser.selectAdjacentTab(1) }.keyboardShortcut("]", modifiers: [.command, .shift])
                Button(L("Vorheriger Tab", "Previous tab")) { browser.selectAdjacentTab(-1) }.keyboardShortcut("[", modifiers: [.command, .shift])
                Button(L("Nächster Space", "Next space")) { browser.cycleSpace(1) }.keyboardShortcut(.rightArrow, modifiers: [.command, .option])
                Button(L("Vorheriger Space", "Previous space")) { browser.cycleSpace(-1) }.keyboardShortcut(.leftArrow, modifiers: [.command, .option])
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

    /// In the tabs-on-top layout ⌘S shows and hides the tab strip.
    private var sidebarToggleTitle: String {
        if browser.tabsOnTop { return browser.showSidebar ? L("Tableiste ausblenden", "Hide tab bar") : L("Tableiste einblenden", "Show tab bar") }
        return browser.showSidebar ? L("Seitenleiste einklappen") : L("Seitenleiste einblenden")
    }

    private func openSettings(_ section: String) {
        browser.settingsSection = section
        browser.showSettings = true
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    weak var browser: BrowserModel?
    private var tabKeyMonitor: Any?
    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .sound])
    }
    /// A new-mail alert opens Mail on its account, in the profile that is open now.
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        if let value = response.notification.request.content.userInfo["account"] as? String, let id = UUID(uuidString: value) {
            Task { @MainActor in
                guard let browser = self.browser, browser.isProfileActive, browser.mail.accounts.contains(where: { $0.id == id }) else { return }
                browser.presentMail()
                browser.mail.selectFolder(account: id)
                NSApp.activate(ignoringOtherApps: true)
            }
        }
        completionHandler()
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Notification Center needs an app bundle; a bare test binary has none.
        if Bundle.main.bundleIdentifier != nil { UNUserNotificationCenter.current().delegate = self }
        // Test only: never change the user's global macOS appearance.
        if ProcessInfo.processInfo.environment["YOBRO_DEV_CAPTURE"] == "1",
           let mode = ProcessInfo.processInfo.environment["YOBRO_TEST_APPEARANCE"] {
            NSApp.appearance = NSAppearance(named: mode == "dark" ? .darkAqua : .aqua)
        }
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        // YoBro has its own tabs; macOS window tabs only added "Show Tab Bar".
        NSWindow.allowsAutomaticWindowTabbing = false
        // A menu item has one shortcut, and the tab items already use ⇧⌘] and ⇧⌘[.
        tabKeyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            self?.cycleTabs(with: event) == true ? nil : event
        }
    }
    /// ⌃⇥ and ⌃⇧⇥ switch tabs, but only in the browser window itself and
    /// never while a sheet or popover has the keyboard.
    @MainActor private func cycleTabs(with event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask).subtracting([.capsLock, .numericPad, .function])
        guard event.keyCode == 48, modifiers == .control || modifiers == [.control, .shift],
              let browser, let window = NSApp.keyWindow, window === NSApp.mainWindow,
              window.attachedSheet == nil, window.sheetParent == nil else { return false }
        browser.selectAdjacentTab(modifiers.contains(.shift) ? -1 : 1)
        return true
    }
    func applicationWillTerminate(_ notification: Notification) {
        if let tabKeyMonitor { NSEvent.removeMonitor(tabKeyMonitor); self.tabKeyMonitor = nil }
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
            // We handled the reopen. Returning true also asks SwiftUI to show
            // the Window scene again, potentially recreating the view graph.
            return false
        }
        return true
    }
}
