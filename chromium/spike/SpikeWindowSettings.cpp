#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::showSettings() {
    if (settingsDialog_) {
        refreshProfileList();
        settingsDialog_->show();
        settingsDialog_->raise();
        settingsDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("settingsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Einstellungen")));
    dialog->resize(620, 560);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#profileList::item{border-radius:11px;margin:3px 0;padding:11px;}"
    ));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(13);

    auto *title = new QLabel(L(QStringLiteral("Einstellungen")), dialog);
    title->setStyleSheet(QStringLiteral("font:500 27px Georgia,serif;color:%1").arg(themePalette(systemPrefersDark()).sheetText));
    layout->addWidget(title);

    auto *passkeyTitle = new QLabel(L(QStringLiteral("PASSKEYS"), QStringLiteral("PASSKEYS")), dialog);
    passkeyTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(passkeyTitle);
    auto *passkeyNote = new QLabel(dialog);
    passkeyNote->setObjectName(QStringLiteral("passkeyStateNote"));
    passkeyNote->setWordWrap(true);
    // The state is read from the engine, so this text cannot drift from reality.
    passkeyNote->setText(qtwebengine::QtBrowserProfile::passkeysWork()
        ? L(QStringLiteral("Passkeys sind verfügbar. Websites können einen Sicherheitsschlüssel oder "
                           "eine gespeicherte Anmeldung anfordern."),
            QStringLiteral("Passkeys are available. Sites can ask for a security key or a stored "
                           "sign-in."))
        : L(QStringLiteral("Passkeys funktionieren mit dieser Engine-Version nicht: eine Anfrage wird "
                           "angenommen, aber nie beantwortet. Damit eine Website nicht endlos wartet, "
                           "wird eine Passkey-Anfrage sofort abgelehnt; wähle dort vorerst eine andere "
                           "Anmeldemethode. Die Bedienung dafür ist fertig und greift, sobald die "
                           "Engine antwortet."),
            QStringLiteral("Passkeys do not work with this engine version: a request is accepted but "
                           "never answered. So a site does not wait forever, a passkey request is "
                           "declined right away; choose another sign-in method there for now. The "
                           "handling is in place and takes over once the engine answers.")));
    layout->addWidget(passkeyNote);

    auto *accountTitle = new QLabel(L(QStringLiteral("KONTO UND SYNCHRONISIERUNG"),
                                      QStringLiteral("ACCOUNT AND SYNCHRONISATION")), dialog);
    accountTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(accountTitle);

    auto *syncEmail = new QLineEdit(dialog);
    syncEmail->setObjectName(QStringLiteral("syncEmailField"));
    syncEmail->setPlaceholderText(L(QStringLiteral("E-Mail"), QStringLiteral("Email")));
    auto *syncPassword = new QLineEdit(dialog);
    syncPassword->setObjectName(QStringLiteral("syncPasswordField"));
    syncPassword->setEchoMode(QLineEdit::Password);
    syncPassword->setPlaceholderText(L(QStringLiteral("Passwort"), QStringLiteral("Password")));
    auto *syncRecoveryField = new QLineEdit(dialog);
    syncRecoveryField->setObjectName(QStringLiteral("syncRecoveryField"));
    syncRecoveryField->setPlaceholderText(L(
        QStringLiteral("Wiederherstellungscode (nur beim zweiten Gerät)"),
        QStringLiteral("Recovery code (only on a second device)")
    ));
    layout->addWidget(syncEmail);
    layout->addWidget(syncPassword);
    layout->addWidget(syncRecoveryField);

    auto *syncButtons = new QHBoxLayout();
    auto *syncSignIn = new QPushButton(L(QStringLiteral("Anmelden"), QStringLiteral("Sign in")), dialog);
    syncSignIn->setObjectName(QStringLiteral("syncSignInButton"));
    auto *syncSignUp = new QPushButton(L(QStringLiteral("Konto anlegen"),
                                         QStringLiteral("Create account")), dialog);
    syncSignUp->setObjectName(QStringLiteral("syncSignUpButton"));
    auto *syncNow = new QPushButton(L(QStringLiteral("Jetzt abgleichen"), QStringLiteral("Sync now")), dialog);
    syncNow->setObjectName(QStringLiteral("syncNowButton"));
    auto *syncSignOut = new QPushButton(L(QStringLiteral("Abmelden"), QStringLiteral("Sign out")), dialog);
    syncSignOut->setObjectName(QStringLiteral("syncSignOutButton"));
    syncButtons->addWidget(syncSignIn);
    syncButtons->addWidget(syncSignUp);
    syncButtons->addWidget(syncNow);
    syncButtons->addWidget(syncSignOut);
    syncButtons->addStretch();
    layout->addLayout(syncButtons);

    syncStatus_ = new QLabel(dialog);
    syncStatus_->setObjectName(QStringLiteral("syncStatusLabel"));
    syncStatus_->setWordWrap(true);
    layout->addWidget(syncStatus_);
    syncRecovery_ = new QLabel(dialog);
    syncRecovery_->setObjectName(QStringLiteral("syncRecoveryLabel"));
    syncRecovery_->setWordWrap(true);
    syncRecovery_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(syncRecovery_);

    // Everything the account section shows comes from the controller, so one
    // place decides what is true.
    const auto refreshSync = [this, syncSignIn, syncSignUp, syncNow, syncSignOut] {
        if (!sync_ || !syncStatus_ || !syncRecovery_) return;
        syncStatus_->setText(sync_->status());
        syncSignIn->setEnabled(!sync_->busy() && !sync_->signedIn());
        syncSignUp->setEnabled(!sync_->busy() && !sync_->signedIn());
        syncNow->setEnabled(!sync_->busy() && sync_->signedIn());
        syncSignOut->setEnabled(!sync_->busy() && sync_->signedIn());
        // The recovery code is the only other copy of the key, so it is shown
        // plainly and only while signed in.
        syncRecovery_->setText(sync_->signedIn() && !sync_->recoveryCode().isEmpty()
            ? L(QStringLiteral("Wiederherstellungscode (aufschreiben, sonst sind die Daten verloren): "),
                QStringLiteral("Recovery code (write it down, or the data is lost): "))
                  + sync_->recoveryCode()
            : QString());
    };
    QObject::connect(sync_, &SyncController::changed, dialog, refreshSync);
    QObject::connect(syncSignIn, &QPushButton::clicked, dialog, [=, this] {
        sync_->signIn(syncEmail->text(), syncPassword->text(), syncRecoveryField->text(),
                      [=, this](QString problem) {
            if (problem.isEmpty()) {
                // Neither the password nor the code stays in the window.
                syncPassword->clear();
                syncRecoveryField->clear();
            }
            refreshSync();
        });
        refreshSync();
    });
    QObject::connect(syncSignUp, &QPushButton::clicked, dialog, [=, this] {
        sync_->signUp(syncEmail->text(), syncPassword->text(), [=](QString) {
            syncPassword->clear();
            refreshSync();
        });
        refreshSync();
    });
    QObject::connect(syncNow, &QPushButton::clicked, dialog, [=, this] {
        sync_->syncNow([refreshSync](QString) { refreshSync(); });
        refreshSync();
    });
    QObject::connect(syncSignOut, &QPushButton::clicked, dialog, [=, this] {
        sync_->signOut();
        refreshSync();
    });
    refreshSync();

    // The same entry the WebKit build offers, so the setup is reachable again
    // after it was skipped.
    auto *reopenOnboarding = new QPushButton(
        L(QStringLiteral("Einrichtung erneut öffnen …"), QStringLiteral("Open setup again …")), dialog);
    reopenOnboarding->setObjectName(QStringLiteral("reopenOnboardingButton"));
    QObject::connect(reopenOnboarding, &QPushButton::clicked, this, [this] { showOnboarding(); });
    layout->addWidget(reopenOnboarding);

    auto *profileTitle = new QLabel(L(QStringLiteral("PROFILE")), dialog);
    profileTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(profileTitle);
    profileList_ = new QListWidget(dialog);
    profileList_->setObjectName(QStringLiteral("profileList"));
    layout->addWidget(profileList_, 1);
    auto *profileHint = new QLabel(
        QStringLiteral("Profile haben getrennte Cookies, Tabs, Verlauf und Erweiterungen. "
                       "Ein anderes Profil öffnet ein eigenes Fenster mit eigenem Agent-Socket."),
        dialog
    );
    profileHint->setWordWrap(true);
    profileHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(profileHint);
    auto *profileActions = new QHBoxLayout();
    auto *openProfile = new QPushButton(L(QStringLiteral("Profil öffnen")), dialog);
    openProfile->setObjectName(QStringLiteral("openProfileButton"));
    auto *newProfile = new QPushButton(L(QStringLiteral("Neues Profil…")), dialog);
    newProfile->setObjectName(QStringLiteral("newProfileButton"));
    auto *renameProfileButton = new QPushButton(
        L(QStringLiteral("Umbenennen…"), QStringLiteral("Rename…")), dialog);
    renameProfileButton->setObjectName(QStringLiteral("renameProfileButton"));
    auto *profileIconButton = new QPushButton(
        L(QStringLiteral("Symbol…"), QStringLiteral("Icon…")), dialog);
    profileIconButton->setObjectName(QStringLiteral("profileIconButton"));
    profileActions->addWidget(openProfile);
    profileActions->addWidget(newProfile);
    profileActions->addWidget(renameProfileButton);
    profileActions->addWidget(profileIconButton);
    profileActions->addStretch();
    layout->addLayout(profileActions);

    auto *proxyTitle = new QLabel(
        L(QStringLiteral("PROXY FÜR „%1“"), QStringLiteral("PROXY FOR “%1”")).arg(activeSpace_), dialog);
    proxyTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(proxyTitle);
    const SpaceProxyConfig storedProxy = proxies_.config(activeSpace_).value_or(SpaceProxyConfig{});
    auto *proxyEnabled = new QCheckBox(
        L(QStringLiteral("Webverkehr dieses Space über einen Proxy leiten"),
          QStringLiteral("Route this space's web traffic through a proxy")), dialog);
    proxyEnabled->setObjectName(QStringLiteral("proxyEnabledToggle"));
    proxyEnabled->setChecked(proxies_.config(activeSpace_).has_value() && storedProxy.enabled);
    layout->addWidget(proxyEnabled);
    auto *proxyForm = new QFormLayout();
    auto *proxyType = new QComboBox(dialog);
    proxyType->setObjectName(QStringLiteral("proxyTypePicker"));
    proxyType->addItem(QStringLiteral("SOCKS5"));
    proxyType->addItem(QStringLiteral("HTTP CONNECT"));
    proxyType->setCurrentIndex(storedProxy.type == SpaceProxyType::socks5 ? 0 : 1);
    auto *proxyHost = new QLineEdit(storedProxy.host, dialog);
    proxyHost->setObjectName(QStringLiteral("proxyHostField"));
    proxyHost->setPlaceholderText(QStringLiteral("proxy.example.com"));
    auto *proxyPort = new QLineEdit(QString::number(storedProxy.port), dialog);
    proxyPort->setObjectName(QStringLiteral("proxyPortField"));
    auto *proxyUser = new QLineEdit(storedProxy.username, dialog);
    proxyUser->setObjectName(QStringLiteral("proxyUserField"));
    auto *proxyPassword = new QLineEdit(dialog);
    proxyPassword->setObjectName(QStringLiteral("proxyPasswordField"));
    proxyPassword->setEchoMode(QLineEdit::Password);
    proxyPassword->setPlaceholderText(
        L(QStringLiteral("Unverändert lassen, um das gespeicherte Passwort zu behalten"),
          QStringLiteral("Leave unchanged to keep the stored password")));
    auto *proxyMatch = new QLineEdit(storedProxy.matchDomains.join(QStringLiteral(", ")), dialog);
    proxyMatch->setObjectName(QStringLiteral("proxyMatchField"));
    proxyMatch->setPlaceholderText(L(QStringLiteral("Leer bedeutet: alle Adressen"),
                                     QStringLiteral("Empty means every address")));
    auto *proxyExcluded = new QLineEdit(storedProxy.excludedDomains.join(QStringLiteral(", ")), dialog);
    proxyExcluded->setObjectName(QStringLiteral("proxyExcludedField"));
    proxyForm->addRow(L(QStringLiteral("Art"), QStringLiteral("Kind")), proxyType);
    proxyForm->addRow(L(QStringLiteral("Server"), QStringLiteral("Server")), proxyHost);
    proxyForm->addRow(L(QStringLiteral("Port"), QStringLiteral("Port")), proxyPort);
    proxyForm->addRow(L(QStringLiteral("Benutzername")), proxyUser);
    proxyForm->addRow(L(QStringLiteral("Passwort")), proxyPassword);
    proxyForm->addRow(L(QStringLiteral("Nur diese Domains"), QStringLiteral("Only these domains")), proxyMatch);
    proxyForm->addRow(L(QStringLiteral("Ausnahmen"), QStringLiteral("Exceptions")), proxyExcluded);
    layout->addLayout(proxyForm);
    auto *proxySave = new QPushButton(L(QStringLiteral("Proxy speichern"), QStringLiteral("Save proxy")), dialog);
    proxySave->setObjectName(QStringLiteral("proxySaveButton"));
    auto *proxyRemove = new QPushButton(L(QStringLiteral("Proxy entfernen"), QStringLiteral("Remove proxy")), dialog);
    proxyRemove->setObjectName(QStringLiteral("proxyRemoveButton"));
    auto *proxyRestart = new QPushButton(
        L(QStringLiteral("Neu starten, um zu übernehmen"), QStringLiteral("Restart to apply")), dialog);
    proxyRestart->setObjectName(QStringLiteral("proxyRestartButton"));
    auto *proxyButtons = new QHBoxLayout();
    proxyButtons->addWidget(proxySave);
    proxyButtons->addWidget(proxyRemove);
    proxyButtons->addWidget(proxyRestart);
    proxyButtons->addStretch();
    layout->addLayout(proxyButtons);
    QObject::connect(proxyRestart, &QPushButton::clicked, dialog, [this] {
        saveSession();
        if (!launchProfileInstance(QString::fromStdString(paths_.profile.filename().string()))) return;
        close();
    });
    auto *proxyHint = new QLabel(
        L(QStringLiteral("Das Passwort liegt im Schlüsselbund, nicht in der Einstellungsdatei. Chromium "
                         "kennt nur einen Proxy pro Programm: der Proxy dieses Space gilt, solange er aktiv "
                         "ist, auch für offene Tabs anderer Spaces. Lässt sich ein eingerichteter Proxy nicht "
                         "aktivieren, wird nichts geladen, statt ungeschützt zu laden."),
          QStringLiteral("The password lives in the keychain, not in the settings file. Chromium knows only "
                         "one proxy per application: this space's proxy applies while the space is active, "
                         "including open tabs of other spaces. If a configured proxy cannot be activated, "
                         "nothing loads instead of loading unprotected.")),
        dialog
    );
    proxyHint->setWordWrap(true);
    proxyHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(proxyHint);
    QObject::connect(proxySave, &QPushButton::clicked, dialog, [this, proxyEnabled, proxyType, proxyHost,
                                                                proxyPort, proxyUser, proxyPassword,
                                                                proxyMatch, proxyExcluded] {
        const auto splitDomains = [](const QString &text) {
            return text.split(QLatin1Char(','), Qt::SkipEmptyParts);
        };
        SpaceProxyConfig config;
        config.enabled = proxyEnabled->isChecked();
        config.type = proxyType->currentIndex() == 0 ? SpaceProxyType::socks5 : SpaceProxyType::httpConnect;
        config.host = proxyHost->text();
        config.port = proxyPort->text().trimmed().toInt();
        config.username = proxyUser->text();
        config.password = proxyPassword->text();
        config.matchDomains = splitDomains(proxyMatch->text());
        config.excludedDomains = splitDomains(proxyExcluded->text());
        if (const QString problem = proxies_.set(activeSpace_, config); !problem.isEmpty()) {
            showStatus(problem);
            return;
        }
        proxyPassword->clear();
        applySpaceProxy();
    });
    QObject::connect(proxyRemove, &QPushButton::clicked, dialog, [this, proxyEnabled, proxyHost] {
        if (const QString problem = proxies_.remove(activeSpace_); !problem.isEmpty()) {
            showStatus(problem);
            return;
        }
        proxyEnabled->setChecked(false);
        proxyHost->clear();
        applySpaceProxy();
        showStatus(
            L(QStringLiteral("Proxy für „%1“ entfernt."), QStringLiteral("Proxy for “%1” removed."))
                .arg(activeSpace_)
        );
    });

    auto *adBlockTitle = new QLabel(L(QStringLiteral("WERBUNG"), QStringLiteral("ADVERTISING")), dialog);
    adBlockTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(adBlockTitle);
    auto *adBlockToggle = new QCheckBox(
        L(QStringLiteral("Werbung und Tracker blockieren"), QStringLiteral("Block ads and trackers")), dialog);
    adBlockToggle->setObjectName(QStringLiteral("adBlockToggle"));
    adBlockToggle->setChecked(adBlock_.enabled());
    layout->addWidget(adBlockToggle);
    auto *strictToggle = new QCheckBox(
        L(QStringLiteral("Strikter Schutz"), QStringLiteral("Strict protection")), dialog);
    strictToggle->setObjectName(QStringLiteral("adBlockStrictToggle"));
    strictToggle->setChecked(adBlock_.strictProtection());
    strictToggle->setEnabled(adBlock_.enabled());
    layout->addWidget(strictToggle);
    auto *adBlockHint = new QLabel(
        L(QStringLiteral("Blockiert Anfragen zu %1 bekannten Werbe-, Analyse- und Tracking-Diensten, bevor sie "
                         "geladen werden, und entfernt Klick-Kennungen aus Adressen. Strikter Schutz verzichtet "
                         "auf Wiedergabe-Manipulation; ohne ihn greifen zusätzliche Seitenfilter, dafür können "
                         "Videoseiten die Wiedergabe sperren."),
          QStringLiteral("Blocks requests to %1 known advertising, analytics and tracking services before they "
                         "load, and strips click identifiers from addresses. Strict protection avoids playback "
                         "manipulation; without it additional page filters apply, but video sites may block "
                         "playback."))
            .arg(AdBlockRules::blockedDomains().size()),
        dialog
    );
    adBlockHint->setWordWrap(true);
    adBlockHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(adBlockHint);
    QObject::connect(adBlockToggle, &QCheckBox::toggled, dialog, [this, strictToggle](bool checked) {
        adBlock_.setEnabled(checked);
        strictToggle->setEnabled(checked);
        updateAdBlocking(true);
        showStatus(checked
            ? L(QStringLiteral("Werbefilter aktiv. Offene Seiten werden neu geladen."),
                QStringLiteral("Ad filter active. Open pages are reloaded."))
            : L(QStringLiteral("Werbefilter aus. Offene Seiten werden neu geladen."),
                QStringLiteral("Ad filter off. Open pages are reloaded.")));
    });
    QObject::connect(strictToggle, &QCheckBox::toggled, dialog, [this](bool checked) {
        adBlock_.setStrictProtection(checked);
        updateAdBlocking(true);
        showStatus(checked
            ? L(QStringLiteral("Strikter Schutz aktiv, keine Wiedergabe-Manipulation."),
                QStringLiteral("Strict protection active, no playback manipulation."))
            : L(QStringLiteral("Zusätzliche Seitenfilter aktiv."),
                QStringLiteral("Additional page filters active.")));
    });

    auto *privacyTitle = new QLabel(L(QStringLiteral("DATENSCHUTZ"), QStringLiteral("PRIVACY")), dialog);
    privacyTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(privacyTitle);
    auto *cookieRow = new QHBoxLayout();
    cookieRow->addWidget(new QLabel(L(QStringLiteral("Cookies"), QStringLiteral("Cookies")), dialog));
    auto *cookiePolicy = new QComboBox(dialog);
    cookiePolicy->setObjectName(QStringLiteral("cookieRetentionPicker"));
    cookiePolicy->addItem(L(QStringLiteral("Behalten, Logins bleiben"), QStringLiteral("Keep, stay signed in")));
    cookiePolicy->addItem(L(QStringLiteral("Beim Beenden löschen"), QStringLiteral("Delete when quitting")));
    cookiePolicy->setCurrentIndex(privacy_.cookieRetention() == CookieRetention::sessionOnly ? 1 : 0);
    cookieRow->addWidget(cookiePolicy, 1);
    layout->addLayout(cookieRow);
    QObject::connect(cookiePolicy, &QComboBox::currentIndexChanged, dialog, [this](int index) {
        privacy_.setCookieRetention(index == 1 ? CookieRetention::sessionOnly : CookieRetention::keep);
        showStatus(index == 1
            ? L(QStringLiteral("Cookies werden beim Beenden gelöscht. Wirkt ab dem nächsten Start."),
                QStringLiteral("Cookies are deleted when quitting. Takes effect on the next start."))
            : L(QStringLiteral("Cookies bleiben erhalten. Wirkt ab dem nächsten Start."),
                QStringLiteral("Cookies are kept. Takes effect on the next start.")));
    });

    auto *clearRow = new QHBoxLayout();
    auto *clearRange = new QComboBox(dialog);
    clearRange->setObjectName(QStringLiteral("clearHistoryRangePicker"));
    clearRange->addItem(L(QStringLiteral("Letzte Stunde"), QStringLiteral("Last hour")));
    clearRange->addItem(L(QStringLiteral("Letzter Tag"), QStringLiteral("Last day")));
    clearRange->addItem(L(QStringLiteral("Letzte Woche"), QStringLiteral("Last week")));
    clearRange->addItem(L(QStringLiteral("Alles"), QStringLiteral("Everything")));
    clearRange->setCurrentIndex(3);
    auto *includeHistory = new QCheckBox(
        L(QStringLiteral("Verlauf mitlöschen"), QStringLiteral("Clear history as well")), dialog);
    includeHistory->setObjectName(QStringLiteral("clearHistoryToggle"));
    // Off by default, like the WebKit build: clearing cookies should not silently
    // take the history with it.
    includeHistory->setChecked(false);
    auto *clearButton = new QPushButton(
        L(QStringLiteral("Browserdaten löschen …"), QStringLiteral("Clear browsing data …")), dialog);
    clearButton->setObjectName(QStringLiteral("clearBrowsingDataButton"));
    clearRow->addWidget(clearRange, 1);
    clearRow->addWidget(includeHistory);
    clearRow->addWidget(clearButton);
    layout->addLayout(clearRow);
    auto *clearHint = new QLabel(
        L(QStringLiteral("Cookies, Zwischenspeicher und besuchte Links werden immer vollständig entfernt; "
                         "der Zeitraum gilt für den Verlauf. Gespeicherte Passwörter bleiben erhalten. "
                         "Du wirst auf betroffenen Seiten abgemeldet."),
          QStringLiteral("Cookies, cache and visited links are always removed completely; the range applies "
                         "to the history. Saved passwords are kept. You will be signed out of affected sites.")),
        dialog
    );
    clearHint->setWordWrap(true);
    clearHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(clearHint);
    QObject::connect(clearButton, &QPushButton::clicked, dialog, [this, dialog, clearRange, includeHistory] {
        // Clearing signs the user out, so it needs a confirmation.
        const auto decision = QMessageBox::question(
            dialog,
            L(QStringLiteral("Browserdaten löschen?"), QStringLiteral("Clear browsing data?")),
            L(QStringLiteral("Cookies und Websitedaten werden entfernt. Du wirst auf betroffenen Seiten abgemeldet."),
              QStringLiteral("Cookies and website data will be removed. You will be signed out of affected sites.")),
            QMessageBox::Cancel | QMessageBox::Yes,
            QMessageBox::Cancel
        );
        if (decision != QMessageBox::Yes) return;
        clearBrowsingData(clearRange->currentIndex(), includeHistory->isChecked());
    });

    auto *appearanceTitle = new QLabel(L(QStringLiteral("DARSTELLUNG")), dialog);
    appearanceTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(appearanceTitle);
    auto *darkenToggle = new QCheckBox(L(QStringLiteral("Helle Websites abdunkeln, sobald das System dunkel ist")), dialog);
    darkenToggle->setObjectName(QStringLiteral("settingsDarkenWebsitesToggle"));
    darkenToggle->setChecked(appearance_.enabled());
    layout->addWidget(darkenToggle);
    QObject::connect(darkenToggle, &QCheckBox::toggled, dialog, [this](bool enabled) {
        appearance_.setEnabled(enabled);
        updateWebAppearance();
    });
    auto *appearanceHint = new QLabel(
        QStringLiteral("Bereits dunkle Seiten behalten ihre Farben. Einzelne Websites lassen sich über "
                       "das Symbol in der Adressleiste ausnehmen."),
        dialog
    );
    appearanceHint->setWordWrap(true);
    appearanceHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(appearanceHint);

    auto *passwordsTitle = new QLabel(L(QStringLiteral("LOGINS")), dialog);
    passwordsTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(passwordsTitle);
    auto *managePasswords = new QPushButton(L(QStringLiteral("Gespeicherte Logins verwalten…")), dialog);
    managePasswords->setObjectName(QStringLiteral("managePasswordsButton"));
    managePasswords->setEnabled(passwords_.available());
    if (!passwords_.available())
        managePasswords->setToolTip(L(QStringLiteral("Der Schlüsselbund ist auf dieser Plattform nicht verfügbar.")));
    layout->addWidget(managePasswords);
    QObject::connect(managePasswords, &QPushButton::clicked, dialog, [this] { showSavedPasswords(); });

    auto *agentTitle = new QLabel(L(QStringLiteral("AGENTENZUGRIFF")), dialog);
    agentTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(agentTitle);
    auto *agentPaused = new QCheckBox(L(QStringLiteral("Agentenzugriff für dieses Fenster pausieren")), dialog);
    agentPaused->setObjectName(QStringLiteral("agentPausedToggle"));
    agentPaused->setChecked(!session_.agentEnabled());
    layout->addWidget(agentPaused);
    auto *libraryToggle = new QCheckBox(L(QStringLiteral("Agenten dürfen Verlauf und Downloads lesen")), dialog);
    libraryToggle->setObjectName(QStringLiteral("settingsLibraryAccessToggle"));
    libraryToggle->setChecked(session_.protocolState().libraryAccess);
    layout->addWidget(libraryToggle);

    auto *pathsTitle = new QLabel(L(QStringLiteral("SPEICHERORTE")), dialog);
    pathsTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
    layout->addWidget(pathsTitle);
    auto *pathsLabel = new QLabel(
        QStringLiteral("Profil: %1\nAgent-Socket: %2\nDownloads: %3")
            .arg(QString::fromStdString(paths_.profile.string()),
                 QString::fromStdString(paths_.control.string()),
                 QString::fromStdString(library_.downloadDirectory())),
        dialog
    );
    pathsLabel->setObjectName(QStringLiteral("settingsPath"));
    pathsLabel->setWordWrap(true);
    pathsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathsLabel);

    auto *footer = new QHBoxLayout();
    auto *openDownloads = new QPushButton(L(QStringLiteral("Downloadordner öffnen")), dialog);
    openDownloads->setObjectName(QStringLiteral("openDownloadDirectoryButton"));
    auto *close = new QPushButton(L(QStringLiteral("Fertig")), dialog);
    close->setObjectName(QStringLiteral("closeSettingsButton"));
    footer->addWidget(openDownloads);
    footer->addStretch();
    footer->addWidget(close);
    layout->addLayout(footer);

    settingsDialog_ = dialog;
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    QObject::connect(openProfile, &QPushButton::clicked, dialog, [this] {
        if (!profileList_ || !profileList_->currentItem()) return;
        activateProfile(profileList_->currentItem()->data(Qt::UserRole).toString());
    });
    QObject::connect(profileList_, &QListWidget::itemActivated, dialog, [this](QListWidgetItem *item) {
        if (item) activateProfile(item->data(Qt::UserRole).toString());
    });
    QObject::connect(renameProfileButton, &QPushButton::clicked, dialog, [this] {
        if (!profileList_ || !profileList_->currentItem()) return;
        renameProfile(profileList_->currentItem()->data(Qt::UserRole).toString());
    });
    QObject::connect(profileIconButton, &QPushButton::clicked, dialog, [this] {
        if (!profileList_ || !profileList_->currentItem()) return;
        setProfileIcon(profileList_->currentItem()->data(Qt::UserRole).toString());
    });
    QObject::connect(newProfile, &QPushButton::clicked, dialog, [this] { createProfile(); });
    QObject::connect(agentPaused, &QCheckBox::toggled, dialog, [this](bool paused) {
        session_.setAgentEnabled(!paused);
        showStatus(paused
            ? L(QStringLiteral("Agentenzugriff pausiert."))
            : L(QStringLiteral("Agentenzugriff aktiv.")));
    });
    QObject::connect(libraryToggle, &QCheckBox::toggled, dialog, [this, libraryToggle](bool enabled) {
        const std::optional<std::string> problem = bridgePolicy_.persistAndApply(
            enabled,
            [this](bool value) { session_.setLibraryAccess(value); }
        );
        if (problem) {
            const QSignalBlocker blocker(libraryToggle);
            libraryToggle->setChecked(session_.protocolState().libraryAccess);
            showStatus(QStringLiteral("Library-Richtlinie unverändert: ") + QString::fromStdString(*problem));
            return;
        }
        synchronizeSession();
    });
    QObject::connect(openDownloads, &QPushButton::clicked, dialog, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(library_.downloadDirectory())));
    });
    QObject::connect(dialog, &QDialog::destroyed, this, [this] { profileList_ = nullptr; });
    refreshProfileList();
    dialog->show();
}

} // namespace yobro::spike
