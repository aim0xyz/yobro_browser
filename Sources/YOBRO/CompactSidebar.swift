import SwiftUI

enum SidebarPreviewTiming {
    static let openDelay: UInt64 = 650_000_000
    static let closeDelay: UInt64 = 350_000_000
}

struct CompactSidebar: View {
    @ObservedObject var model: BrowserModel
    @ObservedObject var mail: MailStore
    var body: some View {
        VStack(spacing: 8) {
            Color.clear.frame(width: 60, height: 32).padding(.top, 10)
            VStack(spacing: 4) {
                icon("sidebar.left", L("Seitenleiste einblenden · ⌘S")) { model.showSidebar = true }
                if let tab = model.active { SidebarNavigation(model: model, tab: tab, compact: true) }
                else { icon("magnifyingglass", L("Adresse öffnen · ⌘L")) { model.focusAddress = true } }
                icon("envelope", L("E-Mail"), active: model.showMail) { model.presentMail() }
                    .overlay(alignment: .topTrailing) { if mail.hasUnread { MailUnreadDot().padding(4) } }
                if model.agentWorkspaceVisible {
                    icon("rectangle.split.2x1.fill", L("Agent arbeitet rechts", "Agent working on the right"), active: true) { model.showAgent.toggle() }
                        .overlay(alignment: .topTrailing) { Circle().fill(brandOrange).frame(width: 7, height: 7).padding(4) }
                }
            }
            .padding(4).background(YOBROTheme.surface.opacity(0.24), in: RoundedRectangle(cornerRadius: 13))
            Divider().padding(.horizontal, 15)
            ScrollView {
                VStack(spacing: 7) {
                    CompactSpaceSwitcher(model: model)
                    Divider().padding(.horizontal, 15)
                    ForEach(model.visibleTabs.filter(\.pinned)) { tab in CompactTabIcon(tab: tab, model: model) }
                    ForEach(model.folders.filter { $0.space == model.space }) { folder in
                        CompactFolderIcon(folder: folder, model: model)
                    }
                    ForEach(model.visibleTabs.filter { !$0.pinned && $0.folderID == nil }) { tab in CompactTabIcon(tab: tab, model: model) }
                    icon("plus", L("Neuer Tab · ⌘T")) { model.requestNewTab() }
                        .contextMenu { Button(L("Neuer privater Tab", "New private tab")) { model.requestPrivateTab() } }
                }
            }.scrollIndicators(.hidden)
            Spacer(minLength: 0)
            VStack(spacing: 4) {
                icon("clock.arrow.circlepath", L("Verlauf · ⌘Y", "History · ⌘Y")) { model.librarySection = .history }
                icon("arrow.down.circle", "Downloads · ⇧⌘J") { model.librarySection = .downloads }
                if let profiles = model.profileSession { ProfileMenu(profiles: profiles, model: model, compact: true) }
            }
            .padding(4).background(YOBROTheme.surface.opacity(0.24), in: RoundedRectangle(cornerRadius: 13))
            .padding(.bottom, 14)
        }.frame(width: 80)
    }
    private func icon(_ symbol: String, _ title: String, active: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).font(.system(size: 17)).frame(width: 42, height: 40).background(active ? moss.opacity(0.18) : .clear, in: RoundedRectangle(cornerRadius: 9)) }
            .buttonStyle(YOBROButtonStyle()).foregroundStyle(moss).help(title).accessibilityLabel(title)
    }
}

private struct CompactSpaceSwitcher: View {
    @ObservedObject var model: BrowserModel
    @State private var presented = false
    @State private var presentTask: Task<Void, Never>?
    @State private var dismissTask: Task<Void, Never>?

    var body: some View {
        Button { presented = true } label: {
            ZStack(alignment: .topTrailing) {
                ZStack {
                    Image(systemName: model.spaceIcon(for: model.space).rawValue)
                        .font(.system(size: 19, weight: .semibold)).foregroundStyle(moss)
                }
                .frame(width: 42, height: 40)
                .background(moss.opacity(0.18), in: RoundedRectangle(cornerRadius: 9))
                if model.spaces.count > 1 {
                    Image(systemName: "chevron.right")
                        .font(.system(size: 7, weight: .bold)).foregroundStyle(moss)
                        .padding(5)
                }
            }
        }
        .buttonStyle(YOBROButtonStyle())
        .help(L("Space wechseln: ", "Switch space: ") + model.space)
        .accessibilityLabel(L("Space wechseln: ", "Switch space: ") + model.space)
        .contextMenu { SpaceIconMenu(model: model, space: model.space) }
        .onHover(perform: hover)
        .popover(isPresented: $presented, arrowEdge: .leading) {
            VStack(alignment: .leading, spacing: 6) {
                Text(L("SPACES")).font(.system(size: 9, weight: .semibold)).tracking(1.3).foregroundStyle(.secondary).padding(.horizontal, 8).padding(.bottom, 2)
                ForEach(model.spaces, id: \.self) { space in
                    let count = model.tabs.filter { $0.space == space }.count
                    Button {
                        presented = false
                        model.switchSpace(space)
                    } label: {
                        HStack(spacing: 10) {
                            ZStack {
                                RoundedRectangle(cornerRadius: 7).fill(space == model.space ? moss.opacity(0.18) : YOBROTheme.field)
                                Image(systemName: model.spaceIcon(for: space).rawValue).font(.system(size: 12, weight: .semibold)).foregroundStyle(moss)
                            }.frame(width: 30, height: 30)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(space).font(.system(size: 12, weight: .medium)).lineLimit(1)
                                Text("\(count) " + (count == 1 ? "Tab" : "Tabs")).font(.system(size: 9)).foregroundStyle(.secondary)
                            }
                            Spacer(minLength: 12)
                            if space == model.space { Image(systemName: "checkmark").font(.system(size: 10, weight: .semibold)).foregroundStyle(moss) }
                        }
                        .padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                        .background(space == model.space ? moss.opacity(0.07) : .clear, in: RoundedRectangle(cornerRadius: 9))
                    }.buttonStyle(CompactPopoverButtonStyle(hoverChanged: hover))
                }
            }
            .padding(10).frame(width: 230).background(paper)
            .onHover(perform: hover)
        }
        .onDisappear {
            presentTask?.cancel()
            dismissTask?.cancel()
        }
    }

    private func hover(_ inside: Bool) {
        presentTask?.cancel()
        dismissTask?.cancel()
        guard !model.extensionActionPopupPresented else {
            presented = false
            return
        }
        if inside {
            guard !presented else { return }
            presentTask = Task {
                try? await Task.sleep(nanoseconds: SidebarPreviewTiming.openDelay)
                guard !Task.isCancelled, !model.extensionActionPopupPresented else { return }
                presented = true
            }
            return
        }
        guard presented else { return }
        dismissTask = Task {
            try? await Task.sleep(nanoseconds: SidebarPreviewTiming.closeDelay)
            guard !Task.isCancelled else { return }
            presented = false
        }
    }
}

private struct CompactTabIcon: View {
    @ObservedObject var tab: BrowserTab
    @ObservedObject var model: BrowserModel
    var indented = false
    private var dropIntent: TabDropIntent? {
        model.tabDropFeedback?.targetID == tab.id ? model.tabDropFeedback?.intent : nil
    }
    private var tooltip: String {
        let prefix = tab.pinned ? L("Angepinnter Tab: ", "Pinned tab: ") : "Tab: "
        return prefix + tab.sidebarHelp
    }
    var body: some View {
        HStack(spacing: 0) {
            Button { model.select(tab.id) } label: {
                Group {
                    if tab.isNote { Image(systemName: "note.text").font(.system(size: 18)).foregroundStyle(moss) }
                    else if let favicon = tab.favicon { Image(nsImage: favicon).resizable().scaledToFit().frame(width: 20, height: 20) }
                    else { Image(systemName: tab.url.isEmpty ? "plus.square" : "globe").font(.system(size: 19)).foregroundStyle(moss) }
                }.frame(width: indented ? 36 : 42, height: 40)
                    .background(model.activeID == tab.id && !model.showMail ? moss.opacity(0.18) : .clear, in: RoundedRectangle(cornerRadius: 9))
                    .overlay(alignment: .leading) { if indented { Circle().fill(moss.opacity(0.45)).frame(width: 3, height: 3).offset(x: -5) } }
            }.buttonStyle(YOBROButtonStyle(minimumSize: indented ? 36 : 40))

            if tab.folderID == nil {
                Button { model.closeSidebarTab(tab.id) } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 10, weight: .semibold))
                        .frame(width: 28, height: 40)
                        .contentShape(Rectangle())
                }
                .buttonStyle(YOBROButtonStyle(minimumSize: 28))
                .help(L("Tab löschen", "Delete tab"))
                .accessibilityLabel(L("Tab löschen", "Delete tab"))
            }
        }
        .contentShape(Rectangle())
        .tabDropIndicator(dropIntent, compact: true, gap: 7)
        .onDrag {
            model.beginSidebarTabDrag(tab.id)
            return NSItemProvider(object: tab.id.uuidString as NSString)
        }
        .onDrop(of: [.plainText], delegate: TabReorderDropDelegate(
            targetID: tab.id, rowHeight: 40, model: model
        ))
        .contextMenu {
            Button(tab.folderID != nil && tab.id == model.activeID && !tab.isSuspended
                ? L("Seite schließen · im Ordner behalten", "Close page · keep in folder")
                : L("Tab löschen", "Delete tab")) { model.closeSidebarTab(tab.id) }
            if model.splitPairs.contains(where: { $0.contains(tab.id) }) { Button(L("Splitview trennen")) { model.separateSplit(tab.id) } }
        }
        .help(tooltip)
        .accessibilityLabel(tooltip)
    }
}

private struct CompactFolderIcon: View {
    let folder: TabFolder
    @ObservedObject var model: BrowserModel
    @State private var revealTask: Task<Void, Never>?
    @State private var dismissTask: Task<Void, Never>?
    @State private var dropTargeted = false
    private var count: Int { model.visibleTabs.filter { !$0.pinned && $0.folderID == folder.id }.count }
    private var active: Bool { model.active.map { $0.folderID == folder.id } ?? false }
    private var tabs: [BrowserTab] { model.visibleTabs.filter { !$0.pinned && $0.folderID == folder.id } }
    var body: some View {
        Button { model.hoveredFolderID = folder.id } label: {
            ZStack(alignment: .topTrailing) {
                Image(systemName: "folder.fill")
                    .font(.system(size: 18)).foregroundStyle(folder.folderColor.color)
                    .frame(width: 42, height: 40)
                    .background(active ? folder.folderColor.color.opacity(0.18) : YOBROTheme.surface.opacity(0.18), in: RoundedRectangle(cornerRadius: 9))
                if count > 0 {
                    Text("\(count)").font(.system(size: 8, weight: .bold, design: .rounded))
                        .padding(.horizontal, 4).frame(minWidth: 14, minHeight: 14)
                        .background(YOBROTheme.chromeTop, in: Capsule()).padding(2)
                }
            }
        }
        .buttonStyle(YOBROButtonStyle())
        .tabDropIndicator(
            model.folderDropFeedback?.targetID == folder.id ? model.folderDropFeedback?.intent : nil,
            compact: true,
            gap: 7
        )
        .contentShape(Rectangle())
        .onDrag {
            model.beginSidebarFolderDrag(folder.id)
            return NSItemProvider(object: "folder:\(folder.id.uuidString)" as NSString)
        }
        .onDrop(of: [.plainText], delegate: FolderHeaderDropDelegate(
            folderID: folder.id, rowHeight: 40, model: model, tabTargeted: $dropTargeted
        ))
        .help(L("Ordner: ") + folder.name + " · \(count) " + (count == 1 ? "Tab" : "Tabs"))
        .accessibilityLabel(L("Ordner: ") + folder.name)
        .contextMenu {
            Button(L("Neue Notiz", "New note")) { model.newNote(space: folder.space, folderID: folder.id) }
            FolderColorMenu(folder: folder, model: model)
        }
        .onHover(perform: hover)
        .popover(isPresented: folderPopoverPresented, arrowEdge: .leading) {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 8) {
                    Image(systemName: "folder.fill").foregroundStyle(folder.folderColor.color)
                    Text(folder.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                    Spacer()
                    Button {
                        model.hoveredFolderID = nil
                        model.newNote(space: folder.space, folderID: folder.id)
                    } label: { Image(systemName: "note.text.badge.plus") }
                        .buttonStyle(.plain)
                        .help(L("Notiz in diesem Ordner erstellen", "Create note in this folder"))
                    Text("\(count)").font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
                }.padding(.horizontal, 8).padding(.bottom, 4)
                if tabs.isEmpty {
                    Text(L("Keine Tabs in diesem Ordner.", "No tabs in this folder."))
                        .font(.system(size: 11)).foregroundStyle(.secondary).padding(10)
                } else {
                    ForEach(tabs) { tab in
                        Button {
                            model.hoveredFolderID = nil
                            model.select(tab.id)
                        } label: {
                            HStack(spacing: 10) {
                                Group {
                                    if tab.isNote { Image(systemName: "note.text").foregroundStyle(moss) }
                                    else if let favicon = tab.favicon { Image(nsImage: favicon).resizable().scaledToFit() }
                                    else { Image(systemName: "globe").foregroundStyle(moss) }
                                }.frame(width: 17, height: 17)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(tab.sidebarTitle).font(.system(size: 12, weight: tab.id == model.activeID ? .medium : .regular)).lineLimit(1)
                                    if let host = URL(string: tab.url)?.host {
                                        Text(host).font(.system(size: 9)).foregroundStyle(.secondary).lineLimit(1)
                                    }
                                }
                                Spacer(minLength: 8)
                                if tab.id == model.activeID { Circle().fill(moss).frame(width: 5, height: 5) }
                            }
                            .padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                            .background(tab.id == model.activeID ? moss.opacity(0.08) : .clear, in: RoundedRectangle(cornerRadius: 9))
                        }
                        .buttonStyle(CompactPopoverButtonStyle(hoverChanged: hover))
                        .contentShape(Rectangle())
                        .tabDropIndicator(model.tabDropFeedback?.targetID == tab.id ? model.tabDropFeedback?.intent : nil, gap: 5)
                        .onDrag {
                            model.beginSidebarTabDrag(tab.id)
                            return NSItemProvider(object: tab.id.uuidString as NSString)
                        }
                        .onDrop(of: [.plainText], delegate: TabReorderDropDelegate(
                            targetID: tab.id, rowHeight: 42, model: model
                        ))
                    }
                }
            }
            .padding(10).frame(width: 260).background(paper)
            .onHover(perform: hover)
        }
        .onDisappear {
            revealTask?.cancel()
            dismissTask?.cancel()
        }
    }

    private func hover(_ inside: Bool) {
        revealTask?.cancel()
        dismissTask?.cancel()
        if model.draggingTabID != nil || model.draggingFolderID != nil {
            model.hoveredFolderID = nil
            return
        }
        guard !model.extensionActionPopupPresented else {
            if model.hoveredFolderID == folder.id { model.hoveredFolderID = nil }
            return
        }
        if inside {
            guard model.hoveredFolderID != folder.id else { return }
            revealTask = Task {
                try? await Task.sleep(nanoseconds: SidebarPreviewTiming.openDelay)
                guard !Task.isCancelled,
                      model.draggingTabID == nil,
                      model.draggingFolderID == nil,
                      !model.extensionActionPopupPresented else { return }
                model.hoveredFolderID = folder.id
            }
            return
        }
        guard model.hoveredFolderID == folder.id else { return }
        dismissTask = Task {
            try? await Task.sleep(nanoseconds: SidebarPreviewTiming.closeDelay)
            guard !Task.isCancelled else { return }
            if model.hoveredFolderID == folder.id { model.hoveredFolderID = nil }
        }
    }

    private var folderPopoverPresented: Binding<Bool> {
        Binding(
            get: { model.draggingTabID == nil && model.draggingFolderID == nil && !model.extensionActionPopupPresented && model.hoveredFolderID == folder.id },
            set: { visible in
                guard !visible || (model.draggingTabID == nil && model.draggingFolderID == nil && !model.extensionActionPopupPresented) else { return }
                if visible { model.hoveredFolderID = folder.id }
                else if model.hoveredFolderID == folder.id { model.hoveredFolderID = nil }
            }
        )
    }
}

private struct CompactPopoverButtonStyle: ButtonStyle {
    let hoverChanged: (Bool) -> Void

    func makeBody(configuration: Configuration) -> some View {
        Surface(configuration: configuration, hoverChanged: hoverChanged)
    }

    private struct Surface: View {
        let configuration: ButtonStyle.Configuration
        let hoverChanged: (Bool) -> Void
        @State private var hovering = false

        var body: some View {
            configuration.label
                .contentShape(Rectangle())
                .background(
                    moss.opacity(configuration.isPressed ? 0.16 : hovering ? 0.10 : 0),
                    in: RoundedRectangle(cornerRadius: 9)
                )
                .animation(.easeOut(duration: 0.12), value: hovering)
                .onHover {
                    hovering = $0
                    hoverChanged($0)
                }
        }
    }
}
