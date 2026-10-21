import SwiftUI
import WebKit
import AppKit
import UniformTypeIdentifiers

enum SidebarJumpDirection: Equatable { case up, down }

enum SidebarVisibility {
    static func jumpDirection(for frame: CGRect?, viewport: CGRect) -> SidebarJumpDirection? {
        guard let frame, !viewport.isNull, viewport.height > 0 else { return nil }
        if frame.maxY < viewport.minY + 6 { return .up }
        if frame.minY > viewport.maxY - 6 { return .down }
        return nil
    }

    static func inferredJumpDirection(activeIndex: Int?, visibleIndices: [Int]) -> SidebarJumpDirection? {
        guard let activeIndex, let first = visibleIndices.min(), let last = visibleIndices.max() else { return nil }
        if activeIndex < first { return .up }
        if activeIndex > last { return .down }
        return nil
    }
}

/// Renders the four web dialog kinds on the shared dialog chrome. `prompt` and
/// `credentials` need input fields, so the state lives here rather than in the
/// model, and the view is re-created per dialog id.
private struct WebPageDialogOverlay: View {
    @ObservedObject var model: BrowserModel
    let dialog: WebPageDialog
    @State private var text = ""
    @State private var password = ""
    @FocusState private var focused: Bool

    private var icon: String {
        switch dialog.kind {
        case .alert: return "info.circle.fill"
        case .confirm: return "questionmark.bubble.fill"
        case .prompt: return "character.cursor.ibeam"
        case .credentials: return "person.badge.key.fill"
        }
    }

    private var cancelTitle: String? {
        dialog.kind == .alert ? nil : L("Abbrechen")
    }

    private var confirmTitle: String {
        if dialog.destructive { return L("Trotzdem fortfahren", "Continue anyway") }
        switch dialog.kind {
        case .alert, .confirm, .prompt: return "OK"
        case .credentials: return L("Anmelden", "Sign in")
        }
    }

    var body: some View {
        YOBRODialogOverlay(
            icon: dialog.destructive ? "lock.trianglebadge.exclamationmark" : icon,
            title: dialog.title,
            message: dialog.message,
            confirmTitle: confirmTitle,
            cancelTitle: cancelTitle,
            destructive: dialog.destructive,
            confirmDisabled: dialog.kind == .credentials && text.isEmpty,
            confirm: { model.resolveWebPageDialog(true, text: text, password: password) },
            cancel: { model.resolveWebPageDialog(false) }
        ) {
            switch dialog.kind {
            case .alert, .confirm:
                EmptyView()
            case .prompt:
                TextField("", text: $text)
                    .textFieldStyle(YOBROTextFieldStyle())
                    .focused($focused)
                    .accessibilityLabel(dialog.message.isEmpty ? L("Eingabe", "Input") : dialog.message)
            case .credentials:
                VStack(alignment: .leading, spacing: 8) {
                    TextField(L("Benutzername", "User name"), text: $text)
                        .textFieldStyle(YOBROTextFieldStyle())
                        .focused($focused)
                    SecureField(L("Passwort", "Password"), text: $password)
                        .textFieldStyle(YOBROTextFieldStyle())
                }
            }
        }
        .appearanceTask {
            text = dialog.defaultText
            await Task.yield()
            focused = dialog.kind == .prompt || dialog.kind == .credentials
        }
    }
}

private struct SidebarTabFrameKey: PreferenceKey {
    static var defaultValue: [UUID: CGRect] = [:]
    static func reduce(value: inout [UUID: CGRect], nextValue: () -> [UUID: CGRect]) {
        value.merge(nextValue(), uniquingKeysWith: { _, latest in latest })
    }
}

private struct SidebarViewportFrameKey: PreferenceKey {
    static var defaultValue: CGRect = .null
    static func reduce(value: inout CGRect, nextValue: () -> CGRect) { value = nextValue() }
}

/// SwiftUI geometry preferences are not guaranteed to publish again when only
/// an AppKit-backed ScrollView's clip bounds move. Observe those bounds so the
/// active-tab visibility calculation is refreshed during manual scrolling.
private struct SidebarScrollObserver: NSViewRepresentable {
    let changed: () -> Void
    var connected: ((NSScrollView) -> Void)? = nil

    func makeNSView(context: Context) -> ObservationView {
        let view = ObservationView()
        view.changed = changed
        view.connected = connected
        return view
    }

    func updateNSView(_ nsView: ObservationView, context: Context) {
        nsView.changed = changed
        nsView.connected = connected
        nsView.attachIfNeeded()
    }

    final class ObservationView: NSView {
        var changed: (() -> Void)?
        var connected: ((NSScrollView) -> Void)?
        private weak var clipView: NSClipView?
        private var observer: NSObjectProtocol?

        override func viewDidMoveToSuperview() {
            super.viewDidMoveToSuperview()
            DispatchQueue.main.async { [weak self] in self?.attachIfNeeded() }
        }

        func attachIfNeeded() {
            guard clipView == nil else { return }
            var ancestor = superview
            while let view = ancestor, !(view is NSScrollView) { ancestor = view.superview }
            guard let scrollView = ancestor as? NSScrollView else { return }
            connected?(scrollView)
            let clip = scrollView.contentView
            clip.postsBoundsChangedNotifications = true
            clipView = clip
            observer = NotificationCenter.default.addObserver(
                forName: NSView.boundsDidChangeNotification,
                object: clip,
                queue: .main
            ) { [weak self] _ in
                DispatchQueue.main.async { self?.changed?() }
            }
        }

        deinit {
            if let observer { NotificationCenter.default.removeObserver(observer) }
        }
    }
}

/// SwiftUI's `onDrag` has no cancellation callback on macOS. Observing mouse-up
/// ensures an aborted drop cannot leave a tab translucent or pin the overlay.
private struct SidebarDragEndObserver: NSViewRepresentable {
    let model: BrowserModel

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        view.isHidden = true
        return view
    }
    func updateNSView(_ nsView: NSView, context: Context) { context.coordinator.model = model }

    final class Coordinator {
        weak var model: BrowserModel?
        private var localMonitor: Any?
        private var globalMonitor: Any?

        init(model: BrowserModel) {
            self.model = model
            localMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseUp, .keyDown]) { [weak self] event in
                if event.type == .keyDown {
                    if event.keyCode == 53 { MainActor.assumeIsolated { self?.model?.finishSidebarDrag() } }
                } else if event.type == .leftMouseUp {
                    self?.finishAfterCurrentEvent()
                }
                return event
            }
            globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseUp) { [weak self] _ in
                self?.finishAfterCurrentEvent()
            }
        }

        private func finishAfterCurrentEvent() {
            DispatchQueue.main.async { [weak self] in
                guard let model = self?.model else { return }
                guard let sessionID = model.sidebarDragSessionID else { return }
                // AppKit can deliver mouse-up before SwiftUI calls performDrop,
                // particularly for an overlay above a WKWebView. Give the drop
                // destination time to finish before clearing an aborted drag.
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
                    self?.model?.finishSidebarDrag(sessionID: sessionID)
                }
            }
        }

        deinit {
            if let localMonitor { NSEvent.removeMonitor(localMonitor) }
            if let globalMonitor { NSEvent.removeMonitor(globalMonitor) }
        }
    }
}

/// The window: a sidebar on the Space's tone, pages as white cards beside it,
/// and the assistant as a card on the right.
struct BrowserShell: View {
    @ObservedObject var model: BrowserModel
    @State private var sidebarOverlayCloseTask: Task<Void, Never>?
    @State private var deletingSpace: String?
    @State private var stripPopoverOpen = false

    private var tone: SpaceTone { model.spaceTone(for: model.space) }

    /// With tabs on top, the strip replaces the sidebar; ⌘S hides either.
    private var showsTopStrip: Bool { model.tabsOnTop && model.showSidebar }
    private var showsSidebar: Bool { !model.tabsOnTop && model.showSidebar }
    private var contentTopInset: CGFloat { showsTopStrip ? 6 : Chrome.inset }
    private var dialogShown: Bool { model.webPageDialog != nil || model.notice != nil || deletingSpace != nil }

    var body: some View {
        VStack(spacing: 0) {
            if showsTopStrip {
                TopTabStrip(model: model, requestDelete: { deletingSpace = $0 })
                    .padding(.leading, 96).padding(.trailing, Chrome.inset)
                    .frame(height: 28)
                    .padding(.top, 6)
            }
            HStack(spacing: 0) {
                if showsSidebar {
                    ShellSidebar(model: model, requestDelete: { deletingSpace = $0 }).frame(width: Chrome.sidebarWidth)
                }
                workspace
                    .overlay {
                        if let peek = model.peekTab { PeekOverlay(tab: peek, model: model) }
                    }
                    // Toasts stay inside the page area, clear of the assistant.
                    .overlay(alignment: .bottomTrailing) { notifications }
                    .padding(.top, contentTopInset)
                    .padding(.bottom, Chrome.inset)
                    .padding(.leading, showsSidebar ? 0 : Chrome.inset)
                    .padding(.trailing, Chrome.inset)
                    .frame(minWidth: 280)
                if model.showAgent {
                    SpaceChatPanel(browser: model, chat: model.chat)
                        .frame(width: 340)
                        .pageCard()
                        .padding(.top, contentTopInset)
                        .padding(.bottom, Chrome.inset)
                        .padding(.trailing, Chrome.inset)
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
        }
        .ignoresSafeArea(.container, edges: .top)
        .overlay(alignment: .leading) {
            if !model.showSidebar && !model.tabsOnTop { hiddenSidebar }
        }
        .overlay(alignment: .top) {
            if !model.showSidebar && model.tabsOnTop { hiddenTopStrip }
        }
        // Keep the single native macOS traffic-light host above the temporary
        // sidebar overlay as well as the ordinary browser chrome.
        .overlay(alignment: .topLeading) {
            WindowControls()
                .frame(width: 60, height: 32)
                .padding(.leading, 18)
                .padding(.top, model.tabsOnTop ? 4 : 10)
                .opacity(sidebarChromeVisible ? 1 : 0)
                .allowsHitTesting(sidebarChromeVisible)
                .accessibilityHidden(!sidebarChromeVisible)
                .ignoresSafeArea(.container, edges: .top)
        }
        .background(YOBROArrowCursorRegion().ignoresSafeArea())
        // The drag view sits above the fill, so every empty stretch of chrome
        // moves the window.
        .background(ZStack { tone.background; ChromeGrain(); WindowDragArea() }.ignoresSafeArea())
        .overlay { AgentActivityFrame(model: model, chat: model.chat) }
        .overlay {
            if let mode = model.commandBar {
                ZStack(alignment: .top) {
                    Color.black.opacity(0.06).ignoresSafeArea()
                        .onTapGesture { model.commandBar = nil }
                    CommandBar(model: model, mode: mode)
                        .padding(.top, 120)
                }
                .transition(.opacity)
            }
        }
        // VoiceOver stays inside an open dialog.
        .accessibilityHidden(dialogShown)
        .overlay {
            if let dialog = model.webPageDialog {
                WebPageDialogOverlay(model: model, dialog: dialog).id(dialog.id)
            } else if let notice = model.notice {
                YOBRODialogOverlay(icon: "info.circle.fill", title: "YoBro", message: notice, confirmTitle: "OK", confirm: { model.notice = nil })
            } else if let name = deletingSpace {
                YOBRODialogOverlay(
                    icon: "trash",
                    title: L("„\(name)“ löschen?", "Delete “\(name)”?"),
                    message: deleteSpaceMessage(name),
                    confirmTitle: L("Space löschen", "Delete space"),
                    cancelTitle: L("Abbrechen"),
                    destructive: true,
                    confirm: {
                        deletingSpace = nil
                        model.removeSpace(name)
                    },
                    cancel: { deletingSpace = nil }
                )
            }
        }
        .environment(\.spaceTone, tone)
        .foregroundStyle(ink)
        .environment(\.locale, YOBROLanguage.locale)
        .animation(.easeOut(duration: 0.45), value: tone)
        .animation(.easeInOut(duration: 0.2), value: model.showAgent)
        .animation(.easeInOut(duration: 0.2), value: model.showSidebar)
        .animation(.easeInOut(duration: 0.25), value: model.tabsOnTop)
        .animation(.easeOut(duration: 0.2), value: model.peekTab?.id)
        .animation(.easeOut(duration: 0.12), value: model.commandBar)
        .animation(.easeInOut(duration: 0.2), value: model.agentWorkspaceVisible)
        .onDisappear { sidebarOverlayCloseTask?.cancel() }
        .onChange(of: model.showSidebar) { _, _ in
            sidebarOverlayCloseTask?.cancel()
            model.sidebarOverlayVisible = false
            stripPopoverOpen = false
        }
        .onChange(of: model.tabsOnTop) { _, _ in
            sidebarOverlayCloseTask?.cancel()
            sidebarOverlayCloseTask = nil
            model.sidebarOverlayVisible = false
            stripPopoverOpen = false
        }
        .onChange(of: model.showControlCenter) { _, on in
            // Without a page the tab-strip layout has no Control Center to
            // open; never leave the request pending.
            if on && model.tabsOnTop && model.active == nil { model.showControlCenter = false }
        }
        .onChange(of: model.spaces) { _, spaces in
            if let name = deletingSpace, !spaces.contains(name) { deletingSpace = nil }
        }
        .onChange(of: model.extensionActionPopupPresented) { _, presented in
            if !presented && !pointerIsInsideSidebarOverlay() { scheduleSidebarOverlayClose() }
        }
        .onChange(of: model.hoveredFolderID) { _, folder in
            if folder == nil && !pointerIsInsideSidebarOverlay() { scheduleSidebarOverlayClose() }
        }
        .onChange(of: model.draggingTabID) { _, _ in sidebarDragStateChanged() }
        .onChange(of: model.draggingFolderID) { _, _ in sidebarDragStateChanged() }
        // A Bool presentation keeps the sheet up while the menu switches its
        // section; `sheet(item:)` would dismiss and present it again.
        .sheet(isPresented: Binding(get: { model.librarySection != nil }, set: { if !$0 { model.librarySection = nil } })) {
            LibraryView(model: model, downloads: model.downloads, section: model.librarySection ?? .history)
        }
        .sheet(isPresented: $model.showOnboarding) { OnboardingView(model: model) }
        .sheet(isPresented: $model.showSettings) { BrowserSettings(model: model, section: model.settingsSection) }
        .sheet(isPresented: $model.showProfileEditor) {
            if let profiles = model.profileSession { ProfileEditor(profiles: profiles, creating: model.creatingProfile) }
        }
        .background(SidebarDragEndObserver(model: model))
    }

    /// The hidden sidebar slides in over the page from the left edge.
    private var hiddenSidebar: some View {
        ZStack(alignment: .leading) {
            let isVisible = model.sidebarOverlayVisible || model.draggingTabID != nil || model.draggingFolderID != nil
            // The overlay is hosted in its own NSHostingView, which does not
            // inherit this view's environment.
            SidebarOverlayHost(visible: isVisible) {
                ShellSidebar(model: model, requestDelete: { deletingSpace = $0 })
                    .frame(width: Chrome.sidebarWidth)
                    .frame(maxHeight: .infinity)
                    .background {
                        SidebarOverlayShape(radius: 14).fill(tone.background)
                            .shadow(color: .black.opacity(0.22), radius: 18, x: 4)
                    }
                    .environment(\.spaceTone, tone)
                    .environment(\.locale, YOBROLanguage.locale)
                    .foregroundStyle(ink)
            }
            .frame(width: Chrome.sidebarWidth + 20)
            .frame(maxHeight: .infinity)
            .onHover { inside in
                guard model.sidebarOverlayVisible else { return }
                if inside { revealSidebarOverlay() }
                else { scheduleSidebarOverlayClose() }
            }
            if !isVisible {
                // The edge trigger stays narrow so the page stays interactive
                // right beside it.
                Color.clear
                    .frame(width: 8)
                    .frame(maxHeight: .infinity)
                    .contentShape(Rectangle())
                    .onHover { inside in
                        if inside { revealSidebarOverlay() }
                    }
            }
        }
        .frame(width: Chrome.sidebarWidth + 20)
        .ignoresSafeArea(.container, edges: .top)
    }

    /// With tabs on top and the strip hidden, the strip drops in over the
    /// page when the pointer reaches the top edge.
    private var hiddenTopStrip: some View {
        let visible = model.sidebarOverlayVisible
        return ZStack(alignment: .top) {
            // Stays mounted while hidden, so a menu choice made as the strip
            // slides away still reaches it.
            TopTabStrip(model: model, requestDelete: { deletingSpace = $0 })
                .padding(.leading, 96).padding(.trailing, Chrome.inset)
                .frame(height: 28)
                .padding(.vertical, 6)
                .frame(maxWidth: .infinity)
                .background {
                    tone.background.overlay(ChromeGrain())
                        .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
                }
                .offset(y: visible ? 0 : -48)
                .opacity(visible ? 1 : 0)
                .allowsHitTesting(visible)
                .accessibilityHidden(!visible)
                .onHover { inside in
                    if inside { revealSidebarOverlay() }
                    else { scheduleSidebarOverlayClose() }
                }
            if !visible {
                // Narrow, like the left-edge trigger, so the page stays usable.
                Color.clear
                    .frame(maxWidth: .infinity)
                    .frame(height: 8)
                    .contentShape(Rectangle())
                    .onHover { inside in
                        if inside { revealSidebarOverlay() }
                    }
            }
        }
        .animation(.easeOut(duration: 0.15), value: visible)
        .onPreferenceChange(StripPopoverKey.self) { open in
            stripPopoverOpen = open
            if open { revealSidebarOverlay() }
            else if !pointerIsInsideSidebarOverlay() { scheduleSidebarOverlayClose() }
        }
        .ignoresSafeArea(.container, edges: .top)
    }

    /// Bottom-right stack over the page: save-login prompt, confirmations,
    /// closed-tab undo and the running download.
    private var notifications: some View {
        VStack(alignment: .trailing, spacing: 8) {
            if let login = model.commandTarget?.logins { LoginSavePrompt(login: login) }
            if let toast = model.toast {
                InfoToast(model: model, toast: toast)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            if let undoID = model.closedTabUndoID {
                ClosedTabUndo(model: model, undoID: undoID)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            // Keep this observer mounted even while there are no downloads so
            // the first download can make the status appear immediately.
            ActiveDownloadStatus(model: model, downloads: model.downloads)
        }
        .padding(.trailing, 14).padding(.bottom, 14)
        .animation(.easeOut(duration: 0.2), value: model.toast)
        .animation(.easeOut(duration: 0.2), value: model.closedTabUndoID)
    }

    private func deleteSpaceMessage(_ name: String) -> String {
        let tabs = model.tabs.filter { $0.space == name && !model.agentTabIDs.contains($0.id) }.count
        let folders = model.folders.filter { $0.space == name }.count
        let tabText = tabs == 1 ? L("1 Tab", "1 tab") : L("\(tabs) Tabs", "\(tabs) tabs")
        let folderText = folders == 1 ? L("1 Ordner", "1 folder") : L("\(folders) Ordner", "\(folders) folders")
        return L("\(tabText) und \(folderText) werden geschlossen. Die Tabs lassen sich mit ⇧⌘T wieder öffnen.",
                 "\(tabText) and \(folderText) will be closed. You can reopen the tabs with ⇧⌘T.")
    }

    @ViewBuilder
    private var workspace: some View {
        if model.agentWorkspaceVisible, let agent = model.agentTab {
            // The agent owns the right half, so a user split collapses to its
            // active page instead of squeezing three pages side by side.
            HSplitView {
                singlePage
                    .padding(.trailing, Chrome.inset / 2)
                    .frame(minWidth: 280, maxWidth: .infinity)
                    .layoutPriority(1)
                ShellPane(tab: agent, model: model, role: .agent)
                    .padding(.leading, Chrome.inset / 2)
                    .frame(minWidth: 294, idealWidth: 380)
                    .accessibilityIdentifier("agent-workspace")
            }
        } else {
            userWorkspace
        }
    }

    @ViewBuilder
    private var userWorkspace: some View {
        if let active = model.activeID, let pair = model.splitPairs.first(where: { $0.contains(active) }),
           let first = model.tabs.first(where: { $0.id == pair.first }), let second = model.tabs.first(where: { $0.id == pair.second }) {
            HSplitView {
                ShellPane(tab: first, model: model, role: .split)
                    .padding(.trailing, Chrome.inset / 2)
                    .frame(minWidth: 180)
                ShellPane(tab: second, model: model, role: .split)
                    .padding(.leading, Chrome.inset / 2)
                    .frame(minWidth: 180)
            }
        } else {
            singlePage
        }
    }

    @ViewBuilder
    private var singlePage: some View {
        if let tab = model.active, model.tabsOnTop {
            VStack(spacing: 0) {
                PageBand(tab: tab, model: model)
                Divider().opacity(0.5)
                TabContent(tab: tab, model: model)
            }
            .id(tab.id)
            .background(paper)
            .pageCard()
        } else if let tab = model.active {
            TabContent(tab: tab, model: model).id(tab.id).pageCard()
        } else {
            StartPage(model: model).pageCard()
        }
    }

    private var sidebarChromeVisible: Bool {
        model.showSidebar || model.sidebarOverlayVisible
    }

    private func revealSidebarOverlay() {
        sidebarOverlayCloseTask?.cancel()
        sidebarOverlayCloseTask = nil
        model.sidebarOverlayVisible = true
    }

    private func scheduleSidebarOverlayClose() {
        guard !sidebarDragIsActive else {
            revealSidebarOverlay()
            return
        }
        sidebarOverlayCloseTask?.cancel()
        sidebarOverlayCloseTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 220_000_000)
            guard !Task.isCancelled else { return }
            guard !sidebarDragIsActive else {
                revealSidebarOverlay()
                return
            }
            // SwiftUI can briefly emit `false` while the pointer crosses child
            // controls. Verify the real pointer position before closing.
            if pointerIsInsideSidebarOverlay() {
                revealSidebarOverlay()
                return
            }
            guard model.hoveredFolderID == nil, !model.extensionActionPopupPresented, !stripPopoverOpen else { return }
            model.sidebarOverlayVisible = false
        }
    }

    private var sidebarDragIsActive: Bool {
        model.draggingTabID != nil || model.draggingFolderID != nil
    }

    private func pointerIsInsideSidebarOverlay() -> Bool {
        guard let window = NSApp.keyWindow else { return false }
        let point = window.mouseLocationOutsideOfEventStream
        if model.tabsOnTop {
            // The strip covers the top 40 pt; window coordinates start at the bottom.
            guard let height = window.contentView?.bounds.height else { return false }
            return point.x >= 0 && point.x <= window.frame.width && point.y >= height - 40 && point.y <= height
        }
        return point.x >= 0 && point.x <= Chrome.sidebarWidth && point.y >= 0 && point.y <= window.frame.height
    }

    private func sidebarDragStateChanged() {
        guard !model.showSidebar else { return }
        if sidebarDragIsActive {
            model.hoveredFolderID = nil
            revealSidebarOverlay()
        } else if !pointerIsInsideSidebarOverlay() {
            scheduleSidebarOverlayClose()
        }
    }
}

/// A page shown beside another one: a split-view half or the agent's tab.
/// YAB gives each half its own slim title band.
private struct ShellPane: View {
    enum Role { case split, agent }
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    let role: Role

    private var focused: Bool { role == .split && model.activeID == tab.id }

    var body: some View {
        VStack(spacing: 0) {
            if role == .split && model.tabsOnTop {
                // No sidebar to navigate from: each half gets the full band.
                PageBand(tab: tab, model: model, onFocus: { model.activateSplitPane(tab.id) }) {
                    Button { model.separateSplit(tab.id) } label: { Image(systemName: "rectangle").font(.system(size: 14)).frame(width: 30, height: 30) }
                        .buttonStyle(YOBROButtonStyle(minimumSize: 30))
                        .yobroIconHelp(L("Splitview trennen · beide Tabs behalten"), label: L("Splitview trennen"))
                }
            } else {
                band
            }
            Divider().opacity(0.5)
            TabContent(tab: tab, model: model).id(tab.id).allowsHitTesting(role != .agent)
        }
        .background(paper)
        .pageCard()
        .overlay {
            RoundedRectangle(cornerRadius: Chrome.radius, style: .continuous)
                .strokeBorder(focused ? ink.opacity(0.3) : .clear, lineWidth: 1)
                .allowsHitTesting(false)
        }
    }

    private var band: some View {
        HStack(spacing: 8) {
            if role == .agent {
                Circle().fill(brandOrange).frame(width: 7, height: 7)
                Text(L("Assistent", "Assistant")).font(.system(size: 12, weight: .semibold)).foregroundStyle(brandOrange)
            }
            Button { if role == .split { model.activateSplitPane(tab.id) } } label: {
                HStack(spacing: 6) {
                    TabFavicon(tab: tab, size: 14)
                    Text(tab.sidebarTitle).font(.system(size: 12.5, weight: .medium)).lineLimit(1)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .yobroHelp(role == .split ? L("Diese Seite aktivieren · ⌘R lädt sie neu", "Activate this page · ⌘R reloads it") : tab.url)
            if role == .split {
                Button { model.separateSplit(tab.id) } label: { Image(systemName: "rectangle") }
                    .yobroIconHelp(L("Splitview trennen · beide Tabs behalten"), label: L("Splitview trennen"))
            } else {
                Button { model.closeAgentWorkspace() } label: { Image(systemName: "xmark") }
                    .yobroIconHelp(L("Assistenten-Tab schließen", "Close assistant tab"))
            }
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 28))
        .font(.system(size: 12))
        .foregroundStyle(ink.opacity(0.75))
        .padding(.leading, 12).padding(.trailing, 4)
        .frame(height: 36)
        .background(paper)
    }
}

// MARK: - Sidebar

private enum SidebarEditor: Equatable {
    /// `newFolder` carries the Space the folder belongs to.
    case newSpace, renameSpace(String), newFolder(String)
}

/// The name popover for new and renamed Spaces and new folders, shared by the
/// sidebar and the tab strip.
private struct SpaceEditorModifier: ViewModifier {
    @Binding var editor: SidebarEditor?
    @ObservedObject var model: BrowserModel
    let arrowEdge: Edge
    @State private var text = ""

    func body(content: Content) -> some View {
        content
            .popover(isPresented: Binding(get: { editor != nil }, set: { if !$0 { editor = nil } }), arrowEdge: arrowEdge) { editorView }
            .onChange(of: editor) { _, value in
                if case .renameSpace(let name) = value { text = name } else if value != nil { text = "" }
            }
    }

    @ViewBuilder
    private var editorView: some View {
        switch editor {
        case .renameSpace(let original):
            YOBRONameEditor(icon: model.spaceIcon(for: original).rawValue, title: L("Space umbenennen", "Rename space"),
                            detail: L("Gib deinem Bereich einen klaren Namen.", "Give your space a clear name."),
                            text: $text, cancel: { editor = nil }, save: save)
        case .newFolder:
            YOBRONameEditor(icon: "folder.badge.plus", title: L("Neuer Ordner", "New folder"),
                            detail: L("Sammle zusammengehörige Tabs an einem Ort.", "Keep related tabs together."),
                            text: $text, cancel: { editor = nil }, save: save)
        default:
            YOBRONameEditor(icon: "square.stack.3d.up.badge.plus", title: L("Neuer Space", "New space"),
                            detail: L("Gib deinem Bereich einen klaren Namen.", "Give your space a clear name."),
                            text: $text, cancel: { editor = nil }, save: save)
        }
    }

    private func save() {
        do {
            switch editor {
            case .renameSpace(let original): try model.renameSpace(original, to: text)
            case .newFolder(let target):
                try model.addFolder(text, in: target)
                // Show the new folder where it was created.
                if target != model.space { model.switchSpace(target) }
            case .newSpace, nil: try model.addSpace(text)
            }
            editor = nil
        } catch { model.notice = error.localizedDescription }
    }
}

extension View {
    fileprivate func spaceEditor(_ editor: Binding<SidebarEditor?>, model: BrowserModel, arrowEdge: Edge) -> some View {
        modifier(SpaceEditorModifier(editor: editor, model: model, arrowEdge: arrowEdge))
    }
}

private struct ShellSidebar: View {
    @ObservedObject var model: BrowserModel
    let requestDelete: (String) -> Void
    @State private var editor: SidebarEditor?

    /// The pin area also appears while a pinnable tab is dragged, so the first
    /// pin can be made by dropping.
    private var showsPins: Bool {
        if model.visibleTabs.contains(where: \.pinned) { return true }
        guard let id = model.draggingTabID else { return false }
        return model.visibleTabs.contains { $0.id == id && !$0.isPrivate }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 5) {
                Spacer(minLength: 0)
                if let tab = model.active { SidebarHistoryControls(tab: tab) }
                else { EmptySidebarHistoryControls() }
            }
            .frame(height: 30)
            .padding(.top, 11)
            AddressPlate(model: model).padding(.top, Chrome.inset)
            if showsPins {
                PinGrid(model: model).padding(.top, Chrome.inset)
            }
            SidebarTabList(model: model, edit: { editor = $0 }, requestDelete: requestDelete)
                .padding(.top, Chrome.inset)
                .spaceEditor($editor, model: model, arrowEdge: .trailing)
            UpdateSidebarCard(updates: model.updates)
            SidebarFooter(model: model, edit: { editor = $0 }, requestDelete: requestDelete).padding(.top, 6)
        }
        .padding(.horizontal, Chrome.inset)
        .padding(.bottom, Chrome.inset)
    }
}

private struct SidebarHistoryControls: View {
    @ObservedObject var tab: BrowserTab

    var body: some View {
        HStack(spacing: 5) {
            Button { tab.webView.goBack() } label: { Image(systemName: "chevron.left") }
                .disabled(!tab.canGoBack).yobroIconHelp(L("Zurück"))
            Button { tab.webView.goForward() } label: { Image(systemName: "chevron.right") }
                .disabled(!tab.canGoForward).yobroIconHelp(L("Vorwärts"))
            Button { if tab.loading { tab.webView.stopLoading() } else { tab.webView.reload() } } label: {
                Image(systemName: tab.loading ? "xmark" : "arrow.clockwise")
            }.disabled(tab.url.isEmpty).yobroIconHelp(tab.loading ? L("Laden stoppen", "Stop loading") : L("Neu laden"))
        }
        .buttonStyle(ChromeButtonStyle())
    }
}

private struct EmptySidebarHistoryControls: View {
    var body: some View {
        HStack(spacing: 5) {
            ForEach(["chevron.left", "chevron.right", "arrow.clockwise"], id: \.self) { symbol in
                Button {} label: { Image(systemName: symbol) }.disabled(true)
            }
        }
        .buttonStyle(ChromeButtonStyle())
        .accessibilityHidden(true)
    }
}

/// Plain HTTP to a public host. Local development servers are exempt.
enum PageSecurity {
    @MainActor static func isInsecure(_ tab: BrowserTab?) -> Bool {
        guard let tab, !tab.isExtensionsHub else { return false }
        return isInsecure(url: tab.url)
    }

    static func isInsecure(url: String) -> Bool {
        guard let parsed = URL(string: url), parsed.scheme?.lowercased() == "http",
              let host = parsed.host, !host.isEmpty else { return false }
        return !CertificateTrustStore.isLocal(host)
    }
}

/// The address of the active page. Clicking it opens the command field; the
/// page actions appear on hover.
private struct AddressPlate: View {
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false
    @State private var showingActions = false
    @FocusState private var plateFocused: Bool

    /// The actions stay mounted while hidden, so the login key can still open
    /// its suggestions when a field gets focus and its popover outlives hover.
    private var revealed: Bool {
        hovering || showingActions || plateFocused
            || NSWorkspace.shared.isVoiceOverEnabled || NSApp.isFullKeyboardAccessEnabled
    }

    var body: some View {
        HStack(spacing: 0) {
            Button { model.openCommandBar(.current) } label: {
                HStack(spacing: 6) {
                    if model.active?.isPrivate == true { Image(systemName: "eyeglasses").font(.system(size: 11)) }
                    if insecure { Image(systemName: "lock.open").font(.system(size: 10)).foregroundStyle(tone.muted) }
                    Text(label).font(.system(size: 12.5)).lineLimit(1).truncationMode(.tail)
                        .foregroundStyle(hasPage ? ink : tone.muted)
                    Spacer(minLength: 0)
                }
                .padding(.leading, 10)
                .frame(maxHeight: .infinity)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .focused($plateFocused)
            .yobroHelp((insecure ? L("Nicht sicher", "Not secure") + " · " : "") + (hasPage ? model.active!.url + " · " : "") + "⌘L")
            .accessibilityLabel((insecure ? L("Nicht sicher", "Not secure") + ", " : "") + L("Adresse öffnen · ⌘L"))
            .accessibilityAction(named: Text(L("Kontrollzentrum", "Control Center"))) { showingActions = true }
            .accessibilityAction(named: Text(L("Adresse kopieren"))) { model.copyLink(model.active) }
            // A blank tab keeps the old hover-only actions, so the placeholder
            // is not cut short; the Control Center is still reachable there.
            if hasPage || revealed {
                HStack(spacing: 0) {
                    if let tab = model.active, !tab.isPrivate, hasPage {
                        LoginAutofillButton(login: tab.logins).id(tab.id).focused($plateFocused)
                    }
                    if hasPage {
                        Button { model.copyLink(model.active) } label: { Image(systemName: "link") }
                            .buttonStyle(ChromeButtonStyle(size: 28))
                            .focused($plateFocused)
                            .yobroIconHelp(L("Adresse kopieren · ⇧⌘C", "Copy address · ⇧⌘C"), label: L("Adresse kopieren"))
                    }
                    Button { showingActions.toggle() } label: { Image(systemName: "slider.horizontal.3") }
                        .buttonStyle(ChromeButtonStyle(size: 28))
                        .focused($plateFocused)
                        .yobroIconHelp(L("Kontrollzentrum · ⌥⌘P", "Control Center · ⌥⌘P"), label: L("Kontrollzentrum", "Control Center"))
                        .popover(isPresented: $showingActions, arrowEdge: .trailing) {
                            SidebarPageActions(model: model, tab: model.active, store: model.extensions) { showingActions = false }
                        }
                }
                .foregroundStyle(tone.muted)
                .padding(.trailing, 4)
                .opacity(revealed ? 1 : 0)
                .allowsHitTesting(revealed)
                .accessibilityHidden(!revealed)
                .transition(.opacity)
            }
        }
        .frame(height: Chrome.rowHeight)
        .background(hovering ? Chrome.restHover : Chrome.rest, in: RoundedRectangle(cornerRadius: Chrome.radius))
        .animation(.easeOut(duration: 0.15), value: hovering)
        .onHover { hovering = $0 }
        .onChange(of: model.showControlCenter) { _, on in
            guard on else { return }
            model.showControlCenter = false
            if !model.showSidebar && !model.tabsOnTop { model.sidebarOverlayVisible = true }
            showingActions = true
        }
    }

    private var hasPage: Bool { !(model.active?.url.isEmpty ?? true) }
    private var insecure: Bool { PageSecurity.isInsecure(model.active) }
    private var label: String { pageLabel(model.active) }
}

/// The host of a page without "www.", as YAB shows it.
@MainActor private func pageLabel(_ tab: BrowserTab?) -> String {
    guard let tab, !tab.url.isEmpty else { return L("Suchen oder Adresse eingeben", "Search or enter address") }
    if tab.isExtensionsHub { return L("Erweiterungen", "Extensions") }
    guard let host = URL(string: tab.url)?.host else { return tab.title }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
}

/// True while a popover opened from the tab strip is showing, so the hidden
/// strip does not slide away from under it.
private struct StripPopoverKey: PreferenceKey {
    static var defaultValue = false
    static func reduce(value: inout Bool, nextValue: () -> Bool) { value = value || nextValue() }
}

/// YAB's tabs-on-top layout: the Space, pinned tabs and today's tabs in one
/// strip beside the window controls.
private struct TopTabStrip: View {
    @ObservedObject var model: BrowserModel
    let requestDelete: (String) -> Void
    @Environment(\.spaceTone) private var tone
    @State private var editor: SidebarEditor?

    private var tabs: [BrowserTab] { model.visibleTabs.filter { !$0.pinned } }

    var body: some View {
        HStack(spacing: 2) {
            // The editor needs an anchor around the menu, not the menu itself.
            HStack(spacing: 0) { spaceMenu }
                .spaceEditor($editor, model: model, arrowEdge: .bottom)
                .preference(key: StripPopoverKey.self, value: editor != nil)
            ForEach(model.visibleTabs.filter(\.pinned)) { tab in TopPin(tab: tab, model: model) }
            GeometryReader { geometry in
                let available = max(0, geometry.size.width - 30)
                let width = min(186, max(36, available / CGFloat(max(1, tabs.count)) - 2))
                HStack(spacing: 2) {
                    ScrollViewReader { proxy in
                        ScrollView(.horizontal) {
                            HStack(spacing: 2) {
                                ForEach(tabs) { tab in TopTab(tab: tab, model: model).frame(width: width).id(tab.id) }
                            }
                        }
                        .scrollIndicators(.hidden)
                        .onAppear { reveal(model.activeID, using: proxy, animated: false) }
                        .onChange(of: model.activeID) { _, id in reveal(id, using: proxy, animated: true) }
                    }
                    // Tabs shrink to fit, then scroll, so + always stays in view.
                    .frame(width: min(available, CGFloat(tabs.count) * (width + 2)))
                    Button { model.openCommandBar(.newTab) } label: { Image(systemName: "plus") }
                        .buttonStyle(ChromeButtonStyle(size: 28))
                        .yobroIconHelp(L("Neuer Tab · ⌘T"), label: L("Neuer Tab", "New tab"))
                    Spacer(minLength: 0)
                }
            }
            // A gap that always moves the window, however many tabs are open.
            Color.clear.frame(width: 44)
            Button { model.showAgent.toggle() } label: {
                Image(systemName: "sparkles")
                    .overlay(alignment: .topTrailing) {
                        if model.agentUsageActive { Circle().fill(brandOrange).frame(width: 6, height: 6).offset(x: 3, y: -2) }
                    }
            }
            .buttonStyle(ChromeButtonStyle(size: 28))
            .yobroIconHelp(L("Assistent · ⇧⌘A", "Assistant · ⇧⌘A"), label: L("Assistent", "Assistant"))
            .accessibilityAddTraits(model.showAgent ? .isSelected : [])
            Menu { LibraryMenuItems(model: model) } label: { Image(systemName: "square.grid.2x2") }
                .menuStyle(.button).buttonStyle(ChromeButtonStyle(size: 28)).menuIndicator(.hidden).fixedSize()
                .yobroIconHelp(L("Bibliothek und Einstellungen", "Library and settings"))
        }
    }

    private var spaceMenu: some View {
        Menu {
            ForEach(model.spaces, id: \.self) { space in
                if space != model.space {
                    Button { model.switchSpace(space) } label: { Label(space, systemImage: model.spaceIcon(for: space).rawValue) }
                }
            }
            if model.spaces.count > 1 { Divider() }
            Button(L("Tabs leeren", "Clear tabs")) { model.clearTodayTabs() }
                .disabled(!model.visibleTabs.contains { !$0.pinned && $0.folderID == nil })
            Divider()
            SpaceMenuItems(model: model, space: model.space, edit: { editor = $0 }, requestDelete: requestDelete, showsFolderActions: false)
        } label: {
            HStack(spacing: 6) {
                Image(systemName: model.spaceIcon(for: model.space).rawValue).font(.system(size: 11))
                Text(model.space).font(.system(size: 12.5, weight: .medium)).lineLimit(1)
                Image(systemName: "chevron.down").font(.system(size: 8, weight: .bold))
            }
            .foregroundStyle(tone.muted)
            .padding(.horizontal, 8).frame(height: 28)
            .contentShape(Rectangle())
        }
        .menuStyle(.button).buttonStyle(.plain).menuIndicator(.hidden).fixedSize()
        .yobroHelp(L("Space wechseln", "Switch space"))
    }

    private func reveal(_ id: UUID?, using proxy: ScrollViewProxy, animated: Bool) {
        guard let id, tabs.contains(where: { $0.id == id }) else { return }
        Task { @MainActor in
            await Task.yield()
            if animated { withAnimation(.easeInOut(duration: 0.2)) { proxy.scrollTo(id) } }
            else { proxy.scrollTo(id) }
        }
    }
}

private struct TopPin: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false

    var body: some View {
        Button { model.select(tab.id) } label: {
            TabFavicon(tab: tab)
                .opacity(tab.isSuspended ? 0.6 : 1)
                .overlay(alignment: .topTrailing) { CaptureIndicator(tab: tab) }
                .foregroundStyle(tone.muted)
                .frame(width: 30, height: 28)
                .background(tab.id == model.activeID ? Chrome.wash : hovering ? Chrome.restHover : Chrome.rest, in: RoundedRectangle(cornerRadius: Chrome.radius))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .contextMenu {
            Button(L("Loslösen")) { tab.pinned = false; model.save() }
            Button(L("Link kopieren", "Copy link")) { model.copyLink(tab) }.disabled(tab.url.isEmpty || tab.isExtensionsHub)
            Divider()
            // ⌘W only unloads a pin; deleting it is explicit.
            Button(L("Tab löschen", "Delete tab")) { model.closeTab(tab.id, revealStartPage: model.activeID == tab.id) }
        }
        .yobroHelp(tab.sidebarHelp)
        .accessibilityLabel(tab.sidebarTitle)
        .accessibilityAddTraits(tab.id == model.activeID ? .isSelected : [])
    }
}

private struct TopTab: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false
    @State private var renaming = false
    @State private var renameText = ""
    @State private var renameOriginal = ""

    private var selected: Bool { tab.id == model.activeID }

    var body: some View {
        Button { model.select(tab.id) } label: {
            HStack(spacing: 6) {
                TabFavicon(tab: tab).overlay(alignment: .topTrailing) { CaptureIndicator(tab: tab) }
                Text(tab.sidebarTitle).font(.system(size: 13)).lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.leading, 6).padding(.trailing, hovering ? 26 : 7)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(tab.sidebarTitle)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .foregroundStyle(selected ? ink : tone.muted)
        .frame(height: 28)
        .background(selected ? Chrome.wash : hovering ? Chrome.hover : .clear, in: RoundedRectangle(cornerRadius: Chrome.radius))
        .overlay(alignment: .trailing) {
            if hovering {
                Button { model.closeSidebarTab(tab.id) } label: { Image(systemName: "xmark").font(.system(size: 9, weight: .semibold)) }
                    .buttonStyle(ChromeButtonStyle(size: 20))
                    .padding(.trailing, 4)
                    .yobroIconHelp(L("Tab löschen", "Delete tab"))
            }
        }
        .onHover { hovering = $0 }
        .contextMenu { TabMenuItems(tab: tab, model: model, rename: beginRenaming) }
        // Strip tabs can be 36 pt wide, too narrow to edit the name in place.
        .popover(isPresented: $renaming, arrowEdge: .bottom) {
            YOBRONameEditor(icon: "character.cursor.ibeam", title: L("Tab umbenennen", "Rename tab"),
                            detail: L("Der Name bleibt, bis du ihn zurücksetzt.", "The name stays until you reset it."),
                            text: $renameText, cancel: { renaming = false }, save: saveRename)
        }
        .preference(key: StripPopoverKey.self, value: renaming)
        .yobroHelp(tab.sidebarHelp)
    }

    private func beginRenaming() {
        renameOriginal = tab.sidebarTitle
        renameText = renameOriginal
        renaming = true
    }

    private func saveRename() {
        // An unchanged name must not freeze the title that follows the page.
        if renameText.trimmingCharacters(in: .whitespacesAndNewlines) != renameOriginal.trimmingCharacters(in: .whitespacesAndNewlines) {
            tab.rename(renameText)
        }
        renaming = false
    }
}

/// The context menu of a sidebar row and a strip tab.
private struct TabMenuItems: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    let rename: (() -> Void)?

    /// Folder tabs close in two steps: first the page, then the entry.
    private var closesPageOnly: Bool { tab.folderID != nil && tab.id == model.activeID && !tab.isSuspended }
    private var hasOtherLooseTab: Bool {
        model.visibleTabs.contains { !$0.pinned && $0.folderID == nil && $0.id != tab.id }
    }

    var body: some View {
        if let rename { Button(L("Tab umbenennen …", "Rename tab …"), action: rename) }
        if tab.customTitle != nil { Button(L("Namen zurücksetzen", "Reset name")) { tab.rename("") } }
        Button(L("Link kopieren", "Copy link")) { model.copyLink(tab) }.disabled(tab.url.isEmpty || tab.isExtensionsHub)
        Divider()
        Menu(L("In Space verschieben")) {
            ForEach(model.spaces.filter { $0 != tab.space }, id: \.self) { space in Button(space) { model.moveToSpace(tab.id, space: space) } }
        }
        .disabled(model.spaces.count < 2)
        if !tab.isPrivate {
            Menu(L("In Ordner verschieben")) {
                Button(L("Ohne Ordner")) { model.moveToFolder(tab.id, folder: nil) }
                ForEach(model.folders.filter { $0.space == tab.space }) { folder in Button(folder.name) { model.moveToFolder(tab.id, folder: folder.id) } }
            }
        }
        if model.splitPairs.contains(where: { $0.contains(tab.id) }) { Button(L("Splitview trennen")) { model.separateSplit(tab.id) } }
        if !tab.isPrivate { Button(tab.pinned ? L("Loslösen") : L("Anpinnen")) { tab.pinned.toggle(); model.save() } }
        Button(L("Tab duplizieren")) { model.newTab(url: tab.url, space: tab.space, isPrivate: tab.isPrivate) }
        Divider()
        Button(L("Andere Tabs schließen", "Close other tabs")) { model.closeOtherTabs(except: tab.id) }
            .disabled(!hasOtherLooseTab)
        Button(closesPageOnly
            ? L("Seite schließen · im Ordner behalten", "Close page · keep in folder")
            : L("Tab löschen", "Delete tab")) { model.closeSidebarTab(tab.id) }
    }
}

/// A red dot on a favicon while the page uses the camera or microphone.
private struct CaptureIndicator: View {
    let tab: BrowserTab

    var body: some View {
        if let signals = (tab.webView as? AppearanceWebView)?.signals { CaptureDot(signals: signals) }
    }
}

/// The slim bar above a page in the tabs-on-top layout: navigation on the
/// left, the site in the middle, page actions on the right. A split half
/// adds its own controls as the trailing accessory.
private struct PageBand<Accessory: View>: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    var onFocus: () -> Void = {}
    @ViewBuilder var accessory: () -> Accessory
    @State private var showingActions = false
    @State private var siteHover = false

    var body: some View {
        HStack(spacing: 2) {
            door("chevron.left", L("Zurück")) { tab.webView.goBack() }.disabled(!tab.canGoBack)
            door("chevron.right", L("Vorwärts")) { tab.webView.goForward() }.disabled(!tab.canGoForward)
            door(tab.loading ? "xmark" : "arrow.clockwise", tab.loading ? L("Laden stoppen", "Stop loading") : L("Neu laden")) {
                if tab.loading { tab.webView.stopLoading() } else { tab.webView.reload() }
            }.disabled(tab.url.isEmpty)
            Spacer(minLength: 0)
            if !tab.isPrivate && !tab.url.isEmpty { LoginAutofillButton(login: tab.logins).id(tab.id) }
            door("slider.horizontal.3", L("Kontrollzentrum", "Control Center")) { showingActions.toggle() }
                .popover(isPresented: $showingActions, arrowEdge: .bottom) {
                    SidebarPageActions(model: model, tab: tab, store: model.extensions) { showingActions = false }
                }
            accessory()
        }
        .overlay {
            Button {
                onFocus()
                model.openCommandBar(.current)
            } label: {
                HStack(spacing: 4) {
                    if tab.isPrivate { Image(systemName: "eyeglasses").font(.system(size: 11)) }
                    if insecure {
                        Group {
                            Image(systemName: "lock.open").font(.system(size: 10))
                            Text(L("Nicht sicher", "Not secure") + " ·").font(.system(size: 13)).lineLimit(1)
                        }
                        .foregroundStyle(ink.opacity(0.55))
                    }
                    Text(pageLabel(tab)).font(.system(size: 13, weight: .medium)).lineLimit(1)
                }
                .foregroundStyle(ink)
                .padding(.horizontal, 8).frame(height: 24)
                .background(siteHover ? ink.opacity(0.06) : .clear, in: RoundedRectangle(cornerRadius: 8))
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .onHover { siteHover = $0 }
            .yobroHelp((insecure ? L("Nicht sicher", "Not secure") + " · " : "") + (tab.url.isEmpty ? "⌘L" : tab.url + " · ⌘L"))
        }
        .padding(.horizontal, 3)
        .frame(height: 36)
        .background(paper)
        .foregroundStyle(ink.opacity(0.72))
        .onChange(of: model.showControlCenter) { _, on in
            // Split halves each have a band; only the active one answers.
            guard on, model.activeID == tab.id else { return }
            model.showControlCenter = false
            showingActions = true
        }
    }

    private var insecure: Bool { PageSecurity.isInsecure(tab) }

    private func door(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).font(.system(size: 14)).frame(width: 30, height: 30) }
            .buttonStyle(YOBROButtonStyle(minimumSize: 30))
            .yobroIconHelp(help)
    }
}

extension PageBand where Accessory == EmptyView {
    init(tab: BrowserTab, model: BrowserModel) {
        self.init(tab: tab, model: model, onFocus: {}, accessory: { EmptyView() })
    }
}

/// Favicon of a tab, with a symbol when the page has none.
private struct TabFavicon: View {
    @ObservedObject var tab: BrowserTab
    var size: CGFloat = 16

    var body: some View {
        Group {
            if tab.isPrivate { Image(systemName: "eyeglasses").font(.system(size: size * 0.8)) }
            else if let icon = tab.favicon { Image(nsImage: icon).resizable().scaledToFit().clipShape(RoundedRectangle(cornerRadius: size * 0.22)) }
            else { Image(systemName: tab.url.isEmpty ? "circle.dotted" : "globe").font(.system(size: size * 0.8)) }
        }
        .frame(width: size, height: size)
    }
}

/// Pinned tabs as a grid of favicon tiles, like YAB's pins.
private struct PinGrid: View {
    @ObservedObject var model: BrowserModel

    var body: some View {
        let pins = model.visibleTabs.filter(\.pinned)
        if pins.isEmpty {
            PinPlaceholder(model: model)
        } else {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: Chrome.inset), count: 4), spacing: Chrome.inset) {
                ForEach(pins) { tab in PinTile(tab: tab, model: model) }
            }
        }
    }
}

/// Shown during a drag while the Space has no pins yet.
private struct PinPlaceholder: View {
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var targeted = false

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "pin").font(.system(size: 11))
            Text(L("Hierher ziehen zum Anpinnen", "Drop here to pin")).font(.system(size: 12)).lineLimit(1)
        }
        .foregroundStyle(tone.muted)
        .frame(maxWidth: .infinity)
        .frame(height: 45)
        .background(targeted ? Chrome.wash : .clear, in: RoundedRectangle(cornerRadius: Chrome.radius))
        .overlay {
            RoundedRectangle(cornerRadius: Chrome.radius)
                .strokeBorder(tone.muted.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
                .allowsHitTesting(false)
        }
        .contentShape(Rectangle())
        .onDrop(of: [UTType.plainText], delegate: PinPlaceholderDropDelegate(model: model, targeted: $targeted))
    }
}

private struct PinTile: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false
    @State private var width: CGFloat = 45

    private var selected: Bool { tab.id == model.activeID }
    private var dropIntent: TabDropIntent? {
        model.tabDropFeedback?.targetID == tab.id ? model.tabDropFeedback?.intent : nil
    }

    var body: some View {
        Button { model.select(tab.id) } label: {
            TabFavicon(tab: tab)
                .opacity(tab.isSuspended ? 0.6 : 1)
                .overlay(alignment: .topTrailing) { CaptureIndicator(tab: tab) }
                .foregroundStyle(tone.muted)
                .frame(maxWidth: .infinity)
                .frame(height: 45)
                .background(selected ? Chrome.wash : hovering ? Chrome.restHover : Chrome.rest, in: RoundedRectangle(cornerRadius: Chrome.radius))
                .overlay(alignment: .bottom) {
                    if tab.loading { Capsule().fill(tone.muted.opacity(0.5)).frame(width: 14, height: 2).padding(.bottom, 5) }
                }
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(tab.sidebarTitle)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .onHover { hovering = $0 }
        .opacity(model.draggingTabID == tab.id ? 0.6 : 1)
        .tabDropIndicator(dropIntent, gap: Chrome.inset, axis: .horizontal)
        .background(GeometryReader { geometry in
            Color.clear
                .onAppear { width = geometry.size.width }
                .onChange(of: geometry.size.width) { _, value in width = value }
        })
        .onDrag {
            model.beginSidebarTabDrag(tab.id)
            return NSItemProvider(object: tab.id.uuidString as NSString)
        }
        .onDrop(of: [UTType.plainText], delegate: PinDropDelegate(targetID: tab.id, width: width, model: model))
        .contextMenu {
            Button(L("Loslösen")) { tab.pinned = false; model.save() }
            Button(L("Link kopieren", "Copy link")) { model.copyLink(tab) }.disabled(tab.url.isEmpty || tab.isExtensionsHub)
            Menu(L("In Space verschieben")) {
                ForEach(model.spaces.filter { $0 != tab.space }, id: \.self) { space in Button(space) { model.moveToSpace(tab.id, space: space) } }
            }
            .disabled(model.spaces.count < 2)
            Divider()
            // ⌘W only unloads a pin; deleting it is explicit.
            Button(L("Tab löschen", "Delete tab")) { model.closeTab(tab.id, revealStartPage: model.activeID == tab.id) }
        }
        .yobroHelp(tab.sidebarHelp, placement: .trailing)
    }
}

private struct SidebarTabList: View {
    @ObservedObject var model: BrowserModel
    let edit: (SidebarEditor) -> Void
    let requestDelete: (String) -> Void
    @Environment(\.spaceTone) private var tone
    @State private var tabFrames: [UUID: CGRect] = [:]
    @State private var viewportFrame: CGRect = .null
    @State private var scrollRevision = 0

    private var todayTabs: [BrowserTab] { model.visibleTabs.filter { !$0.pinned && $0.folderID == nil } }

    var body: some View {
        ScrollViewReader { proxy in
            ZStack {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        SpaceHeaderRow(model: model, edit: edit, requestDelete: requestDelete).padding(.bottom, 5)
                        ForEach(model.folders.filter { $0.space == model.space }) { folder in
                            SpaceFolderRow(folder: folder, model: model).padding(.bottom, 5)
                        }
                        if !todayTabs.isEmpty {
                            ClearTabsDivider { model.clearTodayTabs() }
                        }
                        NewTabRow(model: model).padding(.bottom, 5)
                        ForEach(todayTabs) { TabRow(tab: $0, model: model) }
                        Color.clear
                            .frame(height: 44)
                            .contentShape(Rectangle())
                            .onDrop(of: [UTType.plainText], delegate: SidebarBottomDropDelegate(model: model))
                    }
                    .background(SidebarScrollObserver(changed: { scrollRevision &+= 1 }, connected: { model.sidebarScrollView = $0 }))
                }
                .scrollIndicators(.hidden)
                .background(GeometryReader { geometry in
                    Color.clear.preference(key: SidebarViewportFrameKey.self, value: geometry.frame(in: .global))
                })
                .onPreferenceChange(SidebarTabFrameKey.self) { tabFrames = $0 }
                .onPreferenceChange(SidebarViewportFrameKey.self) { viewportFrame = $0 }
                // A selection only scrolls as far as needed, so clicking a
                // visible row leaves the list where it is.
                .onChange(of: model.activeID) { _, _ in revealActiveTab(using: proxy, animated: true, anchor: nil) }
                .onChange(of: model.space) { _, _ in revealActiveTab(using: proxy, animated: false, anchor: .center) }

                if let direction = jumpDirection, model.draggingTabID == nil, model.draggingFolderID == nil {
                    VStack {
                        Spacer()
                        HStack {
                            Spacer()
                            SidebarActiveTabJump(direction: direction) {
                                revealActiveTab(using: proxy, animated: true, anchor: .center)
                            }
                        }
                    }
                    .padding(.trailing, 6).padding(.bottom, 6)
                }
            }
        }
    }

    private var jumpDirection: SidebarJumpDirection? {
        _ = scrollRevision
        guard let id = model.activeID, !(model.active?.pinned ?? false) else { return nil }
        if let direction = SidebarVisibility.jumpDirection(for: tabFrames[id], viewport: viewportFrame) { return direction }
        let ordered = presentedTabIDs
        let visibleIndices: [Int] = tabFrames.compactMap { entry -> Int? in
            guard entry.value.intersects(viewportFrame) else { return nil }
            return ordered.firstIndex(of: entry.key)
        }
        return SidebarVisibility.inferredJumpDirection(activeIndex: ordered.firstIndex(of: id), visibleIndices: visibleIndices)
    }

    private var presentedTabIDs: [UUID] {
        var result: [UUID] = []
        for folder in model.folders where folder.space == model.space {
            let tabs = model.visibleTabs.filter { !$0.pinned && $0.folderID == folder.id }
            if folder.collapsed {
                if let active = tabs.first(where: { $0.id == model.activeID }) { result.append(active.id) }
            } else {
                result.append(contentsOf: tabs.map(\.id))
            }
        }
        result.append(contentsOf: todayTabs.map(\.id))
        return result
    }

    /// `anchor: nil` scrolls only as far as needed to show the row.
    private func revealActiveTab(using proxy: ScrollViewProxy, animated: Bool, anchor: UnitPoint?) {
        guard let id = model.activeID else { return }
        Task { @MainActor in
            await Task.yield()
            if animated { withAnimation(.easeInOut(duration: 0.22)) { proxy.scrollTo(id, anchor: anchor) } }
            else { proxy.scrollTo(id, anchor: anchor) }
        }
    }
}

/// The Space's name above its folders, with the Space menu on hover.
private struct SpaceHeaderRow: View {
    @ObservedObject var model: BrowserModel
    let edit: (SidebarEditor) -> Void
    let requestDelete: (String) -> Void
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: model.spaceIcon(for: model.space).rawValue).font(.system(size: 11)).frame(width: 14)
            Text(model.space).font(.system(size: 12.5, weight: .semibold)).lineLimit(1)
            Spacer(minLength: 4)
            Menu {
                SpaceMenuItems(model: model, space: model.space, edit: edit, requestDelete: requestDelete)
            } label: {
                Image(systemName: "ellipsis")
            }
            .menuStyle(.button)
            .buttonStyle(ChromeButtonStyle(size: 24))
            .menuIndicator(.hidden)
            .fixedSize()
            .opacity(hovering ? 1 : 0)
            .yobroIconHelp(L("Space und Ordner verwalten", "Manage space and folders"))
        }
        .foregroundStyle(tone.muted)
        .padding(.leading, 10).padding(.trailing, 6)
        .frame(height: Chrome.rowHeight)
        .chromeRow(selected: false, hovering: hovering)
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .contextMenu { SpaceMenuItems(model: model, space: model.space, edit: edit, requestDelete: requestDelete) }
    }
}

private struct SpaceMenuItems: View {
    @ObservedObject var model: BrowserModel
    let space: String
    let edit: (SidebarEditor) -> Void
    let requestDelete: (String) -> Void
    /// The tab strip shows no folders, so it leaves out "New folder".
    var showsFolderActions = true

    private var index: Int? { model.spaces.firstIndex(of: space) }

    var body: some View {
        Button(L("Space umbenennen …")) { edit(.renameSpace(space)) }
        SpaceIconMenu(model: model, space: space)
        SpaceToneMenu(model: model, space: space)
        Button(L("Nach links", "Move left")) { model.moveSpace(space, by: -1) }
            .disabled((index ?? 0) == 0)
        Button(L("Nach rechts", "Move right")) { model.moveSpace(space, by: 1) }
            .disabled((index ?? model.spaces.count) >= model.spaces.count - 1)
        Divider()
        if showsFolderActions { Button(L("Neuer Ordner …")) { edit(.newFolder(space)) } }
        Button(L("Neuer Space …")) { edit(.newSpace) }
        Divider()
        Button(L("Space löschen …", "Delete space …"), role: .destructive) { requestDelete(space) }
            .disabled(model.spaces.count < 2)
    }
}

private struct SpaceToneMenu: View {
    @ObservedObject var model: BrowserModel
    let space: String

    var body: some View {
        Menu {
            ForEach(SpaceTone.allCases) { tone in
                Button { model.setSpaceTone(tone, for: space) } label: {
                    Label { Text((model.spaceTone(for: space) == tone ? "✓  " : "") + tone.title) } icon: { Image(nsImage: tone.swatch) }
                }
            }
        } label: { Label(L("Space-Farbe", "Space color"), systemImage: "paintpalette") }
    }
}

/// YAB's line between the kept tabs and today's tabs. "Clear" closes the
/// latter in one step that a single undo restores.
private struct ClearTabsDivider: View {
    let clear: () -> Void
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            Rectangle().fill(tone.muted.opacity(0.18)).frame(height: 1)
            Button(L("Leeren", "Clear"), action: clear)
                .buttonStyle(.plain)
                .font(.system(size: 11))
                .foregroundStyle(tone.muted.opacity(0.8))
                .opacity(hovering ? 1 : 0)
                .yobroHelp(L("Alle Tabs unter dieser Linie schließen", "Close all tabs below this line"))
        }
        .padding(.horizontal, 10)
        .frame(height: 24)
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .padding(.bottom, 5)
    }
}

private struct NewTabRow: View {
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false

    var body: some View {
        Button { model.openCommandBar(.newTab) } label: {
            HStack(spacing: 10) {
                Image(systemName: "plus").font(.system(size: 13)).frame(width: 16)
                Text(L("Neuer Tab", "New tab")).font(.system(size: 13))
                Spacer()
            }
            .foregroundStyle(tone.muted.opacity(0.75))
            .padding(.leading, 10)
            .frame(height: Chrome.rowHeight)
            .chromeRow(selected: false, hovering: hovering)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .contextMenu { Button(L("Neuer privater Tab", "New private tab")) { model.requestPrivateTab() } }
        .yobroHelp(L("Neuer Tab · ⌘T"))
    }
}

private struct SidebarFooter: View {
    @ObservedObject var model: BrowserModel
    let edit: (SidebarEditor) -> Void
    let requestDelete: (String) -> Void
    @Environment(\.spaceTone) private var tone

    var body: some View {
        HStack(spacing: 2) {
            Menu { LibraryMenuItems(model: model) } label: { Image(systemName: "square.grid.2x2") }
                .menuStyle(.button).buttonStyle(ChromeButtonStyle(size: 26)).menuIndicator(.hidden).fixedSize()
                .yobroIconHelp(L("Bibliothek und Einstellungen", "Library and settings"))
            Spacer(minLength: 2)
            // At most three Spaces fit beside the buttons; more scroll.
            ScrollViewReader { proxy in
                ScrollView(.horizontal) {
                    HStack(spacing: 0) {
                        ForEach(model.spaces, id: \.self) { space in
                            SpaceButton(model: model, space: space, edit: edit, requestDelete: requestDelete).id(space)
                        }
                    }
                }
                .scrollIndicators(.hidden)
                .frame(width: min(CGFloat(model.spaces.count) * 34, 102))
                .onAppear { proxy.scrollTo(model.space) }
                .onChange(of: model.space) { _, space in withAnimation(.easeInOut(duration: 0.2)) { proxy.scrollTo(space) } }
                .onChange(of: model.spaces) { _, _ in proxy.scrollTo(model.space) }
            }
            Spacer(minLength: 2)
            Button { model.showAgent.toggle() } label: {
                Image(systemName: "sparkles")
                    .overlay(alignment: .topTrailing) {
                        if model.agentUsageActive { Circle().fill(brandOrange).frame(width: 6, height: 6).offset(x: 3, y: -2) }
                    }
            }
            .buttonStyle(ChromeButtonStyle(size: 26))
            .yobroIconHelp(model.showAgent ? L("Assistent schließen · ⇧⌘A", "Close assistant · ⇧⌘A") : L("Assistent öffnen · ⇧⌘A", "Open assistant · ⇧⌘A"),
                           label: L("Assistent", "Assistant"))
            .accessibilityAddTraits(model.showAgent ? .isSelected : [])
            .accessibilityIdentifier("agent-quick-access")
            Menu {
                Button(L("Neuer Space …")) { edit(.newSpace) }
                Button(L("Neuer Ordner …")) { edit(.newFolder(model.space)) }
                Divider()
                Button(L("Neuer privater Tab", "New private tab")) { model.requestPrivateTab() }
            } label: { Image(systemName: "plus") }
                .menuStyle(.button).buttonStyle(ChromeButtonStyle(size: 26)).menuIndicator(.hidden).fixedSize()
                .yobroIconHelp(L("Neuer Space, Ordner oder privater Tab", "New space, folder or private tab"))
        }
        .frame(height: 26)
    }
}

private struct SpaceButton: View {
    @ObservedObject var model: BrowserModel
    let space: String
    let edit: (SidebarEditor) -> Void
    let requestDelete: (String) -> Void
    @Environment(\.spaceTone) private var tone
    @State private var hovering = false
    @State private var targeted = false

    private var active: Bool { space == model.space }

    var body: some View {
        Button { if !active { model.switchSpace(space) } } label: {
            Image(systemName: model.spaceIcon(for: space).rawValue)
                .font(.system(size: 12))
                .foregroundStyle(tone.muted)
                .frame(width: 34, height: 26)
                .background(targeted ? Chrome.wash : hovering && !active ? Chrome.hover : .clear, in: RoundedRectangle(cornerRadius: Chrome.radius))
                .opacity(active || targeted ? 1 : hovering ? 0.8 : 0.45)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .onDrop(of: [UTType.plainText], delegate: SpaceDropDelegate(space: space, model: model, targeted: $targeted))
        .contextMenu { SpaceMenuItems(model: model, space: space, edit: edit, requestDelete: requestDelete) }
        .yobroHelp(space)
        .accessibilityLabel(L("Space: ", "Space: ") + space)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

private struct LibraryMenuItems: View {
    @ObservedObject var model: BrowserModel

    var body: some View {
        Button(L("Verlauf", "History")) { model.librarySection = .history }
        Button(L("Lesezeichen", "Bookmarks")) { model.librarySection = .bookmarks }
        Button("Downloads") { model.librarySection = .downloads }
        Divider()
        if let profiles = model.profileSession {
            Menu(L("Profil: ", "Profile: ") + profiles.active.name) {
                ForEach(profiles.registry.profiles) { profile in
                    Button {
                        do { try profiles.switchTo(profile.id) } catch { model.notice = error.localizedDescription }
                    } label: {
                        Label(profile.name, systemImage: profile.id == profiles.active.id ? "checkmark" : profile.symbol)
                    }
                }
                Divider()
                Button(L("Neues Profil …", "New profile …")) { model.creatingProfile = true; model.showProfileEditor = true }
                Button(L("Profil bearbeiten …", "Edit profile …")) { model.creatingProfile = false; model.showProfileEditor = true }
            }
        }
        Button(model.tabsOnTop ? L("Tabs in der Seitenleiste", "Tabs in the sidebar") : L("Tabs oben anzeigen", "Show tabs on top")) { model.tabsOnTop.toggle() }
        Divider()
        Button(L("Browserdaten importieren …", "Import browser data …")) { model.settingsSection = L("Daten importieren"); model.showSettings = true }
        Button(L("Einstellungen …", "Settings …")) { model.settingsSection = L("Allgemein", "General"); model.showSettings = true }
    }
}

private struct SidebarActiveTabJump: View {
    let direction: SidebarJumpDirection?
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: direction == .up ? "arrow.up" : direction == .down ? "arrow.down" : "arrow.up.and.down")
            .font(.system(size: 10, weight: .bold))
            .foregroundStyle(paper)
            .frame(width: 26, height: 26)
            .background(ink.opacity(0.85), in: Circle())
            .shadow(color: .black.opacity(0.16), radius: 6, y: 2)
        }
        .buttonStyle(.plain)
        .yobroHelp(L("Zum aktiven Tab springen", "Jump to active tab"))
        .accessibilityLabel(L("Zum aktiven Tab springen", "Jump to active tab"))
    }
}

/// The overlay stays attached to the left window edge, while its exposed
/// right edge reads as a floating panel.
private struct SidebarOverlayShape: Shape {
    let radius: CGFloat

    func path(in rect: CGRect) -> Path {
        Path(roundedRect: rect, cornerRadii: RectangleCornerRadii(
            topLeading: 0,
            bottomLeading: 0,
            bottomTrailing: radius,
            topTrailing: radius
        ))
    }
}

private struct ActiveDownloadStatus: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var downloads: DownloadStore

    private var current: DownloadEntry? { downloads.activeEntries.first }

    var body: some View {
        if let current {
            HStack(spacing: 10) {
                ZStack {
                    Circle().stroke(.white.opacity(0.2), lineWidth: 2.5)
                    if current.expected > 0 {
                        Circle().trim(from: 0, to: current.fraction).stroke(.white, style: StrokeStyle(lineWidth: 2.5, lineCap: .round)).rotationEffect(.degrees(-90))
                    } else {
                        ProgressView().controlSize(.mini)
                    }
                    Image(systemName: "arrow.down").font(.system(size: 8, weight: .bold))
                }.frame(width: 22, height: 22)
                VStack(alignment: .leading, spacing: 2) {
                    Text(current.name).font(.system(size: 12, weight: .medium)).lineLimit(1)
                    HStack(spacing: 4) {
                        Text(progressLabel(current))
                        if downloads.activeCount > 1 {
                            Text("· " + String(format: L("+%d weitere", "+%d more"), downloads.activeCount - 1))
                        }
                    }.font(.system(size: 10)).opacity(0.65).lineLimit(1)
                }.frame(width: 180, alignment: .leading)
                Button { downloads.cancel(current.id) } label: { Image(systemName: "xmark").frame(width: 22, height: 22) }
                    .buttonStyle(.plain).yobroIconHelp(L("Download abbrechen"))
                Button { model.librarySection = .downloads } label: { Image(systemName: "chevron.right").frame(width: 22, height: 22) }
                    .buttonStyle(.plain).yobroIconHelp(L("Downloads öffnen", "Open downloads"))
            }
            .padding(.horizontal, 12).padding(.vertical, 9)
            .toastSurface()
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("active-download-status")
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }

    private func progressLabel(_ entry: DownloadEntry) -> String {
        let received = ByteCountFormatter.string(fromByteCount: entry.received, countStyle: .file)
        guard entry.expected > 0 else { return L("\(received) geladen", "\(received) downloaded") }
        return "\(Int(entry.fraction * 100)) % · \(received) / \(ByteCountFormatter.string(fromByteCount: entry.expected, countStyle: .file))"
    }
}

/// A short confirmation, such as a copied link or a new bookmark. Unlike
/// `notice`, it never blocks and goes away on its own.
private struct InfoToast: View {
    @ObservedObject var model: BrowserModel
    let toast: BrowserToast

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: toast.icon).font(.system(size: 14))
            Text(toast.text).lineLimit(1)
        }
        .padding(.leading, 12).padding(.trailing, 14)
        .frame(height: 40)
        .toastSurface()
        .accessibilityElement(children: .combine)
        .appearanceTask(id: toast.id) {
            // It is gone before VoiceOver would reach it.
            AccessibilityNotification.Announcement(toast.text).post()
            try? await Task.sleep(nanoseconds: 2_500_000_000)
            guard !Task.isCancelled else { return }
            model.dismissToast(toast.id)
        }
    }
}

private struct ClosedTabUndo: View {
    @ObservedObject var model: BrowserModel
    let undoID: UUID

    var body: some View {
        let count = model.closedTabUndoCount
        HStack(spacing: 10) {
            Image(systemName: "xmark.circle").font(.system(size: 15))
            Text(count > 1 ? L("\(count) Tabs geschlossen", "\(count) tabs closed") : L("Tab geschlossen", "Tab closed"))
            Button(L("Rückgängig", "Undo")) { model.undoClosedTabs() }
                .buttonStyle(.plain)
                .font(.system(size: 12.5, weight: .semibold))
                .accessibilityHint(count > 1
                    ? L("Stellt die Tabs in ihrem ursprünglichen Space und Ordner wieder her.", "Restores the tabs to their original space and folder.")
                    : L("Stellt den Tab in seinem ursprünglichen Space und Ordner wieder her.", "Restores the tab to its original space and folder."))
        }
        .padding(.leading, 12).padding(.trailing, 14)
        .frame(height: 40)
        .toastSurface()
        .appearanceTask(id: undoID) {
            try? await Task.sleep(nanoseconds: 6_000_000_000)
            guard !Task.isCancelled else { return }
            model.dismissClosedTabUndo(undoID)
        }
    }
}

struct TabRow: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    var reorderGap: CGFloat = 5
    @Environment(\.spaceTone) private var tone
    @State private var hover = false
    @State private var isRenaming = false
    @State private var renameText = ""
    @State private var renameOriginal = ""
    @FocusState private var renameIsFocused: Bool
    private var dropIntent: TabDropIntent? {
        model.tabDropFeedback?.targetID == tab.id ? model.tabDropFeedback?.intent : nil
    }
    private var selected: Bool { tab.id == model.activeID }
    /// Folder tabs close in two steps: first the page, then the entry.
    private var closesPageOnly: Bool { tab.folderID != nil && selected && !tab.isSuspended }

    var body: some View {
        HStack(spacing: 0) {
            Button {
                guard !isRenaming else { return }
                model.select(tab.id)
            } label: {
                HStack(spacing: 10) {
                    TabFavicon(tab: tab)
                    if model.splitPairs.contains(where: { $0.contains(tab.id) }) { Image(systemName: "rectangle.split.2x1").font(.system(size: 10)) }
                    if isRenaming {
                        TextField(L("Tabname", "Tab name"), text: $renameText)
                            .textFieldStyle(.plain)
                            .font(.system(size: 13))
                            .focused($renameIsFocused)
                            .onSubmit { finishRenaming() }
                            .onExitCommand { cancelRenaming() }
                    } else {
                        Text(tab.sidebarTitle).font(.system(size: 13)).lineLimit(1)
                            .opacity(tab.isSuspended ? 0.7 : 1)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.leading, 10)
                .frame(maxWidth: .infinity, minHeight: Chrome.rowHeight)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(tab.sidebarTitle)
            .accessibilityAddTraits(selected ? .isSelected : [])

            // Outside the row's button, so its own controls stay clickable.
            if let signals = (tab.webView as? AppearanceWebView)?.signals { MediaIndicator(signals: signals) }

            ZStack {
                if hover || closesPageOnly {
                    Button { model.closeSidebarTab(tab.id) } label: {
                        Image(systemName: closesPageOnly ? "minus" : "xmark").font(.system(size: 10, weight: .semibold))
                            .frame(width: 24, height: 24)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(ChromeButtonStyle(size: 24))
                    .yobroIconHelp(closesPageOnly ? L("Seite schließen · Tab im Ordner behalten", "Close page · keep tab in folder") : L("Tab löschen", "Delete tab"),
                                   label: closesPageOnly ? L("Seite schließen", "Close page") : L("Tab löschen", "Delete tab"))
                } else if tab.loading {
                    ProgressView().controlSize(.mini).allowsHitTesting(false)
                }
            }
            .frame(width: 30)
        }
        .frame(height: Chrome.rowHeight)
        .foregroundStyle(selected ? ink : tone.muted)
        .background(alignment: .leading) {
            // YAB shows loading as a soft fill sweeping across the row.
            if tab.loading {
                GeometryReader { geometry in
                    RoundedRectangle(cornerRadius: Chrome.radius)
                        .fill(ink.opacity(0.055))
                        .frame(width: geometry.size.width * max(0.08, tab.progress))
                }
                .allowsHitTesting(false)
            }
        }
        .chromeRow(selected: selected, hovering: hover)
        .id(tab.id)
        .background(GeometryReader { geometry in
            Color.clear.preference(key: SidebarTabFrameKey.self, value: [tab.id: geometry.frame(in: .global)])
        })
        // Keep the source clearly visible while its separate drag preview
        // moves. Making the row nearly disappear looked like the drag had
        // failed before it had even reached a drop target.
        .opacity(model.draggingTabID == tab.id ? 0.82 : 1)
        .animation(.easeOut(duration: 0.12), value: model.draggingTabID == tab.id)
        .tabDropIndicator(dropIntent, gap: reorderGap)
        .onHover { hover = $0 }
        .onChange(of: renameIsFocused) { _, focused in
            if !focused && isRenaming { finishRenaming() }
        }
        .onChange(of: model.activeID) { _, activeID in
            if activeID != tab.id { cancelRenaming() }
        }
        .contentShape(Rectangle())
        .onDrag {
            model.beginSidebarTabDrag(tab.id)
            return NSItemProvider(object: tab.id.uuidString as NSString)
        } preview: {
            HStack(spacing: 7) {
                Image(systemName: "globe")
                Text(tab.sidebarTitle).lineLimit(1)
            }
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(ink)
            .padding(.horizontal, 10).frame(width: 200, height: 34, alignment: .leading)
            .background(paper.opacity(0.95), in: RoundedRectangle(cornerRadius: Chrome.radius))
        }
        // The visual gap belongs to the preceding row's drop destination.
        // Otherwise the exact space between tabs has no native drop target.
        .padding(.bottom, reorderGap)
        .contentShape(Rectangle())
        .onDrop(of: [UTType.plainText], delegate: TabReorderDropDelegate(
            targetID: tab.id, rowHeight: Chrome.rowHeight + reorderGap, model: model
        ))
        .contextMenu { TabMenuItems(tab: tab, model: model, rename: beginRenaming) }
        .yobroHelp(tab.sidebarHelp, placement: .trailing)
    }

    private func beginRenaming() {
        renameOriginal = tab.sidebarTitle
        renameText = renameOriginal
        isRenaming = true
        Task { @MainActor in
            await Task.yield()
            renameIsFocused = true
        }
    }

    private func finishRenaming() {
        guard isRenaming else { return }
        // An unchanged name must not freeze the title that follows the page.
        if renameText.trimmingCharacters(in: .whitespacesAndNewlines) != renameOriginal.trimmingCharacters(in: .whitespacesAndNewlines) {
            tab.rename(renameText)
        }
        isRenaming = false
        renameIsFocused = false
    }

    private func cancelRenaming() {
        guard isRenaming else { return }
        isRenaming = false
        renameIsFocused = false
    }
}

enum TabDropIntent: Equatable {
    case before, split, after

    static func resolve(locationY: CGFloat, rowHeight: CGFloat) -> Self {
        let edgeZone = rowHeight * 0.25
        if locationY < edgeZone { return .before }
        if locationY > rowHeight - edgeZone { return .after }
        return .split
    }
}

struct TabDropFeedback: Equatable {
    let targetID: UUID
    let intent: TabDropIntent
}

/// Pins sit side by side, so a drop lands before or after a tile by its
/// horizontal half and never pairs the two into a split.
enum PinDropIntent {
    static func resolve(locationX: CGFloat, width: CGFloat) -> TabDropIntent {
        locationX < width / 2 ? .before : .after
    }
}

private struct TabDropIndicatorModifier: ViewModifier {
    let intent: TabDropIntent?
    let compact: Bool
    let gap: CGFloat
    /// `.horizontal` for items in a row: the line stands on the leading or
    /// trailing edge.
    let axis: Axis

    private var offset: CGFloat { gap / 2 + 1.5 }

    func body(content: Content) -> some View {
        content
            .overlay {
                if intent == .split {
                    ZStack {
                        RoundedRectangle(cornerRadius: Chrome.radius)
                            .fill(ink.opacity(0.08))
                            .overlay { RoundedRectangle(cornerRadius: Chrome.radius).stroke(ink.opacity(0.55), lineWidth: 1.5) }
                        HStack(spacing: 5) {
                            Image(systemName: "rectangle.split.2x1")
                            if !compact { Text(L("Splitview", "Split view")) }
                        }
                        .font(.system(size: compact ? 12 : 10, weight: .semibold))
                        .foregroundStyle(ink)
                        .padding(.horizontal, compact ? 0 : 7)
                        .frame(height: 22)
                        .background(paper.opacity(0.94), in: Capsule())
                    }
                    .transition(.opacity.combined(with: .scale(scale: 0.97)))
                    .allowsHitTesting(false)
                }
            }
            .overlay(alignment: axis == .vertical ? .top : .leading) {
                if intent == .before {
                    insertionLine.offset(x: axis == .horizontal ? -offset : 0, y: axis == .vertical ? -offset : 0)
                }
            }
            .overlay(alignment: axis == .vertical ? .bottom : .trailing) {
                if intent == .after {
                    insertionLine.offset(x: axis == .horizontal ? offset : 0, y: axis == .vertical ? offset : 0)
                }
            }
            .animation(.easeOut(duration: 0.10), value: intent)
    }

    private var insertionLine: some View {
        Capsule()
            .fill(ink.opacity(0.7))
            .frame(width: axis == .horizontal ? 2.5 : nil, height: axis == .vertical ? 2.5 : nil)
            .padding(axis == .vertical ? .horizontal : .vertical, compact ? 2 : 4)
            .transition(.opacity)
            .allowsHitTesting(false)
    }
}

extension View {
    func tabDropIndicator(_ intent: TabDropIntent?, compact: Bool = false, gap: CGFloat = 5, axis: Axis = .vertical) -> some View {
        modifier(TabDropIndicatorModifier(intent: intent, compact: compact, gap: gap, axis: axis))
    }
}

struct TabReorderDropDelegate: DropDelegate {
    let targetID: UUID
    let rowHeight: CGFloat
    let model: BrowserModel

    func validateDrop(info: DropInfo) -> Bool {
        guard model.draggingFolderID == nil else { return false }
        if let sourceID = model.draggingTabID {
            guard sourceID != targetID,
                  let source = model.tabs.first(where: { $0.id == sourceID }),
                  let target = model.tabs.first(where: { $0.id == targetID }) else { return false }
            return source.space == target.space
        }
        return info.hasItemsConforming(to: [UTType.plainText.identifier, "public.plain-text"])
    }

    func dropEntered(info: DropInfo) { updateIntent(for: info) }
    func dropExited(info: DropInfo) {
        if model.tabDropFeedback?.targetID == targetID { model.tabDropFeedback = nil }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        guard validateDrop(info: info) else {
            model.tabDropFeedback = nil
            return DropProposal(operation: .forbidden)
        }
        model.autoScrollSidebarDuringDrag()
        updateIntent(for: info)
        return DropProposal(operation: .move)
    }

    func performDrop(info: DropInfo) -> Bool {
        guard validateDrop(info: info) else { return false }
        let sessionID = model.sidebarDragSessionID
        if let sourceID = model.draggingTabID {
            var handled = false
            withAnimation(.easeInOut(duration: 0.18)) {
                handled = model.handleTabDrop(sourceID, on: targetID, locationY: info.location.y, rowHeight: rowHeight)
            }
            DispatchQueue.main.async {
                model.finishSidebarDrag(sessionID: sessionID)
            }
            return handled
        }
        if let provider = info.itemProviders(for: [.plainText]).first {
            _ = provider.loadObject(ofClass: NSString.self) { string, _ in
                guard let uuidString = string as? String, let sourceID = UUID(uuidString: uuidString) else { return }
                Task { @MainActor in
                    withAnimation(.easeInOut(duration: 0.18)) {
                        _ = model.handleTabDrop(sourceID, on: targetID, locationY: info.location.y, rowHeight: rowHeight)
                    }
                    model.finishSidebarDrag(sessionID: sessionID)
                }
            }
            return true
        }
        return false
    }

    private func updateIntent(for info: DropInfo) {
        guard let sourceID = model.draggingTabID else { return }
        let resolved = model.tabDropIntent(sourceID, on: targetID, locationY: info.location.y, rowHeight: rowHeight)
        let feedback = TabDropFeedback(targetID: targetID, intent: resolved)
        guard model.tabDropFeedback != feedback else { return }
        withAnimation(.easeOut(duration: 0.08)) { model.tabDropFeedback = feedback }
    }
}

struct SidebarBottomDropDelegate: DropDelegate {
    let model: BrowserModel

    private var targetTab: BrowserTab? {
        model.visibleTabs.filter { !$0.pinned && $0.folderID == nil }.last
    }

    func validateDrop(info: DropInfo) -> Bool {
        guard model.draggingFolderID == nil else { return false }
        if let sourceID = model.draggingTabID {
            guard let source = model.tabs.first(where: { $0.id == sourceID }) else { return false }
            return source.space == model.space
        }
        return info.hasItemsConforming(to: [UTType.plainText.identifier, "public.plain-text"])
    }

    func dropEntered(info: DropInfo) { updateFeedback() }

    func dropExited(info: DropInfo) {
        if let targetID = targetTab?.id, model.tabDropFeedback?.targetID == targetID {
            model.tabDropFeedback = nil
        }
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        guard validateDrop(info: info) else {
            model.tabDropFeedback = nil
            return DropProposal(operation: .forbidden)
        }
        model.autoScrollSidebarDuringDrag()
        updateFeedback()
        return DropProposal(operation: .move)
    }

    func performDrop(info: DropInfo) -> Bool {
        guard validateDrop(info: info) else { return false }
        let sessionID = model.sidebarDragSessionID
        if let sourceID = model.draggingTabID {
            var handled = false
            withAnimation(.easeInOut(duration: 0.18)) {
                if let target = targetTab {
                    if target.id != sourceID {
                        handled = model.moveTab(sourceID, relativeTo: target.id, after: true)
                    } else {
                        handled = true
                    }
                } else if let source = model.tabs.first(where: { $0.id == sourceID }) {
                    source.pinned = false
                    source.folderID = nil
                    model.save()
                    handled = true
                }
            }
            DispatchQueue.main.async {
                model.finishSidebarDrag(sessionID: sessionID)
            }
            return handled
        }
        if let provider = info.itemProviders(for: [.plainText]).first {
            _ = provider.loadObject(ofClass: NSString.self) { string, _ in
                guard let uuidString = string as? String, let sourceID = UUID(uuidString: uuidString) else { return }
                Task { @MainActor in
                    withAnimation(.easeInOut(duration: 0.18)) {
                        if let target = self.targetTab, target.id != sourceID {
                            _ = model.moveTab(sourceID, relativeTo: target.id, after: true)
                        } else if let source = model.tabs.first(where: { $0.id == sourceID && $0.space == model.space && !model.agentTabIDs.contains($0.id) }) {
                            source.pinned = false
                            source.folderID = nil
                            model.save()
                        }
                    }
                    model.finishSidebarDrag(sessionID: sessionID)
                }
            }
            return true
        }
        return false
    }

    private func updateFeedback() {
        guard let sourceID = model.draggingTabID else { return }
        if let target = targetTab, target.id != sourceID {
            let feedback = TabDropFeedback(targetID: target.id, intent: .after)
            guard model.tabDropFeedback != feedback else { return }
            withAnimation(.easeOut(duration: 0.08)) { model.tabDropFeedback = feedback }
        } else {
            model.tabDropFeedback = nil
        }
    }
}

/// Reorders pins, or pins a tab dropped beside one. Unlike sidebar rows, a
/// pin tile never offers a split.
struct PinDropDelegate: DropDelegate {
    let targetID: UUID
    let width: CGFloat
    let model: BrowserModel

    func validateDrop(info: DropInfo) -> Bool {
        guard model.draggingFolderID == nil, let sourceID = model.draggingTabID, sourceID != targetID,
              let source = model.tabs.first(where: { $0.id == sourceID }),
              let target = model.tabs.first(where: { $0.id == targetID }) else { return false }
        return source.space == target.space
    }

    func dropEntered(info: DropInfo) { updateFeedback(info) }
    func dropExited(info: DropInfo) {
        if model.tabDropFeedback?.targetID == targetID { model.tabDropFeedback = nil }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        guard validateDrop(info: info) else {
            model.tabDropFeedback = nil
            return DropProposal(operation: .forbidden)
        }
        updateFeedback(info)
        return DropProposal(operation: .move)
    }

    func performDrop(info: DropInfo) -> Bool {
        guard validateDrop(info: info), let sourceID = model.draggingTabID else { return false }
        let sessionID = model.sidebarDragSessionID
        let after = PinDropIntent.resolve(locationX: info.location.x, width: width) == .after
        var handled = false
        withAnimation(.easeInOut(duration: 0.18)) {
            handled = model.moveTab(sourceID, relativeTo: targetID, after: after)
        }
        DispatchQueue.main.async { model.finishSidebarDrag(sessionID: sessionID) }
        return handled
    }

    private func updateFeedback(_ info: DropInfo) {
        guard validateDrop(info: info) else { return }
        let feedback = TabDropFeedback(targetID: targetID, intent: PinDropIntent.resolve(locationX: info.location.x, width: width))
        guard model.tabDropFeedback != feedback else { return }
        withAnimation(.easeOut(duration: 0.08)) { model.tabDropFeedback = feedback }
    }
}

/// Makes the first pin of a Space from a dragged tab.
struct PinPlaceholderDropDelegate: DropDelegate {
    let model: BrowserModel
    @Binding var targeted: Bool

    private var source: BrowserTab? {
        guard model.draggingFolderID == nil, let id = model.draggingTabID, !model.agentTabIDs.contains(id) else { return nil }
        return model.tabs.first { $0.id == id && !$0.isPrivate && $0.space == model.space }
    }

    func validateDrop(info: DropInfo) -> Bool { source != nil }
    func dropEntered(info: DropInfo) { targeted = source != nil }
    func dropExited(info: DropInfo) { targeted = false }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        targeted = source != nil
        if targeted { model.tabDropFeedback = nil }
        return DropProposal(operation: targeted ? .move : .forbidden)
    }

    func performDrop(info: DropInfo) -> Bool {
        targeted = false
        guard let source else { return false }
        let sessionID = model.sidebarDragSessionID
        withAnimation(.easeInOut(duration: 0.18)) {
            source.pinned = true
            source.folderID = nil
            model.save()
        }
        DispatchQueue.main.async { model.finishSidebarDrag(sessionID: sessionID) }
        return true
    }
}

/// Moves a tab dropped on a Space icon into that Space.
struct SpaceDropDelegate: DropDelegate {
    let space: String
    let model: BrowserModel
    @Binding var targeted: Bool

    func validateDrop(info: DropInfo) -> Bool {
        guard model.draggingFolderID == nil, let id = model.draggingTabID, !model.agentTabIDs.contains(id),
              let source = model.tabs.first(where: { $0.id == id }) else { return false }
        return source.space != space
    }

    func dropEntered(info: DropInfo) { targeted = validateDrop(info: info) }
    func dropExited(info: DropInfo) { targeted = false }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        targeted = validateDrop(info: info)
        if targeted { model.tabDropFeedback = nil }
        return DropProposal(operation: targeted ? .move : .forbidden)
    }

    func performDrop(info: DropInfo) -> Bool {
        targeted = false
        guard validateDrop(info: info), let id = model.draggingTabID else { return false }
        let sessionID = model.sidebarDragSessionID
        withAnimation(.easeInOut(duration: 0.18)) { model.moveToSpace(id, space: space) }
        DispatchQueue.main.async { model.finishSidebarDrag(sessionID: sessionID) }
        return true
    }
}

private struct SidebarPageActions: View {
    @ObservedObject var model: BrowserModel
    let tab: BrowserTab?
    @ObservedObject var store: ExtensionStore
    let close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            SidebarActionGrid(model: model, tab: tab, close: close)
            Divider()
            Text(L("Erweiterungen")).font(.system(size: 12, weight: .semibold)).foregroundStyle(.secondary)
            ForEach(store.entries.filter { $0.enabled && store.errors[$0.id] == nil }) { entry in
                Button { close(); store.perform(entry, tab: tab) } label: {
                    Label(entry.name, systemImage: "puzzlepiece.extension")
                        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10)
                }.disabled(tab == nil)
            }
            if let tab {
                MarketplaceInstallButton(model: model, tab: tab, store: store)
            }
            Button {
                model.openExtensionsHub()
                // Activate the destination before dismantling this popover.
                // Closing first can discard the follow-up state change while
                // SwiftUI is reconciling the control-center presentation.
                DispatchQueue.main.async { close() }
            } label: {
                Label(L("Erweiterungen entdecken", "Explore extensions"), systemImage: "plus")
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10)
            }
            Divider()
            HStack(spacing: 10) {
                WebAppearanceButton(appearance: model.webAppearance, host: tab.flatMap { URL(string: $0.url)?.host })
                Text(L("Webseiten-Darkmode")).font(.system(size: 12))
                Spacer()
            }
        }
        .buttonStyle(YOBROButtonStyle()).font(.system(size: 12))
        .padding(16).frame(width: 290).background(paper).foregroundStyle(ink)
    }
}

private struct SidebarActionGrid: View {
    @ObservedObject var model: BrowserModel
    let tab: BrowserTab?
    let close: () -> Void
    private var hasPage: Bool { !(tab?.url.isEmpty ?? true) }

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                action(tab?.pinned == true ? "pin.fill" : "pin", tab?.pinned == true ? L("Loslösen") : L("Anpinnen")) { tab?.pinned.toggle(); model.save() }.disabled(tab == nil)
                action(model.splitID == nil ? "rectangle.split.2x1" : "rectangle", L("Splitview · ⇧⌘S")) { model.toggleSplit() }.disabled(tab == nil)
                action(model.activePageIsBookmarked ? "bookmark.fill" : "bookmark", L("Seite merken · ⌘D", "Bookmark page · ⌘D")) {
                    model.bookmarkActivePage()
                }.disabled(!hasPage)
                action("printer", L("Seite drucken · ⌘P", "Print page · ⌘P")) { tab?.printPage() }.disabled(!hasPage)
            }
            HStack(spacing: 8) {
                action("minus.magnifyingglass", L("Seite verkleinern · ⌘-", "Zoom out · ⌘-")) { tab?.changeZoom(-1) }.disabled(!hasPage)
                action("plus.magnifyingglass", L("Seite vergrößern · ⌘+", "Zoom in · ⌘+")) { tab?.changeZoom(1) }.disabled(!hasPage)
                action("text.magnifyingglass", L("Auf Seite suchen · ⌘F", "Find on page · ⌘F")) { tab?.showFind = true }.disabled(!hasPage)
                action("gearshape", L("Einstellungen", "Settings")) {
                    model.settingsSection = L("Allgemein", "General")
                    model.showSettings = true
                }
            }
        }
    }

    private func action(_ symbol: String, _ title: String, perform: @escaping () -> Void) -> some View {
        Button { close(); perform() } label: {
            Image(systemName: symbol).font(.system(size: 16)).frame(maxWidth: .infinity).frame(height: 40)
                .background(ink.opacity(0.05), in: RoundedRectangle(cornerRadius: Chrome.radius))
        }
        .yobroHelp(title)
        .accessibilityLabel(title)
    }
}

// MARK: - Pages

/// Shown when no tab is selected.
private struct StartPage: View {
    @ObservedObject var model: BrowserModel

    var body: some View {
        VStack(spacing: 14) {
            YOBROMark(size: 44).opacity(0.85)
            Text(L("Wohin als Nächstes?", "Where to next?")).font(.system(size: 17, weight: .medium))
            Button { model.openCommandBar(.newTab) } label: {
                HStack(spacing: 8) {
                    Image(systemName: "magnifyingglass")
                    Text(L("Suchen oder Adresse eingeben", "Search or enter address"))
                    Spacer(minLength: 24)
                    Text("⌘T").font(.system(size: 11, design: .monospaced)).opacity(0.5)
                }
                .font(.system(size: 13))
                .foregroundStyle(ink.opacity(0.6))
                .padding(.horizontal, 14).frame(width: 340, height: 40)
                .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
            }
            .buttonStyle(.plain)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(paper)
        .accessibilityIdentifier("new-tab-page")
    }
}

/// A tab that has no page yet asks where to go, inline.
struct NewTabPage: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel

    var body: some View {
        VStack(spacing: 18) {
            if tab.isPrivate {
                VStack(spacing: 8) {
                    Image(systemName: "eyeglasses").font(.system(size: 34, weight: .light))
                    Text(L("Privates Browsen", "Private browsing")).font(.system(size: 22, weight: .semibold))
                    Text(L("Cookies und Websitedaten bleiben nur bis zum Schließen des Tabs. Verlauf und Sitzung werden nicht gespeichert.", "Cookies and website data last only until you close the tab. History and session are not saved."))
                        .font(.system(size: 12)).foregroundStyle(ink.opacity(0.55))
                        .multilineTextAlignment(.center).frame(maxWidth: 460)
                }
            }
            if tab.id == model.activeID {
                CommandBar(model: model, mode: .current, inline: true)
            }
        }
        // Pinned to the top: centring would move the field as suggestions
        // grow below it.
        .padding(.top, tab.isPrivate ? 72 : 120)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(paper)
        .accessibilityIdentifier("new-tab-page")
    }
}

struct TabContent: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    var body: some View {
        ZStack(alignment: .top) {
            if tab.isExtensionsHub {
                ExtensionHubPage(model: model)
            } else if tab.url.isEmpty {
                NewTabPage(tab: tab, model: model)
            } else {
                paper
                WebSurface(webView: tab.webView)
                if let failure = tab.failure {
                    VStack(spacing: 14) {
                        Image(systemName: failure.symbol).font(.system(size: 30, weight: .light)).foregroundStyle(ink.opacity(0.6))
                        Text(failure.title).font(.system(size: 18, weight: .medium)).multilineTextAlignment(.center)
                        Text(failure.detail).font(.system(size: 12)).foregroundStyle(.secondary).multilineTextAlignment(.center)
                        HStack(spacing: 10) {
                            if failure.canRetry {
                                Button(L("Erneut versuchen")) { tab.failure = nil; tab.webView.reload() }
                                    .buttonStyle(YOBROPopupButtonStyle(prominent: true))
                            }
                            if tab.canGoBack {
                                Button(L("Zurück")) { tab.failure = nil; tab.webView.goBack() }.buttonStyle(YOBROPopupButtonStyle())
                            }
                        }
                    }.padding(40).frame(maxWidth: .infinity, maxHeight: .infinity).background(paper)
                        .accessibilityIdentifier("page-failure")
                }
            }
        }
        .overlay(alignment: .bottomLeading) {
            if !tab.url.isEmpty, let signals = (tab.webView as? AppearanceWebView)?.signals {
                HoveredLinkPill(signals: signals).padding(10)
            }
        }
        .overlay(alignment: .topTrailing) {
            if tab.showFind && !tab.url.isEmpty { PageFindBar(tab: tab).padding(12) }
        }
    }
}

struct WebSurface: NSViewRepresentable {
    let webView: WKWebView
    func makeNSView(context: Context) -> WKWebView { webView }
    func updateNSView(_ nsView: WKWebView, context: Context) {}
}

struct YOBROMark: View {
    var size: CGFloat

    private static let artwork: NSImage? = {
        guard let url = YOBROResources.bundle.url(forResource: "YOBROMark", withExtension: "png") else { return nil }
        return NSImage(contentsOf: url)
    }()

    var body: some View {
        Group {
            if let artwork = Self.artwork {
                Image(nsImage: artwork)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }
}

/// An orange glow around the window while the assistant or an external
/// agent is working, independent of the split layout.
private struct AgentActivityFrame: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var chat: SpaceChatStore

    var body: some View {
        if model.agentUsageActive {
            ZStack {
                RoundedRectangle(cornerRadius: 12)
                    .stroke(brandOrange.opacity(0.85), lineWidth: 2)
                    .padding(1)
                RoundedRectangle(cornerRadius: 17)
                    .stroke(brandOrange.opacity(0.25), lineWidth: 10)
                    .blur(radius: 12)
                    .padding(7)
            }
            // The browser content extends beneath the hidden title bar, so
            // the active-agent frame must claim that top inset as well.
            .ignoresSafeArea(.container, edges: .top)
            .allowsHitTesting(false)
            .transition(.opacity)
        }
    }
}
