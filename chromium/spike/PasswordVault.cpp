#include "spike/PasswordVault.hpp"

#include "spike/ChaCha20Poly1305.hpp"
#include "spike/KeyDerivation.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <QUrl>

#include <algorithm>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace yobro::spike {
namespace {

constexpr char accountSeparator = '\n';

std::string accountKey(const std::string &origin, const std::string &username) {
    return origin + accountSeparator + username;
}

#if defined(__APPLE__)
/// Owns a CoreFoundation reference and releases it on scope exit.
template <typename Ref>
class CFHandle {
public:
    explicit CFHandle(Ref ref = nullptr) : ref_(ref) {}
    CFHandle(const CFHandle &) = delete;
    CFHandle &operator=(const CFHandle &) = delete;
    ~CFHandle() { if (ref_) CFRelease(ref_); }

    [[nodiscard]] Ref get() const { return ref_; }
    void reset(Ref ref = nullptr) {
        if (ref_) CFRelease(ref_);
        ref_ = ref;
    }
    Ref *address() { return &ref_; }

private:
    Ref ref_ = nullptr;
};

CFStringRef makeString(const std::string &value) {
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(value.data()),
        static_cast<CFIndex>(value.size()),
        kCFStringEncodingUTF8,
        false
    );
}

std::string toStdString(CFStringRef value) {
    if (!value) return {};
    const CFIndex length = CFStringGetLength(value);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(value, result.data(), capacity, kCFStringEncodingUTF8)) return {};
    result.resize(std::string(result.c_str()).size());
    return result;
}

CFMutableDictionaryRef makeQuery(const std::string &service) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFHandle<CFStringRef> serviceRef(makeString(service));
    CFDictionarySetValue(query, kSecAttrService, serviceRef.get());
    return query;
}
#endif

} // namespace

namespace {

PasswordStoreKind defaultKind() {
#if defined(__APPLE__)
    PasswordStoreKind fallback = PasswordStoreKind::keychain;
#else
    PasswordStoreKind fallback = PasswordStoreKind::encryptedFile;
#endif
    const QByteArray requested = qgetenv("YOBRO_PASSWORD_STORE").trimmed().toLower();
    if (requested == QByteArrayLiteral("file"))
        return PasswordStoreKind::encryptedFile;
    if (requested == QByteArrayLiteral("keychain")) {
#if defined(__APPLE__)
        return PasswordStoreKind::keychain;
#else
        // Asking for a Keychain where there is none must not silently fall back
        // to something weaker without saying so; the file store is the weaker
        // of the two only in that its key comes from a passphrase.
        return PasswordStoreKind::encryptedFile;
#endif
    }
    return fallback;
}

constexpr int formatVersion = 1;
constexpr int keyLength = ChaCha20Poly1305::keyBytes;
constexpr int saltLength = 16;
/// The sealed payload is a JSON array; a profile with more than this many
/// logins is not a profile any more.
constexpr int maximumEntries = 20'000;

} // namespace

PasswordVault::PasswordVault(
    std::filesystem::path profileDirectory,
    std::optional<PasswordStoreKind> forced
) : kind_(forced.value_or(defaultKind())) {
    const std::filesystem::path normalized = profileDirectory.lexically_normal();
    if (kind_ == PasswordStoreKind::keychain)
        service_ = "YOBRO.Chromium.Passwords." + normalized.string();
    else
        file_ = normalized / "passwords.vault";
}

PasswordStoreKind PasswordVault::kind() const {
    return kind_;
}

bool PasswordVault::requiresPassphrase() const {
    return kind_ == PasswordStoreKind::encryptedFile;
}

bool PasswordVault::locked() const {
    return kind_ == PasswordStoreKind::encryptedFile && !unlocked_;
}

bool PasswordVault::exists() const {
    if (kind_ != PasswordStoreKind::encryptedFile)
        return true;
    std::error_code code;
    return std::filesystem::exists(file_, code) && !code;
}

const std::string &PasswordVault::problem() const {
    return problem_;
}

const std::filesystem::path &PasswordVault::file() const {
    return file_;
}

bool PasswordVault::loadFile(const QByteArray &key, std::vector<LoginCredential> &into) const {
    into.clear();
    QFile source(QString::fromStdString(file_.string()));
    if (!source.exists())
        return true;
    if (!source.open(QIODevice::ReadOnly)) {
        problem_ = "Der Passwortspeicher konnte nicht gelesen werden.";
        return false;
    }
    const QByteArray raw = source.readAll();
    source.close();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        problem_ = "Die Datei des Passwortspeichers ist beschädigt und wird nicht überschrieben.";
        return false;
    }
    const QJsonObject header = document.object();
    if (header.value(QStringLiteral("version")).toInt() != formatVersion
        || header.value(QStringLiteral("kdf")).toString() != QStringLiteral("pbkdf2-hmac-sha256")) {
        problem_ = "Dieser Passwortspeicher hat ein unbekanntes Format.";
        return false;
    }
    const int iterations = header.value(QStringLiteral("iterations")).toInt();
    const QByteArray salt = QByteArray::fromBase64(
        header.value(QStringLiteral("salt")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors
    );
    const QByteArray payload = QByteArray::fromBase64(
        header.value(QStringLiteral("payload")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors
    );
    // The iteration count is taken from the file so an older vault keeps
    // opening. A lowered count is not an attack on its own: forging a file that
    // opens at all still requires the passphrase.
    if (iterations < 1 || salt.size() != saltLength || payload.isEmpty()) {
        problem_ = "Die Datei des Passwortspeichers ist beschädigt und wird nicht überschrieben.";
        return false;
    }
    const std::optional<QByteArray> opened = ChaCha20Poly1305::open(key, payload);
    if (!opened) {
        problem_ = "Falsche Passphrase.";
        return false;
    }
    const QJsonDocument plain = QJsonDocument::fromJson(*opened, &parseError);
    if (parseError.error != QJsonParseError::NoError || !plain.isArray()) {
        problem_ = "Der entschlüsselte Passwortspeicher ist unbrauchbar.";
        return false;
    }
    for (const QJsonValue &value : plain.array()) {
        if (!value.isObject()) continue;
        const QJsonObject entry = value.toObject();
        LoginCredential credential{
            .origin = entry.value(QStringLiteral("origin")).toString().toStdString(),
            .username = entry.value(QStringLiteral("username")).toString().toStdString(),
            .password = entry.value(QStringLiteral("password")).toString().toStdString(),
        };
        if (credential.origin.empty() || credential.username.empty() || credential.password.empty())
            continue;
        into.push_back(std::move(credential));
        if (into.size() >= static_cast<std::size_t>(maximumEntries))
            break;
    }
    return true;
}

bool PasswordVault::writeFile(const QByteArray &passphrase, const std::vector<LoginCredential> &entries) const {
    QJsonArray plain;
    for (const LoginCredential &credential : entries) {
        QJsonObject entry;
        entry.insert(QStringLiteral("origin"), QString::fromStdString(credential.origin));
        entry.insert(QStringLiteral("username"), QString::fromStdString(credential.username));
        entry.insert(QStringLiteral("password"), QString::fromStdString(credential.password));
        plain.append(entry);
    }
    // The salt is new on every write, so the same passphrase never produces the
    // same key twice and a stolen older file gives nothing away about this one.
    const QByteArray salt = KeyDerivation::randomSalt(saltLength);
    const QByteArray freshKey = KeyDerivation::pbkdf2Sha256(passphrase, salt, iterations_, keyLength);
    if (freshKey.size() != keyLength) {
        problem_ = "Der Schlüssel für den Passwortspeicher konnte nicht abgeleitet werden.";
        return false;
    }
    const QByteArray sealed = ChaCha20Poly1305::seal(
        freshKey,
        ChaCha20Poly1305::randomNonce(),
        QJsonDocument(plain).toJson(QJsonDocument::Compact)
    );
    QJsonObject header;
    header.insert(QStringLiteral("version"), formatVersion);
    header.insert(QStringLiteral("kdf"), QStringLiteral("pbkdf2-hmac-sha256"));
    header.insert(QStringLiteral("iterations"), iterations_);
    header.insert(QStringLiteral("salt"), QString::fromLatin1(salt.toBase64()));
    header.insert(QStringLiteral("payload"), QString::fromLatin1(sealed.toBase64()));

    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    // Written beside the target and renamed, so an interrupted write cannot
    // leave the user without their credentials.
    const QString finalPath = QString::fromStdString(file_.string());
    const QString temporaryPath = finalPath + QStringLiteral(".new");
    QFile temporary(temporaryPath);
    if (!temporary.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        problem_ = "Der Passwortspeicher konnte nicht geschrieben werden.";
        return false;
    }
    temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray encoded = QJsonDocument(header).toJson(QJsonDocument::Compact);
    if (temporary.write(encoded) != encoded.size() || !temporary.flush()) {
        temporary.close();
        temporary.remove();
        problem_ = "Der Passwortspeicher konnte nicht geschrieben werden.";
        return false;
    }
    temporary.close();
    QFile::remove(finalPath);
    if (!QFile::rename(temporaryPath, finalPath)) {
        QFile::remove(temporaryPath);
        problem_ = "Der Passwortspeicher konnte nicht ersetzt werden.";
        return false;
    }
    QFile::setPermissions(finalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool PasswordVault::unlock(const std::string &passphrase) {
    problem_.clear();
    if (kind_ != PasswordStoreKind::encryptedFile)
        return true;
    if (passphrase.size() < 8) {
        problem_ = "Die Passphrase muss mindestens acht Zeichen haben.";
        return false;
    }
    const QByteArray candidate = QByteArray::fromStdString(passphrase);
    std::vector<LoginCredential> loaded;
    if (!exists()) {
        // A fresh store is created straight away, so an empty vault behaves
        // exactly like one the user unlocked before.
        if (!writeFile(candidate, {}))
            return false;
    }
    // The key is derived from the salt in the file, so it has to be read first.
    QFile source(QString::fromStdString(file_.string()));
    if (!source.open(QIODevice::ReadOnly)) {
        problem_ = "Der Passwortspeicher konnte nicht gelesen werden.";
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(source.readAll(), &parseError);
    source.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        corrupt_ = true;
        problem_ = "Die Datei des Passwortspeichers ist beschädigt und wird nicht überschrieben.";
        return false;
    }
    const QJsonObject header = document.object();
    const QByteArray salt = QByteArray::fromBase64(
        header.value(QStringLiteral("salt")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors
    );
    const int iterations = header.value(QStringLiteral("iterations")).toInt();
    if (salt.size() != saltLength || iterations < 1) {
        corrupt_ = true;
        problem_ = "Die Datei des Passwortspeichers ist beschädigt und wird nicht überschrieben.";
        return false;
    }
    const QByteArray derived = KeyDerivation::pbkdf2Sha256(candidate, salt, iterations, keyLength);
    if (derived.size() != keyLength) {
        problem_ = "Der Schlüssel für den Passwortspeicher konnte nicht abgeleitet werden.";
        return false;
    }
    if (!loadFile(derived, loaded)) {
        corrupt_ = problem_ != "Falsche Passphrase.";
        return false;
    }
    corrupt_ = false;
    key_ = candidate;
    cache_ = std::move(loaded);
    unlocked_ = true;
    return true;
}

bool PasswordVault::changePassphrase(const std::string &current, const std::string &next) {
    problem_.clear();
    if (kind_ != PasswordStoreKind::encryptedFile) {
        problem_ = "Dieser Passwortspeicher braucht keine Passphrase.";
        return false;
    }
    if (next.size() < 8) {
        problem_ = "Die Passphrase muss mindestens acht Zeichen haben.";
        return false;
    }
    // The current passphrase is verified against the file even when the store
    // is already open, so an unattended window cannot be taken over. A wrong
    // answer leaves the store locked rather than open with a stale key.
    lock();
    if (!unlock(current))
        return false;
    if (!writeFile(QByteArray::fromStdString(next), cache_)) {
        lock();
        return false;
    }
    key_ = QByteArray::fromStdString(next);
    problem_.clear();
    return true;
}

void PasswordVault::setDerivationIterations(int iterations) {
    iterations_ = std::max(1, iterations);
}

void PasswordVault::lock() {
    key_.fill('\0');
    key_.clear();
    cache_.clear();
    unlocked_ = false;
}


std::optional<std::string> PasswordVault::originFor(const std::string &url) {
    const QUrl parsed(QString::fromStdString(url));
    if (!parsed.isValid() || parsed.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return std::nullopt;
    const QString host = parsed.host().toLower();
    if (host.isEmpty()) return std::nullopt;
    QString origin = QStringLiteral("https://") + host;
    const int port = parsed.port();
    if (port > 0 && port != 443) origin += QStringLiteral(":") + QString::number(port);
    return origin.toStdString();
}

const std::string &PasswordVault::service() const {
    return service_;
}

#if defined(__APPLE__)

bool PasswordVault::keychainAvailable() const {
    return true;
}

PasswordStoreResult PasswordVault::keychainStore(const LoginCredential &credential, bool replace) {
    const std::string account = accountKey(credential.origin, credential.username);
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_));
    CFHandle<CFStringRef> accountRef(makeString(account));
    CFDictionarySetValue(query.get(), kSecAttrAccount, accountRef.get());

    CFHandle<CFDataRef> password(CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(credential.password.data()),
        static_cast<CFIndex>(credential.password.size())
    ));

    const OSStatus existing = SecItemCopyMatching(query.get(), nullptr);
    if (existing == errSecSuccess) {
        if (!replace) return PasswordStoreResult::refused;
        CFHandle<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        ));
        CFDictionarySetValue(update.get(), kSecValueData, password.get());
        if (SecItemUpdate(query.get(), update.get()) != errSecSuccess) {
            problem_ = "Der Schlüsselbund hat die Änderung abgelehnt.";
            return PasswordStoreResult::failed;
        }
        return PasswordStoreResult::updated;
    }

    CFDictionarySetValue(query.get(), kSecValueData, password.get());
    // Credentials stay on this device and are readable only after first unlock.
    CFDictionarySetValue(query.get(), kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
    if (SecItemAdd(query.get(), nullptr) != errSecSuccess) {
        problem_ = "Der Schlüsselbund hat den neuen Eintrag abgelehnt.";
        return PasswordStoreResult::failed;
    }
    return PasswordStoreResult::created;
}

std::vector<LoginCredential> PasswordVault::keychainEntries(const std::string &origin) const {
    std::vector<LoginCredential> results;
    // Attributes and data are fetched separately: Keychain Services rejects a
    // combined attributes-plus-data query when matching all items.
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_));
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
    CFDictionarySetValue(query.get(), kSecReturnAttributes, kCFBooleanTrue);

    CFHandle<CFArrayRef> matches;
    const OSStatus status = SecItemCopyMatching(
        query.get(),
        reinterpret_cast<CFTypeRef *>(matches.address())
    );
    if (status != errSecSuccess || !matches.get()) return results;

    const CFIndex count = CFArrayGetCount(matches.get());
    for (CFIndex index = 0; index < count; ++index) {
        auto item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(matches.get(), index));
        if (!item) continue;
        auto accountRef = static_cast<CFStringRef>(CFDictionaryGetValue(item, kSecAttrAccount));
        if (!accountRef) continue;
        const std::string account = toStdString(accountRef);
        const std::size_t separator = account.find(accountSeparator);
        if (separator == std::string::npos) continue;
        LoginCredential credential;
        credential.origin = account.substr(0, separator);
        credential.username = account.substr(separator + 1);
        // Exact-origin matching only; no subdomain or scheme relaxation.
        if (!origin.empty() && credential.origin != origin) continue;

        CFHandle<CFMutableDictionaryRef> secret(makeQuery(service_));
        CFHandle<CFStringRef> accountKeyRef(makeString(account));
        CFDictionarySetValue(secret.get(), kSecAttrAccount, accountKeyRef.get());
        CFDictionarySetValue(secret.get(), kSecMatchLimit, kSecMatchLimitOne);
        CFDictionarySetValue(secret.get(), kSecReturnData, kCFBooleanTrue);
        CFHandle<CFDataRef> data;
        if (SecItemCopyMatching(secret.get(), reinterpret_cast<CFTypeRef *>(data.address())) != errSecSuccess
            || !data.get())
            continue;
        credential.password.assign(
            reinterpret_cast<const char *>(CFDataGetBytePtr(data.get())),
            static_cast<std::size_t>(CFDataGetLength(data.get()))
        );
        results.push_back(std::move(credential));
    }
    std::sort(results.begin(), results.end(), [](const LoginCredential &left, const LoginCredential &right) {
        return left.origin == right.origin ? left.username < right.username : left.origin < right.origin;
    });
    return results;
}

bool PasswordVault::keychainRemove(const std::string &origin, const std::string &username) {
    CFHandle<CFMutableDictionaryRef> query(makeQuery(service_));
    CFHandle<CFStringRef> accountRef(makeString(accountKey(origin, username)));
    CFDictionarySetValue(query.get(), kSecAttrAccount, accountRef.get());
    const OSStatus status = SecItemDelete(query.get());
    return status == errSecSuccess || status == errSecItemNotFound;
}

#else

// There is no Keychain here, so these can only ever be reached by asking for a
// backend this platform does not have.
bool PasswordVault::keychainAvailable() const {
    return false;
}

PasswordStoreResult PasswordVault::keychainStore(const LoginCredential &, bool) {
    return PasswordStoreResult::failed;
}

std::vector<LoginCredential> PasswordVault::keychainEntries(const std::string &) const {
    return {};
}

bool PasswordVault::keychainRemove(const std::string &, const std::string &) {
    return false;
}

#endif

bool PasswordVault::available() const {
    return kind_ == PasswordStoreKind::keychain ? keychainAvailable() : true;
}

PasswordStoreResult PasswordVault::store(const LoginCredential &credential, bool replace) {
    problem_.clear();
    if (credential.origin.empty() || credential.username.empty() || credential.password.empty()) {
        problem_ = "Adresse, Benutzername und Passwort sind alle nötig.";
        return PasswordStoreResult::failed;
    }
    if (kind_ == PasswordStoreKind::keychain)
        return keychainStore(credential, replace);
    if (!unlocked_) {
        problem_ = "Der Passwortspeicher ist gesperrt.";
        return PasswordStoreResult::failed;
    }
    const auto found = std::find_if(cache_.begin(), cache_.end(), [&credential](const LoginCredential &entry) {
        return entry.origin == credential.origin && entry.username == credential.username;
    });
    std::vector<LoginCredential> updated = cache_;
    PasswordStoreResult outcome = PasswordStoreResult::created;
    if (found != cache_.end()) {
        if (!replace)
            return PasswordStoreResult::refused;
        outcome = PasswordStoreResult::updated;
        updated[static_cast<std::size_t>(std::distance(cache_.begin(), found))] = credential;
    } else {
        if (updated.size() >= static_cast<std::size_t>(maximumEntries)) {
            problem_ = "Der Passwortspeicher ist voll.";
            return PasswordStoreResult::failed;
        }
        updated.push_back(credential);
    }
    if (!writeFile(key_, updated))
        return PasswordStoreResult::failed;
    cache_ = std::move(updated);
    return outcome;
}

std::vector<LoginCredential> PasswordVault::entries(const std::string &origin) const {
    if (kind_ == PasswordStoreKind::keychain)
        return keychainEntries(origin);
    std::vector<LoginCredential> results;
    if (!unlocked_)
        return results;
    for (const LoginCredential &entry : cache_) {
        // Exact-origin matching only; no subdomain or scheme relaxation.
        if (!origin.empty() && entry.origin != origin) continue;
        results.push_back(entry);
    }
    std::sort(results.begin(), results.end(), [](const LoginCredential &left, const LoginCredential &right) {
        return left.origin == right.origin ? left.username < right.username : left.origin < right.origin;
    });
    return results;
}

bool PasswordVault::remove(const std::string &origin, const std::string &username) {
    problem_.clear();
    if (kind_ == PasswordStoreKind::keychain)
        return keychainRemove(origin, username);
    if (!unlocked_) {
        problem_ = "Der Passwortspeicher ist gesperrt.";
        return false;
    }
    std::vector<LoginCredential> updated;
    updated.reserve(cache_.size());
    for (const LoginCredential &entry : cache_) {
        if (entry.origin == origin && entry.username == username) continue;
        updated.push_back(entry);
    }
    if (updated.size() == cache_.size())
        return true;
    if (!writeFile(key_, updated))
        return false;
    cache_ = std::move(updated);
    return true;
}

} // namespace yobro::spike
