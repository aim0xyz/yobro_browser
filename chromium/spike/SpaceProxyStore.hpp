#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace yobro::spike {

/// The proxy kinds offered by the WebKit build.
///
/// Qt can only reach a proxy over plain TCP, so `httpsConnect` cannot be
/// represented. It is still part of the type list so a configuration written by
/// the WebKit build is understood rather than silently reinterpreted, and it is
/// rejected with an explanation instead of being downgraded to an unencrypted
/// connection that would leak the proxy credentials.
enum class SpaceProxyType { socks5, httpConnect, httpsConnect };

struct SpaceProxyConfig {
    bool enabled = true;
    QString label;
    SpaceProxyType type = SpaceProxyType::socks5;
    QString host;
    int port = 1080;
    QString username;
    /// Only set while a configuration is being edited or resolved. The stored
    /// file never contains it; the Keychain does.
    QString password;
    QStringList matchDomains;
    QStringList excludedDomains{
        QStringLiteral("localhost"), QStringLiteral("127.0.0.1"), QStringLiteral("[::1]"),
    };

    /// The host without a scheme prefix or trailing slash.
    [[nodiscard]] QString cleanHost() const;
    /// The label for the settings list, falling back to host and port.
    [[nodiscard]] QString displayLabel() const;
    /// Empty when the configuration can be used, otherwise the reason.
    [[nodiscard]] QString validationProblem() const;
};

/// Per-space proxy configurations, stored like the WebKit build's
/// `space-proxies.json` with the password kept in the macOS Keychain.
///
/// One honest limitation: Qt WebEngine takes its proxy from the process-wide
/// QNetworkProxyFactory, not per profile. Switching spaces therefore switches
/// the proxy for the whole application, including tabs of other spaces that are
/// still open. The WebKit build can scope it to one data store.
class SpaceProxyStore {
public:
    explicit SpaceProxyStore(std::filesystem::path profileDirectory);

    [[nodiscard]] std::optional<SpaceProxyConfig> config(const QString &space) const;
    /// The configured spaces, sorted, for the settings list.
    [[nodiscard]] std::vector<QString> spaces() const;

    /// Stores the configuration and moves its password into the Keychain.
    /// Returns the reason on failure, or an empty string on success.
    QString set(const QString &space, const SpaceProxyConfig &config);
    QString remove(const QString &space);
    /// Moves a configuration and its password to another space name.
    QString rename(const QString &from, const QString &to);

    /// The configuration with its password filled in from the Keychain.
    [[nodiscard]] SpaceProxyConfig resolved(const QString &space, const SpaceProxyConfig &config) const;

    [[nodiscard]] const std::string &keychainService() const { return service_; }

private:
    bool save() const;
    [[nodiscard]] std::optional<QString> readPassword(const QString &space) const;
    QString writePassword(const QString &space, const QString &password);
    void erasePassword(const QString &space);

    std::filesystem::path file_;
    std::string service_;
    std::map<QString, SpaceProxyConfig> configs_;
};

} // namespace yobro::spike
