#include "spike/WebAppearanceStore.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <system_error>
#include <utility>

namespace yobro::spike {
namespace {

std::string normalisedHost(const std::string &host) {
    return QString::fromStdString(host).toLower().toStdString();
}

} // namespace

WebAppearanceStore::WebAppearanceStore(std::filesystem::path profileDirectory)
    : file_(profileDirectory / "web-appearance.json") {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
    enabled_ = root.value(QStringLiteral("enabled")).toBool(false);
    for (const QJsonValue &value : root.value(QStringLiteral("excludedHosts")).toArray()) {
        const QString host = value.toString().toLower();
        // Keeps a hand-edited file from growing without bound.
        if (host.isEmpty() || host.size() > 255) continue;
        excludedHosts_.insert(host.toStdString());
    }
}

bool WebAppearanceStore::enabled() const {
    return enabled_;
}

void WebAppearanceStore::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    save();
}

bool WebAppearanceStore::isExcluded(const std::string &host) const {
    if (host.empty()) return false;
    return excludedHosts_.count(normalisedHost(host)) > 0;
}

void WebAppearanceStore::setExcluded(const std::string &host, bool excluded) {
    if (host.empty()) return;
    const std::string key = normalisedHost(host);
    const bool changed = excluded ? excludedHosts_.insert(key).second : excludedHosts_.erase(key) > 0;
    if (changed) save();
}

const std::set<std::string> &WebAppearanceStore::excludedHosts() const {
    return excludedHosts_;
}

std::string WebAppearanceStore::json(bool systemDark) const {
    QJsonArray hosts;
    for (const std::string &host : excludedHosts_) hosts.append(QString::fromStdString(host));
    QJsonObject root;
    root.insert(QStringLiteral("enabled"), enabled_);
    root.insert(QStringLiteral("excludedHosts"), hosts);
    root.insert(QStringLiteral("systemDark"), systemDark);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)).toStdString();
}

void WebAppearanceStore::save() const {
    QJsonArray hosts;
    for (const std::string &host : excludedHosts_) hosts.append(QString::fromStdString(host));
    QJsonObject root;
    root.insert(QStringLiteral("enabled"), enabled_);
    root.insert(QStringLiteral("excludedHosts"), hosts);
    // Without the directory the write would fail silently and lose the setting.
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace yobro::spike
