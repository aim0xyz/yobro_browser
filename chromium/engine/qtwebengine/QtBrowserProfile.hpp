#pragma once

#include "engine/api/BrowserEngine.hpp"

#include <memory>

class QWebEngineProfile;
class QWebEngineExtensionInfo;

namespace yobro::qtwebengine {

class QtBrowserProfile final : public engine::BrowserProfile {
public:
    explicit QtBrowserProfile(engine::ProfileSpec spec);
    ~QtBrowserProfile() override;

    [[nodiscard]] const engine::ProfileSpec &spec() const override;
    [[nodiscard]] std::unique_ptr<engine::BrowserPage> createPage(
        std::string id,
        engine::PageOwner owner,
        bool privatePage = false
    ) override;

    [[nodiscard]] QWebEngineProfile *persistentProfile() const;
    [[nodiscard]] QWebEngineProfile *privateProfile() const;
    void setExtensionEnabled(const QWebEngineExtensionInfo &extension, bool enabled);
    /// Turns a freshly installed extension on. Unlike `setExtensionEnabled` this
    /// is safe to call straight from `installFinished`: the switch is deferred
    /// until the extension backend has settled, because switching it earlier
    /// crashes inside Qt 6.11.
    void enableExtensionAfterInstall(const QWebEngineExtensionInfo &extension);
    /// How long `enableExtensionAfterInstall` waits. Exposed so tests can wait
    /// for at least this long.
    static constexpr int installSettleDelayMilliseconds = 1'200;
    /// False while Qt's `setExtensionEnabled` is unusable. An extension can then
    /// be installed and loaded, but not switched on, so its content scripts do
    /// not run. See the definition for what was measured.
    [[nodiscard]] static bool extensionSwitchingWorks();
    /// False while a passkey request never finishes in this Qt build. See the
    /// definition for what was measured. While it is false the shell makes such
    /// a request fail fast, so a site offers another sign-in method instead of
    /// waiting forever.
    [[nodiscard]] static bool passkeysWork();
    void forgetExtensionState(const QWebEngineExtensionInfo &extension);
    void restoreExtensionState(const QWebEngineExtensionInfo &extension);
    void loadInstalledExtensions();

private:
    /// Switches an extension only when the state actually changes, using the
    /// manager's own current info object. Does nothing while
    /// `extensionSwitchingWorks()` is false.
    void applyExtensionEnabled(const QWebEngineExtensionInfo &extension, bool enabled);

    engine::ProfileSpec spec_;
    std::unique_ptr<QWebEngineProfile> persistentProfile_;
    std::unique_ptr<QWebEngineProfile> privateProfile_;
};

class QtBrowserEngine final : public engine::BrowserEngine {
public:
    [[nodiscard]] std::unique_ptr<engine::BrowserProfile> openProfile(engine::ProfileSpec spec) override;
};

} // namespace yobro::qtwebengine
