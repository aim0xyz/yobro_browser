#pragma once

#include <filesystem>

namespace yobro::spike {

/// The two ad-blocking switches, stored exactly like the WebKit build's
/// `adblock.json` so the wording and the defaults stay in sync.
class AdBlockSettings {
public:
    explicit AdBlockSettings(std::filesystem::path profileDirectory);

    /// Both default to true, as in the WebKit build.
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool strictProtection() const { return strict_; }

    void setEnabled(bool value);
    void setStrictProtection(bool value);

    /// The extra page filters that manipulate video playback run only when the
    /// blocker is on and strict protection is off, matching the WebKit build's
    /// `aggressiveVideoFilteringEnabled`.
    [[nodiscard]] bool aggressivePageFilters() const { return enabled_ && !strict_; }

private:
    void save() const;

    std::filesystem::path file_;
    bool enabled_ = true;
    bool strict_ = true;
};

} // namespace yobro::spike
