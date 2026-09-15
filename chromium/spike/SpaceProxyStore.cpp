#include "spike/SpaceProxyStore.hpp"

#include "spike/Localization.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace yobro::spike {
namespace {

constexpr auto socksValue = "SOCKS5";
constexpr auto httpValue = "HTTP CONNECT";
constexpr auto httpsValue = "HTTPS / TLS";

QString typeValue(SpaceProxyType type) {
    switch (type) {
    case SpaceProxyType::socks5: return QString::fromLatin1(socksValue);
    case SpaceProxyType::httpConnect: return QString::fromLatin1(httpValue);
    case SpaceProxyType::httpsConnect: return QString::fromLatin1(httpsValue);
    }
    return QString::fromLatin1(socksValue);
}

SpaceProxyType typeFromValue(const QString &value) {
    if (value == QLatin1String(httpValue)) return SpaceProxyType::httpConnect;
    if (value == QLatin1String(httpsValue)) return SpaceProxyType::httpsConnect;
    return SpaceProxyType::socks5;
}

QStringList trimmedList(const QStringList &values) {
    QStringList result;
    for (const QString &value : values) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) result.append(trimmed);
    }
    return result;
}

QStringList stringsOf(const QJsonObject &root, const QString &key) {
    QStringList values;
    for (const QJsonValue &value : root.value(key).toArray()) {
        if (value.isString()) values.append(value.toString());
    }
    return values;
}

QJsonArray arrayOf(const QStringList &values) {
    QJsonArray array;
    for (const QString &value : values) array.append(value);
    return array;
}

#if defined(__APPLE__)
CFDictionaryRef makeQuery(const std::string &service, const QString &space, bool wantData) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFStringRef serviceValue = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(service.data()),
        static_cast<CFIndex>(service.size()),
        kCFStringEncodingUTF8,
        false
    );
    CFDictionarySetValue(query, kSecAttrService, serviceValue);
    CFRelease(serviceValue);
    const QByteArray account = space.toUtf8();
    CFStringRef accountValue = CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(account.constData()),
        account.size(),
        kCFStringEncodingUTF8,
        false
    );
    CFDictionarySetValue(query, kSecAttrAccount, accountValue);
    CFRelease(accountValue);
    if (wantData) {
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    }
    return query;
}

QString keychainMessage(OSStatus status) {
    CFStringRef message = SecCopyErrorMessageString(status, nullptr);
    if (!message) {
        return L(QStringLiteral("Schlüsselbundfehler"), QStringLiteral("Keychain error"));
    }
    const CFIndex length = CFStringGetLength(message);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<std::size_t>(capacity), '\0');
    const bool converted = CFStringGetCString(message, buffer.data(), capacity, kCFStringEncodingUTF8);
    CFRelease(message);
    if (!converted) {
        return L(QStringLiteral("Schlüsselbundfehler"), QStringLiteral("Keychain error"));
    }
    return QString::fromUtf8(buffer.c_str());
}
#endif

} // namespace

QString SpaceProxyConfig::cleanHost() const {
    QString value = host.trimmed();
    for (const QString &scheme : {
             QStringLiteral("https://"), QStringLiteral("http://"),
             QStringLiteral("socks5://"), QStringLiteral("socks5h://"),
         }) {
        if (value.startsWith(scheme, Qt::CaseInsensitive)) {
            value = value.mid(scheme.size());
            break;
        }
    }
    return value.remove(QLatin1Char('/'));
}

QString SpaceProxyConfig::displayLabel() const {
    const QString trimmedLabel = label.trimmed();
    if (!trimmedLabel.isEmpty()) return trimmedLabel;
    const QString cleaned = cleanHost();
    if (cleaned.isEmpty()) return L(QStringLiteral("Kein Proxy"), QStringLiteral("No proxy"));
    return QStringLiteral("%1:%2").arg(cleaned).arg(port);
}

QString SpaceProxyConfig::validationProblem() const {
    if (cleanHost().isEmpty()) {
        return L(
            QStringLiteral("Bitte einen Server-Hostnamen oder eine IP-Adresse eingeben."),
            QStringLiteral("Please enter a server host name or IP address.")
        );
    }
    if (port < 1 || port > 65535) {
        return L(
            QStringLiteral("Bitte einen gültigen Port zwischen 1 und 65535 eingeben."),
            QStringLiteral("Please enter a valid port between 1 and 65535.")
        );
    }
    if (type == SpaceProxyType::httpsConnect) {
        // Downgrading to a plain connection would send the proxy credentials in
        // the clear, so this is refused rather than silently weakened.
        return L(
            QStringLiteral("Chromium kann den Proxy nur unverschlüsselt erreichen. Wähle SOCKS5 oder "
                           "HTTP CONNECT; eine TLS-Verbindung zum Proxy wird hier nicht unterstützt."),
            QStringLiteral("Chromium can only reach the proxy without encryption. Choose SOCKS5 or "
                           "HTTP CONNECT; a TLS connection to the proxy is not supported here.")
        );
    }
    return {};
}

SpaceProxyStore::SpaceProxyStore(std::filesystem::path profileDirectory)
    : file_(profileDirectory / "space-proxies.json"),
      service_("local.yobro.chromium.proxy." + profileDirectory.string()) {
    QFile input(QString::fromStdString(file_.string()));
    if (!input.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(input.readAll()).object();
    for (auto entry = root.begin(); entry != root.end(); ++entry) {
        if (!entry.value().isObject()) continue;
        const QJsonObject value = entry.value().toObject();
        SpaceProxyConfig config;
        config.enabled = value.value(QStringLiteral("enabled")).toBool(true);
        config.label = value.value(QStringLiteral("label")).toString();
        config.type = typeFromValue(value.value(QStringLiteral("type")).toString());
        config.host = value.value(QStringLiteral("host")).toString();
        config.port = value.value(QStringLiteral("port")).toInt(1080);
        config.username = value.value(QStringLiteral("username")).toString();
        config.matchDomains = stringsOf(value, QStringLiteral("matchDomains"));
        config.excludedDomains = stringsOf(value, QStringLiteral("excludedDomains"));
        configs_[entry.key()] = config;
    }
}

std::optional<SpaceProxyConfig> SpaceProxyStore::config(const QString &space) const {
    const auto found = configs_.find(space);
    if (found == configs_.end()) return std::nullopt;
    return found->second;
}

std::vector<QString> SpaceProxyStore::spaces() const {
    std::vector<QString> values;
    values.reserve(configs_.size());
    // std::map already orders by space name.
    for (const auto &entry : configs_) values.push_back(entry.first);
    return values;
}

QString SpaceProxyStore::set(const QString &space, const SpaceProxyConfig &config) {
    if (space.isEmpty()) {
        return L(QStringLiteral("Ohne Space-Namen kann kein Proxy gespeichert werden."),
                 QStringLiteral("A proxy cannot be stored without a space name."));
    }
    if (const QString problem = config.validationProblem(); !problem.isEmpty()) return problem;

    SpaceProxyConfig metadata = config;
    metadata.matchDomains = trimmedList(config.matchDomains);
    metadata.excludedDomains = trimmedList(config.excludedDomains);
    metadata.host = config.cleanHost();
    metadata.username = config.username.trimmed();
    if (!config.password.isEmpty()) {
        if (const QString problem = writePassword(space, config.password); !problem.isEmpty())
            return problem;
    }
    // The password never reaches the file.
    metadata.password.clear();
    const auto previous = configs_.find(space);
    const std::optional<SpaceProxyConfig> restore =
        previous == configs_.end() ? std::nullopt : std::optional(previous->second);
    configs_[space] = metadata;
    if (!save()) {
        if (restore) configs_[space] = *restore;
        else configs_.erase(space);
        return L(QStringLiteral("Die Proxy-Einstellungen konnten nicht gespeichert werden."),
                 QStringLiteral("The proxy settings could not be saved."));
    }
    return {};
}

QString SpaceProxyStore::remove(const QString &space) {
    const auto found = configs_.find(space);
    if (found == configs_.end()) return {};
    const SpaceProxyConfig previous = found->second;
    configs_.erase(found);
    if (!save()) {
        configs_[space] = previous;
        return L(QStringLiteral("Die Proxy-Einstellungen konnten nicht gespeichert werden."),
                 QStringLiteral("The proxy settings could not be saved."));
    }
    erasePassword(space);
    return {};
}

QString SpaceProxyStore::rename(const QString &from, const QString &to) {
    const auto found = configs_.find(from);
    if (found == configs_.end() || from == to) return {};
    if (to.isEmpty()) {
        return L(QStringLiteral("Ohne Space-Namen kann kein Proxy gespeichert werden."),
                 QStringLiteral("A proxy cannot be stored without a space name."));
    }
    const SpaceProxyConfig config = found->second;
    if (const auto password = readPassword(from)) {
        if (const QString problem = writePassword(to, *password); !problem.isEmpty()) return problem;
    }
    configs_.erase(from);
    configs_[to] = config;
    if (!save()) {
        configs_.erase(to);
        configs_[from] = config;
        return L(QStringLiteral("Die Proxy-Einstellungen konnten nicht gespeichert werden."),
                 QStringLiteral("The proxy settings could not be saved."));
    }
    erasePassword(from);
    return {};
}

SpaceProxyConfig SpaceProxyStore::resolved(const QString &space, const SpaceProxyConfig &config) const {
    SpaceProxyConfig result = config;
    if (result.password.isEmpty()) {
        if (const auto password = readPassword(space)) result.password = *password;
    }
    return result;
}

bool SpaceProxyStore::save() const {
    QJsonObject root;
    for (const auto &entry : configs_) {
        const SpaceProxyConfig &config = entry.second;
        QJsonObject value;
        value.insert(QStringLiteral("enabled"), config.enabled);
        value.insert(QStringLiteral("label"), config.label);
        value.insert(QStringLiteral("type"), typeValue(config.type));
        value.insert(QStringLiteral("host"), config.host);
        value.insert(QStringLiteral("port"), config.port);
        value.insert(QStringLiteral("username"), config.username);
        value.insert(QStringLiteral("matchDomains"), arrayOf(config.matchDomains));
        value.insert(QStringLiteral("excludedDomains"), arrayOf(config.excludedDomains));
        root.insert(entry.first, value);
    }
    std::error_code code;
    std::filesystem::create_directories(file_.parent_path(), code);
    QSaveFile file(QString::fromStdString(file_.string()));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return file.commit();
}

#if defined(__APPLE__)

std::optional<QString> SpaceProxyStore::readPassword(const QString &space) const {
    CFDictionaryRef query = makeQuery(service_, space, true);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result) {
        if (result) CFRelease(result);
        return std::nullopt;
    }
    const auto data = static_cast<CFDataRef>(result);
    const QString password = QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        static_cast<int>(CFDataGetLength(data))
    );
    CFRelease(result);
    return password;
}

QString SpaceProxyStore::writePassword(const QString &space, const QString &password) {
    const QByteArray bytes = password.toUtf8();
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size()
    );
    CFDictionaryRef query = makeQuery(service_, space, false);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(update, kSecValueData, data);
    OSStatus status = SecItemUpdate(query, update);
    CFRelease(update);
    if (status == errSecItemNotFound) {
        CFMutableDictionaryRef item = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, query);
        CFDictionarySetValue(item, kSecValueData, data);
        CFDictionarySetValue(item, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        status = SecItemAdd(item, nullptr);
        CFRelease(item);
    }
    CFRelease(query);
    CFRelease(data);
    if (status != errSecSuccess) return keychainMessage(status);
    return {};
}

void SpaceProxyStore::erasePassword(const QString &space) {
    CFDictionaryRef query = makeQuery(service_, space, false);
    (void)SecItemDelete(query);
    CFRelease(query);
}

#else

std::optional<QString> SpaceProxyStore::readPassword(const QString &) const { return std::nullopt; }

QString SpaceProxyStore::writePassword(const QString &, const QString &) {
    return L(
        QStringLiteral("Proxy-Passwörter brauchen einen Systemschlüsselbund, der hier fehlt."),
        QStringLiteral("Proxy passwords need a system keychain, which is missing here.")
    );
}

void SpaceProxyStore::erasePassword(const QString &) {}

#endif

} // namespace yobro::spike
