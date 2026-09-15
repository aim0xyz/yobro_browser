#pragma once

#include <QString>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace yobro::spike {

/// Session-only certificate exceptions for local development hosts.
///
/// The rule is the same as in the WebKit build: for anything that could be a
/// public website the engine's verdict stands and there is no way to click
/// through, because that button is the classic path into a machine-in-the-middle
/// attack. Only hosts that cannot be a public website may be excepted, the user
/// has to see the fingerprint first, and nothing is written to disk.
///
/// One difference to WebKit is worth knowing: once a certificate is accepted,
/// Chromium remembers it for the life of the profile and stops reporting the
/// error, so this store is not asked again for that host. The exception still
/// dies with the process, but a certificate swapped in the same session would
/// not trigger a second question.
class CertificateTrustStore {
public:
    /// True for hosts that cannot be a public website: loopback, link-local, the
    /// private IPv4 ranges and the reserved local-only suffixes.
    [[nodiscard]] static bool isLocal(const QString &host);

    /// The exception is keyed by host *and* fingerprint, so a swapped
    /// certificate asks again instead of inheriting the previous approval.
    [[nodiscard]] bool isAccepted(const QString &host, const QString &fingerprint) const;
    void accept(const QString &host, const QString &fingerprint);
    void forget(const QString &host);
    [[nodiscard]] std::size_t exceptionCount() const { return accepted_.size(); }
    /// The excepted hosts, sorted, for the settings screen.
    [[nodiscard]] std::vector<QString> hosts() const;

private:
    std::map<std::string, std::string> accepted_;
};

} // namespace yobro::spike
