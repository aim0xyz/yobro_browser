// What is left of the window after the split: the page commands, the permission
// surface, appearance and ad blocking, the proxy check, the split view, the find
// bar and the quick switcher. Its other methods live next door, all in the same
// class, so nothing had to be made public to move them:
//
//   SpikeWindowLayout.cpp       constructor, destructor, menus, compact sidebar
//   SpikeWindowSession.cpp      loading, saving and synchronising the session
//   SpikeWindowDownloads.cpp    the download library window
//   SpikeWindowLibrary.cpp      history and bookmarks
//   SpikeWindowWorkspace.cpp    spaces, folders and the sidebar tree
//   SpikeWindowSettings.cpp     the settings sheet
//   SpikeWindowProfiles.cpp     profile list, renaming, switching
//   SpikeWindowCredentials.cpp  login autofill, suggestions, saved logins
//   SpikeWindowAssistant.cpp    the assistant's view of this window, mailbox
//   SpikeWindowExtensions.cpp   MV3 extensions and the Chrome Web Store
//   SpikeWindowImport.cpp       importing from WebKit and other browsers
//   SpikeWindowSync.cpp         building and applying a sync snapshot
//   SpikeWindowPasskeys.cpp     the passkey conversation
//   SpikeWindowOnboarding.cpp   first-run setup
//   SpikeWindowSupport.cpp      the helpers that used to be file-local
#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

// The helpers below were file-local before the window was split across
// several translation units. Call sites stay unqualified.
using namespace windowSupport;




void SpikeWindow::setFileRevealHandler(FileRevealHandler handler) {
    fileRevealHandler_ = handler ? std::move(handler) : FileRevealHandler(revealFileInPlatformShell);
}

void SpikeWindow::setPermissionSurfaceAllowed(bool allowed) {
    permissionSurfaceAllowed_ = allowed;
    session_.setPermissionSurfaceVisible(
        permissionSurfaceAllowed_
        && isVisible()
        && !testAttribute(Qt::WA_DontShowOnScreen)
    );
}

void SpikeWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    session_.setPermissionSurfaceVisible(
        permissionSurfaceAllowed_ && !testAttribute(Qt::WA_DontShowOnScreen)
    );
    // Only a window the user can actually see offers the setup. Harnesses run
    // with `WA_DontShowOnScreen` and call `showOnboardingIfNeeded()` themselves
    // when that is what they want to check.
    if (!testAttribute(Qt::WA_DontShowOnScreen)) showOnboardingIfNeeded();
}

bool SpikeWindow::showOnboardingIfNeeded() {
    // Once per window: hiding and showing it again must not bring the setup back.
    if (onboardingChecked_) return false;
    onboardingChecked_ = true;
    if (!permissionSurfaceAllowed_) return false;
    if (OnboardingProgress::isComplete(paths_.profile)) return false;
    showOnboarding();
    return true;
}

void SpikeWindow::hideEvent(QHideEvent *event) {
    session_.setPermissionSurfaceVisible(false);
    QMainWindow::hideEvent(event);
}

void SpikeWindow::closeEvent(QCloseEvent *event) {
    session_.setPermissionSurfaceVisible(false);
    saveSession();
    QMainWindow::closeEvent(event);
}

qtwebengine::QtBrowserPage *SpikeWindow::newUserTab(const QString &url, bool privatePage) {
    engine::BrowserPage &generic = session_.newUserTab(url.toStdString(), privatePage);
    auto *page = asQtPage(&generic);
    if (url.isEmpty())
        page->setHtml(diagnosticStartPage(privatePage));
    if (!privatePage)
        workspaceTabs_.try_emplace(
            page->state().id,
            WorkspaceTab{.space = activeSpace_, .order = nextWorkspaceOrder_++}
        );
    synchronizeSession();
    return page;
}

qtwebengine::QtBrowserPage *SpikeWindow::currentUserPage() const {
    QWidget *current = tabs_->currentWidget();
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        if (page->view() == current)
            return page;
    }
    return nullptr;
}

qtwebengine::QtBrowserPage *SpikeWindow::activeAgentPage(bool createIfMissing) {
    engine::BrowserPage *generic = session_.activeAgentPage();
    if (!generic && createIfMissing)
        generic = &session_.newAgentTab();
    return generic ? asQtPage(generic) : nullptr;
}

void SpikeWindow::captureClosedPublicTab(qtwebengine::QtBrowserPage *page) {
    if (!page || page->profile()->isOffTheRecord()) return;
    const auto state = page->state();
    const QString url = QString::fromStdString(state.url);
    if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) return;
    const auto workspace = workspaceTabs_.find(state.id);
    const WorkspaceTab metadata = workspace == workspaceTabs_.end()
        ? WorkspaceTab{.space = activeSpace_}
        : workspace->second;
    if (!spaces_.contains(metadata.space)) return;
    const bool validFolder = metadata.folder.isEmpty() || std::any_of(
        workspaceFolders_.cbegin(), workspaceFolders_.cend(), [&metadata](const WorkspaceFolder &folder) {
            return folder.id == metadata.folder && folder.space == metadata.space;
        }
    );
    closedTabs_.push_back({
        .url = url,
        .title = QString::fromStdString(state.title),
        .space = metadata.space,
        .folder = validFolder ? metadata.folder : QString(),
        .pinned = metadata.pinned,
    });
    if (closedTabs_.size() > 20) closedTabs_.erase(closedTabs_.begin());
    ++closedTabUndoToken_;
    updateClosedTabAction();
    const std::uint64_t undoToken = closedTabUndoToken_;
    QTimer::singleShot(6'000, this, [this, undoToken] {
        if (undoToken != closedTabUndoToken_ || !reopenClosedTabButton_) return;
        reopenClosedTabButton_->setText(L(QStringLiteral("Tab wieder öffnen")));
    });
}

void SpikeWindow::updateClosedTabAction() {
    if (!reopenClosedTabButton_) return;
    reopenClosedTabButton_->setEnabled(!closedTabs_.empty());
    if (closedTabs_.empty()) reopenClosedTabButton_->setText(L(QStringLiteral("Tab wieder öffnen")));
    else reopenClosedTabButton_->setText(L(QStringLiteral("Rückgängig")));
}

void SpikeWindow::reopenClosedTab() {
    if (closedTabs_.empty()) return;
    const ClosedTab closed = closedTabs_.back();
    closedTabs_.pop_back();
    activeSpace_ = closed.space;
    auto *page = newUserTab(closed.url);
    workspaceTabs_[page->state().id] = WorkspaceTab{
        .space = closed.space,
        .folder = closed.folder,
        .pinned = closed.pinned,
    };
    (void)session_.setActiveUserTab(page->state().id);
    switchSpace(closed.space);
    ++closedTabUndoToken_;
    updateClosedTabAction();
    status_->setText(L(QStringLiteral("Geschlossener Tab wiederhergestellt.")));
}

void SpikeWindow::closeUserTab(int index) {
    auto *widget = tabs_->widget(index);
    // A note is ours, not the engine's, so it takes the other route out.
    if (const QString noteId = noteIdFor(widget); !noteId.isEmpty()) {
        closeNote(noteId);
        return;
    }
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        if (page->view() == widget) {
            captureClosedPublicTab(page);
            const std::string pageId = page->state().id;
            (void)session_.closeTab(pageId);
            workspaceTabs_.erase(pageId);
            break;
        }
    }
    if (session_.userPages().empty()) newUserTab();
    saveSession();
    status_->setText(closedTabs_.empty() ? L(QStringLiteral("Bereit")) : L(QStringLiteral("Tab geschlossen. Rückgängig ist sechs Sekunden hervorgehoben.")));
}

void SpikeWindow::navigateCurrent() {
    auto *page = currentUserPage();
    if (!page)
        return;
    // A space that wants a proxy must not fall back to a direct connection.
    if (!proxyIsolationFailure_.isEmpty()) {
        status_->setText(proxyIsolationFailure_);
        return;
    }
    const QString input = address_->text().trimmed();
    if (input.isEmpty())
        return;
    QUrl target;
    if (input.contains(QLatin1Char(' ')) && !input.contains(QStringLiteral("://"))) {
        target = QUrl(QStringLiteral("https://duckduckgo.com/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), input);
        target.setQuery(query);
    } else {
        target = QUrl::fromUserInput(input);
    }
    if (!target.isValid() || (target.scheme() != QStringLiteral("http") && target.scheme() != QStringLiteral("https"))) {
        status_->setText(L(QStringLiteral("Nur gültige HTTP- und HTTPS-Adressen werden unterstützt."), QStringLiteral("Only valid HTTP and HTTPS addresses are supported.")));
        return;
    }
    (void)session_.navigateUserTab(page->state().id, target.toString().toStdString());
}

void SpikeWindow::sendCurrentToAgent() {
    auto *page = currentUserPage();
    if (!page)
        return;
    if (page->profile()->isOffTheRecord()) {
        status_->setText(L(QStringLiteral("Adressen aus privaten Tabs werden nicht in den dauerhaften Agentenkontext übernommen."), QStringLiteral("Private-tab URLs are not copied into the persistent agent context.")));
        return;
    }
    const QString url = QString::fromStdString(page->state().url);
    if (url.startsWith(QStringLiteral("http://")) || url.startsWith(QStringLiteral("https://"))) {
        auto *agent = activeAgentPage(true);
        (void)agent->navigate(url.toStdString());
        synchronizeSession();
    } else {
        status_->setText(L(QStringLiteral("Dieser Tab enthält keine Webadresse."), QStringLiteral("The current tab does not contain a web URL.")));
    }
}

void SpikeWindow::readAgentPage() {
    auto *page = activeAgentPage(false);
    if (!page) {
        agentOutput_->setPlainText(L(QStringLiteral("Es ist keine Agentenseite aktiv."), QStringLiteral("No agent page is active.")));
        return;
    }
    agentOutput_->setPlainText(L(QStringLiteral("Isolierte AgentBridge-Momentaufnahme wird gelesen…"), QStringLiteral("Reading isolated AgentBridge snapshot…")));
    page->readAgentSnapshot([this](std::string json, std::optional<engine::EngineError> error) {
        if (error) {
            agentOutput_->setPlainText(QString::fromStdString(error->message));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json));
        agentOutput_->setPlainText(QString::fromUtf8(document.toJson(QJsonDocument::Indented)));
    });
}

void SpikeWindow::installUnpackedExtension() {
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        L(QStringLiteral("Entpackte Manifest-V3-Erweiterung wählen"), QStringLiteral("Choose an unpacked Manifest V3 extension"))
    );
    if (directory.isEmpty())
        return;
    status_->setText(L(QStringLiteral("MV3-Erweiterung wird installiert…"), QStringLiteral("Installing MV3 extension…")));
    profile_->persistentProfile()->extensionManager()->installExtension(directory);
}

NoteEditor *SpikeWindow::newNote(const QString &title, const QString &html) {
    auto *editor = new NoteEditor(tabs_);
    const QString id = QStringLiteral("note-%1").arg(nextNoteNumber_++);
    editor->setTitle(title);
    if (!html.isEmpty()) editor->setHtml(html);
    notes_.push_back({id, editor});
    // Notes take part in the space, folder and order handling like pages do.
    workspaceTabs_.emplace(
        id.toStdString(),
        WorkspaceTab{.space = activeSpace_, .order = nextWorkspaceOrder_++}
    );
    const int index = tabs_->addTab(editor, editor->displayTitle());
    tabs_->setCurrentIndex(index);
    QObject::connect(editor, &NoteEditor::changed, this, [this, id] {
        // The title in the tab bar and the sidebar follows what is typed.
        if (NoteEditor *note = noteEditorFor(id)) {
            const int tabIndex = tabs_->indexOf(note);
            if (tabIndex >= 0) tabs_->setTabText(tabIndex, note->displayTitle());
        }
        refreshWorkspaceSidebar();
        saveSession();
    });
    editor->focusContent();
    refreshWorkspaceSidebar();
    saveSession();
    status_->setText(L(QStringLiteral("Neue Notiz erstellt."), QStringLiteral("New note created.")));
    return editor;
}

NoteEditor *SpikeWindow::noteEditorFor(const QString &id) const {
    for (const NoteTab &note : notes_) {
        if (note.id == id) return note.editor;
    }
    return nullptr;
}

QString SpikeWindow::noteIdFor(const QWidget *widget) const {
    for (const NoteTab &note : notes_) {
        if (note.editor == widget) return note.id;
    }
    return {};
}

void SpikeWindow::activateNote(const QString &id) {
    NoteEditor *editor = noteEditorFor(id);
    if (!editor) return;
    const auto workspace = workspaceTabs_.find(id.toStdString());
    if (workspace != workspaceTabs_.end() && workspace->second.space != activeSpace_)
        switchSpace(workspace->second.space);
    const int index = tabs_->indexOf(editor);
    if (index >= 0) tabs_->setCurrentIndex(index);
    editor->focusContent();
}

void SpikeWindow::closeNote(const QString &id) {
    const auto found = std::find_if(notes_.begin(), notes_.end(), [&id](const NoteTab &note) {
        return note.id == id;
    });
    if (found == notes_.end()) return;
    NoteEditor *editor = found->editor;
    notes_.erase(found);
    workspaceTabs_.erase(id.toStdString());
    const int index = tabs_->indexOf(editor);
    if (index >= 0) tabs_->removeTab(index);
    editor->deleteLater();
    refreshWorkspaceSidebar();
    saveSession();
    status_->setText(L(QStringLiteral("Notiz geschlossen."), QStringLiteral("Note closed.")));
}








void SpikeWindow::changeZoom(int direction) {
    auto *page = currentUserPage();
    if (!page) return;
    const std::string url = page->state().url;
    const std::optional<double> applied = direction == 0 ? zoom_.reset(url) : zoom_.step(direction, url);
    // A start page without a host has nothing to remember, so the view is
    // adjusted directly instead of dropping the request.
    const double factor = applied.value_or(
        direction == 0 ? PageZoomStore::standard : page->view()->zoomFactor()
    );
    page->view()->setZoomFactor(factor);
    status_->setText(QStringLiteral("Zoom %1 %").arg(static_cast<int>(factor * 100 + 0.5)));
}

void SpikeWindow::applyStoredZoom() {
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent) continue;
        auto *view = asQtPage(tab.page)->view();
        const double stored = zoom_.level(tab.state.url);
        // Comparing first avoids a needless relayout on every synchronisation.
        if (view->zoomFactor() != stored) view->setZoomFactor(stored);
    }
}

void SpikeWindow::printActivePage() {
    auto *page = currentUserPage();
    if (!page || page->state().url.empty()) {
        status_->setText(L(QStringLiteral("Diese Seite kann nicht gedruckt werden.")));
        return;
    }
    auto printer = std::make_shared<QPrinter>(QPrinter::HighResolution);
    QPrintDialog dialog(printer.get(), this);
    dialog.setWindowTitle(L(QStringLiteral("Seite drucken")));
    if (dialog.exec() != QDialog::Accepted) return;
    status_->setText(L(QStringLiteral("Seite wird gedruckt…")));
    QPointer<SpikeWindow> guard(this);
    // Chromium renders asynchronously; the printer must outlive that callback.
    page->view()->print(printer.get());
    QObject::connect(page->view(), &QWebEngineView::printFinished, this, [this, guard, printer](bool success) {
        if (!guard) return;
        status_->setText(success
            ? L(QStringLiteral("Seite gedruckt."))
            : L(QStringLiteral("Drucken wurde abgebrochen oder ist fehlgeschlagen.")));
    });
}

void SpikeWindow::selectVisibleTab(int index) {
    if (!tabs_ || tabs_->count() == 0) return;
    // ⌘9 jumps to the last tab, matching Safari and Chrome.
    const int target = index >= 8 ? tabs_->count() - 1 : index;
    if (target < 0 || target >= tabs_->count()) return;
    tabs_->setCurrentIndex(target);
}

void SpikeWindow::copyCurrentAddress() {
    auto *page = currentUserPage();
    if (!page || page->state().url.empty()) return;
    QGuiApplication::clipboard()->setText(QString::fromStdString(page->state().url));
    status_->setText(L(QStringLiteral("Adresse kopiert.")));
}





void SpikeWindow::synchronizePermissionPrompt() {
    const std::optional<controller::PermissionPrompt> prompt = session_.pendingPermission();
    if (!prompt) {
        if (permissionDialog_) {
            QObject::disconnect(permissionDialog_, nullptr, this, nullptr);
            permissionDialog_->close();
            permissionDialog_.clear();
            permissionDialogId_ = 0;
        }
        return;
    }
    if (permissionDialog_ && permissionDialogId_ == prompt->id)
        return;
    if (permissionDialog_) {
        QObject::disconnect(permissionDialog_, nullptr, this, nullptr);
        permissionDialog_->close();
        permissionDialog_.clear();
        permissionDialogId_ = 0;
    }

    const QUrl origin(QString::fromStdString(prompt->origin));
    const QString host = origin.host().isEmpty()
        ? L(QStringLiteral("Diese Seite"))
        : origin.host();
    const QString subject = permissionDevice(prompt->permission);
    const QString title = prompt->permission == engine::WebPermission::notifications
        ? L(QStringLiteral("%1 möchte dir Mitteilungen senden."), QStringLiteral("%1 wants to send you notifications."))
            .arg(host)
        : L(QStringLiteral("%1 möchte %2 verwenden."), QStringLiteral("%1 wants to use %2."))
            .arg(host, subject);
    const QString windowTitle = isMediaPermission(prompt->permission)
        ? L(QStringLiteral("Medienzugriff"))
        : L(QStringLiteral("Websitezugriff"), QStringLiteral("Website access"));
    auto *dialog = new QMessageBox(QMessageBox::Question, windowTitle, title, QMessageBox::NoButton, this);
    dialog->setObjectName(QStringLiteral("mediaPermissionPrompt"));
    const QString visitOnly = L(QStringLiteral("Die Freigabe gilt nur für diesen Seitenaufruf."));
    const QString caution = permissionCaution(prompt->permission);
    dialog->setInformativeText(caution.isEmpty() ? visitOnly : caution + QStringLiteral(" ") + visitOnly);
    auto *allow = dialog->addButton(L(QStringLiteral("Einmal erlauben")), QMessageBox::AcceptRole);
    allow->setObjectName(QStringLiteral("mediaPermissionAllowOnce"));
    auto *denyButton = dialog->addButton(L(QStringLiteral("Ablehnen")), QMessageBox::RejectRole);
    denyButton->setObjectName(QStringLiteral("mediaPermissionDeny"));
    dialog->setDefaultButton(denyButton);
    dialog->setEscapeButton(denyButton);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    permissionDialog_ = dialog;
    permissionDialogId_ = prompt->id;
    const std::uint64_t requestId = prompt->id;
    QObject::connect(dialog, &QMessageBox::finished, this, [this, dialog, allow, requestId](int) {
        const bool grant = dialog->clickedButton() == allow;
        if (permissionDialog_ == dialog) {
            permissionDialog_.clear();
            permissionDialogId_ = 0;
        }
        (void)session_.resolvePermission(
            requestId,
            grant ? engine::PermissionDecision::grant : engine::PermissionDecision::deny
        );
    });
    dialog->open();
}

void SpikeWindow::updateNavigationState() {
    auto *page = currentUserPage();
    const bool available = page != nullptr;
    back_->setEnabled(available && page->state().canGoBack);
    forward_->setEnabled(available && page->state().canGoForward);
    reload_->setEnabled(available);
    if (available && !address_->hasFocus())
        address_->setText(QString::fromStdString(page->state().url));
}

void SpikeWindow::applyTheme() {
    const bool dark = systemPrefersDark();
    setStyleSheet(windowStyleSheet(dark));
    // Open sheets are restyled too so a switch while running stays consistent.
    for (QPointer<QDialog> dialog : {downloadsDialog_, libraryDialog_, extensionsDialog_, importDialog_,
                                     quickSwitcherDialog_, settingsDialog_, passwordsDialog_}) {
        if (dialog) dialog->setStyleSheet(sheetStyleSheet(dark));
    }
}

void SpikeWindow::clearBrowsingData(int rangeIndex, bool includeHistory) {
    // Cookies, cache and visited links can only be cleared completely: Qt
    // WebEngine has no ranged deletion. The range therefore applies to the
    // history, which this profile stores itself.
    auto *profile = profile_->persistentProfile();
    profile->cookieStore()->deleteAllCookies();
    profile->clearHttpCache();
    profile->clearAllVisitedLinks();
    // Local storage and service workers can only be removed while no profile
    // holds them open, so the removal happens on the next start.
    privacy_.requestSiteDataClear();

    QString summary = L(
        QStringLiteral("Cookies, Zwischenspeicher und besuchte Links entfernt."),
        QStringLiteral("Cookies, cache and visited links removed.")
    );
    if (includeHistory) {
        std::string since;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (rangeIndex == 0) since = now.addSecs(-3600).toString(Qt::ISODate).toStdString();
        else if (rangeIndex == 1) since = now.addDays(-1).toString(Qt::ISODate).toStdString();
        else if (rangeIndex == 2) since = now.addDays(-7).toString(Qt::ISODate).toStdString();
        try {
            const std::size_t removed = library_.clearHistory(since);
            summary += L(
                QStringLiteral(" %1 Verlaufseinträge gelöscht.").arg(removed),
                QStringLiteral(" %1 history entries deleted.").arg(removed)
            );
            refreshLibrary();
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
            return;
        }
    }
    summary += L(
        QStringLiteral(" Websitedaten werden beim nächsten Start entfernt."),
        QStringLiteral(" Website data is removed on the next start.")
    );
    // Open pages are reloaded so they show the signed-out state instead of
    // looking as if the session were still valid.
    for (engine::BrowserPage *generic : session_.userPages()) {
        auto *page = asQtPage(generic);
        const std::string &url = page->state().url;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
            (void)session_.reloadUserTab(page->state().id);
    }
    status_->setText(summary);
}

bool SpikeWindow::systemPrefersDark() const {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

std::string SpikeWindow::currentHost() const {
    auto *page = currentUserPage();
    if (!page) return {};
    const QUrl url(QString::fromStdString(page->state().url));
    return url.host().toLower().toStdString();
}

void SpikeWindow::installWebAppearanceScript() {
    QString source;
    for (const QString &name : {QStringLiteral(":/yobro/DarkReader.js"), QStringLiteral(":/yobro/WebAppearance.js")}) {
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            status_->setText(L(QStringLiteral("Webseiten-Darstellung ist nicht verfügbar: Skriptressource fehlt.")));
            return;
        }
        source += QString::fromUtf8(file.readAll()) + QStringLiteral("\n");
    }
    source += QStringLiteral("window.__yobroAppearance?.configure(%1);")
        .arg(QString::fromStdString(appearance_.json(systemPrefersDark())));

    auto *scripts = profile_->persistentProfile()->scripts();
    const QString name = QStringLiteral("YOBRO WebAppearance");
    // Replacing the script keeps future documents in sync with the settings.
    for (const QWebEngineScript &existing : scripts->find(name)) scripts->remove(existing);
    QWebEngineScript script;
    script.setName(name);
    script.setSourceCode(source);
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    // A world of its own so neither page scripts nor the login script collide.
    script.setWorldId(QWebEngineScript::UserWorld + 1);
    // Subframes are converted too, matching the WebKit build.
    script.setRunsOnSubFrames(true);
    scripts->insert(script);
}

void SpikeWindow::applySpaceProxy() {
    proxyIsolationFailure_.clear();
    const auto stored = proxies_.config(activeSpace_);
    std::optional<SpaceProxyConfig> wanted;
    if (stored && stored->enabled) wanted = proxies_.resolved(activeSpace_, *stored);

    if (wanted) {
        if (const QString problem = wanted->validationProblem(); !problem.isEmpty()) {
            // Loading without the proxy would send the traffic out exactly the
            // way the setting promises to avoid, so nothing is loaded instead.
            proxyIsolationFailure_ = L(
                QStringLiteral("Der Proxy für „%1“ kann nicht verwendet werden: %2"),
                QStringLiteral("The proxy for “%1” cannot be used: %2")
            ).arg(activeSpace_, problem);
            status_->setText(proxyIsolationFailure_);
            return;
        }
    }

    // Qt WebEngine reads the proxy once at startup, so a space with a different
    // proxy needs a restart. Until then nothing may load unprotected.
    if (!SpaceProxyController::matchesApplied(wanted)) {
        proxyIsolationFailure_ = wanted
            ? L(QStringLiteral("Der Proxy für „%1“ gilt erst nach einem Neustart. Bis dahin wird nichts geladen."),
                QStringLiteral("The proxy for “%1” applies after a restart. Nothing loads until then."))
                  .arg(activeSpace_)
            : L(QStringLiteral("„%1“ soll ohne Proxy laufen, es ist aber noch einer aktiv (%2). "
                               "Ein Neustart hebt ihn auf."),
                QStringLiteral("“%1” should run without a proxy, but one is still active (%2). "
                               "A restart removes it."))
                  .arg(activeSpace_, SpaceProxyController::appliedLabel());
        status_->setText(proxyIsolationFailure_);
        return;
    }

    if (!wanted) return;
    QString message =
        L(QStringLiteral("Proxy für „%1“ aktiv: %2"), QStringLiteral("Proxy for “%1” active: %2"))
            .arg(activeSpace_, wanted->displayLabel());
    if (SpaceProxyController::hasUnenforceableRules(*wanted)) {
        message += QStringLiteral(" ") + L(
            QStringLiteral("Eigene Domainlisten wirken hier nicht: es geht mehr über den Proxy, nie weniger."),
            QStringLiteral("Custom domain lists have no effect here: more goes through the proxy, never less.")
        );
    }
    status_->setText(message);
}

void SpikeWindow::installAdBlockScripts() {
    const QString isolatedName = QStringLiteral("YOBRO AdBlocker");
    const QString pageName = QStringLiteral("YOBRO AdBlockerPage");
    // Private tabs get the same filters, matching the WebKit build, which
    // attaches them to every web view rather than to one store.
    std::vector<QWebEngineScriptCollection *> collections{
        profile_->persistentProfile()->scripts(),
        profile_->privateProfile()->scripts(),
    };
    for (QWebEngineScriptCollection *scripts : collections) {
        for (const QString &name : {isolatedName, pageName}) {
            for (const QWebEngineScript &existing : scripts->find(name)) scripts->remove(existing);
        }
    }
    if (!adBlock_.enabled()) return;

    const bool aggressive = adBlock_.aggressivePageFilters();
    const QString flags = QStringLiteral("%1, %2")
        .arg(adBlock_.enabled() ? QStringLiteral("true") : QStringLiteral("false"),
             aggressive ? QStringLiteral("true") : QStringLiteral("false"));

    QFile isolatedFile(QStringLiteral(":/yobro/AdBlocker.js"));
    if (!isolatedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        status_->setText(L(
            QStringLiteral("Werbefilter ist nicht verfügbar: Skriptressource fehlt."),
            QStringLiteral("The ad filter is unavailable: script resource is missing.")
        ));
        return;
    }
    QWebEngineScript isolated;
    isolated.setName(isolatedName);
    // There is no page-to-host channel here, so the script is configured inline
    // instead of waiting for a "ready" message as the WebKit build does.
    isolated.setSourceCode(
        QString::fromUtf8(isolatedFile.readAll())
        + QStringLiteral("\nwindow.__yobroAdBlock?.configure(%1);").arg(flags)
    );
    isolated.setInjectionPoint(QWebEngineScript::DocumentCreation);
    // UserWorld + 2 keeps the cosmetic filter apart from the agent bridge
    // (UserWorld) and the appearance engine (UserWorld + 1).
    isolated.setWorldId(QWebEngineScript::UserWorld + 2);
    for (QWebEngineScriptCollection *scripts : collections) scripts->insert(isolated);

    QFile pageFile(QStringLiteral(":/yobro/AdBlockerPage.js"));
    if (!pageFile.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QWebEngineScript page;
    page.setName(pageName);
    // This one has to share the page's own world: it replaces JSON.parse to hide
    // ad slots from the player, which only works inside the page context.
    page.setSourceCode(
        QString::fromUtf8(pageFile.readAll())
        + QStringLiteral("\nwindow.__yobroAdBlockPage?.setEnabled(%1);")
            .arg(aggressive ? QStringLiteral("true") : QStringLiteral("false"))
    );
    page.setInjectionPoint(QWebEngineScript::DocumentCreation);
    page.setWorldId(QWebEngineScript::MainWorld);
    for (QWebEngineScriptCollection *scripts : collections) scripts->insert(page);
}

void SpikeWindow::updateAdBlocking(bool reloadPages) {
    if (adBlockInterceptor_) adBlockInterceptor_->setEnabled(adBlock_.enabled());
    installAdBlockScripts();
    const bool aggressive = adBlock_.aggressivePageFilters();
    const QString configure = QStringLiteral("window.__yobroAdBlock?.configure(%1, %2);")
        .arg(adBlock_.enabled() ? QStringLiteral("true") : QStringLiteral("false"),
             aggressive ? QStringLiteral("true") : QStringLiteral("false"));
    const QString pageConfigure = QStringLiteral("window.__yobroAdBlockPage?.setEnabled(%1);")
        .arg(aggressive ? QStringLiteral("true") : QStringLiteral("false"));
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        auto *page = asQtPage(tab.page)->view()->page();
        page->runJavaScript(configure, QWebEngineScript::UserWorld + 2);
        page->runJavaScript(pageConfigure, QWebEngineScript::MainWorld);
    }
    if (!reloadPages) return;
    // Network rules only take effect on the next load, so open pages are
    // reloaded exactly as the WebKit build does after a switch.
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        const auto state = asQtPage(tab.page)->state();
        if (!state.url.empty()) (void)session_.reloadUserTab(state.id);
    }
}

void SpikeWindow::updateWebAppearance() {
    installWebAppearanceScript();
    const QString configure = QStringLiteral("window.__yobroAppearance?.configure(%1);")
        .arg(QString::fromStdString(appearance_.json(systemPrefersDark())));
    // Already loaded documents are reconfigured instead of reloaded.
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        auto *view = asQtPage(tab.page)->view();
        view->page()->runJavaScript(configure, QWebEngineScript::UserWorld + 1);
    }
    if (appearanceButton_) {
        const bool active = appearance_.enabled() && systemPrefersDark();
        appearanceButton_->setText(active ? QStringLiteral("◗") : QStringLiteral("◔"));
        appearanceButton_->setToolTip(active
            ? L(QStringLiteral("Webseiten-Darkmode aktiv"))
            : L(QStringLiteral("Webseiten-Darstellung")));
    }
}

void SpikeWindow::showAppearanceMenu() {
    QMenu menu(this);
    auto *heading = menu.addAction(systemPrefersDark()
        ? L(QStringLiteral("System ist im Dunkelmodus"))
        : L(QStringLiteral("System ist im Hellmodus")));
    heading->setEnabled(false);
    auto *toggle = menu.addAction(L(QStringLiteral("Helle Websites abdunkeln")));
    toggle->setObjectName(QStringLiteral("darkenWebsitesAction"));
    toggle->setCheckable(true);
    toggle->setChecked(appearance_.enabled());
    connect(toggle, &QAction::triggered, this, [this](bool checked) {
        appearance_.setEnabled(checked);
        updateWebAppearance();
        status_->setText(checked
            ? L(QStringLiteral("Helle Websites werden abgedunkelt, sobald das System dunkel ist."))
            : L(QStringLiteral("Websites behalten ihre eigenen Farben.")));
    });

    const std::string host = currentHost();
    if (!host.empty()) {
        menu.addSeparator();
        auto *perSite = menu.addAction(QStringLiteral("Auf %1 verwenden").arg(QString::fromStdString(host)));
        perSite->setObjectName(QStringLiteral("darkenThisSiteAction"));
        perSite->setCheckable(true);
        perSite->setChecked(!appearance_.isExcluded(host));
        perSite->setEnabled(appearance_.enabled());
        connect(perSite, &QAction::triggered, this, [this, host](bool checked) {
            appearance_.setExcluded(host, !checked);
            updateWebAppearance();
        });
    }
    menu.addSeparator();
    auto *hint = menu.addAction(L(QStringLiteral("Bereits dunkle Seiten behalten ihre Farben.")));
    hint->setEnabled(false);
    if (appearanceButton_)
        menu.exec(appearanceButton_->mapToGlobal(QPoint(0, appearanceButton_->height())));
}





void SpikeWindow::toggleSplitView() {
    if (!splitPageId_.empty()) {
        clearSplitView();
        return;
    }
    auto *current = currentUserPage();
    if (!current) return;
    // Pair the active tab with the next visible tab of the same space.
    const std::string currentId = current->state().id;
    std::string partner;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent) continue;
        if (tab.state.id == currentId) continue;
        const auto workspace = workspaceTabs_.find(tab.state.id);
        const QString tabSpace = workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space;
        if (!tab.privatePage && tabSpace != activeSpace_) continue;
        partner = tab.state.id;
        break;
    }
    if (partner.empty()) {
        status_->setText(L(QStringLiteral("Split View benötigt einen zweiten Tab in diesem Space.")));
        return;
    }
    splitPageId_ = partner;
    synchronizeSession();
    status_->setText(L(QStringLiteral("Split View aktiv. ⇧⌘S beendet die Teilung.")));
}

void SpikeWindow::clearSplitView() {
    if (splitPageId_.empty()) return;
    splitPageId_.clear();
    synchronizeSession();
    status_->setText(L(QStringLiteral("Split View beendet.")));
}


void SpikeWindow::duplicateCurrentTab() {
    auto *page = currentUserPage();
    if (!page) return;
    const auto state = page->state();
    const QString url = QString::fromStdString(state.url);
    if (!url.startsWith(QStringLiteral("http://")) && !url.startsWith(QStringLiteral("https://"))) {
        status_->setText(L(QStringLiteral("Nur Webseiten können dupliziert werden.")));
        return;
    }
    // Duplicating reloads the URL in a new tab; in-page state is not cloned.
    const bool privatePage = page->profile()->isOffTheRecord();
    auto *copy = newUserTab(url, privatePage);
    if (!privatePage) {
        const auto workspace = workspaceTabs_.find(state.id);
        if (workspace != workspaceTabs_.end()) {
            WorkspaceTab metadata = workspace->second;
            metadata.pinned = false;
            workspaceTabs_[copy->state().id] = metadata;
        }
    }
    (void)session_.setActiveUserTab(copy->state().id);
    synchronizeSession();
    saveSession();
    status_->setText(L(QStringLiteral("Tab dupliziert.")));
}

void SpikeWindow::refreshAgentActivity() {
    if (!agentActivity_) return;
    agentActivity_->clear();
    for (const yobro::controller::AgentEvent &event : session_.agentEvents()) {
        agentActivity_->addItem(
            QString::fromStdString(event.action)
            + QStringLiteral(": ")
            + QString::fromStdString(event.detail)
            + QStringLiteral(" · ")
            + QString::fromStdString(event.space)
        );
    }
}
std::optional<std::string> SpikeWindow::captureWindowImage(std::string &error) {
    QWidget *target = window();
    if (!target || target->size().isEmpty()) {
        error = "Kein Fenster.";
        return std::nullopt;
    }
    const QPixmap image = target->grab();
    if (image.isNull()) {
        error = "Kein Fenster.";
        return std::nullopt;
    }
    const std::filesystem::path destination = paths_.profile / "window.png";
    if (!image.save(QString::fromStdString(destination.string()), "PNG")) {
        error = "Fensterbild konnte nicht geschrieben werden.";
        return std::nullopt;
    }
    return destination.string();
}
void SpikeWindow::showFindBar() {
    if (!findBar_ || !findInput_) return;
    if (!currentUserPage()) return;
    findBar_->show();
    findInput_->setFocus(Qt::ShortcutFocusReason);
    findInput_->selectAll();
}

void SpikeWindow::hideFindBar() {
    if (!findBar_) return;
    // Clear WebKit-style highlighting in the page before closing the bar.
    if (auto *page = currentUserPage())
        page->findText("", false, [](bool, std::optional<engine::EngineError>) {});
    if (findInput_) findInput_->clear();
    if (findStatus_) findStatus_->clear();
    findBar_->hide();
    if (auto *page = currentUserPage()) page->view()->setFocus(Qt::OtherFocusReason);
}

void SpikeWindow::findInPage(bool backwards) {
    auto *page = currentUserPage();
    if (!page || !findBar_ || !findInput_) return;
    if (!findBar_->isVisible()) showFindBar();
    const QString query = findInput_->text();
    if (query.isEmpty()) {
        if (findStatus_) findStatus_->clear();
        return;
    }
    page->findText(
        query.toStdString(),
        backwards,
        [this](bool found, std::optional<engine::EngineError> error) {
            if (!findStatus_) return;
            if (error) {
                findStatus_->setText(QString::fromStdString(error->message));
                return;
            }
            // Chromium reports whether a match exists, not a total count.
            findStatus_->setText(found ? L(QStringLiteral("Treffer")) : L(QStringLiteral("Keine Treffer")));
        }
    );
}

void SpikeWindow::showQuickSwitcher() {
    if (quickSwitcherDialog_) {
        quickSwitcherDialog_->show();
        quickSwitcherDialog_->raise();
        quickSwitcherDialog_->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("quickSwitcher"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(L(QStringLiteral("Schnell öffnen")));
    dialog->resize(560, 420);
    dialog->setStyleSheet(sheetStyleSheet(systemPrefersDark()) + QStringLiteral(
        "#quickSwitcherInput{border-radius:11px;padding:13px;font-size:14px;}"
    ));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(11);
    auto *input = new QLineEdit(dialog);
    input->setObjectName(QStringLiteral("quickSwitcherInput"));
    input->setPlaceholderText(L(QStringLiteral("Tabs und Verlauf durchsuchen oder Adresse eingeben")));
    auto *list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("quickSwitcherList"));
    layout->addWidget(input);
    layout->addWidget(list, 1);
    quickSwitcherDialog_ = dialog;

    const auto refresh = [this, input, list] {
        list->clear();
        const QString query = input->text().trimmed();
        for (const controller::SessionTabView &tab : session_.tabViews()) {
            if (tab.state.owner == engine::PageOwner::agent) continue;
            const QString title = QString::fromStdString(tab.state.title);
            const QString url = QString::fromStdString(tab.state.url);
            if (!query.isEmpty()
                && !title.contains(query, Qt::CaseInsensitive)
                && !url.contains(query, Qt::CaseInsensitive)) continue;
            auto *item = new QListWidgetItem(
                QStringLiteral("Tab · %1\n%2").arg(title.isEmpty() ? L(QStringLiteral("Neuer Tab")) : title, url),
                list
            );
            item->setData(Qt::UserRole, QStringLiteral("tab"));
            item->setData(Qt::UserRole + 1, QString::fromStdString(tab.state.id));
        }
        const core::Json history = library_.history(query.toStdString(), 12);
        if (history.isArray()) {
            for (const core::Json &entry : history.asArray()) {
                const core::Json *url = entry.find("url");
                if (!url || !url->isString()) continue;
                const core::Json *title = entry.find("title");
                const QString label = QString::fromStdString(
                    title && title->isString() && !title->asString().empty() ? title->asString() : url->asString()
                );
                auto *item = new QListWidgetItem(
                    QStringLiteral("Verlauf · %1\n%2").arg(label, QString::fromStdString(url->asString())),
                    list
                );
                item->setData(Qt::UserRole, QStringLiteral("url"));
                item->setData(Qt::UserRole + 1, QString::fromStdString(url->asString()));
            }
        }
        if (!query.isEmpty()) {
            auto *item = new QListWidgetItem(QStringLiteral("Im Web suchen · %1").arg(query), list);
            item->setData(Qt::UserRole, QStringLiteral("search"));
            item->setData(Qt::UserRole + 1, query);
        }
        if (list->count() > 0) list->setCurrentRow(0);
    };

    const auto activate = [this, list, dialog] {
        QListWidgetItem *item = list->currentItem();
        if (!item) return;
        const QString kind = item->data(Qt::UserRole).toString();
        const QString payload = item->data(Qt::UserRole + 1).toString();
        dialog->close();
        if (kind == QStringLiteral("tab")) {
            const auto workspace = workspaceTabs_.find(payload.toStdString());
            if (workspace != workspaceTabs_.end() && workspace->second.space != activeSpace_)
                switchSpace(workspace->second.space);
            (void)session_.setActiveUserTab(payload.toStdString());
            synchronizeSession();
            return;
        }
        if (kind == QStringLiteral("url")) {
            newUserTab(payload);
            saveSession();
            return;
        }
        QUrl target(QStringLiteral("https://duckduckgo.com/"));
        QUrlQuery search;
        search.addQueryItem(QStringLiteral("q"), payload);
        target.setQuery(search);
        newUserTab(target.toString());
        saveSession();
    };

    QObject::connect(input, &QLineEdit::textChanged, dialog, refresh);
    QObject::connect(input, &QLineEdit::returnPressed, dialog, activate);
    QObject::connect(list, &QListWidget::itemActivated, dialog, activate);
    refresh();
    dialog->show();
    input->setFocus(Qt::ShortcutFocusReason);
}

} // namespace yobro::spike
