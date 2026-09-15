#include "spike/PrivacySettings.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <system_error>
#include <utility>
#include <vector>

namespace yobro::spike {
namespace {

constexpr auto sessionOnlyValue = "session";
constexpr auto keepValue = "keep";

} // namespace

PrivacySettings::PrivacySettings(std::filesystem::path profileDirectory)
    : file_(profileDirectory / "privacy.json"),
      marker_(profileDirectory / "pending-site-data-clear") {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
    retention_ = root.value(QStringLiteral("cookies")).toString() == QLatin1String(sessionOnlyValue)
        ? CookieRetention::sessionOnly
        : CookieRetention::keep;
}

CookieRetention PrivacySettings::cookieRetention() const {
    return retention_;
}

void PrivacySettings::setCookieRetention(CookieRetention retention) {
    if (retention_ == retention) return;
    retention_ = retention;
    save();
}

void PrivacySettings::requestSiteDataClear() {
    std::error_code code;
    std::filesystem::create_directories(marker_.parent_path(), code);
    QSaveFile file(QString::fromStdString(marker_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QByteArrayLiteral("1"));
    file.commit();
}

bool PrivacySettings::siteDataClearRequested() const {
    std::error_code code;
    return std::filesystem::exists(marker_, code);
}

const std::vector<std::string> &PrivacySettings::siteDataDirectories() {
    // Chromium's own layout inside the profile's storage directory. Saved
    // passwords live in the macOS Keychain and are deliberately untouched.
    static const std::vector<std::string> directories{
        "Local Storage",
        "Session Storage",
        "IndexedDB",
        "Service Worker",
        "databases",
        "File System",
        "blob_storage",
        "shared_proto_db",
        "WebStorage",
        "Code Cache",
        "GPUCache",
    };
    return directories;
}

std::size_t PrivacySettings::applyPendingSiteDataClear(const std::filesystem::path &profileDirectory) {
    const std::filesystem::path marker = profileDirectory / "pending-site-data-clear";
    std::error_code code;
    if (!std::filesystem::exists(marker, code)) return 0;
    std::size_t removed = 0;
    const std::filesystem::path storage = profileDirectory / "Chromium";
    for (const std::string &name : siteDataDirectories()) {
        const std::filesystem::path target = storage / name;
        if (!std::filesystem::exists(target, code)) continue;
        if (std::filesystem::remove_all(target, code) > 0 && !code) ++removed;
    }
    // The cache directory sits next to the storage directory.
    const std::filesystem::path cache = profileDirectory / "Cache";
    if (std::filesystem::exists(cache, code) && std::filesystem::remove_all(cache, code) > 0) ++removed;
    std::filesystem::remove(marker, code);
    return removed;
}

void PrivacySettings::save() const {
    QJsonObject root;
    root.insert(
        QStringLiteral("cookies"),
        QString::fromLatin1(retention_ == CookieRetention::sessionOnly ? sessionOnlyValue : keepValue)
    );
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace yobro::spike
