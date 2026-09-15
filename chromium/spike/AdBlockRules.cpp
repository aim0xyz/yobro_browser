#include "spike/AdBlockRules.hpp"

#include <QHostAddress>
#include <QUrlQuery>

#include <algorithm>

namespace yobro::spike {
namespace {

/// Multi-label suffixes that must not be mistaken for a registrable domain.
/// A full public suffix list would be more precise, but this covers the cases
/// that matter for deciding whether two hosts belong to the same site.
const std::vector<QString> &knownMultiLabelSuffixes() {
    static const std::vector<QString> suffixes{
        QStringLiteral("co.uk"), QStringLiteral("org.uk"), QStringLiteral("ac.uk"),
        QStringLiteral("gov.uk"), QStringLiteral("me.uk"), QStringLiteral("com.au"),
        QStringLiteral("net.au"), QStringLiteral("org.au"), QStringLiteral("co.nz"),
        QStringLiteral("co.jp"), QStringLiteral("or.jp"), QStringLiteral("com.br"),
        QStringLiteral("com.mx"), QStringLiteral("com.ar"), QStringLiteral("com.tr"),
        QStringLiteral("com.cn"), QStringLiteral("com.hk"), QStringLiteral("com.sg"),
        QStringLiteral("co.in"), QStringLiteral("co.za"), QStringLiteral("com.pl"),
    };
    return suffixes;
}

} // namespace

const std::vector<QString> &AdBlockRules::blockedDomains() {
    static const std::vector<QString> domains{
        QStringLiteral("2mdn.net"), QStringLiteral("33across.com"), QStringLiteral("360yield.com"), QStringLiteral("4dsply.com"),
        QStringLiteral("ad.gt"), QStringLiteral("adform.net"), QStringLiteral("adition.com"), QStringLiteral("adnxs.com"),
        QStringLiteral("adroll.com"), QStringLiteral("adsafeprotected.com"), QStringLiteral("adsrvr.org"), QStringLiteral("ads-twitter.com"),
        QStringLiteral("adscale.de"), QStringLiteral("adservice.google.com"), QStringLiteral("amazon-adsystem.com"), QStringLiteral("amplitude.com"),
        QStringLiteral("app-measurement.com"), QStringLiteral("appsflyer.com"), QStringLiteral("atdmt.com"), QStringLiteral("bingads.microsoft.com"),
        QStringLiteral("bluekai.com"), QStringLiteral("branch.io"), QStringLiteral("casalemedia.com"), QStringLiteral("chartbeat.com"),
        QStringLiteral("clarity.ms"), QStringLiteral("contextweb.com"), QStringLiteral("criteo.com"), QStringLiteral("criteo.net"),
        QStringLiteral("demdex.net"), QStringLiteral("districtm.io"), QStringLiteral("doubleclick.net"), QStringLiteral("doubleverify.com"),
        QStringLiteral("everesttech.net"), QStringLiteral("exelator.com"), QStringLiteral("facebook.net"), QStringLiteral("flashtalking.com"),
        QStringLiteral("gemius.pl"), QStringLiteral("google-analytics.com"), QStringLiteral("googleadservices.com"), QStringLiteral("googlesyndication.com"),
        QStringLiteral("googletagmanager.com"), QStringLiteral("gumgum.com"), QStringLiteral("heapanalytics.com"), QStringLiteral("hotjar.com"),
        QStringLiteral("imrworldwide.com"), QStringLiteral("indexww.com"), QStringLiteral("innovid.com"), QStringLiteral("inmobi.com"),
        QStringLiteral("krxd.net"), QStringLiteral("lijit.com"), QStringLiteral("linkedin.com"), QStringLiteral("lkqd.net"),
        QStringLiteral("mathtag.com"), QStringLiteral("media.net"), QStringLiteral("mediavine.com"), QStringLiteral("mixpanel.com"),
        QStringLiteral("moatads.com"), QStringLiteral("mookie1.com"), QStringLiteral("newrelic.com"), QStringLiteral("nexac.com"),
        QStringLiteral("omtrdc.net"), QStringLiteral("onaudience.com"), QStringLiteral("openx.net"), QStringLiteral("optimizely.com"),
        QStringLiteral("outbrain.com"), QStringLiteral("pardot.com"), QStringLiteral("perfectaudience.com"), QStringLiteral("permutive.com"),
        QStringLiteral("postrelease.com"), QStringLiteral("pubmatic.com"), QStringLiteral("quantcast.com"), QStringLiteral("quantserve.com"),
        QStringLiteral("raygun.io"), QStringLiteral("revcontent.com"), QStringLiteral("rfihub.com"), QStringLiteral("rlcdn.com"),
        QStringLiteral("rubiconproject.com"), QStringLiteral("samba.tv"), QStringLiteral("scorecardresearch.com"), QStringLiteral("segment.com"),
        QStringLiteral("segment.io"), QStringLiteral("serving-sys.com"), QStringLiteral("sharethrough.com"), QStringLiteral("smartadserver.com"),
        QStringLiteral("snapads.com"), QStringLiteral("spotxchange.com"), QStringLiteral("stackadapt.com"), QStringLiteral("taboola.com"),
        QStringLiteral("tapad.com"), QStringLiteral("teads.tv"), QStringLiteral("thetradedesk.com"), QStringLiteral("triplelift.com"),
        QStringLiteral("turn.com"), QStringLiteral("undertone.com"), QStringLiteral("unrulymedia.com"), QStringLiteral("usebutton.com"),
        QStringLiteral("viglink.com"), QStringLiteral("weborama.com"), QStringLiteral("yieldmo.com"), QStringLiteral("zedo.com"),
        QStringLiteral("zemanta.com"),
    };
    return domains;
}

const std::vector<AdBlockRules::PathPattern> &AdBlockRules::thirdPartyPathPatterns() {
    // The delimiters mirror the WebKit character classes, e.g. `/prebid[./?]`
    // also matches `prebid.js`.
    static const QString slashOrQuery = QStringLiteral("/?");
    static const std::vector<PathPattern> patterns{
        {QStringLiteral("ad"), slashOrQuery},
        {QStringLiteral("ads"), slashOrQuery},
        {QStringLiteral("adserver"), slashOrQuery},
        {QStringLiteral("advert"), slashOrQuery},
        {QStringLiteral("advertising"), slashOrQuery},
        {QStringLiteral("analytics"), slashOrQuery},
        {QStringLiteral("collect"), slashOrQuery},
        {QStringLiteral("pixel"), slashOrQuery},
        {QStringLiteral("tracking"), slashOrQuery},
        {QStringLiteral("telemetry"), slashOrQuery},
        {QStringLiteral("beacon"), slashOrQuery},
        {QStringLiteral("prebid"), QStringLiteral("./?")},
        {QStringLiteral("vast"), slashOrQuery},
        {QStringLiteral("pagead"), slashOrQuery},
    };
    return patterns;
}

const std::vector<QString> &AdBlockRules::trackingParameters() {
    static const std::vector<QString> parameters{
        QStringLiteral("fbclid"), QStringLiteral("gclid"), QStringLiteral("dclid"),
        QStringLiteral("gbraid"), QStringLiteral("wbraid"), QStringLiteral("msclkid"),
        QStringLiteral("twclid"), QStringLiteral("ttclid"), QStringLiteral("igshid"),
        QStringLiteral("mc_cid"), QStringLiteral("mc_eid"), QStringLiteral("mkt_tok"),
        QStringLiteral("vero_conv"), QStringLiteral("vero_id"), QStringLiteral("oly_anon_id"),
        QStringLiteral("oly_enc_id"), QStringLiteral("rb_clickid"), QStringLiteral("s_cid"),
        QStringLiteral("wickedid"), QStringLiteral("yclid"), QStringLiteral("_hsenc"),
        QStringLiteral("_hsmi"),
    };
    return parameters;
}

QString AdBlockRules::registrableDomain(const QString &host) {
    const QString lower = host.toLower();
    // An address literal has no registrable domain; splitting it into labels
    // would produce nonsense like "0.1" for 127.0.0.1.
    if (!QHostAddress(lower).isNull()) return lower;
    const QStringList labels = lower.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() <= 2) return lower;
    const QString lastTwo = labels.at(labels.size() - 2) + QLatin1Char('.') + labels.last();
    const auto &suffixes = knownMultiLabelSuffixes();
    const bool multiLabel = std::find(suffixes.begin(), suffixes.end(), lastTwo) != suffixes.end();
    if (!multiLabel) return lastTwo;
    if (labels.size() == 3) return lower;
    return labels.at(labels.size() - 3) + QLatin1Char('.') + lastTwo;
}

bool AdBlockRules::isThirdParty(const QUrl &request, const QUrl &firstParty) {
    const QString requestHost = request.host();
    const QString firstPartyHost = firstParty.host();
    if (requestHost.isEmpty() || firstPartyHost.isEmpty()) return false;
    return registrableDomain(requestHost) != registrableDomain(firstPartyHost);
}

bool AdBlockRules::isBlockedDomain(const QUrl &request) {
    const QString host = request.host().toLower();
    if (host.isEmpty()) return false;
    for (const QString &domain : blockedDomains()) {
        if (host == domain) return true;
        if (host.endsWith(QLatin1Char('.') + domain)) return true;
    }
    return false;
}

bool AdBlockRules::hasAdvertisingPath(const QUrl &request) {
    // The delimiter after the fragment may be the query separator, so the path
    // is compared with a trailing '?' when a query exists. A path that simply
    // ends after the fragment is not a match, matching the WebKit rules.
    QString path = request.path(QUrl::FullyEncoded);
    if (path.isEmpty()) return false;
    if (request.hasQuery()) path += QLatin1Char('?');
    const QString lower = path.toLower();
    for (const PathPattern &pattern : thirdPartyPathPatterns()) {
        const QString prefix = QLatin1Char('/') + pattern.fragment;
        int index = lower.indexOf(prefix);
        while (index >= 0) {
            const int followerIndex = index + prefix.size();
            if (followerIndex < lower.size() && pattern.followers.contains(lower.at(followerIndex)))
                return true;
            index = lower.indexOf(prefix, index + 1);
        }
    }
    return false;
}

bool AdBlockRules::blocks(const QUrl &request, const QUrl &firstParty, bool pathRulesApply) {
    const QString scheme = request.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return false;
    if (!isThirdParty(request, firstParty)) return false;
    if (isBlockedDomain(request)) return true;
    return pathRulesApply && hasAdvertisingPath(request);
}

QUrl AdBlockRules::withoutTrackingParameters(const QUrl &url) {
    if (!url.hasQuery()) return url;
    QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyEncoded);
    if (items.isEmpty()) return url;
    QList<QPair<QString, QString>> kept;
    for (const auto &item : items) {
        const QString name = item.first.toLower();
        const bool tracking = name.startsWith(QStringLiteral("utm_"))
            || std::find(trackingParameters().begin(), trackingParameters().end(), name)
                != trackingParameters().end();
        if (!tracking) kept.append(item);
    }
    if (kept.size() == items.size()) return url;
    QUrl cleaned = url;
    if (kept.isEmpty()) {
        cleaned.setQuery(QString());
    } else {
        QUrlQuery remaining;
        remaining.setQueryItems(kept);
        cleaned.setQuery(remaining.query(QUrl::FullyEncoded));
    }
    return cleaned;
}

} // namespace yobro::spike
