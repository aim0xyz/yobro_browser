#include "spike/ImportWorker.hpp"
#include "spike/NativePasswordImport.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#if defined(__APPLE__)
#include <CommonCrypto/CommonCrypto.h>
#endif

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::ImportWorker;
using yobro::spike::ImportedLoginSet;
using yobro::spike::NativePasswordImport;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

/// The helper script is shared with the WebKit build. If the copies drift apart,
/// the two engines read browser profiles differently.
void checkScriptParity() {
    QFile shipped(QStringLiteral(":/yobro/BrowserImport.py"));
    check(shipped.open(QIODevice::ReadOnly), "The bundled import script is missing.");
    QFile original(QStringLiteral(YOBRO_WEBKIT_IMPORT_SCRIPT));
    check(original.open(QIODevice::ReadOnly), "The WebKit import script could not be read.");
    const QByteArray shippedDigest =
        QCryptographicHash::hash(shipped.readAll(), QCryptographicHash::Sha256);
    const QByteArray originalDigest =
        QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256);
    check(shippedDigest == originalDigest,
          "The bundled import script differs from the one the WebKit build uses.");
}

void checkWorker(const QString &directory) {
    check(ImportWorker::available(),
          "No Python 3 interpreter or script available; the import cannot run here.");
    check(ImportWorker::interpreter().startsWith(QLatin1Char('/')),
          "The interpreter was not resolved to an absolute path.");

    // A well-known action has to come back with a usable answer.
    const ImportWorker::Result profiles = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("profiles")},
        {QStringLiteral("home"), directory},
    });
    check(profiles.problem.isEmpty(), "Listing profiles failed: " + profiles.problem.toStdString());
    check(profiles.value.isArray(), "Listing profiles did not return an array.");

    // An unknown action must be reported, not crash or look successful.
    const ImportWorker::Result unknown = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("definitely-not-an-action")},
    });
    check(!unknown.problem.isEmpty(), "An unknown action was reported as successful.");

    // A profile without a password database has to say so.
    const ImportWorker::Result empty = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("chromium_passwords")},
        {QStringLiteral("path"), directory},
    });
    check(!empty.problem.isEmpty(), "A profile without a password database looked successful.");
}

#if defined(__APPLE__)
/// Produces a blob in the format Chromium writes on macOS, so the decryption can
/// be verified against real input rather than against itself.
QByteArray encryptLikeChromium(const QString &plainText, const QByteArray &key) {
    const QByteArray payload = plainText.toUtf8();
    const QByteArray iv(16, ' ');
    QByteArray output(payload.size() + 32, '\0');
    std::size_t written = 0;
    const CCCryptorStatus status = CCCrypt(
        kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
        key.constData(), static_cast<size_t>(key.size()),
        iv.constData(),
        payload.constData(), static_cast<size_t>(payload.size()),
        output.data(), static_cast<size_t>(output.size()), &written
    );
    if (status != kCCSuccess) fail("The test could not encrypt its fixture.");
    output.truncate(static_cast<int>(written));
    return QByteArrayLiteral("v10") + output;
}
#endif

void checkDecryption() {
#if defined(__APPLE__)
    const QByteArray key = NativePasswordImport::deriveKey(QByteArrayLiteral("peanuts"));
    check(key.size() == 16, "The derived key does not have the AES-128 length.");
    // The derivation must be stable, otherwise stored blobs stop opening.
    check(key == NativePasswordImport::deriveKey(QByteArrayLiteral("peanuts")),
          "The key derivation is not deterministic.");
    check(key != NativePasswordImport::deriveKey(QByteArrayLiteral("peanut")),
          "Two different secrets produced the same key.");
    check(NativePasswordImport::deriveKey({}).isEmpty(), "An empty secret produced a key.");

    const QString secretText = QStringLiteral("Sömmerlich lang & krumm 42");
    const QByteArray blob = encryptLikeChromium(secretText, key);
    const auto decrypted = NativePasswordImport::decrypt(blob, key);
    check(decrypted.has_value() && *decrypted == secretText,
          "A real Chromium blob did not decrypt back to its plain text.");

    // Anything that is not a well-formed v10 payload must be refused rather than
    // handed back as if it were plain text.
    check(!NativePasswordImport::decrypt(QByteArrayLiteral("plain password"), key).has_value(),
          "Plain text without the version tag was accepted.");
    check(!NativePasswordImport::decrypt(QByteArrayLiteral("v11") + blob.mid(3), key).has_value(),
          "A future version tag was accepted.");
    check(!NativePasswordImport::decrypt(QByteArrayLiteral("v10"), key).has_value(),
          "An empty payload was accepted.");
    check(!NativePasswordImport::decrypt(blob.left(blob.size() - 1), key).has_value(),
          "A payload that is not block aligned was accepted.");
    check(!NativePasswordImport::decrypt(blob, NativePasswordImport::deriveKey(QByteArrayLiteral("wrong"))).has_value(),
          "The wrong key produced a password.");
    check(!NativePasswordImport::decrypt(blob, QByteArrayLiteral("too-short")).has_value(),
          "A key of the wrong length was accepted.");
    check(!NativePasswordImport::decrypt({}, key).has_value(), "An empty blob was accepted.");
#else
    std::cout << "NOTE: password decryption is macOS only; skipped.\n";
#endif
}

/// Builds a Chromium `Login Data` database and checks that the shared script
/// reads exactly the rows it should, and that the blobs decrypt afterwards.
void checkChromiumProfileReading(const QString &directory) {
#if defined(__APPLE__)
    const QByteArray key = NativePasswordImport::deriveKey(QByteArrayLiteral("peanuts"));
    const QString wanted = QStringLiteral("hunter2");
    const QByteArray blob = encryptLikeChromium(wanted, key);

    const QString script = directory + QStringLiteral("/build-login-data.py");
    {
        QFile file(script);
        check(file.open(QIODevice::WriteOnly | QIODevice::Text), "Could not write the fixture script.");
        file.write(R"PY(import base64, sqlite3, sys
path, blob = sys.argv[1], base64.b64decode(sys.argv[2])
connection = sqlite3.connect(path)
connection.execute(
    "CREATE TABLE logins (origin_url TEXT, username_value TEXT, password_value BLOB,"
    " blacklisted_by_user INTEGER DEFAULT 0)"
)
rows = [
    ("https://shop.example/login", "scout", blob, 0),
    ("https://blocked.example/login", "nobody", blob, 1),
    ("chrome://settings/passwords", "internal", blob, 0),
    ("https://empty.example/login", "nopassword", b"", 0),
]
connection.executemany("INSERT INTO logins VALUES (?, ?, ?, ?)", rows)
connection.commit()
connection.close()
)PY");
    }

    QProcess builder;
    builder.start(ImportWorker::interpreter(), {
        script,
        directory + QStringLiteral("/Login Data"),
        QString::fromLatin1(blob.toBase64()),
    });
    check(builder.waitForFinished(30'000) && builder.exitCode() == 0,
          "The fixture database could not be created: "
          + QString::fromUtf8(builder.readAllStandardError()).toStdString());

    const ImportWorker::Result rows = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("chromium_passwords")},
        {QStringLiteral("path"), directory},
    });
    check(rows.problem.isEmpty(), "Reading the fixture profile failed: " + rows.problem.toStdString());
    const QJsonArray values = rows.value.toArray();
    // Blocked entries, non-web schemes and empty blobs must not be offered.
    check(values.size() == 1,
          "The script returned " + std::to_string(values.size()) + " rows instead of one.");
    const QJsonObject row = values.first().toObject();
    check(row.value(QStringLiteral("url")).toString() == QStringLiteral("https://shop.example/login"),
          "The wrong row was returned.");
    check(row.value(QStringLiteral("username")).toString() == QStringLiteral("scout"),
          "The user name was not read.");
    const auto password = NativePasswordImport::decrypt(
        QByteArray::fromBase64(row.value(QStringLiteral("encrypted")).toString().toLatin1()),
        key
    );
    check(password.has_value() && *password == wanted,
          "The password from the fixture profile did not decrypt.");
#else
    (void)directory;
#endif
}

void checkUnsupportedBrowser() {
    const ImportedLoginSet result = NativePasswordImport::load(
        QStringLiteral("Netscape"), QStringLiteral("/tmp")
    );
    check(!result.problem.isEmpty(), "An unsupported browser was accepted.");
    check(result.logins.empty(), "An unsupported browser returned logins.");
    check(NativePasswordImport::supportedBrowsers().contains(QStringLiteral("Chrome")),
          "Chrome is missing from the supported browsers.");
    check(NativePasswordImport::supportedBrowsers().size() == 5,
          "The supported browser list does not match the WebKit build.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir home;
    if (!home.isValid()) return 1;

    try {
        checkScriptParity();
        checkWorker(home.path());
        checkDecryption();
        checkChromiumProfileReading(home.path());
        checkUnsupportedBrowser();
    } catch (const std::exception &error) {
        std::cerr << "PASSWORD IMPORT FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASSWORD IMPORT PASS\n";
    return 0;
}
