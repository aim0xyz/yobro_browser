#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace yobro::spike {

/// How long cookies of this profile survive.
enum class CookieRetention {
    /// Cookies persist across restarts, so logins stay.
    keep,
    /// Cookies live only until the browser quits.
    sessionOnly,
};

/// Privacy preferences of one profile plus the deferred site-data clear.
///
/// Qt WebEngine can clear cookies, the HTTP cache and visited links while
/// running, but it has no API for local storage, IndexedDB or service workers.
/// Those directories can only be removed safely while no profile has them
/// open, so a request is recorded and applied on the next start.
class PrivacySettings {
public:
    explicit PrivacySettings(std::filesystem::path profileDirectory);

    [[nodiscard]] CookieRetention cookieRetention() const;
    void setCookieRetention(CookieRetention retention);

    /// Records that the on-disk site data should be removed on the next start.
    void requestSiteDataClear();
    [[nodiscard]] bool siteDataClearRequested() const;

    /// Removes the site-data directories of `profileDirectory` when a clear was
    /// requested, then drops the request. Must run before the profile is opened.
    /// Returns the number of removed directories.
    static std::size_t applyPendingSiteDataClear(const std::filesystem::path &profileDirectory);

    /// The storage directories that hold site data. Cookies and the cache are
    /// handled through the live Qt API instead.
    [[nodiscard]] static const std::vector<std::string> &siteDataDirectories();

private:
    void save() const;

    std::filesystem::path file_;
    std::filesystem::path marker_;
    CookieRetention retention_ = CookieRetention::keep;
};

} // namespace yobro::spike
