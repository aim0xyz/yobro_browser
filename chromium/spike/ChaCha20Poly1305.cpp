#include "spike/ChaCha20Poly1305.hpp"

#include <QRandomGenerator>

#include <array>
#include <cstring>

namespace yobro::spike {
namespace {

quint32 readLittle32(const uchar *bytes) {
    return static_cast<quint32>(bytes[0])
        | (static_cast<quint32>(bytes[1]) << 8)
        | (static_cast<quint32>(bytes[2]) << 16)
        | (static_cast<quint32>(bytes[3]) << 24);
}

void writeLittle32(uchar *bytes, quint32 value) {
    bytes[0] = static_cast<uchar>(value & 0xFF);
    bytes[1] = static_cast<uchar>((value >> 8) & 0xFF);
    bytes[2] = static_cast<uchar>((value >> 16) & 0xFF);
    bytes[3] = static_cast<uchar>((value >> 24) & 0xFF);
}

void writeLittle64(uchar *bytes, quint64 value) {
    for (int index = 0; index < 8; ++index)
        bytes[index] = static_cast<uchar>((value >> (index * 8)) & 0xFF);
}

quint32 rotateLeft(quint32 value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void quarterRound(quint32 &a, quint32 &b, quint32 &c, quint32 &d) {
    a += b; d ^= a; d = rotateLeft(d, 16);
    c += d; b ^= c; b = rotateLeft(b, 12);
    a += b; d ^= a; d = rotateLeft(d, 8);
    c += d; b ^= c; b = rotateLeft(b, 7);
}

/// Compares two byte strings in time that does not depend on where they differ,
/// so a wrong tag cannot be guessed one byte at a time.
bool equalInConstantTime(const QByteArray &left, const QByteArray &right) {
    if (left.size() != right.size()) return false;
    uchar difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
        difference |= static_cast<uchar>(left.at(index)) ^ static_cast<uchar>(right.at(index));
    return difference == 0;
}

/// Poly1305 over 26-bit limbs, following the structure the RFC describes: the
/// accumulator is kept below 2^130-5 by folding the top bits back in after each
/// multiplication.
class Poly1305State {
public:
    Poly1305State(const uchar *key) {
        // r is clamped exactly as the RFC prescribes before it is split up.
        const quint32 t0 = readLittle32(key);
        const quint32 t1 = readLittle32(key + 4);
        const quint32 t2 = readLittle32(key + 8);
        const quint32 t3 = readLittle32(key + 12);
        r_[0] = t0 & 0x3FFFFFF;
        r_[1] = ((t0 >> 26) | (t1 << 6)) & 0x3FFFF03;
        r_[2] = ((t1 >> 20) | (t2 << 12)) & 0x3FFC0FF;
        r_[3] = ((t2 >> 14) | (t3 << 18)) & 0x3F03FFF;
        r_[4] = (t3 >> 8) & 0x00FFFFF;
        for (int index = 0; index < 4; ++index) pad_[index] = readLittle32(key + 16 + index * 4);
    }

    void update(const uchar *data, std::size_t length) {
        while (length > 0) {
            const std::size_t take = std::min(std::size_t{16} - pending_, length);
            std::memcpy(buffer_ + pending_, data, take);
            pending_ += take;
            data += take;
            length -= take;
            if (pending_ < 16) return;
            absorb(buffer_, true);
            pending_ = 0;
        }
    }

    QByteArray finish() {
        if (pending_ > 0) {
            buffer_[pending_++] = 1;
            std::memset(buffer_ + pending_, 0, 16 - pending_);
            absorb(buffer_, false);
        }
        carry();

        // h += -p, and the result is used only when that did not borrow.
        quint32 g[5];
        quint64 carry = 0;
        for (int index = 0; index < 4; ++index) {
            carry = static_cast<quint64>(h_[index]) + carry + (index == 0 ? 5 : 0);
            g[index] = static_cast<quint32>(carry & 0x3FFFFFF);
            carry >>= 26;
        }
        g[4] = static_cast<quint32>(h_[4] + carry - (1U << 26));
        const quint32 mask = (g[4] >> 31) - 1;
        for (int index = 0; index < 5; ++index)
            h_[index] = (h_[index] & ~mask) | (g[index] & mask);

        // Back to four 32-bit words, then add the second half of the key.
        quint32 words[4];
        words[0] = h_[0] | (h_[1] << 26);
        words[1] = (h_[1] >> 6) | (h_[2] << 20);
        words[2] = (h_[2] >> 12) | (h_[3] << 14);
        words[3] = (h_[3] >> 18) | (h_[4] << 8);
        quint64 sum = 0;
        QByteArray tag(ChaCha20Poly1305::tagBytes, Qt::Uninitialized);
        auto *out = reinterpret_cast<uchar *>(tag.data());
        for (int index = 0; index < 4; ++index) {
            sum = static_cast<quint64>(words[index]) + pad_[index] + sum;
            writeLittle32(out + index * 4, static_cast<quint32>(sum & 0xFFFFFFFF));
            sum >>= 32;
        }
        return tag;
    }

private:
    void absorb(const uchar *bytes, bool complete) {
        const quint32 t0 = readLittle32(bytes);
        const quint32 t1 = readLittle32(bytes + 4);
        const quint32 t2 = readLittle32(bytes + 8);
        const quint32 t3 = readLittle32(bytes + 12);
        h_[0] += t0 & 0x3FFFFFF;
        h_[1] += ((t0 >> 26) | (t1 << 6)) & 0x3FFFFFF;
        h_[2] += ((t1 >> 20) | (t2 << 12)) & 0x3FFFFFF;
        h_[3] += ((t2 >> 14) | (t3 << 18)) & 0x3FFFFFF;
        // A full block carries the implicit high bit; a padded final block has
        // its own 0x01 byte instead, which is already part of the limbs above.
        h_[4] += (t3 >> 8) | (complete ? (1U << 24) : 0U);
        multiply();
    }

    void multiply() {
        const quint32 s1 = r_[1] * 5;
        const quint32 s2 = r_[2] * 5;
        const quint32 s3 = r_[3] * 5;
        const quint32 s4 = r_[4] * 5;
        quint64 d0 = static_cast<quint64>(h_[0]) * r_[0] + static_cast<quint64>(h_[1]) * s4
            + static_cast<quint64>(h_[2]) * s3 + static_cast<quint64>(h_[3]) * s2
            + static_cast<quint64>(h_[4]) * s1;
        quint64 d1 = static_cast<quint64>(h_[0]) * r_[1] + static_cast<quint64>(h_[1]) * r_[0]
            + static_cast<quint64>(h_[2]) * s4 + static_cast<quint64>(h_[3]) * s3
            + static_cast<quint64>(h_[4]) * s2;
        quint64 d2 = static_cast<quint64>(h_[0]) * r_[2] + static_cast<quint64>(h_[1]) * r_[1]
            + static_cast<quint64>(h_[2]) * r_[0] + static_cast<quint64>(h_[3]) * s4
            + static_cast<quint64>(h_[4]) * s3;
        quint64 d3 = static_cast<quint64>(h_[0]) * r_[3] + static_cast<quint64>(h_[1]) * r_[2]
            + static_cast<quint64>(h_[2]) * r_[1] + static_cast<quint64>(h_[3]) * r_[0]
            + static_cast<quint64>(h_[4]) * s4;
        quint64 d4 = static_cast<quint64>(h_[0]) * r_[4] + static_cast<quint64>(h_[1]) * r_[3]
            + static_cast<quint64>(h_[2]) * r_[2] + static_cast<quint64>(h_[3]) * r_[1]
            + static_cast<quint64>(h_[4]) * r_[0];

        quint64 carry = d0 >> 26;
        h_[0] = static_cast<quint32>(d0 & 0x3FFFFFF);
        d1 += carry;
        carry = d1 >> 26;
        h_[1] = static_cast<quint32>(d1 & 0x3FFFFFF);
        d2 += carry;
        carry = d2 >> 26;
        h_[2] = static_cast<quint32>(d2 & 0x3FFFFFF);
        d3 += carry;
        carry = d3 >> 26;
        h_[3] = static_cast<quint32>(d3 & 0x3FFFFFF);
        d4 += carry;
        carry = d4 >> 26;
        h_[4] = static_cast<quint32>(d4 & 0x3FFFFFF);
        // 2^130 ≡ 5, so what fell off the top comes back multiplied by five.
        h_[0] += static_cast<quint32>(carry) * 5;
        carry = h_[0] >> 26;
        h_[0] &= 0x3FFFFFF;
        h_[1] += static_cast<quint32>(carry);
    }

    void carry() {
        quint32 c = h_[1] >> 26;
        h_[1] &= 0x3FFFFFF;
        h_[2] += c;
        c = h_[2] >> 26;
        h_[2] &= 0x3FFFFFF;
        h_[3] += c;
        c = h_[3] >> 26;
        h_[3] &= 0x3FFFFFF;
        h_[4] += c;
        c = h_[4] >> 26;
        h_[4] &= 0x3FFFFFF;
        h_[0] += c * 5;
        c = h_[0] >> 26;
        h_[0] &= 0x3FFFFFF;
        h_[1] += c;
    }

    quint32 r_[5]{};
    quint32 h_[5]{};
    quint32 pad_[4]{};
    uchar buffer_[16]{};
    std::size_t pending_ = 0;
};

QByteArray padTo16(qsizetype length) {
    const qsizetype remainder = length % 16;
    return remainder == 0 ? QByteArray() : QByteArray(16 - remainder, '\0');
}

} // namespace

QByteArray ChaCha20Poly1305::block(const QByteArray &key, quint32 counter, const QByteArray &nonce) {
    if (key.size() != keyBytes || nonce.size() != nonceBytes) return {};
    const auto *keyBytesPtr = reinterpret_cast<const uchar *>(key.constData());
    const auto *noncePtr = reinterpret_cast<const uchar *>(nonce.constData());

    std::array<quint32, 16> state{};
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    for (int index = 0; index < 8; ++index) state[4 + index] = readLittle32(keyBytesPtr + index * 4);
    state[12] = counter;
    for (int index = 0; index < 3; ++index) state[13 + index] = readLittle32(noncePtr + index * 4);

    std::array<quint32, 16> working = state;
    for (int round = 0; round < 10; ++round) {
        quarterRound(working[0], working[4], working[8], working[12]);
        quarterRound(working[1], working[5], working[9], working[13]);
        quarterRound(working[2], working[6], working[10], working[14]);
        quarterRound(working[3], working[7], working[11], working[15]);
        quarterRound(working[0], working[5], working[10], working[15]);
        quarterRound(working[1], working[6], working[11], working[12]);
        quarterRound(working[2], working[7], working[8], working[13]);
        quarterRound(working[3], working[4], working[9], working[14]);
    }

    QByteArray output(64, Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(output.data());
    for (int index = 0; index < 16; ++index)
        writeLittle32(out + index * 4, working[index] + state[index]);
    return output;
}

QByteArray ChaCha20Poly1305::chacha20(
    const QByteArray &key,
    quint32 counter,
    const QByteArray &nonce,
    const QByteArray &data
) {
    if (key.size() != keyBytes || nonce.size() != nonceBytes) return {};
    QByteArray output(data.size(), Qt::Uninitialized);
    for (qsizetype offset = 0; offset < data.size(); offset += 64) {
        const QByteArray stream = block(key, counter + static_cast<quint32>(offset / 64), nonce);
        const qsizetype span = std::min<qsizetype>(64, data.size() - offset);
        for (qsizetype index = 0; index < span; ++index)
            output[offset + index] = static_cast<char>(data.at(offset + index) ^ stream.at(index));
    }
    return output;
}

QByteArray ChaCha20Poly1305::poly1305(const QByteArray &key, const QByteArray &data) {
    if (key.size() != 32) return {};
    Poly1305State state(reinterpret_cast<const uchar *>(key.constData()));
    state.update(reinterpret_cast<const uchar *>(data.constData()), static_cast<std::size_t>(data.size()));
    return state.finish();
}

QByteArray ChaCha20Poly1305::seal(
    const QByteArray &key,
    const QByteArray &nonce,
    const QByteArray &plaintext,
    const QByteArray &additionalData
) {
    if (key.size() != keyBytes || nonce.size() != nonceBytes) return {};
    // The authenticator key is the first half of the keystream block zero; the
    // message itself starts at block one.
    const QByteArray macKey = block(key, 0, nonce).left(32);
    const QByteArray ciphertext = chacha20(key, 1, nonce, plaintext);

    QByteArray authenticated;
    authenticated.append(additionalData);
    authenticated.append(padTo16(additionalData.size()));
    authenticated.append(ciphertext);
    authenticated.append(padTo16(ciphertext.size()));
    QByteArray lengths(16, Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(lengths.data());
    writeLittle64(out, static_cast<quint64>(additionalData.size()));
    writeLittle64(out + 8, static_cast<quint64>(ciphertext.size()));
    authenticated.append(lengths);

    return nonce + ciphertext + poly1305(macKey, authenticated);
}

std::optional<QByteArray> ChaCha20Poly1305::open(
    const QByteArray &key,
    const QByteArray &combined,
    const QByteArray &additionalData
) {
    if (key.size() != keyBytes) return std::nullopt;
    if (combined.size() < nonceBytes + tagBytes) return std::nullopt;
    const QByteArray nonce = combined.left(nonceBytes);
    const QByteArray ciphertext = combined.mid(nonceBytes, combined.size() - nonceBytes - tagBytes);
    const QByteArray tag = combined.right(tagBytes);

    const QByteArray macKey = block(key, 0, nonce).left(32);
    QByteArray authenticated;
    authenticated.append(additionalData);
    authenticated.append(padTo16(additionalData.size()));
    authenticated.append(ciphertext);
    authenticated.append(padTo16(ciphertext.size()));
    QByteArray lengths(16, Qt::Uninitialized);
    auto *out = reinterpret_cast<uchar *>(lengths.data());
    writeLittle64(out, static_cast<quint64>(additionalData.size()));
    writeLittle64(out + 8, static_cast<quint64>(ciphertext.size()));
    authenticated.append(lengths);

    // The tag is checked before anything is handed back, and in constant time.
    if (!equalInConstantTime(tag, poly1305(macKey, authenticated))) return std::nullopt;
    return chacha20(key, 1, nonce, ciphertext);
}

QByteArray ChaCha20Poly1305::randomNonce() {
    QByteArray nonce(nonceBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(nonce.begin(), nonce.end());
    return nonce;
}

QByteArray ChaCha20Poly1305::randomKey() {
    QByteArray key(keyBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(key.begin(), key.end());
    return key;
}

} // namespace yobro::spike
