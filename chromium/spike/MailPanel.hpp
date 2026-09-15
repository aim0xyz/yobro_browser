#pragma once

#include "spike/MailStore.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTreeWidget;

namespace yobro::spike {

/// The built-in mail client, mirroring `MailViews.swift`.
///
/// One difference is worth knowing: the WebKit build renders a message in a web
/// view and offers a switch for remote images. Here the message is rendered in a
/// `QTextBrowser`, which has no way to reach the network at all, so remote
/// images, tracking pixels and remote fonts never load. That is stricter than
/// the switch, which is why no switch is offered. The stored preference is kept
/// so the two builds' preference files stay compatible.
class MailPanel final : public QWidget {
    Q_OBJECT

public:
    /// Asks the user where to save a file and answers with the chosen path, or
    /// an empty string when the dialog was cancelled. Replaceable for tests.
    using SavePathChooser = std::function<QString(const QString &suggestedName)>;

    MailPanel(MailStore &store, QWidget *parent = nullptr);

    void setSavePathChooser(SavePathChooser chooser);
    /// Redraws everything from the store.
    void refresh();

private:
    void rebuildFolderTree();
    void rebuildMessageList();
    void showSelectedMessage();
    void activateFolder();
    void activateMessage();
    void reload();
    void loadMore();
    void reply();
    void deleteSelected();
    void moveSelected();
    void sendDraft();
    void discardDraft();
    void saveAttachment();
    void showAccountForm();
    void removeAccount();
    void setStatus(const QString &text, bool isProblem);
    [[nodiscard]] QStringList selectedMessageIds() const;

    MailStore &store_;
    SavePathChooser savePathChooser_;

    QTreeWidget *folders_ = nullptr;
    QListWidget *messages_ = nullptr;
    QTextBrowser *bodyView_ = nullptr;
    QListWidget *attachments_ = nullptr;
    QLabel *header_ = nullptr;
    QLabel *status_ = nullptr;
    QLineEdit *search_ = nullptr;
    QCheckBox *unreadOnly_ = nullptr;
    QCheckBox *notifications_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *loadMoreButton_ = nullptr;
    QPushButton *replyButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QPushButton *moveButton_ = nullptr;
    QPushButton *saveAttachmentButton_ = nullptr;
    QPushButton *addAccountButton_ = nullptr;
    QPushButton *removeAccountButton_ = nullptr;
    QComboBox *composeAccount_ = nullptr;
    QLineEdit *composeTo_ = nullptr;
    QLineEdit *composeSubject_ = nullptr;
    QPlainTextEdit *composeText_ = nullptr;
    QPushButton *sendButton_ = nullptr;
    QPushButton *discardButton_ = nullptr;
    /// Set while the panel writes into its own widgets, so the change handlers
    /// do not write back into the store and fight the redraw.
    bool updating_ = false;
};

} // namespace yobro::spike
