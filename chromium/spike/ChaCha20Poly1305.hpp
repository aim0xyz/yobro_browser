#pragma once

#include <QByteArray>

#include <optional>

namespace yobro::spike {

/// ChaCha20-Poly1305 as specified in RFC 8439.
///
/// Written out here on purpose. The WebKit build seals its sync payloads with
/// CryptoKit's `ChaChaPoly`, Qt offers no authenticated cipher at all, and
/// macOS exposes no stable C interface for this construction. Since the sealed
/// bytes travel through a shared backend, the format has to match exactly
/// rather than be swapped for whatever is locally convenient.
///
/// Every function is verified against the test vectors in RFC 8439 by
/// `chromium-sync`, which is the only reason hand-written crypto is acceptable
/// here.
class ChaCha20Poly1305 {
public:
    static constexpr int keyBytes = 32;
    static constexpr int nonceBytes = 12;
    static constexpr int tagBytes = 16;

    /// One 64-byte ChaCha20 block. Exposed for the RFC's block test vectors.
    [[nodiscard]] static QByteArray block(
        const QByteArray &key,
        quint32 counter,
        const QByteArray &nonce
    );

    /// The RFC 8439 stream cipher, starting at `counter`.
    [[nodiscard]] static QByteArray chacha20(
        const QByteArray &key,
        quint32 counter,
        const QByteArray &nonce,
        const QByteArray &data
    );

    /// The Poly1305 one-time authenticator over `data` with a 32-byte key.
    [[nodiscard]] static QByteArray poly1305(const QByteArray &key, const QByteArray &data);

    /// Seals `plaintext` and returns nonce, ciphertext and tag joined in that
    /// order, which is the layout CryptoKit calls `combined`.
    [[nodiscard]] static QByteArray seal(
        const QByteArray &key,
        const QByteArray &nonce,
        const QByteArray &plaintext,
        const QByteArray &additionalData = {}
    );

    /// Opens a combined box. Returns nothing when the tag does not match, which
    /// is the only answer a caller may act on.
    [[nodiscard]] static std::optional<QByteArray> open(
        const QByteArray &key,
        const QByteArray &combined,
        const QByteArray &additionalData = {}
    );

    /// A fresh nonce from the system's random source.
    [[nodiscard]] static QByteArray randomNonce();
    /// A fresh 32-byte key from the system's random source.
    [[nodiscard]] static QByteArray randomKey();
};

} // namespace yobro::spike
