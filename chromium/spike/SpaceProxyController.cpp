#include "spike/SpaceProxyController.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace yobro::spike {
namespace {

/// The exceptions Chromium applies by itself. Anything beyond these cannot be
/// enforced through the application proxy.
bool isLoopbackRule(const QString &rule) {
    QString value = rule.trimmed().toLower();
    if (value.startsWith(QLatin1Char('[')) && value.endsWith(QLatin1Char(']')))
        value = value.mid(1, value.size() - 2);
    return value == QStringLiteral("localhost") || value == QStringLiteral("127.0.0.1")
        || value == QStringLiteral("::1");
}

} // namespace

QString SpaceProxyController::spaceFromSession(const std::filesystem::path &sessionFile) {
    QFile input(QString::fromStdString(sessionFile.string()));
    if (!input.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(input.readAll())
        .object()
        .value(QStringLiteral("activeSpace"))
        .toString();
}

bool SpaceProxyController::hasUnenforceableRules(const SpaceProxyConfig &config) {
    for (const QString &rule : config.matchDomains) {
        if (!rule.trimmed().isEmpty()) return true;
    }
    for (const QString &rule : config.excludedDomains) {
        if (!rule.trimmed().isEmpty() && !isLoopbackRule(rule)) return true;
    }
    return false;
}

QNetworkProxy SpaceProxyController::proxyFor(const std::optional<SpaceProxyConfig> &config) {
    if (!config || !config->enabled || !config->validationProblem().isEmpty())
        return QNetworkProxy(QNetworkProxy::NoProxy);
    QNetworkProxy proxy(
        config->type == SpaceProxyType::socks5
            ? QNetworkProxy::Socks5Proxy
            : QNetworkProxy::HttpProxy,
        config->cleanHost(),
        static_cast<quint16>(config->port),
        config->username,
        config->password
    );
    // Host name lookup has to happen at the proxy; resolving locally would leak
    // the visited names past a SOCKS5 proxy.
    proxy.setCapabilities(
        QNetworkProxy::TunnelingCapability | QNetworkProxy::CachingCapability
        | QNetworkProxy::HostNameLookupCapability
    );
    return proxy;
}

QString SpaceProxyController::apply(const std::optional<SpaceProxyConfig> &config) {
    if (config && config->enabled) {
        if (const QString problem = config->validationProblem(); !problem.isEmpty()) {
            QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
            return problem;
        }
    }
    QNetworkProxy::setApplicationProxy(proxyFor(config));
    return {};
}

bool SpaceProxyController::matchesApplied(const std::optional<SpaceProxyConfig> &config) {
    const QNetworkProxy wanted = proxyFor(config);
    const QNetworkProxy applied = QNetworkProxy::applicationProxy();
    if (wanted.type() != applied.type()) return false;
    if (wanted.type() == QNetworkProxy::NoProxy) return true;
    return wanted.hostName() == applied.hostName()
        && wanted.port() == applied.port()
        && wanted.user() == applied.user();
}

QString SpaceProxyController::appliedLabel() {
    const QNetworkProxy applied = QNetworkProxy::applicationProxy();
    if (applied.type() == QNetworkProxy::NoProxy) return {};
    const QString kind = applied.type() == QNetworkProxy::Socks5Proxy
        ? QStringLiteral("SOCKS5")
        : QStringLiteral("HTTP CONNECT");
    return QStringLiteral("%1 %2:%3").arg(kind, applied.hostName()).arg(applied.port());
}

} // namespace yobro::spike
