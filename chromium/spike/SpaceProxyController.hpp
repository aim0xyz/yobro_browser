#pragma once

#include "spike/SpaceProxyStore.hpp"

#include <QNetworkProxy>
#include <QString>

#include <filesystem>
#include <optional>

namespace yobro::spike {

/// Puts one space's proxy in front of the whole application.
///
/// Three limits of Qt WebEngine shape this, all three verified by
/// `chromium-space-proxy-network`:
///
/// 1. Only `QNetworkProxy::setApplicationProxy` is honoured. A
///    `QNetworkProxyFactory` is ignored, so the proxy cannot be chosen per
///    request and there is exactly one proxy per process.
/// 2. The proxy is read once, while the network context is created. Setting it
///    after the first page exists changes nothing, so a different proxy needs a
///    restart. `apply` is therefore only useful before the engine starts;
///    `matchesApplied` tells the shell whether a restart is due.
/// 3. There is no bypass list. Chromium exempts loopback addresses on its own,
///    which covers the WebKit build's default exceptions, but custom match and
///    exception domains cannot be enforced. More traffic goes through the proxy
///    than the lists ask for, never less, so the unenforced part fails on the
///    safe side.
class SpaceProxyController {
public:
    /// The space whose proxy should be in front of the application, taken from
    /// the saved session. Empty when there is no session yet.
    [[nodiscard]] static QString spaceFromSession(const std::filesystem::path &sessionFile);

    /// The proxy for a configuration, or a `NoProxy` value when it is empty,
    /// disabled or unusable.
    [[nodiscard]] static QNetworkProxy proxyFor(const std::optional<SpaceProxyConfig> &config);

    /// Sets the application proxy. Must run before the first page is created;
    /// afterwards Qt WebEngine ignores the change. Returns the reason when the
    /// configuration cannot be used, in which case no proxy is set.
    static QString apply(const std::optional<SpaceProxyConfig> &config);

    /// True when the proxy currently in front of the application is the one this
    /// configuration asks for.
    [[nodiscard]] static bool matchesApplied(const std::optional<SpaceProxyConfig> &config);

    /// A short description of the proxy in force, for the settings screen.
    [[nodiscard]] static QString appliedLabel();

    /// True when the configuration asks for routing rules that Qt WebEngine
    /// cannot enforce, so the shell can say so.
    [[nodiscard]] static bool hasUnenforceableRules(const SpaceProxyConfig &config);
};

} // namespace yobro::spike
