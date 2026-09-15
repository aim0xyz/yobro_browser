#include "spike/ChromeStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::ChromeStore;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

const QString validId = QStringLiteral("abcdefghijklmnopabcdefghijklmnop");

void checkIdentifier() {
    check(ChromeStore::identifier(validId) == validId, "A bare extension id was not accepted.");
    check(ChromeStore::identifier(QStringLiteral("  ") + validId + QStringLiteral("  ")) == validId,
          "Surrounding spaces were not trimmed.");
    check(ChromeStore::identifier(
              QStringLiteral("https://chromewebstore.google.com/detail/some-name/") + validId
          ) == validId,
          "A store detail link was not understood.");
    check(ChromeStore::identifier(
              QStringLiteral("https://chrome.google.com/webstore/detail/some-name/") + validId
              + QStringLiteral("?hl=de")
          ) == validId,
          "The older store host was not understood.");

    // Anything that is not clearly one extension must be refused, so a stray
    // link cannot start a download.
    for (const QString &input : {
             QString(),
             QStringLiteral("qrstuvwxyzabcdefghijklmnopabcdef"),          // letters beyond p
             QStringLiteral("abcdefghijklmnopabcdefghijklmno"),           // 31 letters
             QStringLiteral("abcdefghijklmnopabcdefghijklmnopq"),         // 33 letters
             QStringLiteral("ABCDEFGHIJKLMNOPABCDEFGHIJKLMNOP"),          // upper case
             QStringLiteral("https://chromewebstore.google.com/search/adblock"),
             QStringLiteral("https://evil.example/detail/x/") + validId,
             QStringLiteral("http://chromewebstore.google.com/detail/x/") + validId,
             QStringLiteral("https://chromewebstore.google.com/") + validId,
             QStringLiteral("file:///tmp/") + validId,
         }) {
        check(!ChromeStore::identifier(input).has_value(),
              "An input that is not an extension was accepted: " + input.toStdString());
    }
}

void checkDownloadUrl() {
    const QUrl url = ChromeStore::downloadUrl(validId, QStringLiteral("140.0.7339.225"));
    check(url.scheme() == QStringLiteral("https"), "The update service is not addressed over HTTPS.");
    check(url.host() == QStringLiteral("clients2.google.com"), "The update service host is wrong.");
    const QUrlQuery query(url);
    check(query.queryItemValue(QStringLiteral("prodversion")) == QStringLiteral("140.0.7339.225"),
          "The Chrome version was not passed on.");
    check(query.queryItemValue(QStringLiteral("acceptformat")) == QStringLiteral("crx2,crx3"),
          "The accepted formats are wrong.");
    check(query.queryItemValue(QStringLiteral("x")).contains(QStringLiteral("id=") + validId),
          "The extension id is missing from the request.");
}

void checkRedirectPolicy() {
    for (const QString &url : {
             QStringLiteral("https://clients2.google.com/x"),
             QStringLiteral("https://storage.googleapis.com/x"),
             QStringLiteral("https://r5---sn-x.gvt1.com/x"),
             QStringLiteral("https://lh3.googleusercontent.com/x"),
         }) {
        check(ChromeStore::isAllowedRedirect(QUrl(url)), "A Google download host was refused: " + url.toStdString());
    }
    for (const QString &url : {
             QStringLiteral("http://clients2.google.com/x"),          // not HTTPS
             QStringLiteral("https://google.com.evil.example/x"),      // suffix trick
             QStringLiteral("https://notgoogle.com/x"),
             QStringLiteral("https://evil.example/x"),
             QStringLiteral("file:///etc/passwd"),
             QString(),
         }) {
        check(!ChromeStore::isAllowedRedirect(QUrl(url)),
              "A redirect that should be refused was allowed: " + url.toStdString());
    }
}

QByteArray littleEndian(qint64 value) {
    QByteArray bytes(4, '\0');
    for (int index = 0; index < 4; ++index) bytes[index] = static_cast<char>((value >> (index * 8)) & 0xFF);
    return bytes;
}

/// Builds a ZIP with the given files using the system zip tool, so the test runs
/// against a real archive rather than a hand-made one.
QByteArray buildZip(const QString &workDirectory, const QList<QPair<QString, QByteArray>> &files) {
    const QString source = workDirectory + QStringLiteral("/zip-source");
    // Only ever remove the fixture directory itself. An unqualified QDir() would
    // be the working directory, which is a very expensive mistake to make here.
    QDir(source).removeRecursively();
    check(QDir().mkpath(source), "Could not create the archive source directory.");
    for (const auto &entry : files) {
        const QString path = source + QLatin1Char('/') + entry.first;
        check(QDir().mkpath(QFileInfo(path).absolutePath()), "Could not create an archive subdirectory.");
        QFile file(path);
        check(file.open(QIODevice::WriteOnly), "Could not write an archive member.");
        file.write(entry.second);
    }
    const QString archive = workDirectory + QStringLiteral("/built.zip");
    QFile::remove(archive);
    QProcess zip;
    zip.setWorkingDirectory(source);
    zip.start(QStringLiteral("/usr/bin/zip"), {QStringLiteral("-qr"), archive, QStringLiteral(".")});
    check(zip.waitForFinished(60'000) && zip.exitCode() == 0,
          "The archive could not be built: " + QString::fromUtf8(zip.readAllStandardError()).toStdString());
    QFile built(archive);
    check(built.open(QIODevice::ReadOnly), "The built archive could not be read.");
    const QByteArray bytes = built.readAll();
    built.close();
    QDir(source).removeRecursively();
    QFile::remove(archive);
    return bytes;
}

void checkZipPayload(const QString &workDirectory) {
    const QByteArray zip = buildZip(workDirectory, {
        {QStringLiteral("manifest.json"), QByteArrayLiteral(R"({"manifest_version":3,"name":"X","version":"1"})")},
    });
    check(zip.startsWith("PK\x03\x04"), "The built archive does not look like a ZIP.");

    // CRX 3: magic, version, header length, header, payload.
    const QByteArray header = QByteArrayLiteral("pretend-protobuf-header");
    const QByteArray crx3 = QByteArrayLiteral("Cr24") + littleEndian(3)
        + littleEndian(header.size()) + header + zip;
    const auto payload3 = ChromeStore::zipPayload(crx3);
    check(payload3.has_value() && *payload3 == zip, "A CRX 3 payload was not found.");

    // CRX 2: magic, version, key length, signature length, key, signature, payload.
    const QByteArray key = QByteArray(48, 'k');
    const QByteArray signature = QByteArray(64, 's');
    const QByteArray crx2 = QByteArrayLiteral("Cr24") + littleEndian(2)
        + littleEndian(key.size()) + littleEndian(signature.size()) + key + signature + zip;
    const auto payload2 = ChromeStore::zipPayload(crx2);
    check(payload2.has_value() && *payload2 == zip, "A CRX 2 payload was not found.");

    // Malformed input must never be passed through as if it were an archive.
    check(!ChromeStore::zipPayload({}).has_value(), "An empty file was accepted.");
    check(!ChromeStore::zipPayload(QByteArrayLiteral("not a crx at all")).has_value(),
          "A file without the CRX magic was accepted.");
    check(!ChromeStore::zipPayload(QByteArrayLiteral("Cr24") + littleEndian(4) + littleEndian(4) + zip).has_value(),
          "An unknown CRX version was accepted.");
    check(!ChromeStore::zipPayload(QByteArrayLiteral("Cr24") + littleEndian(3) + littleEndian(1'000'000) + zip).has_value(),
          "A header length past the end of the file was accepted.");
    check(!ChromeStore::zipPayload(QByteArrayLiteral("Cr24") + littleEndian(3) + littleEndian(header.size()) + header
                                   + QByteArrayLiteral("no zip here")).has_value(),
          "A CRX whose payload is not a ZIP was accepted.");
    check(!ChromeStore::zipPayload(crx3.left(8)).has_value(), "A truncated CRX was accepted.");
}

void checkAcceptedPackage(const QString &workDirectory) {
    const QByteArray zip = buildZip(workDirectory, {
        {QStringLiteral("manifest.json"), QByteArrayLiteral(R"({
            "manifest_version": 3,
            "name": "Test Extension",
            "version": "2.1",
            "permissions": ["storage", "proxy", "tabs"],
            "host_permissions": ["<all_urls>"]
        })")},
        {QStringLiteral("background/worker.js"), QByteArrayLiteral("// nothing")},
    });
    const QByteArray crx = QByteArrayLiteral("Cr24") + littleEndian(3) + littleEndian(4)
        + QByteArrayLiteral("head") + zip;

    const ChromeStore::Package package =
        ChromeStore::acceptPackage(crx, workDirectory + QStringLiteral("/accepted"));
    check(package.problem.isEmpty(), "A valid package was refused: " + package.problem.toStdString());
    check(package.name == QStringLiteral("Test Extension"), "The name was not read from the manifest.");
    check(package.version == QStringLiteral("2.1"), "The version was not read from the manifest.");
    check(package.manifestVersion == 3, "The manifest version was not read.");
    check(package.permissions == QStringList({QStringLiteral("proxy"), QStringLiteral("storage"),
                                              QStringLiteral("tabs")}),
          "The permissions were not read and sorted.");
    check(package.hosts == QStringList({QStringLiteral("<all_urls>")}), "The host patterns were not read.");
    // Both warnings matter before installing, so both have to appear.
    check(package.warnings.size() == 2,
          "Expected a proxy and an all-sites warning, got " + std::to_string(package.warnings.size()) + ".");
    check(QFile::exists(package.directory + QStringLiteral("/manifest.json")),
          "The unpacked extension has no manifest.");
    check(QFile::exists(package.directory + QStringLiteral("/background/worker.js")),
          "A subdirectory was not unpacked.");
    // The archive itself must not stay behind next to the extension.
    check(!QFile::exists(workDirectory + QStringLiteral("/accepted/package.zip")),
          "The downloaded archive was left on disk.");
}

void checkRefusedPackages(const QString &workDirectory) {
    const auto refusalFor = [&workDirectory](const QByteArray &payload, const QString &name) {
        return ChromeStore::acceptPackage(payload, workDirectory + QLatin1Char('/') + name);
    };

    check(!refusalFor({}, QStringLiteral("empty")).problem.isEmpty(), "An empty package was accepted.");
    check(!refusalFor(QByteArrayLiteral("random bytes"), QStringLiteral("random")).problem.isEmpty(),
          "Random bytes were accepted.");

    // Manifest version 2 is not supported by the engine.
    const QByteArray mv2 = buildZip(workDirectory, {
        {QStringLiteral("manifest.json"), QByteArrayLiteral(R"({"manifest_version":2,"name":"Old","version":"1"})")},
    });
    const ChromeStore::Package old = refusalFor(mv2, QStringLiteral("mv2"));
    check(!old.problem.isEmpty(), "A manifest version 2 extension was accepted.");
    check(!QDir(workDirectory + QStringLiteral("/mv2/unpacked")).exists(),
          "A refused package left its files behind.");

    // An archive without a top-level manifest is not an extension.
    const QByteArray nested = buildZip(workDirectory, {
        {QStringLiteral("inner/manifest.json"), QByteArrayLiteral(R"({"manifest_version":3,"name":"N","version":"1"})")},
    });
    check(!refusalFor(nested, QStringLiteral("nested")).problem.isEmpty(),
          "An archive with the manifest one level down was accepted.");

    // An entry that would escape the target directory must be refused before
    // anything is written.
    const QString escapeSource = workDirectory + QStringLiteral("/escape-source");
    check(QDir().mkpath(escapeSource + QStringLiteral("/sub")), "Could not prepare the escape fixture.");
    {
        QFile manifest(escapeSource + QStringLiteral("/sub/manifest.json"));
        check(manifest.open(QIODevice::WriteOnly), "Could not write the escape fixture manifest.");
        manifest.write(QByteArrayLiteral(R"({"manifest_version":3,"name":"E","version":"1"})"));
    }
    const QString escapeArchive = workDirectory + QStringLiteral("/escape.zip");
    QProcess zip;
    zip.setWorkingDirectory(escapeSource + QStringLiteral("/sub"));
    // Storing the entry as "../evil.js" is what a hostile package would do.
    zip.start(QStringLiteral("/usr/bin/zip"), {
        QStringLiteral("-q"), escapeArchive,
        QStringLiteral("manifest.json"),
    });
    check(zip.waitForFinished(60'000) && zip.exitCode() == 0, "Could not build the escape archive.");
    QProcess rename;
    rename.start(QStringLiteral("/usr/bin/python3"), {
        QStringLiteral("-c"),
        QStringLiteral("import sys,zipfile\n"
                       "source=zipfile.ZipFile(sys.argv[1])\n"
                       "target=zipfile.ZipFile(sys.argv[2],'w')\n"
                       "target.writestr('manifest.json', source.read('manifest.json'))\n"
                       "target.writestr('../escaped.js', b'// outside')\n"
                       "target.close()\n"),
        escapeArchive,
        workDirectory + QStringLiteral("/escape-final.zip"),
    });
    check(rename.waitForFinished(60'000) && rename.exitCode() == 0,
          "Could not rewrite the escape archive: "
          + QString::fromUtf8(rename.readAllStandardError()).toStdString());
    QFile escaping(workDirectory + QStringLiteral("/escape-final.zip"));
    check(escaping.open(QIODevice::ReadOnly), "Could not read the escape archive.");
    const ChromeStore::Package escaped = refusalFor(escaping.readAll(), QStringLiteral("escape"));
    check(!escaped.problem.isEmpty(), "An archive writing outside its folder was accepted.");
    check(!QFile::exists(workDirectory + QStringLiteral("/escaped.js")),
          "An archive wrote outside its target directory.");

    // A symbolic link inside the package could reach any readable file.
    const QString linkSource = workDirectory + QStringLiteral("/link-source");
    check(QDir().mkpath(linkSource), "Could not prepare the symlink fixture.");
    {
        QFile manifest(linkSource + QStringLiteral("/manifest.json"));
        check(manifest.open(QIODevice::WriteOnly), "Could not write the symlink fixture manifest.");
        manifest.write(QByteArrayLiteral(R"({"manifest_version":3,"name":"L","version":"1"})"));
    }
    check(QFile::link(QStringLiteral("/etc/hosts"), linkSource + QStringLiteral("/hosts.txt")),
          "Could not create the fixture symlink.");
    const QString linkArchive = workDirectory + QStringLiteral("/link.zip");
    QProcess linkZip;
    linkZip.setWorkingDirectory(linkSource);
    linkZip.start(QStringLiteral("/usr/bin/zip"), {
        QStringLiteral("-qry"), linkArchive, QStringLiteral("."),
    });
    check(linkZip.waitForFinished(60'000) && linkZip.exitCode() == 0, "Could not build the symlink archive.");
    QFile linked(linkArchive);
    check(linked.open(QIODevice::ReadOnly), "Could not read the symlink archive.");
    const ChromeStore::Package withLink = refusalFor(linked.readAll(), QStringLiteral("link"));
    check(!withLink.problem.isEmpty(), "A package containing a symbolic link was accepted.");
    check(!QDir(workDirectory + QStringLiteral("/link/unpacked")).exists(),
          "A refused symlink package left its files behind.");
}

void checkDownloadRejectsBadId() {
    QString problem;
    const QByteArray payload = ChromeStore::download(QStringLiteral("not-an-id"), problem);
    check(payload.isEmpty(), "A download started for an invalid id.");
    check(!problem.isEmpty(), "An invalid id was not reported.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        checkIdentifier();
        checkDownloadUrl();
        checkRedirectPolicy();
        checkZipPayload(work.path());
        checkAcceptedPackage(work.path());
        checkRefusedPackages(work.path());
        checkDownloadRejectsBadId();
    } catch (const std::exception &error) {
        std::cerr << "CHROME STORE FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "CHROME STORE PASS\n";
    return 0;
}
