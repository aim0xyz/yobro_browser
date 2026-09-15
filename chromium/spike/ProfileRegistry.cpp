#include "spike/ProfileRegistry.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <system_error>
#include <utility>

namespace yobro::spike {
namespace {

constexpr int maximumNameLength = 60;
constexpr int maximumIconLength = 8;

QString trimmed(const std::string &value) {
    return QString::fromStdString(value).trimmed();
}

} // namespace

ProfileRegistry::ProfileRegistry(std::filesystem::path root)
    : file_(root / "profiles.json"), profiles_(root / "Profiles") {
    load();
}

std::string ProfileRegistry::defaultName(const std::string &id) {
    // The first profile is the personal one, like in the WebKit build.
    return id == "default" ? "Persönlich" : id;
}

void ProfileRegistry::load() {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
    for (const QJsonValue &value : root.value(QStringLiteral("profiles")).toArray()) {
        const QJsonObject record = value.toObject();
        const QString id = record.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        const QString name = record.value(QStringLiteral("name")).toString().trimmed();
        const QString icon = record.value(QStringLiteral("icon")).toString();
        entries_.push_back({
            id.toStdString(),
            name.isEmpty() || name.size() > maximumNameLength
                ? defaultName(id.toStdString())
                : name.toStdString(),
            icon.isEmpty() || icon.size() > maximumIconLength ? defaultIcon : icon.toStdString(),
        });
    }
}

std::vector<ProfileEntry> ProfileRegistry::entries() const {
    std::vector<ProfileEntry> result = entries_;
    // A profile directory without a registry record still has to appear.
    std::error_code code;
    for (const auto &item : std::filesystem::directory_iterator(profiles_, code)) {
        if (code) break;
        if (!item.is_directory()) continue;
        const std::string id = item.path().filename().string();
        const bool listed = std::any_of(result.begin(), result.end(), [&id](const ProfileEntry &entry) {
            return entry.id == id;
        });
        if (!listed) result.push_back({id, defaultName(id), defaultIcon});
    }
    std::sort(result.begin(), result.end(), [](const ProfileEntry &left, const ProfileEntry &right) {
        return left.name == right.name ? left.id < right.id : left.name < right.name;
    });
    return result;
}

std::optional<ProfileEntry> ProfileRegistry::known(const std::string &id) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&id](const ProfileEntry &entry) {
        return entry.id == id;
    });
    if (found == entries_.end()) return std::nullopt;
    return *found;
}

ProfileEntry ProfileRegistry::entry(const std::string &id) const {
    if (const std::optional<ProfileEntry> found = known(id)) return *found;
    return {id, defaultName(id), defaultIcon};
}

bool ProfileRegistry::add(const std::string &id, const std::string &name, const std::string &icon) {
    if (id.empty() || known(id)) return false;
    const QString candidate = trimmed(name);
    entries_.push_back({
        id,
        candidate.isEmpty() || candidate.size() > maximumNameLength ? defaultName(id) : candidate.toStdString(),
        icon.empty() || QString::fromStdString(icon).size() > maximumIconLength ? defaultIcon : icon,
    });
    save();
    return true;
}

bool ProfileRegistry::setName(const std::string &id, const std::string &name) {
    const QString candidate = trimmed(name);
    if (id.empty() || candidate.isEmpty() || candidate.size() > maximumNameLength) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&id](const ProfileEntry &entry) {
        return entry.id == id;
    });
    if (found == entries_.end()) entries_.push_back({id, candidate.toStdString(), defaultIcon});
    else found->name = candidate.toStdString();
    save();
    return true;
}

bool ProfileRegistry::setIcon(const std::string &id, const std::string &icon) {
    const QString candidate = QString::fromStdString(icon).trimmed();
    if (id.empty() || candidate.isEmpty() || candidate.size() > maximumIconLength) return false;
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&id](const ProfileEntry &entry) {
        return entry.id == id;
    });
    if (found == entries_.end()) entries_.push_back({id, defaultName(id), candidate.toStdString()});
    else found->icon = candidate.toStdString();
    save();
    return true;
}

void ProfileRegistry::save() const {
    QJsonArray profiles;
    for (const ProfileEntry &entry : entries_) {
        QJsonObject record;
        record.insert(QStringLiteral("id"), QString::fromStdString(entry.id));
        record.insert(QStringLiteral("name"), QString::fromStdString(entry.name));
        record.insert(QStringLiteral("icon"), QString::fromStdString(entry.icon));
        profiles.append(record);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("profiles"), profiles);
    // Without the directory the write would fail silently and lose the name.
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

} // namespace yobro::spike
