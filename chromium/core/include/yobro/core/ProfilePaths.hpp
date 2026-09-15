#pragma once

#include <filesystem>
#include <string_view>

namespace yobro::core {

struct ProfilePaths {
    std::filesystem::path root;
    std::filesystem::path profile;
    std::filesystem::path storage;
    std::filesystem::path cache;
    std::filesystem::path session;
    std::filesystem::path bridgePolicy;
    std::filesystem::path control;

    [[nodiscard]] static std::filesystem::path previewRoot();
    [[nodiscard]] static std::filesystem::path webKitRoot();
    [[nodiscard]] static ProfilePaths forProfile(std::string_view profileId);

    void validateIsolation() const;
    void createDirectories() const;
};

} // namespace yobro::core
