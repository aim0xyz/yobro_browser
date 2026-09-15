#include "spike/MailDiscovery.hpp"

#include "spike/Localization.hpp"

#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <memory>
#include <utility>
#include <vector>

namespace yobro::spike {
namespace {

const QString kGmail = QStringLiteral("Gmail");
const QString kIcloud = QStringLiteral("iCloud Mail");
const QString kSpacemail = QStringLiteral("Spacemail");
const QString kOutlook = QStringLiteral("Outlook / Microsoft 365");

QString expand(const QString &pattern, const QString &address, const QString &local, const QString &domain) {
    QString value = pattern.isEmpty() ? QStringLiteral("%EMAILADDRESS%") : pattern;
    value.replace(QStringLiteral("%EMAILADDRESS%"), address);
    value.replace(QStringLiteral("%EMAILLOCALPART%"), local);
    value.replace(QStringLiteral("%EMAILDOMAIN%"), domain);
    return value;
}

/// True when every listed authentication method is OAuth, so a password cannot
/// be used at all.
bool onlyOAuth(const QString &auth) {
    const QStringList methods = auth.toLower().split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (methods.isEmpty()) return false;
    for (const QString &method : methods)
        if (method.trimmed() != QStringLiteral("oauth2")) return false;
    return true;
}

} // namespace

QStringList MailDiscovery::presetNames() {
    return {kGmail, kIcloud, kSpacemail, kOutlook};
}

MailDiscoveryResult MailDiscovery::preset(const QString &name, const QString &address, const QString &id) {
    MailDiscoveryResult result;
    result.account.id = id;
    result.account.label = name;
    result.account.address = address;
    result.account.username = address;
    result.explanation = L(QStringLiteral("Passwort oder App-Passwort deines Mail-Anbieters verwenden."),
                           QStringLiteral("Use your mail provider's password or app password."));

    if (name == kGmail) {
        result.account.imapHost = QStringLiteral("imap.gmail.com");
        result.account.smtpHost = QStringLiteral("smtp.gmail.com");
        result.explanation = L(
            QStringLiteral("Gmail: App-Passwort erforderlich (2-Faktor-Anmeldung). Falls dein Konto "
                           "keine App-Passwörter erlaubt, wird OAuth benötigt; die Google-Anmeldung "
                           "ist in dieser Vorschau noch nicht eingerichtet."),
            QStringLiteral("Gmail: an app password is required (two-factor sign-in). If your account "
                           "does not allow app passwords, OAuth is needed; the Google sign-in is not "
                           "set up in this preview yet.")
        );
    } else if (name == kIcloud) {
        result.account.imapHost = QStringLiteral("imap.mail.me.com");
        result.account.smtpHost = QStringLiteral("smtp.mail.me.com");
        result.account.smtpPort = 587;
        result.account.smtpSecurity = QStringLiteral("starttls");
        result.explanation = L(
            QStringLiteral("iCloud Mail: ein anwendungsspezifisches Passwort deines Apple Accounts "
                           "verwenden. Apple Mail selbst ist ein Mailprogramm, kein Postfachanbieter."),
            QStringLiteral("iCloud Mail: use an app-specific password from your Apple Account. Apple "
                           "Mail itself is a mail program, not a mailbox provider.")
        );
    } else if (name == kSpacemail) {
        result.account.imapHost = QStringLiteral("mail.spacemail.com");
        result.account.smtpHost = QStringLiteral("mail.spacemail.com");
        result.explanation = L(
            QStringLiteral("Spacemail: vollständige E-Mail-Adresse und Postfachpasswort verwenden. "
                           "Die Serverdaten funktionieren auch bei eigenen Domains."),
            QStringLiteral("Spacemail: use the full email address and the mailbox password. The "
                           "server settings also work for your own domains.")
        );
    } else if (name == kOutlook) {
        result.account.imapHost = QStringLiteral("outlook.office365.com");
        result.account.smtpHost = QStringLiteral("smtp-mail.outlook.com");
        result.account.smtpPort = 587;
        result.account.smtpSecurity = QStringLiteral("starttls");
        result.oauthOnly = true;
        result.explanation = L(
            QStringLiteral("Microsoft verlangt moderne Anmeldung per OAuth. Diese Vorschau hat noch "
                           "keine registrierte Microsoft-Anmeldung; dieses Konto kann derzeit nicht "
                           "verbunden werden."),
            QStringLiteral("Microsoft requires modern authentication over OAuth. This preview has no "
                           "registered Microsoft sign-in yet, so this account cannot be connected.")
        );
    } else {
        result.account.label = L(QStringLiteral("Mein Postfach"), QStringLiteral("My mailbox"));
    }

    result.source = L(QStringLiteral("Anbietervorlage: "), QStringLiteral("Provider template: ")) + name;
    return result;
}

QString MailDiscovery::domainOf(const QString &address) {
    const QStringList parts = address.trimmed().split(QLatin1Char('@'));
    if (parts.size() != 2 || parts.at(0).isEmpty()) return {};
    const QString domain = parts.at(1).toLower();
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\\.[a-z]{2,}$"));
    if (!pattern.match(domain).hasMatch()) return {};
    return domain;
}

QStringList MailDiscovery::autoconfigEndpoints(const QString &domain) {
    return {
        QStringLiteral("https://autoconfig.") + domain + QStringLiteral("/mail/config-v1.1.xml"),
        QStringLiteral("https://") + domain + QStringLiteral("/.well-known/autoconfig/mail/config-v1.1.xml"),
        QStringLiteral("https://autoconfig.thunderbird.net/v1.1/") + domain,
    };
}

MailDiscoveryResult MailDiscovery::parseAutoconfig(
    const QByteArray &xml,
    const QString &address,
    const QString &id,
    const QString &source
) {
    MailDiscoveryResult result;
    result.problem = L(QStringLiteral("Keine sichere automatische Konfiguration gefunden."),
                       QStringLiteral("No secure automatic configuration was found."));
    if (xml.size() > maximumDocumentBytes) return result;
    const QString domain = domainOf(address);
    if (domain.isEmpty()) return result;
    const QString local = address.trimmed().split(QLatin1Char('@')).at(0);

    // Each server element is collected as flat key-value pairs, which is all the
    // format needs and keeps the reader simple.
    std::vector<QMap<QString, QString>> servers;
    QXmlStreamReader reader(xml);
    QMap<QString, QString> current;
    bool inServer = false;
    QString element;
    while (!reader.atEnd()) {
        const QXmlStreamReader::TokenType token = reader.readNext();
        if (token == QXmlStreamReader::DTD) {
            // A document type declaration is where entity tricks live.
            return result;
        }
        if (token == QXmlStreamReader::StartElement) {
            element = reader.name().toString();
            if (element == QStringLiteral("incomingServer") || element == QStringLiteral("outgoingServer")) {
                inServer = true;
                current.clear();
                current.insert(QStringLiteral("type"),
                               reader.attributes().value(QStringLiteral("type")).toString());
            }
        } else if (token == QXmlStreamReader::Characters && inServer && !reader.isWhitespace()) {
            const QString text = reader.text().toString().trimmed();
            if (element == QStringLiteral("authentication")) {
                current.insert(QStringLiteral("auth"),
                               current.value(QStringLiteral("auth")) + text + QLatin1Char(','));
            } else if (!element.isEmpty()) {
                current.insert(element, text);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QString ended = reader.name().toString();
            if (ended == QStringLiteral("incomingServer") || ended == QStringLiteral("outgoingServer")) {
                servers.push_back(current);
                inServer = false;
            }
            element.clear();
        }
    }
    if (reader.hasError()) return result;

    const QMap<QString, QString> *incoming = nullptr;
    const QMap<QString, QString> *outgoing = nullptr;
    for (const auto &server : servers) {
        const QString type = server.value(QStringLiteral("type"));
        const QString socket = server.value(QStringLiteral("socketType"));
        // Only encrypted transports are accepted.
        if (!incoming && type == QStringLiteral("imap") && socket == QStringLiteral("SSL"))
            incoming = &server;
        if (!outgoing && type == QStringLiteral("smtp")
            && (socket == QStringLiteral("SSL") || socket == QStringLiteral("STARTTLS")))
            outgoing = &server;
    }
    if (!incoming || !outgoing) return result;

    bool imapPortOk = false;
    bool smtpPortOk = false;
    const int imapPort = incoming->value(QStringLiteral("port")).toInt(&imapPortOk);
    const int smtpPort = outgoing->value(QStringLiteral("port")).toInt(&smtpPortOk);
    if (!imapPortOk || !smtpPortOk) return result;

    MailAccount account;
    account.id = id;
    account.label = domain;
    account.address = address.trimmed();
    account.username = expand(incoming->value(QStringLiteral("username")), account.address, local, domain);
    account.imapHost = incoming->value(QStringLiteral("hostname"));
    account.imapPort = imapPort;
    account.smtpHost = outgoing->value(QStringLiteral("hostname"));
    account.smtpPort = smtpPort;
    account.smtpSecurity = outgoing->value(QStringLiteral("socketType")) == QStringLiteral("SSL")
        ? QStringLiteral("tls")
        : QStringLiteral("starttls");
    account.smtpUsername = expand(outgoing->value(QStringLiteral("username")), account.address, local, domain);
    if (!account.validationProblem().isEmpty()) return result;

    const bool oauthOnly = onlyOAuth(incoming->value(QStringLiteral("auth")))
        || onlyOAuth(outgoing->value(QStringLiteral("auth")));
    result.problem.clear();
    result.account = account;
    result.oauthOnly = oauthOnly;
    result.source = source;
    result.explanation = oauthOnly
        ? L(QStringLiteral("Anbieter meldet ausschließlich OAuth. Diese Anmeldung ist noch nicht eingerichtet."),
            QStringLiteral("The provider reports OAuth only. That sign-in is not set up yet."))
        : L(QStringLiteral("Serverdaten gefunden. Bitte vor dem Verbinden prüfen. Bei aktivierter "
                           "2-Faktor-Anmeldung kann ein App-Passwort nötig sein."),
            QStringLiteral("Server settings found. Please check them before connecting. With "
                           "two-factor sign-in enabled an app password may be needed."));
    return result;
}

void MailDiscovery::discover(
    const QString &address,
    const QString &id,
    QObject *context,
    std::function<void(MailDiscoveryResult)> done
) {
    const QString domain = domainOf(address);
    if (domain.isEmpty()) {
        MailDiscoveryResult refusal;
        refusal.problem = L(QStringLiteral("Bitte eine gültige E-Mail-Adresse eingeben."),
                            QStringLiteral("Please enter a valid email address."));
        done(refusal);
        return;
    }
    // The big providers are answered from the templates instead of asking the
    // network about something already known.
    if (domain == QStringLiteral("gmail.com") || domain == QStringLiteral("googlemail.com")) {
        done(preset(kGmail, address, id));
        return;
    }
    if (domain == QStringLiteral("icloud.com") || domain == QStringLiteral("me.com")
        || domain == QStringLiteral("mac.com")) {
        done(preset(kIcloud, address, id));
        return;
    }
    if (domain == QStringLiteral("outlook.com") || domain == QStringLiteral("hotmail.com")
        || domain == QStringLiteral("live.com") || domain == QStringLiteral("outlook.de")) {
        done(preset(kOutlook, address, id));
        return;
    }

    auto network = std::make_shared<QNetworkAccessManager>(context);
    // A redirect could send the lookup to a host that was never asked about, so
    // redirects are refused outright.
    network->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    auto endpoints = std::make_shared<QStringList>(autoconfigEndpoints(domain));
    auto step = std::make_shared<std::function<void()>>();
    *step = [network, endpoints, address, id, done, step]() {
        if (endpoints->isEmpty()) {
            MailDiscoveryResult refusal;
            refusal.problem = L(
                QStringLiteral("Keine sichere automatische Konfiguration gefunden. Wähle deinen "
                               "Anbieter oder trage die IMAP-/SMTP-Daten manuell ein."),
                QStringLiteral("No secure automatic configuration was found. Choose your provider or "
                               "enter the IMAP and SMTP settings yourself.")
            );
            done(refusal);
            step->operator=(nullptr);
            return;
        }
        const QString endpoint = endpoints->takeFirst();
        QNetworkRequest request{QUrl(endpoint)};
        request.setTransferTimeout(6'000);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        QNetworkReply *reply = network->get(request);
        QObject::connect(reply, &QNetworkReply::finished, reply,
                         [reply, endpoint, address, id, done, step]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QUrl url = reply->url();
            const QByteArray payload = reply->readAll();
            reply->deleteLater();
            if (status != 200 || url.scheme() != QStringLiteral("https")
                || payload.size() > maximumDocumentBytes) {
                (*step)();
                return;
            }
            const MailDiscoveryResult result = parseAutoconfig(
                payload, address, id, url.host().isEmpty() ? endpoint : url.host());
            if (!result.problem.isEmpty()) {
                (*step)();
                return;
            }
            done(result);
            step->operator=(nullptr);
        });
    };
    (*step)();
}

} // namespace yobro::spike
