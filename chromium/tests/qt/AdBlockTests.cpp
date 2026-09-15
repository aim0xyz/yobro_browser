#include "spike/AdBlockRules.hpp"
#include "spike/AdBlockSettings.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::AdBlockRules;
using yobro::spike::AdBlockSettings;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

/// Everything inside the Swift array that follows `marker`. Brackets inside
/// string literals are skipped, because patterns like "/ad[/?]" contain them.
QString swiftArrayBody(const QString &source, const QString &marker) {
    const int start = source.indexOf(marker);
    if (start < 0) fail("Could not find " + marker.toStdString() + " in the WebKit ad blocker.");
    const int open = source.indexOf(QLatin1Char('['), start);
    if (open < 0) fail("Malformed Swift array for " + marker.toStdString() + ".");
    int depth = 0;
    bool inString = false;
    for (int index = open; index < source.size(); ++index) {
        const QChar character = source.at(index);
        if (inString) {
            if (character == QLatin1Char('\\')) ++index;
            else if (character == QLatin1Char('"')) inString = false;
            continue;
        }
        if (character == QLatin1Char('"')) inString = true;
        else if (character == QLatin1Char('[')) ++depth;
        else if (character == QLatin1Char(']')) {
            if (--depth == 0) return source.mid(open + 1, index - open - 1);
        }
    }
    fail("Unterminated Swift array for " + marker.toStdString() + ".");
}

QStringList swiftStrings(const QString &body) {
    QStringList values;
    const QRegularExpression pattern(QStringLiteral("\"([^\"]+)\""));
    auto matches = pattern.globalMatch(body);
    while (matches.hasNext()) values.append(matches.next().captured(1));
    return values;
}

/// The C++ rules are a reimplementation of the WebKit rule list. If the two
/// drift apart, one engine blocks more than the other, which is exactly the kind
/// of silent difference this test exists to prevent.
void checkListParityWithWebKit() {
    QFile file(QStringLiteral(YOBRO_WEBKIT_ADBLOCK_SOURCE));
    check(file.open(QIODevice::ReadOnly | QIODevice::Text),
          "Could not read the WebKit ad blocker source.");
    const QString source = QString::fromUtf8(file.readAll());

    const QStringList swiftDomains = swiftStrings(swiftArrayBody(source, QStringLiteral("blockedDomains")));
    QStringList cppDomains;
    for (const QString &domain : AdBlockRules::blockedDomains()) cppDomains.append(domain);
    check(swiftDomains.size() == 101, "The WebKit domain list was not parsed as expected: "
          + std::to_string(swiftDomains.size()) + " entries.");
    check(swiftDomains == cppDomains,
          "The blocked domain lists of the two engines differ. Sizes: WebKit "
          + std::to_string(swiftDomains.size()) + ", Chromium " + std::to_string(cppDomains.size()) + ".");

    // The Swift patterns carry their delimiters as a character class, e.g.
    // "/prebid[./?]". Rebuild that form from the C++ table and compare.
    const QStringList swiftPaths = swiftStrings(swiftArrayBody(source, QStringLiteral("thirdPartyPathPatterns")));
    QStringList cppPaths;
    for (const AdBlockRules::PathPattern &pattern : AdBlockRules::thirdPartyPathPatterns()) {
        cppPaths.append(QStringLiteral("/%1[%2]").arg(pattern.fragment, pattern.followers));
    }
    check(swiftPaths.size() == 14, "The WebKit path pattern list was not parsed as expected: "
          + std::to_string(swiftPaths.size()) + " entries.");
    check(swiftPaths == cppPaths, "The advertising path patterns of the two engines differ.");

    const QStringList swiftParameters = swiftStrings(swiftArrayBody(source, QStringLiteral("trackingParameters")));
    QSet<QString> swiftSet(swiftParameters.begin(), swiftParameters.end());
    QSet<QString> cppSet;
    for (const QString &name : AdBlockRules::trackingParameters()) cppSet.insert(name);
    check(swiftSet.size() == 22, "The WebKit tracking parameter list was not parsed as expected: "
          + std::to_string(swiftSet.size()) + " entries.");
    check(swiftSet == cppSet, "The tracking parameter lists of the two engines differ.");
}

void checkDomainRules() {
    const QUrl site(QStringLiteral("https://news.example.test/article"));

    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://doubleclick.net/tag.js")), site, true),
          "A known advertising domain was not blocked.");
    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://stats.g.doubleclick.net/collect")), site, false),
          "A subdomain of a blocked domain was not blocked.");
    // Domain rules must not depend on the resource kind.
    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://googletagmanager.com/gtm.js")), site, false),
          "A blocked domain was allowed because the path rules did not apply.");

    // A host that merely ends in the same letters must survive.
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://notdoubleclick.net/tag.js")), site, true),
          "A host that only looks similar was blocked.");
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.example.test/app.js")), site, true),
          "An unrelated third-party host was blocked.");

    // The list contains real sites like linkedin.com. Visiting them directly has
    // to keep working; only their third-party use is blocked.
    const QUrl linkedIn(QStringLiteral("https://www.linkedin.com/feed"));
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://www.linkedin.com/api/feed")), linkedIn, true),
          "A first-party request to a listed site was blocked.");
    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://px.ads.linkedin.com/collect")), site, true),
          "The same site was not blocked as a third party.");

    // Non-web schemes are none of the filter's business.
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("data:text/plain,doubleclick.net")), site, true),
          "A data URL was treated as a network request.");
}

void checkSameSiteDetection() {
    check(AdBlockRules::registrableDomain(QStringLiteral("ads.example.co.uk")) == QStringLiteral("example.co.uk"),
          "A multi-label suffix was mistaken for the registrable domain.");
    check(AdBlockRules::registrableDomain(QStringLiteral("example.co.uk")) == QStringLiteral("example.co.uk"),
          "A bare registrable domain was shortened.");
    check(AdBlockRules::registrableDomain(QStringLiteral("a.b.example.test")) == QStringLiteral("example.test"),
          "A deep subdomain did not reduce to its registrable domain.");
    // Address literals have no registrable domain and must stay whole.
    check(AdBlockRules::registrableDomain(QStringLiteral("127.0.0.1")) == QStringLiteral("127.0.0.1"),
          "An IPv4 literal was split into labels.");
    check(AdBlockRules::registrableDomain(QStringLiteral("::1")) == QStringLiteral("::1"),
          "An IPv6 literal was altered.");
    check(!AdBlockRules::isThirdParty(
              QUrl(QStringLiteral("http://127.0.0.1:8080/a.js")),
              QUrl(QStringLiteral("http://127.0.0.1:9090/"))
          ),
          "The same address on another port was treated as third party.");

    check(!AdBlockRules::isThirdParty(
              QUrl(QStringLiteral("https://static.example.test/a.js")),
              QUrl(QStringLiteral("https://www.example.test/"))
          ),
          "A sibling subdomain was treated as third party.");
    check(!AdBlockRules::isThirdParty(
              QUrl(QStringLiteral("https://cdn.example.co.uk/a.js")),
              QUrl(QStringLiteral("https://www.example.co.uk/"))
          ),
          "A sibling subdomain under a multi-label suffix was treated as third party.");
    check(AdBlockRules::isThirdParty(
              QUrl(QStringLiteral("https://other.test/a.js")),
              QUrl(QStringLiteral("https://www.example.test/"))
          ),
          "A genuinely different site was treated as first party.");
    // Without a first party there is nothing to compare, so nothing is blocked.
    check(!AdBlockRules::isThirdParty(QUrl(QStringLiteral("https://doubleclick.net/")), QUrl()),
          "A top-level load was treated as third party.");
}

void checkPathRules() {
    const QUrl site(QStringLiteral("https://news.example.test/article"));

    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/ads/banner.png")), site, true),
          "An advertising path was not blocked.");
    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/collect?id=1")), site, true),
          "A tracking path ending in a query was not blocked.");
    check(AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/js/prebid.js")), site, true),
          "The prebid pattern did not accept a dot as delimiter.");

    // The fragment needs its delimiter; a path that simply ends there is not a
    // match, and neither is a longer word that happens to start with it.
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/ads")), site, true),
          "A path without a delimiter was blocked.");
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/adsense-help/index.html")), site, true),
          "A longer word starting with the fragment was blocked.");
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://cdn.other.test/download/adapter.js")), site, true),
          "A path containing the fragment mid-word was blocked.");

    // Documents and form posts must never disappear because of a path match.
    check(!AdBlockRules::blocks(QUrl(QStringLiteral("https://shop.other.test/ads/offer")), site, false),
          "A document navigation was blocked by a path rule.");

    // First-party advertising paths stay, matching the WebKit rule list.
    check(!AdBlockRules::blocks(
              QUrl(QStringLiteral("https://news.example.test/analytics/page")),
              site,
              true
          ),
          "A first-party analytics path was blocked.");
}

void checkTrackingParameters() {
    const QUrl mixed(QStringLiteral("https://shop.test/item?id=7&utm_source=mail&gclid=abc&ref=friend"));
    const QUrl cleaned = AdBlockRules::withoutTrackingParameters(mixed);
    check(cleaned.toString() == QStringLiteral("https://shop.test/item?id=7&ref=friend"),
          "Tracking parameters were not removed correctly: " + cleaned.toString().toStdString());

    const QUrl onlyTracking(QStringLiteral("https://shop.test/item?fbclid=1&UTM_Medium=x"));
    const QUrl stripped = AdBlockRules::withoutTrackingParameters(onlyTracking);
    check(!stripped.hasQuery(), "A query consisting only of trackers was not dropped.");
    check(stripped.toString() == QStringLiteral("https://shop.test/item"),
          "Dropping the whole query changed the rest of the address.");

    const QUrl clean(QStringLiteral("https://shop.test/item?id=7"));
    check(AdBlockRules::withoutTrackingParameters(clean) == clean,
          "An address without trackers was modified.");
    const QUrl none(QStringLiteral("https://shop.test/item"));
    check(AdBlockRules::withoutTrackingParameters(none) == none,
          "An address without a query was modified.");
    // A parameter that merely contains a tracker name has to stay.
    const QUrl similar(QStringLiteral("https://shop.test/item?my_gclid=1"));
    check(AdBlockRules::withoutTrackingParameters(similar) == similar,
          "A parameter that only contains a tracker name was removed.");
}

void checkSettings(const std::filesystem::path &directory) {
    {
        AdBlockSettings settings(directory);
        check(settings.enabled(), "Blocking must be on by default, as in the WebKit build.");
        check(settings.strictProtection(), "Strict protection must be on by default.");
        check(!settings.aggressivePageFilters(),
              "The extra page filters must stay off while strict protection is on.");
        settings.setStrictProtection(false);
        check(settings.aggressivePageFilters(),
              "Turning strict protection off did not enable the extra page filters.");
        settings.setEnabled(false);
        check(!settings.aggressivePageFilters(),
              "The extra page filters ran although blocking is off.");
    }
    {
        AdBlockSettings reopened(directory);
        check(!reopened.enabled(), "The blocking switch did not survive a restart.");
        check(!reopened.strictProtection(), "The strict switch did not survive a restart.");
        reopened.setEnabled(true);
    }
    {
        AdBlockSettings reopened(directory);
        check(reopened.enabled() && !reopened.strictProtection(),
              "Re-enabling blocking was not persisted.");
    }
    // A file written by an older build has no strict flag. Protection must not
    // be silently weakened in that case.
    {
        const std::filesystem::path legacyDirectory = directory / "legacy";
        std::filesystem::create_directories(legacyDirectory);
        QFile legacy(QString::fromStdString((legacyDirectory / "adblock.json").string()));
        check(legacy.open(QIODevice::WriteOnly), "Could not write the legacy settings file.");
        legacy.write(QByteArrayLiteral(R"({"enabled":true})"));
        legacy.close();
        AdBlockSettings migrated(legacyDirectory);
        check(migrated.enabled() && migrated.strictProtection(),
              "A settings file without the strict flag did not default to strict.");
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;

    try {
        checkListParityWithWebKit();
        checkDomainRules();
        checkSameSiteDetection();
        checkPathRules();
        checkTrackingParameters();
        checkSettings(std::filesystem::path(root.path().toStdString()));
    } catch (const std::exception &error) {
        std::cerr << "ADBLOCK FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "ADBLOCK PASS\n";
    return 0;
}
