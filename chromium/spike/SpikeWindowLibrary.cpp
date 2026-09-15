#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::showLibrary(LibrarySection section) {
    if (libraryDialog_) {
        refreshLibrary();
        if (libraryEntries_)
            libraryEntries_->setCurrentIndex(section == LibrarySection::bookmarks ? 1 : 0);
        libraryDialog_->show();
        libraryDialog_->raise();
        libraryDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("browserLibrarySheet"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Deine Bibliothek")));
    dialog->resize(680, 570);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#libraryEntries{background:transparent;border:0;}"
        "#historyLibraryList::item,#bookmarksLibraryList::item{border-radius:14px;margin:4px 0;padding:14px;}"
    ));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(25, 25, 25, 20);
    layout->setSpacing(14);
    auto *header = new QHBoxLayout();
    auto *mark = new QLabel(QStringLiteral("◉"), dialog);
    mark->setStyleSheet(QStringLiteral("font-size:26px;color:%1").arg(themePalette(currentAppearanceIsDark()).brandOrange));
    auto *title = new QLabel(L(QStringLiteral("Deine Bibliothek")), dialog);
    title->setStyleSheet(QStringLiteral("font:500 28px 'New York',Georgia,serif;color:%1").arg(themePalette(currentAppearanceIsDark()).sheetText));
    auto *close = new QPushButton(QStringLiteral("×"), dialog);
    close->setObjectName(QStringLiteral("closeLibraryButton"));
    header->addWidget(mark);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(close);
    layout->addLayout(header);
    auto *segment = new QWidget(dialog);
    segment->setObjectName(QStringLiteral("librarySegment"));
    auto *segmentLayout = new QHBoxLayout(segment);
    segmentLayout->setContentsMargins(4, 4, 4, 4);
    segmentLayout->setSpacing(4);
    auto *historySegment = new QPushButton(L(QStringLiteral("◷  Verlauf")), segment);
    historySegment->setObjectName(QStringLiteral("libraryHistorySegment"));
    historySegment->setStyleSheet(QStringLiteral("background:%1;border-radius:9px").arg(themePalette(currentAppearanceIsDark()).sheetSurface));
    auto *bookmarksSegment = new QPushButton(L(QStringLiteral("⌑  Lesezeichen")), segment);
    bookmarksSegment->setObjectName(QStringLiteral("libraryBookmarksSegment"));
    auto *downloadsSegment = new QPushButton(L(QStringLiteral("⇩  Downloads")), segment);
    downloadsSegment->setObjectName(QStringLiteral("libraryDownloadsSegment"));
    segmentLayout->addWidget(historySegment, 1);
    segmentLayout->addWidget(bookmarksSegment, 1);
    segmentLayout->addWidget(downloadsSegment, 1);
    layout->addWidget(segment);
    librarySearch_ = new QLineEdit(dialog);
    librarySearch_->setObjectName(QStringLiteral("librarySearch"));
    librarySearch_->setPlaceholderText(L(QStringLiteral("Verlauf und Lesezeichen durchsuchen")));
    layout->addWidget(librarySearch_);
    auto *entries = new QTabWidget(dialog);
    libraryEntries_ = entries;
    entries->setObjectName(QStringLiteral("libraryEntries"));
    entries->tabBar()->hide();
    historyList_ = new QListWidget(entries);
    historyList_->setObjectName(QStringLiteral("historyLibraryList"));
    bookmarksList_ = new QListWidget(entries);
    bookmarksList_->setObjectName(QStringLiteral("bookmarksLibraryList"));
    entries->addTab(historyList_, L(QStringLiteral("Verlauf")));
    entries->addTab(bookmarksList_, L(QStringLiteral("Lesezeichen")));
    layout->addWidget(entries, 1);
    // Folder filter and sort order apply to the bookmark list.
    auto *bookmarkTools = new QHBoxLayout();
    bookmarkFolderFilter_ = new QComboBox(dialog);
    bookmarkFolderFilter_->setObjectName(QStringLiteral("bookmarkFolderFilter"));
    bookmarkSort_ = new QComboBox(dialog);
    bookmarkSort_->setObjectName(QStringLiteral("bookmarkSort"));
    bookmarkSort_->addItem(L(QStringLiteral("Zuletzt hinzugefügt"), QStringLiteral("Recently added")));
    bookmarkSort_->addItem(L(QStringLiteral("Name A–Z"), QStringLiteral("Name A–Z")));
    bookmarkSort_->addItem(L(QStringLiteral("Adresse A–Z"), QStringLiteral("Address A–Z")));
    bookmarkTools->addWidget(bookmarkFolderFilter_, 1);
    bookmarkTools->addWidget(bookmarkSort_, 1);
    layout->addLayout(bookmarkTools);

    auto *actions = new QHBoxLayout();
    auto *open = new QPushButton(L(QStringLiteral("In neuem Tab öffnen")), dialog);
    open->setObjectName(QStringLiteral("openLibraryEntryButton"));
    auto *renameBookmarkButton = new QPushButton(
        L(QStringLiteral("Umbenennen…"), QStringLiteral("Rename…")), dialog);
    renameBookmarkButton->setObjectName(QStringLiteral("renameBookmarkButton"));
    auto *moveBookmarkButton = new QPushButton(
        L(QStringLiteral("In Ordner verschieben…"), QStringLiteral("Move to folder…")), dialog);
    moveBookmarkButton->setObjectName(QStringLiteral("moveBookmarkButton"));
    auto *remove = new QPushButton(L(QStringLiteral("Lesezeichen entfernen")), dialog);
    remove->setObjectName(QStringLiteral("removeBookmarkButton"));
    actions->addStretch();
    actions->addWidget(open);
    actions->addWidget(renameBookmarkButton);
    actions->addWidget(moveBookmarkButton);
    actions->addWidget(remove);
    layout->addLayout(actions);
    QObject::connect(renameBookmarkButton, &QPushButton::clicked, dialog, [this] { renameSelectedBookmark(); });
    QObject::connect(moveBookmarkButton, &QPushButton::clicked, dialog, [this] { moveSelectedBookmark(); });
    QObject::connect(bookmarkFolderFilter_, &QComboBox::currentIndexChanged, dialog, [this] { refreshLibrary(); });
    QObject::connect(bookmarkSort_, &QComboBox::currentIndexChanged, dialog, [this] { refreshLibrary(); });
    libraryDialog_ = dialog;
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(historySegment, &QPushButton::clicked, dialog, [entries, historySegment, bookmarksSegment] {
        entries->setCurrentIndex(0);
        historySegment->setStyleSheet(QStringLiteral("background:%1;border-radius:9px").arg(themePalette(currentAppearanceIsDark()).sheetSurface));
        bookmarksSegment->setStyleSheet({});
    });
    QObject::connect(bookmarksSegment, &QPushButton::clicked, dialog, [entries, historySegment, bookmarksSegment] {
        entries->setCurrentIndex(1);
        bookmarksSegment->setStyleSheet(QStringLiteral("background:%1;border-radius:9px").arg(themePalette(currentAppearanceIsDark()).sheetSurface));
        historySegment->setStyleSheet({});
    });
    QObject::connect(downloadsSegment, &QPushButton::clicked, dialog, [this, dialog] {
        dialog->close();
        showDownloads();
    });
    const auto selectedUrl = [this] {
        QListWidgetItem *item = historyList_ && historyList_->currentItem() ? historyList_->currentItem()
            : bookmarksList_ ? bookmarksList_->currentItem() : nullptr;
        return item ? item->data(Qt::UserRole + 1).toString() : QString();
    };
    QObject::connect(librarySearch_, &QLineEdit::textChanged, dialog, [this] { refreshLibrary(); });
    QObject::connect(historyList_, &QListWidget::itemSelectionChanged, dialog, [this] {
        if (historyList_->currentItem() && bookmarksList_) bookmarksList_->clearSelection();
    });
    QObject::connect(bookmarksList_, &QListWidget::itemSelectionChanged, dialog, [this] {
        if (bookmarksList_->currentItem() && historyList_) historyList_->clearSelection();
    });
    QObject::connect(open, &QPushButton::clicked, dialog, [this, selectedUrl] {
        const QString url = selectedUrl();
        if (!url.isEmpty()) newUserTab(url);
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [this] {
        if (!bookmarksList_ || !bookmarksList_->currentItem()) return;
        try {
            (void)library_.removeBookmark(bookmarksList_->currentItem()->data(Qt::UserRole).toString().toStdString());
            refreshLibrary();
        } catch (const std::exception &error) {
            showStatus(QString::fromUtf8(error.what()));
        }
    });
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        libraryDialog_ = nullptr;
        librarySearch_ = nullptr;
        historyList_ = nullptr;
        bookmarksList_ = nullptr;
        libraryEntries_ = nullptr;
        bookmarkFolderFilter_ = nullptr;
        bookmarkSort_ = nullptr;
    });
    refreshLibrary();
    if (section == LibrarySection::bookmarks) {
        entries->setCurrentIndex(1);
        bookmarksSegment->setStyleSheet(QStringLiteral("background:%1;border-radius:9px").arg(themePalette(currentAppearanceIsDark()).sheetSurface));
        historySegment->setStyleSheet({});
    }
    dialog->show();
}

void SpikeWindow::refreshLibrary() {
    if (!historyList_ || !bookmarksList_) return;
    const std::string query = librarySearch_ ? librarySearch_->text().toStdString() : std::string{};
    const auto populate = [](QListWidget *list, const core::Json &entries, bool bookmarks) {
        list->clear();
        if (!entries.isArray()) return;
        for (const core::Json &entry : entries.asArray()) {
            const core::Json *id = entry.find("id");
            const core::Json *url = entry.find("url");
            if (!id || !id->isString() || !url || !url->isString()) continue;
            const core::Json *title = entry.find("title");
            const core::Json *folder = entry.find("folder");
            const QString label = QString::fromStdString(title && title->isString() ? title->asString() : url->asString())
                + QStringLiteral("\n") + QString::fromStdString(bookmarks && folder && folder->isString() ? folder->asString() + " · " + url->asString() : url->asString());
            auto *item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, QString::fromStdString(id->asString()));
            item->setData(Qt::UserRole + 1, QString::fromStdString(url->asString()));
        }
    };
    populate(historyList_, library_.history(query, 200), false);

    // Keep the chosen folder selected across refreshes.
    QString selectedFolder;
    if (bookmarkFolderFilter_) {
        selectedFolder = bookmarkFolderFilter_->currentData().toString();
        const QSignalBlocker blocker(bookmarkFolderFilter_);
        bookmarkFolderFilter_->clear();
        bookmarkFolderFilter_->addItem(L(QStringLiteral("Alle Ordner"), QStringLiteral("All folders")), QString());
        for (const std::string &folder : library_.bookmarkFolders())
            bookmarkFolderFilter_->addItem(QString::fromStdString(folder), QString::fromStdString(folder));
        const int index = bookmarkFolderFilter_->findData(selectedFolder);
        bookmarkFolderFilter_->setCurrentIndex(index < 0 ? 0 : index);
        selectedFolder = bookmarkFolderFilter_->currentData().toString();
    }

    core::Json bookmarks = library_.bookmarks(query, 500);
    core::Json::Array entries = bookmarks.isArray() ? bookmarks.asArray() : core::Json::Array{};
    if (!selectedFolder.isEmpty()) {
        core::Json::Array filtered;
        for (const core::Json &entry : entries) {
            const core::Json *folder = entry.find("folder");
            if (folder && folder->isString() && folder->asString() == selectedFolder.toStdString())
                filtered.push_back(entry);
        }
        entries = std::move(filtered);
    }
    const int order = bookmarkSort_ ? bookmarkSort_->currentIndex() : 0;
    if (order != 0) {
        const char *field = order == 1 ? "title" : "url";
        std::stable_sort(entries.begin(), entries.end(), [field](const core::Json &left, const core::Json &right) {
            const core::Json *a = left.find(field);
            const core::Json *b = right.find(field);
            const QString first = a && a->isString() ? QString::fromStdString(a->asString()) : QString();
            const QString second = b && b->isString() ? QString::fromStdString(b->asString()) : QString();
            return first.compare(second, Qt::CaseInsensitive) < 0;
        });
    }
    populate(bookmarksList_, core::Json(std::move(entries)), true);
}

void SpikeWindow::renameSelectedBookmark() {
    if (!bookmarksList_ || !bookmarksList_->currentItem()) return;
    QListWidgetItem *item = bookmarksList_->currentItem();
    const QString id = item->data(Qt::UserRole).toString();
    // The list shows the title on the first line.
    const QString current = item->text().section(QLatin1Char('\n'), 0, 0);
    bool accepted = false;
    const QString title = QInputDialog::getText(
        libraryDialog_ ? static_cast<QWidget *>(libraryDialog_) : this,
        L(QStringLiteral("Lesezeichen umbenennen"), QStringLiteral("Rename bookmark")),
        L(QStringLiteral("Name"), QStringLiteral("Name")),
        QLineEdit::Normal,
        current,
        &accepted
    ).trimmed();
    if (!accepted || title.isEmpty()) return;
    try {
        if (!library_.renameBookmark(id.toStdString(), title.toStdString())) {
            showStatus(L(QStringLiteral("Das Lesezeichen konnte nicht umbenannt werden."),
                               QStringLiteral("The bookmark could not be renamed.")));
            return;
        }
        refreshLibrary();
    } catch (const std::exception &error) {
        showStatus(QString::fromUtf8(error.what()));
    }
}

void SpikeWindow::moveSelectedBookmark() {
    if (!bookmarksList_ || !bookmarksList_->currentItem()) return;
    const QString id = bookmarksList_->currentItem()->data(Qt::UserRole).toString();
    QStringList folders;
    for (const std::string &folder : library_.bookmarkFolders())
        folders.append(QString::fromStdString(folder));
    if (folders.isEmpty()) folders.append(QStringLiteral("Bookmarks"));
    bool accepted = false;
    // Editable so a new folder can be created right here.
    const QString folder = QInputDialog::getItem(
        libraryDialog_ ? static_cast<QWidget *>(libraryDialog_) : this,
        L(QStringLiteral("In Ordner verschieben"), QStringLiteral("Move to folder")),
        L(QStringLiteral("Ordner"), QStringLiteral("Folder")),
        folders,
        0,
        true,
        &accepted
    ).trimmed();
    if (!accepted || folder.isEmpty()) return;
    try {
        if (!library_.moveBookmark(id.toStdString(), folder.toStdString())) {
            showStatus(L(QStringLiteral("In diesem Ordner ist die Adresse bereits gespeichert."),
                               QStringLiteral("That folder already holds this address.")));
            return;
        }
        refreshLibrary();
    } catch (const std::exception &error) {
        showStatus(QString::fromUtf8(error.what()));
    }
}

void SpikeWindow::bookmarkCurrentPage() {
    auto *page = currentUserPage();
    if (!page || page->profile()->isOffTheRecord()) return;
    const auto state = page->state();
    try {
        const bool added = library_.addBookmark(state.title, state.url, "Bookmarks");
        showStatus(added ? L(QStringLiteral("Lesezeichen gespeichert.")) : L(QStringLiteral("Diese Seite ist bereits als Lesezeichen gespeichert.")));
        refreshLibrary();
    } catch (const std::exception &error) {
        showStatus(QString::fromUtf8(error.what()));
    }
}

} // namespace yobro::spike
