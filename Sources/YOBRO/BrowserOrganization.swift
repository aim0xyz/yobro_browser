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
        spaces[index] = value
        for tab in tabs where tab.space == original { tab.space = value }
        for index in folders.indices where folders[index].space == original { folders[index].space = value }
        for index in closedTabs.indices where closedTabs[index].space == original { closedTabs[index].space = value }
        if let icon = spaceIcons.removeValue(forKey: original) { spaceIcons[value] = icon }
        if let tone = spaceTones.removeValue(forKey: original) { spaceTones[value] = tone }
        moveRememberedTab(from: original, to: value)
        if space == original { space = value }
        persistSession()
    }
    /// Deletes a Space with its tabs and folders. The tabs close as one undoable
    /// step and reopen in the neighbouring Space, which also takes over if the
    /// deleted one was shown. The last Space cannot be deleted.
    func removeSpace(_ name: String) {
        guard spaces.count > 1, let position = spaces.firstIndex(of: name) else { return }
        let fallback = spaces[position + 1 < spaces.count ? position + 1 : position - 1]
        freezeAutomaticAppearance()
        if space == name { switchSpace(fallback) }
        // Agent tabs are included; they are never stored for reopening.
        closeTabs(tabs.filter { $0.space == name }.map(\.id), revealStartPage: true)
        for index in closedTabs.indices where closedTabs[index].space == name {
            closedTabs[index].space = fallback
            closedTabs[index].folderID = nil
        }
        folders.removeAll { $0.space == name }
        spaceIcons[name] = nil
        spaceTones[name] = nil
        chat.newChat(space: name)
        spaces.removeAll { $0 == name }
        persistSession()
    }
    func moveSpace(_ name: String, by offset: Int) {
        guard offset != 0, let index = spaces.firstIndex(of: name), spaces.indices.contains(index + offset) else { return }
        freezeAutomaticAppearance()
        spaces.insert(spaces.remove(at: index), at: index + offset)
        persistSession()
    }
    /// Unchosen icons and tones follow the position, so they are written down
    /// before positions change; otherwise every later Space would repaint.
    private func freezeAutomaticAppearance() {
        for name in spaces {
            spaceIcons[name] = spaceIcon(for: name).rawValue
            spaceTones[name] = spaceTone(for: name).rawValue
        }
    }
    func setSpaceIcon(_ icon: SpaceIcon, for targetSpace: String) {
        guard spaces.contains(targetSpace) else { return }
        spaceIcons[targetSpace] = icon.rawValue
        persistSession()
    }
    /// Like the tone, an unset icon is picked by position so the Space
    /// switcher never shows two identical symbols.
    func spaceIcon(for targetSpace: String) -> SpaceIcon {
        if let chosen = SpaceIcon(rawValue: spaceIcons[targetSpace] ?? "") { return chosen }
        let automatic: [SpaceIcon] = [.home, .work, .ideas, .travel, .creative, .learning, .favorite, .nature, .music, .reading]
        return automatic[(spaces.firstIndex(of: targetSpace) ?? 0) % automatic.count]
    }
    /// Spaces without a chosen colour take one by position, so neighbours differ.
    func spaceTone(for targetSpace: String) -> SpaceTone {
        SpaceTone(rawValue: spaceTones[targetSpace] ?? "") ?? .automatic(at: spaces.firstIndex(of: targetSpace) ?? 0)
    }
    func setSpaceTone(_ tone: SpaceTone, for targetSpace: String) {
        guard spaces.contains(targetSpace) else { return }
        spaceTones[targetSpace] = tone.rawValue
        persistSession()
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
        // Folders are saved with the session; a private tab never is.
        guard folder == nil || !tab.isPrivate else { return }
        if let folder, !folders.contains(where: { $0.id == folder && $0.space == tab.space }) { return }
        let partner = splitPairs.first(where: { $0.contains(tabID) })?.other(tabID).flatMap { id in tabs.first { $0.id == id } }
        // A private partner stays out of the folder, split and all.
        for member in [tab, partner].compactMap({ $0 }) where folder == nil || !member.isPrivate {
            member.folderID = folder
            // A tab lives either in the pins or in a folder, never both.
            if folder != nil { member.pinned = false }
        }
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
        // The partner joins the target's folder, unless it is private: folders
        // are saved, private tabs never are. In a folder it is no longer pinned.
        if !second.isPrivate { second.folderID = first.folderID }
        if second.folderID != nil { second.pinned = false }
        first.resume()
        second.resume()
        select(target); persistSession(); return true
    }
    /// Change command targeting while keeping the split's left/right order.
    func activateSplitPane(_ id: UUID) {
        guard id != activeID, !agentTabIDs.contains(id),
              let activeID, let pair = splitPairs.first(where: { $0.contains(activeID) }),
              pair.contains(id) else { return }
        select(id)
    }

    /// Closes every tab below the sidebar divider: not pinned and in no
    /// folder. They close as one step that a single undo restores; private
    /// tabs are discarded and cannot be reopened.
    func clearTodayTabs() {
        closeTabs(visibleTabs.filter { !$0.pinned && $0.folderID == nil }.map(\.id), revealStartPage: true)
    }

    func separateSplit(_ id: UUID? = nil) {
        let target = id ?? activeID
        if let target { splitPairs.removeAll { $0.contains(target) } }
        if target == activeID || target == splitID { splitID = nil }
        persistSession()
    }
}

struct SpaceFolderRow: View {
    let folder: TabFolder
    @ObservedObject var model: BrowserModel
    @Environment(\.spaceTone) private var tone
    @State private var rename = false
    @State private var name = ""
    @State private var targeted = false
    @State private var sourceHovered = false
    @State private var revealTask: Task<Void, Never>?
    @State private var dismissTask: Task<Void, Never>?
    private var tabs: [BrowserTab] { model.visibleTabs.filter { $0.folderID == folder.id && !$0.pinned } }
    /// Collapsed and not opened for a drag, which shows all its tabs so any
    /// of them can take the drop.
    private var showsCollapsed: Bool { folder.collapsed && !model.springOpenFolderIDs.contains(folder.id) }
    /// Collapsed, the folder still shows its open page and that page's split partner.
    private var collapsedTabs: [BrowserTab] { showsCollapsed ? model.collapsedFolderTabs(tabs) : [] }
    private var displayedTabs: [BrowserTab] { model.sidebarFolderTabs(folder, members: tabs) }
    private var hiddenTabCount: Int { max(0, tabs.count - collapsedTabs.count) }
    var body: some View {
        VStack(spacing: 3) {
            Button {
                // A preview still waiting to open would appear over the
                // folder the click just expanded.
                revealTask?.cancel(); dismissTask?.cancel()
                model.hoveredFolderID = nil
                setCollapsed(!folder.collapsed)
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: showsCollapsed ? "folder.fill" : "folder")
                        .font(.system(size: 13))
                        .foregroundStyle(folder.color == nil ? tone.muted : folder.folderColor.color)
                        .frame(width: 16)
                    Text(folder.name).font(.system(size: 13)).lineLimit(1)
                    Spacer()
                    if !collapsedTabs.isEmpty && hiddenTabCount > 0 {
                        Text("+\(hiddenTabCount)")
                            .font(.system(size: 10, weight: .semibold))
                            .monospacedDigit()
                            .padding(.horizontal, 6)
                            .frame(height: 18)
                            .background(Chrome.rest, in: Capsule())
                            .accessibilityLabel(L("\(hiddenTabCount) weitere Tabs", "\(hiddenTabCount) more tabs"))
                    } else if showsCollapsed {
                        Text("\(tabs.count)").font(.system(size: 10)).opacity(0.6)
                    }
                }
                .foregroundStyle(tone.muted)
                .padding(.leading, 10).padding(.trailing, 10)
                .frame(height: Chrome.rowHeight)
                .chromeRow(selected: targeted, hovering: sourceHovered)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
                .accessibilityValue(folder.collapsed ? L("Eingeklappt", "Collapsed") : L("Ausgeklappt", "Expanded"))
                .contentShape(Rectangle())
                .sidebarDragSource("folder:\(folder.id.uuidString)", title: folder.name, icon: "folder.fill", model: model) {
                    model.beginSidebarFolderDrag(folder.id)
                }
                .onHover {
                    sourceHovered = $0
                    if model.draggingTabID != nil || model.draggingFolderID != nil {
                        model.hoveredFolderID = nil
                    } else if folder.collapsed || (!$0 && model.hoveredFolderID == folder.id) {
                        // Leaving must still close a preview the folder
                        // showed before it was expanded.
                        hoverFolder($0)
                    }
                }
                .popover(isPresented: folderPopoverPresented, arrowEdge: .leading) {
                    VStack(alignment: .leading, spacing: 5) {
                        HStack(spacing: 8) {
                            Image(systemName: "folder.fill").foregroundStyle(folder.folderColor.color)
                            Text(folder.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
                            Spacer()
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
                                            if let favicon = tab.favicon { Image(nsImage: favicon).resizable().scaledToFit() }
                                            else { Image(systemName: "globe").foregroundStyle(moss) }
                                        }.frame(width: 16, height: 16)
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
                                .buttonStyle(OrganizationPopoverButtonStyle())
                                .contentShape(Rectangle())
                                .sidebarDragSource(tab.id.uuidString, title: tab.sidebarTitle, model: model) {
                                    // The popover closes as the drag starts; the
                                    // folder opens in the sidebar instead (see
                                    // `beginSidebarTabDrag`) and must not bring it back.
                                    revealTask?.cancel(); dismissTask?.cancel()
                                    model.beginSidebarTabDrag(tab.id)
                                    model.hoveredFolderID = nil
                                }
                            }
                        }
                    }
                    .padding(10).frame(width: 260).background(paper)
                    // Only the whole popover decides: rows reporting their own
                    // exit closed it while the pointer crossed the title line.
                    .onHover(perform: hoverFolder)
                }
                .onChange(of: model.hoveredFolderID) { _, hoveredID in
                    // A transient macOS popover consumes the first click on its
                    // source view. Treat dismissal while still hovering the
                    // closed folder as that intended click and expand it.
                    if hoveredID == nil && sourceHovered && folder.collapsed && model.draggingTabID == nil { setCollapsed(false) }
                }
                .onDrop(of: [.plainText], delegate: FolderHeaderDropDelegate(
                    folderID: folder.id, rowHeight: 36, model: model, tabTargeted: $targeted
                ))
                .contextMenu {
                    Button(L("Ordner umbenennen …")) { name = folder.name; rename = true }
                    FolderColorMenu(folder: folder, model: model)
                    Button(L("Ordner auflösen · Tabs behalten")) { model.dissolveFolder(folder.id) }
                }
            VStack(spacing: 0) {
                ForEach(displayedTabs) { tab in
                    TabRow(tab: tab, model: model, reorderGap: 3).padding(.leading, 15)
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
            if model.hoveredFolderID == folder.id { model.hoveredFolderID = nil }
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
                      !model.extensionActionPopupPresented,
                      model.folders.first(where: { $0.id == folder.id })?.collapsed == true else { return }
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
        return tab.space == folder.space && !tab.isPrivate
    }

    func dropEntered(info: DropInfo) {
        model.springLoadFolder(folderID)
        updateFeedback(info)
    }
    func dropExited(info: DropInfo) {
        model.cancelFolderSpringLoading(folderID)
        tabTargeted = false
        if model.folderDropFeedback?.targetID == folderID { model.folderDropFeedback = nil }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? {
        // Any tab, even one the folder would refuse, may want a split inside it.
        model.springLoadFolder(folderID)
        guard validateDrop(info: info) else { tabTargeted = false; return DropProposal(operation: .forbidden) }
        model.autoScrollSidebarDuringDrag()
        updateFeedback(info)
        return .sidebarDrop
    }
    func performDrop(info: DropInfo) -> Bool {
        model.cancelFolderSpringLoading(folderID)
        guard validateDrop(info: info) else { return false }
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
    func makeBody(configuration: Configuration) -> some View { Surface(configuration: configuration) }

    private struct Surface: View {
        let configuration: ButtonStyle.Configuration
        @State private var hovering = false
        var body: some View {
            configuration.label
                .contentShape(Rectangle())
                .background(moss.opacity(configuration.isPressed ? 0.16 : hovering ? 0.10 : 0), in: RoundedRectangle(cornerRadius: 9))
                .animation(.easeOut(duration: 0.12), value: hovering)
                .onHover { hovering = $0 }
        }
    }
}
