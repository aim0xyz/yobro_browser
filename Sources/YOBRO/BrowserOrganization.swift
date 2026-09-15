import SwiftUI

struct TabFolder: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var space: String
    var collapsed = false
    var color: String? = nil

    var folderColor: FolderColor { FolderColor(rawValue: color ?? "") ?? .moss }
}
struct StoredSplit: Codable, Identifiable, Equatable {
    var first: UUID
    var second: UUID
    var id: UUID { first }
    func contains(_ id: UUID) -> Bool { first == id || second == id }
    func other(_ id: UUID) -> UUID? { first == id ? second : second == id ? first : nil }
}

extension BrowserModel {
    func addSpace(_ name: String) throws {
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.count <= 40, !spaces.contains(where: { $0.localizedCaseInsensitiveCompare(value) == .orderedSame }) else { throw YOBROError.message(L("Bitte einen neuen Space-Namen mit 1 bis 40 Zeichen wählen.")) }
        spaces.append(value); switchSpace(value); persistSession()
    }
    func renameSpace(_ original: String, to name: String) throws {
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let index = spaces.firstIndex(of: original), !value.isEmpty, value.count <= 40,
              !spaces.contains(where: { $0 != original && $0.localizedCaseInsensitiveCompare(value) == .orderedSame }) else { throw YOBROError.message(L("Dieser Space-Name ist leer oder bereits vergeben.")) }
        chat.rename(original, to: value)
        try proxies.rename(from: original, to: value)
        spaces[index] = value
        for tab in tabs where tab.space == original { tab.space = value }
        for index in folders.indices where folders[index].space == original { folders[index].space = value }
        for index in closedTabs.indices where closedTabs[index].space == original { closedTabs[index].space = value }
        if let icon = spaceIcons.removeValue(forKey: original) { spaceIcons[value] = icon }
        if space == original { space = value }
        persistSession()
    }
    func setSpaceIcon(_ icon: SpaceIcon, for targetSpace: String) {
        guard spaces.contains(targetSpace) else { return }
        if icon == .layers { spaceIcons.removeValue(forKey: targetSpace) }
        else { spaceIcons[targetSpace] = icon.rawValue }
        persistSession()
    }
    func spaceIcon(for targetSpace: String) -> SpaceIcon {
        SpaceIcon(rawValue: spaceIcons[targetSpace] ?? "") ?? .layers
    }
    func addFolder(_ name: String, in targetSpace: String? = nil) throws {
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.count <= 60 else { throw YOBROError.message(L("Bitte einen Ordnernamen mit 1 bis 60 Zeichen wählen.")) }
        folders.append(TabFolder(name: value, space: targetSpace ?? space)); persistSession()
    }
    func renameFolder(_ id: UUID, name: String) throws {
        let value = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.count <= 60, let index = folders.firstIndex(where: { $0.id == id }) else { throw YOBROError.message(L("Ungültiger Ordnername.")) }
        folders[index].name = value; persistSession()
    }
    func setFolderColor(_ color: FolderColor, folder id: UUID) {
        guard let index = folders.firstIndex(where: { $0.id == id }) else { return }
        folders[index].color = color == .moss ? nil : color.rawValue
        persistSession()
    }
    func dissolveFolder(_ id: UUID) {
        for tab in tabs where tab.folderID == id { tab.folderID = nil }
        for index in closedTabs.indices where closedTabs[index].folderID == id { closedTabs[index].folderID = nil }
        folders.removeAll { $0.id == id }; persistSession()
    }
    func moveToFolder(_ tabID: UUID, folder: UUID?) {
        guard let tab = tabs.first(where: { $0.id == tabID }) else { return }
        if let folder, !folders.contains(where: { $0.id == folder && $0.space == tab.space }) { return }
        tab.folderID = folder
        if let partner = splitPairs.first(where: { $0.contains(tabID) })?.other(tabID) { tabs.first(where: { $0.id == partner })?.folderID = folder }
        persistSession()
    }
    func moveFolder(_ id: UUID, relativeTo target: UUID, after: Bool) -> Bool {
        guard id != target,
              let source = folders.first(where: { $0.id == id }),
              let destination = folders.first(where: { $0.id == target }),
              source.space == destination.space else { return false }
        folders.removeAll { $0.id == id }
        guard let targetIndex = folders.firstIndex(where: { $0.id == target }) else { return false }
        folders.insert(source, at: targetIndex + (after ? 1 : 0))
        save()
        return true
    }
    func moveToSpace(_ tabID: UUID, space targetSpace: String) {
        guard spaces.contains(targetSpace), let tab = tabs.first(where: { $0.id == tabID }) else { return }
        splitPairs.removeAll { $0.contains(tabID) }
        tab.space = targetSpace; tab.folderID = nil
        select(tabID); persistSession()
    }
    @discardableResult
    func pairTabs(_ dragged: UUID, with target: UUID) -> Bool {
        guard !agentTabIDs.contains(dragged), !agentTabIDs.contains(target), dragged != target, let first = tabs.first(where: { $0.id == target }), let second = tabs.first(where: { $0.id == dragged }), first.space == second.space else { return false }
        splitPairs.removeAll { $0.contains(dragged) || $0.contains(target) }
        splitPairs.append(StoredSplit(first: target, second: dragged))
        second.folderID = first.folderID
        select(target); persistSession(); return true
    }
    func separateSplit(_ id: UUID? = nil) {
        let target = id ?? activeID
        if let target { splitPairs.removeAll { $0.contains(target) } }
        if target == activeID || target == splitID { splitID = nil }
        persistSession()
    }
}

struct SpaceStrip: View {
    @ObservedObject var model: BrowserModel
    @State private var presented = false
    @State private var presentTask: Task<Void, Never>?
    @State private var dismissTask: Task<Void, Never>?
    @State private var editing = false
    @State private var original: String?
    @State private var name = ""
    @State private var addingFolder = false
    @State private var folderName = ""
    @State private var editingProxySpace: String?
    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 4) {
                Button { presented = true } label: {
                    HStack(spacing: 8) {
                        Image(systemName: model.spaceIcon(for: model.space).rawValue).font(.system(size: 13)).foregroundStyle(moss)
                        Text(model.space).font(.system(size: 11, weight: .semibold)).lineLimit(1)
                        Spacer(minLength: 6)
                        Text("\(model.visibleTabs.count)").font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary)
                        Image(systemName: "chevron.right").font(.system(size: 8, weight: .semibold)).foregroundStyle(.secondary)
                    }
                    .padding(.horizontal, 10).frame(maxWidth: .infinity).frame(height: 34)
                    .background(YOBROTheme.surface.opacity(0.75), in: RoundedRectangle(cornerRadius: 8))
                }
                .buttonStyle(YOBROButtonStyle(minimumSize: 34))
                .help(L("Space wechseln: ", "Switch space: ") + model.space)
                .onHover(perform: hoverSpacePicker)
                .popover(isPresented: $presented, arrowEdge: .leading) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(L("SPACES")).font(.system(size: 9, weight: .semibold)).tracking(1.3).foregroundStyle(.secondary).padding(.horizontal, 8).padding(.bottom, 2)
                        ForEach(model.spaces, id: \.self) { space in
                            let count = model.tabs.filter { $0.space == space }.count
                            let proxyActive = model.proxies.config(for: space)?.enabled == true
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
                                        HStack(spacing: 5) {
                                            Text(space).font(.system(size: 12, weight: .medium)).lineLimit(1)
                                            if proxyActive { Image(systemName: "shield.lefthalf.filled").font(.system(size: 8)).foregroundStyle(moss) }
                                        }
                                        Text("\(count) " + (count == 1 ? "Tab" : "Tabs")).font(.system(size: 9)).foregroundStyle(.secondary)
                                    }
                                    Spacer(minLength: 12)
                                    if space == model.space { Image(systemName: "checkmark").font(.system(size: 10, weight: .semibold)).foregroundStyle(moss) }
                                }
                                .padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                                .background(space == model.space ? moss.opacity(0.07) : .clear, in: RoundedRectangle(cornerRadius: 9))
                            }.buttonStyle(OrganizationPopoverButtonStyle(hoverChanged: hoverSpacePicker))
                        }
                    }
                    .padding(10).frame(width: 230).background(paper)
                    .onHover(perform: hoverSpacePicker)
                }
                Menu {
                    Button(L("Neuer Space …")) { original = nil; name = ""; editing = true }
                    Button(L("Space umbenennen …")) { original = model.space; name = model.space; editing = true }
                    SpaceIconMenu(model: model, space: model.space)
                    Divider()
                    Button(L("Proxy / VPN einrichten …")) { editingProxySpace = model.space }
                    Divider()
                    Button(L("Neuer Ordner …")) { folderName = ""; addingFolder = true }
                } label: { Image(systemName: "plus").frame(width: 24, height: 34) }.menuStyle(.borderlessButton).fixedSize().help(L("Spaces und Ordner verwalten"))
            }.padding(3).background(ink.opacity(0.045), in: RoundedRectangle(cornerRadius: 11))
        }
        .popover(isPresented: editorPresented, arrowEdge: .leading) {
            YOBRONameEditor(
                icon: editing ? (original == nil ? "square.stack.3d.up.badge.plus" : model.spaceIcon(for: model.space).rawValue) : "folder.badge.plus",
                title: editing ? (original == nil ? L("Neuer Space", "New space") : L("Space umbenennen", "Rename space")) : L("Neuer Ordner", "New folder"),
                detail: editing ? L("Gib deinem Bereich einen klaren Namen.", "Give your space a clear name.") : L("Sammle zusammengehörige Tabs an einem Ort.", "Keep related tabs together."),
                text: editorText,
                cancel: closeEditor,
                save: saveEditor
            )
        }
        .sheet(isPresented: Binding(
            get: { editingProxySpace != nil },
            set: { if !$0 { editingProxySpace = nil } }
        )) {
            if let space = editingProxySpace {
                SpaceProxySheet(model: model, space: space)
            }
        }
        .onDisappear {
            presentTask?.cancel()
            dismissTask?.cancel()
        }
    }

    private func hoverSpacePicker(_ inside: Bool) {
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

    private var editorPresented: Binding<Bool> {
        Binding(get: { editing || addingFolder }, set: { if !$0 { closeEditor() } })
    }

    private var editorText: Binding<String> {
        editing ? $name : $folderName
    }

    private func closeEditor() {
        editing = false; addingFolder = false
    }

    private func saveEditor() {
        do {
            if editing {
                if let original { try model.renameSpace(original, to: name) }
                else { try model.addSpace(name) }
            } else { try model.addFolder(folderName) }
            closeEditor()
        } catch { model.notice = error.localizedDescription }
    }
}

struct SpaceFolderRow: View {
    let folder: TabFolder
    @ObservedObject var model: BrowserModel
    @State private var rename = false
    @State private var name = ""
    @State private var targeted = false
    @State private var sourceHovered = false
    @State private var revealTask: Task<Void, Never>?
    @State private var dismissTask: Task<Void, Never>?
    private var tabs: [BrowserTab] { model.visibleTabs.filter { $0.folderID == folder.id && !$0.pinned } }
    private var activeFolderTab: BrowserTab? {
        guard folder.collapsed, let activeID = model.activeID else { return nil }
        return tabs.first { $0.id == activeID }
    }
    private var displayedTabs: [BrowserTab] {
        if folder.collapsed { return activeFolderTab.map { [$0] } ?? [] }
        return tabs
    }
    private var hiddenTabCount: Int { max(0, tabs.count - (activeFolderTab == nil ? 0 : 1)) }
    var body: some View {
        VStack(spacing: 3) {
            Button {
                model.hoveredFolderID = nil
                setCollapsed(!folder.collapsed)
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: folder.collapsed ? "chevron.right" : "chevron.down")
                        .font(.system(size: 8))
                        .frame(width: 8, alignment: .center)
                    Image(systemName: "folder.fill").foregroundStyle(folder.folderColor.color)
                    Text(folder.name).font(.system(size: 11, weight: .medium)).lineLimit(1)
                    Spacer()
                    if activeFolderTab != nil && hiddenTabCount > 0 {
                        Text("+\(hiddenTabCount)")
                            .font(.system(size: 9, weight: .semibold, design: .rounded))
                            .monospacedDigit()
                            .foregroundStyle(folder.folderColor.color)
                            .padding(.horizontal, 6)
                            .frame(height: 18)
                            .background(folder.folderColor.color.opacity(0.14), in: Capsule())
                            .accessibilityLabel(L("\(hiddenTabCount) weitere Tabs", "\(hiddenTabCount) more tabs"))
                    } else {
                        Text("\(tabs.count)").font(.system(size: 9)).foregroundStyle(.secondary)
                    }
                }.padding(.horizontal, 10).frame(height: 36).background(targeted ? moss.opacity(0.18) : .clear, in: RoundedRectangle(cornerRadius: 8))
            }
            .buttonStyle(YOBROButtonStyle(minimumSize: 36))
                .contentShape(Rectangle())
                .onDrag {
                    model.beginSidebarFolderDrag(folder.id)
                    return NSItemProvider(object: "folder:\(folder.id.uuidString)" as NSString)
                }
                .onHover {
                    sourceHovered = $0
                    if model.draggingTabID != nil || model.draggingFolderID != nil {
                        model.hoveredFolderID = nil
                    } else if folder.collapsed {
                        hoverFolder($0)
                    }
                }
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
                            Text("\(tabs.count)").font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
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
                                            if let host = URL(string: tab.url)?.host { Text(host).font(.system(size: 9)).foregroundStyle(.secondary).lineLimit(1) }
                                        }
                                        Spacer(minLength: 8)
                                        if tab.id == model.activeID { Circle().fill(moss).frame(width: 5, height: 5) }
                                    }
                                    .padding(.horizontal, 8).frame(height: 42).contentShape(Rectangle())
                                    .background(tab.id == model.activeID ? moss.opacity(0.08) : .clear, in: RoundedRectangle(cornerRadius: 9))
                                }
                                .buttonStyle(OrganizationPopoverButtonStyle(hoverChanged: hoverFolder))
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
                    .onHover(perform: hoverFolder)
                }
                .onChange(of: model.hoveredFolderID) { _, hoveredID in
                    // A transient macOS popover consumes the first click on its
                    // source view. Treat dismissal while still hovering the
                    // closed folder as that intended click and expand it.
                    if hoveredID == nil && sourceHovered && folder.collapsed { setCollapsed(false) }
                }
                .onDrop(of: [.plainText], delegate: FolderHeaderDropDelegate(
                    folderID: folder.id, rowHeight: 36, model: model, tabTargeted: $targeted
                ))
                .contextMenu {
                    Button(L("Neue Notiz", "New note")) {
                        model.newNote(space: folder.space, folderID: folder.id)
                    }
                    Button(L("Ordner umbenennen …")) { name = folder.name; rename = true }
                    FolderColorMenu(folder: folder, model: model)
                    Button(L("Ordner auflösen · Tabs behalten")) { model.dissolveFolder(folder.id) }
                }
            VStack(spacing: 0) {
                ForEach(displayedTabs) { tab in
                    TabRow(tab: tab, model: model, reorderGap: 3).padding(.leading, 24)
                }
            }
        }
        .tabDropIndicator(
            model.folderDropFeedback?.targetID == folder.id ? model.folderDropFeedback?.intent : nil,
            gap: 5
        )
        .popover(isPresented: $rename, arrowEdge: .leading) {
            YOBRONameEditor(
                icon: "folder.fill",
                title: L("Ordner umbenennen", "Rename folder"),
                detail: L("Der neue Name gilt nur in diesem Space.", "The new name applies only in this space."),
                text: $name,
                cancel: { rename = false },
                save: saveName
            )
        }
        .onDisappear {
            revealTask?.cancel()
            dismissTask?.cancel()
        }
    }

    private func hoverFolder(_ inside: Bool) {
        revealTask?.cancel()
        dismissTask?.cancel()
        guard model.draggingTabID == nil, model.draggingFolderID == nil,
              !model.extensionActionPopupPresented else {
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

    private func setCollapsed(_ collapsed: Bool) {
        guard let index = model.folders.firstIndex(where: { $0.id == folder.id }), model.folders[index].collapsed != collapsed else { return }
        model.folders[index].collapsed = collapsed
        model.save()
    }

    private func saveName() {
        do { try model.renameFolder(folder.id, name: name); rename = false }
        catch { model.notice = error.localizedDescription }
    }
}

struct FolderHeaderDropDelegate: DropDelegate {
    let folderID: UUID
    let rowHeight: CGFloat
    let model: BrowserModel
    @Binding var tabTargeted: Bool

    func validateDrop(info: DropInfo) -> Bool {
        if let sourceID = model.draggingFolderID {
            guard sourceID != folderID,
                  let source = model.folders.first(where: { $0.id == sourceID }),
                  let target = model.folders.first(where: { $0.id == folderID }) else { return false }
            return source.space == target.space
        }
        guard let tabID = model.draggingTabID,
              let tab = model.visibleTabs.first(where: { $0.id == tabID }),
              let folder = model.folders.first(where: { $0.id == folderID }) else { return false }
        return tab.space == folder.space
    }

    func dropEntered(info: DropInfo) { updateFeedback(info) }
    func dropExited(info: DropInfo) {
        tabTargeted = false
        if model.folderDropFeedback?.targetID == folderID { model.folderDropFeedback = nil }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        model.autoScrollSidebarDuringDrag()
        updateFeedback(info)
        return DropProposal(operation: .move)
    }
    func performDrop(info: DropInfo) -> Bool {
        tabTargeted = false
        let sessionID = model.sidebarDragSessionID
        if let sourceID = model.draggingFolderID {
            let intent: TabDropIntent = info.location.y < rowHeight / 2 ? .before : .after
            let moved = model.moveFolder(sourceID, relativeTo: folderID, after: intent == .after)
            DispatchQueue.main.async { model.finishSidebarDrag(sessionID: sessionID) }
            return moved
        }
        guard let tabID = model.draggingTabID else { return false }
        withAnimation(.easeInOut(duration: 0.18)) { model.moveToFolder(tabID, folder: folderID) }
        DispatchQueue.main.async {
            model.finishSidebarDrag(sessionID: sessionID)
        }
        return true
    }

    private func updateFeedback(_ info: DropInfo) {
        if model.draggingFolderID != nil {
            tabTargeted = false
            let intent: TabDropIntent = info.location.y < rowHeight / 2 ? .before : .after
            let feedback = TabDropFeedback(targetID: folderID, intent: intent)
            guard model.folderDropFeedback != feedback else { return }
            withAnimation(.easeOut(duration: 0.08)) { model.folderDropFeedback = feedback }
        } else if model.draggingTabID != nil {
            model.folderDropFeedback = nil
            tabTargeted = true
        }
    }
}

struct SpaceIconMenu: View {
    @ObservedObject var model: BrowserModel
    let space: String
    var body: some View {
        Menu {
            ForEach(SpaceIcon.allCases) { icon in
                Button { model.setSpaceIcon(icon, for: space) } label: {
                    Label(icon.title, systemImage: model.spaceIcon(for: space) == icon ? "checkmark.circle.fill" : icon.rawValue)
                }
            }
        } label: { Label(L("Space-Symbol", "Space icon"), systemImage: "square.grid.2x2") }
    }
}

struct FolderColorMenu: View {
    let folder: TabFolder
    @ObservedObject var model: BrowserModel
    var body: some View {
        Menu {
            ForEach(FolderColor.allCases) { color in
                Button { model.setFolderColor(color, folder: folder.id) } label: {
                    Label {
                        Text((folder.folderColor == color ? "✓  " : "") + color.title)
                    } icon: {
                        Image(nsImage: color.menuImage)
                    }
                }
            }
        } label: { Label(L("Ordnerfarbe", "Folder color"), systemImage: "paintpalette") }
    }
}

private struct OrganizationPopoverButtonStyle: ButtonStyle {
    let hoverChanged: (Bool) -> Void
    func makeBody(configuration: Configuration) -> some View { Surface(configuration: configuration, hoverChanged: hoverChanged) }

    private struct Surface: View {
        let configuration: ButtonStyle.Configuration
        let hoverChanged: (Bool) -> Void
        @State private var hovering = false
        var body: some View {
            configuration.label
                .contentShape(Rectangle())
                .background(moss.opacity(configuration.isPressed ? 0.16 : hovering ? 0.10 : 0), in: RoundedRectangle(cornerRadius: 9))
                .animation(.easeOut(duration: 0.12), value: hovering)
                .onHover { hovering = $0; hoverChanged($0) }
        }
    }
}
