#include "spike/MailPanel.hpp"

#include "spike/Localization.hpp"
#include "spike/MailDiscovery.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace yobro::spike {
namespace {

constexpr int folderRole = Qt::UserRole;
constexpr int accountRole = Qt::UserRole + 1;

QString shortDate(qint64 seconds) {
    if (seconds <= 0) return {};
    return QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("dd.MM.yyyy HH:mm"));
}

QString sizeLabel(int bytes) {
    if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024) + QStringLiteral(" KB");
    return QString::number(bytes / (1024 * 1024)) + QStringLiteral(" MB");
}

} // namespace

MailPanel::MailPanel(MailStore &store, QWidget *parent) : QWidget(parent), store_(store) {
    setObjectName(QStringLiteral("mailPanel"));
    savePathChooser_ = [this](const QString &suggested) {
        return QFileDialog::getSaveFileName(this, L(QStringLiteral("Anhang speichern"),
                                                    QStringLiteral("Save attachment")), suggested);
    };

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *toolbar = new QHBoxLayout();
    addAccountButton_ = new QPushButton(L(QStringLiteral("Postfach hinzufügen"),
                                          QStringLiteral("Add mailbox")), this);
    addAccountButton_->setObjectName(QStringLiteral("mailAddAccountButton"));
    removeAccountButton_ = new QPushButton(L(QStringLiteral("Postfach entfernen"),
                                             QStringLiteral("Remove mailbox")), this);
    removeAccountButton_->setObjectName(QStringLiteral("mailRemoveAccountButton"));
    refreshButton_ = new QPushButton(L(QStringLiteral("Aktualisieren"), QStringLiteral("Refresh")), this);
    refreshButton_->setObjectName(QStringLiteral("mailRefreshButton"));
    notifications_ = new QCheckBox(L(QStringLiteral("Über neue Mail hinweisen"),
                                     QStringLiteral("Announce new mail")), this);
    notifications_->setObjectName(QStringLiteral("mailNotificationsToggle"));
    toolbar->addWidget(addAccountButton_);
    toolbar->addWidget(removeAccountButton_);
    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(notifications_);
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    folders_ = new QTreeWidget(splitter);
    folders_->setObjectName(QStringLiteral("mailFolderTree"));
    folders_->setHeaderHidden(true);
    folders_->setMinimumWidth(180);
    splitter->addWidget(folders_);

    auto *listHost = new QWidget(splitter);
    auto *listLayout = new QVBoxLayout(listHost);
    listLayout->setContentsMargins(0, 0, 0, 0);
    auto *filterRow = new QHBoxLayout();
    search_ = new QLineEdit(listHost);
    search_->setObjectName(QStringLiteral("mailSearchField"));
    search_->setPlaceholderText(L(QStringLiteral("Betreff oder Absender …"),
                                  QStringLiteral("Subject or sender …")));
    unreadOnly_ = new QCheckBox(L(QStringLiteral("Nur ungelesen"), QStringLiteral("Unread only")), listHost);
    unreadOnly_->setObjectName(QStringLiteral("mailUnreadOnlyToggle"));
    filterRow->addWidget(search_, 1);
    filterRow->addWidget(unreadOnly_);
    listLayout->addLayout(filterRow);
    messages_ = new QListWidget(listHost);
    messages_->setObjectName(QStringLiteral("mailMessageList"));
    messages_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listLayout->addWidget(messages_, 1);
    loadMoreButton_ = new QPushButton(L(QStringLiteral("Mehr laden"), QStringLiteral("Load more")), listHost);
    loadMoreButton_->setObjectName(QStringLiteral("mailLoadMoreButton"));
    listLayout->addWidget(loadMoreButton_);
    splitter->addWidget(listHost);

    auto *readerHost = new QWidget(splitter);
    auto *readerLayout = new QVBoxLayout(readerHost);
    readerLayout->setContentsMargins(0, 0, 0, 0);
    header_ = new QLabel(readerHost);
    header_->setObjectName(QStringLiteral("mailMessageHeader"));
    header_->setWordWrap(true);
    header_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    readerLayout->addWidget(header_);

    auto *messageActions = new QHBoxLayout();
    replyButton_ = new QPushButton(L(QStringLiteral("Antworten"), QStringLiteral("Reply")), readerHost);
    replyButton_->setObjectName(QStringLiteral("mailReplyButton"));
    deleteButton_ = new QPushButton(L(QStringLiteral("Löschen"), QStringLiteral("Delete")), readerHost);
    deleteButton_->setObjectName(QStringLiteral("mailDeleteButton"));
    moveButton_ = new QPushButton(L(QStringLiteral("Verschieben"), QStringLiteral("Move")), readerHost);
    moveButton_->setObjectName(QStringLiteral("mailMoveButton"));
    messageActions->addWidget(replyButton_);
    messageActions->addWidget(deleteButton_);
    messageActions->addWidget(moveButton_);
    messageActions->addStretch();
    readerLayout->addLayout(messageActions);

    bodyView_ = new QTextBrowser(readerHost);
    bodyView_->setObjectName(QStringLiteral("mailBodyView"));
    // A message must not be able to fetch anything. `QTextBrowser` has no
    // network access, and both switches below keep it from following a link or
    // reaching for a local file either.
    bodyView_->setOpenExternalLinks(false);
    bodyView_->setOpenLinks(false);
    bodyView_->setReadOnly(true);
    readerLayout->addWidget(bodyView_, 1);

    attachments_ = new QListWidget(readerHost);
    attachments_->setObjectName(QStringLiteral("mailAttachmentList"));
    attachments_->setMaximumHeight(80);
    readerLayout->addWidget(attachments_);
    saveAttachmentButton_ = new QPushButton(L(QStringLiteral("Anhang speichern"),
                                              QStringLiteral("Save attachment")), readerHost);
    saveAttachmentButton_->setObjectName(QStringLiteral("mailSaveAttachmentButton"));
    readerLayout->addWidget(saveAttachmentButton_);
    splitter->addWidget(readerHost);

    splitter->setSizes({200, 320, 520});
    layout->addWidget(splitter, 1);

    auto *compose = new QWidget(this);
    compose->setObjectName(QStringLiteral("mailComposePane"));
    auto *composeLayout = new QFormLayout(compose);
    composeLayout->setContentsMargins(0, 0, 0, 0);
    composeAccount_ = new QComboBox(compose);
    composeAccount_->setObjectName(QStringLiteral("mailComposeAccountPicker"));
    composeTo_ = new QLineEdit(compose);
    composeTo_->setObjectName(QStringLiteral("mailComposeToField"));
    composeSubject_ = new QLineEdit(compose);
    composeSubject_->setObjectName(QStringLiteral("mailComposeSubjectField"));
    composeText_ = new QPlainTextEdit(compose);
    composeText_->setObjectName(QStringLiteral("mailComposeTextField"));
    composeText_->setMaximumHeight(110);
    composeLayout->addRow(L(QStringLiteral("Von"), QStringLiteral("From")), composeAccount_);
    composeLayout->addRow(L(QStringLiteral("An"), QStringLiteral("To")), composeTo_);
    composeLayout->addRow(L(QStringLiteral("Betreff"), QStringLiteral("Subject")), composeSubject_);
    composeLayout->addRow(L(QStringLiteral("Text"), QStringLiteral("Text")), composeText_);
    layout->addWidget(compose);

    auto *footer = new QHBoxLayout();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("mailStatusLabel"));
    status_->setWordWrap(true);
    discardButton_ = new QPushButton(L(QStringLiteral("Entwurf verwerfen"),
                                       QStringLiteral("Discard draft")), this);
    discardButton_->setObjectName(QStringLiteral("mailDiscardButton"));
    sendButton_ = new QPushButton(L(QStringLiteral("Senden"), QStringLiteral("Send")), this);
    sendButton_->setObjectName(QStringLiteral("mailSendButton"));
    footer->addWidget(status_, 1);
    footer->addWidget(discardButton_);
    footer->addWidget(sendButton_);
    layout->addLayout(footer);

    QObject::connect(addAccountButton_, &QPushButton::clicked, this, [this] { showAccountForm(); });
    QObject::connect(removeAccountButton_, &QPushButton::clicked, this, [this] { removeAccount(); });
    QObject::connect(refreshButton_, &QPushButton::clicked, this, [this] { reload(); });
    QObject::connect(loadMoreButton_, &QPushButton::clicked, this, [this] { loadMore(); });
    QObject::connect(replyButton_, &QPushButton::clicked, this, [this] { reply(); });
    QObject::connect(deleteButton_, &QPushButton::clicked, this, [this] { deleteSelected(); });
    QObject::connect(moveButton_, &QPushButton::clicked, this, [this] { moveSelected(); });
    QObject::connect(saveAttachmentButton_, &QPushButton::clicked, this, [this] { saveAttachment(); });
    QObject::connect(sendButton_, &QPushButton::clicked, this, [this] { sendDraft(); });
    QObject::connect(discardButton_, &QPushButton::clicked, this, [this] { discardDraft(); });
    QObject::connect(folders_, &QTreeWidget::itemClicked, this, [this] { activateFolder(); });
    QObject::connect(messages_, &QListWidget::itemClicked, this, [this] { activateMessage(); });
    // Selecting a row has to enable its actions right away, not only after the
    // next redraw.
    QObject::connect(attachments_, &QListWidget::currentRowChanged, this, [this](int row) {
        saveAttachmentButton_->setEnabled(!store_.busy() && row >= 0);
    });
    QObject::connect(messages_, &QListWidget::itemSelectionChanged, this, [this] {
        const bool any = !messages_->selectedItems().isEmpty();
        deleteButton_->setEnabled(!store_.busy() && any);
        moveButton_->setEnabled(!store_.busy() && any);
    });
    QObject::connect(search_, &QLineEdit::textChanged, this, [this](const QString &value) {
        if (updating_) return;
        store_.setSearch(value);
    });
    QObject::connect(unreadOnly_, &QCheckBox::toggled, this, [this](bool value) {
        if (updating_) return;
        store_.setUnreadOnly(value);
    });
    QObject::connect(notifications_, &QCheckBox::toggled, this, [this](bool value) {
        if (updating_) return;
        store_.setNotifications(value);
    });
    // The draft belongs to the store, so it survives closing this window. Its
    // buttons follow every keystroke instead of waiting for the next redraw.
    const auto draftChanged = [this] {
        sendButton_->setEnabled(!store_.busy() && !store_.accounts().empty()
                                && !store_.draft().to.trimmed().isEmpty());
        discardButton_->setEnabled(!store_.draft().isEmpty());
    };
    QObject::connect(composeTo_, &QLineEdit::textChanged, this, [this, draftChanged](const QString &value) {
        if (updating_) return;
        store_.draft().to = value;
        draftChanged();
    });
    QObject::connect(composeSubject_, &QLineEdit::textChanged, this,
                     [this, draftChanged](const QString &value) {
        if (updating_) return;
        store_.draft().subject = value;
        draftChanged();
    });
    QObject::connect(composeText_, &QPlainTextEdit::textChanged, this, [this, draftChanged] {
        if (updating_) return;
        store_.draft().text = composeText_->toPlainText();
        draftChanged();
    });
    QObject::connect(composeAccount_, &QComboBox::activated, this, [this](int index) {
        if (updating_ || index < 0) return;
        store_.draft().accountId = composeAccount_->itemData(index).toString();
    });
    QObject::connect(&store_, &MailStore::changed, this, [this] { refresh(); });

    refresh();
}

void MailPanel::setSavePathChooser(SavePathChooser chooser) {
    savePathChooser_ = std::move(chooser);
}

void MailPanel::refresh() {
    if (updating_) return;
    updating_ = true;
    rebuildFolderTree();
    rebuildMessageList();
    showSelectedMessage();

    if (search_->text() != store_.search()) search_->setText(store_.search());
    unreadOnly_->setChecked(store_.unreadOnly());
    notifications_->setChecked(store_.preferences().notifications);

    composeAccount_->clear();
    for (const MailAccount &account : store_.accounts())
        composeAccount_->addItem(account.displayLabel(), account.id);
    const int index = composeAccount_->findData(store_.draft().accountId);
    if (index >= 0) composeAccount_->setCurrentIndex(index);
    if (composeTo_->text() != store_.draft().to) composeTo_->setText(store_.draft().to);
    if (composeSubject_->text() != store_.draft().subject) composeSubject_->setText(store_.draft().subject);
    if (composeText_->toPlainText() != store_.draft().text) composeText_->setPlainText(store_.draft().text);

    const bool busy = store_.busy();
    const bool haveAccounts = !store_.accounts().empty();
    addAccountButton_->setEnabled(!busy && !store_.loadFailed());
    removeAccountButton_->setEnabled(!busy && haveAccounts);
    refreshButton_->setEnabled(!busy && haveAccounts);
    loadMoreButton_->setEnabled(!busy && !store_.selectedAccount().isEmpty());
    const bool haveMessage = !store_.selectedMessage().isEmpty();
    replyButton_->setEnabled(!busy && haveMessage);
    deleteButton_->setEnabled(!busy && !messages_->selectedItems().isEmpty());
    moveButton_->setEnabled(!busy && !messages_->selectedItems().isEmpty());
    saveAttachmentButton_->setEnabled(!busy && attachments_->currentRow() >= 0);
    sendButton_->setEnabled(!busy && haveAccounts && !store_.draft().to.trimmed().isEmpty());
    discardButton_->setEnabled(!store_.draft().isEmpty());

    QString problem;
    for (const MailAccount &account : store_.accounts()) {
        const QString reported = store_.error(account.id);
        if (reported.isEmpty()) continue;
        problem = account.displayLabel() + QStringLiteral(": ") + reported;
    }
    if (!problem.isEmpty()) setStatus(problem, true);
    else if (busy) setStatus(L(QStringLiteral("Postfach wird abgefragt …"),
                               QStringLiteral("Talking to the mailbox …")), false);
    else if (!store_.notice().isEmpty()) setStatus(store_.notice(), false);
    else if (!haveAccounts) setStatus(L(QStringLiteral("Noch kein Postfach eingerichtet."),
                                        QStringLiteral("No mailbox is set up yet.")), false);
    else setStatus({}, false);
    updating_ = false;
}

void MailPanel::rebuildFolderTree() {
    folders_->clear();
    for (const MailAccount &account : store_.accounts()) {
        auto *node = new QTreeWidgetItem(folders_);
        node->setText(0, account.displayLabel() + (store_.hasUnread(account.id)
                                                       ? QStringLiteral("  •")
                                                       : QString()));
        node->setData(0, accountRole, account.id);
        node->setExpanded(true);
        for (const MailFolder &folder : store_.folders(account.id)) {
            if (!folder.selectable) continue;
            auto *leaf = new QTreeWidgetItem(node);
            leaf->setText(0, folder.unread > 0
                ? folder.displayTitle() + QStringLiteral(" (") + QString::number(folder.unread)
                      + QLatin1Char(')')
                : folder.displayTitle());
            leaf->setData(0, accountRole, account.id);
            leaf->setData(0, folderRole, folder.path);
            if (account.id == store_.selectedAccount() && folder.path == store_.selectedFolder())
                folders_->setCurrentItem(leaf);
        }
    }
}

void MailPanel::rebuildMessageList() {
    const QString selected = store_.selectedMessage();
    messages_->clear();
    for (const MailMessage &message : store_.visibleMessages()) {
        auto *item = new QListWidgetItem(
            (message.unread ? QStringLiteral("● ") : QStringLiteral("   "))
                + message.sender + QStringLiteral("\n")
                + (message.subject.isEmpty()
                       ? L(QStringLiteral("(Ohne Betreff)"), QStringLiteral("(No subject)"))
                       : message.subject)
                + QStringLiteral("  ·  ") + shortDate(message.date),
            messages_
        );
        item->setData(Qt::UserRole, message.id());
        if (message.id() == selected) messages_->setCurrentItem(item);
    }
    if (!store_.selectedAccount().isEmpty()) {
        const int total = store_.folderTotal(store_.selectedAccount(), store_.selectedFolder());
        loadMoreButton_->setText(
            L(QStringLiteral("Mehr laden"), QStringLiteral("Load more"))
            + QStringLiteral(" (") + QString::number(messages_->count()) + QStringLiteral(" / ")
            + QString::number(total) + QLatin1Char(')')
        );
    }
}

void MailPanel::showSelectedMessage() {
    const std::optional<MailMessage> message = store_.message(store_.selectedMessage());
    attachments_->clear();
    if (!message) {
        header_->clear();
        bodyView_->clear();
        return;
    }
    header_->setText(
        QStringLiteral("<b>") + message->subject.toHtmlEscaped() + QStringLiteral("</b><br>")
        + message->sender.toHtmlEscaped() + QStringLiteral(" → ") + message->recipient.toHtmlEscaped()
        + QStringLiteral("<br>") + shortDate(message->date)
    );

    const std::optional<MailBody> body = store_.body(store_.selectedMessage());
    if (!body) {
        bodyView_->setPlainText(L(QStringLiteral("Nachricht wird geladen …"),
                                  QStringLiteral("Loading the message …")));
        return;
    }
    // The HTML part is shown when there is one. Nothing in it can be fetched:
    // this view has no network and no file access.
    if (!body->html.isEmpty()) bodyView_->setHtml(body->html);
    else bodyView_->setPlainText(body->text);

    for (const MailAttachment &attachment : body->attachments) {
        auto *item = new QListWidgetItem(
            attachment.name + QStringLiteral("  ·  ") + sizeLabel(attachment.size)
                + QStringLiteral("  ·  ") + attachment.mime,
            attachments_
        );
        item->setData(Qt::UserRole, attachment.id);
        item->setData(Qt::UserRole + 1, attachment.name);
    }
}

QStringList MailPanel::selectedMessageIds() const {
    QStringList ids;
    for (const QListWidgetItem *item : messages_->selectedItems())
        ids.append(item->data(Qt::UserRole).toString());
    return ids;
}

void MailPanel::activateFolder() {
    QTreeWidgetItem *item = folders_->currentItem();
    if (!item) return;
    const QString accountId = item->data(0, accountRole).toString();
    const QString folder = item->data(0, folderRole).toString();
    if (accountId.isEmpty() || folder.isEmpty()) return;
    if (accountId == store_.selectedAccount() && folder == store_.selectedFolder()) return;
    store_.selectFolder(accountId, folder, [this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::activateMessage() {
    QListWidgetItem *item = messages_->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;
    store_.openMessage(id, [this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::reload() {
    store_.refresh([this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::loadMore() {
    store_.loadMore([this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::reply() {
    const std::optional<MailMessage> message = store_.message(store_.selectedMessage());
    if (!message) return;
    store_.beginReply(*message);
    composeText_->setFocus();
}

void MailPanel::deleteSelected() {
    const QStringList ids = selectedMessageIds();
    if (ids.isEmpty()) return;
    // Deleting moves messages to the provider's Trash, which is worth saying
    // before it happens.
    const auto answer = QMessageBox::question(
        this,
        L(QStringLiteral("Nachrichten löschen?"), QStringLiteral("Delete messages?")),
        L(QStringLiteral("Die Auswahl wird beim Anbieter in den Papierkorb verschoben."),
          QStringLiteral("The selection is moved to the provider's Trash folder."))
            + QStringLiteral("\n\n") + QString::number(ids.size()) + QStringLiteral(" ")
            + L(QStringLiteral("Nachricht(en)"), QStringLiteral("message(s)")),
        QMessageBox::Cancel | QMessageBox::Ok,
        QMessageBox::Cancel
    );
    if (answer != QMessageBox::Ok) return;
    store_.deleteMessages(ids, [this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::moveSelected() {
    const QStringList ids = selectedMessageIds();
    if (ids.isEmpty()) return;
    QStringList targets;
    for (const MailFolder &folder : store_.folders(store_.selectedAccount())) {
        if (!folder.selectable || folder.path == store_.selectedFolder()) continue;
        targets.append(folder.path);
    }
    if (targets.isEmpty()) {
        setStatus(L(QStringLiteral("Der Anbieter meldet keinen anderen Ordner."),
                    QStringLiteral("The provider reports no other folder.")), true);
        return;
    }
    bool accepted = false;
    const QString target = QInputDialog::getItem(
        this,
        L(QStringLiteral("Verschieben"), QStringLiteral("Move")),
        L(QStringLiteral("Zielordner"), QStringLiteral("Destination folder")),
        targets, 0, false, &accepted
    );
    if (!accepted || target.isEmpty()) return;
    store_.moveMessages(ids, target, [this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::sendDraft() {
    store_.sendDraft([this](QString problem) {
        if (!problem.isEmpty()) setStatus(problem, true);
    });
}

void MailPanel::discardDraft() {
    store_.clearDraft();
}

void MailPanel::saveAttachment() {
    QListWidgetItem *item = attachments_->currentItem();
    if (!item) return;
    const QString attachmentId = item->data(Qt::UserRole).toString();
    const QString name = item->data(Qt::UserRole + 1).toString();
    // Only the file name is suggested: a name the sender chose must not decide
    // which directory the file lands in.
    const QString suggested = QFileInfo(name).fileName();
    const QString path = savePathChooser_(suggested.isEmpty() ? QStringLiteral("anhang") : suggested);
    if (path.isEmpty()) return;
    store_.readAttachment(store_.selectedMessage(), attachmentId,
                          [this, path](QString problem, QByteArray data) {
        if (!problem.isEmpty()) {
            setStatus(problem, true);
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            setStatus(L(QStringLiteral("Der Anhang konnte nicht gespeichert werden."),
                        QStringLiteral("The attachment could not be saved.")), true);
            return;
        }
        file.write(data);
        file.close();
        setStatus(L(QStringLiteral("Anhang gespeichert: "), QStringLiteral("Attachment saved: ")) + path,
                  false);
    });
}

void MailPanel::removeAccount() {
    QTreeWidgetItem *item = folders_->currentItem();
    const QString accountId = item ? item->data(0, accountRole).toString()
                                   : (store_.accounts().empty() ? QString() : store_.accounts().front().id);
    const std::optional<MailAccount> account = store_.account(accountId);
    if (!account) return;
    const auto answer = QMessageBox::question(
        this,
        L(QStringLiteral("Postfach entfernen?"), QStringLiteral("Remove mailbox?")),
        L(QStringLiteral("Das Postfach und sein Passwort werden aus dieser App entfernt. Beim Anbieter "
                         "wird nichts gelöscht."),
          QStringLiteral("The mailbox and its password are removed from this app. Nothing is deleted at "
                         "the provider."))
            + QStringLiteral("\n\n") + account->displayLabel(),
        QMessageBox::Cancel | QMessageBox::Ok,
        QMessageBox::Cancel
    );
    if (answer != QMessageBox::Ok) return;
    if (const QString problem = store_.disconnectAccount(accountId); !problem.isEmpty())
        setStatus(problem, true);
}

void MailPanel::showAccountForm() {
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("mailAccountDialog"));
    dialog->setWindowTitle(L(QStringLiteral("Postfach hinzufügen"), QStringLiteral("Add mailbox")));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);

    auto *form = new QFormLayout();
    auto *address = new QLineEdit(dialog);
    address->setObjectName(QStringLiteral("mailAccountAddressField"));
    address->setPlaceholderText(QStringLiteral("ich@example.com"));
    auto *provider = new QComboBox(dialog);
    provider->setObjectName(QStringLiteral("mailAccountProviderPicker"));
    provider->addItem(L(QStringLiteral("Automatisch suchen"), QStringLiteral("Look up automatically")));
    for (const QString &name : MailDiscovery::presetNames()) provider->addItem(name);
    auto *label = new QLineEdit(dialog);
    label->setObjectName(QStringLiteral("mailAccountLabelField"));
    auto *username = new QLineEdit(dialog);
    username->setObjectName(QStringLiteral("mailAccountUsernameField"));
    auto *imapHost = new QLineEdit(dialog);
    imapHost->setObjectName(QStringLiteral("mailAccountImapHostField"));
    auto *imapPort = new QLineEdit(QStringLiteral("993"), dialog);
    imapPort->setObjectName(QStringLiteral("mailAccountImapPortField"));
    auto *smtpHost = new QLineEdit(dialog);
    smtpHost->setObjectName(QStringLiteral("mailAccountSmtpHostField"));
    auto *smtpPort = new QLineEdit(QStringLiteral("465"), dialog);
    smtpPort->setObjectName(QStringLiteral("mailAccountSmtpPortField"));
    auto *security = new QComboBox(dialog);
    security->setObjectName(QStringLiteral("mailAccountSecurityPicker"));
    security->addItem(QStringLiteral("TLS"), QStringLiteral("tls"));
    security->addItem(QStringLiteral("STARTTLS"), QStringLiteral("starttls"));
    auto *password = new QLineEdit(dialog);
    password->setObjectName(QStringLiteral("mailAccountPasswordField"));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(L(QStringLiteral("Passwort oder App-Passwort"),
                                   QStringLiteral("Password or app password")));

    form->addRow(L(QStringLiteral("Adresse"), QStringLiteral("Address")), address);
    form->addRow(L(QStringLiteral("Anbieter"), QStringLiteral("Provider")), provider);
    form->addRow(L(QStringLiteral("Name"), QStringLiteral("Name")), label);
    form->addRow(L(QStringLiteral("Benutzername"), QStringLiteral("User name")), username);
    form->addRow(QStringLiteral("IMAP"), imapHost);
    form->addRow(L(QStringLiteral("IMAP-Port"), QStringLiteral("IMAP port")), imapPort);
    form->addRow(QStringLiteral("SMTP"), smtpHost);
    form->addRow(L(QStringLiteral("SMTP-Port"), QStringLiteral("SMTP port")), smtpPort);
    form->addRow(L(QStringLiteral("Versand"), QStringLiteral("Sending")), security);
    form->addRow(L(QStringLiteral("Passwort"), QStringLiteral("Password")), password);
    layout->addLayout(form);

    auto *explanation = new QLabel(dialog);
    explanation->setObjectName(QStringLiteral("mailAccountExplanation"));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto *buttons = new QHBoxLayout();
    auto *lookup = new QPushButton(L(QStringLiteral("Serverdaten suchen"),
                                     QStringLiteral("Find server settings")), dialog);
    lookup->setObjectName(QStringLiteral("mailAccountLookupButton"));
    auto *connectButton = new QPushButton(L(QStringLiteral("Verbinden"), QStringLiteral("Connect")), dialog);
    connectButton->setObjectName(QStringLiteral("mailAccountConnectButton"));
    auto *cancel = new QPushButton(L(QStringLiteral("Abbrechen"), QStringLiteral("Cancel")), dialog);
    cancel->setObjectName(QStringLiteral("mailAccountCancelButton"));
    buttons->addWidget(lookup);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(connectButton);
    layout->addLayout(buttons);

    // The id is created once, so a lookup and a connect describe the same mailbox.
    const QString accountId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto *oauthOnly = new bool(false);
    QObject::connect(dialog, &QDialog::destroyed, [oauthOnly] { delete oauthOnly; });

    const auto apply = [=](const MailDiscoveryResult &result) {
        if (!result.problem.isEmpty()) {
            explanation->setText(result.problem);
            return;
        }
        *oauthOnly = result.oauthOnly;
        if (label->text().trimmed().isEmpty()) label->setText(result.account.label);
        username->setText(result.account.username);
        imapHost->setText(result.account.imapHost);
        imapPort->setText(QString::number(result.account.imapPort));
        smtpHost->setText(result.account.smtpHost);
        smtpPort->setText(QString::number(result.account.smtpPort));
        security->setCurrentIndex(security->findData(result.account.smtpSecurity));
        explanation->setText(result.explanation + QStringLiteral("\n") + result.source);
        // A provider that only accepts OAuth cannot be connected with a password.
        connectButton->setEnabled(!result.oauthOnly);
    };

    QObject::connect(provider, &QComboBox::activated, dialog, [=](int index) {
        connectButton->setEnabled(true);
        if (index <= 0) return;
        apply(MailDiscovery::preset(provider->itemText(index), address->text().trimmed(), accountId));
    });
    QObject::connect(lookup, &QPushButton::clicked, dialog, [=] {
        explanation->setText(L(QStringLiteral("Serverdaten werden gesucht …"),
                               QStringLiteral("Looking for server settings …")));
        MailDiscovery::discover(address->text().trimmed(), accountId, dialog, apply);
    });
    QObject::connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    QObject::connect(connectButton, &QPushButton::clicked, dialog, [=, this] {
        MailAccount account;
        account.id = accountId;
        account.label = label->text().trimmed();
        account.address = address->text().trimmed();
        account.username = username->text().trimmed().isEmpty() ? account.address
                                                                : username->text().trimmed();
        account.imapHost = imapHost->text().trimmed();
        account.imapPort = imapPort->text().toInt();
        account.smtpHost = smtpHost->text().trimmed();
        account.smtpPort = smtpPort->text().toInt();
        account.smtpSecurity = security->currentData().toString();
        account.smtpUsername = account.username;
        if (*oauthOnly) {
            explanation->setText(L(
                QStringLiteral("Dieser Anbieter verlangt OAuth. Diese Anmeldung ist noch nicht eingerichtet."),
                QStringLiteral("This provider requires OAuth. That sign-in is not set up yet.")
            ));
            return;
        }
        connectButton->setEnabled(false);
        store_.connectAccount(account, password->text(), [=](QString problem) {
            if (!problem.isEmpty()) {
                explanation->setText(problem);
                connectButton->setEnabled(true);
                return;
            }
            // The typed password is not kept in the form once it is stored.
            password->clear();
            dialog->accept();
            reload();
        });
    });

    dialog->show();
}

void MailPanel::setStatus(const QString &text, bool isProblem) {
    status_->setText(text);
    status_->setStyleSheet(isProblem
        ? QStringLiteral("color:#ff541c;font:10px monospace")
        : QStringLiteral("font:10px monospace"));
}

} // namespace yobro::spike
