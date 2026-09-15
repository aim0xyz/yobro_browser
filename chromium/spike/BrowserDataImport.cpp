#include "spike/BrowserDataImport.hpp"

#include "spike/ImportWorker.hpp"
#include "spike/Localization.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace yobro::spike {
namespace {

/// Only public web addresses are imported. The script filters as well; this is
/// the second gate, so a future script change cannot slip a `file:` URL through.
bool isWebUrl(const QString &url) {
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty()) return false;
    const QString scheme = parsed.scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

std::vector<ImportedLink> linksOf(const QJsonObject &root, const QString &key) {
    std::vector<ImportedLink> links;
    for (const QJsonValue &value : root.value(key).toArray()) {
        const QJsonObject row = value.toObject();
        const QString url = row.value(QStringLiteral("url")).toString();
        if (!isWebUrl(url)) continue;
        links.push_back({
            row.value(QStringLiteral("title")).toString(),
            url,
            row.value(QStringLiteral("folder")).toString(),
            row.value(QStringLiteral("timestamp")).toDouble(0),
            std::max<long long>(1, static_cast<long long>(row.value(QStringLiteral("visits")).toDouble(1))),
            row.value(QStringLiteral("pinned")).toBool(false),
        });
    }
    return links;
}

std::vector<ImportedCookie> cookiesOf(const QJsonObject &root) {
    std::vector<ImportedCookie> cookies;
    for (const QJsonValue &value : root.value(QStringLiteral("cookies")).toArray()) {
        const QJsonObject row = value.toObject();
        const QString name = row.value(QStringLiteral("name")).toString();
        const QString domain = row.value(QStringLiteral("domain")).toString();
        // A cookie without a name or a domain cannot be set anywhere.
        if (name.isEmpty() || domain.isEmpty()) continue;
        cookies.push_back({
            name,
            row.value(QStringLiteral("value")).toString(),
            domain,
            row.value(QStringLiteral("path")).toString(QStringLiteral("/")),
            row.value(QStringLiteral("expires")).toDouble(0),
            row.value(QStringLiteral("secure")).toBool(false),
            row.value(QStringLiteral("httpOnly")).toBool(false),
            row.value(QStringLiteral("sameSite")).toString(),
        });
    }
    return cookies;
}

ImportedBrowserData decode(const QJsonObject &root) {
    ImportedBrowserData data;
    data.bookmarks = linksOf(root, QStringLiteral("bookmarks"));
    data.history = linksOf(root, QStringLiteral("history"));
    data.tabs = linksOf(root, QStringLiteral("tabs"));
    data.cookies = cookiesOf(root);
    for (const QJsonValue &warning : root.value(QStringLiteral("warnings")).toArray()) {
        if (warning.isString()) data.warnings.append(warning.toString());
    }
    const QJsonObject availability = root.value(QStringLiteral("availability")).toObject();
    for (auto entry = availability.begin(); entry != availability.end(); ++entry) {
        data.availability.insert(entry.key(), entry.value().toString());
    }
    return data;
}

} // namespace

QStringList BrowserDataImport::kinds() {
    return {
        QStringLiteral("bookmarks"), QStringLiteral("history"),
        QStringLiteral("tabs"), QStringLiteral("cookies"),
    };
}

QString BrowserDataImport::kindLabel(const QString &kind) {
    if (kind == QStringLiteral("bookmarks")) return L(QStringLiteral("Lesezeichen"));
    if (kind == QStringLiteral("history")) return L(QStringLiteral("Verlauf"));
    if (kind == QStringLiteral("tabs")) return L(QStringLiteral("Offene Tabs"));
    if (kind == QStringLiteral("cookies")) return QStringLiteral("Cookies / Logins");
    if (kind == QStringLiteral("passwords")) return L(QStringLiteral("Passwörter"));
    return kind;
}

std::vector<ImportProfileEntry> BrowserDataImport::profiles(const QString &home, QString &problem) {
    QJsonObject request{{QStringLiteral("action"), QStringLiteral("profiles")}};
    if (!home.isEmpty()) request.insert(QStringLiteral("home"), home);
    const ImportWorker::Result answer = ImportWorker::call(request);
    problem = answer.problem;
    if (!problem.isEmpty()) return {};

    std::vector<ImportProfileEntry> entries;
    for (const QJsonValue &value : answer.value.toArray()) {
        const QJsonObject row = value.toObject();
        const QString path = row.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) continue;
        entries.push_back({
            row.value(QStringLiteral("id")).toString(path),
            row.value(QStringLiteral("browser")).toString(),
            row.value(QStringLiteral("name")).toString(),
            path,
        });
    }
    return entries;
}

ImportedBrowserData BrowserDataImport::readProfile(
    const QString &browser,
    const QString &profilePath,
    const QStringList &requestedKinds
) {
    ImportedBrowserData data;
    if (browser.isEmpty() || profilePath.isEmpty()) {
        data.problem = L(
            QStringLiteral("Bitte ein Profil des anderen Browsers wählen."),
            QStringLiteral("Please choose a profile of the other browser.")
        );
        return data;
    }
    QJsonArray kindValues;
    for (const QString &kind : requestedKinds) {
        if (kinds().contains(kind)) kindValues.append(kind);
    }
    if (kindValues.isEmpty()) {
        data.problem = L(
            QStringLiteral("Bitte mindestens eine Datenart auswählen."),
            QStringLiteral("Please select at least one kind of data.")
        );
        return data;
    }

    const ImportWorker::Result answer = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("profile")},
        {QStringLiteral("browser"), browser},
        {QStringLiteral("path"), profilePath},
        {QStringLiteral("kinds"), kindValues},
    });
    if (!answer.problem.isEmpty()) {
        data.problem = answer.problem;
        return data;
    }
    return decode(answer.value.toObject());
}

ImportedBrowserData BrowserDataImport::readFile(const QString &kind, const QString &filePath) {
    ImportedBrowserData data;
    if (!kinds().contains(kind) && kind != QStringLiteral("passwords")) {
        data.problem = L(QStringLiteral("Unbekannte Datenart"), QStringLiteral("Unknown data type"));
        return data;
    }
    if (filePath.isEmpty()) {
        data.problem = L(
            QStringLiteral("Bitte eine Exportdatei wählen."),
            QStringLiteral("Please choose an export file.")
        );
        return data;
    }
    const ImportWorker::Result answer = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("file")},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("path"), filePath},
    });
    if (!answer.problem.isEmpty()) {
        data.problem = answer.problem;
        return data;
    }
    return decode(answer.value.toObject());
}

} // namespace yobro::spike
