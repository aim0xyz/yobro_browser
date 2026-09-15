#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

void SpikeWindow::refreshProfileList() {
    if (!profileList_) return;
    profileList_->clear();
    const QString current = QString::fromStdString(paths_.profile.filename().string());
    for (const ProfileEntry &entry : profiles_.entries()) {
        const QString id = QString::fromStdString(entry.id);
        const bool active = id == current;
        // The storage identifier stays visible so a rename cannot hide which
        // directory a profile actually uses.
        QString label = QStringLiteral("%1  %2\n%3")
            .arg(QString::fromStdString(entry.icon), QString::fromStdString(entry.name), id);
        if (active)
            label += L(QStringLiteral("  ·  dieses Fenster"), QStringLiteral("  ·  this window"));
        auto *item = new QListWidgetItem(label, profileList_);
        item->setData(Qt::UserRole, id);
        if (active) item->setSelected(true);
    }
}

void SpikeWindow::updateProfileFooter() {
    const QString id = QString::fromStdString(paths_.profile.filename().string());
    if (profileFooter_) {
        profileFooter_->setText(
            profileLabel(id) + L(QStringLiteral("  ·  Einstellungen"), QStringLiteral("  ·  Settings"))
        );
    }
    if (auto *compact = findChild<QPushButton *>(QStringLiteral("compactProfileButton"))) {
        const ProfileEntry entry = profiles_.entry(id.toStdString());
        compact->setText(QString::fromStdString(entry.icon));
        compact->setToolTip(profileLabel(id) + QStringLiteral(" · ⌘,"));
    }
}

void SpikeWindow::setProfileSwitchHandler(ProfileSwitchHandler handler) {
    profileSwitchHandler_ = std::move(handler);
}

QString SpikeWindow::profileLabel(const QString &profileId) const {
    const ProfileEntry entry = profiles_.entry(profileId.toStdString());
    return QStringLiteral("%1  %2")
        .arg(QString::fromStdString(entry.icon), QString::fromStdString(entry.name));
}

void SpikeWindow::activateProfile(const QString &profileId) {
    if (profileId.isEmpty()) return;
    if (profileId == QString::fromStdString(paths_.profile.filename().string())) return;
    if (!profileSwitchHandler_) {
        // Without a host that can rebuild the session, a separate window is the
        // only honest option.
        (void)launchProfileInstance(profileId);
        return;
    }
    // The session is torn down by the host, so the current state is written out
    // before the switch rather than after it.
    saveSession();
    if (settingsDialog_) settingsDialog_->close();
    showStatus(L(QStringLiteral("Profil wird gewechselt …"), QStringLiteral("Switching profile …")));
    profileSwitchHandler_(profileId.toStdString());
}

void SpikeWindow::renameProfile(const QString &profileId) {
    if (profileId.isEmpty()) return;
    const ProfileEntry entry = profiles_.entry(profileId.toStdString());
    bool accepted = false;
    const QString name = QInputDialog::getText(
        settingsDialog_ ? static_cast<QWidget *>(settingsDialog_) : this,
        L(QStringLiteral("Profil umbenennen"), QStringLiteral("Rename profile")),
        L(QStringLiteral("Name"), QStringLiteral("Name")),
        QLineEdit::Normal,
        QString::fromStdString(entry.name),
        &accepted
    ).trimmed();
    if (!accepted || name.isEmpty()) return;
    // Renaming only changes the label; the storage identifier stays.
    if (!profiles_.setName(profileId.toStdString(), name.toStdString())) {
        showStatus(L(QStringLiteral("Der Name konnte nicht gespeichert werden."),
                           QStringLiteral("The name could not be saved.")));
        return;
    }
    refreshProfileList();
    updateProfileFooter();
}

void SpikeWindow::setProfileIcon(const QString &profileId) {
    if (profileId.isEmpty()) return;
    const ProfileEntry entry = profiles_.entry(profileId.toStdString());
    bool accepted = false;
    const QString icon = QInputDialog::getText(
        settingsDialog_ ? static_cast<QWidget *>(settingsDialog_) : this,
        L(QStringLiteral("Profil-Symbol"), QStringLiteral("Profile icon")),
        L(QStringLiteral("Symbol"), QStringLiteral("Icon")),
        QLineEdit::Normal,
        QString::fromStdString(entry.icon),
        &accepted
    ).trimmed();
    if (!accepted || icon.isEmpty()) return;
    if (!profiles_.setIcon(profileId.toStdString(), icon.toStdString())) {
        showStatus(L(QStringLiteral("Das Symbol konnte nicht gespeichert werden."),
                           QStringLiteral("The icon could not be saved.")));
        return;
    }
    refreshProfileList();
    updateProfileFooter();
}

bool SpikeWindow::launchProfileInstance(const QString &profileId) {
    // Each profile owns its own storage and control socket, so a profile is
    // opened as a separate instance instead of being swapped in place.
    const bool started = QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--profile"), profileId}
    );
    showStatus(started
        ? QStringLiteral("Profil „%1“ wird in einem eigenen Fenster geöffnet.").arg(profileId)
        : QStringLiteral("Profil „%1“ konnte nicht gestartet werden.").arg(profileId));
    return started;
}

void SpikeWindow::createProfile() {
    bool accepted = false;
    const QString requested = QInputDialog::getText(
        this,
        L(QStringLiteral("Neues Profil")),
        L(QStringLiteral("Name (Buchstaben, Ziffern, - und _):")),
        QLineEdit::Normal,
        QString(),
        &accepted
    ).trimmed();
    if (!accepted || requested.isEmpty()) return;
    try {
        // ProfilePaths enforces the id charset and WebKit-isolation rules.
        const core::ProfilePaths created = core::ProfilePaths::forProfile(requested.toStdString());
        created.createDirectories();
        // The new profile starts with its id as display name until renamed.
        (void)profiles_.add(requested.toStdString(), requested.toStdString(), ProfileRegistry::defaultIcon);
        refreshProfileList();
        activateProfile(requested);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, L(QStringLiteral("Neues Profil")), QString::fromUtf8(error.what()));
    }
}

} // namespace yobro::spike
