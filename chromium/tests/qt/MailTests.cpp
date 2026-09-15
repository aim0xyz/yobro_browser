#include "spike/MailDiscovery.hpp"
#include "spike/MailStore.hpp"
#include "spike/MailWorker.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using yobro::spike::MailAccount;
using yobro::spike::MailAttachment;
using yobro::spike::MailBody;
using yobro::spike::MailDiscovery;
using yobro::spike::MailDiscoveryResult;
using yobro::spike::MailFolder;
using yobro::spike::MailMessage;
using yobro::spike::MailSecrets;
using yobro::spike::MailStore;
using yobro::spike::MailWorker;

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }
void check(bool condition, std::string_view message) { if (!condition) fail(std::string(message)); }

MailAccount workingAccount(const QString &id = QStringLiteral("acc-1")) {
    MailAccount account;
    account.id = id;
    account.label = QStringLiteral("Arbeit");
    account.address = QStringLiteral("ich@example.com");
    account.username = QStringLiteral("ich@example.com");
    account.imapHost = QStringLiteral("imap.example.com");
    account.smtpHost = QStringLiteral("smtp.example.com");
    return account;
}

// MARK: - The shared helper script

/// The IMAP and SMTP helper must be byte for byte the one the WebKit build uses,
/// otherwise the two shells would talk to a provider differently.
void checkSharedScript() {
    QFile original(QStringLiteral(YOBRO_WEBKIT_MAIL_SCRIPT));
    check(original.open(QIODevice::ReadOnly), "The WebKit MailWorker.py could not be read.");
    const QByteArray reference = QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256);

    QFile shipped(QStringLiteral(":/yobro/MailWorker.py"));
    check(shipped.open(QIODevice::ReadOnly), "The bundled MailWorker.py is missing.");
    const QByteArray shippedHash = QCryptographicHash::hash(shipped.readAll(), QCryptographicHash::Sha256);
    check(reference == shippedHash,
          "The bundled mail helper differs from the WebKit original: "
              + QString::fromLatin1(shippedHash.toHex()).toStdString() + " vs "
              + QString::fromLatin1(reference.toHex()).toStdString());
}

void checkWorkerRefusals() {
    if (!MailWorker::available()) return;
    check(MailWorker::interpreter().startsWith(QLatin1Char('/')),
          "The interpreter is not addressed by an absolute path.");
    // An unknown action must come back as a reported problem, never as success.
    QJsonObject request;
    request.insert(QStringLiteral("action"), QStringLiteral("does-not-exist"));
    request.insert(QStringLiteral("password"), QStringLiteral("x"));
    request.insert(QStringLiteral("account"), workingAccount().toJson());
    const MailWorker::Result result = MailWorker::call(request, 30'000);
    check(!result.problem.isEmpty(), "An unknown mail action was accepted.");
}

// MARK: - Value types

void checkAccountValidation() {
    check(workingAccount().validationProblem().isEmpty(), "A usable account was refused.");

    const auto refused = [](const std::function<void(MailAccount &)> &change, const char *what) {
        MailAccount account = workingAccount();
        change(account);
        check(!account.validationProblem().isEmpty(), std::string("An unusable account was accepted: ") + what);
    };
    refused([](MailAccount &a) { a.id.clear(); }, "no id");
    refused([](MailAccount &a) { a.address = QStringLiteral("nope"); }, "address without @");
    refused([](MailAccount &a) { a.address = QStringLiteral("@example.com"); }, "address without a local part");
    refused([](MailAccount &a) { a.address = QStringLiteral("ich@"); }, "address without a domain");
    refused([](MailAccount &a) { a.username = QStringLiteral("  "); }, "blank user name");
    refused([](MailAccount &a) { a.imapHost = QStringLiteral("imaps://imap.example.com"); }, "host with a scheme");
    refused([](MailAccount &a) { a.imapHost = QStringLiteral("imap.example.com/path"); }, "host with a path");
    refused([](MailAccount &a) { a.smtpHost = QStringLiteral("smtp example com"); }, "host with a space");
    refused([](MailAccount &a) { a.smtpHost.clear(); }, "empty host");
    refused([](MailAccount &a) { a.imapPort = 0; }, "port zero");
    refused([](MailAccount &a) { a.smtpPort = 70000; }, "port past the range");
    // Sending without encryption would put the password on the wire.
    refused([](MailAccount &a) { a.smtpSecurity = QStringLiteral("none"); }, "unencrypted sending");
    refused([](MailAccount &a) { a.smtpSecurity = QStringLiteral("plain"); }, "unencrypted sending");

    MailAccount unnamed = workingAccount();
    unnamed.label.clear();
    check(unnamed.displayLabel() == unnamed.address, "An account without a label shows no address.");

    // The password must not have a place in the persisted form.
    const QJsonObject json = workingAccount().toJson();
    check(!json.contains(QStringLiteral("password")), "The account form has a password field.");
    const MailAccount restored = MailAccount::fromJson(json);
    check(restored.id == QStringLiteral("acc-1") && restored.imapPort == 993
              && restored.smtpSecurity == QStringLiteral("tls"),
          "An account did not survive the round trip.");
    // A file from an older version must come back with safe defaults.
    const MailAccount sparse = MailAccount::fromJson(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("x")},
        {QStringLiteral("address"), QStringLiteral("a@b.de")},
    });
    check(sparse.imapPort == 993 && sparse.smtpPort == 465
              && sparse.smtpSecurity == QStringLiteral("tls"),
          "A sparse account did not fall back to encrypted defaults.");
}

void checkFolders() {
    const MailFolder inbox = MailFolder::fromJson(QJsonObject{
        {QStringLiteral("path"), QStringLiteral("INBOX")},
        {QStringLiteral("title"), QStringLiteral("Posteingang")},
        {QStringLiteral("selectable"), true},
        {QStringLiteral("unread"), 4},
        {QStringLiteral("role"), QStringLiteral("inbox")},
    });
    check(inbox.unread == 4 && inbox.selectable, "The inbox was not read.");
    check(!inbox.displayTitle().isEmpty() && !inbox.isTrash(), "The inbox looks like a Trash folder.");

    // Trash is recognised by the provider's role and, failing that, by name.
    check(MailFolder::fromJson(QJsonObject{{QStringLiteral("role"), QStringLiteral("trash")}}).isTrash(),
          "A folder marked as Trash was not recognised.");
    for (const QString &title : {QStringLiteral("Trash"), QStringLiteral("Papierkorb"),
                                 QStringLiteral("Deleted Items"), QStringLiteral("Gelöschte Elemente")}) {
        check(MailFolder::fromJson(QJsonObject{{QStringLiteral("title"), title}}).isTrash(),
              "A Trash folder was not recognised by name: " + title.toStdString());
    }
    check(!MailFolder::fromJson(QJsonObject{{QStringLiteral("title"), QStringLiteral("Archiv")}}).isTrash(),
          "An archive was mistaken for Trash.");
    // A folder the provider cannot select must not look selectable.
    check(!MailFolder::fromJson(QJsonObject{{QStringLiteral("selectable"), false}}).selectable,
          "An unselectable folder looks selectable.");
}

void checkMessageIdentity() {
    QJsonObject row;
    row.insert(QStringLiteral("uid"), QStringLiteral("42"));
    row.insert(QStringLiteral("subject"), QStringLiteral("Rechnung"));
    row.insert(QStringLiteral("sender"), QStringLiteral("Buchhaltung <b@example.com>"));
    row.insert(QStringLiteral("replyTo"), QStringLiteral("antwort@example.com"));
    row.insert(QStringLiteral("date"), 1700000000.0);
    row.insert(QStringLiteral("unread"), true);
    row.insert(QStringLiteral("messageID"), QStringLiteral("<abc@example.com>"));
    const MailMessage message = MailMessage::fromJson(
        row, QStringLiteral("acc-1"), QStringLiteral("INBOX"), QStringLiteral("v1"));
    check(message.uid == QStringLiteral("42") && message.unread && message.date == 1700000000,
          "A message summary was not read.");
    check(message.id() == QStringLiteral("acc-1:INBOX:v1:42"), "The message id changed shape.");

    // A new mailbox generation makes every id different, which is the point.
    MailMessage reset = message;
    reset.validity = QStringLiteral("v2");
    check(reset.id() != message.id(), "A mailbox reset produced the same message id.");
}

void checkNewMailDetection() {
    const auto make = [](const QString &uid, bool unread) {
        MailMessage message;
        message.accountId = QStringLiteral("acc-1");
        message.folder = QStringLiteral("INBOX");
        message.validity = QStringLiteral("v1");
        message.uid = uid;
        message.unread = unread;
        return message;
    };
    const std::vector<MailMessage> loaded{
        make(QStringLiteral("10"), false),
        make(QStringLiteral("11"), true),
        make(QStringLiteral("12"), true),
    };

    // A mailbox nobody has looked at yet announces what is unread.
    check(MailStore::newInboxMessages(loaded, {}).size() == 2,
          "A first look did not announce the unread mail.");
    // Afterwards only mail above the highest known uid counts.
    check(MailStore::newInboxMessages(loaded, {QStringLiteral("acc-1:INBOX:v1:11")}).size() == 1,
          "A refresh announced mail that was already known.");
    check(MailStore::newInboxMessages(loaded, {QStringLiteral("acc-1:INBOX:v1:12")}).empty(),
          "A refresh announced the same mail twice.");
    // A mailbox reset must not announce every message as new.
    check(MailStore::newInboxMessages(loaded, {QStringLiteral("acc-1:INBOX:v0:99")}).empty(),
          "A mailbox reset announced the whole inbox.");
    check(MailStore::newInboxMessages({}, {}).empty(), "An empty mailbox announced something.");
    // Mail that is already read is never announced.
    check(MailStore::newInboxMessages({make(QStringLiteral("20"), false)},
                                      {QStringLiteral("acc-1:INBOX:v1:1")}).empty(),
          "Mail that was already read was announced.");
}

// MARK: - Discovery

void checkPresets() {
    check(MailDiscovery::presetNames().size() == 4, "There are not four provider templates.");

    const MailDiscoveryResult gmail =
        MailDiscovery::preset(QStringLiteral("Gmail"), QStringLiteral("a@gmail.com"), QStringLiteral("x"));
    check(gmail.account.imapHost == QStringLiteral("imap.gmail.com")
              && gmail.account.smtpHost == QStringLiteral("smtp.gmail.com")
              && gmail.account.imapPort == 993 && gmail.account.smtpPort == 465,
          "The Gmail template changed.");
    check(!gmail.oauthOnly, "Gmail was marked as OAuth only.");

    const MailDiscoveryResult icloud =
        MailDiscovery::preset(QStringLiteral("iCloud Mail"), QStringLiteral("a@icloud.com"), QStringLiteral("x"));
    check(icloud.account.smtpPort == 587 && icloud.account.smtpSecurity == QStringLiteral("starttls"),
          "The iCloud template changed.");

    const MailDiscoveryResult outlook = MailDiscovery::preset(
        QStringLiteral("Outlook / Microsoft 365"), QStringLiteral("a@outlook.com"), QStringLiteral("x"));
    // Microsoft cannot be connected with a password, and the form has to say so.
    check(outlook.oauthOnly, "Outlook was not marked as OAuth only.");
    check(!outlook.explanation.isEmpty(), "Outlook has no explanation.");

    for (const QString &name : MailDiscovery::presetNames()) {
        const MailDiscoveryResult result =
            MailDiscovery::preset(name, QStringLiteral("a@example.com"), QStringLiteral("x"));
        check(result.account.validationProblem().isEmpty(),
              "A provider template is not usable: " + name.toStdString());
        check(result.account.smtpSecurity == QStringLiteral("tls")
                  || result.account.smtpSecurity == QStringLiteral("starttls"),
              "A provider template sends without encryption: " + name.toStdString());
        check(result.source.contains(name), "A template does not say where it came from.");
    }
}

void checkDomainParsing() {
    check(MailDiscovery::domainOf(QStringLiteral("ich@example.com")) == QStringLiteral("example.com"),
          "A plain domain was not read.");
    check(MailDiscovery::domainOf(QStringLiteral("Ich@Example.COM")) == QStringLiteral("example.com"),
          "The domain was not lower-cased.");
    for (const QString &address : {
             QString(),
             QStringLiteral("ich"),
             QStringLiteral("ich@"),
             QStringLiteral("@example.com"),
             QStringLiteral("ich@example"),
             QStringLiteral("ich@.com"),
             QStringLiteral("ich@example..com"),
             QStringLiteral("ich@a@b.com"),
             QStringLiteral("ich@exa mple.com"),
         }) {
        check(MailDiscovery::domainOf(address).isEmpty(),
              "An unusable address was accepted: " + address.toStdString());
    }
}

void checkAutoconfigEndpoints() {
    const QStringList endpoints = MailDiscovery::autoconfigEndpoints(QStringLiteral("example.com"));
    check(endpoints.size() == 3, "There are not three autoconfig endpoints.");
    for (const QString &endpoint : endpoints) {
        // A plaintext lookup would let anyone choose where the password goes.
        check(endpoint.startsWith(QStringLiteral("https://")),
              "An autoconfig endpoint is not HTTPS: " + endpoint.toStdString());
    }
    check(endpoints.at(0).contains(QStringLiteral("autoconfig.example.com")),
          "The provider's own endpoint is not tried first.");
    check(endpoints.at(2).contains(QStringLiteral("autoconfig.thunderbird.net")),
          "The shared database is not the last resort.");
}

QByteArray autoconfigDocument(
    const QString &imapSocket,
    const QString &smtpSocket,
    const QString &imapAuth,
    const QString &smtpAuth,
    const QString &imapPort = QStringLiteral("993")
) {
    return QStringLiteral(R"(<?xml version="1.0"?>
<clientConfig version="1.1">
  <emailProvider id="example.com">
    <incomingServer type="imap">
      <hostname>imap.example.com</hostname>
      <port>%1</port>
      <socketType>%2</socketType>
      <username>%EMAILADDRESS%</username>
      <authentication>%3</authentication>
    </incomingServer>
    <outgoingServer type="smtp">
      <hostname>smtp.example.com</hostname>
      <port>587</port>
      <socketType>%4</socketType>
      <username>%EMAILLOCALPART%</username>
      <authentication>%5</authentication>
    </outgoingServer>
  </emailProvider>
</clientConfig>)").arg(imapPort, imapSocket, imapAuth, smtpSocket, smtpAuth).toUtf8();
}

void checkAutoconfigParsing() {
    const MailDiscoveryResult accepted = MailDiscovery::parseAutoconfig(
        autoconfigDocument(QStringLiteral("SSL"), QStringLiteral("STARTTLS"),
                           QStringLiteral("password-cleartext"), QStringLiteral("password-cleartext")),
        QStringLiteral("ich@example.com"), QStringLiteral("acc-1"), QStringLiteral("autoconfig.example.com"));
    check(accepted.problem.isEmpty(), "A valid autoconfig document was refused: "
              + accepted.problem.toStdString());
    check(accepted.account.imapHost == QStringLiteral("imap.example.com")
              && accepted.account.imapPort == 993
              && accepted.account.smtpHost == QStringLiteral("smtp.example.com")
              && accepted.account.smtpPort == 587
              && accepted.account.smtpSecurity == QStringLiteral("starttls"),
          "The server settings were not read.");
    // The placeholders the format uses have to be filled in.
    check(accepted.account.username == QStringLiteral("ich@example.com"),
          "The IMAP user name placeholder was not expanded: " + accepted.account.username.toStdString());
    check(accepted.account.smtpUsername == QStringLiteral("ich"),
          "The SMTP local-part placeholder was not expanded: " + accepted.account.smtpUsername.toStdString());
    check(!accepted.oauthOnly, "A password provider was marked as OAuth only.");
    check(accepted.source == QStringLiteral("autoconfig.example.com"),
          "The source was not carried through.");

    // A provider that only offers OAuth has to be flagged, not silently used.
    const MailDiscoveryResult oauth = MailDiscovery::parseAutoconfig(
        autoconfigDocument(QStringLiteral("SSL"), QStringLiteral("SSL"),
                           QStringLiteral("OAuth2"), QStringLiteral("OAuth2")),
        QStringLiteral("ich@example.com"), QStringLiteral("acc-1"), QStringLiteral("x"));
    check(oauth.problem.isEmpty() && oauth.oauthOnly, "An OAuth-only provider was not flagged.");

    // An unencrypted server is ignored rather than accepted.
    for (const auto &sockets : QList<QPair<QString, QString>>{
             {QStringLiteral("plain"), QStringLiteral("SSL")},
             {QStringLiteral("SSL"), QStringLiteral("plain")},
             {QStringLiteral("plain"), QStringLiteral("plain")},
         }) {
        const MailDiscoveryResult refused = MailDiscovery::parseAutoconfig(
            autoconfigDocument(sockets.first, sockets.second, QStringLiteral("password-cleartext"),
                               QStringLiteral("password-cleartext")),
            QStringLiteral("ich@example.com"), QStringLiteral("acc-1"), QStringLiteral("x"));
        check(!refused.problem.isEmpty(),
              "An unencrypted server was accepted: " + sockets.first.toStdString() + "/"
                  + sockets.second.toStdString());
    }

    // A port that is not a number is not a port.
    check(!MailDiscovery::parseAutoconfig(
              autoconfigDocument(QStringLiteral("SSL"), QStringLiteral("SSL"),
                                 QStringLiteral("password-cleartext"),
                                 QStringLiteral("password-cleartext"), QStringLiteral("secure")),
              QStringLiteral("ich@example.com"), QStringLiteral("acc-1"), QStringLiteral("x"))
              .problem.isEmpty(),
          "A document without a numeric port was accepted.");

    // A document type declaration is where entity expansion attacks live.
    const QByteArray withDtd = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><!DOCTYPE clientConfig [<!ENTITY x SYSTEM \"file:///etc/passwd\">]>"
        "<clientConfig><emailProvider><incomingServer type=\"imap\">"
        "<hostname>imap.example.com</hostname><port>993</port><socketType>SSL</socketType>"
        "</incomingServer></emailProvider></clientConfig>");
    check(!MailDiscovery::parseAutoconfig(withDtd, QStringLiteral("ich@example.com"),
                                          QStringLiteral("acc-1"), QStringLiteral("x"))
              .problem.isEmpty(),
          "A document with a DTD was accepted.");

    for (const QByteArray &payload : {
             QByteArray(),
             QByteArrayLiteral("not xml at all"),
             QByteArrayLiteral("<clientConfig><emailProvider/></clientConfig>"),
             QByteArray(MailDiscovery::maximumDocumentBytes + 1, 'x'),
         }) {
        check(!MailDiscovery::parseAutoconfig(payload, QStringLiteral("ich@example.com"),
                                              QStringLiteral("acc-1"), QStringLiteral("x"))
                  .problem.isEmpty(),
              "An unusable autoconfig document was accepted.");
    }

    // Without a usable address there is nothing to fill the placeholders with.
    check(!MailDiscovery::parseAutoconfig(
              autoconfigDocument(QStringLiteral("SSL"), QStringLiteral("SSL"),
                                 QStringLiteral("password-cleartext"), QStringLiteral("password-cleartext")),
              QStringLiteral("not-an-address"), QStringLiteral("acc-1"), QStringLiteral("x"))
              .problem.isEmpty(),
          "A document was accepted for an invalid address.");
}

// MARK: - Store

/// Answers store requests from a script instead of a mail server.
class FakeServer {
public:
    struct Call {
        QString action;
        QString accountId;
        QJsonObject extra;
    };

    QList<Call> calls;
    /// Per action: the answer, or a problem when the string is not empty.
    QHash<QString, QJsonObject> answers;
    QHash<QString, QString> problems;
    bool verifierAccepts = true;
    QString verifierProblem = QStringLiteral("Anmeldung fehlgeschlagen.");

    MailStore::Transport transport() {
        return [this](const QString &action, const MailAccount &account, const QJsonObject &extra,
                      std::function<void(QString, QJsonObject)> done) {
            calls.append({action, account.id, extra});
            done(problems.value(action), answers.value(action));
        };
    }

    MailStore::Verifier verifier() {
        return [this](const MailAccount &, const QString &, std::function<void(QString)> done) {
            done(verifierAccepts ? QString() : verifierProblem);
        };
    }

    [[nodiscard]] QStringList actions() const {
        QStringList names;
        for (const Call &call : calls) names.append(call.action);
        return names;
    }

    [[nodiscard]] QJsonObject lastExtra(const QString &action) const {
        QJsonObject extra;
        for (const Call &call : calls)
            if (call.action == action) extra = call.extra;
        return extra;
    }
};

QJsonObject folderAnswer() {
    QJsonArray folders;
    folders.append(QJsonObject{
        {QStringLiteral("path"), QStringLiteral("INBOX")},
        {QStringLiteral("title"), QStringLiteral("Posteingang")},
        {QStringLiteral("selectable"), true},
        {QStringLiteral("unread"), 2},
        {QStringLiteral("role"), QStringLiteral("inbox")},
    });
    folders.append(QJsonObject{
        {QStringLiteral("path"), QStringLiteral("Trash")},
        {QStringLiteral("title"), QStringLiteral("Trash")},
        {QStringLiteral("selectable"), true},
        {QStringLiteral("unread"), 0},
        {QStringLiteral("role"), QStringLiteral("trash")},
    });
    return QJsonObject{{QStringLiteral("folders"), folders}};
}

QJsonObject listAnswer(const QString &validity = QStringLiteral("v1")) {
    QJsonArray messages;
    for (const auto &row : QList<QPair<QString, bool>>{
             {QStringLiteral("10"), false},
             {QStringLiteral("11"), true},
         }) {
        messages.append(QJsonObject{
            {QStringLiteral("uid"), row.first},
            {QStringLiteral("subject"), QStringLiteral("Betreff ") + row.first},
            {QStringLiteral("sender"), QStringLiteral("Absender ") + row.first},
            {QStringLiteral("to"), QStringLiteral("ich@example.com")},
            {QStringLiteral("replyTo"), QStringLiteral("antwort@example.com")},
            {QStringLiteral("date"), 1700000000.0 + row.first.toDouble()},
            {QStringLiteral("unread"), row.second},
            {QStringLiteral("messageID"), QStringLiteral("<") + row.first + QStringLiteral("@example.com>")},
        });
    }
    return QJsonObject{
        {QStringLiteral("validity"), validity},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("total"), 57},
    };
}

/// A store with one connected account and a loaded inbox.
struct Fixture {
    FakeServer server;
    std::unique_ptr<MailStore> store;
    QString accountId = QStringLiteral("acc-1");

    explicit Fixture(const QString &directory) {
        check(QDir().mkpath(directory), "Could not create a mail store directory.");
        store = std::make_unique<MailStore>(directory.toStdString());
        store->setTransport(server.transport());
        store->setVerifier(server.verifier());
        server.answers.insert(QStringLiteral("folders"), folderAnswer());
        server.answers.insert(QStringLiteral("list"), listAnswer());

        QString reported = QStringLiteral("not called");
        store->connectAccount(workingAccount(accountId), QStringLiteral("app-password"),
                              [&reported](QString problem) { reported = problem; });
        check(reported.isEmpty(), "The fixture account could not be connected: " + reported.toStdString());
        store->refresh([](QString) {});
        server.calls.clear();
    }
};

void checkConnectAndDisconnect(const QString &root) {
    const QString directory = root + QStringLiteral("/connect");
    check(QDir().mkpath(directory), "Could not create the connect directory.");
    FakeServer server;
    MailStore store(directory.toStdString());
    store.setTransport(server.transport());
    store.setVerifier(server.verifier());

    // An unusable account is refused before anything reaches the server.
    MailAccount broken = workingAccount();
    broken.imapHost = QStringLiteral("imaps://imap.example.com");
    QString reported;
    store.connectAccount(broken, QStringLiteral("pw"), [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "An unusable account was connected.");
    check(store.accounts().empty(), "A refused account was stored anyway.");

    // So is a missing password.
    store.connectAccount(workingAccount(), QString(), [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "An account without a password was connected.");

    // A server that refuses the login must leave nothing behind.
    server.verifierAccepts = false;
    store.connectAccount(workingAccount(), QStringLiteral("wrong"), [&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A refused login was reported as success.");
    check(store.accounts().empty(), "A refused login stored the account.");
    check(!QFile::exists(directory + QStringLiteral("/mail-accounts.json")),
          "A refused login wrote the account list.");

    server.verifierAccepts = true;
    store.connectAccount(workingAccount(), QStringLiteral("app-password"),
                         [&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "A working account was refused: " + reported.toStdString());
    check(store.accounts().size() == 1, "The account was not stored.");
    check(!store.busy(), "The store stayed busy after connecting.");

    // Connecting the same account again replaces it instead of duplicating it.
    MailAccount renamed = workingAccount();
    renamed.label = QStringLiteral("Privat");
    store.connectAccount(renamed, QStringLiteral("app-password"), [&reported](QString p) { reported = p; });
    check(store.accounts().size() == 1, "The same account was stored twice.");
    check(store.accounts().front().label == QStringLiteral("Privat"), "The account was not updated.");

    // The password belongs in the Keychain, never in the JSON file.
    QFile file(directory + QStringLiteral("/mail-accounts.json"));
    check(file.open(QIODevice::ReadOnly), "The account list was not written.");
    const QString stored = QString::fromUtf8(file.readAll());
    check(!stored.contains(QStringLiteral("app-password")),
          "The mailbox password was written into the account list.");
    file.close();

    check(!store.disconnectAccount(QStringLiteral("does-not-exist")).isEmpty(),
          "Disconnecting an unknown account was reported as success.");
    check(store.disconnectAccount(workingAccount().id).isEmpty(), "The account could not be disconnected.");
    check(store.accounts().empty(), "The account survived disconnecting.");

    // A restarted store sees exactly what was persisted.
    MailStore reopened(directory.toStdString());
    check(reopened.accounts().empty(), "Disconnecting did not reach the file.");
}

void checkDamagedAccountList(const QString &root) {
    const QString directory = root + QStringLiteral("/damaged");
    check(QDir().mkpath(directory), "Could not create the damaged directory.");
    const QByteArray garbage = QByteArrayLiteral("[ this is not json");
    {
        QFile file(directory + QStringLiteral("/mail-accounts.json"));
        check(file.open(QIODevice::WriteOnly), "Could not write the damaged file.");
        file.write(garbage);
    }
    FakeServer server;
    {
        MailStore store(directory.toStdString());
        store.setTransport(server.transport());
        store.setVerifier(server.verifier());
        check(store.loadFailed(), "A damaged account list was not detected.");
        check(!store.notice().isEmpty(), "A damaged account list was not reported.");
        QString reported;
        store.connectAccount(workingAccount(), QStringLiteral("pw"), [&reported](QString p) { reported = p; });
        check(!reported.isEmpty(), "An account was added on top of a damaged list.");
    }
    QFile file(directory + QStringLiteral("/mail-accounts.json"));
    check(file.open(QIODevice::ReadOnly), "The damaged file disappeared.");
    check(file.readAll() == garbage, "The damaged account list was overwritten.");
}

void checkRefreshAndFilters(const QString &root) {
    Fixture fixture(root + QStringLiteral("/refresh"));
    MailStore &store = *fixture.store;

    check(store.folders(fixture.accountId).size() == 2, "The folders were not stored.");
    check(store.messages(fixture.accountId, QStringLiteral("INBOX")).size() == 2,
          "The inbox was not stored.");
    check(store.folderTotal(fixture.accountId, QStringLiteral("INBOX")) == 57,
          "The message count reported by the provider was lost.");
    check(store.hasUnread(), "The unread state was not seen.");
    // Newest first, as in the WebKit build.
    check(store.messages(fixture.accountId, QStringLiteral("INBOX")).front().uid == QStringLiteral("11"),
          "The message list is not sorted newest first.");

    // Without a chosen account the inboxes are shown together.
    check(store.visibleMessages().size() == 2, "The combined inbox is empty.");

    store.setSearch(QStringLiteral("betreff 10"));
    check(store.visibleMessages().size() == 1, "The search does not filter.");
    store.setSearch(QStringLiteral("ABSENDER 11"));
    check(store.visibleMessages().size() == 1, "The search does not look at the sender.");
    store.setSearch(QStringLiteral("gibt es nicht"));
    check(store.visibleMessages().empty(), "The search matched something it should not.");
    store.setSearch({});

    store.setUnreadOnly(true);
    check(store.visibleMessages().size() == 1, "The unread filter does not work.");
    // The message being read stays visible even once it counts as read.
    store.setSelectedMessage(QStringLiteral("acc-1:INBOX:v1:10"));
    check(store.visibleMessages().size() == 2, "The open message vanished from the filtered list.");
    store.setUnreadOnly(false);
    store.setSelectedMessage({});

    // A folder switch loads that folder and forgets the selection.
    fixture.server.answers.insert(QStringLiteral("list"), listAnswer());
    store.selectFolder(fixture.accountId, QStringLiteral("Trash"), [](QString) {});
    check(store.selectedFolder() == QStringLiteral("Trash"), "The folder was not switched.");
    check(fixture.server.lastExtra(QStringLiteral("list")).value(QStringLiteral("folder")).toString()
              == QStringLiteral("Trash"),
          "The folder was not requested from the provider.");
    check(fixture.server.lastExtra(QStringLiteral("list")).value(QStringLiteral("limit")).toInt() == 100,
          "The first page is not 100 messages.");

    store.loadMore([](QString) {});
    check(fixture.server.lastExtra(QStringLiteral("list")).value(QStringLiteral("limit")).toInt() == 200,
          "Loading more did not raise the limit.");
    for (int index = 0; index < 60; ++index) store.loadMore([](QString) {});
    check(fixture.server.lastExtra(QStringLiteral("list")).value(QStringLiteral("limit")).toInt() == 5000,
          "The limit went past what the helper accepts.");

    // A reported failure ends up on the account, not silently nowhere.
    fixture.server.problems.insert(QStringLiteral("list"), QStringLiteral("Postfach nicht erreichbar."));
    QString reported;
    store.selectFolder(fixture.accountId, QStringLiteral("INBOX"), [&reported](QString p) { reported = p; });
    check(reported == QStringLiteral("Postfach nicht erreichbar."), "A folder failure was not reported.");
    check(store.error(fixture.accountId) == QStringLiteral("Postfach nicht erreichbar."),
          "A folder failure was not kept for the account.");
    fixture.server.problems.remove(QStringLiteral("list"));
    store.selectFolder(fixture.accountId, QStringLiteral("INBOX"), [](QString) {});
    check(store.error(fixture.accountId).isEmpty(), "A resolved failure stayed on the account.");
}

void checkOpenMessage(const QString &root) {
    Fixture fixture(root + QStringLiteral("/open"));
    MailStore &store = *fixture.store;
    const QString unreadId = QStringLiteral("acc-1:INBOX:v1:11");

    QJsonArray attachments;
    attachments.append(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("2")},
        {QStringLiteral("name"), QStringLiteral("rechnung.pdf")},
        {QStringLiteral("mime"), QStringLiteral("application/pdf")},
        {QStringLiteral("size"), 1234},
    });
    fixture.server.answers.insert(QStringLiteral("body"), QJsonObject{
        {QStringLiteral("text"), QStringLiteral("Hallo Welt")},
        {QStringLiteral("html"), QStringLiteral("<p>Hallo Welt</p>")},
        {QStringLiteral("attachments"), attachments},
        {QStringLiteral("seen"), true},
    });

    const int unreadBefore = store.folders(fixture.accountId).front().unread;
    QString reported = QStringLiteral("not called");
    store.openMessage(unreadId, [&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "Opening a message failed: " + reported.toStdString());
    check(fixture.server.actions().contains(QStringLiteral("body")), "The body was not requested.");

    const auto body = store.body(unreadId);
    check(body.has_value(), "The body was not stored.");
    check(body->text == QStringLiteral("Hallo Welt") && body->html == QStringLiteral("<p>Hallo Welt</p>"),
          "The body content was lost.");
    check(body->attachments.size() == 1 && body->attachments.front().size == 1234,
          "The attachment list was lost.");
    check(store.selectedMessage() == unreadId, "The opened message was not selected.");
    check(!store.message(unreadId)->unread, "The message stayed unread after being read.");
    check(store.folders(fixture.accountId).front().unread == unreadBefore - 1,
          "The folder's unread count was not lowered.");

    // Opening again only marks it, it does not fetch the body twice.
    fixture.server.calls.clear();
    store.openMessage(unreadId, [](QString) {});
    check(!fixture.server.actions().contains(QStringLiteral("body")),
          "The body was fetched again although it was already there.");

    // A provider that cannot set the flag has to say so instead of pretending.
    fixture.server.answers.insert(QStringLiteral("body"), QJsonObject{
        {QStringLiteral("text"), QStringLiteral("Zweite")},
        {QStringLiteral("seen"), false},
        {QStringLiteral("warning"), QStringLiteral("Konnte nicht als gelesen markiert werden.")},
    });
    store.openMessage(QStringLiteral("acc-1:INBOX:v1:10"), [](QString) {});
    check(store.notice().contains(QStringLiteral("gelesen")), "The provider's warning was dropped.");

    // A refresh must not undo what was read locally.
    fixture.server.answers.insert(QStringLiteral("list"), listAnswer());
    store.refresh([](QString) {});
    check(!store.message(unreadId)->unread, "A refresh marked a read message as unread again.");

    check(!store.body(QStringLiteral("acc-1:INBOX:v1:999")).has_value(),
          "A body appeared for a message that does not exist.");
    QString missing;
    store.openMessage(QStringLiteral("acc-1:INBOX:v1:999"), [&missing](QString p) { missing = p; });
    check(!missing.isEmpty(), "Opening a message that does not exist was reported as success.");
}

void checkReplyAndSend(const QString &root) {
    Fixture fixture(root + QStringLiteral("/send"));
    MailStore &store = *fixture.store;

    const auto original = store.message(QStringLiteral("acc-1:INBOX:v1:11"));
    check(original.has_value(), "The fixture message is missing.");
    store.beginReply(*original);
    check(store.draft().accountId == fixture.accountId, "The reply has no sending account.");
    check(store.draft().to == QStringLiteral("antwort@example.com"),
          "The reply does not go to the Reply-To address.");
    check(store.draft().subject == QStringLiteral("Re: Betreff 11"), "The reply subject is wrong.");
    check(store.draft().replyToMessageId == original->messageId, "The reply is not threaded.");

    // A reply to a reply keeps one prefix, not two.
    MailMessage already = *original;
    already.subject = QStringLiteral("Re: Betreff 11");
    store.beginReply(already);
    check(store.draft().subject == QStringLiteral("Re: Betreff 11"), "The reply prefix was doubled.");
    // Without a Reply-To the sender is used.
    MailMessage noReplyTo = *original;
    noReplyTo.replyTo.clear();
    store.beginReply(noReplyTo);
    check(store.draft().to == noReplyTo.sender, "Without Reply-To the sender was not used.");

    // A draft without a recipient is not sent.
    store.draft().to.clear();
    QString reported;
    store.sendDraft([&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A draft without a recipient was sent.");
    check(!fixture.server.actions().contains(QStringLiteral("send")),
          "A refused draft still reached the server.");

    // A draft without a known account is not sent either.
    store.draft().to = QStringLiteral("du@example.com");
    store.draft().accountId = QStringLiteral("does-not-exist");
    store.sendDraft([&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A draft without a sending account was sent.");

    store.draft().accountId = fixture.accountId;
    store.draft().subject = QStringLiteral("Hallo");
    store.draft().text = QStringLiteral("Nur Text");
    fixture.server.answers.insert(QStringLiteral("send"), QJsonObject{});
    store.sendDraft([&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "A complete draft was refused: " + reported.toStdString());
    check(store.draft().isEmpty(), "A sent draft was not cleared.");
    // Either wording, so the test does not depend on the display language.
    check(store.notice().contains(QStringLiteral("versendet"))
              || store.notice().contains(QStringLiteral("was sent")),
          "Sending was not confirmed: " + store.notice().toStdString());
    const QJsonObject sent = fixture.server.lastExtra(QStringLiteral("send"));
    check(sent.value(QStringLiteral("to")).toString() == QStringLiteral("du@example.com")
              && sent.value(QStringLiteral("subject")).toString() == QStringLiteral("Hallo")
              && sent.value(QStringLiteral("text")).toString() == QStringLiteral("Nur Text"),
          "The draft was not passed on as written.");
    check(!sent.contains(QStringLiteral("html")),
          "A plain-text draft was sent with an HTML part.");

    // Recipients the server refused have to be named.
    store.draft().accountId = fixture.accountId;
    store.draft().to = QStringLiteral("gut@example.com, schlecht@example.com");
    fixture.server.answers.insert(QStringLiteral("send"), QJsonObject{
        {QStringLiteral("refused"), QJsonArray{QStringLiteral("schlecht@example.com")}},
    });
    store.sendDraft([](QString) {});
    check(store.notice().contains(QStringLiteral("schlecht@example.com")),
          "A refused recipient was not named.");

    // A failed send keeps the draft: nothing the user wrote may be lost.
    store.draft().accountId = fixture.accountId;
    store.draft().to = QStringLiteral("du@example.com");
    store.draft().text = QStringLiteral("Wichtiger Text");
    fixture.server.problems.insert(QStringLiteral("send"), QStringLiteral("Versandstatus unklar."));
    store.sendDraft([&reported](QString p) { reported = p; });
    check(!reported.isEmpty(), "A failed send was reported as success.");
    check(store.draft().text == QStringLiteral("Wichtiger Text"), "A failed send threw away the draft.");
    fixture.server.problems.remove(QStringLiteral("send"));
}

void checkDeleteAndMove(const QString &root) {
    Fixture fixture(root + QStringLiteral("/organize"));
    MailStore &store = *fixture.store;
    const QString first = QStringLiteral("acc-1:INBOX:v1:10");
    const QString second = QStringLiteral("acc-1:INBOX:v1:11");

    store.setSelectedMessage(first);
    fixture.server.answers.insert(QStringLiteral("delete"), QJsonObject{{QStringLiteral("changed"), 1}});
    QString reported = QStringLiteral("not called");
    store.deleteMessages({first}, [&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "Deleting failed: " + reported.toStdString());
    check(!store.message(first).has_value(), "The deleted message is still listed.");
    check(store.selectedMessage().isEmpty(), "The selection survived the deletion.");
    // Deleting is a move to Trash, so no target is sent.
    check(!fixture.server.lastExtra(QStringLiteral("delete")).contains(QStringLiteral("target")),
          "Deleting named a target folder.");
    check(fixture.server.lastExtra(QStringLiteral("delete")).value(QStringLiteral("validity")).toString()
              == QStringLiteral("v1"),
          "The mailbox generation was not sent along.");

    fixture.server.answers.insert(QStringLiteral("move"), QJsonObject{{QStringLiteral("changed"), 1}});
    store.moveMessages({second}, QStringLiteral("Archiv"), [&reported](QString p) { reported = p; });
    check(reported.isEmpty(), "Moving failed: " + reported.toStdString());
    check(fixture.server.lastExtra(QStringLiteral("move")).value(QStringLiteral("target")).toString()
              == QStringLiteral("Archiv"),
          "The destination folder was not sent.");
    check(!store.message(second).has_value(), "The moved message is still in the old folder.");

    // A selection with nothing usable in it must not reach the server.
    fixture.server.calls.clear();
    store.deleteMessages({}, [&reported](QString p) { reported = p; });
    check(fixture.server.calls.isEmpty(), "An empty selection reached the server.");
    store.deleteMessages({QStringLiteral("acc-1:INBOX:v1:999")}, [&reported](QString p) { reported = p; });
    check(!reported.isEmpty() && fixture.server.calls.isEmpty(),
          "A selection of messages that do not exist reached the server.");

    // A failure leaves the messages where they are.
    store.refresh([](QString) {});
    fixture.server.problems.insert(QStringLiteral("delete"), QStringLiteral("Kein Papierkorb."));
    store.deleteMessages({first}, [&reported](QString p) { reported = p; });
    check(reported == QStringLiteral("Kein Papierkorb."), "A delete failure was not reported.");
    check(store.message(first).has_value(), "A failed deletion removed the message anyway.");
}

void checkAttachments(const QString &root) {
    Fixture fixture(root + QStringLiteral("/attachments"));
    MailStore &store = *fixture.store;
    const QString id = QStringLiteral("acc-1:INBOX:v1:11");

    fixture.server.answers.insert(QStringLiteral("attachment"), QJsonObject{
        {QStringLiteral("data"), QString::fromUtf8(QByteArrayLiteral("Rechnungsinhalt").toBase64())},
    });
    QString reported = QStringLiteral("not called");
    QByteArray payload;
    store.readAttachment(id, QStringLiteral("2"), [&](QString problem, QByteArray data) {
        reported = problem;
        payload = data;
    });
    check(reported.isEmpty(), "Reading an attachment failed: " + reported.toStdString());
    check(payload == QByteArrayLiteral("Rechnungsinhalt"), "The attachment content was lost.");

    // Anything that is not decodable is a failure, not an empty file.
    for (const QJsonObject &answer : {
             QJsonObject{{QStringLiteral("data"), QStringLiteral("nicht base64!!")}},
             QJsonObject{{QStringLiteral("data"), QString()}},
             QJsonObject{},
         }) {
        fixture.server.answers.insert(QStringLiteral("attachment"), answer);
        store.readAttachment(id, QStringLiteral("2"), [&](QString problem, QByteArray data) {
            reported = problem;
            payload = data;
        });
        check(!reported.isEmpty() && payload.isEmpty(), "An undecodable attachment was accepted.");
    }

    store.readAttachment(QStringLiteral("acc-1:INBOX:v1:999"), QStringLiteral("2"),
                         [&](QString problem, QByteArray) { reported = problem; });
    check(!reported.isEmpty(), "An attachment of a message that does not exist was accepted.");
}

void checkPreferences(const QString &root) {
    const QString directory = root + QStringLiteral("/preferences");
    {
        Fixture fixture(directory);
        MailStore &store = *fixture.store;
        // The WebKit defaults: notifications off, remote images on.
        check(!store.preferences().notifications, "Notifications are on by default.");
        check(store.preferences().remoteImages, "Remote images are off by default.");
        store.setNotifications(true);
        store.setRemoteImages(false);
    }
    MailStore reopened(directory.toStdString());
    check(reopened.preferences().notifications, "The notification setting was not persisted.");
    check(!reopened.preferences().remoteImages, "The remote-image setting was not persisted.");
    // The inbox ids are remembered, otherwise every restart would announce
    // everything again.
    check(!reopened.preferences().knownInbox.empty(), "The known inbox ids were not persisted.");
}

void checkNotificationSignal(const QString &root) {
    Fixture fixture(root + QStringLiteral("/notify"));
    MailStore &store = *fixture.store;
    store.setNotifications(true);

    int announced = 0;
    QString sender;
    QObject::connect(&store, &MailStore::newMail,
                     [&](const QString &, int count, const QString &from, const QString &) {
        announced += count;
        sender = from;
    });

    // Mail that was already there on the previous refresh is not announced.
    fixture.server.answers.insert(QStringLiteral("list"), listAnswer());
    store.refresh([](QString) {});
    check(announced == 0, "Known mail was announced.");

    // A genuinely newer unread message is.
    QJsonObject answer = listAnswer();
    QJsonArray messages = answer.value(QStringLiteral("messages")).toArray();
    messages.append(QJsonObject{
        {QStringLiteral("uid"), QStringLiteral("12")},
        {QStringLiteral("subject"), QStringLiteral("Neu")},
        {QStringLiteral("sender"), QStringLiteral("Neuer Absender")},
        {QStringLiteral("date"), 1700000100.0},
        {QStringLiteral("unread"), true},
    });
    answer.insert(QStringLiteral("messages"), messages);
    fixture.server.answers.insert(QStringLiteral("list"), answer);
    store.refresh([](QString) {});
    check(announced == 1, "New mail was not announced: " + std::to_string(announced));
    check(sender == QStringLiteral("Neuer Absender"), "The wrong sender was announced.");

    // With notifications switched off nothing is announced.
    store.setNotifications(false);
    messages.append(QJsonObject{
        {QStringLiteral("uid"), QStringLiteral("13")},
        {QStringLiteral("subject"), QStringLiteral("Noch neuer")},
        {QStringLiteral("sender"), QStringLiteral("Jemand")},
        {QStringLiteral("date"), 1700000200.0},
        {QStringLiteral("unread"), true},
    });
    answer.insert(QStringLiteral("messages"), messages);
    fixture.server.answers.insert(QStringLiteral("list"), answer);
    store.refresh([](QString) {});
    check(announced == 1, "Mail was announced with notifications switched off.");
}

void checkKeychain(const QString &root) {
    const QString directory = root + QStringLiteral("/keys");
    MailSecrets secrets(directory.toStdString());
    if (!secrets.available()) return;
    // A separate service from the WebKit build's `local.yobro.mail`, so one
    // shell cannot read the other's mailbox passwords.
    check(secrets.service().find("local.yobro.mail") == std::string::npos,
          "The mail Keychain service is the WebKit build's.");
    check(secrets.service().find(directory.toStdString()) != std::string::npos,
          "The mail Keychain service is not profile specific.");

    const QString id = QStringLiteral("mail-test-account");
    secrets.remove(id);
    check(secrets.read(id).isEmpty(), "A password existed before it was stored.");
    check(secrets.store(id, QStringLiteral("app-pw-1")), "A password could not be stored.");
    check(secrets.read(id) == QStringLiteral("app-pw-1"), "A stored password was not read back.");
    check(secrets.store(id, QStringLiteral("app-pw-2")), "A password could not be replaced.");
    check(secrets.read(id) == QStringLiteral("app-pw-2"), "A replaced password was not read back.");
    check(!secrets.store(id, QString()), "An empty password was stored.");
    check(!secrets.store(QString(), QStringLiteral("x")), "A password was stored without an account.");
    check(secrets.remove(id), "A password could not be removed.");
    check(secrets.remove(id), "Removing an absent password failed.");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        checkSharedScript();
        checkWorkerRefusals();
        checkAccountValidation();
        checkFolders();
        checkMessageIdentity();
        checkNewMailDetection();
        checkPresets();
        checkDomainParsing();
        checkAutoconfigEndpoints();
        checkAutoconfigParsing();
        checkConnectAndDisconnect(work.path());
        checkDamagedAccountList(work.path());
        checkRefreshAndFilters(work.path());
        checkOpenMessage(work.path());
        checkReplyAndSend(work.path());
        checkDeleteAndMove(work.path());
        checkAttachments(work.path());
        checkPreferences(work.path());
        checkNotificationSignal(work.path());
        checkKeychain(work.path());
    } catch (const std::exception &error) {
        std::cerr << "MAIL FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "MAIL PASS\n";
    return 0;
}
