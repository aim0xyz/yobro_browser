#include "yobro/core/ProfilePaths.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace yobro::core {
namespace {

std::filesystem::path environmentPath(const char *name) {
    const char *value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (auto path = environmentPath("USERPROFILE"); !path.empty())
        return path;
#endif
    if (auto path = environmentPath("HOME"); !path.empty())
        return path;
    throw std::runtime_error("Unable to determine the current user's home directory.");
}

std::filesystem::path normalized(const std::filesystem::path &path) {
    return std::filesystem::absolute(path).lexically_normal();
}

bool isInside(const std::filesystem::path &candidate, const std::filesystem::path &parent) {
    const auto child = normalized(candidate);
    const auto base = normalized(parent);
    auto childIt = child.begin();
    for (auto baseIt = base.begin(); baseIt != base.end(); ++baseIt, ++childIt) {
        if (childIt == child.end() || *childIt != *baseIt)
            return false;
    }
    return true;
}

std::string checkedProfileId(std::string_view id) {
    if (id.empty())
        throw std::invalid_argument("A Chromium profile id is required.");
    std::string value(id);
    for (const unsigned char character : value) {
        const bool valid = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_';
        if (!valid)
            throw std::invalid_argument("Chromium profile ids may contain only letters, digits, '-' and '_'.");
    }
    return value;
}

} // namespace

std::filesystem::path ProfilePaths::previewRoot() {
    if (auto configured = environmentPath("YOBRO_CHROMIUM_HOME"); !configured.empty())
        return normalized(configured);
#if defined(_WIN32)
    if (auto local = environmentPath("LOCALAPPDATA"); !local.empty())
        return normalized(local / "YOBRO" / "Chromium Feasibility");
    return normalized(homeDirectory() / "AppData" / "Local" / "YOBRO" / "Chromium Feasibility");
#elif defined(__APPLE__)
    return normalized(homeDirectory() / "Library" / "Application Support" / "YOBRO Chromium Feasibility");
#else
    if (auto data = environmentPath("XDG_DATA_HOME"); !data.empty())
        return normalized(data / "yobro-chromium-feasibility");
    return normalized(homeDirectory() / ".local" / "share" / "yobro-chromium-feasibility");
#endif
}

std::filesystem::path ProfilePaths::webKitRoot() {
    if (auto configured = environmentPath("YOBRO_WEBKIT_HOME"); !configured.empty())
        return normalized(configured);
    if (auto configured = environmentPath("YOBRO_HOME"); !configured.empty())
        return normalized(configured);
#if defined(__APPLE__)
    return normalized(homeDirectory() / "Library" / "Application Support" / "YOBRO");
#elif defined(_WIN32)
    return normalized(homeDirectory() / "YOBRO-WebKit-Reference");
#else
    return normalized(homeDirectory() / ".yobro-webkit-reference");
#endif
}

ProfilePaths ProfilePaths::forProfile(std::string_view profileId) {
    const auto id = checkedProfileId(profileId);
    ProfilePaths paths;
    paths.root = previewRoot();
    paths.profile = paths.root / "Profiles" / id;
    paths.storage = paths.profile / "Chromium";
    paths.cache = paths.profile / "Cache";
    paths.session = paths.profile / "session.json";
    paths.bridgePolicy = paths.profile / "bridge-policy.json";
    paths.control = paths.profile / "control.sock";
    paths.validateIsolation();
    return paths;
}

void ProfilePaths::validateIsolation() const {
    const auto preview = normalized(root);
    const auto webKit = normalized(webKitRoot());
    if (preview == webKit || isInside(preview, webKit) || isInside(webKit, preview)) {
        throw std::runtime_error(
            "The Chromium feasibility profile must not equal, contain, or live inside the WebKit profile."
        );
    }
}

void ProfilePaths::createDirectories() const {
    validateIsolation();
    std::filesystem::create_directories(storage);
    std::filesystem::create_directories(cache);
#if !defined(_WIN32)
    const auto permissions = std::filesystem::perms::owner_all;
    std::filesystem::permissions(root, permissions, std::filesystem::perm_options::replace);
    std::filesystem::permissions(profile, permissions, std::filesystem::perm_options::replace);
    std::filesystem::permissions(storage, permissions, std::filesystem::perm_options::replace);
    std::filesystem::permissions(cache, permissions, std::filesystem::perm_options::replace);
#endif
}

} // namespace yobro::core
