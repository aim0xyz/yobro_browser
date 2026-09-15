#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::loadSession(const QString &initialUrl) {
    if (!initialUrl.isEmpty()) {
        newUserTab(initialUrl);
        return;
    }
    QFile file(QString::fromStdString(paths_.session.string()));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QJsonObject root = document.object();
        const QJsonArray savedSpaces = root.value(QStringLiteral("spaces")).toArray();
        for (const QJsonValue &value : savedSpaces) {
            const QString space = value.toString().trimmed();
            if (!space.isEmpty() && !spaces_.contains(space, Qt::CaseInsensitive)) spaces_.append(space);
        }
        const QJsonObject savedIcons = root.value(QStringLiteral("spaceIcons")).toObject();
        for (auto it = savedIcons.begin(); it != savedIcons.end(); ++it) {
            const QString icon = it.value().toString();
            if (spaces_.contains(it.key()) && !icon.isEmpty() && icon.size() <= 8) spaceIcons_.insert(it.key(), icon);
        }
        const QString requestedSpace = root.value(QStringLiteral("activeSpace")).toString();
        if (spaces_.contains(requestedSpace)) activeSpace_ = requestedSpace;
        for (const QJsonValue &value : root.value(QStringLiteral("folders")).toArray()) {
            const QJsonObject folder = value.toObject();
            const QString id = folder.value(QStringLiteral("id")).toString();
            const QString name = folder.value(QStringLiteral("name")).toString().trimmed();
            const QString space = folder.value(QStringLiteral("space")).toString();
            const QString color = folder.value(QStringLiteral("color")).toString();
            if (!id.isEmpty() && !name.isEmpty() && spaces_.contains(space)) {
                workspaceFolders_.push_back({id, name, space, QColor(color).isValid() ? QColor(color).name(QColor::HexRgb) : QStringLiteral("#536157")});
            }
        }
        for (const QJsonValue &value : root.value(QStringLiteral("collapsedFolders")).toArray()) {
            const QString id = value.toString();
            if (!id.isEmpty()) collapsedFolderIds_.insert(id);
        }
        for (const QJsonValue &value : root.value(QStringLiteral("closedTabs")).toArray()) {
            if (closedTabs_.size() == 20) break;
            const QJsonObject record = value.toObject();
            const QString url = record.value(QStringLiteral("url")).toString();
            const QString space = record.value(QStringLiteral("space")).toString();
            const QString folder = record.value(QStringLiteral("folder")).toString();
            if ((!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) || !spaces_.contains(space)) continue;
            const bool validFolder = folder.isEmpty() || std::any_of(workspaceFolders_.cbegin(), workspaceFolders_.cend(), [&folder, &space](const WorkspaceFolder &item) {
                return item.id == folder && item.space == space;
            });
            closedTabs_.push_back({
                .url = url,
                .title = record.value(QStringLiteral("title")).toString(),
                .space = space,
                .folder = validFolder ? folder : QString(),
                .pinned = record.value(QStringLiteral("pinned")).toBool(false),
            });
        }
        const QByteArray geometry = QByteArray::fromBase64(
            root.value(QStringLiteral("geometry")).toString().toLatin1()
        );
        if (!geometry.isEmpty()) restoreGeometry(geometry);
        // A collapsed sidebar comes back as the icon column, not as nothing.
        if (!root.value(QStringLiteral("sidebarVisible")).toBool(true)) setSidebarCompact(true);

        // Notes come back before the pages so their ids keep their old order.
        for (const QJsonValue &value : root.value(QStringLiteral("notes")).toArray()) {
            const QJsonObject record = value.toObject();
            const QString space = record.value(QStringLiteral("space")).toString();
            const QString restoredSpace = spaces_.contains(space) ? space : activeSpace_;
            const QString folder = record.value(QStringLiteral("folder")).toString();
            const QString priorSpace = activeSpace_;
            activeSpace_ = restoredSpace;
            NoteEditor *editor = newNote(
                record.value(QStringLiteral("title")).toString(),
                record.value(QStringLiteral("html")).toString()
            );
            activeSpace_ = priorSpace;
            const QString id = noteIdFor(editor);
            if (id.isEmpty()) continue;
            auto &workspace = workspaceTabs_[id.toStdString()];
            workspace.space = restoredSpace;
            workspace.folder = std::any_of(workspaceFolders_.begin(), workspaceFolders_.end(),
                                           [&folder, &restoredSpace](const WorkspaceFolder &item) {
                                               return item.id == folder && item.space == restoredSpace;
                                           }) ? folder : QString();
            workspace.pinned = record.value(QStringLiteral("pinned")).toBool(false);
            workspace.order = record.value(QStringLiteral("order")).toInt(workspace.order);
            if (workspace.order >= nextWorkspaceOrder_) nextWorkspaceOrder_ = workspace.order + 1;
        }

        std::vector<std::string> restoredOrder;
        const QJsonArray savedTabs = root.value(QStringLiteral("tabs")).toArray();
        for (const QJsonValue &value : savedTabs) {
            const QJsonObject record = value.toObject();
            const QString url = record.isEmpty() ? value.toString() : record.value(QStringLiteral("url")).toString();
            if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) continue;
            const QString space = record.value(QStringLiteral("space")).toString();
            const QString folder = record.value(QStringLiteral("folder")).toString();
            const bool validSpace = spaces_.contains(space);
            const QString restoredSpace = validSpace ? space : activeSpace_;
            const QString priorSpace = activeSpace_;
            activeSpace_ = restoredSpace;
            auto *page = newUserTab(url);
            activeSpace_ = priorSpace;
            auto &workspace = workspaceTabs_[page->state().id];
            workspace.space = restoredSpace;
            workspace.folder = std::any_of(workspaceFolders_.begin(), workspaceFolders_.end(), [&folder, &restoredSpace](const WorkspaceFolder &item) {
                return item.id == folder && item.space == restoredSpace;
            }) ? folder : QString();
            workspace.pinned = record.value(QStringLiteral("pinned")).toBool(false);
            // Sessions written before reordering existed have no order field.
            workspace.order = record.value(QStringLiteral("order")).toInt(workspace.order);
            if (workspace.order >= nextWorkspaceOrder_) nextWorkspaceOrder_ = workspace.order + 1;
            const QIcon favicon = decodeIcon(record.value(QStringLiteral("favicon")).toString());
            if (!favicon.isNull()) restoredFavicons_.emplace(page->state().id, favicon);
            const QString savedTitle = record.value(QStringLiteral("title")).toString().left(200);
            if (!savedTitle.isEmpty()) restoredTitles_.emplace(page->state().id, savedTitle);
            restoredOrder.push_back(page->state().id);
        }

        // Both indices refer to the saved tab array, so they are resolved only
        // after every tab of that array exists again.
        const int activeIndex = root.value(QStringLiteral("activeTab")).toInt(-1);
        if (activeIndex >= 0 && activeIndex < static_cast<int>(restoredOrder.size()))
            restoreActiveIndex_ = activeIndex;
        const int splitIndex = root.value(QStringLiteral("splitTab")).toInt(-1);
        if (splitIndex >= 0 && splitIndex < static_cast<int>(restoredOrder.size()))
            splitPageId_ = restoredOrder[static_cast<std::size_t>(splitIndex)];
        if (restoreActiveIndex_ >= 0) {
            const std::string &activeId = restoredOrder[static_cast<std::size_t>(restoreActiveIndex_)];
            const auto workspace = workspaceTabs_.find(activeId);
            if (workspace != workspaceTabs_.end()) activeSpace_ = workspace->second.space;
            (void)session_.setActiveUserTab(activeId);
        }
    }
    if (spacePicker_) {
        const QSignalBlocker blocker(spacePicker_);
        spacePicker_->clear();
        spacePicker_->addItems(spaces_);
    }
    switchSpace(activeSpace_);
    updateClosedTabAction();
    if (session_.userPages().empty()) newUserTab();
}

void SpikeWindow::saveSession() const {
    QJsonArray tabs;
    const engine::BrowserPage *activePage = session_.activeUserPage();
    int activeIndex = -1;
    int splitIndex = -1;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent || tab.privatePage) continue;
        const QString url = QString::fromStdString(tab.state.url);
        if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) continue;
        // Indices refer to this array, which loadSession recreates in order.
        if (tab.page == activePage) activeIndex = static_cast<int>(tabs.size());
        if (!splitPageId_.empty() && tab.state.id == splitPageId_) splitIndex = static_cast<int>(tabs.size());
        const auto workspace = workspaceTabs_.find(tab.state.id);
        QJsonObject record;
        record.insert(QStringLiteral("url"), url);
        const QString favicon = encodeIcon(faviconFor(tab));
        if (!favicon.isEmpty()) record.insert(QStringLiteral("favicon"), favicon);
        if (!tab.state.title.empty())
            record.insert(QStringLiteral("title"), QString::fromStdString(tab.state.title));
        record.insert(QStringLiteral("space"), workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space);
        record.insert(QStringLiteral("folder"), workspace == workspaceTabs_.end() ? QString() : workspace->second.folder);
        record.insert(QStringLiteral("pinned"), workspace != workspaceTabs_.end() && workspace->second.pinned);
        record.insert(QStringLiteral("order"), workspace == workspaceTabs_.end() ? 0 : workspace->second.order);
        tabs.append(record);
    }
    QJsonArray spaces;
    for (const QString &space : spaces_) spaces.append(space);
    QJsonArray folders;
    for (const WorkspaceFolder &folder : workspaceFolders_) {
        QJsonObject record;
        record.insert(QStringLiteral("id"), folder.id);
        record.insert(QStringLiteral("name"), folder.name);
        record.insert(QStringLiteral("space"), folder.space);
        record.insert(QStringLiteral("color"), QColor(folder.color).isValid() ? QColor(folder.color).name(QColor::HexRgb) : QStringLiteral("#536157"));
        folders.append(record);
    }
    QJsonObject spaceIcons;
    for (auto it = spaceIcons_.cbegin(); it != spaceIcons_.cend(); ++it) {
        if (spaces_.contains(it.key()) && !it.value().isEmpty()) spaceIcons.insert(it.key(), it.value());
    }
    QJsonArray collapsedFolders;
    for (const QString &id : collapsedFolderIds_) collapsedFolders.append(id);
    QJsonArray closedTabs;
    for (const ClosedTab &closed : closedTabs_) {
        if ((!closed.url.startsWith(QStringLiteral("http://")) && !closed.url.startsWith(QStringLiteral("https://"))) || !spaces_.contains(closed.space)) continue;
        QJsonObject record;
        record.insert(QStringLiteral("url"), closed.url);
        record.insert(QStringLiteral("title"), closed.title);
        record.insert(QStringLiteral("space"), closed.space);
        record.insert(QStringLiteral("folder"), closed.folder);
        record.insert(QStringLiteral("pinned"), closed.pinned);
        closedTabs.append(record);
    }
    QJsonArray noteRecords;
    for (const NoteTab &note : notes_) {
        const auto workspace = workspaceTabs_.find(note.id.toStdString());
        QJsonObject record;
        record.insert(QStringLiteral("id"), note.id);
        record.insert(QStringLiteral("title"), note.editor->title());
        record.insert(QStringLiteral("html"), note.editor->html());
        record.insert(QStringLiteral("space"),
                      workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space);
        record.insert(QStringLiteral("folder"),
                      workspace == workspaceTabs_.end() ? QString() : workspace->second.folder);
        record.insert(QStringLiteral("pinned"),
                      workspace != workspaceTabs_.end() && workspace->second.pinned);
        record.insert(QStringLiteral("order"),
                      workspace == workspaceTabs_.end() ? 0 : workspace->second.order);
        noteRecords.append(record);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("notes"), noteRecords);
    root.insert(QStringLiteral("activeSpace"), activeSpace_);
    root.insert(QStringLiteral("spaces"), spaces);
    root.insert(QStringLiteral("spaceIcons"), spaceIcons);
    root.insert(QStringLiteral("folders"), folders);
    root.insert(QStringLiteral("collapsedFolders"), collapsedFolders);
    root.insert(QStringLiteral("tabs"), tabs);
    root.insert(QStringLiteral("closedTabs"), closedTabs);
    root.insert(QStringLiteral("activeTab"), activeIndex);
    root.insert(QStringLiteral("splitTab"), splitIndex);
    root.insert(QStringLiteral("sidebarVisible"), workspaceSidebar_ == nullptr || workspaceSidebar_->isVisible());
    root.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    QSaveFile file(QString::fromStdString(paths_.session.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

void SpikeWindow::synchronizeSession() {
    if (synchronizing_ || !tabs_)
        return;
    synchronizing_ = true;
    attachLoginChannels();
    {
        const QSignalBlocker blocker(libraryAccess_);
        libraryAccess_->setChecked(session_.protocolState().libraryAccess);
    }

    std::vector<QWidget *> expected;
    std::vector<QWidget *> expectedSplit;
    bool splitPageVisible = false;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent)
            continue;
        const auto workspace = workspaceTabs_.find(tab.state.id);
        const QString tabSpace = workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space;
        if (!tab.privatePage && tabSpace != activeSpace_)
            continue;
        auto *page = asQtPage(tab.page);
        const QString pageId = QString::fromStdString(tab.state.id);
        // Favicons arrive asynchronously, so each view is watched exactly once.
        if (!faviconWatched_.contains(pageId)) {
            faviconWatched_.insert(pageId);
            QObject::connect(page->view(), &QWebEngineView::iconChanged, this, [this](const QIcon &) {
                if (!synchronizing_) synchronizeSession();
            });
        }
        const QString restoredLabel = titleFor(tab);
        const QString title = restoredLabel.isEmpty()
            ? (tab.privatePage ? L(QStringLiteral("Privat")) : L(QStringLiteral("Neue Seite")))
            : restoredLabel;
        const bool split = !splitPageId_.empty() && tab.state.id == splitPageId_;
        QTabWidget *host = split ? splitTabs_ : tabs_;
        if (split) {
            splitPageVisible = true;
            expectedSplit.push_back(page->view());
        } else {
            expected.push_back(page->view());
        }
        // Remove the view from the other host first so a single reparent happens.
        QTabWidget *other = split ? tabs_ : splitTabs_;
        const int staleIndex = other->indexOf(page->view());
        if (staleIndex >= 0) other->removeTab(staleIndex);
        int index = host->indexOf(page->view());
        if (index < 0)
            index = host->addTab(page->view(), title.left(42));
        host->setTabText(index, title.left(42));
        host->setTabIcon(index, faviconFor(tab));
    }
    for (int index = tabs_->count() - 1; index >= 0; --index) {
        if (std::find(expected.begin(), expected.end(), tabs_->widget(index)) == expected.end())
            tabs_->removeTab(index);
    }
    for (int index = splitTabs_->count() - 1; index >= 0; --index) {
        if (std::find(expectedSplit.begin(), expectedSplit.end(), splitTabs_->widget(index)) == expectedSplit.end())
            splitTabs_->removeTab(index);
    }
    // A split partner that was closed or moved to another space ends the split.
    if (!splitPageId_.empty() && !splitPageVisible) splitPageId_.clear();
    const bool splitActive = !splitPageId_.empty();
    if (splitTabs_->isVisible() != splitActive) {
        splitTabs_->setVisible(splitActive);
        if (splitActive) contentSplitter_->setSizes({1, 1});
    }
    if (auto *active = session_.activeUserPage()) {
        auto *page = asQtPage(active);
        const int index = tabs_->indexOf(page->view());
        if (index >= 0 && tabs_->currentIndex() != index)
            tabs_->setCurrentIndex(index);
    }
    refreshWorkspaceSidebar();
    refreshCompactSidebar();

    auto *agent = activeAgentPage(false);
    QWebEngineView *desiredView = agent ? agent->view() : nullptr;
    if (displayedAgentView_ != desiredView) {
        if (displayedAgentView_) {
            agentWebLayout_->removeWidget(displayedAgentView_);
            displayedAgentView_->hide();
        }
        displayedAgentView_ = desiredView;
        if (displayedAgentView_) {
            agentPlaceholder_->hide();
            agentWebLayout_->addWidget(displayedAgentView_);
            displayedAgentView_->show();
        } else {
            agentPlaceholder_->show();
        }
    }

    if (auto *active = session_.activeUserPage()) {
        const auto views = session_.tabViews();
        const auto activeView = std::find_if(views.begin(), views.end(), [active](const controller::SessionTabView &view) {
            return view.page == active;
        });
        if (activeView != views.end()) {
            if (activeView->state.error) {
                const QString prefix = activeView->rendererRecovery == controller::RendererRecoveryState::failed
                    ? QStringLiteral("Die Seite wurde beendet. ")
                    : QString();
                status_->setText(prefix + QString::fromStdString(*activeView->state.error));
            } else if (activeView->rendererRecovery == controller::RendererRecoveryState::recovering) {
                status_->setText(L(QStringLiteral("Der Seiteninhalt wird nach einem Absturz neu geladen…")));
            } else if (activeView->rendererRecovery == controller::RendererRecoveryState::failed) {
                status_->setText(L(QStringLiteral("Die Seite wurde beendet. Der Seiteninhalt konnte nicht wiederhergestellt werden.")));
            } else if (activeView->state.loading) {
                status_->setText(L(QStringLiteral("Laden…")));
            } else {
                status_->setText(L(QStringLiteral("Bereit")));
            }
        }
    }
    synchronizing_ = false;
    applyStoredZoom();
    updateNavigationState();
    synchronizePermissionPrompt();
}

} // namespace yobro::spike
