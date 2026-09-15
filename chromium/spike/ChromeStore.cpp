#include "spike/ChromeStore.hpp"

#include "spike/Localization.hpp"

#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

namespace yobro::spike {
namespace {

/// Extension ids are 32 letters from a to p, which is how Chromium encodes the
/// hash of the signing key.
const QRegularExpression &idPattern() {
    static const QRegularExpression pattern(QStringLiteral("^[a-p]{32}$"));
    return pattern;
}

const QStringList &allowedRedirectHosts() {
    static const QStringList hosts{
        QStringLiteral("google.com"), QStringLiteral("googleusercontent.com"),
        QStringLiteral("gvt1.com"), QStringLiteral("gvt2.com"), QStringLiteral("googleapis.com"),
    };
    return hosts;
}

/// Little-endian 32 bit value, or nothing when the read would run past the end.
std::optional<qint64> readUint32(const QByteArray &data, int offset) {
    if (offset < 0 || offset + 4 > data.size()) return std::nullopt;
    qint64 value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<qint64>(static_cast<unsigned char>(data.at(offset + index))) << (index * 8);
    }
    return value;
}

QStringList sortedStrings(const QJsonArray &values) {
    QStringList result;
    for (const QJsonValue &value : values) {
        if (value.isString()) result.append(value.toString());
    }
    result.sort();
    return result;
}

/// The Chrome version the update service is asked for. The installed Chrome is
/// preferred; otherwise a recent version is claimed, which the service accepts.
QString chromeVersion() {
    QFile info(QStringLiteral("/Applications/Google Chrome.app/Contents/Info.plist"));
    if (info.open(QIODevice::ReadOnly)) {
        const QByteArray contents = info.readAll();
        const int key = contents.indexOf("CFBundleShortVersionString");
        if (key >= 0) {
            const QRegularExpression version(QStringLiteral("<string>([0-9][0-9.]+)</string>"));
            const auto match = version.match(QString::fromLatin1(contents.mid(key, 200)));
            if (match.hasMatch()) return match.captured(1);
        }
    }
    return QStringLiteral("140.0.0.0");
}

} // namespace

std::optional<QString> ChromeStore::identifier(const QString &input) {
    const QString value = input.trimmed();
    if (idPattern().match(value).hasMatch()) return value;

    const QUrl url(value);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) return std::nullopt;
    const QString host = url.host().toLower();
    if (host != QStringLiteral("chromewebstore.google.com")
        && host != QStringLiteral("chrome.google.com")) {
        return std::nullopt;
    }
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    // Only a detail page names an extension; a search result page does not.
    if (!parts.contains(QStringLiteral("detail"))) return std::nullopt;
    for (auto part = parts.crbegin(); part != parts.crend(); ++part) {
        if (idPattern().match(*part).hasMatch()) return *part;
    }
    return std::nullopt;
}

QUrl ChromeStore::downloadUrl(const QString &id, const QString &version) {
    QUrl url(QStringLiteral("https://clients2.google.com/service/update2/crx"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response"), QStringLiteral("redirect"));
    query.addQueryItem(QStringLiteral("prodversion"), version);
    query.addQueryItem(QStringLiteral("acceptformat"), QStringLiteral("crx2,crx3"));
    query.addQueryItem(QStringLiteral("x"), QStringLiteral("id=%1&installsource=ondemand&uc").arg(id));
    url.setQuery(query);
    return url;
}

bool ChromeStore::isAllowedRedirect(const QUrl &url) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) return false;
    const QString host = url.host().toLower();
    for (const QString &allowed : allowedRedirectHosts()) {
        if (host == allowed || host.endsWith(QLatin1Char('.') + allowed)) return true;
    }
    return false;
}

std::optional<QByteArray> ChromeStore::zipPayload(const QByteArray &crx) {
    if (crx.size() < 12 || !crx.startsWith("Cr24")) return std::nullopt;
    const auto version = readUint32(crx, 4);
    if (!version) return std::nullopt;

    qint64 offset = 0;
    if (*version == 2) {
        const auto publicKeyLength = readUint32(crx, 8);
        const auto signatureLength = readUint32(crx, 12);
        if (!publicKeyLength || !signatureLength) return std::nullopt;
        offset = 16 + *publicKeyLength + *signatureLength;
    } else if (*version == 3) {
        const auto headerLength = readUint32(crx, 8);
        if (!headerLength) return std::nullopt;
        offset = 12 + *headerLength;
    } else {
        return std::nullopt;
    }

    if (offset < 0 || offset + 4 > crx.size()) return std::nullopt;
    // The payload has to start with a local ZIP file header.
    if (crx.mid(static_cast<int>(offset), 4) != QByteArrayLiteral("PK\x03\x04")) return std::nullopt;
    return crx.mid(static_cast<int>(offset));
}

ChromeStore::Package ChromeStore::acceptPackage(
    const QByteArray &crxOrZip,
    const QString &parentDirectory
) {
    Package package;
    const auto reject = [&package](const QString &reason) {
        package.problem = reason;
        return package;
    };

    if (crxOrZip.isEmpty()) {
        return reject(L(QStringLiteral("Das Erweiterungspaket ist leer."),
                        QStringLiteral("The extension package is empty.")));
    }
    if (crxOrZip.size() > maximumPackageBytes) {
        return reject(L(QStringLiteral("Das Erweiterungspaket ist größer als 100 MB."),
                        QStringLiteral("The extension package is larger than 100 MB.")));
    }

    QByteArray zip = crxOrZip;
    if (crxOrZip.startsWith("Cr24")) {
        const auto payload = zipPayload(crxOrZip);
        if (!payload) {
            return reject(L(QStringLiteral("Diese CRX-Datei konnte nicht gelesen werden."),
                            QStringLiteral("This CRX file could not be read.")));
        }
        zip = *payload;
    } else if (!crxOrZip.startsWith("PK\x03\x04")) {
        return reject(L(QStringLiteral("Das ist weder ein CRX- noch ein ZIP-Paket."),
                        QStringLiteral("This is neither a CRX nor a ZIP package.")));
    }

    QDir parent(parentDirectory);
    if (!parent.mkpath(QStringLiteral("."))) {
        return reject(L(QStringLiteral("Das Zielverzeichnis konnte nicht angelegt werden."),
                        QStringLiteral("The target directory could not be created.")));
    }
    const QString archivePath = parent.filePath(QStringLiteral("package.zip"));
    {
        QFile file(archivePath);
        if (!file.open(QIODevice::WriteOnly)) {
            return reject(L(QStringLiteral("Das Paket konnte nicht gespeichert werden."),
                            QStringLiteral("The package could not be saved.")));
        }
        file.write(zip);
    }
    const QString target = parent.filePath(QStringLiteral("unpacked"));

    // The listing is inspected before anything is written: an entry pointing
    // outside the target directory would otherwise land anywhere on disk.
    QProcess listing;
    listing.start(QStringLiteral("/usr/bin/unzip"), {QStringLiteral("-Z1"), archivePath});
    if (!listing.waitForFinished(60'000) || listing.exitCode() != 0) {
        QFile::remove(archivePath);
        return reject(L(QStringLiteral("Das Archiv konnte nicht gelesen werden."),
                        QStringLiteral("The archive could not be read.")));
    }
    const QStringList entries = QString::fromUtf8(listing.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    bool hasManifest = false;
    for (const QString &entry : entries) {
        const QString name = entry.trimmed();
        if (name.isEmpty()) continue;
        if (name.startsWith(QLatin1Char('/')) || name.contains(QStringLiteral(".."))) {
            QFile::remove(archivePath);
            return reject(L(
                QStringLiteral("Das Paket will außerhalb seines Ordners schreiben und wurde abgelehnt."),
                QStringLiteral("The package tries to write outside its folder and was rejected.")
            ));
        }
        if (name == QStringLiteral("manifest.json")) hasManifest = true;
    }
    if (!hasManifest) {
        QFile::remove(archivePath);
        return reject(L(
            QStringLiteral("Im Paket fehlt manifest.json direkt im Hauptordner."),
            QStringLiteral("The package has no manifest.json at its top level.")
        ));
    }

    QProcess unzip;
    unzip.start(QStringLiteral("/usr/bin/unzip"), {
        QStringLiteral("-qq"), QStringLiteral("-o"), archivePath,
        QStringLiteral("-d"), target,
    });
    const bool unpacked = unzip.waitForFinished(120'000) && unzip.exitCode() == 0;
    QFile::remove(archivePath);
    if (!unpacked) {
        QDir(target).removeRecursively();
        return reject(L(QStringLiteral("Das Paket konnte nicht entpackt werden."),
                        QStringLiteral("The package could not be unpacked.")));
    }

    // A symbolic link inside the extension could reach any file the app can
    // read, so such a package is refused rather than cleaned up.
    QDirIterator walk(target, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        walk.next();
        if (walk.fileInfo().isSymLink()) {
            QDir(target).removeRecursively();
            return reject(L(
                QStringLiteral("Das Paket enthält symbolische Links und wurde abgelehnt."),
                QStringLiteral("The package contains symbolic links and was rejected.")
            ));
        }
    }

    QFile manifestFile(QDir(target).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        QDir(target).removeRecursively();
        return reject(L(QStringLiteral("manifest.json konnte nicht gelesen werden."),
                        QStringLiteral("manifest.json could not be read.")));
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    if (manifest.isEmpty()) {
        QDir(target).removeRecursively();
        return reject(L(QStringLiteral("manifest.json ist unlesbar."),
                        QStringLiteral("manifest.json cannot be parsed.")));
    }

    package.manifestVersion = manifest.value(QStringLiteral("manifest_version")).toInt(0);
    package.name = manifest.value(QStringLiteral("name")).toString();
    package.version = manifest.value(QStringLiteral("version")).toString();
    package.permissions = sortedStrings(manifest.value(QStringLiteral("permissions")).toArray());
    package.hosts = sortedStrings(manifest.value(QStringLiteral("host_permissions")).toArray());

    if (package.manifestVersion != 3) {
        QDir(target).removeRecursively();
        return reject(L(
            QStringLiteral("Diese Erweiterung nutzt Manifest-Version %1. Unterstützt wird nur Version 3."),
            QStringLiteral("This extension uses manifest version %1. Only version 3 is supported.")
        ).arg(package.manifestVersion));
    }

    // The same warning the WebKit build shows: without the proxy API a VPN
    // extension cannot protect anything, and users should know before installing.
    if (package.permissions.contains(QStringLiteral("proxy"))) {
        package.warnings.append(L(
            QStringLiteral("Diese Erweiterung verlangt die Proxy-API. Ihre VPN-Funktionen werden hier "
                           "nicht wirken; nutze dafür die eigene App des Anbieters."),
            QStringLiteral("This extension requires the proxy API. Its VPN features will not work here; "
                           "use the provider's own app instead.")
        ));
    }
    if (package.hosts.contains(QStringLiteral("<all_urls>"))
        || package.permissions.contains(QStringLiteral("<all_urls>"))) {
        package.warnings.append(L(
            QStringLiteral("Diese Erweiterung darf auf allen Websites mitlesen und eingreifen."),
            QStringLiteral("This extension may read and change every website.")
        ));
    }

    package.directory = target;
    return package;
}

QByteArray ChromeStore::download(const QString &id, QString &problem) {
    problem.clear();
    const auto checked = identifier(id);
    if (!checked || *checked != id) {
        problem = L(QStringLiteral("Ungültige Erweiterungs-ID."),
                    QStringLiteral("Invalid extension id."));
        return {};
    }

    QNetworkAccessManager manager;
    // Redirects are followed by hand so every hop can be checked against the
    // allowed hosts.
    manager.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    QUrl next = downloadUrl(id, chromeVersion());
    QByteArray payload;

    for (int hop = 0; hop < 6; ++hop) {
        if (!isAllowedRedirect(next)) {
            problem = L(
                QStringLiteral("Der Chrome Web Store hat auf eine nicht erlaubte Adresse verwiesen."),
                QStringLiteral("The Chrome Web Store pointed at an address that is not allowed.")
            );
            return {};
        }
        QNetworkRequest request(next);
        request.setTransferTimeout(30'000);
        QNetworkReply *reply = manager.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        // A reply larger than the cap is dropped while it streams in.
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                         [reply](qint64 received, qint64) {
                             if (received > maximumPackageBytes) reply->abort();
                         });
        loop.exec();

        const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QNetworkReply::NetworkError error = reply->error();
        if (!redirect.isEmpty()) {
            next = next.resolved(redirect);
            reply->deleteLater();
            continue;
        }
        if (error != QNetworkReply::NoError) {
            problem = L(
                QStringLiteral("Das Paket konnte nicht geladen werden: %1"),
                QStringLiteral("The package could not be downloaded: %1")
            ).arg(reply->errorString());
            reply->deleteLater();
            return {};
        }
        payload = reply->readAll();
        reply->deleteLater();
        break;
    }

    if (payload.isEmpty()) {
        problem = L(
            QStringLiteral("Der Chrome Web Store stellt für diese Erweiterung kein Paket bereit."),
            QStringLiteral("The Chrome Web Store offers no package for this extension.")
        );
    }
    return payload;
}

} // namespace yobro::spike
