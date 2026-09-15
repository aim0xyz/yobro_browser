#include "spike/KeyDerivation.hpp"
#include "spike/PasswordVault.hpp"

#include <QFile>
#include <QProcess>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using yobro::spike::LoginCredential;
using yobro::spike::KeyDerivation;
using yobro::spike::PasswordStoreKind;
using yobro::spike::PasswordStoreResult;
using yobro::spike::PasswordVault;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

void checkOriginNormalisation() {
    const auto origin = [](const char *url) { return PasswordVault::originFor(url); };

    check(origin("https://example.test/login?token=secret") == std::string("https://example.test"),
          "The default HTTPS port and path were not normalised away.");
    check(origin("https://example.test:443/login") == std::string("https://example.test"),
          "An explicit port 443 did not normalise to the default origin.");
    check(origin("https://example.test:8443/login") == std::string("https://example.test:8443"),
          "A non-default port must stay part of the origin.");
    check(origin("https://EXAMPLE.test") == std::string("https://example.test"),
          "The host was not lowercased.");

    // Anything that is not a plain HTTPS address must be refused outright.
    check(!origin("http://example.test/login").has_value(), "An HTTP URL was accepted.");
    check(!origin("file:///tmp/login.html").has_value(), "A file URL was accepted.");
    check(!origin("about:blank").has_value(), "about:blank was accepted.");
    check(!origin("").has_value(), "An empty URL was accepted.");

    // Neighbouring hosts must never share credentials.
    check(origin("https://example.test") != origin("https://accounts.example.test"),
          "A subdomain collided with its parent origin.");
    check(origin("https://example.test") != origin("https://example.test.evil.test"),
          "A suffix-extended host collided with the real origin.");
}

void checkStorageRoundTrip() {
    QTemporaryDir home;
    check(home.isValid(), "Could not create an isolated vault directory.");
    // Explicitly the Keychain backend, so the environment override cannot turn
    // this into a second run of the file-store checks.
    PasswordVault vault(
        std::filesystem::path(home.path().toStdString()) / "Profiles" / "vault-test",
        PasswordStoreKind::keychain
    );
    if (!vault.available()) {
        std::cout << "PASSWORD VAULT: keychain unavailable on this platform, storage checks skipped.\n";
        return;
    }

    const LoginCredential first{"https://login.example.test", "fixture@example.test", "fixture-only-1"};
    const LoginCredential other{"https://other.example.test", "fixture@example.test", "fixture-only-2"};
    const LoginCredential ported{"https://login.example.test:8443", "fixture@example.test", "fixture-only-3"};

    // Remove any residue from an interrupted earlier run before asserting.
    for (const LoginCredential &entry : vault.entries())
        (void)vault.remove(entry.origin, entry.username);
    check(vault.entries().empty(), "The isolated vault did not start empty.");

    struct Cleanup {
        PasswordVault &vault;
        ~Cleanup() {
            for (const LoginCredential &entry : vault.entries())
                (void)vault.remove(entry.origin, entry.username);
        }
    } cleanup{vault};

    check(vault.store(first, false) == PasswordStoreResult::created, "Storing a new credential failed.");
    check(vault.store(other, false) == PasswordStoreResult::created, "Storing a second origin failed.");
    check(vault.store(ported, false) == PasswordStoreResult::created, "Storing a non-default port origin failed.");

    check(vault.store({first.origin, first.username, "changed"}, false) == PasswordStoreResult::refused,
          "An existing credential was overwritten without replace.");
    const std::vector<LoginCredential> refused = vault.entries(first.origin);
    check(refused.size() == 1, "The stored credential was not readable back.");
    check(refused.front().password == first.password,
          "A refused update still changed the stored password.");
    check(vault.store({first.origin, first.username, "fixture-only-updated"}, true) == PasswordStoreResult::updated,
          "Replacing an existing credential failed.");

    const std::vector<LoginCredential> exact = vault.entries(first.origin);
    check(exact.size() == 1, "Exact-origin lookup returned the wrong number of entries.");
    check(exact.front().origin == first.origin, "Exact-origin lookup returned a foreign origin.");
    check(exact.front().username == first.username, "Exact-origin lookup lost the username.");
    check(exact.front().password == "fixture-only-updated", "Exact-origin lookup returned a stale password.");

    // A different port is a different origin and must not be offered.
    check(vault.entries("https://login.example.test:9443").empty(),
          "A credential leaked to an unrelated port.");
    check(vault.entries("https://sub.login.example.test").empty(),
          "A credential leaked to a subdomain.");
    check(vault.entries().size() == 3, "Listing all entries returned the wrong count.");

    check(vault.remove(first.origin, first.username), "Removing a credential failed.");
    check(vault.entries(first.origin).empty(), "A removed credential was still returned.");
    check(vault.remove(first.origin, first.username), "Removing a missing credential must succeed idempotently.");
    check(vault.entries().size() == 2, "Removal deleted more entries than requested.");

    // Empty fields are rejected so half-filled forms cannot create entries.
    check(vault.store({first.origin, "", "fixture-only"}, true) == PasswordStoreResult::failed, "An empty username was stored.");
    check(vault.store({first.origin, first.username, ""}, true) == PasswordStoreResult::failed, "An empty password was stored.");
}

void checkProfileIsolation() {
    QTemporaryDir home;
    check(home.isValid(), "Could not create an isolated vault directory.");
    const std::filesystem::path root(home.path().toStdString());
    PasswordVault first(root / "Profiles" / "one", PasswordStoreKind::keychain);
    PasswordVault second(root / "Profiles" / "two", PasswordStoreKind::keychain);
    check(first.service() != second.service(), "Two profiles shared one keychain service name.");
    if (!first.available()) return;

    struct Cleanup {
        PasswordVault &a;
        PasswordVault &b;
        ~Cleanup() {
            for (const LoginCredential &entry : a.entries()) (void)a.remove(entry.origin, entry.username);
            for (const LoginCredential &entry : b.entries()) (void)b.remove(entry.origin, entry.username);
        }
    } cleanup{first, second};

    const LoginCredential credential{"https://isolation.example.test", "fixture@example.test", "fixture-only"};
    check(first.store(credential, true) == PasswordStoreResult::created, "Storing into the first profile failed.");
    check(first.entries(credential.origin).size() == 1, "The first profile lost its own credential.");
    check(second.entries(credential.origin).empty(), "A credential leaked into another profile.");
}

/// The derivation is hand-written, so it is checked against an independent
/// implementation instead of only against itself.
void checkKeyDerivation() {
    struct Vector {
        QByteArray passphrase;
        QByteArray salt;
        int iterations;
        int length;
    };
    const QList<Vector> vectors{
        {QByteArrayLiteral("password"), QByteArrayLiteral("salt"), 1, 32},
        {QByteArrayLiteral("password"), QByteArrayLiteral("salt"), 2, 32},
        {QByteArrayLiteral("password"), QByteArrayLiteral("salt"), 4096, 32},
        {QByteArrayLiteral("passwd"), QByteArrayLiteral("salt"), 1, 64},
        {QByteArrayLiteral("Password"), QByteArrayLiteral("NaCl"), 80, 100},
        {QByteArrayLiteral(""), QByteArrayLiteral("salt"), 10, 32},
        {QByteArrayLiteral("pass\0word"), QByteArrayLiteral("sa\0lt"), 4096, 16},
    };
    for (const Vector &vector : vectors) {
        QProcess python;
        python.start(QStringLiteral("/usr/bin/python3"), {
            QStringLiteral("-c"),
            QStringLiteral("import hashlib,sys;"
                           "print(hashlib.pbkdf2_hmac('sha256',sys.stdin.buffer.read(int(sys.argv[1])),"
                           "sys.stdin.buffer.read(),int(sys.argv[2]),int(sys.argv[3])).hex())"),
            QString::number(vector.passphrase.size()),
            QString::number(vector.iterations),
            QString::number(vector.length),
        });
        check(python.waitForStarted(10'000), "Could not start python3 for the derivation oracle.");
        python.write(vector.passphrase + vector.salt);
        python.closeWriteChannel();
        check(python.waitForFinished(60'000) && python.exitCode() == 0,
              "The derivation oracle failed: "
                  + QString::fromUtf8(python.readAllStandardError()).toStdString());
        const QString expected = QString::fromUtf8(python.readAllStandardOutput()).trimmed();
        const QByteArray derived = KeyDerivation::pbkdf2Sha256(
            vector.passphrase, vector.salt, vector.iterations, vector.length);
        check(derived.size() == vector.length, "The derived key has the wrong length.");
        check(QString::fromLatin1(derived.toHex()) == expected,
              "PBKDF2-HMAC-SHA256 disagrees with python3 for "
                  + std::to_string(vector.iterations) + " iterations: got "
                  + QString::fromLatin1(derived.toHex()).toStdString() + ", expected "
                  + expected.toStdString());
    }
    // Out-of-range arguments must not produce a key at all.
    check(KeyDerivation::pbkdf2Sha256(QByteArrayLiteral("x"), QByteArrayLiteral("y"), 0, 32).isEmpty(),
          "Zero iterations produced a key.");
    check(KeyDerivation::pbkdf2Sha256(QByteArrayLiteral("x"), QByteArrayLiteral("y"), 1, 0).isEmpty(),
          "A zero-length key was produced.");
    check(KeyDerivation::pbkdf2Sha256(QByteArrayLiteral("x"), QByteArrayLiteral("y"), 1, 4096).isEmpty(),
          "An absurdly long key was produced.");
    check(KeyDerivation::randomSalt(16).size() == 16, "The salt has the wrong length.");
    check(KeyDerivation::randomSalt(16) != KeyDerivation::randomSalt(16), "Two salts were identical.");
}

/// The portable store is the whole point of this backend: it has to work on a
/// machine with no Keychain, so it is exercised here on purpose even on macOS.
void checkEncryptedFileStore() {
    QTemporaryDir home;
    check(home.isValid(), "Could not create an isolated vault directory.");
    const std::filesystem::path profile = std::filesystem::path(home.path().toStdString()) / "Profiles" / "portable";
    const auto makeVault = [&profile] {
        PasswordVault vault(profile, PasswordStoreKind::encryptedFile);
        // Production derives 600 000 times; a test does not need seconds of it.
        vault.setDerivationIterations(2'000);
        return vault;
    };
    PasswordVault vault = makeVault();
    check(vault.kind() == PasswordStoreKind::encryptedFile, "The forced backend was not honoured.");
    check(vault.requiresPassphrase(), "The file store did not ask for a passphrase.");
    check(vault.available(), "The file store reported itself as unavailable.");
    check(vault.locked(), "A fresh file store did not start locked.");
    check(!vault.exists(), "A store file existed before anything was written.");
    check(vault.service().empty(), "The file store claimed a keychain service name.");

    // Nothing works while locked, and nothing is silently dropped either.
    const LoginCredential first{"https://login.example.test", "ada@example.test", "portable-one"};
    check(vault.store(first, true) == PasswordStoreResult::failed, "A locked store accepted a credential.");
    check(!vault.problem().empty(), "A locked store failed without saying why.");
    check(vault.entries().empty(), "A locked store returned entries.");
    check(!vault.remove(first.origin, first.username), "A locked store accepted a removal.");

    check(!vault.unlock("short"), "A too short passphrase was accepted.");
    check(!vault.exists(), "A refused passphrase created a store file.");
    check(vault.unlock("correct horse battery"), "Creating the store failed: " + vault.problem());
    check(!vault.locked(), "The store stayed locked after unlocking.");
    check(vault.exists(), "Unlocking did not create the store file.");
    check(vault.entries().empty(), "A new store was not empty.");

    check(vault.store(first, false) == PasswordStoreResult::created, "Storing into the file store failed: " + vault.problem());
    check(vault.store({first.origin, first.username, "changed"}, false) == PasswordStoreResult::refused,
          "An existing credential was overwritten without replace.");
    check(vault.entries(first.origin).front().password == first.password,
          "A refused update still changed the password.");
    check(vault.store({first.origin, first.username, "portable-updated"}, true) == PasswordStoreResult::updated,
          "Replacing a credential failed.");
    check(vault.store({"https://other.example.test", "ada@example.test", "portable-two"}, false) == PasswordStoreResult::created,
          "Storing a second origin failed.");
    check(vault.store({"https://login.example.test:8443", "ada@example.test", "portable-three"}, false) == PasswordStoreResult::created,
          "Storing a non-default port origin failed.");
    check(vault.entries().size() == 3, "Listing all entries returned the wrong count.");
    check(vault.entries("https://login.example.test").size() == 1, "Exact-origin lookup is wrong.");
    check(vault.entries("https://login.example.test:9443").empty(), "A credential leaked to another port.");
    check(vault.entries("https://sub.login.example.test").empty(), "A credential leaked to a subdomain.");

    // No plaintext anywhere in the file.
    QFile stored(QString::fromStdString(vault.file().string()));
    check(stored.open(QIODevice::ReadOnly), "The store file could not be read.");
    const QByteArray raw = stored.readAll();
    stored.close();
    for (const char *secret : {"portable-updated", "portable-two", "portable-three",
                               "ada@example.test", "login.example.test", "correct horse battery"}) {
        check(!raw.contains(secret), std::string("The store file contains ") + secret + " in the clear.");
    }
    check(raw.contains("pbkdf2-hmac-sha256"), "The store file does not name its derivation.");
    check(!QFile::exists(QString::fromStdString(vault.file().string()) + QStringLiteral(".new")),
          "The temporary file from the atomic replace was left behind.");

    // A second instance sees the same data, and only with the right passphrase.
    PasswordVault reopened = makeVault();
    check(reopened.exists(), "The second instance did not find the store.");
    check(!reopened.unlock("wrong passphrase"), "A wrong passphrase opened the store.");
    check(reopened.problem() == "Falsche Passphrase.", "A wrong passphrase gave the wrong reason: " + reopened.problem());
    check(reopened.locked(), "A wrong passphrase left the store unlocked.");
    check(reopened.unlock("correct horse battery"), "Reopening with the right passphrase failed.");
    check(reopened.entries().size() == 3, "The reopened store lost entries.");
    check(reopened.entries("https://login.example.test").front().password == "portable-updated",
          "The reopened store returned a stale password.");

    check(reopened.remove("https://other.example.test", "ada@example.test"), "Removing failed.");
    check(reopened.entries().size() == 2, "Removal deleted the wrong number of entries.");
    check(reopened.remove("https://other.example.test", "ada@example.test"),
          "Removing a missing credential must succeed idempotently.");
    check(reopened.store({first.origin, "", "x"}, true) == PasswordStoreResult::failed, "An empty username was stored.");
    check(reopened.store({first.origin, first.username, ""}, true) == PasswordStoreResult::failed, "An empty password was stored.");

    // Locking really drops the key.
    reopened.lock();
    check(reopened.locked(), "The store did not lock.");
    check(reopened.entries().empty(), "A locked store still returned entries.");

    // Changing the passphrase keeps the data and invalidates the old one.
    PasswordVault rotating = makeVault();
    check(!rotating.changePassphrase("wrong passphrase", "a new long passphrase"),
          "The passphrase was changed with the wrong current one.");
    check(rotating.locked(), "A failed change left the store unlocked.");
    check(!rotating.changePassphrase("correct horse battery", "short"),
          "A too short new passphrase was accepted.");
    check(rotating.changePassphrase("correct horse battery", "a new long passphrase"),
          "Changing the passphrase failed: " + rotating.problem());
    check(rotating.entries().size() == 2, "Changing the passphrase lost entries.");
    PasswordVault afterRotation = makeVault();
    check(!afterRotation.unlock("correct horse battery"), "The old passphrase still worked.");
    check(afterRotation.unlock("a new long passphrase"), "The new passphrase did not work.");
    check(afterRotation.entries().size() == 2, "The rotated store lost entries.");

    // A damaged file is reported and never overwritten.
    const QString path = QString::fromStdString(afterRotation.file().string());
    QFile::remove(path + QStringLiteral(".backup"));
    check(QFile::copy(path, path + QStringLiteral(".backup")), "Could not back the store file up.");
    for (const QByteArray &damage : {
             QByteArrayLiteral("not json"),
             QByteArrayLiteral("[]"),
             QByteArrayLiteral("{}"),
             QByteArrayLiteral("{\"version\":1,\"kdf\":\"pbkdf2-hmac-sha256\",\"iterations\":2000,\"salt\":\"AAAA\",\"payload\":\"AAAA\"}"),
             QByteArrayLiteral("{\"version\":2,\"kdf\":\"pbkdf2-hmac-sha256\",\"iterations\":2000,\"salt\":\"MTIzNDU2Nzg5MDEyMzQ1Ng==\",\"payload\":\"AAAA\"}"),
         }) {
        QFile damaged(path);
        check(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not damage the store file.");
        damaged.write(damage);
        damaged.close();
        PasswordVault broken = makeVault();
        broken.setDerivationIterations(2'000);
        check(!broken.unlock("a new long passphrase"), "A damaged store file was opened.");
        check(!broken.problem().empty(), "A damaged store failed without saying why.");
        QFile again(path);
        check(again.open(QIODevice::ReadOnly), "The damaged file disappeared.");
        check(again.readAll() == damage, "A damaged store file was overwritten.");
        again.close();
    }
    QFile::remove(path);
    check(QFile::copy(path + QStringLiteral(".backup"), path), "Could not restore the store file.");
    PasswordVault restored = makeVault();
    check(restored.unlock("a new long passphrase"), "The restored store did not open.");
    check(restored.entries().size() == 2, "The restored store lost entries.");
}

/// Which backend a profile gets must be predictable, and asking for one that
/// does not exist on this platform must not silently weaken anything.
void checkBackendSelection() {
    QTemporaryDir home;
    check(home.isValid(), "Could not create an isolated vault directory.");
    const std::filesystem::path profile = std::filesystem::path(home.path().toStdString()) / "Profiles" / "choice";
    qunsetenv("YOBRO_PASSWORD_STORE");
#if defined(__APPLE__)
    check(PasswordVault(profile).kind() == PasswordStoreKind::keychain,
          "macOS did not default to the Keychain.");
#else
    check(PasswordVault(profile).kind() == PasswordStoreKind::encryptedFile,
          "A platform without a Keychain did not default to the file store.");
#endif
    qputenv("YOBRO_PASSWORD_STORE", "file");
    check(PasswordVault(profile).kind() == PasswordStoreKind::encryptedFile,
          "The file store was not selected by the environment.");
    qputenv("YOBRO_PASSWORD_STORE", "  FILE  ");
    check(PasswordVault(profile).kind() == PasswordStoreKind::encryptedFile,
          "The environment value was not trimmed and lowercased.");
    qputenv("YOBRO_PASSWORD_STORE", "keychain");
#if defined(__APPLE__)
    check(PasswordVault(profile).kind() == PasswordStoreKind::keychain,
          "The Keychain was not selected by the environment.");
#else
    check(PasswordVault(profile).kind() == PasswordStoreKind::encryptedFile,
          "A Keychain was promised on a platform that has none.");
#endif
    qputenv("YOBRO_PASSWORD_STORE", "something else");
#if defined(__APPLE__)
    check(PasswordVault(profile).kind() == PasswordStoreKind::keychain,
          "An unknown value did not fall back to the platform default.");
#else
    check(PasswordVault(profile).kind() == PasswordStoreKind::encryptedFile,
          "An unknown value did not fall back to the platform default.");
#endif
    qunsetenv("YOBRO_PASSWORD_STORE");
    // An explicit argument always wins over the environment.
    qputenv("YOBRO_PASSWORD_STORE", "file");
    check(PasswordVault(profile, PasswordStoreKind::keychain).kind() == PasswordStoreKind::keychain,
          "The explicit backend argument was ignored.");
    qunsetenv("YOBRO_PASSWORD_STORE");
}

/// Two profiles must not be able to open each other's file store either.
void checkFileStoreIsolation() {
    QTemporaryDir home;
    check(home.isValid(), "Could not create an isolated vault directory.");
    const std::filesystem::path root(home.path().toStdString());
    PasswordVault first(root / "Profiles" / "one", PasswordStoreKind::encryptedFile);
    PasswordVault second(root / "Profiles" / "two", PasswordStoreKind::encryptedFile);
    first.setDerivationIterations(2'000);
    second.setDerivationIterations(2'000);
    check(first.file() != second.file(), "Two profiles shared one store file.");
    check(first.unlock("first profile passphrase"), "The first store did not open.");
    check(second.unlock("second profile passphrase"), "The second store did not open.");
    const LoginCredential credential{"https://isolation.example.test", "ada@example.test", "portable-only"};
    check(first.store(credential, true) == PasswordStoreResult::created, "Storing into the first profile failed.");
    check(first.entries(credential.origin).size() == 1, "The first profile lost its own credential.");
    check(second.entries(credential.origin).empty(), "A credential leaked into another profile.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    try {
        checkOriginNormalisation();
        checkKeyDerivation();
        checkBackendSelection();
        checkEncryptedFileStore();
        checkFileStoreIsolation();
        checkStorageRoundTrip();
        checkProfileIsolation();
    } catch (const std::exception &error) {
        std::cerr << "PASSWORD VAULT FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASSWORD VAULT PASS\n";
    return 0;
}
