#pragma once

#include "spike/KeyDerivation.hpp"

#include <QByteArray>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace yobro::spike {

struct LoginCredential {
    /// Normalised origin in the form `https://host` or `https://host:port`.
    std::string origin;
    std::string username;
    std::string password;
};

/// What one `store()` call did. A single boolean could not tell "the entry was
/// already there and was updated" from "nothing was written", which made the
/// confirmation the user sees unreliable.
enum class PasswordStoreResult {
    created,
    updated,
    /// The entry exists and `replace` was false. Nothing was written.
    refused,
    /// The store could not be written. `problem()` says why.
    failed,
};

/// Where a profile's credentials physically live.
enum class PasswordStoreKind {
    /// The macOS Keychain. The operating system holds the secret and the user
    /// can inspect and revoke it with tools they already have.
    keychain,
    /// A file next to the profile, sealed with ChaCha20-Poly1305 under a key
    /// derived from a passphrase the user types. Used wherever there is no
    /// Keychain, and selectable on macOS for a portable profile.
    encryptedFile,
};

/// Profile-scoped credential storage.
///
/// On macOS entries are Keychain items under a service name derived from the
/// profile directory, so profiles never see each other's logins and the WebKit
/// build's vault stays untouched. Everywhere else the store is an encrypted
/// file, which has one consequence the Keychain does not have: it must be
/// unlocked with a passphrase before it can be read or written, and it stays
/// locked for the rest of the session if the user does not. That is deliberate.
/// A file whose key sits next to it would only look like a password store.
///
/// Credentials are only ever offered for the exact HTTPS origin they were saved
/// for, in both backends.
class PasswordVault {
public:
    /// `forced` overrides the platform default; without it the environment
    /// variable `YOBRO_PASSWORD_STORE` (`keychain` or `file`) is honoured, and
    /// otherwise macOS uses the Keychain and every other platform the file.
    explicit PasswordVault(
        std::filesystem::path profileDirectory,
        std::optional<PasswordStoreKind> forced = std::nullopt
    );

    /// Returns the canonical origin for a URL, or nothing when the URL is not a
    /// plain HTTPS address. Default ports are dropped so `https://host:443` and
    /// `https://host` match, while any other port stays significant.
    [[nodiscard]] static std::optional<std::string> originFor(const std::string &url);

    [[nodiscard]] PasswordStoreKind kind() const;
    /// True when this backend cannot be used before `unlock()` succeeded.
    [[nodiscard]] bool requiresPassphrase() const;
    /// True while a passphrase-backed store has no key in memory. Always false
    /// for the Keychain.
    [[nodiscard]] bool locked() const;
    /// True when a passphrase-backed store file already exists, so the user has
    /// to be asked for their existing passphrase rather than a new one.
    [[nodiscard]] bool exists() const;
    /// Opens an existing store, or creates one with this passphrase. Returns
    /// false and sets `problem()` on a wrong passphrase or a broken file.
    bool unlock(const std::string &passphrase);
    /// Re-seals the store under a new passphrase. The current one has to be
    /// right even when the store is already unlocked.
    bool changePassphrase(const std::string &current, const std::string &next);
    /// Drops the key. Nothing can be read or written afterwards.
    void lock();
    /// Lowers the key-derivation cost. Only for tests, which would otherwise
    /// spend seconds deriving keys; the app never calls this.
    void setDerivationIterations(int iterations);
    /// The last failure in plain words, or empty when the last call succeeded.
    [[nodiscard]] const std::string &problem() const;

    /// True when this backend can work at all on this machine. A locked file
    /// store is available but not readable; check `locked()` as well.
    [[nodiscard]] bool available() const;
    /// Writes one credential. Updating an existing entry requires `replace`.
    PasswordStoreResult store(const LoginCredential &credential, bool replace);
    [[nodiscard]] std::vector<LoginCredential> entries(const std::string &origin = {}) const;
    bool remove(const std::string &origin, const std::string &username);

    /// The Keychain service name. Empty for the file backend.
    [[nodiscard]] const std::string &service() const;
    /// The store file. Empty for the Keychain backend.
    [[nodiscard]] const std::filesystem::path &file() const;

private:
    [[nodiscard]] bool keychainAvailable() const;
    PasswordStoreResult keychainStore(const LoginCredential &credential, bool replace);
    [[nodiscard]] std::vector<LoginCredential> keychainEntries(const std::string &origin) const;
    bool keychainRemove(const std::string &origin, const std::string &username);
    [[nodiscard]] bool loadFile(const QByteArray &key, std::vector<LoginCredential> &into) const;
    [[nodiscard]] bool writeFile(
        const QByteArray &passphrase,
        const std::vector<LoginCredential> &entries
    ) const;

    PasswordStoreKind kind_ = PasswordStoreKind::keychain;
    std::string service_;
    std::filesystem::path file_;
    /// Only ever held while the store is unlocked, and cleared by `lock()`.
    QByteArray key_;
    std::vector<LoginCredential> cache_;
    int iterations_ = KeyDerivation::defaultIterations;
    bool unlocked_ = false;
    /// A file that cannot be parsed is never overwritten; the user may still
    /// have the passphrase for a backup of it.
    bool corrupt_ = false;
    mutable std::string problem_;
};

} // namespace yobro::spike
