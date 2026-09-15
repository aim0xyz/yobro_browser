#pragma once

#include "yobro/controller/BrowserLibrary.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace yobro::qtwebengine {

class QtBrowserProfile;

class QtBrowserLibrary final : public controller::BrowserLibrary {
public:
    using UserDownloadPrompt = std::function<bool(
        const std::string &suggestedName,
        const std::string &directory
    )>;
    using DownloadsObserver = std::function<void()>;

    QtBrowserLibrary(
        QtBrowserProfile &profile,
        std::filesystem::path profileDirectory,
        std::filesystem::path downloadDirectory = {}
    );
    ~QtBrowserLibrary() override;

    void setUserDownloadPrompt(UserDownloadPrompt prompt);
    void setDownloadsObserver(DownloadsObserver observer);

    [[nodiscard]] core::Json history(std::string_view query, std::size_t limit) const override;
    [[nodiscard]] core::Json bookmarks(std::string_view query, std::size_t limit) const;
    /// Stores a public HTTP(S) bookmark in this profile. Duplicate URLs in the
    /// same folder are retained only once; false means it already existed.
    bool addBookmark(std::string_view title, std::string_view url, std::string_view folder = "Bookmarks");
    bool removeBookmark(std::string_view id);
    /// Changes only the label. An empty title is refused so no entry becomes
    /// unreadable.
    bool renameBookmark(std::string_view id, std::string_view title);
    /// Moves an entry to another folder. Refused when the target folder already
    /// holds this URL, which keeps the no-duplicates rule intact.
    bool moveBookmark(std::string_view id, std::string_view folder);
    /// The folders in use, sorted, so a picker never needs to guess.
    [[nodiscard]] std::vector<std::string> bookmarkFolders() const;
    /// Merges filtered neutral WebKit history into this Chromium profile without
    /// reading or writing the source profile. Returns the number of new URLs.
    std::size_t importHistory(const core::Json::Array &entries);
    /// Removes visits recorded at or after `sinceIso8601`, or the whole history
    /// when the timestamp is empty. Returns how many entries were removed.
    std::size_t clearHistory(std::string_view sinceIso8601 = {});
    [[nodiscard]] core::Json downloads() const override;
    [[nodiscard]] std::string downloadDirectory() const override;
    void recordVisit(const engine::PageState &state) override;
    void startDownload(
        engine::BrowserPage &page,
        std::string_view url,
        DownloadCompletion completion
    ) override;
    [[nodiscard]] controller::CancelDownloadResult cancelDownload(std::string_view id) override;
    void endAgentDownloads() override;
    /// Cancels every transfer and releases every native page this library owns.
    /// The embedder must call this before the profile is released; afterwards
    /// `retainedNativePages()` is zero.
    void shutdownDownloads() override;
    /// How many `QWebEnginePage` objects this library currently keeps alive for
    /// explicit agent downloads. Exists so a test can prove that none of them
    /// outlives the profile.
    [[nodiscard]] std::size_t retainedNativePages() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yobro::qtwebengine
