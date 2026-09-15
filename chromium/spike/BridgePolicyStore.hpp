#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace yobro::spike {

struct BridgePolicyLoadResult {
    bool allowsLibraryAccess = false;
    std::optional<std::string> problem;
    std::optional<std::filesystem::path> quarantinedPath;
};

class BridgePolicyStore final {
public:
    using Apply = std::function<void(bool)>;

    explicit BridgePolicyStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] BridgePolicyLoadResult load() const;
    void save(bool allowsLibraryAccess) const;

    // The callback is invoked only after the new policy is durably committed.
    // A persistence failure therefore leaves the running session unchanged.
    [[nodiscard]] std::optional<std::string> persistAndApply(
        bool allowsLibraryAccess,
        const Apply &apply
    ) const;

private:
    [[nodiscard]] BridgePolicyLoadResult quarantine(std::string reason) const;

    std::filesystem::path path_;
};

} // namespace yobro::spike
