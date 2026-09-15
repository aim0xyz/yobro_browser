#include "spike/KeyDerivation.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace yobro::spike {
namespace {

constexpr int hashBytes = 32;

QByteArray hmac(const QByteArray &key, const QByteArray &data) {
    QMessageAuthenticationCode code(QCryptographicHash::Sha256, key);
    code.addData(data);
    return code.result();
}

QByteArray bigEndian(int value) {
    QByteArray encoded(4, '\0');
    encoded[0] = static_cast<char>((static_cast<unsigned>(value) >> 24) & 0xFFU);
    encoded[1] = static_cast<char>((static_cast<unsigned>(value) >> 16) & 0xFFU);
    encoded[2] = static_cast<char>((static_cast<unsigned>(value) >> 8) & 0xFFU);
    encoded[3] = static_cast<char>(static_cast<unsigned>(value) & 0xFFU);
    return encoded;
}

} // namespace

QByteArray KeyDerivation::pbkdf2Sha256(
    const QByteArray &passphrase,
    const QByteArray &salt,
    int iterations,
    int length
) {
    if (iterations < 1 || length < 1 || length > 1024)
        return {};
    QByteArray result;
    result.reserve(length);
    for (int block = 1; result.size() < length; ++block) {
        // U1 = HMAC(password, salt || INT_32_BE(block))
        QByteArray previous = hmac(passphrase, salt + bigEndian(block));
        QByteArray accumulated = previous;
        for (int round = 1; round < iterations; ++round) {
            previous = hmac(passphrase, previous);
            for (int index = 0; index < hashBytes; ++index)
                accumulated[index] = static_cast<char>(accumulated[index] ^ previous[index]);
        }
        result.append(accumulated);
    }
    result.resize(length);
    return result;
}

QByteArray KeyDerivation::randomSalt(int length) {
    if (length < 1 || length > 1024)
        return {};
    QByteArray salt(length, Qt::Uninitialized);
    QRandomGenerator::system()->generate(salt.begin(), salt.end());
    return salt;
}

} // namespace yobro::spike
