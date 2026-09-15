#pragma once

#include <QWebEngineUrlRequestInterceptor>

#include <atomic>
#include <cstdint>

namespace yobro::spike {

/// Applies `AdBlockRules` to every request of a profile.
///
/// The WebKit build hands its rules to WebKit's own content blocker. Qt WebEngine
/// has no equivalent, so the same decisions are made here. Tracking parameters
/// are stripped by redirecting top-level navigations, which mirrors the reload
/// the WebKit build performs in its navigation delegate.
class AdBlockInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
    explicit AdBlockInterceptor(QObject *parent = nullptr);

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    /// Safe to call from the UI thread while requests are being intercepted.
    void setEnabled(bool value);
    [[nodiscard]] bool isEnabled() const { return enabled_.load(); }

    /// How many requests have been blocked since the process started. Used by
    /// the settings screen and by the test.
    [[nodiscard]] std::uint64_t blockedCount() const { return blocked_.load(); }

private:
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint64_t> blocked_{0};
};

} // namespace yobro::spike
