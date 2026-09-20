#pragma once

#include "spike/SpaceChat.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace yobro::spike {

/// The assistant surface for one Space, mirroring `SpaceChatPanel.swift`.
///
/// The panel owns no state of its own: the conversation, the drafts and the
/// connection live in `SpaceChatStore`, and the window supplies the tools
/// through `SpaceChatHost`. Switching Space therefore only means asking the
/// panel to redraw.
class SpaceChatPanel final : public QWidget {
    Q_OBJECT

public:
    SpaceChatPanel(
        SpaceChatStore &store,
        SpaceChatRunner &runner,
        SpaceChatHost &host,
        QWidget *parent = nullptr
    );

    /// Redraws the timeline and the input for the host's current Space.
    void refresh();

signals:
    void closeRequested();

private:
    void send();
    void updateSendState();
    void startNewChat();
    void saveConnection();
    void forgetKey();
    void applyProviderPreset(int index);
    void showConnection(bool visible);
    void setStatus(const QString &text, bool isProblem);

    SpaceChatStore &store_;
    SpaceChatRunner &runner_;
    SpaceChatHost &host_;

    QLabel *subheading_ = nullptr;
    QLabel *heading_ = nullptr;
    QWidget *statusPill_ = nullptr;
    QLabel *statusDot_ = nullptr;
    QLabel *statusPillLabel_ = nullptr;
    QPushButton *newChatButton_ = nullptr;
    QPushButton *connectionButton_ = nullptr;
    QPushButton *closeButton_ = nullptr;

    QWidget *connectionPane_ = nullptr;
    QComboBox *providerPicker_ = nullptr;
    QLineEdit *endpointField_ = nullptr;
    QComboBox *modelPicker_ = nullptr;
    QLineEdit *keyField_ = nullptr;

    QListWidget *timeline_ = nullptr;
    QWidget *emptyState_ = nullptr;

    QWidget *composerCard_ = nullptr;
    QWidget *contextBanner_ = nullptr;
    QLabel *contextBannerLabel_ = nullptr;
    QPlainTextEdit *input_ = nullptr;
    QPushButton *sendButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QLabel *status_ = nullptr;

    QLabel *noticeLabel_ = nullptr;
    QPushButton *whatSentHeader_ = nullptr;
    QLabel *whatSentDetail_ = nullptr;
    QPushButton *agentAccessHeader_ = nullptr;
    QWidget *agentAccessPane_ = nullptr;

    /// The Space the timeline currently shows, so a redraw can tell whether the
    /// draft in the input still belongs to it.
    QString shownSpace_;
};

} // namespace yobro::spike
