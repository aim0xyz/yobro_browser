#include "engine/qtwebengine/QtBrowserProfile.hpp"

#include "engine/qtwebengine/QtBrowserPage.hpp"

#include <QDir>
#include <QEventLoop>
#include <QStandardPaths>
#include <QSettings>
#include <QString>
#include <QTimer>
#include <QWebEngineExtensionInfo>
#include <QWebEngineExtensionManager>
#include <QWebEnginePermission>
#include <QWebEngineProfile>

#include <stdexcept>
#include <utility>

namespace yobro::qtwebengine {
namespace {

QString extensionStatePath(const engine::ProfileSpec &spec) {
    return QDir(QString::fromStdString(spec.storagePath)).filePath(
        QStringLiteral("yobro-extension-state.ini")
    );
}

void configureEphemeralPermissions(QWebEngineProfile &profile) {
    const QList<QWebEnginePermission> legacyPermissions = profile.listAllPermissions();
    for (const QWebEnginePermission &permission : legacyPermissions)
        permission.reset();

    if (!legacyPermissions.isEmpty()) {
        // Under StoreOnDisk, reset() is persisted asynchronously. Switching
        // policy immediately can cancel that write and resurrect the grant if
        // a later build ever changes policy again. Flush the one-time legacy
        // migration before enforcing AskEveryTime for this live profile.
        QEventLoop flushLoop;
        QTimer::singleShot(250, &flushLoop, &QEventLoop::quit);
        flushLoop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    profile.setPersistentPermissionsPolicy(
        QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime
    );
}

} // namespace

QtBrowserProfile::QtBrowserProfile(engine::ProfileSpec spec) : spec_(std::move(spec)) {
    if (spec_.id.empty() || spec_.storagePath.empty() || spec_.cachePath.empty())
        throw std::invalid_argument("A Chromium profile requires an id, storage path, and cache path.");

    QDir().mkpath(QString::fromStdString(spec_.storagePath));
    QDir().mkpath(QString::fromStdString(spec_.cachePath));

    if (spec_.persistent) {
        persistentProfile_ = std::make_unique<QWebEngineProfile>(QString::fromStdString(spec_.id));
        persistentProfile_->setPersistentStoragePath(QString::fromStdString(spec_.storagePath));
        persistentProfile_->setCachePath(QString::fromStdString(spec_.cachePath));
        persistentProfile_->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
        persistentProfile_->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    } else {
        persistentProfile_ = std::make_unique<QWebEngineProfile>();
    }
    configureEphemeralPermissions(*persistentProfile_);
    persistentProfile_->setDownloadPath(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));

    // An unnamed profile is off-the-record by Qt contract and cannot share
    // cookies, cache, permissions, or installed extensions with the user profile.
    privateProfile_ = std::make_unique<QWebEngineProfile>();
    privateProfile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    privateProfile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    configureEphemeralPermissions(*privateProfile_);
    privateProfile_->setDownloadPath(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
}

QtBrowserProfile::~QtBrowserProfile() = default;

const engine::ProfileSpec &QtBrowserProfile::spec() const {
    return spec_;
}

std::unique_ptr<engine::BrowserPage> QtBrowserProfile::createPage(
    std::string id,
    engine::PageOwner owner,
    bool privatePage
) {
    QWebEngineProfile *selectedProfile = privatePage ? privateProfile_.get() : persistentProfile_.get();
    if (owner == engine::PageOwner::agent && selectedProfile->isOffTheRecord())
        throw std::invalid_argument("Agent-owned pages may not use off-the-record browsing contexts.");
    return std::make_unique<QtBrowserPage>(
        selectedProfile,
        std::move(id),
        owner
    );
}

QWebEngineProfile *QtBrowserProfile::persistentProfile() const {
    return persistentProfile_.get();
}

QWebEngineProfile *QtBrowserProfile::privateProfile() const {
    return privateProfile_.get();
}

void QtBrowserProfile::restoreExtensionState(const QWebEngineExtensionInfo &extension) {
    if (!extension.isInstalled() || extension.id().isEmpty()) return;
    QSettings settings(extensionStatePath(spec_), QSettings::IniFormat);
    const QStringList enabled = settings.value(QStringLiteral("enabledIds")).toStringList();
    applyExtensionEnabled(extension, enabled.contains(extension.id()));
}

bool QtBrowserProfile::extensionSwitchingWorks() {
    // Qt 6.11.2's QWebEngineExtensionManager::setExtensionEnabled segfaults on
    // every call, at any point after the install has finished. Verified with an
    // isolated probe at delays of 0, 200, 600, 1200 and 2500 ms: the function
    // never returns. Calling it would take the whole browser down, so the switch
    // is not attempted at all until a Qt version fixes it. Flip this to true to
    // re-test, and run chromium-extension-mv3.
    return false;
}

bool QtBrowserProfile::passkeysWork() {
    // Qt 6.11.2 declares the whole WebAuthn UX API and links it in, but a page
    // calling navigator.credentials.get() or .create() with a publicKey option
    // gets a promise that never settles: no webAuthUxRequested signal is raised,
    // no rejection arrives, and the site's own timeout is ignored. Verified with
    // an isolated probe over 14 seconds, offscreen and with a real window, for
    // both create() and get(), including an explicit 6-second page timeout.
    //
    // The handling for the real conversation is written and wired, so it takes
    // over the moment this becomes true. Flip it to re-test, and run
    // chromium-passkeys.
    return false;
}

void QtBrowserProfile::applyExtensionEnabled(
    const QWebEngineExtensionInfo &extension,
    bool enabled
) {
    if (!extensionSwitchingWorks()) return;
    // The info object is re-resolved because the one delivered by installFinished
    // can refer to a state the backend has already replaced, and a switch to the
    // state an extension already has is skipped.
    auto *manager = persistentProfile_->extensionManager();
    for (const QWebEngineExtensionInfo &current : manager->extensions()) {
        if (current.id() != extension.id()) continue;
        if (current.isEnabled() == enabled) return;
        manager->setExtensionEnabled(current, enabled);
        return;
    }
}

void QtBrowserProfile::loadInstalledExtensions() {
    if (!spec_.persistent) return;
    auto *manager = persistentProfile_->extensionManager();
    const QDir root(manager->installPath());
    for (const QFileInfo &entry : root.entryInfoList(
             QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name
         )) {
        bool loaded = false;
        for (const auto &extension : manager->extensions()) {
            if (QFileInfo(extension.path()).canonicalFilePath() == entry.canonicalFilePath()) {
                loaded = true;
                break;
            }
        }
        if (!loaded) manager->loadExtension(entry.absoluteFilePath());
    }
}

void QtBrowserProfile::setExtensionEnabled(
    const QWebEngineExtensionInfo &extension,
    bool enabled
) {
    if (!extension.isInstalled() || extension.id().isEmpty()) return;
    applyExtensionEnabled(extension, enabled);
    QSettings settings(extensionStatePath(spec_), QSettings::IniFormat);
    QStringList ids = settings.value(QStringLiteral("enabledIds")).toStringList();
    ids.removeAll(extension.id());
    if (enabled) ids.append(extension.id());
    ids.sort();
    settings.setValue(QStringLiteral("enabledIds"), ids);
    settings.sync();
}

void QtBrowserProfile::enableExtensionAfterInstall(const QWebEngineExtensionInfo &extension) {
    if (!extension.isInstalled() || extension.id().isEmpty()) return;
    // The persisted state is written at once, so a restart activates the
    // extension even if the deferred call below never runs.
    QSettings settings(extensionStatePath(spec_), QSettings::IniFormat);
    QStringList ids = settings.value(QStringLiteral("enabledIds")).toStringList();
    ids.removeAll(extension.id());
    ids.append(extension.id());
    ids.sort();
    settings.setValue(QStringLiteral("enabledIds"), ids);
    settings.sync();

    // The switch itself is deferred so the extension backend can settle first.
    // It only runs at all once Qt's setExtensionEnabled stops crashing; see
    // extensionSwitchingWorks().
    const QString id = extension.id();
    QTimer::singleShot(installSettleDelayMilliseconds, persistentProfile_.get(), [this, id] {
        for (const QWebEngineExtensionInfo &current : persistentProfile_->extensionManager()->extensions()) {
            if (current.id() != id) continue;
            applyExtensionEnabled(current, true);
            return;
        }
    });
}

void QtBrowserProfile::forgetExtensionState(const QWebEngineExtensionInfo &extension) {
    if (extension.id().isEmpty()) return;
    QSettings settings(extensionStatePath(spec_), QSettings::IniFormat);
    QStringList ids = settings.value(QStringLiteral("enabledIds")).toStringList();
    ids.removeAll(extension.id());
    settings.setValue(QStringLiteral("enabledIds"), ids);
    settings.sync();
}

std::unique_ptr<engine::BrowserProfile> QtBrowserEngine::openProfile(engine::ProfileSpec spec) {
    return std::make_unique<QtBrowserProfile>(std::move(spec));
}

} // namespace yobro::qtwebengine
