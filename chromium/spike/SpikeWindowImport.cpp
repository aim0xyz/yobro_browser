#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::showWebKitImport() {
    if (importDialog_) {
        importDialog_->show();
        importDialog_->raise();
        importDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("webkitImportDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("YOBRO-Daten importieren")));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(640, 410);
    auto *layout = new QVBoxLayout(dialog);
    auto *title = new QLabel(L(QStringLiteral("Deine YOBRO-Daten ziehen mit um")), dialog);
    title->setStyleSheet(QStringLiteral("font-size:22px;font-weight:700"));
    layout->addWidget(title);
    auto *detail = new QLabel(
        QStringLiteral("Wähle den Ordner eines bestehenden YOBRO-WebKit-Profils. Die Vorschau liest nur "
                       "history.json, bookmarks.json und session.json. Erst nach deiner Bestätigung wird "
                       "eine gefilterte Kopie in dieses getrennte Chromium-Profil geschrieben."), dialog
    );
    detail->setWordWrap(true);
    layout->addWidget(detail);
    auto *sourceRow = new QHBoxLayout();
    importSource_ = new QLineEdit(dialog);
    importSource_->setObjectName(QStringLiteral("webkitImportSource"));
    importSource_->setReadOnly(true);
    const auto suggested = core::ProfilePaths::webKitRoot();
    importSource_->setText(QString::fromStdString(suggested.string()));
    auto *choose = new QPushButton(L(QStringLiteral("Ordner wählen …")), dialog);
    choose->setObjectName(QStringLiteral("webkitImportChooseSource"));
    sourceRow->addWidget(importSource_, 1);
    sourceRow->addWidget(choose);
    layout->addLayout(sourceRow);
    auto *preview = new QPushButton(L(QStringLiteral("Vorschau aktualisieren")), dialog);
    preview->setObjectName(QStringLiteral("webkitImportPreview"));
    layout->addWidget(preview);
    importPreview_ = new QLabel(L(QStringLiteral("Noch keine Vorschau erstellt.")), dialog);
    importPreview_->setObjectName(QStringLiteral("webkitImportPreviewSummary"));
    importPreview_->setWordWrap(true);
    importPreview_->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;border-radius:8px;padding:12px").arg(themePalette(systemPrefersDark()).sheetSurface, themePalette(systemPrefersDark()).sheetBorder));
    layout->addWidget(importPreview_);
    auto *browserTitle = new QLabel(
        L(QStringLiteral("AUS ANDEREM BROWSER"), QStringLiteral("FROM ANOTHER BROWSER")), dialog);
    browserTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(browserTitle);
    auto *browserRow = new QHBoxLayout();
    browserImportPicker_ = new QComboBox(dialog);
    browserImportPicker_->setObjectName(QStringLiteral("browserImportProfilePicker"));
    auto *findProfiles = new QPushButton(
        L(QStringLiteral("Profile suchen"), QStringLiteral("Find profiles")), dialog);
    findProfiles->setObjectName(QStringLiteral("browserImportFindButton"));
    browserRow->addWidget(browserImportPicker_, 1);
    browserRow->addWidget(findProfiles);
    layout->addLayout(browserRow);
    auto *kindRow = new QHBoxLayout();
    for (const QString &kind : BrowserDataImport::kinds()) {
        auto *box = new QCheckBox(BrowserDataImport::kindLabel(kind), dialog);
        box->setObjectName(QStringLiteral("browserImportKind-") + kind);
        // Cookies stay off by default: they carry live sessions and the source
        // browser may sign out when they are reused elsewhere.
        box->setChecked(kind != QStringLiteral("cookies"));
        browserImportKinds_.insert(kind, box);
        kindRow->addWidget(box);
    }
    kindRow->addStretch();
    layout->addLayout(kindRow);
    auto *browserActions = new QHBoxLayout();
    auto *browserPreview = new QPushButton(
        L(QStringLiteral("Vorschau lesen"), QStringLiteral("Read preview")), dialog);
    browserPreview->setObjectName(QStringLiteral("browserImportPreviewButton"));
    browserImportCommit_ = new QPushButton(
        L(QStringLiteral("Daten übernehmen"), QStringLiteral("Transfer data")), dialog);
    browserImportCommit_->setObjectName(QStringLiteral("browserImportCommitButton"));
    browserImportCommit_->setEnabled(false);
    browserActions->addWidget(browserPreview);
    browserActions->addWidget(browserImportCommit_);
    browserActions->addStretch();
    layout->addLayout(browserActions);
    browserImportPreview_ = new QLabel(
        L(QStringLiteral("Noch keine Vorschau gelesen."), QStringLiteral("No preview read yet.")), dialog);
    browserImportPreview_->setObjectName(QStringLiteral("browserImportPreviewSummary"));
    browserImportPreview_->setWordWrap(true);
    browserImportPreview_->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(browserImportPreview_);
    QObject::connect(findProfiles, &QPushButton::clicked, dialog, [this] { refreshBrowserImportProfiles(); });
    QObject::connect(browserPreview, &QPushButton::clicked, dialog, [this] { previewBrowserImport(); });
    QObject::connect(browserImportCommit_, &QPushButton::clicked, dialog, [this] { commitBrowserImport(); });
    QObject::connect(browserImportPicker_, &QComboBox::currentIndexChanged, dialog, [this](int) {
        browserImportData_.reset();
        if (browserImportCommit_) browserImportCommit_->setEnabled(false);
        if (browserImportPreview_)
            browserImportPreview_->setText(L(
                QStringLiteral("Profil geändert. Vorschau erneut lesen."),
                QStringLiteral("Profile changed. Read the preview again.")
            ));
    });

    auto *safety = new QLabel(
        QStringLiteral("Sicherheit: file:, data:, javascript: und ungültige URLs werden ausgelassen. "
                       "Das Quellprofil wird während der Vorschau und vor dem Commit erneut auf Änderungen geprüft."), dialog
    );
    safety->setWordWrap(true);
    safety->setStyleSheet(QStringLiteral("color:%1").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(safety);
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    commitImport_ = buttons->addButton(L(QStringLiteral("Auswahl importieren")), QDialogButtonBox::AcceptRole);
    commitImport_->setObjectName(QStringLiteral("webkitImportCommit"));
    commitImport_->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(L(QStringLiteral("Abbrechen")));
    layout->addWidget(buttons);
    importDialog_ = dialog;
    QObject::connect(choose, &QPushButton::clicked, dialog, [this] {
        const QString source = QFileDialog::getExistingDirectory(
            this, L(QStringLiteral("Bestehendes YOBRO-WebKit-Profil wählen")), importSource_ ? importSource_->text() : QString()
        );
        if (source.isEmpty()) return;
        importSource_->setText(source);
        importSourcePath_.clear();
        importPlan_.reset();
        if (commitImport_) commitImport_->setEnabled(false);
        if (importPreview_) importPreview_->setText(L(QStringLiteral("Quelle geändert. Vorschau aktualisieren.")));
    });
    QObject::connect(preview, &QPushButton::clicked, dialog, [this] { refreshWebKitImportPreview(); });
    QObject::connect(commitImport_, &QPushButton::clicked, dialog, [this] { commitWebKitImport(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        importDialog_ = nullptr;
        importSource_ = nullptr;
        importPreview_ = nullptr;
        commitImport_ = nullptr;
        importSourcePath_.clear();
        importPlan_.reset();
        browserImportPicker_ = nullptr;
        browserImportPreview_ = nullptr;
        browserImportCommit_ = nullptr;
        browserImportKinds_.clear();
        browserImportProfiles_.clear();
        browserImportData_.reset();
    });
    refreshBrowserImportProfiles();
    dialog->show();
}

void SpikeWindow::refreshBrowserImportProfiles() {
    if (!browserImportPicker_) return;
    QString problem;
    browserImportProfiles_ = BrowserDataImport::profiles({}, problem);
    const QSignalBlocker blocker(browserImportPicker_);
    browserImportPicker_->clear();
    for (const ImportProfileEntry &entry : browserImportProfiles_) {
        browserImportPicker_->addItem(
            QStringLiteral("%1 · %2").arg(entry.browser, entry.name.isEmpty() ? entry.path : entry.name)
        );
    }
    if (!problem.isEmpty()) {
        if (browserImportPreview_) browserImportPreview_->setText(problem);
        return;
    }
    if (browserImportProfiles_.empty() && browserImportPreview_) {
        browserImportPreview_->setText(L(
            QStringLiteral("Keine Profile anderer Browser gefunden."),
            QStringLiteral("No profiles of other browsers were found.")
        ));
    }
}

void SpikeWindow::previewBrowserImport() {
    if (!browserImportPicker_ || !browserImportPreview_) return;
    const int index = browserImportPicker_->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= browserImportProfiles_.size()) {
        browserImportPreview_->setText(L(
            QStringLiteral("Bitte zuerst ein Profil suchen und auswählen."),
            QStringLiteral("Please find and select a profile first.")
        ));
        return;
    }
    QStringList wanted;
    for (auto entry = browserImportKinds_.begin(); entry != browserImportKinds_.end(); ++entry) {
        if (entry.value() && entry.value()->isChecked()) wanted.append(entry.key());
    }

    const ImportProfileEntry &profile = browserImportProfiles_.at(static_cast<std::size_t>(index));
    ImportedBrowserData data = BrowserDataImport::readProfile(profile.browser, profile.path, wanted);
    if (!data.problem.isEmpty()) {
        browserImportData_.reset();
        if (browserImportCommit_) browserImportCommit_->setEnabled(false);
        browserImportPreview_->setText(data.problem);
        return;
    }

    QStringList lines;
    for (const QString &kind : BrowserDataImport::kinds()) {
        if (!wanted.contains(kind)) continue;
        const std::size_t count = kind == QStringLiteral("bookmarks") ? data.bookmarks.size()
            : kind == QStringLiteral("history") ? data.history.size()
            : kind == QStringLiteral("tabs") ? data.tabs.size()
            : data.cookies.size();
        QString line = QStringLiteral("%1: %2").arg(BrowserDataImport::kindLabel(kind)).arg(count);
        if (const QString note = data.availability.value(kind); !note.isEmpty())
            line += QStringLiteral(" · ") + note;
        lines.append(line);
    }
    lines.append(data.warnings);
    lines.append(L(
        QStringLiteral("Die Quelle bleibt unverändert. Erst „Daten übernehmen“ schreibt in dieses Profil."),
        QStringLiteral("The source stays unchanged. Only “Transfer data” writes into this profile.")
    ));
    browserImportPreview_->setText(lines.join(QStringLiteral("\n")));
    const bool anything = data.total() > 0;
    browserImportData_ = std::move(data);
    if (browserImportCommit_) browserImportCommit_->setEnabled(anything);
}

void SpikeWindow::commitBrowserImport() {
    if (!browserImportData_ || !importDialog_) return;
    const ImportedBrowserData &data = *browserImportData_;

    const auto decision = QMessageBox::question(
        importDialog_,
        L(QStringLiteral("Daten übernehmen?"), QStringLiteral("Transfer data?")),
        L(QStringLiteral("%1 Lesezeichen, %2 Verlaufseinträge, %3 Tabs und %4 Cookies in dieses Profil "
                         "übernehmen? Tabs öffnen sich sofort. Cookies können dich im Quellbrowser abmelden."),
          QStringLiteral("Transfer %1 bookmarks, %2 history entries, %3 tabs and %4 cookies into this "
                         "profile? Tabs open right away. Cookies may sign you out in the source browser."))
            .arg(data.bookmarks.size())
            .arg(data.history.size())
            .arg(data.tabs.size())
            .arg(data.cookies.size()),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel
    );
    if (decision != QMessageBox::Yes) return;

    QString problem;
    const QStringList summary = applyImportedBrowserData(data, &problem);
    if (!problem.isEmpty()) {
        status_->setText(problem);
        return;
    }
    status_->setText(summary.join(QStringLiteral(" ")));
    if (browserImportPreview_) browserImportPreview_->setText(summary.join(QStringLiteral("\n")));
    browserImportData_.reset();
    if (browserImportCommit_) browserImportCommit_->setEnabled(false);
}

QStringList SpikeWindow::applyImportedBrowserData(const ImportedBrowserData &data, QString *problem) {
    if (problem) problem->clear();
    std::size_t addedBookmarks = 0;
    for (const ImportedLink &link : data.bookmarks) {
        const std::string folder = link.folder.trimmed().isEmpty()
            ? std::string("Imported")
            : link.folder.trimmed().toStdString();
        if (library_.addBookmark(
                link.title.isEmpty() ? link.url.toStdString() : link.title.toStdString(),
                link.url.toStdString(),
                folder
            )) ++addedBookmarks;
    }

    core::Json::Array historyEntries;
    historyEntries.reserve(data.history.size());
    for (const ImportedLink &link : data.history) {
        core::Json::Object entry;
        entry.emplace("url", core::Json(link.url.toStdString()));
        entry.emplace("title", core::Json(link.title.toStdString()));
        entry.emplace("visits", core::Json(static_cast<std::int64_t>(link.visits)));
        historyEntries.emplace_back(std::move(entry));
    }
    std::size_t addedHistory = 0;
    try {
        addedHistory = library_.importHistory(historyEntries);
    } catch (const std::exception &error) {
        if (problem) *problem = QString::fromUtf8(error.what());
        return {};
    }

    // Opening hundreds of tabs at once would be unusable, so this is bounded and
    // the rest is kept as bookmarks instead of being dropped silently.
    constexpr std::size_t maxOpenedTabs = 30;
    std::size_t openedTabs = 0;
    std::size_t tabsAsBookmarks = 0;
    for (const ImportedLink &tab : data.tabs) {
        if (openedTabs < maxOpenedTabs) {
            newUserTab(tab.url);
            ++openedTabs;
            continue;
        }
        if (library_.addBookmark(
                tab.title.isEmpty() ? tab.url.toStdString() : tab.title.toStdString(),
                tab.url.toStdString(),
                "Imported Tabs"
            )) ++tabsAsBookmarks;
    }

    std::size_t addedCookies = 0;
    if (!data.cookies.empty()) {
        auto *store = profile_->persistentProfile()->cookieStore();
        for (const ImportedCookie &cookie : data.cookies) {
            QNetworkCookie value(cookie.name.toUtf8(), cookie.value.toUtf8());
            value.setDomain(cookie.domain);
            value.setPath(cookie.path.isEmpty() ? QStringLiteral("/") : cookie.path);
            value.setSecure(cookie.secure);
            value.setHttpOnly(cookie.httpOnly);
            if (cookie.expires > 0)
                value.setExpirationDate(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(cookie.expires)));
            // The origin decides which store the cookie lands in; a leading dot
            // in the domain marks a host-spanning cookie and is not part of it.
            QString host = cookie.domain;
            if (host.startsWith(QLatin1Char('.'))) host = host.mid(1);
            const QUrl origin(
                (cookie.secure ? QStringLiteral("https://") : QStringLiteral("http://")) + host
            );
            if (!origin.isValid() || origin.host().isEmpty()) continue;
            store->setCookie(value, origin);
            ++addedCookies;
        }
    }

    saveSession();
    refreshLibrary();
    QStringList summary;
    summary.append(L(
        QStringLiteral("Übernommen: %1 Lesezeichen, %2 Verlaufseinträge, %3 Tabs geöffnet, %4 Cookies gesetzt."),
        QStringLiteral("Transferred: %1 bookmarks, %2 history entries, %3 tabs opened, %4 cookies set.")
    ).arg(addedBookmarks).arg(addedHistory).arg(openedTabs).arg(addedCookies));
    if (tabsAsBookmarks > 0) {
        summary.append(L(
            QStringLiteral("%1 weitere Tabs liegen als Lesezeichen im Ordner „Imported Tabs“."),
            QStringLiteral("%1 further tabs are bookmarks in the “Imported Tabs” folder.")
        ).arg(tabsAsBookmarks));
    }
    return summary;
}

void SpikeWindow::refreshWebKitImportPreview() {
    if (!importSource_ || !importPreview_) return;
    const QString source = importSource_->text().trimmed();
    if (source.isEmpty()) return;
    try {
        importSourcePath_ = std::filesystem::path(source.toStdString());
        importPlan_ = core::WebKitImport::preview(importSourcePath_);
        const auto &plan = *importPlan_;
        importPreview_->setText(QStringLiteral("Vorschau: %1 Verlaufseinträge · %2 Lesezeichen · %3 offene Tabs\n"
                                               "%4 unsichere oder ungültige Einträge werden nicht übernommen.\n"
                                               "Die Quelle bleibt unverändert; erst ‚Auswahl importieren‘ schreibt in dieses Chromium-Profil.")
            .arg(plan.history).arg(plan.bookmarks).arg(plan.tabs).arg(plan.rejected));
        commitImport_->setEnabled(plan.history + plan.bookmarks + plan.tabs > 0);
    } catch (const std::exception &error) {
        importSourcePath_.clear();
        importPlan_.reset();
        importPreview_->setText(QStringLiteral("Vorschau fehlgeschlagen: ") + QString::fromUtf8(error.what()));
        commitImport_->setEnabled(false);
    }
}

void SpikeWindow::commitWebKitImport() {
    if (!importPlan_ || importSourcePath_.empty() || !importDialog_) return;
    const auto &plan = *importPlan_;
    const auto decision = QMessageBox::question(
        importDialog_,
        L(QStringLiteral("Import bestätigen")),
        QStringLiteral("%1 Verlaufseinträge, %2 Lesezeichen und %3 Tabs in dieses getrennte Chromium-Profil importieren?\n\n"
                       "Die WebKit-Quelle wird nicht verändert. Tabs öffnen sich jetzt und werden beim Neustart wiederhergestellt.")
            .arg(plan.history).arg(plan.bookmarks).arg(plan.tabs),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel
    );
    if (decision != QMessageBox::Yes) return;
    try {
        const auto target = paths_.profile / "WebKit Import";
        (void)core::WebKitImport::commit(importSourcePath_, target);
        const auto readJson = [](const std::filesystem::path &path) {
            QFile input(QString::fromStdString(path.string()));
            if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read committed WebKit import.");
            const QByteArray bytes = input.readAll();
            return core::Json::parse(
                std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                {.maxBytes = 16 * 1024 * 1024, .maxDepth = 48, .rejectDuplicateKeys = true}
            );
        };
        const core::Json history = readJson(target / "history.json");
        const std::size_t addedHistory = history.isArray() ? library_.importHistory(history.asArray()) : 0;
        const core::Json bookmarks = readJson(target / "bookmarks.json");
        std::size_t addedBookmarks = 0;
        if (bookmarks.isArray()) {
            for (const core::Json &entry : bookmarks.asArray()) {
                const core::Json *url = entry.find("url");
                if (!url || !url->isString()) continue;
                const core::Json *title = entry.find("title");
                const core::Json *folder = entry.find("folder");
                if (library_.addBookmark(
                        title && title->isString() ? title->asString() : url->asString(),
                        url->asString(),
                        folder && folder->isString() ? folder->asString() : "Imported"
                    )) ++addedBookmarks;
            }
        }
        const core::Json session = readJson(target / "session.json");
        std::size_t openedTabs = 0;
        if (const core::Json *tabs = session.find("tabs"); tabs && tabs->isArray()) {
            for (const core::Json &entry : tabs->asArray()) {
                const core::Json *url = entry.find("url");
                if (!url || !url->isString()) continue;
                const QString address = QString::fromStdString(url->asString());
                if (!address.startsWith(QStringLiteral("https://")) && !address.startsWith(QStringLiteral("http://"))) continue;
                newUserTab(address);
                ++openedTabs;
            }
        }
        saveSession();
        status_->setText(L(QStringLiteral("Import abgeschlossen: %1 neue Verlaufseinträge, %2 Lesezeichen, %3 Tabs geöffnet."))
            .arg(addedHistory).arg(addedBookmarks).arg(openedTabs));
        importDialog_->accept();
    } catch (const std::exception &error) {
        if (importPreview_) importPreview_->setText(QStringLiteral("Import fehlgeschlagen: ") + QString::fromUtf8(error.what()));
        if (commitImport_) commitImport_->setEnabled(false);
        status_->setText(L(QStringLiteral("WebKit-Import fehlgeschlagen.")));
    }
}

} // namespace yobro::spike
