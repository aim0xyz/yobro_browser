#include "spike/SpaceChatPanel.hpp"

#include "spike/Localization.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace yobro::spike {
namespace {

/// The speaker label in front of a line, so the timeline reads like a chat.
QString prefixFor(const QString &kind) {
    if (kind == QStringLiteral("user")) return L(QStringLiteral("Du"), QStringLiteral("You")) + QStringLiteral(" · ");
    if (kind == QStringLiteral("assistant"))
        return L(QStringLiteral("Assistent"), QStringLiteral("Assistant")) + QStringLiteral(" · ");
    if (kind == QStringLiteral("context"))
        return L(QStringLiteral("Kontext"), QStringLiteral("Context")) + QStringLiteral(" · ");
    if (kind == QStringLiteral("error"))
        return L(QStringLiteral("Fehler"), QStringLiteral("Error")) + QStringLiteral(" · ");
    return QStringLiteral("· ");
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
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(7);

    auto *header = new QHBoxLayout();
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("spaceChatHeading"));
    heading_->setStyleSheet(QStringLiteral("font:700 11px monospace"));
    newChatButton_ = new QPushButton(L(QStringLiteral("Neuer Chat"), QStringLiteral("New chat")), this);
    newChatButton_->setObjectName(QStringLiteral("spaceChatNewButton"));
    connectionButton_ = new QPushButton(L(QStringLiteral("Verbindung"), QStringLiteral("Connection")), this);
    connectionButton_->setObjectName(QStringLiteral("spaceChatConnectionButton"));
    connectionButton_->setCheckable(true);
    header->addWidget(heading_);
    header->addStretch();
    header->addWidget(newChatButton_);
    header->addWidget(connectionButton_);
    layout->addLayout(header);

    connectionPane_ = new QWidget(this);
    connectionPane_->setObjectName(QStringLiteral("spaceChatConnectionPane"));
    auto *connectionLayout = new QVBoxLayout(connectionPane_);
    connectionLayout->setContentsMargins(0, 0, 0, 0);
    connectionLayout->setSpacing(5);

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
    // The key is never shown and never written to the conversation file.
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
    layout->addWidget(connectionPane_);
    connectionPane_->hide();

    timeline_ = new QListWidget(this);
    timeline_->setObjectName(QStringLiteral("spaceChatTimeline"));
    timeline_->setWordWrap(true);
    timeline_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(timeline_, 1);

    input_ = new QPlainTextEdit(this);
    input_->setObjectName(QStringLiteral("spaceChatInput"));
    input_->setPlaceholderText(L(
        QStringLiteral("Frage zu diesem Space…"),
        QStringLiteral("Ask about this Space…")
    ));
    input_->setMaximumHeight(90);
    layout->addWidget(input_);

    auto *footer = new QHBoxLayout();
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("spaceChatStatus"));
    status_->setWordWrap(true);
    stopButton_ = new QPushButton(L(QStringLiteral("Anhalten"), QStringLiteral("Stop")), this);
    stopButton_->setObjectName(QStringLiteral("spaceChatStopButton"));
    sendButton_ = new QPushButton(L(QStringLiteral("Senden"), QStringLiteral("Send")), this);
    sendButton_->setObjectName(QStringLiteral("spaceChatSendButton"));
    footer->addWidget(status_, 1);
    footer->addWidget(stopButton_);
    footer->addWidget(sendButton_);
    layout->addLayout(footer);

    QObject::connect(sendButton_, &QPushButton::clicked, this, [this] { send(); });
    QObject::connect(stopButton_, &QPushButton::clicked, this, [this] { runner_.cancel(); });
    QObject::connect(newChatButton_, &QPushButton::clicked, this, [this] { startNewChat(); });
    QObject::connect(connectionButton_, &QPushButton::toggled, this, [this](bool visible) {
        showConnection(visible);
    });
    QObject::connect(saveButton, &QPushButton::clicked, this, [this] { saveConnection(); });
    QObject::connect(forgetButton, &QPushButton::clicked, this, [this] { forgetKey(); });
    QObject::connect(providerPicker_, &QComboBox::activated, this, [this](int index) {
        applyProviderPreset(index);
    });
    // The draft belongs to the Space, so every keystroke goes to the store.
    QObject::connect(input_, &QPlainTextEdit::textChanged, this, [this] {
        if (shownSpace_.isEmpty()) return;
        store_.setDraft(shownSpace_, input_->toPlainText());
    });
    QObject::connect(timeline_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty() || !store_.toggleSaved(id)) return;
        refresh();
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
    heading_->setText(
        L(QStringLiteral("ASSISTENT · Space "), QStringLiteral("ASSISTANT · Space ")) + space
    );

    timeline_->clear();
    for (const SpaceChatEntry &entry : store_.entries(space)) {
        auto *item = new QListWidgetItem(
            (entry.saved ? QStringLiteral("★ ") : QString())
            + prefixFor(entry.kind) + entry.text,
            timeline_
        );
        item->setData(Qt::UserRole, entry.id);
        if (entry.kind == QStringLiteral("error")) item->setForeground(Qt::red);
    }
    if (timeline_->count() > 0) timeline_->scrollToBottom();

    if (shownSpace_ != space) {
        shownSpace_ = space;
        const QString draft = store_.draft(space);
        if (input_->toPlainText() != draft) input_->setPlainText(draft);
    }

    const ChatConnection connection = store_.connection();
    if (endpointField_->text() != connection.endpoint) endpointField_->setText(connection.endpoint);
    if (modelPicker_->currentText() != connection.model) modelPicker_->setCurrentText(connection.model);
    const ChatProvider provider = ChatProviderPresets::matching(connection.endpoint);
    const std::vector<ChatProvider> providers = ChatProviderPresets::all();
    for (std::size_t index = 0; index < providers.size(); ++index)
        if (providers[index] == provider) providerPicker_->setCurrentIndex(static_cast<int>(index));

    const bool busy = runner_.running();
    // Sending is blocked while a run is active, and while the conversation file
    // is damaged, because nothing may be written in that state.
    sendButton_->setEnabled(!busy && !store_.loadFailed() && host_.agentAllowed());
    stopButton_->setEnabled(busy);
    input_->setReadOnly(store_.loadFailed());
    if (busy) setStatus(L(QStringLiteral("Der Assistent arbeitet…"), QStringLiteral("The assistant is working…")), false);
    else if (store_.loadFailed()) setStatus(store_.problem(), true);
}

void SpaceChatPanel::send() {
    store_.setDraft(host_.space(), input_->toPlainText());
    setStatus({}, false);
    runner_.send(host_);
    // A refused send leaves the draft alone; a started one cleared it.
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
        // A custom endpoint is typed in, so nothing is overwritten here.
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
    // The field is cleared once the key is in the Keychain, so it is not left
    // sitting in the window.
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
    status_->setText(text);
    status_->setStyleSheet(isProblem
        ? QStringLiteral("color:#ff541c;font:10px monospace")
        : QStringLiteral("font:10px monospace"));
}

} // namespace yobro::spike
