#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

// MARK: - The assistant's view of this window

QString SpikeWindow::space() const {
    return activeSpace_;
}

bool SpikeWindow::agentAllowed() const {
    // The same two switches the WebKit build checks: an inactive profile or a
    // disabled agent stops the assistant, mid-run as well as before it starts.
    const controller::ProtocolHostState state = session_.protocolState();
    return state.profileActive && state.agentEnabled;
}

QJsonArray SpikeWindow::inventory() const {
    QJsonArray list;
    for (const NoteTab &note : notes_) {
        const auto workspace = workspaceTabs_.find(note.id.toStdString());
        if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), note.id);
        item.insert(QStringLiteral("kind"), QStringLiteral("NOTE"));
        item.insert(QStringLiteral("title"), note.editor->displayTitle());
        item.insert(QStringLiteral("url"), QString());
        list.append(item);
    }
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        const auto state = page->state();
        const auto workspace = workspaceTabs_.find(state.id);
        if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) continue;
        // A private tab is not listed at all: its address must not travel to a
        // model, let alone its content.
        if (page->profile()->isOffTheRecord()) continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), QString::fromStdString(state.id));
        item.insert(QStringLiteral("kind"), QStringLiteral("TAB"));
        item.insert(QStringLiteral("title"), QString::fromStdString(state.title));
        item.insert(QStringLiteral("url"), QString::fromStdString(state.url));
        list.append(item);
    }
    return list;
}

QJsonArray SpikeWindow::listNotes() const {
    QJsonArray list;
    for (const NoteTab &note : notes_) {
        const auto workspace = workspaceTabs_.find(note.id.toStdString());
        if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) continue;
        QJsonObject item;
        item.insert(QStringLiteral("id"), note.id);
        item.insert(QStringLiteral("title"), note.editor->displayTitle());
        list.append(item);
    }
    return list;
}

QString SpikeWindow::createNote(const QString &title, const QString &content) {
    NoteEditor *editor = newNote(title);
    if (!editor) return {};
    // Model output is text, never markup.
    editor->setPlainText(content);
    const QString id = noteIdFor(editor);
    if (chatPanel_) chatPanel_->refresh();
    refreshWorkspaceSidebar();
    saveSession();
    return id;
}

std::optional<QJsonObject> SpikeWindow::readNote(const QString &id) const {
    const auto workspace = workspaceTabs_.find(id.toStdString());
    if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) return std::nullopt;
    NoteEditor *editor = noteEditorFor(id);
    if (!editor) return std::nullopt;
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), id);
    payload.insert(QStringLiteral("title"), editor->displayTitle());
    payload.insert(QStringLiteral("content"), editor->plainText());
    return payload;
}

bool SpikeWindow::writeNote(const QString &id, const QString &content, const QString &mode) {
    const auto workspace = workspaceTabs_.find(id.toStdString());
    if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) return false;
    NoteEditor *editor = noteEditorFor(id);
    if (!editor) return false;
    const QString existing = editor->plainText();
    // Appending is the default so a careless call cannot wipe out what the user
    // wrote; replacing has to be asked for explicitly.
    editor->setPlainText(mode == QStringLiteral("replace") || existing.isEmpty()
        ? content
        : existing + QLatin1Char('\n') + content);
    const int index = tabs_->indexOf(editor);
    if (index >= 0) tabs_->setTabText(index, editor->displayTitle());
    refreshWorkspaceSidebar();
    saveSession();
    return true;
}

void SpikeWindow::readTab(const QString &id, std::function<void(std::optional<QJsonObject>)> done) {
    const auto workspace = workspaceTabs_.find(id.toStdString());
    if (workspace == workspaceTabs_.end() || workspace->second.space != activeSpace_) {
        done(std::nullopt);
        return;
    }
    qtwebengine::QtBrowserPage *source = nullptr;
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        if (page->state().id == id.toStdString()) source = page;
    }
    if (!source || source->profile()->isOffTheRecord()) {
        done(std::nullopt);
        return;
    }
    const QString url = QString::fromStdString(source->state().url);
    if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) {
        done(std::nullopt);
        return;
    }

    // The page is loaded again in an agent-owned tab instead of being read out
    // of the user's tab, so the assistant never runs inside a session the user
    // is signed in to.
    auto *agent = activeAgentPage(true);
    if (!agent) {
        done(std::nullopt);
        return;
    }
    auto subscription = std::make_shared<std::unique_ptr<engine::PageEventSubscription>>();
    auto answered = std::make_shared<bool>(false);
    const auto answer = [subscription, answered, done](std::optional<QJsonObject> payload) {
        if (*answered) return;
        *answered = true;
        // The subscription is dropped after the current callback has returned,
        // never from inside it.
        QTimer::singleShot(0, [subscription] { subscription->reset(); });
        done(std::move(payload));
    };

    engine::PageEventHandlers handlers;
    handlers.navigationFinished = [agent, answer](const engine::NavigationResult &result) {
        if (result.error) {
            answer(std::nullopt);
            return;
        }
        agent->readAgentSnapshot([answer](std::string json, std::optional<engine::EngineError> error) {
            if (error) {
                answer(std::nullopt);
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json));
            answer(document.isObject() ? std::optional<QJsonObject>(document.object()) : std::nullopt);
        });
    };
    *subscription = agent->subscribe(handlers);
    // A page that never finishes loading must not leave the assistant hanging.
    QTimer::singleShot(30'000, this, [answer] { answer(std::nullopt); });
    (void)agent->navigate(url.toStdString());
    synchronizeSession();
}

void SpikeWindow::showMail() {
    if (!mailDialog_) {
        // The WebKit build shows mail as a full-width overlay. Here it is a
        // window of its own, which is how every other surface in this shell
        // works, so the browser stays usable next to it.
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("mailDialog"));
        dialog->setWindowTitle(QStringLiteral("YoBro Mail"));
        dialog->resize(1080, 720);
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        mailPanel_ = new MailPanel(mail_, dialog);
        layout->addWidget(mailPanel_);
        dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()));
        mailDialog_ = dialog;
        QObject::connect(&mail_, &MailStore::newMail, this,
                         [this](const QString &accountId, int count, const QString &sender,
                                const QString &subject) {
            const std::optional<MailAccount> account = mail_.account(accountId);
            const QString label = account ? account->displayLabel() : accountId;
            showStatus(count == 1
                ? L(QStringLiteral("Neue Mail in "), QStringLiteral("New mail in ")) + label
                      + QStringLiteral(": ") + sender + QStringLiteral(" · ") + subject
                : L(QStringLiteral("Neue Mail in "), QStringLiteral("New mail in ")) + label
                      + QStringLiteral(": ") + QString::number(count));
        });
    }
    mailDialog_->show();
    mailDialog_->raise();
    mailDialog_->activateWindow();
}

void SpikeWindow::readMail(int limit, std::function<void(std::optional<QJsonObject>)> done) {
    // The assistant works on the same mail screen the user sees, so opening it
    // is part of the answer rather than a side effect to hide.
    showMail();
    if (mail_.accounts().empty()) {
        done(std::nullopt);
        return;
    }
    mail_.refresh([this, limit, done](QString) {
        QJsonArray rows;
        for (const MailAccount &account : mail_.accounts()) {
            for (const MailMessage &message : mail_.messages(account.id, QStringLiteral("INBOX"))) {
                if (rows.size() >= limit) break;
                QJsonObject row;
                row.insert(QStringLiteral("id"), message.id());
                row.insert(QStringLiteral("subject"), message.subject);
                row.insert(QStringLiteral("sender"), message.sender);
                row.insert(QStringLiteral("date"),
                           QDateTime::fromSecsSinceEpoch(message.date).toString(Qt::ISODate));
                row.insert(QStringLiteral("unread"), message.unread);
                rows.append(row);
            }
        }
        QJsonArray problems;
        for (const MailAccount &account : mail_.accounts()) {
            const QString reported = mail_.error(account.id);
            if (!reported.isEmpty()) problems.append(reported);
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("source"), QStringLiteral("YoBro built-in mail"));
        payload.insert(QStringLiteral("messages"), rows);
        payload.insert(QStringLiteral("errors"), problems);
        done(payload);
    });
}

void SpikeWindow::readMailMessage(const QString &id, std::function<void(std::optional<QJsonObject>)> done) {
    const std::optional<MailMessage> target = mail_.message(id);
    if (!target) {
        done(std::nullopt);
        return;
    }
    showMail();
    mail_.openMessage(id, [this, id, done](QString problem) {
        const std::optional<MailMessage> target = mail_.message(id);
        const std::optional<MailBody> body = mail_.body(id);
        if (!problem.isEmpty() || !target || !body) {
            done(std::nullopt);
            return;
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("source"), QStringLiteral("YoBro built-in mail"));
        payload.insert(QStringLiteral("subject"), target->subject);
        payload.insert(QStringLiteral("sender"), target->sender);
        payload.insert(QStringLiteral("recipient"), target->recipient);
        payload.insert(QStringLiteral("date"),
                       QDateTime::fromSecsSinceEpoch(target->date).toString(Qt::ISODate));
        // Only the readable text goes to the model, never the HTML part.
        payload.insert(QStringLiteral("body"), body->text.left(SpaceChatProtocol::textLimit));
        done(payload);
    });
}

void SpikeWindow::requestOpenUrl(const QUrl &url, std::function<void(std::optional<QString>)> done) {
    if (!permissionSurfaceAllowed_) {
        done(std::nullopt);
        return;
    }
    QMessageBox box(this);
    box.setObjectName(QStringLiteral("spaceChatApprovalDialog"));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(L(QStringLiteral("Adresse öffnen?"), QStringLiteral("Open address?")));
    box.setText(L(
        QStringLiteral("Der Assistent möchte eine Seite in einem Agententab öffnen."),
        QStringLiteral("The assistant wants to open a page in an agent tab.")
    ));
    box.setInformativeText(url.toString());
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(L(QStringLiteral("Öffnen"), QStringLiteral("Open")));
    if (box.exec() != QMessageBox::Ok) {
        done(std::nullopt);
        return;
    }
    engine::BrowserPage &page = session_.newAgentTab();
    (void)page.navigate(url.toString().toStdString());
    synchronizeSession();
    done(QString::fromStdString(page.state().id));
}

} // namespace yobro::spike
