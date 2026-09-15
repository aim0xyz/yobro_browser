#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace yobro::spike {

struct ProfileEntry {
    /// Directory name under `Profiles/`; never changes once created.
    std::string id;
    std::string name;
    /// A short glyph shown in the sidebar and the profile list.
    std::string icon;
};

/// Display names and icons for every profile of this installation.
///
/// The registry lives next to the profile directories, so all profiles show the
/// same names. Storage identifiers stay untouched when a name changes, which is
/// what keeps a rename from moving any data.
class ProfileRegistry {
public:
    /// `root` is the directory that contains the `Profiles/` folder.
    explicit ProfileRegistry(std::filesystem::path root);

    /// Profiles from the registry plus any directory found on disk, sorted by
    /// name so the list order does not depend on the file.
    [[nodiscard]] std::vector<ProfileEntry> entries() const;
    [[nodiscard]] ProfileEntry entry(const std::string &id) const;
    [[nodiscard]] std::optional<ProfileEntry> known(const std::string &id) const;

    /// Empty names and oversized glyphs are rejected instead of stored.
    bool setName(const std::string &id, const std::string &name);
    bool setIcon(const std::string &id, const std::string &icon);
    /// Registers a profile that has just been created on disk.
    bool add(const std::string &id, const std::string &name, const std::string &icon);

    [[nodiscard]] static std::string defaultName(const std::string &id);
    static constexpr const char *defaultIcon = "●";

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    std::filesystem::path profiles_;
    std::vector<ProfileEntry> entries_;
};

} // namespace yobro::spike
