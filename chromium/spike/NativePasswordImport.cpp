#include "spike/NativePasswordImport.hpp"

#include "spike/ImportWorker.hpp"
#include "spike/Localization.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#if defined(__APPLE__)
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace yobro::spike {
namespace {

constexpr int aesKeyLength = 16;
constexpr int aesBlockLength = 16;
constexpr auto versionPrefix = "v10";

bool isChromiumFamily(const QString &browser) {
    return browser == QStringLiteral("Chrome") || browser == QStringLiteral("Brave")
        || browser == QStringLiteral("Arc");
}

/// Only public web addresses are worth importing; anything else is dropped.
bool isWebUrl(const QString &url) {
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty()) return false;
    const QString scheme = parsed.scheme().toLower();
    return scheme == QStringLiteral("http") || scheme == QStringLiteral("https");
}

#if defined(__APPLE__)
QString accessDeniedMessage() {
    return L(
        QStringLiteral("Passwortzugriff nicht freigegeben. Du kannst es erneut versuchen oder eine "
                       "CSV-Datei verwenden."),
        QStringLiteral("Password access was not granted. Try again or use a CSV file.")
    );
}
#endif

} // namespace

QStringList NativePasswordImport::supportedBrowsers() {
    return {
        QStringLiteral("Chrome"), QStringLiteral("Brave"), QStringLiteral("Arc"),
        QStringLiteral("Firefox"), QStringLiteral("Safari"),
    };
}

#if defined(__APPLE__)

QByteArray NativePasswordImport::safeStorageSecret(const QString &browser) {
    const QByteArray service = (browser + QStringLiteral(" Safe Storage")).toUtf8();
    CFStringRef serviceValue = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(service.constData()),
        service.size(),
        kCFStringEncodingUTF8,
        false
    );
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, serviceValue);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    CFRelease(serviceValue);
    if (status != errSecSuccess || !result) {
        if (result) CFRelease(result);
        return {};
    }
    const auto data = static_cast<CFDataRef>(result);
    const QByteArray secret(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        static_cast<int>(CFDataGetLength(data))
    );
    CFRelease(result);
    return secret;
}

QByteArray NativePasswordImport::deriveKey(const QByteArray &secret) {
    if (secret.isEmpty()) return {};
    const QByteArray salt = QByteArrayLiteral("saltysalt");
    QByteArray key(aesKeyLength, '\0');
    const int status = CCKeyDerivationPBKDF(
        kCCPBKDF2,
        secret.constData(),
        static_cast<size_t>(secret.size()),
        reinterpret_cast<const uint8_t *>(salt.constData()),
        static_cast<size_t>(salt.size()),
        kCCPRFHmacAlgSHA1,
        1003,
        reinterpret_cast<uint8_t *>(key.data()),
        static_cast<size_t>(key.size())
    );
    if (status != kCCSuccess) return {};
    return key;
}

std::optional<QString> NativePasswordImport::decrypt(
    const QByteArray &encrypted,
    const QByteArray &key
) {
    // Chromium on macOS: PBKDF2-SHA1 / AES-128-CBC / PKCS#7 behind a "v10" tag.
    // An unknown format must never be handed back as if it were plain text.
    if (key.size() != aesKeyLength) return std::nullopt;
    if (!encrypted.startsWith(versionPrefix)) return std::nullopt;
    const QByteArray payload = encrypted.mid(3);
    if (payload.isEmpty() || payload.size() % aesBlockLength != 0) return std::nullopt;

    const QByteArray iv(aesBlockLength, ' ');
    QByteArray output(payload.size() + aesBlockLength, '\0');
    std::size_t written = 0;
    const CCCryptorStatus status = CCCrypt(
        kCCDecrypt,
        kCCAlgorithmAES,
        kCCOptionPKCS7Padding,
        key.constData(),
        static_cast<size_t>(key.size()),
        iv.constData(),
        payload.constData(),
        static_cast<size_t>(payload.size()),
        output.data(),
        static_cast<size_t>(output.size()),
        &written
    );
    if (status != kCCSuccess) return std::nullopt;
    output.truncate(static_cast<int>(written));
    const QString text = QString::fromUtf8(output);
    // A payload that decrypts to invalid UTF-8 is not a password.
    if (text.toUtf8() != output) return std::nullopt;
    return text;
}

ImportedLoginSet NativePasswordImport::safariLogins() {
    ImportedLoginSet result;
    // Safari's web logins live in the shared macOS internet password store. The
    // protected iCloud and Passwords-app access groups are not reachable from
    // here, which is why the warning below is always added.
    for (CFStringRef protocolValue : {kSecAttrProtocolHTTP, kSecAttrProtocolHTTPS}) {
        CFMutableDictionaryRef query = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
        );
        CFDictionarySetValue(query, kSecClass, kSecClassInternetPassword);
        CFDictionarySetValue(query, kSecAttrProtocol, protocolValue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
        CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecReturnPersistentRef, kCFBooleanTrue);
        CFTypeRef matches = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &matches);
        CFRelease(query);
        if (status == errSecItemNotFound) continue;
        if (status != errSecSuccess || !matches) {
            if (matches) CFRelease(matches);
            result.problem = accessDeniedMessage();
            return result;
        }

        const auto items = static_cast<CFArrayRef>(matches);
        const CFIndex count = CFArrayGetCount(items);
        for (CFIndex index = 0; index < count; ++index) {
            const auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, index));
            const auto server = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrServer));
            const auto reference = static_cast<CFDataRef>(CFDictionaryGetValue(item, kSecValuePersistentRef));
            if (!server || !reference) continue;
            const QString host = QString::fromCFString(server);
            if (host.isEmpty()) continue;

            QString url = (protocolValue == kSecAttrProtocolHTTPS ? QStringLiteral("https://")
                                                                 : QStringLiteral("http://")) + host;
            if (const auto port = static_cast<CFNumberRef>(CFDictionaryGetValue(item, kSecAttrPort))) {
                int value = 0;
                if (CFNumberGetValue(port, kCFNumberIntType, &value) && value > 0)
                    url += QStringLiteral(":%1").arg(value);
            }
            if (!isWebUrl(url)) continue;

            CFMutableDictionaryRef lookup = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
            );
            CFDictionarySetValue(lookup, kSecValuePersistentRef, reference);
            CFDictionarySetValue(lookup, kSecReturnData, kCFBooleanTrue);
            CFDictionarySetValue(lookup, kSecMatchLimit, kSecMatchLimitOne);
            CFTypeRef value = nullptr;
            const OSStatus read = SecItemCopyMatching(lookup, &value);
            CFRelease(lookup);
            if (read != errSecSuccess || !value) {
                if (value) CFRelease(value);
                // A denied or cancelled prompt stops the whole operation rather
                // than quietly importing half the logins.
                CFRelease(matches);
                result.problem = accessDeniedMessage();
                return result;
            }
            const auto data = static_cast<CFDataRef>(value);
            const QString password = QString::fromUtf8(
                reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                static_cast<int>(CFDataGetLength(data))
            );
            CFRelease(value);
            if (password.isEmpty()) continue;

            QString account;
            if (const auto stored = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrAccount)))
                account = QString::fromCFString(stored);
            result.logins.push_back({url, account, password});
        }
        CFRelease(matches);
    }
    result.warnings.append(L(
        QStringLiteral("Nur zugängliche lokale macOS-Web-Passwörter wurden gelesen. Für iCloud und "
                       "weitere Safari-Passwörter: App „Passwörter“ → Ablage → Alle Passwörter "
                       "exportieren."),
        QStringLiteral("Only accessible local macOS web passwords were read. For iCloud and other "
                       "Safari passwords: Passwords app → File → Export All Passwords.")
    ));
    return result;
}

#else

ImportedLoginSet NativePasswordImport::safariLogins() {
    ImportedLoginSet result;
    result.problem = L(
        QStringLiteral("Safari-Passwörter gibt es nur auf macOS."),
        QStringLiteral("Safari passwords exist only on macOS.")
    );
    return result;
}

QByteArray NativePasswordImport::safeStorageSecret(const QString &) { return {}; }
QByteArray NativePasswordImport::deriveKey(const QByteArray &) { return {}; }
std::optional<QString> NativePasswordImport::decrypt(const QByteArray &, const QByteArray &) {
    return std::nullopt;
}

#endif

ImportedLoginSet NativePasswordImport::load(
    const QString &browser,
    const QString &profilePath,
    const QString &primaryPassword
) {
    ImportedLoginSet result;

    if (browser == QStringLiteral("Safari")) return safariLogins();

    if (browser == QStringLiteral("Firefox")) {
        const ImportWorker::Result answer = ImportWorker::call({
            {QStringLiteral("action"), QStringLiteral("firefox_passwords")},
            {QStringLiteral("path"), profilePath},
            {QStringLiteral("primaryPassword"), primaryPassword},
        });
        if (!answer.problem.isEmpty()) {
            result.problem = answer.problem;
            return result;
        }
        const QJsonObject payload = answer.value.toObject();
        result.needsPrimaryPassword =
            payload.value(QStringLiteral("needsPrimaryPassword")).toBool(false);
        for (const QJsonValue &warning : payload.value(QStringLiteral("warnings")).toArray()) {
            if (warning.isString()) result.warnings.append(warning.toString());
        }
        for (const QJsonValue &entry : payload.value(QStringLiteral("passwords")).toArray()) {
            const QJsonObject row = entry.toObject();
            const QString url = row.value(QStringLiteral("url")).toString();
            const QString password = row.value(QStringLiteral("password")).toString();
            if (!isWebUrl(url) || password.isEmpty()) continue;
            result.logins.push_back({
                url,
                row.value(QStringLiteral("username")).toString(),
                password,
            });
        }
        return result;
    }

    if (!isChromiumFamily(browser)) {
        result.problem = L(
            QStringLiteral("Dieser Browser wird nicht unterstützt."),
            QStringLiteral("This browser is not supported.")
        );
        return result;
    }

    const ImportWorker::Result answer = ImportWorker::call({
        {QStringLiteral("action"), QStringLiteral("chromium_passwords")},
        {QStringLiteral("path"), profilePath},
    });
    if (!answer.problem.isEmpty()) {
        result.problem = answer.problem;
        return result;
    }
    const QJsonArray rows = answer.value.toArray();
    if (rows.isEmpty()) return result;

    const QByteArray secret = safeStorageSecret(browser);
    if (secret.isEmpty()) {
#if defined(__APPLE__)
        result.problem = L(
            QStringLiteral("Der Schlüsselbund-Eintrag für %1 ist nicht verfügbar oder der Zugriff wurde "
                           "nicht freigegeben. Öffne den Quellbrowser auf diesem Mac oder verwende einen "
                           "CSV-Export."),
            QStringLiteral("The keychain entry for %1 is unavailable or access was not granted. Open the "
                           "source browser on this Mac or use a CSV export.")
        ).arg(browser);
#else
        result.problem = accessDeniedMessage();
#endif
        return result;
    }
    const QByteArray key = deriveKey(secret);
    if (key.isEmpty()) {
        result.problem = L(
            QStringLiteral("Passwortschlüssel konnte nicht gelesen werden."),
            QStringLiteral("Could not read the password key.")
        );
        return result;
    }

    std::size_t unsupported = 0;
    for (const QJsonValue &entry : rows) {
        const QJsonObject row = entry.toObject();
        const QString url = row.value(QStringLiteral("url")).toString();
        const QByteArray encrypted = QByteArray::fromBase64(
            row.value(QStringLiteral("encrypted")).toString().toLatin1()
        );
        if (!isWebUrl(url) || encrypted.isEmpty()) continue;
        const auto password = decrypt(encrypted, key);
        if (!password || password->isEmpty()) {
            ++unsupported;
            continue;
        }
        result.logins.push_back({
            url,
            row.value(QStringLiteral("username")).toString(),
            *password,
        });
    }
    if (unsupported > 0) {
        result.warnings.append(L(
            QStringLiteral("%1 Passwörter konnten nicht entschlüsselt werden. Bitte diese Konten über "
                           "einen CSV-Export übernehmen."),
            QStringLiteral("Could not decrypt %1 passwords. Transfer these accounts using a CSV export.")
        ).arg(unsupported));
    }
    return result;
}

} // namespace yobro::spike
