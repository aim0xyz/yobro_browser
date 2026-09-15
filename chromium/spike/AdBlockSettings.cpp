#include "spike/AdBlockSettings.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <system_error>
#include <utility>

namespace yobro::spike {

AdBlockSettings::AdBlockSettings(std::filesystem::path profileDirectory)
    : file_(std::move(profileDirectory) / "adblock.json") {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
    // A missing "strictProtection" means an older file; the WebKit build treats
    // that as strict, so protection is never silently weakened.
    if (root.contains(QStringLiteral("enabled")))
        enabled_ = root.value(QStringLiteral("enabled")).toBool(true);
    strict_ = root.value(QStringLiteral("strictProtection")).toBool(true);
}

void AdBlockSettings::setEnabled(bool value) {
    if (enabled_ == value) return;
    enabled_ = value;
    save();
}

void AdBlockSettings::setStrictProtection(bool value) {
    if (strict_ == value) return;
    strict_ = value;
    save();
}

void AdBlockSettings::save() const {
    QJsonObject root;
    root.insert(QStringLiteral("enabled"), enabled_);
    root.insert(QStringLiteral("strictProtection"), strict_);
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace yobro::spike
