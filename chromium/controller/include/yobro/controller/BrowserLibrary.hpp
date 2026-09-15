#pragma once

#include "engine/api/BrowserEngine.hpp"
#include "yobro/core/Json.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace yobro::controller {

enum class CancelDownloadResult { requested, unknown, inactive };

class BrowserLibrary {
public:
    using DownloadCompletion = std::function<void(std::optional<std::string> error)>;

    virtual ~BrowserLibrary() = default;
    [[nodiscard]] virtual core::Json history(std::string_view query, std::size_t limit) const = 0;
    [[nodiscard]] virtual core::Json downloads() const = 0;
    [[nodiscard]] virtual std::string downloadDirectory() const = 0;
    virtual void recordVisit(const engine::PageState &state) = 0;
    virtual void startDownload(
        engine::BrowserPage &page,
        std::string_view url,
        DownloadCompletion completion
    ) = 0;
    [[nodiscard]] virtual CancelDownloadResult cancelDownload(std::string_view id) = 0;
    virtual void endAgentDownloads() = 0;
    virtual void shutdownDownloads() = 0;
};

} // namespace yobro::controller
