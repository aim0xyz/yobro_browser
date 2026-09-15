#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace yobro::spike {

struct ImportedLogin {
    QString url;
    QString username;
    QString password;
};

struct ImportedLoginSet {
    std::vector<ImportedLogin> logins;
    /// Things the user has to know, e.g. that some rows could not be decrypted.
    QStringList warnings;
    /// Empty unless the whole operation failed.
    QString problem;
    /// True when Firefox refused without its primary password.
    bool needsPrimaryPassword = false;
};

/// Reads saved logins out of another browser on this Mac.
///
/// Nothing happens until the user asks for it. macOS owns every Keychain
/// authorization prompt and a denial simply stops the operation; no access
/// control lists are changed. The same rules as in the WebKit build apply, and
/// the same helper script does the reading.
class NativePasswordImport {
public:
    /// The browsers this can read, in the order the WebKit build lists them.
    [[nodiscard]] static QStringList supportedBrowsers();

    /// `profilePath` is the browser profile directory. For Safari it is ignored,
    /// because those credentials live in the macOS Keychain rather than in a
    /// profile.
    [[nodiscard]] static ImportedLoginSet load(
        const QString &browser,
        const QString &profilePath,
        const QString &primaryPassword = {}
    );

    /// The `<Browser> Safe Storage` secret from the Keychain. Empty when the
    /// entry is missing or access was denied.
    [[nodiscard]] static QByteArray safeStorageSecret(const QString &browser);

    /// PBKDF2-SHA1 over the Keychain secret, exactly as Chromium derives it on
    /// macOS: salt `saltysalt`, 1003 iterations, 16 byte key.
    [[nodiscard]] static QByteArray deriveKey(const QByteArray &secret);

    /// Decrypts one `v10` blob. Anything that is not a well-formed v10 payload is
    /// rejected rather than being mistaken for plain text.
    [[nodiscard]] static std::optional<QString> decrypt(
        const QByteArray &encrypted,
        const QByteArray &key
    );

private:
    /// Safari's web logins come from the shared macOS internet password store,
    /// not from a profile directory.
    [[nodiscard]] static ImportedLoginSet safariLogins();
};

} // namespace yobro::spike
