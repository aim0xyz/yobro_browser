#include "engine/qtwebengine/QtBrowserLibrary.hpp"

#include "engine/qtwebengine/QtBrowserPage.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtDownloadFilename.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace yobro::qtwebengine {
namespace {

constexpr std::size_t maximumRetiredExplicitPages = 8;
constexpr auto restartInterruption = "Durch Neustart unterbrochen.";
constexpr auto shutdownInterruption = "Browser closed before the download completed.";
constexpr auto requestEnded = "Chromium download request ended before completion.";

struct HistoryRecord {
    std::string id;
    std::string title;
    std::string url;
    std::string date;
    std::int64_t visits = 1;
};

struct BookmarkRecord {
    std::string id;
    std::string title;
    std::string url;
    std::string folder;
};

struct DownloadRecord {
    std::string id;
    std::string name;
    std::string source;
    std::string date;
    std::string state = "downloading";
    std::int64_t received = 0;
    std::int64_t expected = 0;
    std::optional<std::string> path;
    std::optional<std::string> error;
};

std::string uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower().toStdString();
}

std::string nowIso8601() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
}

const core::Json *member(const core::Json &value, std::string_view name) {
    return value.isObject() ? value.find(name) : nullptr;
}

std::string jsonString(const core::Json &value, std::string_view name, std::string fallback = {}) {
    const core::Json *field = member(value, name);
    return field && field->isString() ? field->asString() : std::move(fallback);
}

std::int64_t jsonInteger(const core::Json &value, std::string_view name, std::int64_t fallback = 0) {
    const core::Json *field = member(value, name);
    return field && field->isInteger() ? field->asInteger() : fallback;
}

std::optional<std::string> jsonOptionalString(const core::Json &value, std::string_view name) {
    const core::Json *field = member(value, name);
    if (!field || field->isNull()) return std::nullopt;
    return field->isString() ? std::optional<std::string>(field->asString()) : std::nullopt;
}

core::Json historyJson(const HistoryRecord &entry) {
    core::Json::Object value;
    value.emplace("id", core::Json(entry.id));
    value.emplace("title", core::Json(entry.title));
    value.emplace("url", core::Json(entry.url));
    value.emplace("date", core::Json(entry.date));
    value.emplace("visits", core::Json(entry.visits));
    return core::Json(std::move(value));
}

core::Json bookmarkJson(const BookmarkRecord &entry) {
    core::Json::Object value;
    value.emplace("id", core::Json(entry.id));
    value.emplace("title", core::Json(entry.title));
    value.emplace("url", core::Json(entry.url));
    value.emplace("folder", core::Json(entry.folder));
    return core::Json(std::move(value));
}

core::Json downloadJson(const DownloadRecord &entry) {
    core::Json::Object value;
    value.emplace("id", core::Json(entry.id));
    value.emplace("name", core::Json(entry.name));
    value.emplace("source", core::Json(entry.source));
    value.emplace("date", core::Json(entry.date));
    value.emplace("state", core::Json(entry.state));
    value.emplace("received", core::Json(entry.received));
    value.emplace("expected", core::Json(entry.expected));
    value.emplace("path", entry.path ? core::Json(*entry.path) : core::Json(nullptr));
    value.emplace("error", entry.error ? core::Json(*entry.error) : core::Json(nullptr));
    return core::Json(std::move(value));
}

std::optional<core::Json> readJson(const std::filesystem::path &path, bool &corrupt) {
    corrupt = false;
    const QString encoded = QString::fromStdString(path.string());
    if (!QFileInfo::exists(encoded)) return std::nullopt;
    QFile file(encoded);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read persisted Chromium library state: " + path.string());
    const QByteArray bytes = file.readAll();
    try {
        return core::Json::parse(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            {.maxBytes = 8 * 1'048'576, .maxDepth = 32, .rejectDuplicateKeys = true}
        );
    } catch (const std::exception &) {
        corrupt = true;
        return std::nullopt;
    }
}

void quarantine(const std::filesystem::path &path) {
    const QString source = QString::fromStdString(path.string());
    if (!QFileInfo::exists(source)) return;
    const QString suffix = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
    const QString destination = source + QStringLiteral(".corrupt-") + suffix
        + QStringLiteral("-") + QString::fromStdString(uuid());
    QFile file(source);
    if (!file.rename(destination))
        throw std::runtime_error("Could not quarantine corrupt Chromium library state: " + path.string());
}

bool writeJson(const std::filesystem::path &path, const core::Json &value) {
    QSaveFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const std::string encoded = value.serialize();
    if (file.write(encoded.data(), static_cast<qint64>(encoded.size())) != static_cast<qint64>(encoded.size())) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

std::string sanitizedHistoryUrl(std::string_view value) {
    QUrl url(QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
    const QString scheme = url.scheme().toLower();
    if ((scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) || url.host().isEmpty())
        return {};
    url.setFragment({});
    const QString host = url.host().toLower();
    if (host == QStringLiteral("accounts.google.com")
        || url.path().contains(QStringLiteral("signin_prompt"), Qt::CaseInsensitive)) {
        url.setPath(QStringLiteral("/"));
        url.setQuery({});
    }
    return url.toString().toStdString();
}

QString uniqueFilename(
    const QString &directory,
    const QString &suggested,
    const std::vector<DownloadRecord> &entries,
    const std::vector<QString> &reservations
) {
    const QString safe = sanitizedDownloadFilename(suggested);
    const QFileInfo info(safe);
    const QString stem = info.completeBaseName().isEmpty() ? QStringLiteral("Download") : info.completeBaseName();
    const QString suffix = info.completeSuffix();
    QString candidate = safe;
    int number = 1;
    auto isReserved = [&entries, &reservations](const QString &path) {
        const bool persisted = std::any_of(entries.begin(), entries.end(), [&path](const DownloadRecord &entry) {
            return entry.path && QDir::cleanPath(QString::fromStdString(*entry.path)) == QDir::cleanPath(path);
        });
        return persisted || std::any_of(reservations.begin(), reservations.end(), [&path](const QString &reserved) {
            return QDir::cleanPath(reserved) == QDir::cleanPath(path);
        });
    };
    while (true) {
        const QString path = QDir(directory).filePath(candidate);
        if (!QFileInfo::exists(path) && !isReserved(path)) return candidate;
        candidate = QStringLiteral("%1 (%2)%3").arg(
            stem,
            QString::number(number++),
            suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix
        );
    }
}

bool isWithin(const std::filesystem::path &child, const std::filesystem::path &parent) {
    const auto normalizedChild = child.lexically_normal();
    const auto normalizedParent = parent.lexically_normal();
    auto childPart = normalizedChild.begin();
    for (auto parentPart = normalizedParent.begin(); parentPart != normalizedParent.end(); ++parentPart, ++childPart) {
        if (childPart == normalizedChild.end() || *childPart != *parentPart) return false;
    }
    return true;
}

} // namespace

struct QtBrowserLibrary::Impl {
    enum class Owner { user, privatePage, agent };

    struct ActiveDownload {
        QPointer<QWebEngineDownloadRequest> request;
        Owner owner = Owner::user;
        QString stagingDirectory;
        QString stagingFile;
        QString finalPath;
        std::unique_ptr<QWebEnginePage> explicitPage;
    };

    struct PendingDownload {
        std::uint64_t token = 0;
        std::unique_ptr<QWebEnginePage> page;
        QUrl url;
        DownloadCompletion completion;
    };

    struct RetiredExplicitPage {
        std::unique_ptr<QWebEnginePage> page;
        QUrl url;
    };

    Impl(
        QtBrowserProfile &configuredProfile,
        std::filesystem::path directory,
        std::filesystem::path configuredDownloadDirectory
    )
        : profile(configuredProfile),
          profileDirectory(std::filesystem::absolute(std::move(directory)).lexically_normal()),
          historyPath(profileDirectory / "history.json"),
          bookmarksPath(profileDirectory / "bookmarks.json"),
          downloadsPath(profileDirectory / "downloads.json"),
          stagingPath(profileDirectory / "download-staging"),
          stagingRoot(QString::fromStdString(stagingPath.string())),
          context(std::make_unique<QObject>()) {
        if (configuredDownloadDirectory.empty()) {
            const QString standardDownloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            if (standardDownloads.isEmpty())
                throw std::runtime_error("Chromium could not resolve the user download directory.");
            downloadDirectory = QDir(standardDownloads).filePath(QStringLiteral("YOBRO"));
        } else {
            const auto absoluteDownload = std::filesystem::absolute(configuredDownloadDirectory).lexically_normal();
            if (isWithin(absoluteDownload, stagingPath))
                throw std::runtime_error("The download directory cannot be inside Chromium's private staging directory.");
            downloadDirectory = QString::fromStdString(absoluteDownload.string());
        }
        if (!QDir().mkpath(QString::fromStdString(profileDirectory.string())))
            throw std::runtime_error("Chromium could not create the profile library directory.");
        if (!QDir().mkpath(downloadDirectory) || !QFileInfo(downloadDirectory).isDir())
            throw std::runtime_error("Chromium could not create the configured download directory.");
        resetStaging();
        load();
        QObject::connect(
            profile.persistentProfile(),
            &QWebEngineProfile::downloadRequested,
            context.get(),
            [this](QWebEngineDownloadRequest *request) { handleDownloadRequest(request); }
        );
        QObject::connect(
            profile.privateProfile(),
            &QWebEngineProfile::downloadRequested,
            context.get(),
            [this](QWebEngineDownloadRequest *request) { handleDownloadRequest(request); }
        );
    }

    void resetStaging() {
        QDir root(stagingRoot);
        if (root.exists() && !root.removeRecursively())
            throw std::runtime_error("Chromium could not clean its owned download staging directory.");
        if (!QDir().mkpath(stagingRoot)
            || !QFile::setPermissions(
                stagingRoot,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            )) {
            throw std::runtime_error("Chromium could not create its owner-only download staging directory.");
        }
    }

    void load() {
        bool historyCorrupt = false;
        const auto historyRoot = readJson(historyPath, historyCorrupt);
        if (historyRoot && !historyRoot->isArray()) historyCorrupt = true;
        if (historyCorrupt) {
            quarantine(historyPath);
        } else if (historyRoot) {
            for (const core::Json &value : historyRoot->asArray()) {
                if (!value.isObject()) continue;
                const std::string url = jsonString(value, "url");
                if (url.empty()) continue;
                historyEntries.push_back({
                    .id = jsonString(value, "id", uuid()),
                    .title = jsonString(value, "title"),
                    .url = url,
                    .date = jsonString(value, "date", nowIso8601()),
                    .visits = std::max<std::int64_t>(1, jsonInteger(value, "visits", 1)),
                });
            }
        }

        bool bookmarksCorrupt = false;
        const auto bookmarksRoot = readJson(bookmarksPath, bookmarksCorrupt);
        if (bookmarksRoot && !bookmarksRoot->isArray()) bookmarksCorrupt = true;
        if (bookmarksCorrupt) {
            quarantine(bookmarksPath);
        } else if (bookmarksRoot) {
            for (const core::Json &value : bookmarksRoot->asArray()) {
                if (!value.isObject()) continue;
                const std::string url = sanitizedHistoryUrl(jsonString(value, "url"));
                if (url.empty()) continue;
                const std::string folder = jsonString(value, "folder", "Bookmarks");
                const bool duplicate = std::any_of(bookmarkEntries.begin(), bookmarkEntries.end(), [&url, &folder](const BookmarkRecord &entry) {
                    return entry.url == url && entry.folder == folder;
                });
                if (!duplicate) bookmarkEntries.push_back({
                    .id = jsonString(value, "id", uuid()),
                    .title = jsonString(value, "title", url),
                    .url = url,
                    .folder = folder.empty() ? "Bookmarks" : folder,
                });
            }
        }

        bool downloadsCorrupt = false;
        const auto downloadsRoot = readJson(downloadsPath, downloadsCorrupt);
        if (downloadsRoot && !downloadsRoot->isArray()) downloadsCorrupt = true;
        if (downloadsCorrupt) {
            quarantine(downloadsPath);
        } else if (downloadsRoot) {
            for (const core::Json &value : downloadsRoot->asArray()) {
                if (!value.isObject()) continue;
                DownloadRecord entry{
                    .id = jsonString(value, "id", uuid()),
                    .name = jsonString(value, "name", "Download"),
                    .source = jsonString(value, "source"),
                    .date = jsonString(value, "date", nowIso8601()),
                    .state = jsonString(value, "state", "interrupted"),
                    .received = jsonInteger(value, "received"),
                    .expected = jsonInteger(value, "expected"),
                    .path = jsonOptionalString(value, "path"),
                    .error = jsonOptionalString(value, "error"),
                };
                if (entry.state == "downloading") {
                    entry.state = "interrupted";
                    entry.path.reset();
                    entry.error = restartInterruption;
                } else if (entry.state != "completed") {
                    entry.path.reset();
                }
                downloadEntries.push_back(std::move(entry));
            }
        }
        if (!persistHistory() || !persistBookmarks() || !persistDownloads())
            throw std::runtime_error("Chromium could not persist initialized library state.");
    }

    bool persistHistory() const {
        core::Json::Array values;
        values.reserve(historyEntries.size());
        for (const auto &entry : historyEntries) values.push_back(historyJson(entry));
        return writeJson(historyPath, core::Json(std::move(values)));
    }

    bool persistBookmarks() const {
        core::Json::Array values;
        values.reserve(bookmarkEntries.size());
        for (const auto &entry : bookmarkEntries) values.push_back(bookmarkJson(entry));
        return writeJson(bookmarksPath, core::Json(std::move(values)));
    }

    bool persistDownloads() const {
        core::Json::Array values;
        values.reserve(downloadEntries.size());
        for (const auto &entry : downloadEntries) values.push_back(downloadJson(entry));
        return writeJson(downloadsPath, core::Json(std::move(values)));
    }

    void notifyDownloads() {
        if (!persistDownloads())
            qWarning("Chromium download metadata could not be persisted.");
        if (!downloadsObserver) return;
        try {
            downloadsObserver();
        } catch (const std::exception &error) {
            qWarning("Chromium download observer failed: %s", error.what());
        } catch (...) {
            qWarning("Chromium download observer failed.");
        }
    }

    DownloadRecord *download(std::string_view id) {
        const auto found = std::find_if(downloadEntries.begin(), downloadEntries.end(), [id](const DownloadRecord &entry) {
            return entry.id == id;
        });
        return found == downloadEntries.end() ? nullptr : &*found;
    }

    std::vector<QString> reservations() const {
        std::vector<QString> result;
        result.reserve(active.size());
        for (const auto &[id, item] : active) {
            (void)id;
            result.push_back(item.finalPath);
        }
        return result;
    }

    bool prepareStaging(const std::string &id, QString &directory, QString &file) const {
        directory = QDir(stagingRoot).filePath(QString::fromStdString(id));
        if (!QDir().mkpath(directory)) return false;
        if (!QFile::setPermissions(
                directory,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
            )) return false;
        file = QDir(directory).filePath(QStringLiteral("payload.yobro-part"));
        return true;
    }

    void cleanupStaging(const ActiveDownload &item) const {
        if (item.stagingDirectory.isEmpty()) return;
        QDir directory(item.stagingDirectory);
        if (directory.exists() && !directory.removeRecursively())
            qWarning("Chromium could not immediately remove owned staging path: %s", qPrintable(item.stagingDirectory));
    }

    bool publish(const ActiveDownload &item, std::string &problem) const {
        if (!QFileInfo::exists(item.stagingFile)) {
            problem = "Chromium completed the download without an owned staging file.";
            return false;
        }
        if (!QDir().mkpath(downloadDirectory)) {
            problem = "Chromium could not recreate the configured download directory.";
            return false;
        }
        if (QFileInfo::exists(item.finalPath)) {
            problem = "A file already exists at the selected download path.";
            return false;
        }

        QFile source(item.stagingFile);
        if (source.rename(item.finalPath)) return true;
        if (QFileInfo::exists(item.finalPath)) {
            problem = "A file already exists at the selected download path.";
            return false;
        }
        if (QFile::copy(item.stagingFile, item.finalPath)) {
            (void)QFile::remove(item.stagingFile);
            return true;
        }
        if (QFileInfo::exists(item.finalPath))
            problem = "A file already exists at the selected download path.";
        else
            problem = "Chromium could not publish the completed download: " + source.errorString().toStdString();
        return false;
    }

    void finish(std::string_view id, std::string state, std::optional<std::string> error = std::nullopt) {
        auto activeItem = active.find(id);
        if (activeItem == active.end()) return;
        ActiveDownload item = std::move(activeItem->second);
        active.erase(activeItem);
        if (item.request)
            QObject::disconnect(item.request, nullptr, context.get(), nullptr);

        DownloadRecord *entry = download(id);
        if (!entry) {
            cleanupStaging(item);
            return;
        }
        if (state == "completed") {
            std::string publishProblem;
            if (publish(item, publishProblem)) {
                const QFileInfo completed(item.finalPath);
                entry->name = completed.fileName().toStdString();
                entry->path = item.finalPath.toStdString();
                entry->received = completed.size();
                entry->expected = completed.size();
                entry->state = "completed";
                entry->error.reset();
            } else {
                entry->state = "failed";
                entry->path.reset();
                entry->error = std::move(publishProblem);
            }
        } else {
            entry->state = std::move(state);
            entry->path.reset();
            entry->error = std::move(error);
        }
        cleanupStaging(item);
        notifyDownloads();
    }

    void synchronize(std::string_view id, QWebEngineDownloadRequest *request) {
        const auto activeItem = active.find(id);
        DownloadRecord *entry = download(id);
        if (activeItem == active.end() || !entry || !request || entry->state != "downloading") return;
        entry->received = request->receivedBytes();
        entry->expected = request->totalBytes();
        switch (request->state()) {
        case QWebEngineDownloadRequest::DownloadRequested:
        case QWebEngineDownloadRequest::DownloadInProgress:
            notifyDownloads();
            break;
        case QWebEngineDownloadRequest::DownloadCompleted:
            finish(id, "completed");
            break;
        case QWebEngineDownloadRequest::DownloadCancelled:
            finish(id, "cancelled");
            break;
        case QWebEngineDownloadRequest::DownloadInterrupted: {
            std::string reason = request->interruptReasonString().toStdString();
            if (reason.empty()) reason = "Chromium interrupted the download.";
            finish(id, "failed", std::move(reason));
            break;
        }
        }
    }

    std::optional<std::string> trackAndAccept(
        QWebEngineDownloadRequest *request,
        Owner owner,
        std::unique_ptr<QWebEnginePage> explicitPage = {}
    ) {
        if (!request) return "Chromium did not provide a download request.";
        const std::string id = uuid();
        const QString fileName = uniqueFilename(
            downloadDirectory,
            request->suggestedFileName(),
            downloadEntries,
            reservations()
        );
        QString stagingDirectory;
        QString stagingFile;
        if (!prepareStaging(id, stagingDirectory, stagingFile))
            return "Chromium could not create an owner-only download staging path.";

        request->setDownloadDirectory(stagingDirectory);
        request->setDownloadFileName(QFileInfo(stagingFile).fileName());
        DownloadRecord entry{
            .id = id,
            .name = fileName.toStdString(),
            .source = request->url().toString().toStdString(),
            .date = nowIso8601(),
            .state = "downloading",
            .received = request->receivedBytes(),
            .expected = request->totalBytes(),
            .path = std::nullopt,
            .error = std::nullopt,
        };
        downloadEntries.insert(downloadEntries.begin(), std::move(entry));
        active.emplace(id, ActiveDownload{
            .request = request,
            .owner = owner,
            .stagingDirectory = stagingDirectory,
            .stagingFile = stagingFile,
            .finalPath = QDir(downloadDirectory).filePath(fileName),
            .explicitPage = std::move(explicitPage),
        });
        if (!persistDownloads()) {
            ActiveDownload failed = std::move(active.at(id));
            active.erase(id);
            downloadEntries.erase(downloadEntries.begin());
            cleanupStaging(failed);
            request->cancel();
            return "Chromium could not persist download metadata before starting.";
        }

        QObject::connect(request, &QWebEngineDownloadRequest::receivedBytesChanged, context.get(), [this, id, request] {
            synchronize(id, request);
        });
        QObject::connect(request, &QWebEngineDownloadRequest::totalBytesChanged, context.get(), [this, id, request] {
            synchronize(id, request);
        });
        QObject::connect(
            request,
            &QWebEngineDownloadRequest::stateChanged,
            context.get(),
            [this, id, request](QWebEngineDownloadRequest::DownloadState) { synchronize(id, request); }
        );
        QObject::connect(request, &QObject::destroyed, context.get(), [this, id] {
            if (active.contains(id)) finish(id, "failed", requestEnded);
        });
        notifyDownloads();
        request->accept();
        return std::nullopt;
    }

    void retireExplicitPage(PendingDownload &value) {
        if (!value.page) return;
        retiredExplicitPages.push_back({.page = std::move(value.page), .url = value.url});
        if (retiredExplicitPages.size() > maximumRetiredExplicitPages)
            retiredExplicitPages.erase(retiredExplicitPages.begin());
    }

    bool consumeRetiredExplicitPage(QWebEngineDownloadRequest *request) {
        const auto found = std::find_if(
            retiredExplicitPages.begin(),
            retiredExplicitPages.end(),
            [request](const RetiredExplicitPage &value) {
                return request && request->page() == value.page.get() && request->url() == value.url;
            }
        );
        if (found == retiredExplicitPages.end()) return false;
        retiredExplicitPages.erase(found);
        return true;
    }

    void handleDownloadRequest(QWebEngineDownloadRequest *request) {
        if (!request) return;
        if (shuttingDown) {
            request->cancel();
            return;
        }
        if (pending
            && pending->page
            && request->page() == pending->page.get()
            && request->url() == pending->url) {
            auto completion = std::move(pending->completion);
            auto explicitPage = std::move(pending->page);
            pending.reset();
            const auto problem = trackAndAccept(request, Owner::agent, std::move(explicitPage));
            completion(problem);
            return;
        }
        if (consumeRetiredExplicitPage(request)) {
            request->cancel();
            return;
        }

        const std::optional<engine::PageOwner> nativeOwner =
            QtBrowserPage::ownerForNativePage(request->page());
        const Owner owner = nativeOwner == engine::PageOwner::agent
            ? Owner::agent
            : request->page() && request->page()->profile() == profile.privateProfile()
                ? Owner::privatePage
                : Owner::user;
        if (owner == Owner::agent) {
            if (const auto problem = trackAndAccept(request, owner)) {
                qWarning("Chromium rejected an agent-page download: %s", problem->c_str());
                request->cancel();
            }
            return;
        }

        bool accepted = true;
        if (userPrompt) {
            QPointer<QWebEngineDownloadRequest> guarded(request);
            try {
                accepted = userPrompt(
                    request->suggestedFileName().toStdString(),
                    downloadDirectory.toStdString()
                );
            } catch (...) {
                accepted = false;
            }
            if (!guarded) return;
        }
        if (!accepted) {
            request->cancel();
            return;
        }
        if (const auto problem = trackAndAccept(request, owner)) {
            qWarning("Chromium rejected a user download: %s", problem->c_str());
            request->cancel();
        }
    }

    void cancelPending(std::string error) {
        if (!pending) return;
        auto callback = std::move(pending->completion);
        retireExplicitPage(*pending);
        pending.reset();
        callback(std::move(error));
    }

    void finishOwned(Owner owner, std::string state, std::optional<std::string> error = std::nullopt) {
        std::vector<std::string> identifiers;
        for (const auto &[id, item] : active) {
            if (item.owner == owner) identifiers.push_back(id);
        }
        for (const std::string &id : identifiers) {
            const auto found = active.find(id);
            if (found == active.end()) continue;
            const QPointer<QWebEngineDownloadRequest> request = found->second.request;
            if (request) request->cancel();
            if (active.contains(id)) finish(id, state, error);
        }
    }

    QtBrowserProfile &profile;
    std::filesystem::path profileDirectory;
    std::filesystem::path historyPath;
    std::filesystem::path bookmarksPath;
    std::filesystem::path downloadsPath;
    std::filesystem::path stagingPath;
    QString stagingRoot;
    QString downloadDirectory;
    std::unique_ptr<QObject> context;
    std::vector<HistoryRecord> historyEntries;
    std::vector<BookmarkRecord> bookmarkEntries;
    std::vector<DownloadRecord> downloadEntries;
    std::map<std::string, ActiveDownload, std::less<>> active;
    std::optional<PendingDownload> pending;
    std::vector<RetiredExplicitPage> retiredExplicitPages;
    std::uint64_t nextPendingToken = 1;
    UserDownloadPrompt userPrompt;
    DownloadsObserver downloadsObserver;
    bool shuttingDown = false;
};

QtBrowserLibrary::QtBrowserLibrary(
    QtBrowserProfile &profile,
    std::filesystem::path profileDirectory,
    std::filesystem::path downloadDirectory
) : impl_(std::make_unique<Impl>(profile, std::move(profileDirectory), std::move(downloadDirectory))) {}

QtBrowserLibrary::~QtBrowserLibrary() {
    if (impl_ && !impl_->shuttingDown) shutdownDownloads();
}

void QtBrowserLibrary::setUserDownloadPrompt(UserDownloadPrompt prompt) {
    impl_->userPrompt = std::move(prompt);
}

void QtBrowserLibrary::setDownloadsObserver(DownloadsObserver observer) {
    impl_->downloadsObserver = std::move(observer);
}

core::Json QtBrowserLibrary::history(std::string_view query, std::size_t limit) const {
    const QString term = QString::fromUtf8(query.data(), static_cast<qsizetype>(query.size())).trimmed();
    core::Json::Array values;
    values.reserve(std::min(limit, impl_->historyEntries.size()));
    for (const auto &entry : impl_->historyEntries) {
        const bool matches = term.isEmpty()
            || QString::fromStdString(entry.title).contains(term, Qt::CaseInsensitive)
            || QString::fromStdString(entry.url).contains(term, Qt::CaseInsensitive);
        if (!matches) continue;
        values.push_back(historyJson(entry));
        if (values.size() >= limit) break;
    }
    return core::Json(std::move(values));
}

core::Json QtBrowserLibrary::bookmarks(std::string_view query, std::size_t limit) const {
    const QString term = QString::fromUtf8(query.data(), static_cast<qsizetype>(query.size())).trimmed();
    core::Json::Array values;
    values.reserve(std::min(limit, impl_->bookmarkEntries.size()));
    for (const BookmarkRecord &entry : impl_->bookmarkEntries) {
        const bool matches = term.isEmpty()
            || QString::fromStdString(entry.title).contains(term, Qt::CaseInsensitive)
            || QString::fromStdString(entry.url).contains(term, Qt::CaseInsensitive)
            || QString::fromStdString(entry.folder).contains(term, Qt::CaseInsensitive);
        if (!matches) continue;
        values.push_back(bookmarkJson(entry));
        if (values.size() >= limit) break;
    }
    return core::Json(std::move(values));
}

bool QtBrowserLibrary::addBookmark(std::string_view title, std::string_view address, std::string_view folder) {
    const std::string url = sanitizedHistoryUrl(address);
    if (url.empty()) return false;
    QString normalizedFolder = QString::fromUtf8(folder.data(), static_cast<qsizetype>(folder.size())).trimmed();
    if (normalizedFolder.isEmpty()) normalizedFolder = QStringLiteral("Bookmarks");
    normalizedFolder = normalizedFolder.left(80);
    const std::string folderText = normalizedFolder.toStdString();
    const bool duplicate = std::any_of(impl_->bookmarkEntries.begin(), impl_->bookmarkEntries.end(), [&url, &folderText](const BookmarkRecord &entry) {
        return entry.url == url && entry.folder == folderText;
    });
    if (duplicate) return false;
    QString normalizedTitle = QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())).trimmed();
    if (normalizedTitle.isEmpty()) normalizedTitle = QString::fromStdString(url);
    impl_->bookmarkEntries.insert(impl_->bookmarkEntries.begin(), {
        .id = uuid(), .title = normalizedTitle.left(200).toStdString(), .url = url, .folder = folderText,
    });
    if (!impl_->persistBookmarks()) {
        impl_->bookmarkEntries.erase(impl_->bookmarkEntries.begin());
        throw std::runtime_error("Chromium could not persist the bookmark.");
    }
    return true;
}

bool QtBrowserLibrary::removeBookmark(std::string_view id) {
    const auto found = std::find_if(impl_->bookmarkEntries.begin(), impl_->bookmarkEntries.end(), [id](const BookmarkRecord &entry) {
        return entry.id == id;
    });
    if (found == impl_->bookmarkEntries.end()) return false;
    const BookmarkRecord removed = *found;
    const auto position = static_cast<std::size_t>(std::distance(impl_->bookmarkEntries.begin(), found));
    impl_->bookmarkEntries.erase(found);
    if (!impl_->persistBookmarks()) {
        impl_->bookmarkEntries.insert(impl_->bookmarkEntries.begin() + static_cast<std::ptrdiff_t>(position), removed);
        throw std::runtime_error("Chromium could not persist bookmark removal.");
    }
    return true;
}

bool QtBrowserLibrary::renameBookmark(std::string_view id, std::string_view title) {
    QString normalized = QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size())).trimmed();
    if (normalized.isEmpty()) return false;
    normalized = normalized.left(200);
    const auto found = std::find_if(impl_->bookmarkEntries.begin(), impl_->bookmarkEntries.end(), [id](const BookmarkRecord &entry) {
        return entry.id == id;
    });
    if (found == impl_->bookmarkEntries.end()) return false;
    const std::string previous = found->title;
    if (previous == normalized.toStdString()) return true;
    found->title = normalized.toStdString();
    if (!impl_->persistBookmarks()) {
        found->title = previous;
        throw std::runtime_error("Chromium could not persist the bookmark title.");
    }
    return true;
}

bool QtBrowserLibrary::moveBookmark(std::string_view id, std::string_view folder) {
    QString normalized = QString::fromUtf8(folder.data(), static_cast<qsizetype>(folder.size())).trimmed();
    if (normalized.isEmpty()) normalized = QStringLiteral("Bookmarks");
    normalized = normalized.left(80);
    const std::string target = normalized.toStdString();
    const auto found = std::find_if(impl_->bookmarkEntries.begin(), impl_->bookmarkEntries.end(), [id](const BookmarkRecord &entry) {
        return entry.id == id;
    });
    if (found == impl_->bookmarkEntries.end()) return false;
    if (found->folder == target) return true;
    // The same URL must not appear twice in one folder.
    const std::string url = found->url;
    const bool clash = std::any_of(impl_->bookmarkEntries.begin(), impl_->bookmarkEntries.end(), [&url, &target, id](const BookmarkRecord &entry) {
        return entry.id != id && entry.url == url && entry.folder == target;
    });
    if (clash) return false;
    const std::string previous = found->folder;
    found->folder = target;
    if (!impl_->persistBookmarks()) {
        found->folder = previous;
        throw std::runtime_error("Chromium could not persist the bookmark folder.");
    }
    return true;
}

std::size_t QtBrowserLibrary::clearHistory(std::string_view sinceIso8601) {
    const std::string since(sinceIso8601);
    const std::vector<HistoryRecord> previous = impl_->historyEntries;
    if (since.empty()) {
        impl_->historyEntries.clear();
    } else {
        // Dates are stored as UTC ISO-8601, so comparing the strings orders them
        // correctly without parsing.
        impl_->historyEntries.erase(
            std::remove_if(impl_->historyEntries.begin(), impl_->historyEntries.end(), [&since](const HistoryRecord &entry) {
                return entry.date >= since;
            }),
            impl_->historyEntries.end()
        );
    }
    const std::size_t removed = previous.size() - impl_->historyEntries.size();
    if (removed == 0) return 0;
    if (!impl_->persistHistory()) {
        impl_->historyEntries = previous;
        throw std::runtime_error("Chromium could not persist the history removal.");
    }
    return removed;
}

std::vector<std::string> QtBrowserLibrary::bookmarkFolders() const {
    std::vector<std::string> folders;
    for (const BookmarkRecord &entry : impl_->bookmarkEntries) {
        if (std::find(folders.begin(), folders.end(), entry.folder) == folders.end())
            folders.push_back(entry.folder);
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

std::size_t QtBrowserLibrary::importHistory(const core::Json::Array &entries) {
    std::size_t added = 0;
    for (const core::Json &value : entries) {
        const std::string url = sanitizedHistoryUrl(jsonString(value, "url"));
        if (url.empty()) continue;
        const std::int64_t sourceVisits = std::max<std::int64_t>(1, jsonInteger(value, "visits", 1));
        const auto found = std::find_if(
            impl_->historyEntries.begin(), impl_->historyEntries.end(),
            [&url](const HistoryRecord &entry) { return entry.url == url; }
        );
        if (found == impl_->historyEntries.end()) {
            impl_->historyEntries.insert(impl_->historyEntries.begin(), {
                .id = uuid(),
                .title = jsonString(value, "title", url),
                .url = url,
                .date = nowIso8601(),
                .visits = sourceVisits,
            });
            ++added;
        } else {
            found->visits = std::max(found->visits, sourceVisits);
            if (found->title.empty()) found->title = jsonString(value, "title", url);
        }
    }
    if (impl_->historyEntries.size() > 2'000) impl_->historyEntries.resize(2'000);
    if (!impl_->persistHistory())
        throw std::runtime_error("Chromium could not persist imported history.");
    return added;
}

core::Json QtBrowserLibrary::downloads() const {
    core::Json::Array values;
    values.reserve(impl_->downloadEntries.size());
    for (const auto &entry : impl_->downloadEntries) values.push_back(downloadJson(entry));
    return core::Json(std::move(values));
}

std::string QtBrowserLibrary::downloadDirectory() const {
    return impl_->downloadDirectory.toStdString();
}

void QtBrowserLibrary::recordVisit(const engine::PageState &state) {
    const std::string url = sanitizedHistoryUrl(state.url);
    if (url.empty()) return;
    const auto found = std::find_if(
        impl_->historyEntries.begin(),
        impl_->historyEntries.end(),
        [&url](const HistoryRecord &entry) { return entry.url == url; }
    );
    HistoryRecord entry;
    if (found != impl_->historyEntries.end()) {
        entry = *found;
        impl_->historyEntries.erase(found);
        ++entry.visits;
    } else {
        entry.id = uuid();
        entry.url = url;
    }
    entry.title = state.title.empty() ? url : state.title;
    entry.date = nowIso8601();
    impl_->historyEntries.insert(impl_->historyEntries.begin(), std::move(entry));
    if (impl_->historyEntries.size() > 2'000) impl_->historyEntries.resize(2'000);
    if (!impl_->persistHistory()) qWarning("Chromium history metadata could not be persisted.");
}

void QtBrowserLibrary::startDownload(
    engine::BrowserPage &page,
    std::string_view url,
    DownloadCompletion completion
) {
    if (impl_->shuttingDown) {
        completion("Browser is shutting down.");
        return;
    }
    auto *qtPage = dynamic_cast<QtBrowserPage *>(&page);
    if (!qtPage || qtPage->profile() != impl_->profile.persistentProfile()
        || page.state().owner != engine::PageOwner::agent) {
        completion("Download requests require an agent-owned page in the active profile.");
        return;
    }
    if (impl_->pending) {
        completion("A download request is already pending.");
        return;
    }
    const std::uint64_t token = impl_->nextPendingToken++;
    auto downloadPage = std::make_unique<QWebEnginePage>(impl_->profile.persistentProfile());
    QtBrowserPage::setNativePageOwner(downloadPage.get(), engine::PageOwner::agent);
    QWebEnginePage *nativeDownloadPage = downloadPage.get();
    const QUrl downloadUrl(QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size())));
    impl_->pending = Impl::PendingDownload{
        .token = token,
        .page = std::move(downloadPage),
        .url = downloadUrl,
        .completion = std::move(completion),
    };
    QTimer::singleShot(5'000, impl_->context.get(), [impl = impl_.get(), token] {
        if (!impl->pending || impl->pending->token != token) return;
        impl->cancelPending("Chromium did not create the download request.");
    });
    nativeDownloadPage->download(downloadUrl);
}

controller::CancelDownloadResult QtBrowserLibrary::cancelDownload(std::string_view id) {
    DownloadRecord *entry = impl_->download(id);
    if (!entry) return controller::CancelDownloadResult::unknown;
    if (entry->state != "downloading") return controller::CancelDownloadResult::inactive;
    const auto active = impl_->active.find(id);
    if (active == impl_->active.end()) return controller::CancelDownloadResult::inactive;
    const QPointer<QWebEngineDownloadRequest> request = active->second.request;
    if (request) request->cancel();
    if (impl_->active.contains(id)) impl_->finish(id, "cancelled");
    return controller::CancelDownloadResult::requested;
}

void QtBrowserLibrary::endAgentDownloads() {
    if (impl_->shuttingDown) return;
    impl_->cancelPending("Agent operation was paused.");
    impl_->finishOwned(Impl::Owner::agent, "cancelled");
}

void QtBrowserLibrary::shutdownDownloads() {
    if (impl_->shuttingDown) return;
    impl_->shuttingDown = true;
    impl_->cancelPending("Browser is shutting down.");
    std::vector<std::string> identifiers;
    identifiers.reserve(impl_->active.size());
    for (const auto &[id, item] : impl_->active) {
        (void)item;
        identifiers.push_back(id);
    }
    for (const std::string &id : identifiers) {
        const auto found = impl_->active.find(id);
        if (found == impl_->active.end()) continue;
        const QPointer<QWebEngineDownloadRequest> request = found->second.request;
        if (request) request->cancel();
        if (impl_->active.contains(id)) impl_->finish(id, "interrupted", shutdownInterruption);
    }
    impl_->downloadsObserver = {};
    // Every native page this library owns has to go now. A retired page only
    // exists to recognise a late download request, and `shuttingDown` already
    // cancels those. Keeping them would outlive the profile they belong to:
    // the shell destroys its session, and therefore the profile, after asking
    // the library to shut down, and Qt then warns that a page is still alive.
    impl_->retiredExplicitPages.clear();
}

std::size_t QtBrowserLibrary::retainedNativePages() const {
    // The pending page plus every retired one. Must be zero once the library
    // has been shut down, because the profile is released right after.
    return impl_->retiredExplicitPages.size() + ((impl_->pending && impl_->pending->page) ? 1U : 0U);
}

} // namespace yobro::qtwebengine
