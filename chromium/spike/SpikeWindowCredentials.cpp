#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::installLoginAutofillScript() {
    QFile source(QStringLiteral(":/yobro/LoginAutofill.js"));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        showStatus(L(QStringLiteral("Login-AutoFill ist nicht verfügbar: Skriptressource fehlt.")));
        return;
    }
    // The channel client is prepended instead of injected as a second script,
    // because Qt does not promise an execution order between two scripts that
    // share an injection point, and ours needs QWebChannel to already exist.
    QFile channelClient(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (!channelClient.open(QIODevice::ReadOnly | QIODevice::Text)) {
        showStatus(L(
            QStringLiteral("Login-AutoFill ist nicht verfügbar: Kanalskript fehlt."),
            QStringLiteral("Login autofill is unavailable: the channel script is missing.")
        ));
        return;
    }
    QWebEngineScript script;
    script.setName(QStringLiteral("YOBRO LoginAutofill"));
    script.setSourceCode(
        QString::fromUtf8(channelClient.readAll()) + QStringLiteral("\n") + QString::fromUtf8(source.readAll())
    );
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    // UserWorld keeps this separate from both page scripts and the agent bridge.
    script.setWorldId(QWebEngineScript::UserWorld);
    script.setRunsOnSubFrames(false);
    // Private pages deliberately get no credential handling at all.
    profile_->persistentProfile()->scripts()->insert(script);
}

void SpikeWindow::attachLoginChannels() {
    // Every non-private user page gets its own channel and its own receiver, so
    // an event carries the page it came from without the page having to be
    // asked. Private pages get no credential handling at all, and agent pages
    // are never a place where the user types a password.
    std::set<std::string> live;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent || tab.privatePage)
            continue;
        live.insert(tab.state.id);
        if (loginChannels_.contains(tab.state.id))
            continue;
        auto *page = asQtPage(tab.page);
        if (!page->view() || !page->view()->page())
            continue;
        auto *channel = new QWebChannel(this);
        auto *receiver = new LoginAutofillChannel(channel);
        const QString pageId = QString::fromStdString(tab.state.id);
        QObject::connect(receiver, &LoginAutofillChannel::received, this, [this, pageId](const QJsonObject &event) {
            handleLoginEvent(pageId, event);
        });
        channel->registerObject(QStringLiteral("yobroLoginHost"), receiver);
        // Bound to the same isolated world as the script, so nothing the page
        // itself runs can reach the transport or the receiver.
        page->view()->page()->setWebChannel(channel, QWebEngineScript::UserWorld);
        loginChannels_.emplace(tab.state.id, channel);
    }
    for (auto entry = loginChannels_.begin(); entry != loginChannels_.end();) {
        if (live.contains(entry->first)) {
            ++entry;
            continue;
        }
        if (entry->second)
            entry->second->deleteLater();
        entry = loginChannels_.erase(entry);
    }
}
void SpikeWindow::handleLoginEvent(const QString &pageId, const QJsonObject &event) {
    auto *current = currentUserPage();
    // Only the tab the user is looking at may drive the credential surfaces.
    if (!current || QString::fromStdString(current->state().id) != pageId) return;
    if (current->profile()->isOffTheRecord()) return;
    const std::optional<std::string> origin = PasswordVault::originFor(current->state().url);
    if (!origin) return;
    // An event that was queued before a navigation must not be attributed to
    // the page that is loaded now.
    if (event.value(QStringLiteral("origin")).toString().toStdString() != *origin) return;
    const QString kind = event.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("focus")) {
        handleLoginFocus();
    } else if (kind == QStringLiteral("save")) {
        handleLoginCapture(
            event.value(QStringLiteral("username")).toString(),
            event.value(QStringLiteral("password")).toString()
        );
    }
}

void SpikeWindow::handleLoginFocus() {
    if (loginSuggestionPopup_) return;
    showLoginSuggestions(false);
}

void SpikeWindow::showLoginSuggestions(bool userRequested) {
    if (loginSuggestionPopup_) {
        loginSuggestionPopup_->deleteLater();
        loginSuggestionPopup_.clear();
    }
    auto *page = currentUserPage();
    if (!page) return;
    if (page->profile()->isOffTheRecord()) {
        if (userRequested)
            showStatus(L(QStringLiteral("In privaten Tabs werden keine Logins gespeichert oder angeboten.")));
        return;
    }
    if (!passwords_.available()) {
        if (userRequested)
            showStatus(L(QStringLiteral("Der Schlüsselbund ist auf dieser Plattform nicht verfügbar.")));
        return;
    }
    // A locked file store is not a failure, it is a state the user can change.
    if (passwords_.locked()) {
        if (userRequested) {
            showStatus(L(
                QStringLiteral("Der Passwortspeicher ist gesperrt. In den Einstellungen unter „Gespeicherte Logins“ entsperren."),
                QStringLiteral("The password store is locked. Unlock it in Settings under “Saved logins”.")
            ));
        }
        return;
    }
    const std::optional<std::string> origin = PasswordVault::originFor(page->state().url);
    if (!origin) {
        if (userRequested)
            showStatus(L(QStringLiteral("Logins werden nur auf HTTPS-Websites angeboten.")));
        return;
    }
    const std::vector<LoginCredential> entries = passwords_.entries(*origin);
    if (entries.empty()) {
        if (userRequested) {
            showStatus(QStringLiteral(
                "Für diese Website ist noch kein Login gespeichert. YOBRO fragt nach dem Anmelden."
            ));
        }
        return;
    }

    loginSuggestionOrigin_ = QString::fromStdString(*origin);
    auto *popup = new QFrame(this);
    popup->setObjectName(QStringLiteral("loginSuggestionPopup"));
    popup->setFrameShape(QFrame::NoFrame);
    // Shown without activation so it never steals focus from the page field.
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#loginSuggestionPopup{background:%1;border:1px solid %2;border-radius:13px;}"
        "QLabel{font-size:11px;}"
        "QPushButton{text-align:left;padding:9px 11px;}"
    ).arg(themePalette(systemPrefersDark()).sheetBackground, themePalette(systemPrefersDark()).sheetBorder, themePalette(systemPrefersDark()).moss, themePalette(systemPrefersDark()).paper));
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(13, 11, 13, 12);
    layout->setSpacing(6);
    layout->addWidget(new QLabel(QStringLiteral("Gespeicherte Logins · %1").arg(loginSuggestionOrigin_), popup));
    for (const LoginCredential &entry : entries) {
        auto *choice = new QPushButton(QString::fromStdString(entry.username), popup);
        choice->setObjectName(QStringLiteral("loginSuggestionEntry"));
        QObject::connect(choice, &QPushButton::clicked, popup, [this, entry, popup] {
            popup->close();
            fillLogin(entry);
        });
        layout->addWidget(choice);
    }
    auto *dismiss = new QPushButton(L(QStringLiteral("Nicht ausfüllen")), popup);
    dismiss->setObjectName(QStringLiteral("loginSuggestionDismiss"));
    QObject::connect(dismiss, &QPushButton::clicked, popup, [popup] { popup->close(); });
    layout->addWidget(dismiss);

    loginSuggestionPopup_ = popup;
    popup->adjustSize();
    const QPoint anchor = address_->mapTo(this, QPoint(address_->width(), address_->height()));
    popup->move(std::max(12, anchor.x() - popup->width()), anchor.y() + 6);
    popup->show();
    popup->raise();
}

void SpikeWindow::fillLogin(const LoginCredential &credential) {
    auto *page = currentUserPage();
    if (!page) return;
    const std::optional<std::string> origin = PasswordVault::originFor(page->state().url);
    if (!origin || *origin != credential.origin) {
        showStatus(L(QStringLiteral("Die Website hat gewechselt. Bitte erneut auswählen.")));
        return;
    }
    QPointer<SpikeWindow> guard(this);
    QWebEnginePage *native = page->view()->page();
    native->runJavaScript(
        QStringLiteral("window.__yobroLogin ? window.__yobroLogin.documentId : ''"),
        QWebEngineScript::UserWorld,
        [this, guard, native, credential](const QVariant &value) {
            if (!guard) return;
            const QString documentId = value.toString();
            if (documentId.isEmpty()) {
                showStatus(L(QStringLiteral("Kein unterstütztes Loginformular gefunden.")));
                return;
            }
            // Arguments travel as JSON so no value can break out of the call.
            const QJsonArray arguments{
                QString::fromStdString(credential.origin),
                documentId,
                QString::fromStdString(credential.username),
                QString::fromStdString(credential.password),
            };
            const QString script = QStringLiteral(
                "(function(a){return window.__yobroLogin ? window.__yobroLogin.fill(a[0],a[1],a[2],a[3]) : false;})(%1)"
            ).arg(QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact)));
            native->runJavaScript(script, QWebEngineScript::UserWorld, [this, guard](const QVariant &result) {
                if (!guard) return;
                showStatus(result.toBool()
                    ? L(QStringLiteral("Login ausgefüllt. Das Formular wird nicht automatisch abgeschickt."))
                    : L(QStringLiteral("Kein passendes Loginformular gefunden.")));
            });
        }
    );
}

void SpikeWindow::handleLoginCapture(const QString &username, const QString &password) {
    if (username.isEmpty() || password.isEmpty()) return;
    if (username.size() > 1024 || password.size() > 4096) return;
    auto *page = currentUserPage();
    if (!page || page->profile()->isOffTheRecord() || !passwords_.available()) return;
    // Nothing can be offered for saving while the store cannot be written.
    if (passwords_.locked()) return;
    const std::optional<std::string> origin = PasswordVault::originFor(page->state().url);
    if (!origin) return;

    if (loginSavePrompt_) {
        loginSavePrompt_->deleteLater();
        loginSavePrompt_.clear();
    }
    const LoginCredential credential{
        .origin = *origin,
        .username = username.toStdString(),
        .password = password.toStdString(),
    };
    const std::vector<LoginCredential> existing = passwords_.entries(*origin);
    const bool known = std::any_of(existing.cbegin(), existing.cend(), [&credential](const LoginCredential &entry) {
        return entry.username == credential.username && entry.password == credential.password;
    });
    // Nothing to ask about when this exact credential is already stored.
    if (known) return;

    auto *prompt = new QFrame(this);
    prompt->setObjectName(QStringLiteral("loginSavePrompt"));
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    prompt->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#loginSavePrompt{background:%1;border:1px solid %2;border-radius:14px;}"
        "#loginSaveConfirm{background:%3;color:%4;border-color:%3;font-weight:600;}"
    ).arg(themePalette(systemPrefersDark()).sheetBackground, themePalette(systemPrefersDark()).sheetBorder));
    auto *layout = new QVBoxLayout(prompt);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(7);
    auto *title = new QLabel(L(QStringLiteral("Login in YOBRO speichern?")), prompt);
    title->setStyleSheet(QStringLiteral("font-weight:600;font-size:13px"));
    layout->addWidget(title);
    auto *originLabel = new QLabel(QString::fromStdString(credential.origin), prompt);
    originLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(originLabel);
    layout->addWidget(new QLabel(QString::fromStdString(credential.username), prompt));
    auto *hint = new QLabel(L(QStringLiteral("Wird im macOS-Schlüsselbund dieses Profils gespeichert.")), prompt);
    hint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *actions = new QHBoxLayout();
    auto *later = new QPushButton(L(QStringLiteral("Nicht jetzt")), prompt);
    later->setObjectName(QStringLiteral("loginSaveDismiss"));
    auto *save = new QPushButton(L(QStringLiteral("Speichern")), prompt);
    save->setObjectName(QStringLiteral("loginSaveConfirm"));
    actions->addWidget(later);
    actions->addStretch();
    actions->addWidget(save);
    layout->addLayout(actions);

    loginSavePrompt_ = prompt;
    QObject::connect(later, &QPushButton::clicked, prompt, [prompt] { prompt->close(); });
    QObject::connect(save, &QPushButton::clicked, prompt, [this, prompt, credential] {
        prompt->close();
        // Saving always requires this explicit confirmation.
        switch (passwords_.store(credential, true)) {
        case PasswordStoreResult::created:
            showStatus(L(QStringLiteral("Login gespeichert.")));
            break;
        case PasswordStoreResult::updated:
            showStatus(L(QStringLiteral("Login aktualisiert.")));
            break;
        case PasswordStoreResult::refused:
        case PasswordStoreResult::failed:
            showStatus(passwords_.problem().empty()
                ? L(QStringLiteral("Login konnte nicht gespeichert werden."),
                    QStringLiteral("The login could not be saved."))
                : QString::fromStdString(passwords_.problem()));
            break;
        }
        refreshSavedPasswords();
    });
    prompt->adjustSize();
    prompt->move(std::max(12, width() - prompt->width() - 26), std::max(12, height() - prompt->height() - 46));
    prompt->show();
    prompt->raise();
}

void SpikeWindow::refreshSavedPasswords() {
    if (passwordVaultNote_) {
        passwordVaultNote_->setText(
            passwords_.locked()
                ? (passwords_.exists()
                       ? L(QStringLiteral("Gesperrt. Passphrase eingeben, um die gespeicherten Logins zu benutzen."),
                           QStringLiteral("Locked. Enter the passphrase to use the saved logins."))
                       : L(QStringLiteral("Noch kein Speicher angelegt. Eine Passphrase eingeben, um einen zu erstellen."),
                           QStringLiteral("No store yet. Enter a passphrase to create one.")))
                : L(QStringLiteral("Entsperrt. Der Schlüssel liegt nur im Arbeitsspeicher dieses Fensters."),
                    QStringLiteral("Unlocked. The key is only held in this window's memory."))
        );
    }
    if (!savedPasswordsList_) return;
    savedPasswordsList_->clear();
    for (const LoginCredential &entry : passwords_.entries()) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(
                QString::fromStdString(entry.username),
                QString::fromStdString(entry.origin)
            ),
            savedPasswordsList_
        );
        item->setData(Qt::UserRole, QString::fromStdString(entry.origin));
        item->setData(Qt::UserRole + 1, QString::fromStdString(entry.username));
    }
}

void SpikeWindow::showSavedPasswords() {
    if (passwordsDialog_) {
        refreshSavedPasswords();
        passwordsDialog_->show();
        passwordsDialog_->raise();
        passwordsDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("savedPasswordsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Gespeicherte Logins")));
    dialog->resize(560, 480);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#savedPasswordsList::item{border-radius:11px;margin:3px 0;padding:11px;}"
    ));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(12);
    auto *title = new QLabel(L(QStringLiteral("Gespeicherte Logins")), dialog);
    title->setStyleSheet(QStringLiteral("font:500 25px Georgia,serif"));
    layout->addWidget(title);
    auto *hint = new QLabel(
        passwords_.requiresPassphrase()
            ? L(QStringLiteral("Passwörter liegen verschlüsselt in einer Datei dieses Profils und werden nur für "
                               "die exakt gleiche HTTPS-Adresse angeboten. Passwörter werden hier nicht angezeigt."),
                QStringLiteral("Passwords are kept in an encrypted file in this profile and are only offered for "
                               "the exact same HTTPS address. Passwords are never shown here."))
            : L(QStringLiteral("Passwörter liegen im macOS-Schlüsselbund dieses Profils und werden nur für "
                               "die exakt gleiche HTTPS-Adresse angeboten. Passwörter werden hier nicht angezeigt."),
                QStringLiteral("Passwords are kept in this profile's macOS Keychain and are only offered for "
                               "the exact same HTTPS address. Passwords are never shown here.")),
        dialog
    );
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(hint);
    // Only the file-backed store can be locked. On macOS the Keychain answers
    // for itself and this whole section stays out of the way.
    if (passwords_.requiresPassphrase()) {
        auto *vaultTitle = new QLabel(
            L(QStringLiteral("PASSWORTSPEICHER"), QStringLiteral("PASSWORD STORE")),
            dialog
        );
        vaultTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
        layout->addWidget(vaultTitle);
        passwordVaultNote_ = new QLabel(QString(), dialog);
        passwordVaultNote_->setObjectName(QStringLiteral("passwordVaultStateNote"));
        passwordVaultNote_->setWordWrap(true);
        passwordVaultNote_->setStyleSheet(
            QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
        layout->addWidget(passwordVaultNote_);
        auto *vaultRow = new QHBoxLayout();
        passwordVaultPassphrase_ = new QLineEdit(dialog);
        passwordVaultPassphrase_->setObjectName(QStringLiteral("passwordVaultPassphraseField"));
        passwordVaultPassphrase_->setEchoMode(QLineEdit::Password);
        passwordVaultPassphrase_->setPlaceholderText(
            L(QStringLiteral("Passphrase, mindestens acht Zeichen"),
              QStringLiteral("Passphrase, at least eight characters")));
        auto *unlock = new QPushButton(L(QStringLiteral("Entsperren"), QStringLiteral("Unlock")), dialog);
        unlock->setObjectName(QStringLiteral("passwordVaultUnlockButton"));
        auto *lockButton = new QPushButton(L(QStringLiteral("Sperren"), QStringLiteral("Lock")), dialog);
        lockButton->setObjectName(QStringLiteral("passwordVaultLockButton"));
        vaultRow->addWidget(passwordVaultPassphrase_, 1);
        vaultRow->addWidget(unlock);
        vaultRow->addWidget(lockButton);
        layout->addLayout(vaultRow);
        auto *changeRow = new QHBoxLayout();
        passwordVaultNewPassphrase_ = new QLineEdit(dialog);
        passwordVaultNewPassphrase_->setObjectName(QStringLiteral("passwordVaultNewPassphraseField"));
        passwordVaultNewPassphrase_->setEchoMode(QLineEdit::Password);
        passwordVaultNewPassphrase_->setPlaceholderText(
            L(QStringLiteral("Neue Passphrase"), QStringLiteral("New passphrase")));
        auto *change = new QPushButton(
            L(QStringLiteral("Passphrase ändern"), QStringLiteral("Change passphrase")), dialog);
        change->setObjectName(QStringLiteral("passwordVaultChangeButton"));
        changeRow->addWidget(passwordVaultNewPassphrase_, 1);
        changeRow->addWidget(change);
        layout->addLayout(changeRow);
        QObject::connect(unlock, &QPushButton::clicked, dialog, [this] {
            const bool opened = passwords_.unlock(passwordVaultPassphrase_->text().toStdString());
            passwordVaultPassphrase_->clear();
            if (!opened)
                showStatus(QString::fromStdString(passwords_.problem()));
            refreshSavedPasswords();
        });
        QObject::connect(lockButton, &QPushButton::clicked, dialog, [this] {
            passwords_.lock();
            refreshSavedPasswords();
        });
        QObject::connect(change, &QPushButton::clicked, dialog, [this] {
            const bool changed = passwords_.changePassphrase(
                passwordVaultPassphrase_->text().toStdString(),
                passwordVaultNewPassphrase_->text().toStdString()
            );
            passwordVaultPassphrase_->clear();
            passwordVaultNewPassphrase_->clear();
            showStatus(changed
                ? L(QStringLiteral("Passphrase geändert."), QStringLiteral("Passphrase changed."))
                : QString::fromStdString(passwords_.problem()));
            refreshSavedPasswords();
        });
    }
    savedPasswordsList_ = new QListWidget(dialog);
    savedPasswordsList_->setObjectName(QStringLiteral("savedPasswordsList"));
    layout->addWidget(savedPasswordsList_, 1);
    auto *importTitle = new QLabel(
        L(QStringLiteral("AUS ANDEREM BROWSER ÜBERNEHMEN"), QStringLiteral("TRANSFER FROM ANOTHER BROWSER")),
        dialog
    );
    importTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(importTitle);
    auto *importRow = new QHBoxLayout();
    auto *importBrowser = new QComboBox(dialog);
    importBrowser->setObjectName(QStringLiteral("passwordImportBrowserPicker"));
    importBrowser->addItems(NativePasswordImport::supportedBrowsers());
    auto *importPath = new QLineEdit(dialog);
    importPath->setObjectName(QStringLiteral("passwordImportPathField"));
    importPath->setPlaceholderText(
        L(QStringLiteral("Profilordner des anderen Browsers"), QStringLiteral("Profile folder of the other browser")));
    auto *importBrowse = new QPushButton(L(QStringLiteral("Wählen …"), QStringLiteral("Choose …")), dialog);
    importBrowse->setObjectName(QStringLiteral("passwordImportBrowseButton"));
    importRow->addWidget(importBrowser);
    importRow->addWidget(importPath, 1);
    importRow->addWidget(importBrowse);
    layout->addLayout(importRow);
    auto *importPrimary = new QLineEdit(dialog);
    importPrimary->setObjectName(QStringLiteral("passwordImportPrimaryField"));
    importPrimary->setEchoMode(QLineEdit::Password);
    importPrimary->setPlaceholderText(
        L(QStringLiteral("Firefox-Hauptpasswort, falls gesetzt"), QStringLiteral("Firefox primary password, if set")));
    importPrimary->setVisible(false);
    layout->addWidget(importPrimary);
    auto *importAction = new QPushButton(
        L(QStringLiteral("Passwörter übernehmen …"), QStringLiteral("Transfer passwords …")), dialog);
    importAction->setObjectName(QStringLiteral("passwordImportButton"));
    layout->addWidget(importAction);
    auto *importHint = new QLabel(
        L(QStringLiteral("macOS fragt beim Lesen selbst nach Erlaubnis; eine Ablehnung bricht ab. Der "
                         "Quellbrowser wird nur gelesen, nie verändert. Übernommen wird nur, was zu einer "
                         "Webadresse gehört."),
          QStringLiteral("macOS asks for permission itself while reading; a denial stops the transfer. "
                         "The source browser is only read, never changed. Only logins belonging to a web "
                         "address are transferred.")),
        dialog
    );
    importHint->setWordWrap(true);
    importHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(importHint);
    QObject::connect(importBrowser, &QComboBox::currentTextChanged, dialog, [importPrimary, importPath](const QString &name) {
        importPrimary->setVisible(name == QStringLiteral("Firefox"));
        importPath->setEnabled(name != QStringLiteral("Safari"));
    });
    QObject::connect(importBrowse, &QPushButton::clicked, dialog, [dialog, importPath] {
        const QString chosen = QFileDialog::getExistingDirectory(
            dialog,
            L(QStringLiteral("Profilordner wählen"), QStringLiteral("Choose profile folder")),
            importPath->text()
        );
        if (!chosen.isEmpty()) importPath->setText(chosen);
    });
    QObject::connect(importAction, &QPushButton::clicked, dialog, [this, dialog, importBrowser, importPath, importPrimary] {
        importPasswordsFromBrowser(
            dialog, importBrowser->currentText(), importPath->text().trimmed(), importPrimary->text()
        );
        importPrimary->clear();
    });

    auto *actions = new QHBoxLayout();
    auto *remove = new QPushButton(L(QStringLiteral("Login entfernen")), dialog);
    remove->setObjectName(QStringLiteral("removeSavedLoginButton"));
    auto *close = new QPushButton(L(QStringLiteral("Fertig")), dialog);
    close->setObjectName(QStringLiteral("closeSavedPasswordsButton"));
    actions->addWidget(remove);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);

    passwordsDialog_ = dialog;
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(remove, &QPushButton::clicked, dialog, [this] {
        if (!savedPasswordsList_ || !savedPasswordsList_->currentItem()) return;
        QListWidgetItem *item = savedPasswordsList_->currentItem();
        const bool removed = passwords_.remove(
            item->data(Qt::UserRole).toString().toStdString(),
            item->data(Qt::UserRole + 1).toString().toStdString()
        );
        showStatus(removed
            ? L(QStringLiteral("Login entfernt."))
            : L(QStringLiteral("Login konnte nicht entfernt werden.")));
        refreshSavedPasswords();
    });
    QObject::connect(dialog, &QDialog::destroyed, this, [this] {
        savedPasswordsList_ = nullptr;
        passwordVaultNote_ = nullptr;
        passwordVaultPassphrase_ = nullptr;
        passwordVaultNewPassphrase_ = nullptr;
    });
    refreshSavedPasswords();
    dialog->show();
}

void SpikeWindow::importPasswordsFromBrowser(
    QWidget *parent,
    const QString &browser,
    const QString &profilePath,
    const QString &primaryPassword
) {
    if (browser != QStringLiteral("Safari") && profilePath.isEmpty()) {
        showStatus(L(
            QStringLiteral("Bitte den Profilordner des anderen Browsers wählen."),
            QStringLiteral("Please choose the other browser's profile folder.")
        ));
        return;
    }

    const ImportedLoginSet found =
        NativePasswordImport::load(browser, profilePath, primaryPassword);
    if (!found.problem.isEmpty()) {
        showStatus(found.problem);
        QMessageBox::warning(
            parent,
            L(QStringLiteral("Passwörter übernehmen"), QStringLiteral("Transfer passwords")),
            found.problem
        );
        return;
    }
    if (found.needsPrimaryPassword) {
        showStatus(L(
            QStringLiteral("Firefox verlangt das Hauptpasswort. Bitte eintragen und erneut versuchen."),
            QStringLiteral("Firefox requires its primary password. Enter it and try again.")
        ));
        return;
    }
    if (found.logins.empty()) {
        showStatus(L(
            QStringLiteral("In diesem Profil wurden keine übernehmbaren Logins gefunden."),
            QStringLiteral("No transferable logins were found in this profile.")
        ));
        return;
    }

    // Overwriting an existing login is a decision the user has to make, so the
    // count is shown before anything is written.
    const auto decision = QMessageBox::question(
        parent,
        L(QStringLiteral("Passwörter übernehmen?"), QStringLiteral("Transfer passwords?")),
        L(QStringLiteral("%1 Logins aus %2 in den Schlüsselbund dieses Profils übernehmen? "
                         "Vorhandene Einträge für dieselbe Adresse und denselben Benutzernamen werden "
                         "überschrieben."),
          QStringLiteral("Transfer %1 logins from %2 into this profile's keychain? Existing entries for "
                         "the same address and user name are overwritten."))
            .arg(found.logins.size())
            .arg(browser),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel
    );
    if (decision != QMessageBox::Yes) return;

    std::size_t stored = 0;
    std::size_t skipped = 0;
    for (const ImportedLogin &login : found.logins) {
        // The vault only accepts plain HTTPS origins, which also drops anything
        // that is not a real web login.
        const auto origin = PasswordVault::originFor(login.url.toStdString());
        if (!origin) {
            ++skipped;
            continue;
        }
        const PasswordStoreResult outcome = passwords_.store(
            {*origin, login.username.toStdString(), login.password.toStdString()},
            true
        );
        if (outcome == PasswordStoreResult::created || outcome == PasswordStoreResult::updated)
            ++stored;
        else
            ++skipped;
    }

    QStringList summary;
    summary.append(
        L(QStringLiteral("%1 Logins übernommen."), QStringLiteral("%1 logins transferred.")).arg(stored)
    );
    if (skipped > 0) {
        summary.append(L(
            QStringLiteral("%1 Einträge übersprungen, weil sie keine reine HTTPS-Adresse haben."),
            QStringLiteral("%1 entries skipped because they are not a plain HTTPS address.")
        ).arg(skipped));
    }
    summary.append(found.warnings);
    showStatus(summary.join(QStringLiteral(" ")));
    QMessageBox::information(
        parent,
        L(QStringLiteral("Passwörter übernehmen"), QStringLiteral("Transfer passwords")),
        summary.join(QStringLiteral("\n"))
    );
    refreshSavedPasswords();
}

} // namespace yobro::spike
