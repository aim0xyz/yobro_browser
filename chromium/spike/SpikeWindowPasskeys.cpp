#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

// MARK: - Passkeys

void SpikeWindow::installPasskeyShim() {
    const QString name = QStringLiteral("YOBRO PasskeyShim");
    std::vector<QWebEngineScriptCollection *> collections{
        profile_->persistentProfile()->scripts(),
        profile_->privateProfile()->scripts(),
    };
    for (QWebEngineScriptCollection *scripts : collections) {
        for (const QWebEngineScript &existing : scripts->find(name)) scripts->remove(existing);
    }
    // Once the engine answers passkey requests properly, the real dialog takes
    // over and nothing is injected.
    if (qtwebengine::QtBrowserProfile::passkeysWork()) return;

    QFile source(QStringLiteral(":/yobro/PasskeyShim.js"));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QWebEngineScript script;
    script.setName(name);
    script.setSourceCode(QString::fromUtf8(source.readAll()));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    // It has to replace what the page itself calls, so it shares the page world.
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(true);
    for (QWebEngineScriptCollection *scripts : collections) scripts->insert(script);
}

QString SpikeWindow::passkeyFailureText(engine::PasskeyFailure failure) {
    switch (failure) {
    case engine::PasskeyFailure::timeout:
        return L(QStringLiteral("Die Anmeldung hat zu lange gedauert."),
                 QStringLiteral("The sign-in took too long."));
    case engine::PasskeyFailure::keyNotRegistered:
        return L(QStringLiteral("Dieser Sicherheitsschlüssel ist bei der Website nicht registriert."),
                 QStringLiteral("This security key is not registered with the site."));
    case engine::PasskeyFailure::keyAlreadyRegistered:
        return L(QStringLiteral("Dieser Sicherheitsschlüssel ist dort schon registriert."),
                 QStringLiteral("This security key is already registered there."));
    case engine::PasskeyFailure::softPinBlock:
        return L(QStringLiteral("Zu viele falsche PIN-Eingaben. Schlüssel abziehen und erneut anstecken."),
                 QStringLiteral("Too many wrong PIN attempts. Unplug the key and plug it in again."));
    case engine::PasskeyFailure::hardPinBlock:
        return L(QStringLiteral("Der Schlüssel ist dauerhaft gesperrt und muss zurückgesetzt werden."),
                 QStringLiteral("The key is permanently locked and has to be reset."));
    case engine::PasskeyFailure::authenticatorRemovedDuringPinEntry:
        return L(QStringLiteral("Der Schlüssel wurde während der PIN-Eingabe abgezogen."),
                 QStringLiteral("The key was removed during PIN entry."));
    case engine::PasskeyFailure::authenticatorMissingResidentKeys:
        return L(QStringLiteral("Dieser Schlüssel kann keine Passkeys speichern."),
                 QStringLiteral("This key cannot store passkeys."));
    case engine::PasskeyFailure::authenticatorMissingUserVerification:
        return L(QStringLiteral("Dieser Schlüssel kann dich nicht überprüfen."),
                 QStringLiteral("This key cannot verify you."));
    case engine::PasskeyFailure::authenticatorMissingLargeBlob:
        return L(QStringLiteral("Dieser Schlüssel kann die verlangten Zusatzdaten nicht speichern."),
                 QStringLiteral("This key cannot store the extra data that was asked for."));
    case engine::PasskeyFailure::noCommonAlgorithms:
        return L(QStringLiteral("Website und Schlüssel haben kein gemeinsames Verfahren."),
                 QStringLiteral("The site and the key share no algorithm."));
    case engine::PasskeyFailure::storageFull:
        return L(QStringLiteral("Auf dem Schlüssel ist kein Platz mehr."),
                 QStringLiteral("The key has no space left."));
    case engine::PasskeyFailure::userConsentDenied:
        return L(QStringLiteral("Die Anmeldung wurde abgebrochen."),
                 QStringLiteral("The sign-in was declined."));
    case engine::PasskeyFailure::windowsUserCancelled:
        return L(QStringLiteral("Die Anmeldung wurde im Systemdialog abgebrochen."),
                 QStringLiteral("The sign-in was cancelled in the system dialog."));
    case engine::PasskeyFailure::unknown:
        break;
    }
    return L(QStringLiteral("Die Anmeldung ist fehlgeschlagen."), QStringLiteral("The sign-in failed."));
}

void SpikeWindow::presentPasskeyStep(
    const engine::PasskeyRequest &request,
    const engine::PasskeyControls &controls
) {
    const QString site = QString::fromStdString(request.relyingPartyId);
    // A finished conversation closes the dialog rather than leaving it stale.
    if (request.stage == engine::PasskeyStage::completed
        || request.stage == engine::PasskeyStage::cancelled) {
        passkeyControls_ = {};
        if (passkeyDialog_) passkeyDialog_->close();
        status_->setText(request.stage == engine::PasskeyStage::completed
            ? L(QStringLiteral("Passkey-Anmeldung abgeschlossen."), QStringLiteral("Passkey sign-in finished."))
            : L(QStringLiteral("Passkey-Anmeldung abgebrochen."), QStringLiteral("Passkey sign-in cancelled.")));
        return;
    }

    passkeyControls_ = controls;
    if (!passkeyDialog_) {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("passkeyDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(L(QStringLiteral("Passkey"), QStringLiteral("Passkey")));
        dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()));
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(22, 20, 22, 18);
        layout->setSpacing(11);

        passkeyMessage_ = new QLabel(dialog);
        passkeyMessage_->setObjectName(QStringLiteral("passkeyMessage"));
        passkeyMessage_->setWordWrap(true);
        layout->addWidget(passkeyMessage_);

        passkeyAccounts_ = new QListWidget(dialog);
        passkeyAccounts_->setObjectName(QStringLiteral("passkeyAccountList"));
        layout->addWidget(passkeyAccounts_);

        passkeyPin_ = new QLineEdit(dialog);
        passkeyPin_->setObjectName(QStringLiteral("passkeyPinField"));
        // The PIN of a security key is a secret like any other.
        passkeyPin_->setEchoMode(QLineEdit::Password);
        layout->addWidget(passkeyPin_);

        auto *buttons = new QHBoxLayout();
        passkeyRetry_ = new QPushButton(L(QStringLiteral("Erneut versuchen"),
                                         QStringLiteral("Try again")), dialog);
        passkeyRetry_->setObjectName(QStringLiteral("passkeyRetryButton"));
        auto *cancel = new QPushButton(L(QStringLiteral("Abbrechen"), QStringLiteral("Cancel")), dialog);
        cancel->setObjectName(QStringLiteral("passkeyCancelButton"));
        passkeyContinue_ = new QPushButton(L(QStringLiteral("Weiter"), QStringLiteral("Continue")), dialog);
        passkeyContinue_->setObjectName(QStringLiteral("passkeyContinueButton"));
        passkeyContinue_->setDefault(true);
        buttons->addWidget(passkeyRetry_);
        buttons->addStretch();
        buttons->addWidget(cancel);
        buttons->addWidget(passkeyContinue_);
        layout->addLayout(buttons);

        QObject::connect(passkeyRetry_, &QPushButton::clicked, dialog, [this] {
            if (passkeyControls_.retry) passkeyControls_.retry();
        });
        QObject::connect(cancel, &QPushButton::clicked, dialog, [this] {
            if (passkeyControls_.cancel) passkeyControls_.cancel();
        });
        QObject::connect(passkeyContinue_, &QPushButton::clicked, dialog, [this] {
            if (passkeyPin_->isVisible()) {
                if (passkeyControls_.setPin) passkeyControls_.setPin(passkeyPin_->text().toStdString());
                passkeyPin_->clear();
                return;
            }
            if (QListWidgetItem *item = passkeyAccounts_->currentItem();
                item && passkeyControls_.selectAccount) {
                passkeyControls_.selectAccount(item->text().toStdString());
            }
        });
        // Closing the window is a decline, never a silent hang.
        QObject::connect(dialog, &QDialog::finished, this, [this](int) {
            if (passkeyControls_.cancel) passkeyControls_.cancel();
            passkeyControls_ = {};
            passkeyAccounts_ = nullptr;
            passkeyPin_ = nullptr;
            passkeyMessage_ = nullptr;
            passkeyContinue_ = nullptr;
            passkeyRetry_ = nullptr;
        });
        passkeyDialog_ = dialog;
    }

    passkeyAccounts_->setVisible(request.stage == engine::PasskeyStage::selectAccount);
    passkeyPin_->setVisible(request.stage == engine::PasskeyStage::collectPin);
    passkeyRetry_->setVisible(request.stage == engine::PasskeyStage::requestFailed);
    passkeyContinue_->setVisible(request.stage == engine::PasskeyStage::selectAccount
                                || request.stage == engine::PasskeyStage::collectPin);

    switch (request.stage) {
    case engine::PasskeyStage::selectAccount: {
        passkeyAccounts_->clear();
        for (const std::string &name : request.userNames)
            passkeyAccounts_->addItem(QString::fromStdString(name));
        if (passkeyAccounts_->count() > 0) passkeyAccounts_->setCurrentRow(0);
        passkeyMessage_->setText(L(QStringLiteral("Konto für die Anmeldung bei %1 wählen."),
                                   QStringLiteral("Choose the account to sign in to %1.")).arg(site));
        break;
    }
    case engine::PasskeyStage::collectPin: {
        QStringList lines;
        switch (request.pinReason) {
        case engine::PasskeyPinReason::set:
            lines.append(L(QStringLiteral("Neue PIN für den Sicherheitsschlüssel festlegen."),
                           QStringLiteral("Set a new PIN for the security key.")));
            break;
        case engine::PasskeyPinReason::change:
            lines.append(L(QStringLiteral("PIN des Sicherheitsschlüssels ändern."),
                           QStringLiteral("Change the security key's PIN.")));
            break;
        case engine::PasskeyPinReason::challenge:
            lines.append(L(QStringLiteral("PIN des Sicherheitsschlüssels eingeben."),
                           QStringLiteral("Enter the security key's PIN.")));
            break;
        }
        switch (request.pinError) {
        case engine::PasskeyPinError::wrongPin:
            lines.append(L(QStringLiteral("Die PIN war falsch."), QStringLiteral("The PIN was wrong.")));
            break;
        case engine::PasskeyPinError::tooShort:
            lines.append(L(QStringLiteral("Die PIN ist zu kurz."), QStringLiteral("The PIN is too short.")));
            break;
        case engine::PasskeyPinError::invalidCharacters:
            lines.append(L(QStringLiteral("Die PIN enthält unerlaubte Zeichen."),
                           QStringLiteral("The PIN contains characters that are not allowed.")));
            break;
        case engine::PasskeyPinError::sameAsCurrentPin:
            lines.append(L(QStringLiteral("Die neue PIN darf nicht die alte sein."),
                           QStringLiteral("The new PIN must not be the old one.")));
            break;
        case engine::PasskeyPinError::userVerificationLocked:
            lines.append(L(QStringLiteral("Die Überprüfung am Schlüssel ist gesperrt."),
                           QStringLiteral("Verification on the key is locked.")));
            break;
        case engine::PasskeyPinError::none:
            break;
        }
        if (request.minimumPinLength > 0) {
            lines.append(L(QStringLiteral("Mindestens %1 Zeichen."), QStringLiteral("At least %1 characters."))
                             .arg(request.minimumPinLength));
        }
        // How many tries are left decides whether a key gets locked, so it is
        // said rather than left to guesswork.
        if (request.remainingAttempts > 0) {
            lines.append(L(QStringLiteral("Verbleibende Versuche: %1"),
                           QStringLiteral("Attempts left: %1")).arg(request.remainingAttempts));
        }
        passkeyMessage_->setText(lines.join(QStringLiteral("\n")));
        passkeyPin_->clear();
        passkeyPin_->setFocus();
        break;
    }
    case engine::PasskeyStage::finishTokenCollection:
        passkeyMessage_->setText(L(
            QStringLiteral("Sicherheitsschlüssel berühren, um die Anmeldung bei %1 zu bestätigen."),
            QStringLiteral("Touch the security key to confirm signing in to %1.")
        ).arg(site));
        break;
    case engine::PasskeyStage::requestFailed:
        passkeyMessage_->setText(passkeyFailureText(request.failure));
        break;
    case engine::PasskeyStage::notStarted:
        passkeyMessage_->setText(L(QStringLiteral("Anmeldung bei %1 wird vorbereitet …"),
                                   QStringLiteral("Preparing the sign-in to %1 …")).arg(site));
        break;
    case engine::PasskeyStage::completed:
    case engine::PasskeyStage::cancelled:
        break;
    }

    passkeyDialog_->show();
    passkeyDialog_->raise();
}

} // namespace yobro::spike

// MARK: - Screen sharing
//
// The WebKit build gets a system picker from WebKit itself and needs no code for
// this. Qt hands over two lists and expects one answer, so the picker is ours.
namespace yobro::spike {

void SpikeWindow::presentDesktopMediaPicker(
    const engine::DesktopMediaRequest &request,
    const engine::DesktopMediaControls &controls
) {
    const auto refuse = [&controls] {
        if (controls.cancel) controls.cancel();
    };
    if (request.sources.empty()) {
        // Nothing to offer. On macOS this is what an denied screen-recording
        // permission for the app itself looks like, so it is worth saying.
        status_->setText(L(
            QStringLiteral("Keine Bildschirme oder Fenster verfügbar. Prüfe die Bildschirmaufnahme-Berechtigung des Systems."),
            QStringLiteral("No screens or windows available. Check the system's screen-recording permission.")
        ));
        refuse();
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("desktopMediaDialog"));
    dialog.setWindowTitle(L(QStringLiteral("Bildschirm teilen"), QStringLiteral("Share your screen")));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.resize(460, 420);
    dialog.setStyleSheet(sheetStyleSheet(systemPrefersDark()));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(11);
    const QString host = request.origin.empty()
        ? L(QStringLiteral("Diese Seite"), QStringLiteral("This page"))
        : QUrl(QString::fromStdString(request.origin)).host();
    auto *heading = new QLabel(
        L(QStringLiteral("%1 möchte etwas von deinem Bildschirm sehen."),
          QStringLiteral("%1 wants to see something from your screen.")).arg(
            host.isEmpty() ? QString::fromStdString(request.origin) : host),
        &dialog
    );
    heading->setWordWrap(true);
    heading->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px"));
    layout->addWidget(heading);
    auto *note = new QLabel(
        L(QStringLiteral("Wähle genau eine Fläche. Alles, was darauf zu sehen ist, geht an die Website, "
                         "auch Fenster, die du später davor legst. Die Freigabe gilt nur für diesen Seitenaufruf."),
          QStringLiteral("Pick exactly one surface. Everything visible on it goes to the website, including "
                         "windows you put in front of it later. Sharing applies to this page visit only.")),
        &dialog
    );
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("desktopMediaNote"));
    note->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(note);
    auto *list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("desktopMediaList"));
    for (const engine::DesktopMediaSource &source : request.sources) {
        const QString name = source.name.empty()
            ? L(QStringLiteral("Ohne Namen"), QStringLiteral("Unnamed"))
            : QString::fromStdString(source.name);
        auto *item = new QListWidgetItem(
            (source.window
                 ? L(QStringLiteral("Fenster: %1"), QStringLiteral("Window: %1"))
                 : L(QStringLiteral("Bildschirm: %1"), QStringLiteral("Screen: %1"))).arg(name),
            list
        );
        item->setData(Qt::UserRole, source.window);
        item->setData(Qt::UserRole + 1, source.index);
    }
    list->setCurrentRow(0);
    layout->addWidget(list, 1);
    auto *actions = new QHBoxLayout();
    auto *cancel = new QPushButton(L(QStringLiteral("Nicht teilen"), QStringLiteral("Don't share")), &dialog);
    cancel->setObjectName(QStringLiteral("desktopMediaCancelButton"));
    auto *share = new QPushButton(L(QStringLiteral("Teilen"), QStringLiteral("Share")), &dialog);
    share->setObjectName(QStringLiteral("desktopMediaShareButton"));
    // Sharing is never the default answer; the safe button is.
    cancel->setDefault(true);
    actions->addWidget(cancel);
    actions->addStretch();
    actions->addWidget(share);
    layout->addLayout(actions);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(share, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem *) {
        dialog.accept();
    });
    // Modal on purpose: the engine cancels a request that is not answered by the
    // time this returns.
    const int outcome = dialog.exec();
    QListWidgetItem *chosen = list->currentItem();
    if (outcome != QDialog::Accepted || !chosen) {
        refuse();
        status_->setText(L(QStringLiteral("Bildschirm wird nicht geteilt."), QStringLiteral("Not sharing your screen.")));
        return;
    }
    const bool window = chosen->data(Qt::UserRole).toBool();
    const int index = chosen->data(Qt::UserRole + 1).toInt();
    if (!controls.select || !controls.select(window, index)) {
        // The list is a snapshot; a window can be gone by the time we answer.
        refuse();
        status_->setText(L(
            QStringLiteral("Die gewählte Fläche ist nicht mehr verfügbar."),
            QStringLiteral("The chosen surface is no longer available.")
        ));
        return;
    }
    status_->setText(window
        ? L(QStringLiteral("Ein Fenster wird geteilt."), QStringLiteral("Sharing a window."))
        : L(QStringLiteral("Ein Bildschirm wird geteilt."), QStringLiteral("Sharing a screen.")));
}

} // namespace yobro::spike
