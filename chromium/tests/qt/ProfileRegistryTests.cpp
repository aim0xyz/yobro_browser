#include "spike/ProfileRegistry.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::ProfileEntry;
using yobro::spike::ProfileRegistry;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

bool lists(const std::vector<ProfileEntry> &entries, const std::string &id) {
    return std::any_of(entries.begin(), entries.end(), [&id](const ProfileEntry &entry) {
        return entry.id == id;
    });
}

ProfileEntry find(const std::vector<ProfileEntry> &entries, const std::string &id) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&id](const ProfileEntry &entry) {
        return entry.id == id;
    });
    if (found == entries.end()) fail("A profile is missing from the listing.");
    return *found;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const std::filesystem::path rootPath = root.path().toStdString();

    try {
        std::filesystem::create_directories(rootPath / "Profiles" / "default");
        std::filesystem::create_directories(rootPath / "Profiles" / "arbeit");

        {
            ProfileRegistry registry(rootPath);
            // Directories without a record still have to appear, with a default
            // name and icon, so no profile can become unreachable.
            const std::vector<ProfileEntry> entries = registry.entries();
            check(entries.size() == 2, "The registry did not list both profile directories.");
            check(lists(entries, "default") && lists(entries, "arbeit"), "A profile directory is missing.");
            check(find(entries, "default").name == "Persönlich",
                  "The first profile should be named like the WebKit build's personal one.");
            check(find(entries, "arbeit").name == "arbeit", "An unnamed profile should fall back to its id.");
            check(find(entries, "arbeit").icon == std::string(ProfileRegistry::defaultIcon),
                  "An unnamed profile should use the default icon.");

            check(registry.setName("arbeit", "  Arbeit  "), "Renaming failed.");
            check(registry.setIcon("arbeit", "◆"), "Changing the icon failed.");
            check(find(registry.entries(), "arbeit").name == "Arbeit",
                  "The name should be stored without surrounding spaces.");

            // Invalid input must be refused rather than stored.
            check(!registry.setName("arbeit", "   "), "An empty name must be refused.");
            check(!registry.setName("arbeit", std::string(80, 'x')), "An oversized name must be refused.");
            check(!registry.setIcon("arbeit", ""), "An empty icon must be refused.");
            check(!registry.setIcon("arbeit", "viel zu langes symbol"), "An oversized icon must be refused.");
            check(find(registry.entries(), "arbeit").name == "Arbeit", "A refused change altered the name.");
            check(find(registry.entries(), "arbeit").icon == "◆", "A refused change altered the icon.");

            check(registry.add("privat", "Privat", "○"), "Adding a profile failed.");
            check(!registry.add("privat", "Zweimal", "○"), "A profile must not be added twice.");
        }

        {
            // Names and icons must survive a restart.
            ProfileRegistry reopened(rootPath);
            const std::vector<ProfileEntry> entries = reopened.entries();
            check(find(entries, "arbeit").name == "Arbeit", "The name was not persisted.");
            check(find(entries, "arbeit").icon == "◆", "The icon was not persisted.");
            check(find(entries, "privat").name == "Privat", "An added profile was not persisted.");
            check(reopened.entry("unbekannt").name == "unbekannt",
                  "An unknown profile must still resolve to a usable label.");
            check(!reopened.known("unbekannt").has_value(),
                  "An unknown profile must not be reported as registered.");

            // Sorting by name keeps the list order independent of the file.
            const std::vector<ProfileEntry> sorted = reopened.entries();
            check(std::is_sorted(sorted.begin(), sorted.end(), [](const ProfileEntry &a, const ProfileEntry &b) {
                      return a.name == b.name ? a.id < b.id : a.name < b.name;
                  }),
                  "The profile listing is not sorted by name.");
        }
    } catch (const std::exception &error) {
        std::cerr << "PROFILE REGISTRY FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PROFILE REGISTRY PASS\n";
    return 0;
}
