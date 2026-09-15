#pragma once

#include "spike/MailStore.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace yobro::spike {

/// What a lookup produced: the server settings plus what the user needs to know.
struct MailDiscoveryResult {
    MailAccount account;
    QString explanation;
    /// True when the provider accepts only OAuth, which this shell cannot do.
    bool oauthOnly = false;
    /// Where the settings came from, so a wrong guess can be recognised.
    QString source;
    /// Empty unless nothing usable was found.
    QString problem;
};

/// Finds IMAP and SMTP settings for an address, like `MailDiscovery.swift`.
class MailDiscovery {
public:
    /// The provider templates offered in the account form, in the same order as
    /// the WebKit build.
    [[nodiscard]] static QStringList presetNames();
    [[nodiscard]] static MailDiscoveryResult preset(
        const QString &name,
        const QString &address,
        const QString &id
    );

    /// The autoconfig endpoints tried for a domain, in order.
    ///
    /// All three are HTTPS; a plain HTTP endpoint would let anyone on the path
    /// pick the mail server the password is sent to.
    [[nodiscard]] static QStringList autoconfigEndpoints(const QString &domain);

    /// The domain part of an address in lower case, or empty when the address is
    /// not usable.
    [[nodiscard]] static QString domainOf(const QString &address);

    /// Reads a Thunderbird-style autoconfig document.
    ///
    /// Only an encrypted IMAP server and an encrypted SMTP server are accepted;
    /// a plaintext entry is ignored rather than downgraded. External entities
    /// are never resolved.
    [[nodiscard]] static MailDiscoveryResult parseAutoconfig(
        const QByteArray &xml,
        const QString &address,
        const QString &id,
        const QString &source
    );

    /// Presets first, then the autoconfig endpoints over the network.
    static void discover(
        const QString &address,
        const QString &id,
        QObject *context,
        std::function<void(MailDiscoveryResult)> done
    );

    /// The largest autoconfig document that is read, as in the WebKit build.
    static constexpr int maximumDocumentBytes = 262144;
};

} // namespace yobro::spike
