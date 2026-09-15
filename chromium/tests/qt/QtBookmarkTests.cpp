#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "yobro/core/Json.hpp"

#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using yobro::core::Json;
using yobro::qtwebengine::QtBrowserLibrary;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

std::string field(const Json &entry, const char *name) {
    const Json *value = entry.find(name);
    return value && value->isString() ? value->asString() : std::string{};
}

std::vector<Json> list(const QtBrowserLibrary &library, const std::string &query = {}) {
    const Json values = library.bookmarks(query, 500);
    return values.isArray() ? values.asArray() : std::vector<Json>{};
}

/// The same address may exist in several folders, so both are needed to select
/// one entry without ambiguity.
std::optional<Json> byUrl(const QtBrowserLibrary &library, const std::string &url, const std::string &folder) {
    for (const Json &entry : list(library)) {
        if (field(entry, "url") == url && field(entry, "folder") == folder) return entry;
    }
    return std::nullopt;
}

std::optional<Json> byId(const QtBrowserLibrary &library, const std::string &id) {
    for (const Json &entry : list(library)) {
        if (field(entry, "id") == id) return entry;
    }
    return std::nullopt;
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
        yobro::qtwebengine::QtBrowserProfile profile({
            .id = "bookmark-test",
            .storagePath = (rootPath / "storage").string(),
            .cachePath = (rootPath / "cache").string(),
            .persistent = true,
        });
        std::filesystem::create_directories(rootPath / "profile");
        QtBrowserLibrary library(profile, rootPath / "profile");

        check(library.addBookmark("Alpha", "https://alpha.example.test/", "Bookmarks"),
              "Adding a bookmark failed.");
        check(library.addBookmark("Beta", "https://beta.example.test/", "Arbeit"),
              "Adding a bookmark to another folder failed.");
        check(!library.addBookmark("Alpha again", "https://alpha.example.test/", "Bookmarks"),
              "The same URL must not be stored twice in one folder.");
        check(library.addBookmark("Alpha at work", "https://alpha.example.test/", "Arbeit"),
              "The same URL in a different folder must be allowed.");

        // Folders are reported sorted so a picker never has to guess.
        const std::vector<std::string> folders = library.bookmarkFolders();
        check(folders.size() == 2, "The folder listing has the wrong size.");
        check(folders.front() == "Arbeit" && folders.back() == "Bookmarks",
              "The folder listing is not sorted.");

        // The entry in the default folder is the one under test; the copy in
        // "Arbeit" stays untouched and later blocks a move.
        const Json alpha = byUrl(library, "https://alpha.example.test/", "Bookmarks").value_or(Json{});
        const std::string alphaId = field(alpha, "id");
        check(!alphaId.empty(), "The stored bookmark has no identifier.");

        // Renaming changes only the label.
        check(library.renameBookmark(alphaId, "  Alpha umbenannt  "), "Renaming failed.");
        const Json renamed = byId(library, alphaId).value_or(Json{});
        check(field(renamed, "title") == "Alpha umbenannt",
              "The title was not trimmed and stored.");
        check(field(renamed, "url") == "https://alpha.example.test/", "Renaming changed the address.");
        check(field(renamed, "folder") == "Bookmarks", "Renaming changed the folder.");
        check(!library.renameBookmark(alphaId, "   "), "An empty title must be refused.");
        check(!library.renameBookmark("unknown-id", "Egal"), "An unknown identifier must be refused.");
        check(field(byId(library, alphaId).value_or(Json{}), "title") == "Alpha umbenannt",
              "A refused rename changed the title anyway.");

        // Moving is refused when the target folder already holds the URL.
        check(!library.moveBookmark(alphaId, "Arbeit"),
              "Moving into a folder that already holds the URL must be refused.");
        check(field(byId(library, alphaId).value_or(Json{}), "folder") == "Bookmarks",
              "A refused move changed the folder anyway.");
        check(library.moveBookmark(alphaId, "Lesen"), "Moving to a fresh folder failed.");
        check(field(byId(library, alphaId).value_or(Json{}), "folder") == "Lesen",
              "The bookmark did not move.");
        check(library.moveBookmark(alphaId, "Lesen"), "Moving to the current folder should succeed.");
        check(!library.moveBookmark("unknown-id", "Lesen"), "An unknown identifier must be refused.");

        // An empty folder name falls back instead of creating a nameless folder.
        check(library.moveBookmark(alphaId, "   "), "An empty folder should fall back to the default.");
        check(field(byId(library, alphaId).value_or(Json{}), "folder") == "Bookmarks",
              "The fallback folder was not applied.");

        // Search covers title, address and folder.
        check(!list(library, "Arbeit").empty(), "Searching by folder returned nothing.");
        check(!list(library, "beta.example").empty(), "Searching by address returned nothing.");
        check(!list(library, "umbenannt").empty(), "Searching by title returned nothing.");

        const std::size_t before = list(library).size();
        check(library.removeBookmark(alphaId), "Removing a bookmark failed.");
        check(list(library).size() == before - 1, "Removal did not shrink the list.");
        check(!library.removeBookmark(alphaId), "Removing twice must be refused.");
    } catch (const std::exception &error) {
        std::cerr << "QT BOOKMARK FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "QT BOOKMARK PASS\n";
    return 0;
}
