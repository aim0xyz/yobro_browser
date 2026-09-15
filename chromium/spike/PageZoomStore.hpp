#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace yobro::spike {

/// Per-website zoom levels for one profile.
///
/// Levels are remembered per host so a site that needs a larger scale keeps it
/// on the next visit. `www.` is ignored so both spellings share one level.
class PageZoomStore {
public:
    /// The familiar browser zoom ladder.
    [[nodiscard]] static const std::vector<double> &levels();
    static constexpr double standard = 1.0;

    explicit PageZoomStore(std::filesystem::path profileDirectory);

    /// Returns the host key for a URL, or nothing when the URL has no host.
    [[nodiscard]] static std::optional<std::string> key(const std::string &url);

    [[nodiscard]] double level(const std::string &url) const;
    /// `direction` is +1 larger or -1 smaller. Returns the applied level.
    std::optional<double> step(int direction, const std::string &url);
    std::optional<double> reset(const std::string &url);

private:
    void store(double value, const std::string &host);
    void save() const;

    std::filesystem::path file_;
    std::map<std::string, double> levelsByHost_;
};

} // namespace yobro::spike
