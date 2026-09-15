#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::switchSpace(const QString &space) {
    if (!spaces_.contains(space)) return;
    activeSpace_ = space;
    if (spacePicker_ && spacePicker_->currentText() != space) {
        const QSignalBlocker blocker(spacePicker_);
        spacePicker_->setCurrentText(space);
    }
    if (folderPicker_) {
        const QSignalBlocker blocker(folderPicker_);
        folderPicker_->clear();
        folderPicker_->addItem(L(QStringLiteral("Ohne Ordner")), QString());
        for (const WorkspaceFolder &folder : workspaceFolders_)
            if (folder.space == activeSpace_) folderPicker_->addItem(folder.name, folder.id);
    }
    // Each space may route through its own proxy, so the switch has to follow.
    applySpaceProxy();
    // A run belongs to the space it started in and is abandoned on a switch, so
    // an answer never lands in the wrong conversation.
    if (chatRunner_ && chatRunner_->running() && chatRunner_->runningSpace() != activeSpace_)
        chatRunner_->cancel();
    if (chatPanel_) chatPanel_->refresh();
    synchronizeSession();
    saveSession();
}

void SpikeWindow::createSpace() {
    bool accepted = false;
    const QString name = QInputDialog::getText(this, L(QStringLiteral("Neuer Space")), L(QStringLiteral("Name")), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.size() > 40 || spaces_.contains(name, Qt::CaseInsensitive)) return;
    spaces_.append(name);
    if (spacePicker_) spacePicker_->addItem(name);
    switchSpace(name);
}

void SpikeWindow::createFolder() {
    bool accepted = false;
    const QString name = QInputDialog::getText(this, L(QStringLiteral("Neuer Ordner")), L(QStringLiteral("Name")), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.size() > 60) return;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    workspaceFolders_.push_back({.id = id, .name = name, .space = activeSpace_, .color = defaultFolderColor()});
    switchSpace(activeSpace_);
}

void SpikeWindow::renameSpace(const QString &space) {
    if (!spaces_.contains(space)) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, L(QStringLiteral("Space umbenennen")), L(QStringLiteral("Name")), QLineEdit::Normal, space, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.size() > 40 || (name.compare(space, Qt::CaseInsensitive) != 0 && spaces_.contains(name, Qt::CaseInsensitive))) return;
    if (name == space) return;
    const int index = spaces_.indexOf(space);
    spaces_[index] = name;
    for (auto &[pageId, tab] : workspaceTabs_)
        if (tab.space == space) tab.space = name;
    for (WorkspaceFolder &folder : workspaceFolders_)
        if (folder.space == space) folder.space = name;
    if (spaceIcons_.contains(space)) spaceIcons_.insert(name, spaceIcons_.take(space));
    // The proxy follows the rename, otherwise the space would silently lose it.
    if (const QString problem = proxies_.rename(space, name); !problem.isEmpty())
        showStatus(problem);
    // So does the conversation, otherwise the chat history would look empty
    // after a rename.
    if (chatRunner_) chatRunner_->cancel();
    chat_.renameSpace(space, name);
    if (activeSpace_ == space) activeSpace_ = name;
    if (spacePicker_) {
        const QSignalBlocker blocker(spacePicker_);
        spacePicker_->clear();
        spacePicker_->addItems(spaces_);
    }
    switchSpace(activeSpace_);
}

void SpikeWindow::setSpaceIcon(const QString &space) {
    if (!spaces_.contains(space)) return;
    const QStringList icons{QStringLiteral("◉"), QStringLiteral("◆"), QStringLiteral("●"), QStringLiteral("✦"), QStringLiteral("⌂")};
    bool accepted = false;
    const QString icon = QInputDialog::getItem(this, L(QStringLiteral("Space-Symbol")), L(QStringLiteral("Symbol")), icons, icons.indexOf(spaceIcons_.value(space, icons.front())), false, &accepted);
    if (!accepted) return;
    spaceIcons_.insert(space, icon);
    synchronizeSession();
    saveSession();
}

void SpikeWindow::renameFolder(const QString &folderId) {
    auto found = std::find_if(workspaceFolders_.begin(), workspaceFolders_.end(), [&folderId](const WorkspaceFolder &folder) { return folder.id == folderId; });
    if (found == workspaceFolders_.end() || found->space != activeSpace_) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, L(QStringLiteral("Ordner umbenennen")), L(QStringLiteral("Name")), QLineEdit::Normal, found->name, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name.size() > 60 || name == found->name) return;
    found->name = name;
    switchSpace(activeSpace_);
}

void SpikeWindow::setFolderColor(const QString &folderId) {
    auto found = std::find_if(workspaceFolders_.begin(), workspaceFolders_.end(), [&folderId](const WorkspaceFolder &folder) { return folder.id == folderId; });
    if (found == workspaceFolders_.end() || found->space != activeSpace_) return;
    bool accepted = false;
    const QString color = QInputDialog::getText(this, L(QStringLiteral("Ordnerfarbe")), L(QStringLiteral("Hex-Farbe")), QLineEdit::Normal, found->color, &accepted).trimmed();
    if (!accepted || !QColor(color).isValid()) return;
    found->color = QColor(color).name(QColor::HexRgb);
    synchronizeSession();
    saveSession();
}

void SpikeWindow::dissolveFolder(const QString &folderId) {
    const auto found = std::find_if(workspaceFolders_.begin(), workspaceFolders_.end(), [&folderId](const WorkspaceFolder &folder) { return folder.id == folderId; });
    if (found == workspaceFolders_.end() || found->space != activeSpace_) return;
    const QString folderSpace = found->space;
    if (QMessageBox::question(this, L(QStringLiteral("Ordner auflösen")), L(QStringLiteral("Tabs bleiben erhalten und werden aus diesem Ordner entfernt."))) != QMessageBox::Yes) return;
    for (auto &[pageId, tab] : workspaceTabs_)
        if (tab.space == folderSpace && tab.folder == folderId) tab.folder.clear();
    collapsedFolderIds_.remove(folderId);
    workspaceFolders_.erase(found);
    switchSpace(activeSpace_);
}

void SpikeWindow::showWorkspaceContextMenu(const QPoint &position) {
    QTreeWidgetItem *item = workspaceTree_ ? workspaceTree_->itemAt(position) : nullptr;
    if (!item) return;
    const QString group = item->data(0, Qt::UserRole + 2).toString();
    const QString space = item->data(0, Qt::UserRole + 1).toString();
    const QString folderId = item->data(0, Qt::UserRole + 3).toString();
    QMenu menu(workspaceTree_);
    if (item->parent() == nullptr && !space.isEmpty()) {
        auto *rename = menu.addAction(L(QStringLiteral("Space umbenennen")));
        rename->setObjectName(QStringLiteral("renameSpaceAction"));
        connect(rename, &QAction::triggered, this, [this, space] { renameSpace(space); });
        auto *icon = menu.addAction(L(QStringLiteral("Space-Symbol ändern")));
        icon->setObjectName(QStringLiteral("setSpaceIconAction"));
        connect(icon, &QAction::triggered, this, [this, space] { setSpaceIcon(space); });
    } else if (const QString noteId = item->data(0, Qt::UserRole).toString();
               !noteId.isEmpty() && noteEditorFor(noteId)) {
        auto *show = menu.addAction(L(QStringLiteral("Notiz anzeigen"), QStringLiteral("Show note")));
        show->setObjectName(QStringLiteral("showNoteAction"));
        connect(show, &QAction::triggered, this, [this, noteId] { activateNote(noteId); });
        auto *pin = menu.addAction(L(QStringLiteral("Anpinnen oder lösen")));
        pin->setObjectName(QStringLiteral("toggleNotePinnedAction"));
        connect(pin, &QAction::triggered, this, [this, noteId] {
            const auto workspace = workspaceTabs_.find(noteId.toStdString());
            if (workspace == workspaceTabs_.end()) return;
            workspace->second.pinned = !workspace->second.pinned;
            refreshWorkspaceSidebar();
            saveSession();
        });
        menu.addSeparator();
        auto *close = menu.addAction(L(QStringLiteral("Notiz schließen"), QStringLiteral("Close note")));
        close->setObjectName(QStringLiteral("closeNoteAction"));
        connect(close, &QAction::triggered, this, [this, noteId] { closeNote(noteId); });
    } else if (!item->data(0, Qt::UserRole).toString().isEmpty()) {
        // Tab row: the actions that have no key of their own live here.
        const QString pageId = item->data(0, Qt::UserRole).toString();
        auto *activate = menu.addAction(L(QStringLiteral("Tab anzeigen")));
        connect(activate, &QAction::triggered, this, [this, item] { activateWorkspaceItem(item); });
        auto *duplicate = menu.addAction(L(QStringLiteral("Tab duplizieren")));
        duplicate->setObjectName(QStringLiteral("duplicateTabAction"));
        connect(duplicate, &QAction::triggered, this, [this, item] {
            activateWorkspaceItem(item);
            duplicateCurrentTab();
        });
        auto *pin = menu.addAction(L(QStringLiteral("Anpinnen oder lösen")));
        pin->setObjectName(QStringLiteral("togglePinnedAction"));
        connect(pin, &QAction::triggered, this, [this, item] {
            activateWorkspaceItem(item);
            toggleCurrentTabPinned();
        });
        auto *copy = menu.addAction(L(QStringLiteral("Adresse kopieren")));
        copy->setObjectName(QStringLiteral("copyAddressAction"));
        connect(copy, &QAction::triggered, this, [this, item] {
            activateWorkspaceItem(item);
            copyCurrentAddress();
        });
        menu.addSeparator();
        auto *close = menu.addAction(L(QStringLiteral("Tab schließen")));
        close->setObjectName(QStringLiteral("closeTabAction"));
        connect(close, &QAction::triggered, this, [this, pageId] {
            (void)session_.setActiveUserTab(pageId.toStdString());
            if (tabs_ && tabs_->currentIndex() >= 0) closeUserTab(tabs_->currentIndex());
        });
    } else if (group == QStringLiteral("folderGroup") && !folderId.isEmpty()) {
        auto *rename = menu.addAction(L(QStringLiteral("Ordner umbenennen")));
        rename->setObjectName(QStringLiteral("renameFolderAction"));
        connect(rename, &QAction::triggered, this, [this, folderId] { renameFolder(folderId); });
        auto *color = menu.addAction(L(QStringLiteral("Ordnerfarbe ändern")));
        color->setObjectName(QStringLiteral("setFolderColorAction"));
        connect(color, &QAction::triggered, this, [this, folderId] { setFolderColor(folderId); });
        menu.addSeparator();
        auto *dissolve = menu.addAction(L(QStringLiteral("Ordner auflösen")));
        dissolve->setObjectName(QStringLiteral("dissolveFolderAction"));
        connect(dissolve, &QAction::triggered, this, [this, folderId] { dissolveFolder(folderId); });
    } else return;
    menu.exec(workspaceTree_->viewport()->mapToGlobal(position));
}

void SpikeWindow::moveCurrentTabToFolder() {
    auto *page = currentUserPage();
    if (!page || page->profile()->isOffTheRecord()) return;
    auto found = workspaceTabs_.find(page->state().id);
    if (found == workspaceTabs_.end()) return;
    found->second.folder = folderPicker_ ? folderPicker_->currentData().toString() : QString();
    saveSession();
    synchronizeSession();
}

QIcon SpikeWindow::faviconFor(const controller::SessionTabView &tab) const {
    const QIcon live = asQtPage(tab.page)->view()->icon();
    if (!live.isNull()) return live;
    // Until the page reloads its icon, the one from the last session is shown.
    const auto restored = restoredFavicons_.find(tab.state.id);
    return restored == restoredFavicons_.end() ? QIcon() : restored->second;
}

QString SpikeWindow::titleFor(const controller::SessionTabView &tab) const {
    if (!tab.state.title.empty()) return QString::fromStdString(tab.state.title);
    const auto restored = restoredTitles_.find(tab.state.id);
    if (restored != restoredTitles_.end() && !restored->second.isEmpty()) return restored->second;
    return {};
}

void SpikeWindow::refreshWorkspaceSidebar() {
    if (!workspaceTree_) return;
    const QSignalBlocker blocker(workspaceTree_);
    workspaceTree_->clear();

    auto *spaceItem = new QTreeWidgetItem(workspaceTree_, {QStringLiteral("◈  ") + spaceIcons_.value(activeSpace_) + QStringLiteral(" ") + activeSpace_});
    spaceItem->setData(0, Qt::UserRole + 1, activeSpace_);
    // Only the group rows accept drops, so tabs cannot be nested under a space.
    spaceItem->setFlags(spaceItem->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
    spaceItem->setExpanded(true);
    spaceItem->setSizeHint(0, QSize(0, 46));
    // Item text and washes come from the window style sheet so both
    // appearances stay correct; only folder colours are data-driven.
    spaceItem->setFont(0, QFont(spaceItem->font(0).family(), -1, QFont::DemiBold));

    auto addGroup = [spaceItem, this](const QString &label, const QString &name, const QString &folderId = {}, const QString &color = defaultFolderColor()) {
        auto *group = new QTreeWidgetItem(spaceItem, {label});
        group->setData(0, Qt::UserRole + 2, name);
        group->setData(0, Qt::UserRole + 3, folderId);
        group->setFlags((group->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled)) | Qt::ItemIsDropEnabled);
        group->setForeground(0, QColor(QColor(color).isValid() ? color : themePalette(currentAppearanceIsDark()).textMuted));
        group->setFont(0, QFont(group->font(0).family(), -1, QFont::DemiBold));
        group->setSizeHint(0, QSize(0, 31));
        group->setExpanded(!collapsedFolderIds_.contains(folderId));
        return group;
    };
    auto *pinnedGroup = addGroup(L(QStringLiteral("ANGEPINNT")), QStringLiteral("pinnedTabsGroup"));
    std::map<QString, QTreeWidgetItem *> folderGroups;
    for (const WorkspaceFolder &folder : workspaceFolders_) {
        if (folder.space != activeSpace_) continue;
        auto *group = addGroup(folder.name, QStringLiteral("folderGroup"), folder.id, folder.color);
        folderGroups.emplace(folder.id, group);
    }
    auto *unfiledGroup = addGroup(L(QStringLiteral("DEINE TABS")), QStringLiteral("unfiledTabsGroup"));

    const engine::BrowserPage *activePage = session_.activeUserPage();
    QTreeWidgetItem *activeItem = nullptr;
    // Render in the user's stored order; private tabs keep their arrival order.
    std::vector<controller::SessionTabView> ordered;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent) continue;
        ordered.push_back(tab);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [this](const controller::SessionTabView &left, const controller::SessionTabView &right) {
        const auto leftEntry = workspaceTabs_.find(left.state.id);
        const auto rightEntry = workspaceTabs_.find(right.state.id);
        const int leftOrder = leftEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : leftEntry->second.order;
        const int rightOrder = rightEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : rightEntry->second.order;
        return leftOrder < rightOrder;
    });
    for (const controller::SessionTabView &tab : ordered) {
        const auto workspace = workspaceTabs_.find(tab.state.id);
        const QString tabSpace = workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space;
        if (!tab.privatePage && tabSpace != activeSpace_) continue;
        QTreeWidgetItem *group = unfiledGroup;
        if (!tab.privatePage && workspace != workspaceTabs_.end() && workspace->second.pinned) {
            group = pinnedGroup;
        } else if (!tab.privatePage && workspace != workspaceTabs_.end() && !workspace->second.folder.isEmpty()) {
            const auto folder = folderGroups.find(workspace->second.folder);
            if (folder != folderGroups.end()) group = folder->second;
        }
        const QString label = titleFor(tab);
        QString title = label.isEmpty() ? L(QStringLiteral("Neue Seite")) : label;
        if (tab.privatePage) title = L(QStringLiteral("◌  Private"));
        const QIcon favicon = faviconFor(tab);
        // Websites without a favicon keep the neutral text marker.
        if (!tab.privatePage && favicon.isNull()) title = QStringLiteral("◎  ") + title;
        if (!splitPageId_.empty() && tab.state.id == splitPageId_)
            title = QStringLiteral("⬓  ") + title;
        auto *item = new QTreeWidgetItem(group, {title.left(42)});
        item->setFlags((item->flags() & ~Qt::ItemIsDropEnabled) | Qt::ItemIsDragEnabled);
        if (!tab.privatePage && !favicon.isNull()) item->setIcon(0, favicon);
        item->setSizeHint(0, QSize(0, 44));
        item->setData(0, Qt::UserRole, QString::fromStdString(tab.state.id));
        item->setData(0, Qt::UserRole + 1, tabSpace);
        item->setToolTip(0, QString::fromStdString(tab.state.url));
        if (tab.page == activePage) activeItem = item;
    }
    // Notes sit in the same groups as pages and are sorted by the same order.
    std::vector<NoteTab> orderedNotes = notes_;
    std::stable_sort(orderedNotes.begin(), orderedNotes.end(), [this](const NoteTab &left, const NoteTab &right) {
        const auto leftEntry = workspaceTabs_.find(left.id.toStdString());
        const auto rightEntry = workspaceTabs_.find(right.id.toStdString());
        const int leftOrder = leftEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : leftEntry->second.order;
        const int rightOrder = rightEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : rightEntry->second.order;
        return leftOrder < rightOrder;
    });
    QWidget *currentWidget = tabs_ ? tabs_->currentWidget() : nullptr;
    for (const NoteTab &note : orderedNotes) {
        const auto workspace = workspaceTabs_.find(note.id.toStdString());
        const QString noteSpace = workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space;
        if (noteSpace != activeSpace_) continue;
        QTreeWidgetItem *group = unfiledGroup;
        if (workspace != workspaceTabs_.end() && workspace->second.pinned) {
            group = pinnedGroup;
        } else if (workspace != workspaceTabs_.end() && !workspace->second.folder.isEmpty()) {
            const auto folder = folderGroups.find(workspace->second.folder);
            if (folder != folderGroups.end()) group = folder->second;
        }
        auto *item = new QTreeWidgetItem(group, {QStringLiteral("✎  ") + note.editor->displayTitle().left(38)});
        item->setFlags((item->flags() & ~Qt::ItemIsDropEnabled) | Qt::ItemIsDragEnabled);
        item->setSizeHint(0, QSize(0, 44));
        item->setData(0, Qt::UserRole, note.id);
        item->setData(0, Qt::UserRole + 1, noteSpace);
        item->setToolTip(0, L(QStringLiteral("Notiz: "), QStringLiteral("Note: ")) + note.editor->displayTitle());
        if (note.editor == currentWidget) activeItem = item;
    }
    if (activeItem) workspaceTree_->setCurrentItem(activeItem);
}

void SpikeWindow::scheduleWorkspaceOrderCommit() {
    // Ignore the model churn caused by rebuilding the sidebar ourselves.
    if (synchronizing_ || workspaceOrderCommitPending_) return;
    workspaceOrderCommitPending_ = true;
    QTimer::singleShot(0, this, [this] {
        workspaceOrderCommitPending_ = false;
        applyWorkspaceOrderFromSidebar();
    });
}

void SpikeWindow::applyWorkspaceOrderFromSidebar() {
    if (!workspaceTree_ || synchronizing_) return;
    QTreeWidgetItem *spaceItem = workspaceTree_->topLevelItem(0);
    if (!spaceItem) return;
    int order = 0;
    bool changed = false;
    for (int groupIndex = 0; groupIndex < spaceItem->childCount(); ++groupIndex) {
        QTreeWidgetItem *group = spaceItem->child(groupIndex);
        const QString kind = group->data(0, Qt::UserRole + 2).toString();
        for (int tabIndex = 0; tabIndex < group->childCount(); ++tabIndex) {
            QTreeWidgetItem *item = group->child(tabIndex);
            const QString pageId = item->data(0, Qt::UserRole).toString();
            if (pageId.isEmpty()) continue;
            const auto entry = workspaceTabs_.find(pageId.toStdString());
            // Private tabs have no persisted workspace record and stay unfiled.
            if (entry == workspaceTabs_.end()) continue;
            const QString folder = kind == QStringLiteral("folderGroup")
                ? group->data(0, Qt::UserRole + 3).toString()
                : QString();
            const bool pinned = kind == QStringLiteral("pinnedTabsGroup");
            if (entry->second.order != order || entry->second.folder != folder || entry->second.pinned != pinned)
                changed = true;
            entry->second.order = order;
            entry->second.folder = folder;
            entry->second.pinned = pinned;
            ++order;
        }
    }
    if (order > nextWorkspaceOrder_) nextWorkspaceOrder_ = order;
    if (!changed) return;
    saveSession();
    synchronizeSession();
}

void SpikeWindow::activateWorkspaceItem(QTreeWidgetItem *item) {
    if (!item) return;
    const QString space = item->data(0, Qt::UserRole + 1).toString();
    const QString pageId = item->data(0, Qt::UserRole).toString();
    if (pageId.isEmpty()) {
        if (!space.isEmpty() && space != activeSpace_) switchSpace(space);
        return;
    }
    if (!space.isEmpty() && space != activeSpace_) switchSpace(space);
    // A note has no engine page; it is a widget of our own in the same tab bar.
    if (noteEditorFor(pageId)) {
        activateNote(pageId);
        return;
    }
    (void)session_.setActiveUserTab(pageId.toStdString());
}

void SpikeWindow::setWorkspaceGroupCollapsed(QTreeWidgetItem *item, bool collapsed) {
    if (!item || item->data(0, Qt::UserRole + 2).toString() != QStringLiteral("folderGroup")) return;
    const QString id = item->data(0, Qt::UserRole + 3).toString();
    if (id.isEmpty()) return;
    if (collapsed) collapsedFolderIds_.insert(id);
    else collapsedFolderIds_.remove(id);
    saveSession();
}

void SpikeWindow::setAgentPaneVisible(bool visible) {
    if (!agentPane_ || !mainSplitter_ || agentPaneVisible_ == visible) return;
    agentPaneVisible_ = visible;
    agentPane_->setVisible(visible);
    if (visible) mainSplitter_->setSizes({880, 480});
    else mainSplitter_->setSizes({1360, 0});
}

void SpikeWindow::toggleCurrentTabPinned() {
    auto *page = currentUserPage();
    if (!page || page->profile()->isOffTheRecord()) return;
    auto workspace = workspaceTabs_.find(page->state().id);
    if (workspace == workspaceTabs_.end()) return;
    workspace->second.pinned = !workspace->second.pinned;
    saveSession();
    synchronizeSession();
}

} // namespace yobro::spike
