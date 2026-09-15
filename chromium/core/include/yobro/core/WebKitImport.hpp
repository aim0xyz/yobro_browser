#pragma once

#include <cstddef>
#include <filesystem>

namespace yobro::core {

struct WebKitImportPreview {
    std::size_t history = 0;
    std::size_t bookmarks = 0;
    std::size_t tabs = 0;
    std::size_t rejected = 0;
};

class WebKitImport final {
public:
    [[nodiscard]] static WebKitImportPreview preview(const std::filesystem::path &source);
    [[nodiscard]] static WebKitImportPreview commit(
        const std::filesystem::path &source,
        const std::filesystem::path &destination
    );
};

} // namespace yobro::core
