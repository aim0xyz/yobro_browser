#include "spike/CertificateTrust.hpp"

#include <QStringList>

namespace yobro::spike {
namespace {

/// The private and reserved IPv4 ranges. A host outside them may be a public
/// website and therefore never qualifies for an exception.
bool isPrivateIPv4(const QString &host) {
    const QStringList parts = host.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    int octets[4] = {0, 0, 0, 0};
    for (int index = 0; index < 4; ++index) {
        bool valid = false;
        const int value = parts.at(index).toInt(&valid);
        if (!valid || value < 0 || value > 255) return false;
        octets[index] = value;
    }
    if (octets[0] == 10) return true;                          // 10.0.0.0/8
    if (octets[0] == 127) return true;                         // loopback
    if (octets[0] == 169 && octets[1] == 254) return true;     // link-local
    if (octets[0] == 192 && octets[1] == 168) return true;     // 192.168.0.0/16
    if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true; // 172.16.0.0/12
    return false;
}

} // namespace

bool CertificateTrustStore::isLocal(const QString &host) {
    const QString name = host.toLower();
    if (name == QStringLiteral("localhost") || name == QStringLiteral("::1")) return true;
    for (const QString &suffix : {
             QStringLiteral(".localhost"), QStringLiteral(".local"), QStringLiteral(".internal"),
             QStringLiteral(".test"), QStringLiteral(".home.arpa"),
         }) {
        if (name.endsWith(suffix)) return true;
    }
    return isPrivateIPv4(name);
}

bool CertificateTrustStore::isAccepted(const QString &host, const QString &fingerprint) const {
    const auto found = accepted_.find(host.toLower().toStdString());
    return found != accepted_.end() && found->second == fingerprint.toStdString();
}

void CertificateTrustStore::accept(const QString &host, const QString &fingerprint) {
    accepted_[host.toLower().toStdString()] = fingerprint.toStdString();
}

void CertificateTrustStore::forget(const QString &host) {
    accepted_.erase(host.toLower().toStdString());
}

std::vector<QString> CertificateTrustStore::hosts() const {
    std::vector<QString> values;
    values.reserve(accepted_.size());
    // std::map already orders by host.
    for (const auto &entry : accepted_) values.push_back(QString::fromStdString(entry.first));
    return values;
}

} // namespace yobro::spike
