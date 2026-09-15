#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::showDownloads() {
    if (downloadsDialog_) {
        refreshDownloads();
        downloadsDialog_->show();
        downloadsDialog_->raise();
        downloadsDialog_->activateWindow();
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("downloadLibraryDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Deine Bibliothek")));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(680, 570);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#downloadsList::item{border-radius:14px;margin:5px 0;padding:18px;}"
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
    close->setToolTip(L(QStringLiteral("Bibliothek schließen")));
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
    auto *downloadsSegment = new QPushButton(L(QStringLiteral("⇩  Downloads")), segment);
    downloadsSegment->setObjectName(QStringLiteral("libraryDownloadsSegment"));
    downloadsSegment->setStyleSheet(QStringLiteral("background:%1;border-radius:9px").arg(themePalette(currentAppearanceIsDark()).sheetSurface));
    segmentLayout->addWidget(historySegment, 1);
    segmentLayout->addWidget(downloadsSegment, 1);
    layout->addWidget(segment);
    auto *section = new QLabel(QStringLiteral("Downloads"), dialog);
    section->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:700;letter-spacing:1px").arg(themePalette(currentAppearanceIsDark()).sheetMuted));
    layout->addWidget(section);

    downloadsList_ = new QListWidget(dialog);
    downloadsList_->setObjectName(QStringLiteral("downloadsList"));
    downloadsList_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(downloadsList_, 1);
    downloadsEmptyTitle_ = new QLabel(L(QStringLiteral("Alles an einem Ort")), dialog);
    downloadsEmptyTitle_->setAlignment(Qt::AlignCenter);
    downloadsEmptyTitle_->setStyleSheet(QStringLiteral("font-size:18px;font-weight:700"));
    layout->addWidget(downloadsEmptyTitle_);
    downloadsEmptyDetail_ = new QLabel(
        L(QStringLiteral("Deine Downloads landen im Ordner Downloads/YOBRO.")),
        dialog
    );
    downloadsEmptyDetail_->setAlignment(Qt::AlignCenter);
    downloadsEmptyDetail_->setWordWrap(true);
    downloadsEmptyDetail_->setStyleSheet(QStringLiteral("color:%1").arg(themePalette(currentAppearanceIsDark()).sheetMuted));
    layout->addWidget(downloadsEmptyDetail_);

    auto *actions = new QHBoxLayout();
    downloadsFooter_ = new QLabel(dialog);
    cancelDownload_ = new QPushButton(L(QStringLiteral("Abbrechen")), dialog);
    cancelDownload_->setObjectName(QStringLiteral("cancelDownloadButton"));
    cancelDownload_->setToolTip(L(QStringLiteral("Download abbrechen")));
    revealDownload_ = new QPushButton(L(QStringLiteral("Im Finder zeigen")), dialog);
    revealDownload_->setObjectName(QStringLiteral("revealDownloadButton"));
    revealDownload_->setToolTip(L(QStringLiteral("Im Finder zeigen")));
    auto *openDirectory = new QPushButton(L(QStringLiteral("Downloadordner öffnen")), dialog);
    openDirectory->setObjectName(QStringLiteral("openDownloadDirectoryButton"));
    actions->addWidget(downloadsFooter_);
    actions->addStretch();
    actions->addWidget(cancelDownload_);
    actions->addWidget(revealDownload_);
    actions->addWidget(openDirectory);
    layout->addLayout(actions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);

    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(historySegment, &QPushButton::clicked, dialog, [this, dialog] {
        dialog->close();
        showLibrary();
    });
    downloadsDialog_ = dialog;
    const auto updateActions = [this] {
        QListWidgetItem *item = downloadsList_ ? downloadsList_->currentItem() : nullptr;
        const QString state = item ? item->data(Qt::UserRole + 1).toString() : QString();
        const QString path = item ? item->data(Qt::UserRole + 2).toString() : QString();
        if (cancelDownload_) cancelDownload_->setEnabled(state == QStringLiteral("downloading"));
        if (revealDownload_) revealDownload_->setEnabled(state == QStringLiteral("completed") && !path.isEmpty());
    };
    QObject::connect(downloadsList_, &QListWidget::itemSelectionChanged, dialog, updateActions);
    QObject::connect(cancelDownload_, &QPushButton::clicked, dialog, [this] {
        QListWidgetItem *item = downloadsList_ ? downloadsList_->currentItem() : nullptr;
        if (!item) return;
        const std::string id = item->data(Qt::UserRole).toString().toStdString();
        (void)library_.cancelDownload(id);
        refreshDownloads();
    });
    QObject::connect(revealDownload_, &QPushButton::clicked, dialog, [this] {
        QListWidgetItem *item = downloadsList_ ? downloadsList_->currentItem() : nullptr;
        if (!item) return;
        const QString path = item->data(Qt::UserRole + 2).toString();
        if (!path.isEmpty() && fileRevealHandler_)
            (void)fileRevealHandler_(path);
    });
    QObject::connect(openDirectory, &QPushButton::clicked, dialog, [this] {
        (void)QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(library_.downloadDirectory())));
    });
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        downloadsList_ = nullptr;
        downloadsEmptyTitle_ = nullptr;
        downloadsEmptyDetail_ = nullptr;
        downloadsFooter_ = nullptr;
        cancelDownload_ = nullptr;
        revealDownload_ = nullptr;
    });

    refreshDownloads();
    updateActions();
    dialog->show();
}

void SpikeWindow::refreshDownloads() {
    if (!downloadsList_) return;
    const QString selectedId = downloadsList_->currentItem()
        ? downloadsList_->currentItem()->data(Qt::UserRole).toString()
        : QString();
    downloadsList_->clear();
    const core::Json values = library_.downloads();
    int activeCount = 0;
    if (values.isArray()) {
        for (const core::Json &entry : values.asArray()) {
            if (!entry.isObject()) continue;
            const core::Json *id = entry.find("id");
            const core::Json *name = entry.find("name");
            const core::Json *state = entry.find("state");
            if (!id || !id->isString() || !name || !name->isString() || !state || !state->isString()) continue;
            if (state->asString() == "downloading") ++activeCount;
            QString detail = downloadStateLabel(state->asString());
            const core::Json *received = entry.find("received");
            const core::Json *expected = entry.find("expected");
            if (state->asString() == "downloading" && received && received->isInteger()
                && expected && expected->isInteger() && expected->asInteger() > 0) {
                detail += QStringLiteral(" · %1 / %2 Bytes").arg(
                    received->asInteger(),
                    expected->asInteger()
                );
            }
            const core::Json *error = entry.find("error");
            if (error && error->isString() && !error->asString().empty())
                detail += QStringLiteral(" · ") + QString::fromStdString(error->asString());
            auto *item = new QListWidgetItem(
                QString::fromStdString(name->asString()) + QStringLiteral("\n") + detail,
                downloadsList_
            );
            item->setData(Qt::UserRole, QString::fromStdString(id->asString()));
            item->setData(Qt::UserRole + 1, QString::fromStdString(state->asString()));
            const core::Json *path = entry.find("path");
            item->setData(
                Qt::UserRole + 2,
                path && path->isString() ? QString::fromStdString(path->asString()) : QString()
            );
            if (selectedId == QString::fromStdString(id->asString()))
                downloadsList_->setCurrentItem(item);
        }
    }
    const bool empty = downloadsList_->count() == 0;
    downloadsList_->setVisible(!empty);
    downloadsEmptyTitle_->setVisible(empty);
    downloadsEmptyDetail_->setVisible(empty);
    downloadsFooter_->setText(QStringLiteral("%1 aktive Downloads").arg(activeCount));
    QListWidgetItem *selected = downloadsList_->currentItem();
    if (!selected && downloadsList_->count() > 0) {
        downloadsList_->setCurrentRow(0);
        selected = downloadsList_->currentItem();
    }
    const QString state = selected ? selected->data(Qt::UserRole + 1).toString() : QString();
    const QString path = selected ? selected->data(Qt::UserRole + 2).toString() : QString();
    cancelDownload_->setEnabled(state == QStringLiteral("downloading"));
    revealDownload_->setEnabled(state == QStringLiteral("completed") && !path.isEmpty());
}

} // namespace yobro::spike
