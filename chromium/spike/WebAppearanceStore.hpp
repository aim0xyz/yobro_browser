#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace yobro::spike {

/// Website appearance preferences for one profile.
///
/// Mirrors the WebKit build: light websites are only darkened when the user
/// asked for it and the system itself is dark, and single hosts can opt out.
class WebAppearanceStore {
public:
    explicit WebAppearanceStore(std::filesystem::path profileDirectory);

    [[nodiscard]] bool enabled() const;
    void setEnabled(bool enabled);

    /// Host comparison is lower-case; an empty host is ignored.
    [[nodiscard]] bool isExcluded(const std::string &host) const;
    void setExcluded(const std::string &host, bool excluded);

    [[nodiscard]] const std::set<std::string> &excludedHosts() const;
    /// The exact payload the injected script expects.
    [[nodiscard]] std::string json(bool systemDark) const;

private:
    void save() const;

    std::filesystem::path file_;
    bool enabled_ = false;
    std::set<std::string> excludedHosts_;
};

} // namespace yobro::spike
