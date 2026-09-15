#pragma once

#include <QByteArray>

namespace yobro::spike {

/// PBKDF2-HMAC-SHA256, as specified in RFC 8018.
///
/// Written out here because Qt offers HMAC but no key-derivation function, and
/// the portable password store needs one to turn a passphrase into a key. On
/// macOS CommonCrypto has PBKDF2, but that would leave every other platform
/// without one, and a store that only works on macOS is exactly the gap this
/// closes. `chromium-password-vault` checks the output against Python's
/// `hashlib.pbkdf2_hmac`, an independent implementation, which is the only
/// reason hand-written key derivation is acceptable here.
class KeyDerivation {
public:
    /// OWASP's 2023 guidance for PBKDF2-HMAC-SHA256. Stored in the vault file,
    /// so raising it later does not lock anyone out of an existing vault.
    static constexpr int defaultIterations = 600'000;
    /// Anything below this is not worth calling a derived key.
    static constexpr int minimumIterations = 100'000;

    /// Returns `length` bytes, or nothing usable (an empty array) when the
    /// arguments are out of range.
    [[nodiscard]] static QByteArray pbkdf2Sha256(
        const QByteArray &passphrase,
        const QByteArray &salt,
        int iterations,
        int length
    );

    [[nodiscard]] static QByteArray randomSalt(int length = 16);
};

} // namespace yobro::spike
