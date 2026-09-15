#include "yobro/core/Json.hpp"
#include "yobro/core/WebKitImport.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    if (!output) throw std::runtime_error("Could not write test fixture.");
}

std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const yobro::core::Json &required(const yobro::core::Json &object, const char *key) {
    const auto *value = object.find(key);
    if (!value) throw std::runtime_error("Expected JSON member is missing.");
    return *value;
}

void expectFailure(const std::filesystem::path &source, const std::filesystem::path &destination) {
    try {
        static_cast<void>(yobro::core::WebKitImport::commit(source, destination));
    } catch (const std::runtime_error &) {
        return;
    }
    throw std::runtime_error("Unsafe import destination was accepted.");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "yobro-webkit-import-test";
    std::filesystem::remove_all(root);
    const auto source = root / "source";
    const auto destination = root / "destination";
    try {
        write(source / "history.json", R"JSON([{"id":"h1","title":"Safe","url":"https://example.test","date":810000000,"visits":2},{"id":"h2","title":"Local","url":"file:///etc/passwd","date":0,"visits":1},{"id":"h3","title":"Whitespace","url":"https://example.test/a b","date":0,"visits":1}])JSON");
        write(source / "bookmarks.json", R"JSON([{"id":"b1","title":"Docs","url":"http://docs.test","folder":"Work"},{"id":"b2","title":"Script","url":"javascript:alert(1)","folder":""},{"id":"b3","title":"No host","url":"https:///path","folder":""}])JSON");
        write(source / "session.json", R"JSON({"tabs":[{"id":"t1","title":"Tab","url":"https://tab.test","space":"Personal","pinned":false,"kind":"web"},{"id":"t2","title":"Bad","url":"data:text/html,bad","space":"Personal","pinned":false,"kind":"web"}],"activeID":"t1","interactionState":"must-not-be-restored"})JSON");
        const std::string historyBefore = read(source / "history.json");
        const std::string bookmarksBefore = read(source / "bookmarks.json");
        const std::string sessionBefore = read(source / "session.json");

        const auto preview = yobro::core::WebKitImport::preview(source);
        check(preview.history == 1 && preview.bookmarks == 1 && preview.tabs == 1
              && preview.rejected == 5, "Import preview counts differ.");
        check(!std::filesystem::exists(destination), "Preview created a destination.");

        write(destination / "obsolete.txt", "old destination contents");
        const auto committed = yobro::core::WebKitImport::commit(source, destination);
        check(committed.history == 1 && committed.bookmarks == 1 && committed.tabs == 1
              && committed.rejected == 5, "Committed import counts differ.");
        check(read(source / "history.json") == historyBefore
              && read(source / "bookmarks.json") == bookmarksBefore
              && read(source / "session.json") == sessionBefore,
              "Read-only import changed its source.");
        check(!std::filesystem::exists(destination / "obsolete.txt"),
              "Published import retained stale destination contents.");
        check(yobro::core::Json::parse(read(destination / "history.json")).asArray().size() == 1,
              "Imported history differs.");
        check(yobro::core::Json::parse(read(destination / "bookmarks.json")).asArray().size() == 1,
              "Imported bookmarks differ.");
        const auto importedSession = yobro::core::Json::parse(read(destination / "session.json"));
        check(required(importedSession, "tabs").asArray().size() == 1,
              "Imported tabs differ.");
        check(importedSession.find("activeID") == nullptr && importedSession.find("interactionState") == nullptr,
              "Unsafe WebKit session state was retained.");

        const auto importJournal = yobro::core::Json::parse(read(destination / "webkit-import-journal.json"));
        check(required(importJournal, "format").asString() == "yobro-webkit-import-v1",
              "Import journal format is missing.");
        check(required(importJournal, "source").asString() == std::filesystem::weakly_canonical(source).string(),
              "Import journal source differs.");
        const auto &files = required(importJournal, "sourceFiles").asArray();
        check(files.size() == 3, "Import journal source manifest differs.");
        for (const auto &file : files) {
            check(required(file, "bytes").asInteger() >= 0 && !required(file, "fingerprint").asString().empty(),
                  "Import journal is missing source integrity evidence.");
        }

        expectFailure(source, source);
        expectFailure(source, source / "destination");
        check(read(source / "history.json") == historyBefore
              && read(source / "bookmarks.json") == bookmarksBefore
              && read(source / "session.json") == sessionBefore,
              "Rejected import changed its source.");
        check(!std::filesystem::exists(source / "destination"),
              "Rejected import created a child destination.");

        std::filesystem::remove_all(root);
        std::cout << "WEBKIT IMPORT TESTS PASS\n";
        return 0;
    } catch (const std::exception &error) {
        std::filesystem::remove_all(root);
        std::cerr << "WEBKIT IMPORT TESTS FAIL: " << error.what() << '\n';
        return 1;
    }
}
