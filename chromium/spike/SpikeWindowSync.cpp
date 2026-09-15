#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

// MARK: - Synchronisation

SyncSnapshot SpikeWindow::buildSyncSnapshot() const {
    SyncSnapshot snapshot;
    snapshot.spaces = spaces_;
    snapshot.currentSpace = activeSpace_;
    snapshot.modifiedAt = QDateTime::currentMSecsSinceEpoch();

    for (const auto &[name, symbol] : spaceIcons_.asKeyValueRange())
        snapshot.spaceIcons.insert(name, symbol);

    for (const WorkspaceFolder &folder : workspaceFolders_)
        snapshot.folders.push_back({folder.id, folder.name, folder.space, folder.color});

    // Notes travel with their text; pages travel with their address.
    for (const NoteTab &note : notes_) {
        const auto workspace = workspaceTabs_.find(note.id.toStdString());
        if (workspace == workspaceTabs_.end()) continue;
        SyncTab tab;
        tab.id = note.id;
        tab.title = note.editor->displayTitle();
        tab.space = workspace->second.space;
        tab.folder = workspace->second.folder;
        tab.pinned = workspace->second.pinned;
        tab.order = workspace->second.order;
        tab.note = true;
        tab.noteHtml = note.editor->html();
        snapshot.tabs.push_back(std::move(tab));
    }
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        // A private tab is never synced, and neither is a page without an address.
        if (page->profile()->isOffTheRecord()) continue;
        const auto state = page->state();
        const QString url = QString::fromStdString(state.url);
        if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://")))
            continue;
        const auto workspace = workspaceTabs_.find(state.id);
        if (workspace == workspaceTabs_.end()) continue;
        SyncTab tab;
        tab.id = QString::fromStdString(state.id);
        tab.url = url;
        tab.title = QString::fromStdString(state.title);
        tab.space = workspace->second.space;
        tab.folder = workspace->second.folder;
        tab.pinned = workspace->second.pinned;
        tab.order = workspace->second.order;
        snapshot.tabs.push_back(std::move(tab));
        if (currentUserPage() == page) snapshot.activeId = QString::fromStdString(state.id);
    }

    for (const ClosedTab &closed : closedTabs_) {
        SyncTab tab;
        // Closed tabs have no engine id any more, so the address identifies them.
        tab.id = closed.url;
        tab.url = closed.url;
        tab.title = closed.title;
        tab.space = spaces_.contains(closed.space) ? closed.space : activeSpace_;
        tab.folder = closed.folder;
        tab.pinned = closed.pinned;
        snapshot.closedTabs.push_back(std::move(tab));
    }

    try {
        const core::Json stored = library_.bookmarks({}, SyncSnapshot::maximumBookmarks);
        if (stored.isArray()) {
            for (const core::Json &entry : stored.asArray()) {
                if (!entry.isObject()) continue;
                SyncBookmark bookmark;
                if (const core::Json *value = entry.find("title"); value && value->isString())
                    bookmark.title = QString::fromStdString(value->asString());
                if (const core::Json *value = entry.find("url"); value && value->isString())
                    bookmark.url = QString::fromStdString(value->asString());
                if (const core::Json *value = entry.find("folder"); value && value->isString())
                    bookmark.folder = QString::fromStdString(value->asString());
                if (bookmark.url.isEmpty()) continue;
                snapshot.bookmarks.push_back(std::move(bookmark));
            }
        }
        const core::Json visits = library_.history({}, SyncSnapshot::maximumHistory);
        if (visits.isArray()) {
            for (const core::Json &entry : visits.asArray()) {
                if (!entry.isObject()) continue;
                SyncHistoryEntry record;
                if (const core::Json *value = entry.find("url"); value && value->isString())
                    record.url = QString::fromStdString(value->asString());
                if (const core::Json *value = entry.find("title"); value && value->isString())
                    record.title = QString::fromStdString(value->asString());
                if (const core::Json *value = entry.find("visits"); value && value->isInteger())
                    record.visits = value->asInteger();
                if (const core::Json *value = entry.find("lastVisit"); value && value->isString()) {
                    record.date = QDateTime::fromString(
                        QString::fromStdString(value->asString()), Qt::ISODate).toMSecsSinceEpoch();
                }
                if (record.url.isEmpty()) continue;
                snapshot.history.push_back(std::move(record));
            }
        }
    } catch (const std::exception &) {
        // A library that cannot be read yields an empty archive rather than
        // taking the whole sync down with it.
    }

    snapshot.sanitize();
    return snapshot;
}

QString SpikeWindow::applySyncSnapshot(const SyncSnapshot &snapshot, SyncTabResolution resolution) {
    if (const QString problem = snapshot.validationProblem(); !problem.isEmpty()) return problem;

    for (const QString &name : snapshot.spaces) {
        if (spaces_.contains(name)) continue;
        spaces_.append(name);
        if (spacePicker_) spacePicker_->addItem(name);
    }
    for (auto entry = snapshot.spaceIcons.begin(); entry != snapshot.spaceIcons.end(); ++entry)
        if (spaces_.contains(entry.key()) && !spaceIcons_.contains(entry.key()))
            spaceIcons_.insert(entry.key(), entry.value());

    for (const SyncFolder &folder : snapshot.folders) {
        const bool known = std::any_of(
            workspaceFolders_.cbegin(), workspaceFolders_.cend(),
            [&folder](const WorkspaceFolder &existing) { return existing.id == folder.id; }
        );
        if (known || !spaces_.contains(folder.space)) continue;
        workspaceFolders_.push_back({
            .id = folder.id,
            .name = folder.name,
            .space = folder.space,
            .color = folder.color.isEmpty() ? defaultFolderColor() : folder.color,
        });
    }

    for (const SyncBookmark &bookmark : snapshot.bookmarks) {
        (void)library_.addBookmark(
            bookmark.title.isEmpty() ? bookmark.url.toStdString() : bookmark.title.toStdString(),
            bookmark.url.toStdString(),
            bookmark.folder.trimmed().isEmpty() ? std::string("Bookmarks")
                                                : bookmark.folder.trimmed().toStdString()
        );
    }

    core::Json::Array historyEntries;
    historyEntries.reserve(snapshot.history.size());
    for (const SyncHistoryEntry &entry : snapshot.history) {
        core::Json::Object row;
        row.emplace("url", core::Json(entry.url.toStdString()));
        row.emplace("title", core::Json(entry.title.toStdString()));
        row.emplace("visits", core::Json(static_cast<std::int64_t>(entry.visits)));
        historyEntries.emplace_back(std::move(row));
    }
    try {
        (void)library_.importHistory(historyEntries);
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }

    // The closed-tab archive is a convenience, so entries only ever get added.
    for (const SyncTab &tab : snapshot.closedTabs) {
        const bool known = std::any_of(
            closedTabs_.cbegin(), closedTabs_.cend(),
            [&tab](const ClosedTab &existing) { return existing.url == tab.url; }
        );
        if (known || tab.url.isEmpty()) continue;
        closedTabs_.push_back({
            .url = tab.url,
            .title = tab.title,
            .space = spaces_.contains(tab.space) ? tab.space : activeSpace_,
            .folder = tab.folder,
            .pinned = tab.pinned,
        });
    }
    if (closedTabs_.size() > 20) closedTabs_.erase(closedTabs_.begin(), closedTabs_.end() - 20);
    updateClosedTabAction();

    if (resolution == SyncTabResolution::adoptRemote) {
        // A deliberate difference from the WebKit build: it replaces the local
        // tab set outright. Here nothing is closed, because a wrong merge would
        // otherwise throw away open work. Tabs and notes that are not here yet
        // are opened, and the count is bounded so a large snapshot cannot bury
        // the window.
        constexpr int maximumOpened = 30;
        int opened = 0;
        for (const SyncTab &tab : snapshot.tabs) {
            if (opened >= maximumOpened) break;
            if (!spaces_.contains(tab.space)) continue;
            if (tab.note) {
                bool known = false;
                for (const NoteTab &note : notes_)
                    if (note.editor->html() == tab.noteHtml
                        && note.editor->displayTitle() == tab.title) known = true;
                if (known) continue;
                const QString priorSpace = activeSpace_;
                activeSpace_ = tab.space;
                NoteEditor *editor = newNote(tab.title, tab.noteHtml);
                activeSpace_ = priorSpace;
                if (editor) {
                    const auto workspace = workspaceTabs_.find(noteIdFor(editor).toStdString());
                    if (workspace != workspaceTabs_.end()) {
                        workspace->second.folder = tab.folder;
                        workspace->second.pinned = tab.pinned;
                    }
                }
                ++opened;
                continue;
            }
            bool known = false;
            for (engine::BrowserPage *generic : session_.userPages()) {
                auto *page = asQtPage(generic);
                const auto state = page->state();
                const auto workspace = workspaceTabs_.find(state.id);
                if (workspace == workspaceTabs_.end()) continue;
                if (QString::fromStdString(state.url) == tab.url && workspace->second.space == tab.space)
                    known = true;
            }
            if (known) continue;
            auto *page = newUserTab(tab.url);
            if (!page) continue;
            workspaceTabs_[page->state().id] = WorkspaceTab{
                .space = tab.space,
                .folder = tab.folder,
                .pinned = tab.pinned,
                .order = nextWorkspaceOrder_++,
            };
            ++opened;
        }
        if (spaces_.contains(snapshot.currentSpace)) switchSpace(snapshot.currentSpace);
    }

    refreshWorkspaceSidebar();
    refreshLibrary();
    saveSession();
    return {};
}

} // namespace yobro::spike
