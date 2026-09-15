#include "spike/BrowserDataImport.hpp"
#include "spike/ImportWorker.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::BrowserDataImport;
using yobro::spike::ImportProfileEntry;
using yobro::spike::ImportedBrowserData;
using yobro::spike::ImportedLink;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

void writeFile(const QString &path, const QByteArray &contents) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "Could not create a fixture directory.");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "Could not write a fixture file.");
    file.write(contents);
}

bool listsUrl(const std::vector<ImportedLink> &links, const QString &url) {
    return std::any_of(links.begin(), links.end(), [&url](const ImportedLink &link) {
        return link.url == url;
    });
}

const ImportedLink &find(const std::vector<ImportedLink> &links, const QString &url) {
    const auto found = std::find_if(links.begin(), links.end(), [&url](const ImportedLink &link) {
        return link.url == url;
    });
    if (found == links.end()) fail("A link is missing: " + url.toStdString());
    return *found;
}

/// A home directory that looks like a Mac with Chrome installed.
void buildFakeHome(const QString &home) {
    const QString chrome = home + QStringLiteral("/Library/Application Support/Google/Chrome/Default");
    writeFile(chrome + QStringLiteral("/Preferences"), R"JSON({"profile":{"name":"Arbeit"}})JSON");
    const QString firefox = home + QStringLiteral("/Library/Application Support/Firefox/Profiles/test.default");
    check(QDir().mkpath(firefox), "Could not create the Firefox fixture profile.");
    // Chromium's bookmark file, including entries that must be dropped.
    writeFile(chrome + QStringLiteral("/Bookmarks"), R"JSON({
      "roots": {
        "bookmark_bar": {
          "children": [
            {"type": "url", "name": "Shop", "url": "https://shop.example/"},
            {"type": "url", "name": "Local file", "url": "file:///etc/hosts"},
            {"type": "url", "name": "Script", "url": "javascript:alert(1)"},
            {"type": "folder", "name": "Arbeit", "children": [
              {"type": "url", "name": "Wiki", "url": "https://wiki.example/start"}
            ]}
          ]
        },
        "other": {"children": []}
      }
    })JSON");
}

void checkProfileDiscovery(const QString &home) {
    QString problem;
    const std::vector<ImportProfileEntry> profiles = BrowserDataImport::profiles(home, problem);
    check(problem.isEmpty(), "Listing profiles failed: " + problem.toStdString());
    // Safari is always offered because its data is not in a profile directory.
    const bool hasSafari = std::any_of(profiles.begin(), profiles.end(), [](const ImportProfileEntry &entry) {
        return entry.browser == QStringLiteral("Safari");
    });
    check(hasSafari, "Safari was not offered as a source.");
    const auto chrome = std::find_if(profiles.begin(), profiles.end(), [](const ImportProfileEntry &entry) {
        return entry.browser == QStringLiteral("Chrome");
    });
    check(chrome != profiles.end(), "The Chrome profile in the fake home was not found.");
    // The profile name comes out of the browser's own preferences.
    check(chrome->name == QStringLiteral("Arbeit"),
          "The profile name was not read: " + chrome->name.toStdString());
    check(chrome->path.endsWith(QStringLiteral("/Default")), "The profile path is wrong.");
    const bool hasFirefox = std::any_of(profiles.begin(), profiles.end(), [](const ImportProfileEntry &entry) {
        return entry.browser == QStringLiteral("Firefox");
    });
    check(hasFirefox, "The Firefox profile in the fake home was not found.");
}

void checkBookmarkReading(const QString &home) {
    const QString chrome = home + QStringLiteral("/Library/Application Support/Google/Chrome/Default");
    const ImportedBrowserData data =
        BrowserDataImport::readProfile(QStringLiteral("Chrome"), chrome, {QStringLiteral("bookmarks")});
    check(data.problem.isEmpty(), "Reading bookmarks failed: " + data.problem.toStdString());
    check(data.bookmarks.size() == 2,
          "Expected two bookmarks, got " + std::to_string(data.bookmarks.size()) + ".");
    check(listsUrl(data.bookmarks, QStringLiteral("https://shop.example/")), "A bookmark is missing.");
    // A `file:` and a `javascript:` bookmark must never come through.
    check(!listsUrl(data.bookmarks, QStringLiteral("file:///etc/hosts")), "A file URL was imported.");
    const ImportedLink &wiki = find(data.bookmarks, QStringLiteral("https://wiki.example/start"));
    check(wiki.folder == QStringLiteral("Arbeit"),
          "The folder was not kept: " + wiki.folder.toStdString());
    check(wiki.title == QStringLiteral("Wiki"), "The title was not kept.");
    // Only the requested kind is read.
    check(data.history.empty() && data.tabs.empty() && data.cookies.empty(),
          "Kinds that were not requested came back anyway.");
    check(data.availability.contains(QStringLiteral("bookmarks")),
          "The availability note for bookmarks is missing.");
}

void checkHistoryReading(const QString &home, const QString &interpreter) {
    const QString chrome = home + QStringLiteral("/Library/Application Support/Google/Chrome/Default");
    const QString script = home + QStringLiteral("/build-history.py");
    writeFile(script, R"PY(import sqlite3, sys
connection = sqlite3.connect(sys.argv[1])
connection.execute("CREATE TABLE urls (url TEXT, title TEXT, last_visit_time INTEGER, visit_count INTEGER)")
# Chromium counts microseconds since 1601; 13'400'000'000'000'000 is in 2025.
connection.executemany("INSERT INTO urls VALUES (?, ?, ?, ?)", [
    ("https://news.example/article", "Article", 13400000000000000, 7),
    ("about:blank", "Blank", 13400000000000000, 1),
])
connection.commit()
connection.close()
)PY");
    QProcess builder;
    builder.start(interpreter, {script, chrome + QStringLiteral("/History")});
    check(builder.waitForFinished(30'000) && builder.exitCode() == 0,
          "The history fixture could not be created: "
          + QString::fromUtf8(builder.readAllStandardError()).toStdString());

    const ImportedBrowserData data =
        BrowserDataImport::readProfile(QStringLiteral("Chrome"), chrome, {QStringLiteral("history")});
    check(data.problem.isEmpty(), "Reading history failed: " + data.problem.toStdString());
    check(data.history.size() == 1,
          "Expected one history entry, got " + std::to_string(data.history.size()) + ".");
    const ImportedLink &entry = data.history.front();
    check(entry.url == QStringLiteral("https://news.example/article"), "The wrong entry was read.");
    check(entry.visits == 7, "The visit count was not kept.");
    check(entry.timestamp > 1'700'000'000, "The timestamp was not converted to Unix seconds.");
}

void checkRejectedInput(const QString &home) {
    // A profile directory that does not exist has to be reported.
    const ImportedBrowserData missing = BrowserDataImport::readProfile(
        QStringLiteral("Chrome"), home + QStringLiteral("/nope"), {QStringLiteral("bookmarks")}
    );
    check(!missing.problem.isEmpty(), "A missing profile directory was accepted.");

    // Without a kind there is nothing to do, and it must not reach the script.
    const ImportedBrowserData noKind =
        BrowserDataImport::readProfile(QStringLiteral("Chrome"), home, {});
    check(!noKind.problem.isEmpty(), "An empty kind list was accepted.");
    const ImportedBrowserData unknownKind =
        BrowserDataImport::readProfile(QStringLiteral("Chrome"), home, {QStringLiteral("secrets")});
    check(!unknownKind.problem.isEmpty(), "An unknown kind was accepted.");

    const ImportedBrowserData noProfile =
        BrowserDataImport::readProfile(QStringLiteral("Chrome"), QString(), {QStringLiteral("bookmarks")});
    check(!noProfile.problem.isEmpty(), "An empty profile path was accepted.");

    const ImportedBrowserData noFile =
        BrowserDataImport::readFile(QStringLiteral("bookmarks"), QString());
    check(!noFile.problem.isEmpty(), "An empty file path was accepted.");
    const ImportedBrowserData wrongKind =
        BrowserDataImport::readFile(QStringLiteral("secrets"), home);
    check(!wrongKind.problem.isEmpty(), "An unknown kind was accepted for a file.");
}

void checkFileImport(const QString &home) {
    // The HTML export format every browser can write.
    const QString file = home + QStringLiteral("/bookmarks.html");
    writeFile(file, R"HTML(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<DL><p>
    <DT><H3>Reisen</H3>
    <DL><p>
        <DT><A HREF="https://bahn.example/">Bahn</A>
    </DL><p>
    <DT><A HREF="https://start.example/">Start</A>
    <DT><A HREF="javascript:void(0)">Bad</A>
</DL><p>
)HTML");
    const ImportedBrowserData data = BrowserDataImport::readFile(QStringLiteral("bookmarks"), file);
    check(data.problem.isEmpty(), "Reading the export file failed: " + data.problem.toStdString());
    check(data.bookmarks.size() == 2,
          "Expected two bookmarks from the file, got " + std::to_string(data.bookmarks.size()) + ".");
    check(find(data.bookmarks, QStringLiteral("https://bahn.example/")).folder == QStringLiteral("Reisen"),
          "The folder from the export file was lost.");
    check(!listsUrl(data.bookmarks, QStringLiteral("javascript:void(0)")),
          "A javascript URL came in through the export file.");
}

void checkCookieReading(const QString &home, const QString &interpreter) {
    // Firefox is the one browser whose cookies can be read from a live profile.
    const QString firefox = home + QStringLiteral("/Library/Application Support/Firefox/Profiles/test.default");
    const QString script = home + QStringLiteral("/build-cookies.py");
    writeFile(script, R"PY(import sqlite3, sys
connection = sqlite3.connect(sys.argv[1])
connection.execute(
    "CREATE TABLE moz_cookies (name TEXT, value TEXT, host TEXT, path TEXT, expiry INTEGER,"
    " isSecure INTEGER, isHttpOnly INTEGER, sameSite INTEGER, originAttributes TEXT DEFAULT '')"
)
connection.executemany(
    "INSERT INTO moz_cookies (name, value, host, path, expiry, isSecure, isHttpOnly, sameSite,"
    " originAttributes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
    [
        ("session", "abc123", "shop.example", "/", 2000000000, 1, 1, 1, ""),
        ("isolated", "nope", "other.example", "/", 2000000000, 0, 0, 0, "^partitionKey=x"),
    ],
)
connection.commit()
connection.close()
)PY");
    QProcess builder;
    builder.start(interpreter, {script, firefox + QStringLiteral("/cookies.sqlite")});
    check(builder.waitForFinished(30'000) && builder.exitCode() == 0,
          "The cookie fixture could not be created: "
          + QString::fromUtf8(builder.readAllStandardError()).toStdString());

    const ImportedBrowserData data =
        BrowserDataImport::readProfile(QStringLiteral("Firefox"), firefox, {QStringLiteral("cookies")});
    check(data.problem.isEmpty(), "Reading cookies failed: " + data.problem.toStdString());
    check(data.cookies.size() == 1,
          "Expected one cookie, got " + std::to_string(data.cookies.size()) + ".");
    const auto &cookie = data.cookies.front();
    check(cookie.name == QStringLiteral("session") && cookie.value == QStringLiteral("abc123"),
          "The cookie was not read correctly.");
    check(cookie.domain == QStringLiteral("shop.example"), "The cookie domain is wrong.");
    check(cookie.secure && cookie.httpOnly, "The cookie flags were lost.");
    check(cookie.expires > 1'900'000'000, "The cookie expiry was lost.");
}

void checkKindLabels() {
    check(BrowserDataImport::kinds().size() == 4,
          "The kind list changed; passwords belong to NativePasswordImport.");
    for (const QString &kind : BrowserDataImport::kinds()) {
        check(!BrowserDataImport::kindLabel(kind).isEmpty(),
              "A kind has no label: " + kind.toStdString());
    }
    check(BrowserDataImport::kindLabel(QStringLiteral("unknown")) == QStringLiteral("unknown"),
          "An unknown kind should fall back to its own name.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    const QString interpreter = yobro::spike::ImportWorker::interpreter();

    try {
        check(!interpreter.isEmpty(), "No Python 3 interpreter available; the import cannot run here.");
        checkKindLabels();
        buildFakeHome(home.path());
        checkProfileDiscovery(home.path());
        checkBookmarkReading(home.path());
        checkHistoryReading(home.path(), interpreter);
        checkCookieReading(home.path(), interpreter);
        checkFileImport(home.path());
        checkRejectedInput(home.path());
    } catch (const std::exception &error) {
        std::cerr << "BROWSER DATA IMPORT FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "BROWSER DATA IMPORT PASS\n";
    return 0;
}
