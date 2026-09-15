#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

bool SpikeWindow::installExtensionFromStore(QWidget *parent, const QString &input) {
    const auto id = ChromeStore::identifier(input);
    if (!id) {
        status_->setText(L(
            QStringLiteral("Bitte den Link einer Erweiterung aus dem Chrome Web Store eingeben."),
            QStringLiteral("Please enter the link of an extension from the Chrome Web Store.")
        ));
        return false;
    }

    status_->setText(L(
        QStringLiteral("Paket wird vom Chrome Web Store geladen …"),
        QStringLiteral("Loading the package from the Chrome Web Store …")
    ));
    QGuiApplication::processEvents();
    QString problem;
    const QByteArray payload = ChromeStore::download(*id, problem);
    if (!problem.isEmpty()) {
        status_->setText(problem);
        QMessageBox::warning(parent, L(QStringLiteral("Erweiterungen")), problem);
        return false;
    }

    // Each package gets its own directory so a reinstall never mixes files of
    // two versions.
    const std::filesystem::path root = paths_.profile / "Store Extensions" / id->toStdString();
    std::error_code code;
    std::filesystem::remove_all(root, code);
    const ChromeStore::Package package =
        ChromeStore::acceptPackage(payload, QString::fromStdString(root.string()));
    if (!package.problem.isEmpty()) {
        status_->setText(package.problem);
        QMessageBox::warning(parent, L(QStringLiteral("Erweiterungen")), package.problem);
        return false;
    }

    QStringList details;
    details.append(L(QStringLiteral("Name: %1"), QStringLiteral("Name: %1")).arg(package.name));
    details.append(L(QStringLiteral("Version: %1"), QStringLiteral("Version: %1")).arg(package.version));
    details.append(L(QStringLiteral("Berechtigungen: %1"), QStringLiteral("Permissions: %1"))
                       .arg(package.permissions.isEmpty()
                                ? L(QStringLiteral("keine"), QStringLiteral("none"))
                                : package.permissions.join(QStringLiteral(", "))));
    if (!package.hosts.isEmpty()) {
        details.append(L(QStringLiteral("Websites: %1"), QStringLiteral("Websites: %1"))
                           .arg(package.hosts.join(QStringLiteral(", "))));
    }
    details.append(package.warnings);

    // The permissions decide what the extension may do, so they are shown before
    // anything is installed.
    const auto decision = QMessageBox::question(
        parent,
        L(QStringLiteral("Erweiterung installieren?"), QStringLiteral("Install extension?")),
        details.join(QStringLiteral("\n")),
        QMessageBox::Cancel | QMessageBox::Yes,
        QMessageBox::Cancel
    );
    if (decision != QMessageBox::Yes) {
        std::filesystem::remove_all(root, code);
        status_->setText(L(QStringLiteral("Installation abgebrochen."),
                           QStringLiteral("Installation cancelled.")));
        return false;
    }

    status_->setText(L(QStringLiteral("MV3-Erweiterung wird installiert…"),
                       QStringLiteral("Installing MV3 extension…")));
    profile_->persistentProfile()->extensionManager()->installExtension(package.directory);
    return true;
}

void SpikeWindow::showExtensions() {
    if (extensionsDialog_) {
        refreshExtensions();
        extensionsDialog_->show();
        extensionsDialog_->raise();
        extensionsDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("extensionsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Erweiterungen")));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->resize(600, 420);
    auto *layout = new QVBoxLayout(dialog);
    auto *description = new QLabel(
        L(QStringLiteral("Installierte Chromium-Erweiterungen dieses Nutzerprofils.")), dialog
    );
    layout->addWidget(description);
    if (!qtwebengine::QtBrowserProfile::extensionSwitchingWorks()) {
        auto *limitation = new QLabel(
            L(QStringLiteral("Diese Qt-Version kann Erweiterungen nicht aktivieren: der Aufruf dafür "
                             "stürzt ab. Erweiterungen lassen sich installieren und werden geladen, ihre "
                             "Skripte laufen aber noch nicht. Der Zustand wird gespeichert und greift, "
                             "sobald Qt das behebt."),
              QStringLiteral("This Qt version cannot activate extensions: the call for it crashes. "
                             "Extensions can be installed and are loaded, but their scripts do not run "
                             "yet. The state is stored and takes effect once Qt fixes this.")),
            dialog
        );
        limitation->setObjectName(QStringLiteral("extensionLimitationNote"));
        limitation->setWordWrap(true);
        limitation->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
        layout->addWidget(limitation);
    }
    extensionsList_ = new QListWidget(dialog);
    extensionsList_->setObjectName(QStringLiteral("extensionsList"));
    layout->addWidget(extensionsList_, 1);
    auto *storeRow = new QHBoxLayout();
    auto *storeField = new QLineEdit(dialog);
    storeField->setObjectName(QStringLiteral("extensionStoreField"));
    storeField->setPlaceholderText(
        L(QStringLiteral("Link oder ID aus dem Chrome Web Store"),
          QStringLiteral("Link or id from the Chrome Web Store")));
    auto *storeInstall = new QPushButton(
        L(QStringLiteral("Aus dem Store laden"), QStringLiteral("Load from the store")), dialog);
    storeInstall->setObjectName(QStringLiteral("extensionStoreInstallButton"));
    storeRow->addWidget(storeField, 1);
    storeRow->addWidget(storeInstall);
    layout->addLayout(storeRow);
    auto *storeHint = new QLabel(
        L(QStringLiteral("Der Store hat keine Installations-Schnittstelle; das Paket wird über Googles "
                         "Update-Dienst geladen und lokal entpackt. Vor der Installation siehst du die "
                         "geforderten Berechtigungen."),
          QStringLiteral("The store has no install interface; the package is fetched from Google's update "
                         "service and unpacked locally. You see the requested permissions before "
                         "installing.")),
        dialog
    );
    storeHint->setWordWrap(true);
    storeHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px").arg(themePalette(systemPrefersDark()).sheetMuted));
    layout->addWidget(storeHint);
    QObject::connect(storeInstall, &QPushButton::clicked, dialog, [this, dialog, storeField] {
        if (installExtensionFromStore(dialog, storeField->text())) storeField->clear();
    });

    auto *actions = new QHBoxLayout();
    auto *install = new QPushButton(L(QStringLiteral("Entpackte MV3 installieren")), dialog);
    install->setObjectName(QStringLiteral("extensionInstallButton"));
    toggleExtension_ = new QPushButton(L(QStringLiteral("Deaktivieren")), dialog);
    toggleExtension_->setObjectName(QStringLiteral("extensionToggleButton"));
    if (!qtwebengine::QtBrowserProfile::extensionSwitchingWorks()) {
        toggleExtension_->setEnabled(false);
        toggleExtension_->setToolTip(
            L(QStringLiteral("Mit dieser Qt-Version nicht möglich."),
              QStringLiteral("Not possible with this Qt version."))
        );
    }
    removeExtension_ = new QPushButton(L(QStringLiteral("Entfernen")), dialog);
    removeExtension_->setObjectName(QStringLiteral("extensionRemoveButton"));
    actions->addWidget(install);
    actions->addStretch();
    actions->addWidget(toggleExtension_);
    actions->addWidget(removeExtension_);
    layout->addLayout(actions);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);

    extensionsDialog_ = dialog;
    QObject::connect(extensionsList_, &QListWidget::itemSelectionChanged,
                     dialog, [this] { refreshExtensions(); });
    QObject::connect(install, &QPushButton::clicked, dialog,
                     [this] { installUnpackedExtension(); });
    QObject::connect(toggleExtension_, &QPushButton::clicked, dialog, [this] {
        QListWidgetItem *item = extensionsList_ ? extensionsList_->currentItem() : nullptr;
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        auto *manager = profile_->persistentProfile()->extensionManager();
        for (const auto &extension : manager->extensions()) {
            if (extension.id() != id) continue;
            profile_->setExtensionEnabled(extension, !extension.isEnabled());
            QTimer::singleShot(0, this, [this] { refreshExtensions(); });
            return;
        }
    });
    QObject::connect(removeExtension_, &QPushButton::clicked, dialog, [this] {
        QListWidgetItem *item = extensionsList_ ? extensionsList_->currentItem() : nullptr;
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        auto *manager = profile_->persistentProfile()->extensionManager();
        for (const auto &extension : manager->extensions()) {
            if (extension.id() != id || !extension.isInstalled()) continue;
            profile_->forgetExtensionState(extension);
            manager->uninstallExtension(extension);
            return;
        }
    });
    QObject::connect(dialog, &QObject::destroyed, this, [this] {
        extensionsList_ = nullptr;
        toggleExtension_ = nullptr;
        removeExtension_ = nullptr;
    });
    refreshExtensions();
    dialog->show();
}

void SpikeWindow::refreshExtensions() {
    if (!extensionsList_) return;
    const QString selected = extensionsList_->currentItem()
        ? extensionsList_->currentItem()->data(Qt::UserRole).toString() : QString();
    const QSignalBlocker blocker(extensionsList_);
    extensionsList_->clear();
    for (const auto &extension : profile_->persistentProfile()->extensionManager()->extensions()) {
        const QString name = extension.name().isEmpty() ? extension.id() : extension.name();
        const QString state = extension.isEnabled()
            ? L(QStringLiteral("Aktiv")) : L(QStringLiteral("Deaktiviert"));
        auto *item = new QListWidgetItem(name + QStringLiteral(" — ") + state, extensionsList_);
        item->setData(Qt::UserRole, extension.id());
        item->setToolTip(extension.description());
        if (extension.id() == selected)
            extensionsList_->setCurrentItem(item);
    }
    QListWidgetItem *item = extensionsList_->currentItem();
    const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    bool installed = false;
    bool enabled = false;
    for (const auto &extension : profile_->persistentProfile()->extensionManager()->extensions()) {
        if (extension.id() == id) {
            installed = extension.isInstalled();
            enabled = extension.isEnabled();
            break;
        }
    }
    toggleExtension_->setEnabled(item != nullptr);
    toggleExtension_->setText(enabled ? L(QStringLiteral("Deaktivieren")) : L(QStringLiteral("Aktivieren")));
    removeExtension_->setEnabled(installed);
}

} // namespace yobro::spike
