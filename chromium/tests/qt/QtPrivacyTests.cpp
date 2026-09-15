#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "spike/PrivacySettings.hpp"
#include "yobro/core/Json.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using yobro::core::Json;
using yobro::qtwebengine::QtBrowserLibrary;
using yobro::spike::CookieRetention;
using yobro::spike::PrivacySettings;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

std::string isoOffsetHours(int hours) {
    return QDateTime::currentDateTimeUtc().addSecs(hours * 3600).toString(Qt::ISODate).toStdString();
}

/// Writes a history file directly, which is the only way to obtain visits with
/// controlled timestamps: recording a visit always stamps the current time.
void seedHistory(const std::filesystem::path &profileDirectory) {
    const std::string body = std::string("[")
        + R"({"id":"recent","title":"Recent","url":"https://recent.example.test/","date":")" + isoOffsetHours(0) + R"(","visits":1},)"
        + R"({"id":"hours","title":"Hours","url":"https://hours.example.test/","date":")" + isoOffsetHours(-2) + R"(","visits":1},)"
        + R"({"id":"days","title":"Days","url":"https://days.example.test/","date":")" + isoOffsetHours(-72) + R"(","visits":1})"
        + "]";
    std::ofstream file(profileDirectory / "history.json", std::ios::binary | std::ios::trunc);
    file << body;
    if (!file) fail("The test could not seed the history file.");
}

std::vector<Json> historyEntries(const QtBrowserLibrary &library) {
    const Json values = library.history({}, 500);
    return values.isArray() ? values.asArray() : std::vector<Json>{};
}

bool listsUrl(const QtBrowserLibrary &library, const std::string &url) {
    for (const Json &entry : historyEntries(library)) {
        const Json *value = entry.find("url");
        if (value && value->isString() && value->asString() == url) return true;
    }
    return false;
}

void touch(const std::filesystem::path &file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << "x";
    if (!stream) fail("The test could not create a placeholder file.");
}

void checkCookieRetention(const std::filesystem::path &profileDirectory) {
    {
        PrivacySettings settings(profileDirectory);
        check(settings.cookieRetention() == CookieRetention::keep,
              "Cookies must be kept by default so logins survive a restart.");
        settings.setCookieRetention(CookieRetention::sessionOnly);
        check(settings.cookieRetention() == CookieRetention::sessionOnly,
              "The retention choice was not applied.");
    }
    {
        PrivacySettings reopened(profileDirectory);
        check(reopened.cookieRetention() == CookieRetention::sessionOnly,
              "The retention choice did not survive a restart.");
        reopened.setCookieRetention(CookieRetention::keep);
    }
    {
        PrivacySettings reopened(profileDirectory);
        check(reopened.cookieRetention() == CookieRetention::keep,
              "Switching back to persistent cookies was not persisted.");
    }
}

void checkPendingSiteDataClear(const std::filesystem::path &profileDirectory) {
    // A profile that has been used holds site data, a cache, and the library
    // files. Only the first two may be removed.
    touch(profileDirectory / "Chromium" / "Local Storage" / "leveldb" / "CURRENT");
    touch(profileDirectory / "Chromium" / "IndexedDB" / "store" / "data");
    touch(profileDirectory / "Chromium" / "Service Worker" / "worker");
    touch(profileDirectory / "Cache" / "data_0");
    touch(profileDirectory / "Chromium" / "Cookies");
    touch(profileDirectory / "history.json");
    touch(profileDirectory / "bookmarks.json");
    touch(profileDirectory / "session.json");

    PrivacySettings settings(profileDirectory);
    check(!settings.siteDataClearRequested(), "No clear should be pending on a fresh profile.");
    check(PrivacySettings::applyPendingSiteDataClear(profileDirectory) == 0,
          "Without a request nothing may be removed.");
    check(std::filesystem::exists(profileDirectory / "Chromium" / "Local Storage" / "leveldb" / "CURRENT"),
          "Site data was removed without a request.");

    settings.requestSiteDataClear();
    check(settings.siteDataClearRequested(), "The request was not recorded.");

    const std::size_t removed = PrivacySettings::applyPendingSiteDataClear(profileDirectory);
    check(removed >= 4, "Not all requested directories were removed.");
    check(!std::filesystem::exists(profileDirectory / "Chromium" / "Local Storage"),
          "Local storage survived the clear.");
    check(!std::filesystem::exists(profileDirectory / "Chromium" / "IndexedDB"),
          "IndexedDB survived the clear.");
    check(!std::filesystem::exists(profileDirectory / "Chromium" / "Service Worker"),
          "Service workers survived the clear.");
    check(!std::filesystem::exists(profileDirectory / "Cache"), "The cache survived the clear.");
    // The library files and the profile itself have to stay usable.
    check(std::filesystem::exists(profileDirectory / "history.json"), "The clear deleted the history file.");
    check(std::filesystem::exists(profileDirectory / "bookmarks.json"), "The clear deleted the bookmarks file.");
    check(std::filesystem::exists(profileDirectory / "session.json"), "The clear deleted the session file.");
    check(std::filesystem::exists(profileDirectory / "Chromium"), "The clear deleted the storage directory.");

    // The request is consumed, so a restart does not clear again.
    check(!PrivacySettings(profileDirectory).siteDataClearRequested(),
          "The request was not consumed.");
    check(PrivacySettings::applyPendingSiteDataClear(profileDirectory) == 0,
          "A consumed request cleared a second time.");
}

void checkRangedHistoryClear(QtBrowserLibrary &library, const std::filesystem::path &profileDirectory) {
    check(historyEntries(library).size() == 3, "The seeded history was not loaded.");

    // Qt WebEngine cannot delete cookies or the cache for a time range, so the
    // range applies to the history this profile keeps itself.
    check(library.clearHistory(isoOffsetHours(-1)) == 1, "The last hour removed the wrong number of visits.");
    check(!listsUrl(library, "https://recent.example.test/"), "The recent visit survived.");
    check(listsUrl(library, "https://hours.example.test/"), "A visit outside the range was removed.");

    check(library.clearHistory(isoOffsetHours(-1)) == 0, "Repeating a range must remove nothing.");

    check(library.clearHistory(isoOffsetHours(-24)) == 1, "The last day removed the wrong number of visits.");
    check(listsUrl(library, "https://days.example.test/"), "The three-day-old visit was removed too early.");

    check(library.clearHistory() == 1, "Clearing everything removed the wrong number of visits.");
    check(historyEntries(library).empty(), "The history is not empty.");
    check(library.clearHistory() == 0, "Clearing an empty history must report nothing.");

    // The removal has to be on disk, not only in memory.
    QFile stored(QString::fromStdString((profileDirectory / "history.json").string()));
    check(stored.open(QIODevice::ReadOnly), "The history file is missing.");
    check(stored.readAll().trimmed() == QByteArrayLiteral("[]"), "The history file still holds visits.");
}

} // namespace

int main(int argc, char *argv[]) {
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const std::filesystem::path rootPath = root.path().toStdString();
    qputenv("YOBRO_CHROMIUM_HOME", QString::fromStdString((rootPath / "chromium").string()).toUtf8());
    qputenv("YOBRO_WEBKIT_HOME", QString::fromStdString((rootPath / "webkit").string()).toUtf8());
    QApplication application(argc, argv);

    try {
        checkCookieRetention(rootPath / "settings-profile");
        checkPendingSiteDataClear(rootPath / "clear-profile");

        const std::filesystem::path profileDirectory = rootPath / "history-profile";
        std::filesystem::create_directories(profileDirectory);
        seedHistory(profileDirectory);
        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "privacy-test",
            .storagePath = (rootPath / "storage").string(),
            .cachePath = (rootPath / "cache").string(),
            .persistent = true,
        });
        QtBrowserLibrary library(profile, profileDirectory);
        checkRangedHistoryClear(library, profileDirectory);
    } catch (const std::exception &error) {
        std::cerr << "QT PRIVACY FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "QT PRIVACY PASS\n";
    return 0;
}
