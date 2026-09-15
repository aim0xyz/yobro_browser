#include "engine/qtwebengine/QtBrowserLibrary.hpp"
#include "engine/qtwebengine/QtBrowserProfile.hpp"
#include "engine/qtwebengine/QtEventLoop.hpp"
#include "spike/BridgePolicyStore.hpp"
#include "spike/MailPanel.hpp"
#include "spike/MailStore.hpp"
#include "spike/SpikeWindow.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTreeWidget>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using yobro::spike::MailAccount;
using yobro::spike::MailPanel;
using yobro::spike::MailStore;
using yobro::spike::SpikeWindow;

void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct MailApp {
    yobro::qtwebengine::QtBrowserEngine engine;
    yobro::core::ProfilePaths paths;
    yobro::spike::BridgePolicyStore policy;
    std::unique_ptr<yobro::engine::BrowserProfile> profile;
    std::shared_ptr<yobro::qtwebengine::QtBrowserLibrary> library;
    yobro::qtwebengine::QtEventLoop eventLoop;
    std::unique_ptr<yobro::controller::BrowserSession> session;
    std::unique_ptr<SpikeWindow> window;

    MailApp() : paths(yobro::core::ProfilePaths::forProfile("mail-ui")), policy(paths.bridgePolicy) {
        paths.createDirectories();
        // Only these named files are removed, never a directory, so an earlier
        // run cannot make the counts wrong.
        for (const char *name : {"mail-accounts.json", "mail-preferences.json", "space-chat.json"})
            QFile::remove(QString::fromStdString((paths.profile / name).string()));
        QFile::remove(QString::fromStdString(paths.session.string()));

        profile = engine.openProfile({
            .id = "mail-ui",
            .storagePath = paths.storage.string(),
            .cachePath = paths.cache.string(),
            .persistent = true,
        });
        auto *qtProfile = dynamic_cast<yobro::qtwebengine::QtBrowserProfile *>(profile.get());
        check(qtProfile != nullptr, "Could not create the mail test profile.");
        library = std::make_shared<yobro::qtwebengine::QtBrowserLibrary>(*qtProfile, paths.profile);
        session = std::make_unique<yobro::controller::BrowserSession>(
            std::move(profile), eventLoop, yobro::controller::BrowserSessionConfig{
                .browser = "YoBro", .version = "test", .engine = "Chromium",
                .socketPath = paths.control.string(), .profileName = "mail-ui",
                .space = "Personal", .profileActive = true, .agentEnabled = true,
                .libraryAccess = false, .library = library,
            }
        );
        window = std::make_unique<SpikeWindow>(*session, *library, policy, paths);
        window->setAttribute(Qt::WA_DontShowOnScreen);
        window->show();
    }

    ~MailApp() {
        window.reset();
        if (library) library->shutdownDownloads();
        session.reset();
        library.reset();
    }
};

/// Answers mail requests from a script instead of a mail server.
class FakeServer {
public:
    QStringList actions;
    QHash<QString, QJsonObject> answers;
    QHash<QString, QString> problems;

    MailStore::Transport transport() {
        return [this](const QString &action, const MailAccount &, const QJsonObject &,
                      std::function<void(QString, QJsonObject)> done) {
            actions.append(action);
            done(problems.value(action), answers.value(action));
        };
    }

    MailStore::Verifier verifier() {
        return [](const MailAccount &, const QString &, std::function<void(QString)> done) {
            done({});
        };
    }
};

QJsonObject folderAnswer() {
    return QJsonObject{{QStringLiteral("folders"), QJsonArray{
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("INBOX")},
            {QStringLiteral("title"), QStringLiteral("Posteingang")},
            {QStringLiteral("selectable"), true},
            {QStringLiteral("unread"), 1},
            {QStringLiteral("role"), QStringLiteral("inbox")},
        },
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("Archiv")},
            {QStringLiteral("title"), QStringLiteral("Archiv")},
            {QStringLiteral("selectable"), true},
            {QStringLiteral("unread"), 0},
        },
        // A folder the provider cannot select must not show up as a target.
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("Gruppen")},
            {QStringLiteral("title"), QStringLiteral("Gruppen")},
            {QStringLiteral("selectable"), false},
        },
    }}};
}

QJsonObject listAnswer() {
    return QJsonObject{
        {QStringLiteral("validity"), QStringLiteral("v1")},
        {QStringLiteral("total"), 2},
        {QStringLiteral("messages"), QJsonArray{
            QJsonObject{
                {QStringLiteral("uid"), QStringLiteral("10")},
                {QStringLiteral("subject"), QStringLiteral("Gelesene Nachricht")},
                {QStringLiteral("sender"), QStringLiteral("Alt <alt@example.com>")},
                {QStringLiteral("to"), QStringLiteral("ich@example.com")},
                {QStringLiteral("date"), 1700000010.0},
                {QStringLiteral("unread"), false},
                {QStringLiteral("messageID"), QStringLiteral("<10@example.com>")},
            },
            QJsonObject{
                {QStringLiteral("uid"), QStringLiteral("11")},
                {QStringLiteral("subject"), QStringLiteral("Rechnung")},
                {QStringLiteral("sender"), QStringLiteral("Buchhaltung <b@example.com>")},
                {QStringLiteral("to"), QStringLiteral("ich@example.com")},
                {QStringLiteral("replyTo"), QStringLiteral("antwort@example.com")},
                {QStringLiteral("date"), 1700000020.0},
                {QStringLiteral("unread"), true},
                {QStringLiteral("messageID"), QStringLiteral("<11@example.com>")},
            },
        }},
    };
}

QJsonObject bodyAnswer() {
    return QJsonObject{
        {QStringLiteral("text"), QStringLiteral("Bitte bis Freitag zahlen.")},
        {QStringLiteral("html"),
         QStringLiteral("<p>Bitte bis Freitag zahlen.</p><img src=\"https://tracker.example/pixel.gif\">")},
        {QStringLiteral("seen"), true},
        {QStringLiteral("attachments"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("2")},
            {QStringLiteral("name"), QStringLiteral("rechnung.pdf")},
            {QStringLiteral("mime"), QStringLiteral("application/pdf")},
            {QStringLiteral("size"), 2048},
        }}},
    };
}

MailAccount testAccount() {
    MailAccount account;
    account.id = QStringLiteral("acc-1");
    account.label = QStringLiteral("Arbeit");
    account.address = QStringLiteral("ich@example.com");
    account.username = QStringLiteral("ich@example.com");
    account.imapHost = QStringLiteral("imap.example.com");
    account.smtpHost = QStringLiteral("smtp.example.com");
    return account;
}

template <typename Widget>
Widget *widget(const SpikeWindow &window, const QString &name) {
    Widget *found = window.findChild<Widget *>(name);
    check(found != nullptr, "The mail surface is missing " + name.toStdString() + ".");
    return found;
}

QStringList listTexts(const QListWidget *list) {
    QStringList texts;
    for (int index = 0; index < list->count(); ++index) texts.append(list->item(index)->text());
    return texts;
}

void checkWindowOpens(SpikeWindow &window) {
    check(window.findChild<QDialog *>(QStringLiteral("mailDialog")) == nullptr,
          "The mail window exists before it was asked for.");
    // The same command the menu offers on ⇧⌘M.
    auto *action = window.findChild<QAction *>(QStringLiteral("mailAction"));
    check(action != nullptr, "There is no menu command for mail.");
    check(action->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
          "The mail command is not on ⇧⌘M.");
    check(window.findChild<QPushButton *>(QStringLiteral("mailButton")) != nullptr,
          "There is no sidebar button for mail.");

    action->trigger();
    check(window.findChild<QDialog *>(QStringLiteral("mailDialog")) != nullptr,
          "The mail window did not open.");
    for (const QString &name : {
             QStringLiteral("mailPanel"), QStringLiteral("mailFolderTree"),
             QStringLiteral("mailMessageList"), QStringLiteral("mailBodyView"),
             QStringLiteral("mailAttachmentList"), QStringLiteral("mailSearchField"),
             QStringLiteral("mailUnreadOnlyToggle"), QStringLiteral("mailNotificationsToggle"),
             QStringLiteral("mailAddAccountButton"), QStringLiteral("mailRemoveAccountButton"),
             QStringLiteral("mailRefreshButton"), QStringLiteral("mailLoadMoreButton"),
             QStringLiteral("mailReplyButton"), QStringLiteral("mailDeleteButton"),
             QStringLiteral("mailMoveButton"), QStringLiteral("mailSaveAttachmentButton"),
             QStringLiteral("mailComposeAccountPicker"), QStringLiteral("mailComposeToField"),
             QStringLiteral("mailComposeSubjectField"), QStringLiteral("mailComposeTextField"),
             QStringLiteral("mailSendButton"), QStringLiteral("mailDiscardButton"),
             QStringLiteral("mailStatusLabel"),
         }) {
        check(window.findChild<QWidget *>(name) != nullptr,
              "The mail surface is missing " + name.toStdString() + ".");
    }

    // Without a mailbox nothing can be sent or refreshed.
    check(!widget<QPushButton>(window, QStringLiteral("mailRefreshButton"))->isEnabled(),
          "Refresh is offered without a mailbox.");
    check(!widget<QPushButton>(window, QStringLiteral("mailSendButton"))->isEnabled(),
          "Sending is offered without a mailbox.");
    check(!widget<QLabel>(window, QStringLiteral("mailStatusLabel"))->text().isEmpty(),
          "The empty state says nothing.");
}

void checkMailboxAndReading(SpikeWindow &window, FakeServer &server) {
    MailStore &store = window.mailStore();
    server.answers.insert(QStringLiteral("folders"), folderAnswer());
    server.answers.insert(QStringLiteral("list"), listAnswer());
    server.answers.insert(QStringLiteral("body"), bodyAnswer());

    QString reported = QStringLiteral("not called");
    store.connectAccount(testAccount(), QStringLiteral("app-password"),
                         [&reported](QString problem) { reported = problem; });
    check(reported.isEmpty(), "The mailbox could not be connected: " + reported.toStdString());
    store.refresh([](QString) {});

    auto *folders = widget<QTreeWidget>(window, QStringLiteral("mailFolderTree"));
    check(folders->topLevelItemCount() == 1, "The mailbox is not listed.");
    QTreeWidgetItem *mailbox = folders->topLevelItem(0);
    check(mailbox->text(0).contains(QStringLiteral("Arbeit")), "The mailbox has no name.");
    // The unselectable folder must not be offered.
    check(mailbox->childCount() == 2,
          "The folder list is wrong: " + std::to_string(mailbox->childCount()) + " folders.");
    check(mailbox->child(0)->text(0).contains(QStringLiteral("(1)")),
          "The unread count is not shown on the folder.");

    auto *messages = widget<QListWidget>(window, QStringLiteral("mailMessageList"));
    check(messages->count() == 2, "The inbox is not listed.");
    // Newest first, and unread marked.
    check(messages->item(0)->text().contains(QStringLiteral("Rechnung")),
          "The message list is not sorted newest first.");
    check(messages->item(0)->text().startsWith(QStringLiteral("●")), "Unread mail is not marked.");
    check(!messages->item(1)->text().startsWith(QStringLiteral("●")),
          "Read mail is marked as unread.");

    // Opening a message loads it and shows it.
    messages->setCurrentRow(0);
    Q_EMIT messages->itemClicked(messages->item(0));
    check(server.actions.contains(QStringLiteral("body")), "The message body was not requested.");
    auto *body = widget<QTextBrowser>(window, QStringLiteral("mailBodyView"));
    check(body->toPlainText().contains(QStringLiteral("Freitag")), "The message body is not shown.");
    // Nothing in a message may be fetched. This view has no network at all, and
    // a link in it must not open either.
    check(!body->openExternalLinks() && !body->openLinks(),
          "The message view would follow a link out of a message.");
    check(!body->toPlainText().contains(QStringLiteral("tracker.example")),
          "A tracking pixel showed up as text, so the HTML was not parsed.");
    check(widget<QLabel>(window, QStringLiteral("mailMessageHeader"))->text()
              .contains(QStringLiteral("Buchhaltung")),
          "The sender is not shown.");

    auto *attachments = widget<QListWidget>(window, QStringLiteral("mailAttachmentList"));
    check(attachments->count() == 1, "The attachment is not listed.");
    check(attachments->item(0)->text().contains(QStringLiteral("rechnung.pdf"))
              && attachments->item(0)->text().contains(QStringLiteral("2 KB")),
          "The attachment is listed without its name and size.");
    check(!store.message(QStringLiteral("acc-1:INBOX:v1:11"))->unread,
          "The opened message stayed unread.");

    // The filters work on the visible list.
    auto *search = widget<QLineEdit>(window, QStringLiteral("mailSearchField"));
    search->setText(QStringLiteral("Rechnung"));
    check(widget<QListWidget>(window, QStringLiteral("mailMessageList"))->count() == 1,
          "The search does not filter the list.");
    search->clear();
    auto *unreadOnly = widget<QCheckBox>(window, QStringLiteral("mailUnreadOnlyToggle"));
    unreadOnly->setChecked(true);
    // The open message stays, everything else that is read disappears.
    check(widget<QListWidget>(window, QStringLiteral("mailMessageList"))->count() == 1,
          "The unread filter does not work.");
    unreadOnly->setChecked(false);

    // A folder switch asks the provider for that folder. The tree is rebuilt on
    // every redraw, so its items are looked up again right before they are used.
    const auto folderRow = [&window](int index) {
        auto *tree = widget<QTreeWidget>(window, QStringLiteral("mailFolderTree"));
        check(tree->topLevelItemCount() == 1, "The mailbox disappeared from the folder tree.");
        QTreeWidgetItem *root = tree->topLevelItem(0);
        check(index < root->childCount(), "The folder tree has fewer folders than expected.");
        QTreeWidgetItem *row = root->child(index);
        tree->setCurrentItem(row);
        Q_EMIT tree->itemClicked(row, 0);
    };
    server.actions.clear();
    folderRow(1);
    check(store.selectedFolder() == QStringLiteral("Archiv"), "The folder was not switched.");
    check(server.actions.contains(QStringLiteral("list")), "The new folder was not loaded.");
    folderRow(0);
    check(store.selectedFolder() == QStringLiteral("INBOX"), "The folder was not switched back.");
}

void checkAttachmentSaving(SpikeWindow &window, FakeServer &server, const QString &root) {
    MailStore &store = window.mailStore();
    store.openMessage(QStringLiteral("acc-1:INBOX:v1:11"), [](QString) {});
    auto *panel = window.findChild<MailPanel *>(QStringLiteral("mailPanel"));
    check(panel != nullptr, "The mail panel is missing.");

    const QString target = root + QStringLiteral("/rechnung.pdf");
    QString suggested;
    panel->setSavePathChooser([&suggested, target](const QString &name) {
        suggested = name;
        return target;
    });
    server.answers.insert(QStringLiteral("attachment"), QJsonObject{
        {QStringLiteral("data"), QString::fromUtf8(QByteArrayLiteral("PDF-Inhalt").toBase64())},
    });

    auto *attachments = widget<QListWidget>(window, QStringLiteral("mailAttachmentList"));
    check(attachments->count() == 1, "The attachment is not listed.");
    attachments->setCurrentRow(0);
    widget<QPushButton>(window, QStringLiteral("mailSaveAttachmentButton"))->click();

    check(suggested == QStringLiteral("rechnung.pdf"),
          "The suggested file name is wrong: " + suggested.toStdString());
    QFile saved(target);
    check(saved.open(QIODevice::ReadOnly), "The attachment was not written.");
    check(saved.readAll() == QByteArrayLiteral("PDF-Inhalt"), "The attachment content is wrong.");
    saved.close();

    // A sender-chosen path must not decide where the file lands.
    server.answers.insert(QStringLiteral("body"), QJsonObject{
        {QStringLiteral("text"), QStringLiteral("Anhang mit Pfad")},
        {QStringLiteral("seen"), true},
        {QStringLiteral("attachments"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("3")},
            {QStringLiteral("name"), QStringLiteral("../../evil.sh")},
            {QStringLiteral("mime"), QStringLiteral("text/plain")},
            {QStringLiteral("size"), 10},
        }}},
    });
    store.openMessage(QStringLiteral("acc-1:INBOX:v1:10"), [](QString) {});
    attachments = widget<QListWidget>(window, QStringLiteral("mailAttachmentList"));
    check(attachments->count() == 1, "The second attachment is not listed.");
    attachments->setCurrentRow(0);
    widget<QPushButton>(window, QStringLiteral("mailSaveAttachmentButton"))->click();
    check(suggested == QStringLiteral("evil.sh"),
          "A path from the sender was suggested as a file name: " + suggested.toStdString());
}

void checkReplyAndSend(SpikeWindow &window, FakeServer &server) {
    MailStore &store = window.mailStore();
    store.openMessage(QStringLiteral("acc-1:INBOX:v1:11"), [](QString) {});
    widget<QPushButton>(window, QStringLiteral("mailReplyButton"))->click();

    check(widget<QLineEdit>(window, QStringLiteral("mailComposeToField"))->text()
              == QStringLiteral("antwort@example.com"),
          "The reply does not go to the Reply-To address.");
    check(widget<QLineEdit>(window, QStringLiteral("mailComposeSubjectField"))->text()
              == QStringLiteral("Re: Rechnung"),
          "The reply subject is wrong.");

    auto *text = widget<QPlainTextEdit>(window, QStringLiteral("mailComposeTextField"));
    text->setPlainText(QStringLiteral("Ist überwiesen."));
    check(store.draft().text == QStringLiteral("Ist überwiesen."),
          "The typed text did not reach the draft.");

    server.answers.insert(QStringLiteral("send"), QJsonObject{});
    server.actions.clear();
    widget<QPushButton>(window, QStringLiteral("mailSendButton"))->click();
    check(server.actions.contains(QStringLiteral("send")), "The message was not sent.");
    check(store.draft().isEmpty(), "The draft was not cleared after sending.");
    check(widget<QLineEdit>(window, QStringLiteral("mailComposeToField"))->text().isEmpty(),
          "The form still shows the sent draft.");

    // A draft is thrown away only when asked for.
    text->setPlainText(QStringLiteral("Halber Satz"));
    widget<QLineEdit>(window, QStringLiteral("mailComposeToField"))->setText(QStringLiteral("x@y.de"));
    check(!store.draft().isEmpty(), "The new draft was not kept.");
    widget<QPushButton>(window, QStringLiteral("mailDiscardButton"))->click();
    check(store.draft().isEmpty(), "Discarding did not clear the draft.");
}

void checkAssistantMailTools(SpikeWindow &window, FakeServer &server) {
    server.answers.insert(QStringLiteral("body"), bodyAnswer());

    std::optional<QJsonObject> inbox;
    bool answered = false;
    window.readMail(20, [&](std::optional<QJsonObject> payload) {
        inbox = payload;
        answered = true;
    });
    check(answered, "The mail tool did not answer.");
    check(inbox.has_value(), "The mail tool found no mailbox.");
    check(inbox->value(QStringLiteral("source")).toString() == QStringLiteral("YoBro built-in mail"),
          "The mail tool does not say where the data came from.");
    const QJsonArray rows = inbox->value(QStringLiteral("messages")).toArray();
    check(rows.size() == 2, "The mail tool returned the wrong number of messages.");
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        for (const QString &field : {QStringLiteral("id"), QStringLiteral("subject"),
                                     QStringLiteral("sender"), QStringLiteral("date"),
                                     QStringLiteral("unread")}) {
            check(row.contains(field), "The mail tool omits " + field.toStdString() + ".");
        }
    }

    // A limit is honoured, so one call cannot pull in the whole mailbox.
    window.readMail(1, [&](std::optional<QJsonObject> payload) { inbox = payload; });
    check(inbox->value(QStringLiteral("messages")).toArray().size() == 1,
          "The mail tool ignored the limit.");

    std::optional<QJsonObject> message;
    window.readMailMessage(QStringLiteral("acc-1:INBOX:v1:11"),
                           [&message](std::optional<QJsonObject> payload) { message = payload; });
    check(message.has_value(), "The message tool found nothing.");
    check(message->value(QStringLiteral("subject")).toString() == QStringLiteral("Rechnung"),
          "The message tool returned the wrong message.");
    check(message->value(QStringLiteral("body")).toString().contains(QStringLiteral("Freitag")),
          "The message tool returned no body.");
    // Only readable text goes to a model, never the HTML with its remote parts.
    check(!message->value(QStringLiteral("body")).toString().contains(QStringLiteral("tracker.example")),
          "The HTML part with its remote image reached the model.");

    // A message that does not exist is refused rather than guessed at.
    message = QJsonObject{};
    window.readMailMessage(QStringLiteral("acc-1:INBOX:v1:999"),
                           [&message](std::optional<QJsonObject> payload) { message = payload; });
    check(!message.has_value(), "A message that does not exist was answered.");
}

void checkDisconnect(SpikeWindow &window) {
    MailStore &store = window.mailStore();
    check(store.disconnectAccount(QStringLiteral("acc-1")).isEmpty(),
          "The mailbox could not be removed.");
    check(store.accounts().empty(), "The mailbox survived being removed.");
    check(widget<QTreeWidget>(window, QStringLiteral("mailFolderTree"))->topLevelItemCount() == 0,
          "The removed mailbox is still shown.");
    check(widget<QListWidget>(window, QStringLiteral("mailMessageList"))->count() == 0,
          "Messages of a removed mailbox are still shown.");
    check(widget<QTextBrowser>(window, QStringLiteral("mailBodyView"))->toPlainText().isEmpty(),
          "The reader still shows a message of a removed mailbox.");
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication application(argc, argv);
    QTemporaryDir work;
    if (!work.isValid()) return 1;

    try {
        MailApp app;
        SpikeWindow &window = *app.window;
        FakeServer server;
        window.mailStore().setTransport(server.transport());
        window.mailStore().setVerifier(server.verifier());

        checkWindowOpens(window);
        checkMailboxAndReading(window, server);
        checkAttachmentSaving(window, server, work.path());
        checkReplyAndSend(window, server);
        checkAssistantMailTools(window, server);
        checkDisconnect(window);
    } catch (const std::exception &error) {
        std::cerr << "MAIL UI FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "MAIL UI PASS\n";
    return 0;
}
