import SwiftUI
import AppKit
import UserNotifications

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
                    browser.startBridge(); browser.mail.startPolling()
                    browser.showOnboarding = !OnboardingProgress.isComplete(home: browser.home)
                    delegate.mail = browser.mail
                    delegate.browser = browser
                    browser.mail.notificationOpened = { [weak model = browser] account in guard let model, model.isProfileActive else { return }; model.presentMail(); model.mail.selectFolder(account: account) }
                }
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in profiles.persistAll() }
                .onDisappear {
                    browser.pauseAllMediaPlayback()
                    browser.persistSession()
                }
                .onOpenURL { url in
                    if ["http", "https"].contains(url.scheme?.lowercased() ?? "") {
                        browser.newTab(url: url.absoluteString)
                        NSApp.activate(ignoringOtherApps: true)
                    } else {
                        Task { await browser.sync.handleAuthURL(url, model: browser) }
                    }
                }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1360, height: 880)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button(L("Neuer Tab")) { browser.requestNewTab() }.keyboardShortcut("t")
                Button(L("Neue Notiz", "New note")) { browser.newNote() }.keyboardShortcut("n", modifiers: [.command])
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
                Button("YoBro Mail") { browser.presentMail(!browser.showMail) }.keyboardShortcut("m", modifiers: [.command, .shift])
                Button(L("Adresse öffnen")) {
                    browser.focusAddress = true
                }.keyboardShortcut("l")
                Button(L("Schnellsuche")) { browser.showPalette = true }.keyboardShortcut("k")
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
                Button(L("Agentenpanel")) { browser.showAgent.toggle() }.keyboardShortcut("a", modifiers: [.command, .shift])
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

final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    weak var mail: MailStore?
    weak var browser: BrowserModel?
    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification, withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) { completionHandler([.banner, .sound]) }
    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse, withCompletionHandler completionHandler: @escaping () -> Void) {
        if let value = response.notification.request.content.userInfo["account"] as? String, let id = UUID(uuidString: value) {
            Task { @MainActor in self.mail?.notificationOpened?(id); NSApp.activate(ignoringOtherApps: true) }
        }
        completionHandler()
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        if Bundle.main.bundleIdentifier != nil { UNUserNotificationCenter.current().delegate = self }
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
        browser?.managedVPN.shutdown()
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
        }
        return true
    }
}
