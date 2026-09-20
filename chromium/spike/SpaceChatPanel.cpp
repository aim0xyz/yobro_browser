#include "spike/SpaceChatPanel.hpp"
#include "spike/Theme.hpp"
#include "spike/Icons.hpp"
#include "spike/Localization.hpp"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace yobro::spike {
namespace {

class ChatMessageInput final : public QPlainTextEdit {
public:
    explicit ChatMessageInput(QWidget *parent = nullptr) : QPlainTextEdit(parent) {}

    std::function<void()> onSubmit;

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
            e->accept();
            if (onSubmit) onSubmit();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
};

QWidget *createMessageBubble(
    const SpaceChatEntry &entry,
    const ThemePalette &colors,
    QWidget *parent,
    const std::function<void(const QString &)> &onCopy
) {
    auto *row = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 2, 0, 2);
    rowLayout->setSpacing(0);

    auto *card = new QWidget(row);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(6);

    const bool isUser = (entry.kind == QStringLiteral("user"));
    const bool isAssistant = (entry.kind == QStringLiteral("assistant"));
    const bool isContext = (entry.kind == QStringLiteral("context"));
    const bool isError = (entry.kind == QStringLiteral("error"));

    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    const QString senderTitle = isUser ? L(QStringLiteral("DU"), QStringLiteral("YOU"))
                                : isAssistant ? QStringLiteral("YoBro")
                                : isContext ? QStringLiteral("MAIL")
                                : isError ? L(QStringLiteral("FEHLER"), QStringLiteral("ERROR"))
                                : L(QStringLiteral("AKTION"), QStringLiteral("ACTION"));

    auto *senderLabel = new QLabel(senderTitle, card);
    senderLabel->setStyleSheet(QStringLiteral(
        "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "font-size:9px;font-weight:700;color:%1;"
    ).arg(isUser || isAssistant ? colors.moss : colors.textMuted));
    headerLayout->addWidget(senderLabel);
    headerLayout->addStretch();

    if (!entry.date.isEmpty()) {
        const QDateTime dt = QDateTime::fromString(entry.date, Qt::ISODate);
        if (dt.isValid()) {
            auto *timeLabel = new QLabel(dt.toLocalTime().toString(QStringLiteral("hh:mm")), card);
            timeLabel->setStyleSheet(QStringLiteral("font-size:8px;color:%1;").arg(colors.textMuted));
            headerLayout->addWidget(timeLabel);
        }
    }
    cardLayout->addLayout(headerLayout);

    auto *bodyLabel = new QLabel(entry.text, card);
    bodyLabel->setWordWrap(true);
    bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bodyLabel->setStyleSheet(QStringLiteral(
        "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "font-size:12px;color:%1;line-height:1.4;"
    ).arg(isError ? colors.brandOrange : colors.ink));
    cardLayout->addWidget(bodyLabel);

    if (isAssistant) {
        auto *copyBtn = new QPushButton(L(QStringLiteral("Antwort kopieren"), QStringLiteral("Copy response")), card);
        copyBtn->setCursor(Qt::PointingHandCursor);
        copyBtn->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;border:0;border-radius:6px;font-size:10px;font-weight:600;color:%2;padding:4px 8px;}"
            "QPushButton:hover{background:%3;color:%4;}"
        ).arg(colors.hoverWash, colors.textMuted, colors.pressedWash, colors.ink));
        QObject::connect(copyBtn, &QPushButton::clicked, [entry, copyBtn, onCopy] {
            onCopy(entry.text);
            copyBtn->setText(L(QStringLiteral("Kopiert!"), QStringLiteral("Copied!")));
            QTimer::singleShot(1500, copyBtn, [copyBtn] {
                copyBtn->setText(L(QStringLiteral("Antwort kopieren"), QStringLiteral("Copy response")));
            });
        });
        cardLayout->addWidget(copyBtn, 0, Qt::AlignLeft);
    }

    if (isUser) {
        card->setStyleSheet(QStringLiteral("background:%1;border-radius:13px;").arg(colors.selectionWash));
        rowLayout->addStretch(1);
        rowLayout->addWidget(card, 5);
    } else {
        card->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;border-radius:13px;")
            .arg(colors.surface, colors.borderHairline));
        rowLayout->addWidget(card, 5);
        rowLayout->addStretch(1);
    }
    return row;
}

} // namespace

SpaceChatPanel::SpaceChatPanel(
    SpaceChatStore &store,
    SpaceChatRunner &runner,
    SpaceChatHost &host,
    QWidget *parent
)
    : QWidget(parent), store_(store), runner_(runner), host_(host) {
    setObjectName(QStringLiteral("spaceChatPanel"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- Header ---
    auto *headerWidget = new QWidget(this);
    headerWidget->setObjectName(QStringLiteral("spaceChatHeader"));
    auto *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(12, 16, 12, 10);
    headerLayout->setSpacing(6);

    auto *titleBlock = new QVBoxLayout();
    titleBlock->setContentsMargins(0, 0, 0, 0);
    titleBlock->setSpacing(1);
    subheading_ = new QLabel(QStringLiteral("YoBro AGENT"), headerWidget);
    subheading_->setObjectName(QStringLiteral("spaceChatAgentLabel"));
    heading_ = new QLabel(headerWidget);
    heading_->setObjectName(QStringLiteral("spaceChatHeading"));
    titleBlock->addWidget(subheading_);
    titleBlock->addWidget(heading_);
    headerLayout->addLayout(titleBlock);
    headerLayout->addStretch();

    statusPill_ = new QWidget(headerWidget);
    statusPill_->setObjectName(QStringLiteral("spaceChatStatusPill"));
    auto *statusPillLayout = new QHBoxLayout(statusPill_);
    statusPillLayout->setContentsMargins(6, 2, 6, 2);
    statusPillLayout->setSpacing(4);
    statusDot_ = new QLabel(QStringLiteral("●"), statusPill_);
    statusPillLabel_ = new QLabel(L(QStringLiteral("Bereit"), QStringLiteral("Ready")), statusPill_);
    statusPillLayout->addWidget(statusDot_);
    statusPillLayout->addWidget(statusPillLabel_);
    headerLayout->addWidget(statusPill_);

    newChatButton_ = new QPushButton(headerWidget);
    newChatButton_->setObjectName(QStringLiteral("spaceChatCircleButton"));
    newChatButton_->setToolTip(L(QStringLiteral("Neuer Chat"), QStringLiteral("New chat")));
    installIcon(newChatButton_, QStringLiteral("file-plus"), IconRole::normal);

    connectionButton_ = new QPushButton(headerWidget);
    connectionButton_->setObjectName(QStringLiteral("spaceChatCircleButton"));
    connectionButton_->setToolTip(L(QStringLiteral("Modell verbinden"), QStringLiteral("Connect model")));
    connectionButton_->setCheckable(true);
    installIcon(connectionButton_, QStringLiteral("settings"), IconRole::normal);

    closeButton_ = new QPushButton(headerWidget);
    closeButton_->setObjectName(QStringLiteral("spaceChatCircleButton"));
    closeButton_->setToolTip(L(QStringLiteral("Assistent schließen"), QStringLiteral("Close assistant")));
    installIcon(closeButton_, QStringLiteral("x"), IconRole::normal);

    headerLayout->addWidget(newChatButton_);
    headerLayout->addWidget(connectionButton_);
    headerLayout->addWidget(closeButton_);
    layout->addWidget(headerWidget);

    // --- Content Container ---
    auto *bodyWidget = new QWidget(this);
    auto *bodyLayout = new QVBoxLayout(bodyWidget);
    bodyLayout->setContentsMargins(12, 10, 12, 12);
    bodyLayout->setSpacing(10);

    // --- Connection Settings Pane ---
    connectionPane_ = new QWidget(bodyWidget);
    connectionPane_->setObjectName(QStringLiteral("spaceChatConnectionPane"));
    auto *connectionLayout = new QVBoxLayout(connectionPane_);
    connectionLayout->setContentsMargins(8, 8, 8, 8);
    connectionLayout->setSpacing(6);

    providerPicker_ = new QComboBox(connectionPane_);
    providerPicker_->setObjectName(QStringLiteral("spaceChatProviderPicker"));
    for (const ChatProvider provider : ChatProviderPresets::all())
        providerPicker_->addItem(ChatProviderPresets::title(provider));
    connectionLayout->addWidget(providerPicker_);

    endpointField_ = new QLineEdit(connectionPane_);
    endpointField_->setObjectName(QStringLiteral("spaceChatEndpointField"));
    endpointField_->setPlaceholderText(QStringLiteral("https://…/v1/chat/completions"));
    connectionLayout->addWidget(endpointField_);

    modelPicker_ = new QComboBox(connectionPane_);
    modelPicker_->setObjectName(QStringLiteral("spaceChatModelPicker"));
    modelPicker_->setEditable(true);
    connectionLayout->addWidget(modelPicker_);

    keyField_ = new QLineEdit(connectionPane_);
    keyField_->setObjectName(QStringLiteral("spaceChatKeyField"));
    keyField_->setEchoMode(QLineEdit::Password);
    keyField_->setPlaceholderText(L(
        QStringLiteral("API-Schlüssel (bleibt im Schlüsselbund)"),
        QStringLiteral("API key (stays in the Keychain)")
    ));
    connectionLayout->addWidget(keyField_);

    auto *connectionButtons = new QHBoxLayout();
    auto *saveButton = new QPushButton(L(QStringLiteral("Speichern"), QStringLiteral("Save")), connectionPane_);
    saveButton->setObjectName(QStringLiteral("spaceChatSaveButton"));
    auto *forgetButton = new QPushButton(
        L(QStringLiteral("Schlüssel entfernen"), QStringLiteral("Remove key")), connectionPane_);
    forgetButton->setObjectName(QStringLiteral("spaceChatForgetButton"));
    connectionButtons->addWidget(saveButton);
    connectionButtons->addWidget(forgetButton);
    connectionButtons->addStretch();
    connectionLayout->addLayout(connectionButtons);
    bodyLayout->addWidget(connectionPane_);
    connectionPane_->hide();

    // --- Timeline List ---
    timeline_ = new QListWidget(bodyWidget);
    timeline_->setObjectName(QStringLiteral("spaceChatTimeline"));
    timeline_->setSelectionMode(QAbstractItemView::NoSelection);
    timeline_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    timeline_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bodyLayout->addWidget(timeline_, 1);

    // --- Composer Card ---
    composerCard_ = new QWidget(bodyWidget);
    composerCard_->setObjectName(QStringLiteral("spaceChatComposerCard"));
    auto *composerLayout = new QVBoxLayout(composerCard_);
    composerLayout->setContentsMargins(10, 8, 10, 8);
    composerLayout->setSpacing(6);

    contextBanner_ = new QWidget(composerCard_);
    auto *contextBannerLayout = new QHBoxLayout(contextBanner_);
    contextBannerLayout->setContentsMargins(0, 0, 0, 0);
    contextBannerLayout->setSpacing(6);
    auto *mailIcon = new QLabel(QStringLiteral("✉"), contextBanner_);
    contextBannerLabel_ = new QLabel(L(QStringLiteral("Mail-Kontext angehängt"), QStringLiteral("Email context attached")), contextBanner_);
    auto *clearContextBtn = new QPushButton(QStringLiteral("✕"), contextBanner_);
    clearContextBtn->setCursor(Qt::PointingHandCursor);
    clearContextBtn->setStyleSheet(QStringLiteral("QPushButton{border:0;background:transparent;color:%1;font-size:10px;}")
        .arg(themePalette(currentAppearanceIsDark()).textMuted));
    contextBannerLayout->addWidget(mailIcon);
    contextBannerLayout->addWidget(contextBannerLabel_);
    contextBannerLayout->addStretch();
    contextBannerLayout->addWidget(clearContextBtn);
    composerLayout->addWidget(contextBanner_);
    contextBanner_->hide();

    auto *chatInput = new ChatMessageInput(composerCard_);
    input_ = chatInput;
    input_->setObjectName(QStringLiteral("spaceChatInput"));
    input_->setPlaceholderText(L(
        QStringLiteral("Nachricht an diesen Space …"),
        QStringLiteral("Message this Space …")
    ));
    input_->setMaximumHeight(70);
    composerLayout->addWidget(input_);

    auto *composerFooter = new QHBoxLayout();
    composerFooter->setContentsMargins(0, 0, 0, 0);
    composerFooter->setSpacing(6);

    auto *contextHint = new QLabel(QStringLiteral("◇ ") + L(QStringLiteral("Space-Kontext"), QStringLiteral("Space context")), composerCard_);
    contextHint->setStyleSheet(QStringLiteral("font-size:9px;color:%1;").arg(themePalette(currentAppearanceIsDark()).textMuted));
    composerFooter->addWidget(contextHint);
    composerFooter->addStretch();

    stopButton_ = new QPushButton(L(QStringLiteral("Stopp"), QStringLiteral("Stop")), composerCard_);
    stopButton_->setObjectName(QStringLiteral("spaceChatStopButton"));
    stopButton_->setCursor(Qt::PointingHandCursor);
    stopButton_->hide();

    sendButton_ = new QPushButton(QStringLiteral("↑"), composerCard_);
    sendButton_->setObjectName(QStringLiteral("spaceChatSendButton"));
    sendButton_->setCursor(Qt::PointingHandCursor);
    sendButton_->setToolTip(L(QStringLiteral("Senden (Enter)"), QStringLiteral("Send (Enter)")));

    composerFooter->addWidget(stopButton_);
    composerFooter->addWidget(sendButton_);
    composerLayout->addLayout(composerFooter);
    bodyLayout->addWidget(composerCard_);

    // --- Footers & Notice ---
    status_ = new QLabel(bodyWidget);
    status_->setObjectName(QStringLiteral("spaceChatStatus"));
    status_->setWordWrap(true);
    status_->hide();
    bodyLayout->addWidget(status_);

    noticeLabel_ = new QLabel(bodyWidget);
    noticeLabel_->setObjectName(QStringLiteral("spaceChatNotice"));
    noticeLabel_->setWordWrap(true);
    bodyLayout->addWidget(noticeLabel_);

    auto *disclosureLayout = new QVBoxLayout();
    disclosureLayout->setContentsMargins(0, 0, 0, 0);
    disclosureLayout->setSpacing(2);

    whatSentHeader_ = new QPushButton(QStringLiteral("▶ ") + L(QStringLiteral("Was wird gesendet?"), QStringLiteral("What is sent?")), bodyWidget);
    whatSentHeader_->setObjectName(QStringLiteral("spaceChatDisclosureHeader"));
    whatSentHeader_->setCursor(Qt::PointingHandCursor);
    whatSentDetail_ = new QLabel(bodyWidget);
    whatSentDetail_->setObjectName(QStringLiteral("spaceChatDisclosureContent"));
    whatSentDetail_->setWordWrap(true);
    whatSentDetail_->setText(L(
        QStringLiteral("Bei jeder Anfrage gehen bis zu 30 letzte Chat-Nachrichten, Tab-/Notiz-Metadaten und ausgewählter Mail-Kontext an das verbundene Modell."),
        QStringLiteral("Each request sends up to 30 recent chat messages, tab/note metadata, and selected email context to the connected model.")
    ));
    whatSentDetail_->hide();

    agentAccessHeader_ = new QPushButton(QStringLiteral("▶ ") + L(QStringLiteral("Browserzugriff für Agenten"), QStringLiteral("Browser access for agents")), bodyWidget);
    agentAccessHeader_->setObjectName(QStringLiteral("spaceChatDisclosureHeader"));
    agentAccessHeader_->setCursor(Qt::PointingHandCursor);
    agentAccessPane_ = new QWidget(bodyWidget);
    agentAccessPane_->setObjectName(QStringLiteral("spaceChatDisclosureContent"));
    auto *agentAccessLayout = new QVBoxLayout(agentAccessPane_);
    agentAccessLayout->setContentsMargins(0, 2, 0, 4);
    agentAccessLayout->setSpacing(4);
    auto *agentToggle = new QCheckBox(L(QStringLiteral("Agenten Zugriff erlauben"), QStringLiteral("Allow agent access")), agentAccessPane_);
    agentToggle->setChecked(host_.agentAllowed());
    agentAccessLayout->addWidget(agentToggle);
    agentAccessPane_->hide();

    disclosureLayout->addWidget(whatSentHeader_);
    disclosureLayout->addWidget(whatSentDetail_);
    disclosureLayout->addWidget(agentAccessHeader_);
    disclosureLayout->addWidget(agentAccessPane_);
    bodyLayout->addLayout(disclosureLayout);

    layout->addWidget(bodyWidget);

    // --- Connections ---
    QObject::connect(sendButton_, &QPushButton::clicked, this, [this] { send(); });
    chatInput->onSubmit = [this] { send(); };
    QObject::connect(chatInput, &QPlainTextEdit::textChanged, this, [this] { updateSendState(); });
    QObject::connect(stopButton_, &QPushButton::clicked, this, [this] { runner_.cancel(); });
    QObject::connect(newChatButton_, &QPushButton::clicked, this, [this] { startNewChat(); });
    QObject::connect(connectionButton_, &QPushButton::toggled, this, [this](bool visible) {
        showConnection(visible);
    });
    QObject::connect(closeButton_, &QPushButton::clicked, this, [this] {
        emit closeRequested();
    });
    QObject::connect(saveButton, &QPushButton::clicked, this, [this] { saveConnection(); });
    QObject::connect(forgetButton, &QPushButton::clicked, this, [this] { forgetKey(); });
    QObject::connect(providerPicker_, &QComboBox::activated, this, [this](int index) {
        applyProviderPreset(index);
    });
    QObject::connect(clearContextBtn, &QPushButton::clicked, this, [this] {
        if (!shownSpace_.isEmpty()) store_.setContext(shownSpace_, QString());
        refresh();
    });
    QObject::connect(whatSentHeader_, &QPushButton::clicked, this, [this] {
        const bool hidden = whatSentDetail_->isHidden();
        whatSentDetail_->setVisible(hidden);
        whatSentHeader_->setText((hidden ? QStringLiteral("▼ ") : QStringLiteral("▶ ")) + L(QStringLiteral("Was wird gesendet?"), QStringLiteral("What is sent?")));
    });
    QObject::connect(agentAccessHeader_, &QPushButton::clicked, this, [this] {
        const bool hidden = agentAccessPane_->isHidden();
        agentAccessPane_->setVisible(hidden);
        agentAccessHeader_->setText((hidden ? QStringLiteral("▼ ") : QStringLiteral("▶ ")) + L(QStringLiteral("Browserzugriff für Agenten"), QStringLiteral("Browser access for agents")));
    });
    QObject::connect(input_, &QPlainTextEdit::textChanged, this, [this] {
        if (shownSpace_.isEmpty()) return;
        store_.setDraft(shownSpace_, input_->toPlainText());
    });
    QObject::connect(&runner_, &SpaceChatRunner::changed, this, [this] { refresh(); });
    QObject::connect(&runner_, &SpaceChatRunner::failed, this, [this](const QString &message) {
        setStatus(message, true);
    });

    if (!store_.problem().isEmpty()) setStatus(store_.problem(), true);
    refresh();
}

void SpaceChatPanel::refresh() {
    const QString space = host_.space();
    heading_->setText(space);

    const ThemePalette &colors = themePalette(currentAppearanceIsDark());

    const ChatConnection connection = store_.connection();
    const bool isConfigured = !connection.endpoint.trimmed().isEmpty() && !connection.model.trimmed().isEmpty();
    const bool busy = runner_.running();

    statusDot_->setStyleSheet(QStringLiteral("font-size:10px;color:%1;").arg(
        busy ? colors.brandOrange : isConfigured ? colors.moss : colors.textMuted
    ));
    statusPillLabel_->setText(
        busy ? L(QStringLiteral("Läuft"), QStringLiteral("Working"))
        : isConfigured ? L(QStringLiteral("Bereit"), QStringLiteral("Ready"))
        : L(QStringLiteral("Setup"), QStringLiteral("Setup"))
    );

    const bool isLocal = connection.endpoint.contains(QStringLiteral("localhost")) || connection.endpoint.contains(QStringLiteral("127.0.0.1"));
    const QString providerTitle = ChatProviderPresets::title(ChatProviderPresets::matching(connection.endpoint));
    noticeLabel_->setText(isLocal
        ? L(QStringLiteral("Lokale AI: YoBro sendet Agent-Daten nur an deinen lokalen Server."),
            QStringLiteral("Local AI: YoBro sends agent data only to your local server."))
        : L(QStringLiteral("Cloud-AI: Chat und alles, was der Agent liest, gehen an %1.").arg(providerTitle),
            QStringLiteral("Cloud AI: Chat and everything the agent reads is sent to %1.").arg(providerTitle)));
    noticeLabel_->setObjectName(isLocal ? QStringLiteral("spaceChatNoticeLocal") : QStringLiteral("spaceChatNotice"));

    timeline_->clear();
    for (const SpaceChatEntry &entry : store_.entries(space)) {
        auto *item = new QListWidgetItem(timeline_);
        auto *bubble = createMessageBubble(entry, colors, timeline_, [](const QString &text) {
            if (auto *clipboard = QGuiApplication::clipboard()) clipboard->setText(text);
        });
        // The bubble is the visual; the item text keeps the entry readable for
        // screen readers and tests.
        item->setText(entry.text);
        item->setToolTip(entry.text);
        item->setSizeHint(bubble->sizeHint());
        timeline_->setItemWidget(item, bubble);
    }
    if (timeline_->count() > 0) timeline_->scrollToBottom();

    if (shownSpace_ != space) {
        shownSpace_ = space;
        const QString draft = store_.draft(space);
        if (input_->toPlainText() != draft) input_->setPlainText(draft);
    }

    if (endpointField_->text() != connection.endpoint) endpointField_->setText(connection.endpoint);
    if (modelPicker_->currentText() != connection.model) modelPicker_->setCurrentText(connection.model);
    const ChatProvider provider = ChatProviderPresets::matching(connection.endpoint);
    const std::vector<ChatProvider> providers = ChatProviderPresets::all();
    for (std::size_t index = 0; index < providers.size(); ++index)
        if (providers[index] == provider) providerPicker_->setCurrentIndex(static_cast<int>(index));

    updateSendState();

    if (busy) setStatus(L(QStringLiteral("Der Assistent arbeitet…"), QStringLiteral("The assistant is working…")), false);
    else if (store_.loadFailed()) setStatus(store_.problem(), true);
    else if (status_->isVisible()) status_->hide();
}

void SpaceChatPanel::updateSendState() {
    const bool busy = runner_.running();
    const bool canSend = !busy && !store_.loadFailed() && host_.agentAllowed()
        && !input_->toPlainText().trimmed().isEmpty();
    sendButton_->setEnabled(canSend);
    sendButton_->setVisible(!busy);
    stopButton_->setVisible(busy);
    input_->setReadOnly(store_.loadFailed());
}

void SpaceChatPanel::send() {
    const QString text = input_->toPlainText().trimmed();
    if (text.isEmpty() || runner_.running()) return;
    store_.setDraft(host_.space(), text);
    setStatus({}, false);
    runner_.send(host_);
    const QString draft = store_.draft(host_.space());
    if (input_->toPlainText() != draft) input_->setPlainText(draft);
    refresh();
}

void SpaceChatPanel::startNewChat() {
    runner_.cancel();
    store_.newChat(host_.space());
    input_->clear();
    setStatus({}, false);
    refresh();
}

void SpaceChatPanel::applyProviderPreset(int index) {
    const std::vector<ChatProvider> providers = ChatProviderPresets::all();
    if (index < 0 || static_cast<std::size_t>(index) >= providers.size()) return;
    const ChatProvider provider = providers[static_cast<std::size_t>(index)];
    if (provider == ChatProvider::custom) {
        setStatus(L(QStringLiteral("Endpunkt und Modell selbst eintragen."),
                    QStringLiteral("Enter the endpoint and model yourself.")), false);
        return;
    }
    store_.selectProvider(provider);
    modelPicker_->clear();
    if (provider == ChatProvider::anthropic) modelPicker_->addItems(ChatProviderPresets::anthropicModels());
    keyField_->clear();
    refresh();
    setStatus(store_.apiKey().isEmpty()
        ? L(QStringLiteral("Für diesen Anbieter ist kein Schlüssel gespeichert."),
            QStringLiteral("No key is stored for this provider."))
        : L(QStringLiteral("Gespeicherter Schlüssel gefunden."),
            QStringLiteral("Found a stored key.")), false);
}

void SpaceChatPanel::saveConnection() {
    ChatConnection connection;
    connection.endpoint = endpointField_->text().trimmed();
    connection.model = modelPicker_->currentText().trimmed();
    store_.setConnection(connection);
    if (!keyField_->text().isEmpty()) store_.setApiKey(keyField_->text());

    QString problem;
    if (!store_.saveConnection(&problem)) {
        setStatus(problem, true);
        return;
    }
    keyField_->clear();
    setStatus(L(QStringLiteral("Verbindung gespeichert."), QStringLiteral("Connection saved.")), false);
    refresh();
}

void SpaceChatPanel::forgetKey() {
    QString problem;
    if (!store_.forgetApiKey(&problem)) {
        setStatus(problem, true);
        return;
    }
    keyField_->clear();
    setStatus(L(QStringLiteral("Schlüssel entfernt."), QStringLiteral("Key removed.")), false);
    refresh();
}

void SpaceChatPanel::showConnection(bool visible) {
    connectionPane_->setVisible(visible);
}

void SpaceChatPanel::setStatus(const QString &text, bool isProblem) {
    if (text.isEmpty()) {
        status_->hide();
        return;
    }
    status_->setText(text);
    status_->setVisible(true);
    status_->setStyleSheet(isProblem
        ? QStringLiteral("color:%1;font-size:10px;").arg(themePalette(currentAppearanceIsDark()).brandOrange)
        : QStringLiteral("font-size:10px;color:%1;").arg(themePalette(currentAppearanceIsDark()).textMuted));
}

} // namespace yobro::spike
