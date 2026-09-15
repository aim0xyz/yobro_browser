#include "spike/PageZoomStore.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <system_error>
#include <utility>

namespace yobro::spike {

const std::vector<double> &PageZoomStore::levels() {
    static const std::vector<double> values{
        0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0
    };
    return values;
}

PageZoomStore::PageZoomStore(std::filesystem::path profileDirectory)
    : file_(profileDirectory / "page-zoom.json") {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject saved = QJsonDocument::fromJson(input.readAll()).object();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        const double value = it.value().toDouble();
        // Ignore levels outside the ladder so a hand-edited file cannot apply
        // an arbitrary scale.
        if (std::find(levels().begin(), levels().end(), value) == levels().end()) continue;
        if (it.key().isEmpty()) continue;
        levelsByHost_.emplace(it.key().toStdString(), value);
    }
}

std::optional<std::string> PageZoomStore::key(const std::string &url) {
    const QUrl parsed(QString::fromStdString(url));
    QString host = parsed.host().toLower();
    if (host.isEmpty()) return std::nullopt;
    if (host.startsWith(QStringLiteral("www."))) host = host.mid(4);
    if (host.isEmpty()) return std::nullopt;
    return host.toStdString();
}

double PageZoomStore::level(const std::string &url) const {
    const std::optional<std::string> host = key(url);
    if (!host) return standard;
    const auto found = levelsByHost_.find(*host);
    return found == levelsByHost_.end() ? standard : found->second;
}

std::optional<double> PageZoomStore::step(int direction, const std::string &url) {
    const std::optional<std::string> host = key(url);
    if (!host || direction == 0) return std::nullopt;
    const double current = level(url);
    auto position = std::find(levels().begin(), levels().end(), current);
    if (position == levels().end())
        position = std::find(levels().begin(), levels().end(), standard);
    const auto index = static_cast<int>(std::distance(levels().begin(), position));
    const int next = std::clamp(index + direction, 0, static_cast<int>(levels().size()) - 1);
    store(levels()[static_cast<std::size_t>(next)], *host);
    return levels()[static_cast<std::size_t>(next)];
}

std::optional<double> PageZoomStore::reset(const std::string &url) {
    const std::optional<std::string> host = key(url);
    if (!host) return std::nullopt;
    store(standard, *host);
    return standard;
}

void PageZoomStore::store(double value, const std::string &host) {
    // The default needs no entry; dropping it keeps the file small.
    if (value == standard) levelsByHost_.erase(host);
    else levelsByHost_[host] = value;
    save();
}

void PageZoomStore::save() const {
    QJsonObject root;
    for (const auto &[host, value] : levelsByHost_)
        root.insert(QString::fromStdString(host), value);
    // Without the directory the write would fail silently and lose the level.
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace yobro::spike
