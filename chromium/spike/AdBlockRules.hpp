#pragma once

#include <QString>
#include <QUrl>

#include <vector>

namespace yobro::spike {

/// The network filter shared in spirit with the WebKit build's `AdBlocker`.
///
/// WebKit compiles a JSON rule list into its content blocker. Qt WebEngine has
/// no such compiler, so the same rules are evaluated here and applied through a
/// request interceptor. The lists are deliberately kept byte-identical to
/// `Sources/YOBRO/AdBlocker.swift`; `chromium-adblock` fails when they drift.
class AdBlockRules {
public:
    /// Third-party hosts that are blocked outright, with their subdomains.
    [[nodiscard]] static const std::vector<QString> &blockedDomains();

    /// Path fragments that mark a third-party request as advertising. Each entry
    /// is the fragment without its trailing delimiter, paired with the
    /// delimiters that may follow it.
    struct PathPattern {
        QString fragment;
        QString followers;
    };
    [[nodiscard]] static const std::vector<PathPattern> &thirdPartyPathPatterns();

    /// Query parameters that only exist to identify the visitor.
    [[nodiscard]] static const std::vector<QString> &trackingParameters();

    /// True when `request` is loaded from a different site than `firstParty`.
    /// An empty first party means a top-level load, which is never third party.
    [[nodiscard]] static bool isThirdParty(const QUrl &request, const QUrl &firstParty);

    /// True when a blocked domain covers this host, either exactly or as a
    /// parent domain.
    [[nodiscard]] static bool isBlockedDomain(const QUrl &request);

    /// True when the path marks the request as advertising or tracking.
    [[nodiscard]] static bool hasAdvertisingPath(const QUrl &request);

    /// The decision for one request. `subresourceKind` tells whether the request
    /// is the kind of subresource the path rules apply to; the domain rules
    /// apply to every kind.
    [[nodiscard]] static bool blocks(
        const QUrl &request,
        const QUrl &firstParty,
        bool pathRulesApply
    );

    /// The URL without tracking parameters, or the unchanged URL when it carries
    /// none. Everything starting with `utm_` is dropped as well.
    [[nodiscard]] static QUrl withoutTrackingParameters(const QUrl &url);

    /// The registrable part of a host, e.g. `ads.example.co.uk` -> `example.co.uk`.
    /// This uses a short list of known multi-label suffixes rather than the full
    /// public suffix list, which is enough for same-site comparisons here.
    [[nodiscard]] static QString registrableDomain(const QString &host);
};

} // namespace yobro::spike
