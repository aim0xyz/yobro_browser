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
        .task {
            text = dialog.defaultText
            await Task.yield()
            focused = dialog.kind == .prompt || dialog.kind == .credentials
        }
    }
}

/// Keyboard selection for the address bar's history suggestions.
///
/// `nil` is the initial state and means "open exactly what was typed". Without
/// it, every keystroke reset the selection to the first suggestion and Enter
/// silently opened that history URL instead of the address the user entered.
enum SuggestionSelection {
    static func next(_ current: Int?, count: Int) -> Int? {
        guard count > 0 else { return nil }
        guard let current else { return 0 }
        return min(current + 1, count - 1)
    }

    /// Walking up past the first suggestion returns to the typed text.
    static func previous(_ current: Int?) -> Int? {
        guard let current, current > 0 else { return nil }
        return current - 1
    }

    static func destination(_ current: Int?, suggestions: [HistoryEntry], typed: String) -> String {
        guard let current, suggestions.indices.contains(current) else { return typed }
        return suggestions[current].url
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
            localMonitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseUp) { [weak self] event in
                self?.finishAfterCurrentEvent()
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

struct BrowserShell: View {
    @ObservedObject var model: BrowserModel
    @State private var sidebarOverlayVisible = false
    @State private var sidebarOverlayCloseTask: Task<Void, Never>?

    var body: some View {
        HStack(spacing: 0) {
            if model.showSidebar {
                sidebar.frame(width: 248)
            } else if !model.sidebarAutoHide {
                CompactSidebar(model: model, mail: model.mail)
            }
            Group {
                if model.agentWorkspaceVisible, let agent = model.agentTab {
                    HSplitView {
                        userAgentSplitPane
                            .frame(minWidth: 280, maxWidth: .infinity)
                            .layoutPriority(1)
                        agentSplitPane(agent)
                            .frame(minWidth: 294, idealWidth: 294)
                    }
                } else {
                    userWorkspace
                }
            }
            .ignoresSafeArea(.container, edges: .top)
            .frame(minWidth: 280)
            .overlay(alignment: .top) {
                if let undoID = model.closedTabUndoID {
                    ClosedTabUndo(model: model, undoID: undoID)
                        .padding(.top, 14)
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
            }
            AgentPanelRail(model: model)
        }
        .overlay(alignment: .leading) {
            if model.sidebarAutoHide && !model.showSidebar {
                ZStack(alignment: .leading) {
                    let overlayShape = SidebarOverlayShape(radius: 22)
                    // Keep the same native drop destinations mounted while the
                    // sidebar hides and reappears. Recreating them during a drag
                    // makes AppKit intermittently route the drop to the WebView.
                    sidebar
                        .frame(width: 248)
                        .frame(maxHeight: .infinity)
                        .background { overlayShape.fill(.ultraThinMaterial) }
                        .overlay { overlayShape.stroke(ink.opacity(0.14), lineWidth: 1) }
                        .shadow(color: .black.opacity(0.22), radius: 18, x: 8)
                        .offset(x: sidebarOverlayVisible ? 0 : -248)
                        .opacity(sidebarOverlayVisible ? 1 : 0)
                        .allowsHitTesting(sidebarOverlayVisible)
                        .onHover { inside in
                            guard sidebarOverlayVisible else { return }
                            if inside { revealSidebarOverlay() }
                            else { scheduleSidebarOverlayClose() }
                        }
                    if !sidebarOverlayVisible {
                        // The edge trigger stays narrow so the hidden sidebar
                        // leaves the webpage interactive right beside it.
                        Color.clear
                            .frame(width: 8)
                            .frame(maxHeight: .infinity)
                            .contentShape(Rectangle())
                            .onHover { inside in
                                if inside { revealSidebarOverlay() }
                            }
                    }
                }
                .frame(width: 248)
            }
        }
        // Keep the single native macOS traffic-light host above the temporary
        // sidebar material as well as the ordinary browser chrome.
        .overlay(alignment: .topLeading) {
            WindowControls()
                .frame(width: 60, height: 32)
                .padding(.leading, model.showSidebar || model.sidebarAutoHide ? 17 : 10)
                .padding(.top, 10)
                .opacity(sidebarChromeVisible ? 1 : 0)
                .allowsHitTesting(sidebarChromeVisible)
                .accessibilityHidden(!sidebarChromeVisible)
                .ignoresSafeArea(.container, edges: .top)
        }
        .ignoresSafeArea(.container, edges: .top)
        .background(YOBROArrowCursorRegion().ignoresSafeArea())
        .background(LinearGradient(colors: [YOBROTheme.chromeTop, YOBROTheme.chromeBottom], startPoint: .topLeading, endPoint: .bottomTrailing))
        .overlay(alignment: .bottomTrailing) {
            // Keep this observer mounted even while there are no downloads so
            // the first download can make the status appear immediately.
            ActiveDownloadStatus(model: model, downloads: model.downloads)
                .padding(.trailing, 22).padding(.bottom, 22)
        }
        .overlay {
            if model.agentWorkspaceVisible, model.agentTab != nil {
                ZStack {
                    RoundedRectangle(cornerRadius: 17)
                        .stroke(brandOrange.opacity(0.28), lineWidth: 10)
                        .blur(radius: 12)
                        .padding(7)
                    RoundedRectangle(cornerRadius: 19)
                        .stroke(brandOrange.opacity(0.14), lineWidth: 20)
                        .blur(radius: 24)
                        .padding(10)
                }
                // The browser content extends beneath the hidden title bar, so
                // the active-agent frame must claim that top inset as well.
                .ignoresSafeArea(.container, edges: .top)
                .allowsHitTesting(false)
                .transition(.opacity)
            }
        }
        .overlay {
            if let dialog = model.webPageDialog {
                WebPageDialogOverlay(model: model, dialog: dialog).id(dialog.id)
            } else if let notice = model.notice {
                YOBRODialogOverlay(icon: "info.circle.fill", title: "YoBro", message: notice, confirmTitle: "OK", confirm: { model.notice = nil })
            }
        }
        .foregroundStyle(ink)
        .environment(\.locale, YOBROLanguage.locale)
        .animation(.easeInOut(duration: 0.2), value: model.showAgent)
        .animation(.easeInOut(duration: 0.2), value: model.showSidebar)
        .onChange(of: model.showSidebar) { _, shown in
            if shown { sidebarOverlayVisible = false }
        }
        .onChange(of: model.sidebarAutoHide) { _, enabled in
            if !enabled { sidebarOverlayVisible = false }
        }
        .onChange(of: model.draggingTabID) { _, _ in sidebarDragStateChanged() }
        .onChange(of: model.draggingFolderID) { _, _ in sidebarDragStateChanged() }
        .animation(.easeInOut(duration: 0.2), value: model.agentWorkspaceVisible)
        .sheet(item: $model.librarySection) { section in LibraryView(model: model, downloads: model.downloads, section: section) }
        .sheet(isPresented: $model.showOnboarding) { OnboardingView(model: model) }
        .sheet(isPresented: $model.showSettings) { BrowserSettings(model: model, section: model.settingsSection) }
        .sheet(isPresented: $model.showProfileEditor) {
            if let profiles = model.profileSession { ProfileEditor(profiles: profiles, creating: model.creatingProfile) }
        }
        .sheet(isPresented: $model.showPalette) { CommandPalette(model: model) }
        .background(SidebarDragEndObserver(model: model))
    }

    private func revealSidebarOverlay() {
        sidebarOverlayCloseTask?.cancel()
        sidebarOverlayCloseTask = nil
        sidebarOverlayVisible = true
    }

    private var sidebarChromeVisible: Bool {
        !model.sidebarAutoHide || model.showSidebar || sidebarOverlayVisible
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
            sidebarOverlayVisible = false
            restoreBrowserKeyboardFocus()
        }
    }

    private var sidebarDragIsActive: Bool {
        model.draggingTabID != nil || model.draggingFolderID != nil
    }


    private func pointerIsInsideSidebarOverlay() -> Bool {
        guard let window = NSApp.keyWindow else { return false }
        let point = window.mouseLocationOutsideOfEventStream
        return point.x >= 0 && point.x <= 248 && point.y >= 0 && point.y <= window.frame.height
    }

    private func sidebarDragStateChanged() {
        if sidebarDragIsActive {
            model.hoveredFolderID = nil
            revealSidebarOverlay()
        } else if !pointerIsInsideSidebarOverlay() {
            scheduleSidebarOverlayClose()
        }
    }

    private func restoreBrowserKeyboardFocus() {
        guard !model.showMail, let webView = model.active?.webView, let window = webView.window else { return }
        window.makeFirstResponder(webView)
    }

    @ViewBuilder
    private var userWorkspace: some View {
        if model.showMail {
            mailWorkspace
        } else {
            VStack(spacing: 0) {
                Group {
                    if let active = model.activeID, let pair = model.splitPairs.first(where: { $0.contains(active) }),
                       let first = model.tabs.first(where: { $0.id == pair.first }), let second = model.tabs.first(where: { $0.id == pair.second }) {
                        HSplitView {
                            splitPane(first).frame(minWidth: 180)
                            splitPane(second).frame(minWidth: 180)
                        }
                    } else if let tab = model.active { TabContent(tab: tab, model: model).id(tab.id) }
                    else { NewTabPage(tab: nil, model: model) }
                }.padding(10)
            }
        }
    }

    private var mailWorkspace: some View {
        MailWorkspace(store: model.mail, close: { model.presentMail(false) }, openURL: { model.newTab(url: $0.absoluteString) }, askAgent: { item, body, prompt in
            model.chat.stageMail(item, body: body, prompt: prompt, space: model.space)
            model.showAgent = true
        })
        .clipShape(RoundedRectangle(cornerRadius: 14)).padding(10)
    }

    @ViewBuilder
    private var userAgentSplitPane: some View {
        if model.showMail {
            mailWorkspace
        } else if let tab = model.active {
            parallelPane(tab, owner: L("DU · LINKS", "YOU · LEFT"), color: moss, interactive: true)
        } else {
            paper
        }
    }

    private func agentSplitPane(_ tab: BrowserTab) -> some View {
        parallelPane(tab, owner: L("AGENT · RECHTS", "AGENT · RIGHT"), color: brandOrange, interactive: false)
            .accessibilityIdentifier("agent-workspace")
    }

    private func parallelPane(_ tab: BrowserTab, owner: String, color: Color, interactive: Bool) -> some View {
        VStack(spacing: 4) {
            HStack(spacing: 8) {
                Circle().fill(color).frame(width: 7, height: 7)
                Text(tab.title).font(.system(size: 11, weight: .medium)).lineLimit(1)
                Spacer(minLength: 8)
                Text(owner).font(.system(size: 8, weight: .bold, design: .rounded)).tracking(0.8).foregroundStyle(color)
                if !interactive {
                    Button { model.pauseAgentWorkspace() } label: { Image(systemName: "xmark").frame(width: 22, height: 22) }
                        .buttonStyle(.plain).help(L("Agent-Sitzung beenden", "End agent session"))
                }
            }.padding(.horizontal, 9).frame(height: 27)
            TabContent(tab: tab, model: model).id(tab.id).allowsHitTesting(interactive)
        }.padding(6)
    }

    private func splitPane(_ tab: BrowserTab) -> some View {
        VStack(spacing: 4) {
            HStack {
                Button { model.select(tab.id) } label: {
                    HStack { Circle().fill(model.activeID == tab.id ? moss : ink.opacity(0.2)).frame(width: 6, height: 6); Text(tab.title).lineLimit(1); Spacer() }
                }.buttonStyle(.plain).help(L("Diese Seite in der Adressleiste auswählen"))
                Button { model.separateSplit(tab.id) } label: { Image(systemName: "rectangle") }.buttonStyle(.plain).help(L("Splitview trennen · beide Tabs behalten"))
            }.font(.system(size: 11)).padding(.horizontal, 8).frame(height: 26)
            TabContent(tab: tab, model: model).id(tab.id)
        }
    }

    private var sidebarToggle: some View {
        Button { model.showSidebar.toggle() } label: {
            Image(systemName: "sidebar.left")
                .font(.system(size: 14)).foregroundStyle(ink.opacity(0.55))
                .frame(width: 32, height: 32)
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 32))
        .help(model.showSidebar ? L("Seitenleiste einklappen · ⌘S") : L("Seitenleiste einblenden · ⌘S"))
        .accessibilityLabel(model.showSidebar ? L("Seitenleiste einklappen") : L("Seitenleiste einblenden"))
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 0) {
                Color.clear.frame(width: 60, height: 32)
                Spacer(minLength: 8)
                if let tab = model.active { SidebarHistoryControls(tab: tab) }
                else { EmptySidebarHistoryControls() }
                sidebarToggle
            }.frame(height: 32, alignment: .center)
                .padding(.horizontal, -7).padding(.top, 10).padding(.bottom, 16)
            HStack(spacing: 10) {
                YOBROMark(size: 30)
                Text("YoBro").font(.system(size: 29, weight: .semibold, design: .rounded)).tracking(-1.4)
                Spacer()
                Text("PREVIEW").font(.system(size: 8, weight: .bold, design: .monospaced)).tracking(1).foregroundStyle(moss)
            }.padding(.bottom, 22)
            if let tab = model.active, !tab.isNote {
                SidebarNavigation(model: model, tab: tab)
            } else {
                EmptySidebarNavigation(model: model, opensNewTab: model.active != nil)
            }

            MailSidebarButton(store: model.mail, active: model.showMail) { model.presentMail() }
                .padding(.top, 10).padding(.bottom, 12)

            SpaceStrip(model: model)

            SidebarTabList(model: model)
            LibraryButtons(model: model, downloads: model.downloads)
                .padding(.top, 4).padding(.bottom, 6)
            Button { model.showAgent.toggle() } label: {
                VStack(alignment: .leading, spacing: 9) {
                    HStack(spacing: 7) {
                        Circle().fill(model.agentWorkspaceVisible ? brandOrange : model.agentEnabled && model.bridgeStatus == L("Bereit") ? moss : .orange).frame(width: 6, height: 6)
                        Text(agentSidebarTitle).font(.system(size: 11, weight: .medium))
                        Spacer()
                        Image(systemName: model.agentWorkspaceVisible ? "rectangle.split.2x1.fill" : "arrow.up.right").font(.system(size: 10))
                    }
                    Text(model.agentWorkspaceVisible ? L("Du links · Agent rechts", "You left · agent right") : model.events.first.map { L("Zuletzt: \($0.action)", "Last: \($0.action)") } ?? L("Ein Browser. Für euch beide."))
                        .font(.system(size: 10)).foregroundStyle(ink.opacity(0.5))
                }.padding(13).background(YOBROTheme.surface.opacity(0.32), in: RoundedRectangle(cornerRadius: 12))
            }.buttonStyle(YOBROButtonStyle())
            if let profiles = model.profileSession {
                ProfileMenu(profiles: profiles, model: model).padding(.top, 8).padding(.bottom, 6)
            }

        }.padding(.horizontal, 17)
    }

    private var agentSidebarTitle: String {
        if !model.agentEnabled { return L("Agentenzugriff pausiert", "Agent access paused") }
        if model.agentAction != nil { return L("Agent arbeitet rechts", "Agent working on the right") }
        if model.agentWorkspaceVisible { return L("Agent-Browser rechts geöffnet", "Agent browser open on the right") }
        return L("Bereit für deine Agenten", "Ready for your agents")
    }

}

/// The panel and its edge button share one animating layout guide. The button's
/// right edge is pinned to the browser/panel divider, so it cannot jump ahead of
/// the sidebar transition when the parent HStack is resized.
private struct AgentPanelRail: View {
    @ObservedObject var model: BrowserModel

    var body: some View {
        ZStack(alignment: .leading) {
            SpaceChatPanel(browser: model, chat: model.chat)
                .frame(width: 320)
                .offset(x: model.showAgent ? 0 : 320)
                .opacity(model.showAgent ? 1 : 0)
                .allowsHitTesting(model.showAgent)
        }
        .frame(width: model.showAgent ? 320 : 0)
        .overlay(alignment: .leading) {
            AgentQuickAccessButton(model: model)
                .frame(width: 22, alignment: .trailing)
                .offset(x: -22)
        }
        .animation(.easeInOut(duration: 0.24), value: model.showAgent)
    }
}

/// A slim vertical edge tab for the chat panel. Its rectangular silhouette is
/// deliberately distinct from the pill-shaped status controls elsewhere.
private struct AgentQuickAccessButton: View {
    @ObservedObject var model: BrowserModel
    @State private var hovered = false

    var body: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.24)) { model.showAgent.toggle() }
        } label: {
            VStack(spacing: 0) {
                Text("A\nG\nE\nN\nT")
                    .font(.system(size: 7, weight: .bold, design: .rounded))
                    .tracking(0.4)
                    .multilineTextAlignment(.center)
                    .lineSpacing(0)
            }
            .foregroundStyle(model.showAgent ? paper : moss)
            .frame(width: hovered ? 22 : 18, height: hovered ? 108 : 96)
            .background(model.showAgent ? moss : YOBROTheme.surface.opacity(0.94), in: RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(model.showAgent ? Color.clear : moss.opacity(0.22)))
            .shadow(color: .black.opacity(0.14), radius: 7, x: -2, y: 2)
            .contentShape(RoundedRectangle(cornerRadius: 6))
            .opacity(hovered ? 1 : 0.52)
        }
        .buttonStyle(.plain)
        .onHover { value in
            withAnimation(.easeInOut(duration: 0.18)) { hovered = value }
        }
        .help(model.showAgent ? L("Agentenpanel schließen", "Close agent panel") : L("Agentenpanel öffnen", "Open agent panel"))
        .accessibilityLabel(model.showAgent ? L("Agentenpanel schließen", "Close agent panel") : L("Agentenpanel öffnen", "Open agent panel"))
        .accessibilityIdentifier("agent-quick-access")
    }
}

private struct SidebarTabList: View {
    @ObservedObject var model: BrowserModel
    @State private var tabFrames: [UUID: CGRect] = [:]
    @State private var viewportFrame: CGRect = .null
    @State private var scrollRevision = 0

    var body: some View {
        ScrollViewReader { proxy in
            ZStack {
                ScrollView {
                    VStack(alignment: .leading, spacing: 5) {
                    if model.visibleTabs.contains(where: { $0.pinned }) {
                        sectionLabel(L("ANGEPINNT"))
                        VStack(spacing: 0) {
                            ForEach(model.visibleTabs.filter { $0.pinned }) { TabRow(tab: $0, model: model) }
                        }
                    }
                    ForEach(model.folders.filter { $0.space == model.space }) { folder in SpaceFolderRow(folder: folder, model: model) }
                    HStack {
                        sectionLabel(L("DEINE TABS"))
                        Spacer()
                        Text("\(model.visibleTabs.filter { !$0.pinned }.count)").font(.system(size: 10, design: .monospaced)).opacity(0.4).padding(.top, 22)
                    }
                    VStack(spacing: 0) {
                        ForEach(model.visibleTabs.filter { !$0.pinned && $0.folderID == nil }) { TabRow(tab: $0, model: model) }
                    }
                    Button { model.requestNewTab() } label: {
                        HStack(spacing: 10) {
                            Image(systemName: "plus").font(.system(size: 14))
                            Text(L("Neuer Tab")).font(.system(size: 12))
                            Spacer()
                            Text("⌘ T").font(.system(size: 10, design: .monospaced)).opacity(0.5)
                        }.foregroundStyle(ink.opacity(0.55)).padding(11)
                    }.buttonStyle(YOBROButtonStyle()).padding(.top, 5)
                    Button { model.newNote() } label: {
                        HStack(spacing: 10) {
                            Image(systemName: "note.text.badge.plus").font(.system(size: 14))
                            Text(L("Neue Notiz", "New note")).font(.system(size: 12))
                            Spacer()
                            Text("⌘ N").font(.system(size: 10, design: .monospaced)).opacity(0.5)
                        }.foregroundStyle(ink.opacity(0.55)).padding(11)
                    }.buttonStyle(YOBROButtonStyle())
                    Button { model.requestPrivateTab() } label: {
                        HStack(spacing: 10) {
                            Image(systemName: "eyeglasses").font(.system(size: 14))
                            Text(L("Privater Tab", "Private tab")).font(.system(size: 12))
                            Spacer()
                            Text("⇧ ⌘ T").font(.system(size: 10, design: .monospaced)).opacity(0.5)
                        }.foregroundStyle(ink.opacity(0.55)).padding(11)
                    }.buttonStyle(YOBROButtonStyle())
                    }
                    .background(SidebarScrollObserver(changed: {
                        // The jump control is hidden while Mail is open. Updating
                        // state for every clip-view pixel there needlessly
                        // invalidates the WebKit-backed mail workspace.
                        guard !model.showMail else { return }
                        scrollRevision &+= 1
                    }, connected: { model.sidebarScrollView = $0 }))
                }
                .scrollIndicators(.hidden)
                .background(GeometryReader { geometry in
                    Color.clear.preference(key: SidebarViewportFrameKey.self, value: geometry.frame(in: .global))
                })
                .onPreferenceChange(SidebarTabFrameKey.self) { tabFrames = $0 }
                .onPreferenceChange(SidebarViewportFrameKey.self) { viewportFrame = $0 }
                .onChange(of: model.activeID) { _, _ in revealActiveTab(using: proxy, animated: true) }
                .onChange(of: model.space) { _, _ in revealActiveTab(using: proxy, animated: false) }

                if !model.showMail, model.activeID != nil {
                    VStack {
                        Spacer()
                        HStack {
                            Spacer()
                            SidebarActiveTabJump(direction: jumpDirection) {
                                revealActiveTab(using: proxy, animated: true)
                            }
                        }
                    }
                    .padding(.trailing, 7).padding(.bottom, 7)
                }
            }
        }
    }

    private func sectionLabel(_ title: String) -> some View {
        Text(title).font(.system(size: 9, weight: .semibold)).tracking(1.4).foregroundStyle(ink.opacity(0.4)).padding(.top, 23).padding(.bottom, 9).padding(.leading, 10)
    }

    private var jumpDirection: SidebarJumpDirection? {
        _ = scrollRevision
        guard !model.showMail, let id = model.activeID else { return nil }
        if let direction = SidebarVisibility.jumpDirection(for: tabFrames[id], viewport: viewportFrame) { return direction }
        let ordered = presentedTabIDs
        let visibleIndices: [Int] = tabFrames.compactMap { entry -> Int? in
            guard entry.value.intersects(viewportFrame) else { return nil }
            return ordered.firstIndex(of: entry.key)
        }
        return SidebarVisibility.inferredJumpDirection(activeIndex: ordered.firstIndex(of: id), visibleIndices: visibleIndices)
    }

    private var presentedTabIDs: [UUID] {
        var result = model.visibleTabs.filter(\.pinned).map(\.id)
        for folder in model.folders where folder.space == model.space {
            let tabs = model.visibleTabs.filter { !$0.pinned && $0.folderID == folder.id }
            if folder.collapsed {
                if let active = tabs.first(where: { $0.id == model.activeID }) { result.append(active.id) }
            } else {
                result.append(contentsOf: tabs.map(\.id))
            }
        }
        result.append(contentsOf: model.visibleTabs.filter { !$0.pinned && $0.folderID == nil }.map(\.id))
        return result
    }

    private func revealActiveTab(using proxy: ScrollViewProxy, animated: Bool) {
        guard let id = model.activeID else { return }
        Task { @MainActor in
            await Task.yield()
            if animated { withAnimation(.easeInOut(duration: 0.22)) { proxy.scrollTo(id, anchor: .center) } }
            else { proxy.scrollTo(id, anchor: .center) }
        }
    }
}

private struct SidebarActiveTabJump: View {
    let direction: SidebarJumpDirection?
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: direction == .up ? "arrow.up" : direction == .down ? "arrow.down" : "arrow.up.and.down")
            .font(.system(size: 11, weight: .bold))
            .foregroundStyle(paper)
            .frame(width: 30, height: 30)
            .background(moss, in: Circle())
            .shadow(color: .black.opacity(0.16), radius: 7, y: 3)
        }
        .buttonStyle(.plain)
        .help(L("Zum aktiven Tab springen", "Jump to active tab"))
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
            HStack(spacing: 11) {
                ZStack {
                    Circle().stroke(moss.opacity(0.2), lineWidth: 3)
                    if current.expected > 0 {
                        Circle().trim(from: 0, to: current.fraction).stroke(moss, style: StrokeStyle(lineWidth: 3, lineCap: .round)).rotationEffect(.degrees(-90))
                    } else {
                        ProgressView().controlSize(.small).tint(moss)
                    }
                    Image(systemName: "arrow.down").font(.system(size: 9, weight: .bold)).foregroundStyle(moss)
                }.frame(width: 30, height: 30)
                VStack(alignment: .leading, spacing: 3) {
                    Text(current.name).font(.system(size: 11, weight: .semibold)).lineLimit(1)
                    HStack(spacing: 4) {
                        Text(progressLabel(current))
                        if downloads.activeCount > 1 {
                            Text("· " + String(format: L("+%d weitere", "+%d more"), downloads.activeCount - 1))
                        }
                    }.font(.system(size: 9)).foregroundStyle(.secondary).lineLimit(1)
                }.frame(width: 190, alignment: .leading)
                Button { downloads.cancel(current.id) } label: { Image(systemName: "xmark").frame(width: 24, height: 24) }
                    .buttonStyle(.plain).help(L("Download abbrechen"))
                Button { model.librarySection = .downloads } label: { Image(systemName: "chevron.right").frame(width: 24, height: 24) }
                    .buttonStyle(.plain).help(L("Downloads öffnen", "Open downloads"))
            }
            .padding(11)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(ink.opacity(0.1)))
            .shadow(color: .black.opacity(0.15), radius: 14, y: 5)
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

private struct ClosedTabUndo: View {
    @ObservedObject var model: BrowserModel
    let undoID: UUID

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: 14))
                .foregroundStyle(ink.opacity(0.58))
            Text(L("Tab geschlossen", "Tab closed"))
                .font(.system(size: 12, weight: .medium))
            Button(L("Rückgängig", "Undo")) { model.reopenTab() }
                .buttonStyle(.plain)
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(moss)
                .padding(.horizontal, 9)
                .frame(height: 28)
                .background(moss.opacity(0.13), in: Capsule())
                .accessibilityHint(L("Stellt den Tab in seinem ursprünglichen Space und Ordner wieder her.", "Restores the tab to its original space and folder."))
        }
        .padding(.leading, 14)
        .padding(.trailing, 8)
        .frame(height: 44)
        .background(.regularMaterial, in: Capsule())
        .overlay { Capsule().stroke(YOBROTheme.border.opacity(0.7), lineWidth: 1) }
        .shadow(color: .black.opacity(0.16), radius: 12, y: 5)
        .task(id: undoID) {
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
    @State private var hover = false
    @State private var isRenaming = false
    @State private var renameText = ""
    @FocusState private var renameIsFocused: Bool
    private var dropIntent: TabDropIntent? {
        model.tabDropFeedback?.targetID == tab.id ? model.tabDropFeedback?.intent : nil
    }
    var body: some View {
        HStack(spacing: 0) {
            HStack(spacing: 10) {
                    Group {
                        if tab.isNote { Image(systemName: "note.text").font(.system(size: 13)).foregroundStyle(moss) }
                        else if tab.isPrivate { Image(systemName: "eyeglasses").font(.system(size: 13)).foregroundStyle(moss) }
                        else if let icon = tab.favicon { Image(nsImage: icon).resizable().scaledToFit() }
                        else { Image(systemName: tab.url.isEmpty ? "circle.dotted" : "globe").font(.system(size: 13)).foregroundStyle(moss) }
                    }.frame(width: 17, height: 17)
                    if model.splitPairs.contains(where: { $0.contains(tab.id) }) { Image(systemName: "rectangle.split.2x1").font(.system(size: 10)).foregroundStyle(moss) }
                    if isRenaming {
                        TextField(L("Tabname", "Tab name"), text: $renameText)
                            .textFieldStyle(.plain)
                            .font(.system(size: 12, weight: .medium))
                            .focused($renameIsFocused)
                            .onSubmit { finishRenaming() }
                            .onExitCommand { cancelRenaming() }
                    } else {
                        Text(tab.sidebarTitle)
                            .font(.system(size: 12, weight: tab.id == model.activeID ? .medium : .regular))
                            .lineLimit(1)
                            .onTapGesture(count: 2) {
                                if tab.id == model.activeID { beginRenaming() }
                            }
                    }
                    Spacer(minLength: 0)
                }
                .padding(.leading, 11)
                .frame(maxWidth: .infinity, minHeight: 44)
                .contentShape(Rectangle())
                .onTapGesture {
                    guard !isRenaming else { return }
                    model.select(tab.id)
                }
            Button { model.closeSidebarTab(tab.id) } label: {
                ZStack {
                    if tab.folderID != nil && !tab.isNote {
                        Image(systemName: tab.id == model.activeID && !tab.isSuspended ? "minus" : "xmark")
                            .font(.system(size: 10, weight: .semibold))
                            .foregroundStyle(tab.id == model.activeID && !tab.isSuspended ? moss : ink.opacity(0.6))
                    } else if hover {
                        Image(systemName: "xmark").font(.system(size: 10, weight: .semibold))
                    } else if tab.loading {
                        ProgressView().controlSize(.mini).allowsHitTesting(false)
                    }
                }.frame(width: 36, height: 40).contentShape(Rectangle())
            }.buttonStyle(YOBROButtonStyle(minimumSize: 36)).help(tab.folderID != nil && !tab.isNote && tab.id == model.activeID && !tab.isSuspended
                ? L("Seite schließen · Tab im Ordner behalten", "Close page · keep tab in folder")
                : L("Tab löschen", "Delete tab"))
        }.frame(height: 44)
            .id(tab.id)
            .background(GeometryReader { geometry in
                Color.clear.preference(key: SidebarTabFrameKey.self, value: [tab.id: geometry.frame(in: .global)])
            })
            .background(tab.id == model.activeID ? YOBROTheme.surface.opacity(0.75) : hover ? YOBROTheme.surface.opacity(0.24) : .clear, in: RoundedRectangle(cornerRadius: 9))
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
                    Image(systemName: tab.isNote ? "note.text" : "globe")
                    Text(tab.sidebarTitle).lineLimit(1)
                }
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(ink)
                .padding(.horizontal, 10).frame(width: 190, height: 34, alignment: .leading)
                .background(paper.opacity(0.94), in: RoundedRectangle(cornerRadius: 8))
            }
            // The visual gap belongs to the preceding row's drop destination.
            // Otherwise the exact space between tabs has no native drop target.
            .padding(.bottom, reorderGap)
            .contentShape(Rectangle())
            .onDrop(of: [UTType.plainText], delegate: TabReorderDropDelegate(
                targetID: tab.id, rowHeight: 44 + reorderGap, model: model
            ))
            .contextMenu {
                if !tab.isPrivate { Button(tab.pinned ? L("Loslösen") : L("Anpinnen")) { tab.pinned.toggle(); model.save() } }
                Menu(L("In Space verschieben")) {
                    ForEach(model.spaces.filter { $0 != tab.space }, id: \.self) { space in Button(space) { model.moveToSpace(tab.id, space: space) } }
                }
                if !tab.isPrivate { Menu(L("In Ordner verschieben")) {
                    Button(L("Ohne Ordner")) { model.moveToFolder(tab.id, folder: nil) }
                    ForEach(model.folders.filter { $0.space == tab.space }) { folder in Button(folder.name) { model.moveToFolder(tab.id, folder: folder.id) } }
                } }
                if model.splitPairs.contains(where: { $0.contains(tab.id) }) { Button(L("Splitview trennen")) { model.separateSplit(tab.id) } }
                Button(tab.folderID != nil && tab.id == model.activeID && !tab.isSuspended
                    ? L("Seite schließen · im Ordner behalten", "Close page · keep in folder")
                    : L("Tab löschen", "Delete tab")) { model.closeSidebarTab(tab.id) }
                if tab.isNote {
                    Button(L("Notiz duplizieren", "Duplicate note")) {
                        let copy = model.newNote(title: tab.title, space: tab.space, folderID: tab.folderID)
                        copy.noteContent = tab.noteContent; copy.noteRTF = tab.noteRTF; model.save()
                    }
                } else {
                    Button(L("Tab duplizieren")) { model.newTab(url: tab.url, space: tab.space, isPrivate: tab.isPrivate) }
                }
                Button(L("Geschlossenen Tab wieder öffnen")) { model.reopenTab() }.disabled(model.closedTabs.isEmpty)
            }
            .help(tab.sidebarHelp)
            .accessibilityLabel(tab.sidebarTitle)
    }

    private func beginRenaming() {
        renameText = tab.sidebarTitle
        isRenaming = true
        Task { @MainActor in
            await Task.yield()
            renameIsFocused = true
        }
    }

    private func finishRenaming() {
        guard isRenaming else { return }
        tab.rename(renameText)
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

private struct TabDropIndicatorModifier: ViewModifier {
    let intent: TabDropIntent?
    let compact: Bool
    let gap: CGFloat

    func body(content: Content) -> some View {
        content
            .overlay {
                if intent == .split {
                    ZStack {
                        RoundedRectangle(cornerRadius: 9)
                            .fill(moss.opacity(0.18))
                            .overlay { RoundedRectangle(cornerRadius: 9).stroke(moss, lineWidth: 2) }
                        HStack(spacing: 5) {
                            Image(systemName: "rectangle.split.2x1")
                            if !compact { Text(L("Split Screen", "Split screen")) }
                        }
                        .font(.system(size: compact ? 12 : 10, weight: .bold))
                        .foregroundStyle(moss)
                        .padding(.horizontal, compact ? 0 : 7)
                        .frame(height: 22)
                        .background(YOBROTheme.chromeTop.opacity(0.94), in: Capsule())
                    }
                    .transition(.opacity.combined(with: .scale(scale: 0.97)))
                    .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .top) {
                if intent == .before { insertionLine.offset(y: -(gap / 2 + 1.5)) }
            }
            .overlay(alignment: .bottom) {
                if intent == .after { insertionLine.offset(y: gap / 2 + 1.5) }
            }
            .animation(.easeOut(duration: 0.10), value: intent)
    }

    private var insertionLine: some View {
        Capsule()
            .fill(moss)
            .frame(height: 3)
            .padding(.horizontal, compact ? 2 : 4)
            .shadow(color: moss.opacity(0.35), radius: 2)
            .transition(.opacity)
            .allowsHitTesting(false)
    }
}

extension View {
    func tabDropIndicator(_ intent: TabDropIntent?, compact: Bool = false, gap: CGFloat = 5) -> some View {
        modifier(TabDropIndicatorModifier(intent: intent, compact: compact, gap: gap))
    }
}

struct TabReorderDropDelegate: DropDelegate {
    let targetID: UUID
    let rowHeight: CGFloat
    let model: BrowserModel

    func validateDrop(info: DropInfo) -> Bool {
        guard let sourceID = model.draggingTabID, sourceID != targetID,
              let source = model.tabs.first(where: { $0.id == sourceID }),
              let target = model.tabs.first(where: { $0.id == targetID }) else { return false }
        return source.space == target.space
    }

    func dropEntered(info: DropInfo) { updateIntent(for: info) }
    func dropExited(info: DropInfo) {
        if model.tabDropFeedback?.targetID == targetID { model.tabDropFeedback = nil }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        guard model.draggingTabID != nil else {
            model.tabDropFeedback = nil
            return DropProposal(operation: .forbidden)
        }
        model.autoScrollSidebarDuringDrag()
        updateIntent(for: info)
        return DropProposal(operation: .move)
    }

    func performDrop(info: DropInfo) -> Bool {
        guard let sourceID = model.draggingTabID else { return false }
        let sessionID = model.sidebarDragSessionID
        var handled = false
        withAnimation(.easeInOut(duration: 0.18)) {
            handled = model.handleTabDrop(sourceID, on: targetID, locationY: info.location.y, rowHeight: rowHeight)
        }
        DispatchQueue.main.async {
            model.finishSidebarDrag(sessionID: sessionID)
        }
        return handled
    }

    private func updateIntent(for info: DropInfo) {
        guard let sourceID = model.draggingTabID else { return }
        let resolved = model.tabDropIntent(sourceID, on: targetID, locationY: info.location.y, rowHeight: rowHeight)
        let feedback = TabDropFeedback(targetID: targetID, intent: resolved)
        guard model.tabDropFeedback != feedback else { return }
        withAnimation(.easeOut(duration: 0.08)) { model.tabDropFeedback = feedback }
    }
}

private struct SidebarHistoryControls: View {
    @ObservedObject var tab: BrowserTab

    var body: some View {
        HStack(spacing: 0) {
            Button { tab.webView.goBack() } label: { Image(systemName: "chevron.left").frame(width: 32, height: 32) }
                .disabled(!tab.canGoBack).help(L("Zurück"))
            Button { tab.webView.goForward() } label: { Image(systemName: "chevron.right").frame(width: 32, height: 32) }
                .disabled(!tab.canGoForward).help(L("Vorwärts"))
            Button { if tab.loading { tab.webView.stopLoading() } else { tab.webView.reload() } } label: {
                Image(systemName: tab.loading ? "xmark" : "arrow.clockwise").frame(width: 32, height: 32)
            }.disabled(tab.url.isEmpty).help(tab.loading ? L("Laden stoppen", "Stop loading") : L("Neu laden"))
        }
        .buttonStyle(YOBROButtonStyle(minimumSize: 32))
        .font(.system(size: 12)).foregroundStyle(ink.opacity(0.65))
    }
}

private struct EmptySidebarHistoryControls: View {
    var body: some View {
        HStack(spacing: 0) {
            Image(systemName: "chevron.left").frame(width: 32, height: 32)
            Image(systemName: "chevron.right").frame(width: 32, height: 32)
            Image(systemName: "arrow.clockwise").frame(width: 32, height: 32)
        }
        .font(.system(size: 12))
        .foregroundStyle(ink.opacity(0.22))
        .accessibilityHidden(true)
    }
}

private struct EmptySidebarNavigation: View {
    @ObservedObject var model: BrowserModel
    var opensNewTab = false
    @StateObject private var emptyLogin = LoginAutofill()

    var body: some View {
        HStack(spacing: 0) {
            Button {
                if opensNewTab { model.requestNewTab() }
                model.focusAddress = true
            } label: {
                HStack(spacing: 7) {
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 10)).foregroundStyle(moss)
                    Text(L("Suchen oder URL eingeben"))
                        .lineLimit(1).font(.system(size: 12, weight: .medium))
                        .foregroundStyle(ink.opacity(0.75))
                    Spacer(minLength: 0)
                }
                .padding(.leading, 16)
                .frame(maxWidth: .infinity, minHeight: 40)
                .contentShape(Rectangle())
            }
            .help(L("Adresse öffnen · ⌘L"))
            .accessibilityLabel(L("Adresse öffnen · ⌘L"))
            LoginAutofillButton(login: emptyLogin)
            ManagedVPNButton(model: model)
            Button { model.settingsSection = L("Allgemein", "General"); model.showSettings = true } label: {
                Image(systemName: "slider.horizontal.3")
                    .font(.system(size: 16))
                    .frame(width: 40, height: 40)
                    .background(ink.opacity(0.06), in: RoundedRectangle(cornerRadius: 9))
            }
            .help(L("Browser-Einstellungen", "Browser settings"))
            .accessibilityLabel(L("Browser-Einstellungen", "Browser settings"))
        }
        .buttonStyle(YOBROButtonStyle())
        .background(YOBROTheme.surface.opacity(0.5), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(ink.opacity(0.05)))
    }
}

struct SidebarNavigation: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var tab: BrowserTab
    var compact = false
    @State private var editingAddress = false
    @State private var showingActions = false

    var body: some View {
        addressBar
        .buttonStyle(YOBROButtonStyle()).font(.system(size: 12))
        .task(id: tab.id) {
            editingAddress = false
            showingActions = false
            presentRequestedAddress()
        }
        .onChange(of: model.showOnboarding) { _, value in if value { editingAddress = false; showingActions = false } }
        .onChange(of: model.focusAddress) { _, _ in presentRequestedAddress() }
    }

    private var addressBar: some View {
        Group {
            if compact {
                VStack(spacing: 4) { addressButton; if !tab.isPrivate { LoginAutofillButton(login: tab.logins) }; ManagedVPNButton(model: model, compact: true); actionsButton }
            } else {
                HStack(spacing: 0) { addressButton.padding(.leading, 4); if !tab.isPrivate { LoginAutofillButton(login: tab.logins) }; ManagedVPNButton(model: model); actionsButton.padding(4) }
            }
        }
        .background(YOBROTheme.surface.opacity(0.5), in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(ink.opacity(0.05)))
    }

    private var addressButton: some View {
        Button { editingAddress = true } label: {
            HStack(spacing: 7) {
                Image(systemName: tab.isPrivate ? "eyeglasses" : (tab.url.isEmpty ? "magnifyingglass" : (tab.url.hasPrefix("https://") ? "lock" : "globe")))
                    .font(.system(size: compact ? 16 : 10)).foregroundStyle(moss)
                if !compact {
                    Text(tab.isPrivate && tab.url.isEmpty ? L("Privat suchen oder URL eingeben", "Search privately or enter URL") : (tab.url.isEmpty ? L("Suchen oder URL eingeben") : (URL(string: tab.url)?.host ?? tab.title)))
                        .lineLimit(1).truncationMode(.tail).font(.system(size: 12, weight: .medium))
                        .foregroundStyle(ink.opacity(0.75))
                    Spacer(minLength: 0)
                }
            }.padding(.leading, compact ? 0 : 12)
                .frame(maxWidth: compact ? 42 : .infinity, minHeight: 40)
                .contentShape(Rectangle())
        }
        .help(tab.url.isEmpty ? L("Adresse öffnen · ⌘L") : tab.url + " · ⌘L")
        .accessibilityLabel(L("Adresse öffnen · ⌘L"))
        .popover(isPresented: $editingAddress, arrowEdge: .leading) {
            AddressEntry(model: model, tab: tab) { editingAddress = false }
        }
    }

    private var actionsButton: some View {
        Button { showingActions.toggle() } label: {
            Image(systemName: "slider.horizontal.3").font(.system(size: 16))
                .frame(width: 40, height: 40)
                .background(ink.opacity(showingActions ? 0.12 : 0.06), in: RoundedRectangle(cornerRadius: 9))
        }
        .help(L("Seitenaktionen und Erweiterungen", "Page actions and extensions"))
        .accessibilityLabel(L("Seitenaktionen und Erweiterungen", "Page actions and extensions"))
        .popover(isPresented: $showingActions, arrowEdge: compact ? .leading : .top) {
            SidebarPageActions(model: model, tab: tab, store: model.extensions) { showingActions = false }
        }
    }

    private func presentRequestedAddress() {
        guard model.focusAddress, !model.showOnboarding else { return }
        showingActions = false
        editingAddress = true
        model.focusAddress = false
    }
}

private struct ManagedVPNButton: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject private var vpn: ManagedVPNStore
    var compact = false
    @State private var presented = false

    init(model: BrowserModel, compact: Bool = false) {
        self.model = model
        self.compact = compact
        vpn = model.managedVPN
    }

    var body: some View {
        Button { presented.toggle() } label: {
            ZStack(alignment: .bottomTrailing) {
                Image(systemName: vpn.isConnected ? "shield.checkered" : "shield")
                    .font(.system(size: 15)).foregroundStyle(vpn.isConnected ? moss : ink.opacity(0.55))
                    .frame(width: 38, height: 40)
                if vpn.isConnected { Circle().fill(Color.green).frame(width: 7, height: 7).overlay(Circle().stroke(paper, lineWidth: 1.5)).offset(x: -4, y: -5) }
            }
        }
        .help(vpn.connectedLocation.map { L("VPN: \($0.city)", "VPN: \($0.city)") } ?? L("VPN-Standort wählen", "Choose VPN location"))
        .accessibilityLabel(vpn.connectedLocation.map { L("VPN verbunden: \($0.city)", "VPN connected: \($0.city)") } ?? L("VPN-Standort wählen", "Choose VPN location"))
        .popover(isPresented: $presented, arrowEdge: compact ? .leading : .top) {
            VStack(alignment: .leading, spacing: 10) {
                Text(L("VPN-Standort", "VPN location")).font(.headline)
                locationButton(nil)
                ForEach(vpn.locations) { location in locationButton(location) }
                if case .connecting = vpn.status {
                    HStack { ProgressView().controlSize(.small); Text(L("Verbinden …", "Connecting …")) }.font(.caption).foregroundStyle(.secondary)
                }
            }.padding(14).frame(width: 260).background(paper)
        }
    }

    private func locationButton(_ location: ManagedVPNLocation?) -> some View {
        let selected = location?.id == vpn.selectedLocationID || (location == nil && vpn.selectedLocationID == nil)
        return Button {
            if let location {
                Task {
                    do { try await vpn.connect(location, to: model.websiteDataStore); model.active?.webView.reload(); presented = false }
                    catch { model.notice = error.localizedDescription }
                }
            } else {
                vpn.disconnect(from: model.websiteDataStore)
                model.updateActiveProxy()
                model.active?.webView.reload()
                presented = false
            }
        } label: {
            HStack(spacing: 10) {
                Text(location?.flag ?? "○").font(.system(size: 19)).frame(width: 28)
                VStack(alignment: .leading, spacing: 1) {
                    Text(location?.title ?? L("Aus", "Off")).font(.system(size: 12, weight: .semibold))
                    Text(location?.subtitle ?? L("Normale Verbindung", "Normal connection")).font(.system(size: 10)).foregroundStyle(.secondary)
                }
                Spacer()
                if selected { Image(systemName: "checkmark").foregroundStyle(moss) }
            }.padding(8).contentShape(Rectangle())
        }.buttonStyle(.plain).disabled(vpn.status == .connecting)
    }
}

private struct SidebarPageActions: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var tab: BrowserTab
    @ObservedObject var store: ExtensionStore
    let close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 8) {
                action("plus", L("Neuer Tab · ⌘T")) { model.requestNewTab() }
                action(tab.pinned ? "pin.fill" : "pin", tab.pinned ? L("Loslösen") : L("Anpinnen")) { tab.pinned.toggle(); model.save() }
                action(model.splitID == nil ? "rectangle.split.2x1" : "rectangle", L("Splitview · ⇧⌘S")) { model.toggleSplit() }
                action("sparkle", L("Agenten · ⇧⌘A")) { model.showAgent.toggle() }
            }
            HStack(spacing: 8) {
                action("chevron.left", L("Zurück")) { tab.webView.goBack() }.disabled(!tab.canGoBack)
                action("chevron.right", L("Vorwärts")) { tab.webView.goForward() }.disabled(!tab.canGoForward)
                action(tab.loading ? "xmark" : "arrow.clockwise", tab.loading ? L("Laden stoppen", "Stop loading") : L("Neu laden")) {
                    if tab.loading { tab.webView.stopLoading() } else { tab.webView.reload() }
                }.disabled(tab.url.isEmpty)
                action("link", L("Adresse kopieren")) {
                    NSPasteboard.general.clearContents(); NSPasteboard.general.setString(tab.url, forType: .string)
                }.disabled(tab.url.isEmpty)
            }
            HStack(spacing: 8) {
                action(model.activePageIsBookmarked ? "bookmark.fill" : "bookmark", L("Seite merken · ⌘D", "Bookmark page · ⌘D")) {
                    model.bookmarkActivePage()
                }.disabled(tab.url.isEmpty)
                action("minus.magnifyingglass", L("Seite verkleinern · ⌘-", "Zoom out · ⌘-")) { tab.changeZoom(-1) }.disabled(tab.url.isEmpty)
                action("plus.magnifyingglass", L("Seite vergrößern · ⌘+", "Zoom in · ⌘+")) { tab.changeZoom(1) }.disabled(tab.url.isEmpty)
                action("printer", L("Seite drucken · ⌘P", "Print page · ⌘P")) { tab.printPage() }.disabled(tab.url.isEmpty)
            }
            Divider()
            Text(L("Erweiterungen")).font(.system(size: 15, weight: .semibold))
            ForEach(store.entries.filter { $0.enabled && store.errors[$0.id] == nil }) { entry in
                Button { close(); store.perform(entry, tab: tab) } label: {
                    Label(entry.name, systemImage: "puzzlepiece.extension")
                        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10)
                }
            }
            Button { close(); model.showSettings = true } label: {
                Label(L("Erweiterungen und Import …"), systemImage: "plus")
                    .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 10)
            }
            MarketplaceInstallButton(model: model, tab: tab, store: store)
            Divider()
            HStack(spacing: 10) {
                WebAppearanceButton(appearance: model.webAppearance, host: URL(string: tab.url)?.host)
                Text(L("Webseiten-Darkmode")).font(.system(size: 12))
                Spacer()
                Button {
                    close()
                    model.settingsSection = L("Allgemein", "General")
                    model.showSettings = true
                } label: { Image(systemName: "gearshape") }
                    .help(L("Einstellungen", "Settings"))
            }
        }
        .buttonStyle(YOBROButtonStyle()).font(.system(size: 12))
        .padding(16).frame(width: 300).background(paper).foregroundStyle(ink)
    }

    private func action(_ symbol: String, _ title: String, perform: @escaping () -> Void) -> some View {
        Button { close(); perform() } label: {
            Image(systemName: symbol).font(.system(size: 18)).frame(maxWidth: .infinity).frame(height: 44)
                .background(ink.opacity(0.055), in: RoundedRectangle(cornerRadius: 10))
        }.help(title).accessibilityLabel(title)
    }
}

struct AddressEntry: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var tab: BrowserTab
    let close: () -> Void
    @State private var address = ""
    /// `nil` means "use exactly what was typed". Preselecting a history entry
    /// would silently redirect a freshly entered address to an older URL.
    @State private var selectedSuggestion: Int?
    @FocusState private var focused: Bool
    private var suggestions: [HistoryEntry] { model.addressSuggestions(address) }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(L("Suchen oder URL eingeben")).font(.system(size: 12)).foregroundStyle(.secondary)
            TextField(L("Wohin möchtest du?"), text: $address).textFieldStyle(YOBROTextFieldStyle()).font(.system(size: 17)).focused($focused)
                .onSubmit { openSelectedSuggestion() }
            if !suggestions.isEmpty {
                VStack(spacing: 2) {
                    ForEach(Array(suggestions.enumerated()), id: \.element.id) { index, suggestion in
                        Button {
                            selectedSuggestion = index
                            openSelectedSuggestion()
                        } label: {
                            HStack(spacing: 10) {
                                Image(systemName: "clock.arrow.circlepath").foregroundStyle(moss).frame(width: 18)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(suggestion.title).lineLimit(1).foregroundStyle(ink)
                                    Text(suggestion.url).lineLimit(1).foregroundStyle(.secondary)
                                }
                                Spacer()
                                Text("\(suggestion.visits)×").font(.system(size: 10)).foregroundStyle(.tertiary)
                                if index == selectedSuggestion { Image(systemName: "return").font(.system(size: 11)).foregroundStyle(moss) }
                            }.padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                                .background(index == selectedSuggestion ? moss.opacity(0.09) : .clear, in: RoundedRectangle(cornerRadius: 8))
                        }.buttonStyle(.plain)
                    }
                }
            }
            HStack {
                if !tab.url.isEmpty { Button(L("Adresse kopieren")) { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(tab.url, forType: .string) } }
                Spacer()
                Button(L("Schließen"), action: close).keyboardShortcut(.cancelAction)
                Button(L("Öffnen")) { openSelectedSuggestion() }.keyboardShortcut(.defaultAction).disabled(address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }.font(.system(size: 12))
        }.padding(20).frame(width: 500).background(paper)
            .task(id: tab.id) {
                address = tab.url
                focused = false
                await Task.yield()
                focused = true
                await Task.yield()
                NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil)
            }
            .onChange(of: address) { _, _ in selectedSuggestion = nil }
            .onKeyPress(.downArrow) {
                guard !suggestions.isEmpty else { return .ignored }
                selectedSuggestion = SuggestionSelection.next(selectedSuggestion, count: suggestions.count)
                return .handled
            }
            .onKeyPress(.upArrow) {
                guard !suggestions.isEmpty else { return .ignored }
                selectedSuggestion = SuggestionSelection.previous(selectedSuggestion)
                return .handled
            }
    }
    private func openSelectedSuggestion() {
        model.navigate(SuggestionSelection.destination(selectedSuggestion, suggestions: suggestions, typed: address))
        close()
    }
}

struct NewTabPage: View {
    let tab: BrowserTab?
    @ObservedObject var model: BrowserModel
    @State private var address = ""
    /// See `AddressEntry`: no suggestion is preselected so Enter opens the
    /// typed address rather than the highest scoring history entry.
    @State private var selectedSuggestion: Int?
    @FocusState private var focused: Bool
    private var suggestions: [HistoryEntry] { model.addressSuggestions(address) }

    var body: some View {
        GeometryReader { geometry in
        let cardWidth = min(540, max(220, geometry.size.width - 32))
        let compact = geometry.size.width < 620 || geometry.size.height < 620
        VStack(spacing: compact ? 14 : 22) {
            Spacer()
            VStack(spacing: compact ? 6 : 10) {
                if tab?.isPrivate == true {
                    Image(systemName: "eyeglasses")
                        .font(.system(size: compact ? 38 : 58, weight: .light))
                        .foregroundStyle(moss)
                } else {
                    YOBROMark(size: compact ? 52 : 82)
                }
                Text(tab?.isPrivate == true ? L("Privates Browsen", "Private browsing") : "YoBro")
                    .font(.system(size: compact ? 30 : 42, weight: .semibold, design: .rounded))
                    .tracking(-2)
                Text(tab?.isPrivate == true
                     ? L("Cookies und Websitedaten bleiben nur bis zum Schließen des Tabs. Verlauf und Sitzung werden nicht gespeichert.", "Cookies and website data last only until you close the tab. History and session are not saved.")
                     : L("Wohin geht es als Nächstes?", "Where to next?"))
                    .font(.system(size: 13))
                    .foregroundStyle(ink.opacity(0.5))
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 520)
            }
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 10) {
                    Image(systemName: tab?.isPrivate == true ? "eyeglasses" : "magnifyingglass").foregroundStyle(moss)
                    TextField(tab?.isPrivate == true ? L("Privat suchen oder URL eingeben", "Search privately or enter URL") : L("Suchen oder URL eingeben"), text: $address)
                        .textFieldStyle(.plain)
                        .font(.system(size: 16))
                        .focused($focused)
                        .onSubmit { openSelectedSuggestion() }
                }
                .padding(.horizontal, 14).frame(height: 48)
                .background(YOBROTheme.field, in: RoundedRectangle(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).stroke(moss.opacity(focused ? 0.5 : 0.16), lineWidth: focused ? 1.5 : 1))

                if !suggestions.isEmpty {
                    VStack(spacing: 2) {
                        ForEach(Array(suggestions.enumerated()), id: \.element.id) { index, suggestion in
                            Button {
                                selectedSuggestion = index
                                openSelectedSuggestion()
                            } label: {
                                HStack(spacing: 10) {
                                    Image(systemName: "clock.arrow.circlepath").foregroundStyle(moss).frame(width: 18)
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(suggestion.title).lineLimit(1).foregroundStyle(ink)
                                        Text(suggestion.url).lineLimit(1).foregroundStyle(.secondary)
                                    }
                                    Spacer()
                                    Text("\(suggestion.visits)×").font(.system(size: 10)).foregroundStyle(.tertiary)
                                    if index == selectedSuggestion { Image(systemName: "return").font(.system(size: 11)).foregroundStyle(moss) }
                                }
                                .padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                                .background(index == selectedSuggestion ? moss.opacity(0.09) : .clear, in: RoundedRectangle(cornerRadius: 8))
                            }.buttonStyle(.plain)
                        }
                    }
                }
                Text(L("Adresse eingeben oder direkt im Web suchen · Enter zum Öffnen", "Enter an address or search the web · Press Enter to open"))
                    .font(.system(size: 10)).foregroundStyle(.secondary).padding(.horizontal, 4)
            }
            .padding(16)
            .frame(width: cardWidth)
            .background(YOBROTheme.surface.opacity(0.72), in: RoundedRectangle(cornerRadius: 18))
            .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(YOBROTheme.border.opacity(0.45)))
            .shadow(color: .black.opacity(0.10), radius: 24, y: 10)
            .task(id: tab?.id) { await Task.yield(); focused = true }
            .onChange(of: model.focusAddress) { _, requested in
                guard requested else { return }
                focused = true
                model.focusAddress = false
            }
            .onChange(of: address) { _, _ in selectedSuggestion = nil }
            .onKeyPress(.downArrow) {
                guard !suggestions.isEmpty else { return .ignored }
                selectedSuggestion = SuggestionSelection.next(selectedSuggestion, count: suggestions.count)
                return .handled
            }
            .onKeyPress(.upArrow) {
                guard !suggestions.isEmpty else { return .ignored }
                selectedSuggestion = SuggestionSelection.previous(selectedSuggestion)
                return .handled
            }
            // Balance the flexible space above and below the start content.
            // The previous fixed bottom inset pushed the whole composition too
            // far down, especially after closing a tab into this empty state.
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(
            RadialGradient(colors: [moss.opacity(0.08), .clear], center: .center, startRadius: 20, endRadius: 420)
        )
        .accessibilityIdentifier("new-tab-page")
        }
    }

    private func openSelectedSuggestion() {
        let destination = SuggestionSelection.destination(selectedSuggestion, suggestions: suggestions, typed: address)
        guard !destination.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        if let tab {
            do { try tab.navigate(destination) }
            catch { model.notice = error.localizedDescription }
        } else {
            model.newTab(url: destination)
        }
    }
}

struct TabContent: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    var body: some View {
        ZStack(alignment: .top) {
            if tab.isNote {
                NoteEditor(tab: tab, model: model)
            } else if tab.url.isEmpty {
                NewTabPage(tab: tab, model: model)
            } else {
                paper
                WebSurface(webView: tab.webView)
                if let failure = tab.failure {
                    VStack(spacing: 17) {
                        Image(systemName: failure.symbol).font(.system(size: 32)).foregroundStyle(moss)
                        Text(failure.title).font(.system(size: 20, weight: .medium)).multilineTextAlignment(.center)
                        Text(failure.detail).font(.system(size: 12)).foregroundStyle(.secondary).multilineTextAlignment(.center)
                        HStack(spacing: 10) {
                            if failure.canRetry {
                                Button(L("Erneut versuchen")) { tab.failure = nil; tab.webView.reload() }
                                    .buttonStyle(.borderedProminent).tint(moss)
                            }
                            if tab.canGoBack {
                                Button(L("Zurück")) { tab.failure = nil; tab.webView.goBack() }.buttonStyle(.bordered)
                            }
                        }
                    }.padding(40).frame(maxWidth: .infinity, maxHeight: .infinity).background(paper)
                        .accessibilityIdentifier("page-failure")
                }
                if tab.loading {
                    GeometryReader { geometry in Rectangle().fill(moss.opacity(0.7)).frame(width: geometry.size.width * max(0.03, tab.progress), height: 2) }.frame(height: 2)
                }
            }
        }.clipShape(RoundedRectangle(cornerRadius: 14))
            .overlay(alignment: .bottomTrailing) { LoginSavePrompt(login: tab.logins) }
            .overlay {
                if !tab.url.isEmpty && !tab.isNote {
                    RoundedRectangle(cornerRadius: 14)
                        .strokeBorder(ink.opacity(0.08), lineWidth: 1)
                }
            }
            .overlay(alignment: .topTrailing) {
                if tab.showFind && !tab.url.isEmpty && !tab.isNote { PageFindBar(tab: tab).padding(12) }
            }
    }
}

private struct NoteEditor: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    @StateObject private var commands = RichTextCommands()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "note.text").foregroundStyle(moss)
                TextField(L("Titel der Notiz", "Note title"), text: $tab.title)
                    .textFieldStyle(.plain)
                    .font(.system(size: 18, weight: .semibold))
                    .onChange(of: tab.title) { _, _ in model.save() }
                Spacer()
                Text(L("Automatisch gespeichert", "Saved automatically"))
                    .font(.system(size: 10)).foregroundStyle(.secondary)
            }
            .padding(.horizontal, 24).frame(height: 58)
            Divider().opacity(0.45)
            RichTextToolbar(commands: commands).padding(.horizontal, 20).frame(height: 42)
            Divider().opacity(0.3)
            RichTextEditor(rtf: tab.noteRTF, fallbackText: tab.noteContent, commands: commands) { rtf, text in
                tab.noteRTF = rtf
                tab.noteContent = text
                model.save()
            }
                .accessibilityLabel(L("Notizinhalt", "Note content"))
        }
        .background(paper)
        .accessibilityIdentifier("note-editor")
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
        guard let url = Bundle.module.url(forResource: "YOBROMark", withExtension: "png") else { return nil }
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

private struct AgentPageHeader: View {
    @ObservedObject var tab: BrowserTab
    var body: some View {
        HStack(spacing: 8) {
            if tab.loading { ProgressView().controlSize(.mini) }
            Text(tab.url.isEmpty ? L("Agentenseite", "Agent page") : tab.url)
                .font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            Spacer(minLength: 0)
        }.padding(.horizontal, 12).frame(height: 28)
    }
}
