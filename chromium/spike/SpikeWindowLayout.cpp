#include "spike/SpikeWindowInternal.hpp"

namespace yobro::spike {

using namespace windowSupport;

SpikeWindow::SpikeWindow(
    controller::BrowserSession &session,
    qtwebengine::QtBrowserLibrary &library,
    BridgePolicyStore &bridgePolicy,
    core::ProfilePaths paths,
    QString initialUrl,
    QString startupPolicyProblem
) : session_(session),
    library_(library),
    bridgePolicy_(bridgePolicy),
    paths_(std::move(paths)),
    passwords_(paths_.profile),
    profiles_(paths_.root),
    privacy_(paths_.profile),
    zoom_(paths_.profile),
    appearance_(paths_.profile),
    adBlock_(paths_.profile),
    proxies_(paths_.profile),
    chat_(paths_.profile),
    mail_(paths_.profile, this) {
    profile_ = dynamic_cast<qtwebengine::QtBrowserProfile *>(&session_.profile());
    if (!profile_)
        throw std::runtime_error("The feasibility shell requires the Qt WebEngine profile adapter.");
    fileRevealHandler_ = revealFileInPlatformShell;

    setWindowTitle(QStringLiteral("YOBRO"));
    resize(1360, 880);
    setMinimumSize(960, 640);

    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("productShell"));
    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, root);
    mainSplitter_ = splitter;
    splitter->setChildrenCollapsible(false);
    rootLayout->addWidget(splitter, 1);

    auto *userPane = new QWidget(splitter);
    auto *userLayout = new QHBoxLayout(userPane);
    userLayout->setContentsMargins(0, 0, 0, 0);
    userLayout->setSpacing(0);

    back_ = new QPushButton(QStringLiteral("←"), userPane);
    back_->setObjectName(QStringLiteral("backButton"));
    back_->setToolTip(L(QStringLiteral("Zurück")));
    forward_ = new QPushButton(QStringLiteral("→"), userPane);
    forward_->setObjectName(QStringLiteral("forwardButton"));
    forward_->setToolTip(L(QStringLiteral("Vorwärts")));
    reload_ = new QPushButton(QStringLiteral("↻"), userPane);
    reload_->setObjectName(QStringLiteral("reloadButton"));
    reload_->setToolTip(L(QStringLiteral("Neu laden")));
    auto *libraryButton = new QPushButton(L(QStringLiteral("Bibliothek")), userPane);
    libraryButton->setObjectName(QStringLiteral("libraryButton"));
    libraryButton->setToolTip(L(QStringLiteral("Verlauf und Lesezeichen dieses Profils")));
    address_ = new QLineEdit(userPane);
    address_->setObjectName(QStringLiteral("topAddress"));
    address_->setPlaceholderText(L(QStringLiteral("Suchen oder URL eingeben")));
    // Created before the toolbar layout so it is never added as a null widget.
    loginFillButton_ = new QPushButton(QStringLiteral("⚿"), userPane);
    loginFillButton_->setObjectName(QStringLiteral("loginFillButton"));
    loginFillButton_->setToolTip(L(QStringLiteral("Gespeicherte Logins ausfüllen")));
    loginFillButton_->setAccessibleName(L(QStringLiteral("Gespeicherte Logins ausfüllen")));
    appearanceButton_ = new QPushButton(QStringLiteral("◔"), userPane);
    appearanceButton_->setObjectName(QStringLiteral("appearanceButton"));
    appearanceButton_->setToolTip(L(QStringLiteral("Webseiten-Darstellung")));
    appearanceButton_->setAccessibleName(L(QStringLiteral("Webseiten-Darstellung")));

    auto *workspaceSidebar = new QWidget(userPane);
    workspaceSidebar_ = workspaceSidebar;
    workspaceSidebar->setObjectName(QStringLiteral("workspaceSidebar"));
    workspaceSidebar->setMinimumWidth(248);
    workspaceSidebar->setMaximumWidth(248);
    auto *sidebarLayout = new QVBoxLayout(workspaceSidebar);
    sidebarLayout->setContentsMargins(17, 12, 17, 12);
    sidebarLayout->setSpacing(8);
    auto *workspaceTitle = new QLabel(QStringLiteral("yobro"), workspaceSidebar);
    workspaceTitle->setObjectName(QStringLiteral("productBrand"));
    workspaceTitle->setStyleSheet(QStringLiteral("font-size:29px;font-weight:700;letter-spacing:-1.4px;color:#e3e9e0"));
    auto *previewLabel = new QLabel(QStringLiteral("PREVIEW"), workspaceSidebar);
    previewLabel->setStyleSheet(QStringLiteral("color:#b9cbb3;font:700 8px monospace;letter-spacing:1.5px"));
    auto *brandRow = new QWidget(workspaceSidebar);
    brandRow->setObjectName(QStringLiteral("sidebarBrandRow"));
    auto *brandLayout = new QHBoxLayout(brandRow);
    brandLayout->setContentsMargins(0, 3, 0, 14);
    brandLayout->setSpacing(10);
    auto *brandMark = new QLabel(QStringLiteral("Y"), brandRow);
    brandMark->setObjectName(QStringLiteral("productBrandMark"));
    brandMark->setStyleSheet(QStringLiteral("font-size:33px;color:#f46a35;font-weight:800;font-family:system-ui"));
    brandLayout->addWidget(brandMark);
    brandLayout->addWidget(workspaceTitle);
    brandLayout->addStretch();
    brandLayout->addWidget(previewLabel, 0, Qt::AlignVCenter);
    sidebarLayout->addWidget(brandRow);
    auto *sidebarQuickSearch = new QPushButton(L(QStringLiteral("⌕   Suchen oder öffnen                 ⌘ K")), workspaceSidebar);
    sidebarQuickSearch->setObjectName(QStringLiteral("sidebarQuickSearch"));
    sidebarQuickSearch->setToolTip(L(QStringLiteral("Adresse öffnen")));
    sidebarLayout->addWidget(sidebarQuickSearch);
    auto *sidebarNavigation = new QWidget(userPane);
    sidebarNavigation->setObjectName(QStringLiteral("topNavigation"));
    auto *navigationLayout = new QHBoxLayout(sidebarNavigation);
    navigationLayout->setContentsMargins(3, 0, 3, 0);
    navigationLayout->setSpacing(2);
    navigationLayout->addWidget(back_);
    navigationLayout->addWidget(forward_);
    navigationLayout->addWidget(reload_);
    auto *workspaceSection = new QLabel(QStringLiteral("WORKSPACE"), workspaceSidebar);
    workspaceSection->setText(L(QStringLiteral("DEINE TABS")));
    workspaceSection->setStyleSheet(QStringLiteral("color:#8e988f;font:700 10px monospace;letter-spacing:1.8px;padding:14px 3px 7px"));
    sidebarLayout->addWidget(workspaceSection);
    workspaceTree_ = new QTreeWidget(workspaceSidebar);
    workspaceTree_->setObjectName(QStringLiteral("workspaceTree"));
    workspaceTree_->setHeaderHidden(true);
    workspaceTree_->setRootIsDecorated(true);
    workspaceTree_->setIndentation(10);
    workspaceTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    workspaceTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Tabs are reordered and refiled by dragging them inside the sidebar.
    workspaceTree_->setDragEnabled(true);
    workspaceTree_->setAcceptDrops(true);
    workspaceTree_->viewport()->setAcceptDrops(true);
    workspaceTree_->setDropIndicatorShown(true);
    workspaceTree_->setDragDropMode(QAbstractItemView::InternalMove);
    workspaceTree_->setDefaultDropAction(Qt::MoveAction);
    workspaceTree_->setStyleSheet(QStringLiteral(
        "QTreeWidget{background:transparent;border:0;padding:0;color:#e3e9e0;outline:0;}"
        "QTreeWidget::item{padding:10px 9px;border-radius:11px;margin:2px 0;min-height:24px;}"
        "QTreeWidget::item:hover{background:#303a33;}"
        "QTreeWidget::item:selected{background:#39433b;color:#f7faf2;}"
        "QTreeWidget::branch{background:transparent;}"
    ));
    sidebarLayout->addWidget(workspaceTree_, 1);
    userLayout->addWidget(workspaceSidebar);

    auto *browserContentHost = new QWidget(userPane);
    browserContentHost->setObjectName(QStringLiteral("browserContentHost"));
    auto *browserLayout = new QVBoxLayout(browserContentHost);
    browserLayout->setContentsMargins(10, 10, 10, 10);
    browserLayout->setSpacing(10);
    auto *topChrome = new QWidget(browserContentHost);
    topChrome->setObjectName(QStringLiteral("topChrome"));
    auto *topChromeLayout = new QHBoxLayout(topChrome);
    topChromeLayout->setContentsMargins(0, 0, 0, 0);
    topChromeLayout->setSpacing(10);
    topChromeLayout->addWidget(sidebarNavigation);
    topChromeLayout->addWidget(address_, 1);
    topChromeLayout->addWidget(appearanceButton_);
    topChromeLayout->addWidget(loginFillButton_);
    browserLayout->addWidget(topChrome);

    auto *newNoteButton = new QPushButton(L(QStringLiteral("Neue Notiz"), QStringLiteral("New note")), userPane);
    newNoteButton->setObjectName(QStringLiteral("newNoteButton"));
    QObject::connect(newNoteButton, &QPushButton::clicked, this, [this] { (void)newNote(); });
    auto *newTabButton = new QPushButton(L(QStringLiteral("Neuer Tab")), userPane);
    newTabButton->setObjectName(QStringLiteral("newTabButton"));
    auto *privateTabButton = new QPushButton(L(QStringLiteral("Privat")), userPane);
    privateTabButton->setObjectName(QStringLiteral("privateTabButton"));
    auto *bookmarkButton = new QPushButton(QStringLiteral("★"), userPane);
    bookmarkButton->setObjectName(QStringLiteral("bookmarkCurrentPageButton"));
    bookmarkButton->setToolTip(L(QStringLiteral("Aktuelle Seite als Lesezeichen speichern")));
    reopenClosedTabButton_ = new QPushButton(L(QStringLiteral("Tab wieder öffnen")), userPane);
    reopenClosedTabButton_->setObjectName(QStringLiteral("reopenClosedTabButton"));
    // The WebKit build offers this as a menu command without a key, so ⇧⌘T can
    // stay on the private tab.
    reopenClosedTabButton_->setToolTip(L(QStringLiteral("Zuletzt geschlossenen Tab wieder öffnen")));
    reopenClosedTabButton_->setEnabled(false);
    auto *downloadsButton = new QPushButton(QStringLiteral("Downloads"), userPane);
    auto *importButton = new QPushButton(L(QStringLiteral("Daten importieren")), userPane);
    importButton->setObjectName(QStringLiteral("webkitImportButton"));
    importButton->setToolTip(L(QStringLiteral("Lesezeichen, Verlauf und Tabs aus einem YOBRO-WebKit-Profil importieren")));
    auto *extensionsButton = new QPushButton(L(QStringLiteral("Erweiterungen")), userPane);
    extensionsButton->setObjectName(QStringLiteral("extensionsButton"));
    auto *mailButton = new QPushButton(QStringLiteral("YoBro Mail"), userPane);
    mailButton->setObjectName(QStringLiteral("mailButton"));
    // ⇧⌘M belongs to the menu action, as with Downloads.
    mailButton->setToolTip(L(QStringLiteral("Eingebautes Postfach (⇧⌘M)"),
                             QStringLiteral("Built-in mailbox (⇧⌘M)")));
    auto *agentPaneToggle = new QPushButton(L(QStringLiteral("Agent anzeigen"), QStringLiteral("Show agent")), userPane);
    agentPaneToggle->setObjectName(QStringLiteral("agentPaneToggle"));
    agentPaneToggle->setCheckable(true);
    agentPaneToggle->setToolTip(L(QStringLiteral("Isolierte Agentenfläche ein- oder ausblenden")));
    downloadsButton->setObjectName(QStringLiteral("downloadsButton"));
    // ⇧⌘J belongs to the menu action; a second binding would be ambiguous.
    downloadsButton->setToolTip(L(QStringLiteral("Deine Bibliothek · Downloads (⇧⌘J)")));
    spacePicker_ = new QComboBox(workspaceSidebar);
    spacePicker_->setObjectName(QStringLiteral("spacePicker"));
    spacePicker_->setToolTip(L(QStringLiteral("Aktiven Space wechseln")));
    spacePicker_->addItem(activeSpace_);
    auto *addSpaceButton = new QPushButton(QStringLiteral("+"), workspaceSidebar);
    addSpaceButton->setObjectName(QStringLiteral("addSpaceButton"));
    addSpaceButton->setToolTip(L(QStringLiteral("Neuen Space erstellen")));
    folderPicker_ = new QComboBox(workspaceSidebar);
    folderPicker_->setObjectName(QStringLiteral("folderPicker"));
    folderPicker_->addItem(L(QStringLiteral("Ohne Ordner")), QString());
    auto *addFolderButton = new QPushButton(QStringLiteral("+"), workspaceSidebar);
    addFolderButton->setObjectName(QStringLiteral("addFolderButton"));
    addFolderButton->setToolTip(L(QStringLiteral("Neuen Ordner erstellen")));
    auto *moveFolderButton = new QPushButton(L(QStringLiteral("Ordner")), workspaceSidebar);
    moveFolderButton->setObjectName(QStringLiteral("moveToFolderButton"));
    auto *togglePinnedButton = new QPushButton(L(QStringLiteral("Pin")), workspaceSidebar);
    togglePinnedButton->setObjectName(QStringLiteral("togglePinnedButton"));
    togglePinnedButton->setToolTip(L(QStringLiteral("Aktuellen Tab in der Sidebar an- oder lospinnen")));
    auto *tabActions = new QWidget(workspaceSidebar);
    tabActions->setObjectName(QStringLiteral("sidebarTabActions"));
    auto *tabActionsLayout = new QHBoxLayout(tabActions);
    tabActionsLayout->setContentsMargins(0, 0, 0, 0);
    tabActionsLayout->setSpacing(4);
    tabActionsLayout->addWidget(newTabButton);
    tabActionsLayout->addWidget(newNoteButton);
    tabActionsLayout->addWidget(privateTabButton);
    tabActionsLayout->addWidget(reopenClosedTabButton_);
    sidebarLayout->addWidget(tabActions);

    auto *spaceActions = new QWidget(workspaceSidebar);
    spaceActions->setObjectName(QStringLiteral("sidebarSpaceActions"));
    auto *spaceActionsLayout = new QHBoxLayout(spaceActions);
    spaceActionsLayout->setContentsMargins(0, 0, 0, 0);
    spaceActionsLayout->setSpacing(4);
    spaceActionsLayout->addWidget(spacePicker_, 1);
    spaceActionsLayout->addWidget(addSpaceButton);
    sidebarLayout->addWidget(spaceActions);

    auto *folderActions = new QWidget(workspaceSidebar);
    folderActions->setObjectName(QStringLiteral("sidebarFolderActions"));
    auto *folderActionsLayout = new QHBoxLayout(folderActions);
    folderActionsLayout->setContentsMargins(0, 0, 0, 0);
    folderActionsLayout->setSpacing(4);
    folderActionsLayout->addWidget(folderPicker_, 1);
    folderActionsLayout->addWidget(addFolderButton);
    sidebarLayout->addWidget(folderActions);
    sidebarLayout->insertWidget(4, spaceActions);
    sidebarLayout->insertWidget(5, folderActions);

    auto *pageActions = new QWidget(workspaceSidebar);
    pageActions->setObjectName(QStringLiteral("sidebarPageActions"));
    auto *pageActionsLayout = new QHBoxLayout(pageActions);
    pageActionsLayout->setContentsMargins(0, 0, 0, 0);
    pageActionsLayout->setSpacing(2);
    pageActionsLayout->addWidget(bookmarkButton);
    pageActionsLayout->addWidget(moveFolderButton);
    pageActionsLayout->addWidget(togglePinnedButton);
    pageActionsLayout->addWidget(importButton);
    pageActionsLayout->addWidget(extensionsButton);
    sidebarLayout->addWidget(pageActions);

    auto *libraryActions = new QWidget(workspaceSidebar);
    libraryActions->setObjectName(QStringLiteral("sidebarLibraryActions"));
    auto *libraryActionsLayout = new QHBoxLayout(libraryActions);
    libraryActionsLayout->setContentsMargins(0, 0, 0, 0);
    libraryActionsLayout->setSpacing(4);
    libraryActionsLayout->addWidget(libraryButton, 1);
    libraryActionsLayout->addWidget(downloadsButton, 1);
    libraryActionsLayout->addWidget(mailButton, 1);
    sidebarLayout->addWidget(libraryActions);

    auto *agentCard = new QWidget(workspaceSidebar);
    agentCard->setObjectName(QStringLiteral("sidebarAgentCard"));
    auto *agentCardLayout = new QVBoxLayout(agentCard);
    agentCardLayout->setContentsMargins(13, 12, 13, 12);
    agentCardLayout->setSpacing(4);
    agentPaneToggle->setText(QStringLiteral("●  Bereit für deine Agenten\nEin Browser. Für euch beide."));
    agentPaneToggle->setObjectName(QStringLiteral("agentPaneToggle"));
    agentPaneToggle->setMinimumHeight(58);
    agentCardLayout->addWidget(agentPaneToggle);
    sidebarLayout->addWidget(agentCard);
    profileFooter_ = new QPushButton(workspaceSidebar);
    profileFooter_->setObjectName(QStringLiteral("sidebarProfileFooter"));
    profileFooter_->setToolTip(L(QStringLiteral("Profile und Einstellungen (⌘,)")));
    sidebarLayout->addWidget(profileFooter_);
    QObject::connect(profileFooter_, &QPushButton::clicked, this, [this] { showSettings(); });

    // The collapsed sidebar keeps spaces and tabs reachable as icons instead of
    // hiding the workspace entirely.
    buildCompactSidebar(userPane);
    userLayout->addWidget(compactSidebar_);
    compactSidebar_->hide();


    tabs_ = new QTabWidget(browserContentHost);
    tabs_->setObjectName(QStringLiteral("browserTabs"));
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->tabBar()->hide();

    // Split view keeps both pages inside their own QTabWidget host so the
    // engine-owned views are only reparented, never re-created or cloned.
    contentSplitter_ = new QSplitter(Qt::Horizontal, browserContentHost);
    contentSplitter_->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter_->setChildrenCollapsible(false);
    contentSplitter_->setHandleWidth(10);
    splitTabs_ = new QTabWidget(contentSplitter_);
    splitTabs_->setObjectName(QStringLiteral("splitTabs"));
    splitTabs_->tabBar()->hide();
    contentSplitter_->addWidget(tabs_);
    contentSplitter_->addWidget(splitTabs_);
    splitTabs_->hide();
    browserLayout->addWidget(contentSplitter_, 1);

    findBar_ = new QWidget(browserContentHost);
    findBar_->setObjectName(QStringLiteral("findBar"));
    auto *findLayout = new QHBoxLayout(findBar_);
    findLayout->setContentsMargins(10, 6, 10, 6);
    findLayout->setSpacing(6);
    findInput_ = new QLineEdit(findBar_);
    findInput_->setObjectName(QStringLiteral("findInput"));
    findInput_->setPlaceholderText(L(QStringLiteral("Auf dieser Seite suchen")));
    findStatus_ = new QLabel(QString(), findBar_);
    findStatus_->setObjectName(QStringLiteral("findStatus"));
    auto *findPrevious = new QPushButton(QStringLiteral("‹"), findBar_);
    findPrevious->setObjectName(QStringLiteral("findPreviousButton"));
    findPrevious->setToolTip(L(QStringLiteral("Vorherige Übereinstimmung (⇧⌘G)")));
    auto *findNext = new QPushButton(QStringLiteral("›"), findBar_);
    findNext->setObjectName(QStringLiteral("findNextButton"));
    findNext->setToolTip(L(QStringLiteral("Nächste Übereinstimmung (⌘G)")));
    auto *findClose = new QPushButton(QStringLiteral("×"), findBar_);
    findClose->setObjectName(QStringLiteral("findCloseButton"));
    findLayout->addWidget(findInput_, 1);
    findLayout->addWidget(findStatus_);
    findLayout->addWidget(findPrevious);
    findLayout->addWidget(findNext);
    findLayout->addWidget(findClose);
    findBar_->hide();
    browserLayout->addWidget(findBar_);

    status_ = new QLabel(L(QStringLiteral("Bereit")), browserContentHost);
    status_->setObjectName(QStringLiteral("browserStatus"));
    browserLayout->addWidget(status_);
    userLayout->addWidget(browserContentHost, 1);

    auto *agentPane = new QWidget(splitter);
    agentPane_ = agentPane;
    agentPane->setObjectName(QStringLiteral("agentPane"));
    auto *agentLayout = new QVBoxLayout(agentPane);
    agentLayout->setContentsMargins(5, 10, 10, 10);
    agentLayout->setSpacing(7);
    auto *agentHeader = new QHBoxLayout();
    auto *agentLabel = new QLabel(L(QStringLiteral("AGENT · isolierte Anwendungswelt"), QStringLiteral("AGENT · isolated application world")), agentPane);
    agentLabel->setStyleSheet(QStringLiteral("color:#ff541c;font:700 11px monospace"));
    libraryAccess_ = new QCheckBox(L(QStringLiteral("Verlauf und Downloads erlauben"), QStringLiteral("Allow history + downloads")), agentPane);
    libraryAccess_->setObjectName(QStringLiteral("libraryAccessToggle"));
    libraryAccess_->setToolTip(QStringLiteral(
        "Off by default. When enabled, local Agent Protocol clients may read persisted history and download paths."
    ));
    libraryAccess_->setChecked(session_.protocolState().libraryAccess);
    auto *sendToAgentButton = new QPushButton(L(QStringLiteral("Aktuelle Adresse öffnen"), QStringLiteral("Open current URL")), agentPane);
    auto *readAgentButton = new QPushButton(L(QStringLiteral("Momentaufnahme lesen"), QStringLiteral("Read snapshot")), agentPane);
    agentHeader->addWidget(agentLabel);
    agentHeader->addWidget(libraryAccess_);
    agentHeader->addStretch();
    agentHeader->addWidget(sendToAgentButton);
    agentHeader->addWidget(readAgentButton);
    agentLayout->addLayout(agentHeader);

    // The assistant lives in the same pane as the agent surface and shares its
    // shortcut, exactly as in the WebKit build where ⌘⇧A opens both.
    sync_ = new SyncController(
        paths_.profile,
        [this] { return buildSyncSnapshot(); },
        [this](const SyncSnapshot &snapshot, SyncTabResolution resolution) {
            return applySyncSnapshot(snapshot, resolution);
        },
        this
    );

    chatRunner_ = new SpaceChatRunner(chat_, this);
    chatPanel_ = new SpaceChatPanel(chat_, *chatRunner_, *this, agentPane);
    agentLayout->addWidget(chatPanel_, 3);

    auto *agentWebHost = new QWidget(agentPane);
    agentWebLayout_ = new QVBoxLayout(agentWebHost);
    agentWebLayout_->setContentsMargins(0, 0, 0, 0);
    agentPlaceholder_ = new QLabel(QStringLiteral(
        "Agent access is ready. The right pane is created only when a local v2 command or ‘Open current URL’ requests it."
    ), agentWebHost);
    agentPlaceholder_->setWordWrap(true);
    agentPlaceholder_->setAlignment(Qt::AlignCenter);
    agentPlaceholder_->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;border-radius:8px;padding:30px;color:%3").arg(themePalette(systemPrefersDark()).sheetSurface, themePalette(systemPrefersDark()).sheetBorder, themePalette(systemPrefersDark()).sheetMuted));
    agentWebLayout_->addWidget(agentPlaceholder_);
    agentLayout->addWidget(agentWebHost, 2);

    // The user must be able to read back what the agent did without trusting
    // the agent's own report, like the event list in the WebKit build.
    agentActivity_ = new QListWidget(agentPane);
    agentActivity_->setObjectName(QStringLiteral("agentActivityList"));
    agentActivity_->setSelectionMode(QAbstractItemView::NoSelection);
    agentActivity_->setFocusPolicy(Qt::NoFocus);
    agentActivity_->setMaximumHeight(96);
    agentLayout->addWidget(agentActivity_);
    agentOutput_ = new QPlainTextEdit(agentPane);
    agentOutput_->setReadOnly(true);
    agentOutput_->setPlaceholderText(L(QStringLiteral("Hier erscheint das Protokoll-v2-JSON."), QStringLiteral("Protocol-v2 snapshot JSON appears here.")));
    agentLayout->addWidget(agentOutput_, 1);
    profileStatus_ = new QLabel(
        QString::fromStdString("Profile: " + paths_.profile.string() + " · socket: " + paths_.control.string()),
        agentPane
    );
    profileStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    profileStatus_->setStyleSheet(QStringLiteral("color:%1;font:10px monospace").arg(themePalette(systemPrefersDark()).textMuted));
    agentLayout->addWidget(profileStatus_);

    splitter->addWidget(userPane);
    splitter->addWidget(agentPane);
    splitter->setSizes({1360, 0});
    agentPane->hide();
    setCentralWidget(root);
    applyTheme();

    QObject::connect(newTabButton, &QPushButton::clicked, this, [this] { newUserTab(); });
    QObject::connect(privateTabButton, &QPushButton::clicked, this, [this] { newUserTab({}, true); });
    QObject::connect(libraryButton, &QPushButton::clicked, this, [this] { showLibrary(); });
    QObject::connect(sidebarQuickSearch, &QPushButton::clicked, this, [this] {
        address_->setFocus(Qt::ShortcutFocusReason);
        address_->selectAll();
    });
    QObject::connect(bookmarkButton, &QPushButton::clicked, this, [this] { bookmarkCurrentPage(); });
    QObject::connect(reopenClosedTabButton_, &QPushButton::clicked, this, [this] { reopenClosedTab(); });
    QObject::connect(addSpaceButton, &QPushButton::clicked, this, [this] { createSpace(); });
    QObject::connect(addFolderButton, &QPushButton::clicked, this, [this] { createFolder(); });
    QObject::connect(moveFolderButton, &QPushButton::clicked, this, [this] { moveCurrentTabToFolder(); });
    QObject::connect(togglePinnedButton, &QPushButton::clicked, this, [this] { toggleCurrentTabPinned(); });
    QObject::connect(workspaceTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        activateWorkspaceItem(item);
    });
    QObject::connect(workspaceTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        activateWorkspaceItem(item);
    });
    QObject::connect(workspaceTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        setWorkspaceGroupCollapsed(item, true);
    });
    QObject::connect(workspaceTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        setWorkspaceGroupCollapsed(item, false);
    });
    QObject::connect(workspaceTree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        showWorkspaceContextMenu(position);
    });
    // QTreeWidget performs internal moves as insert plus remove, so the new
    // arrangement is read back once the view has settled.
    QObject::connect(workspaceTree_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        scheduleWorkspaceOrderCommit();
    });
    QObject::connect(workspaceTree_->model(), &QAbstractItemModel::rowsInserted, this, [this] {
        scheduleWorkspaceOrderCommit();
    });
    QObject::connect(spacePicker_, &QComboBox::textActivated, this, [this](const QString &value) { switchSpace(value); });
    QObject::connect(downloadsButton, &QPushButton::clicked, this, [this] { showDownloads(); });
    QObject::connect(mailButton, &QPushButton::clicked, this, [this] { showMail(); });
    QObject::connect(importButton, &QPushButton::clicked, this, [this] { showWebKitImport(); });
    QObject::connect(extensionsButton, &QPushButton::clicked, this, [this] { showExtensions(); });
    QObject::connect(agentPaneToggle, &QPushButton::toggled, this, [this, agentPaneToggle](bool visible) {
        setAgentPaneVisible(visible);
        agentPaneToggle->setText(visible
            ? QStringLiteral("●  Agentenfläche offen\nDu links · Agent rechts")
            : QStringLiteral("●  Bereit für deine Agenten\nEin Browser. Für euch beide."));
    });
    QObject::connect(address_, &QLineEdit::returnPressed, this, [this] { navigateCurrent(); });
    QObject::connect(findInput_, &QLineEdit::returnPressed, this, [this] { findInPage(false); });
    QObject::connect(findInput_, &QLineEdit::textChanged, this, [this](const QString &value) {
        if (value.isEmpty() && findStatus_) findStatus_->clear();
    });
    QObject::connect(findNext, &QPushButton::clicked, this, [this] { findInPage(false); });
    QObject::connect(findPrevious, &QPushButton::clicked, this, [this] { findInPage(true); });
    QObject::connect(findClose, &QPushButton::clicked, this, [this] { hideFindBar(); });
    QObject::connect(back_, &QPushButton::clicked, this, [this] {
        if (auto *page = currentUserPage())
            (void)session_.goBackUserTab(page->state().id);
    });
    QObject::connect(forward_, &QPushButton::clicked, this, [this] {
        if (auto *page = currentUserPage())
            (void)session_.goForwardUserTab(page->state().id);
    });
    QObject::connect(reload_, &QPushButton::clicked, this, [this] {
        if (auto *page = currentUserPage())
            (void)session_.reloadUserTab(page->state().id);
    });
    QObject::connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeUserTab(index); });
    QObject::connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        if (!synchronizing_) {
            if (auto *page = currentUserPage())
                (void)session_.setActiveUserTab(page->state().id);
        }
        updateNavigationState();
    });
    QObject::connect(sendToAgentButton, &QPushButton::clicked, this, [this] { sendCurrentToAgent(); });
    QObject::connect(readAgentButton, &QPushButton::clicked, this, [this] { readAgentPage(); });
    QObject::connect(libraryAccess_, &QCheckBox::toggled, this, [this](bool enabled) {
        const std::optional<std::string> problem = bridgePolicy_.persistAndApply(
            enabled,
            [this](bool value) { session_.setLibraryAccess(value); }
        );
        if (problem) {
            const QSignalBlocker blocker(libraryAccess_);
            libraryAccess_->setChecked(session_.protocolState().libraryAccess);
            status_->setText(QStringLiteral("Library-access policy was not changed: ") + QString::fromStdString(*problem));
            return;
        }
        status_->setText(enabled
            ? QStringLiteral("Agent history and download-list access enabled for this profile.")
            : QStringLiteral("Agent history and download-list access disabled for this profile."));
    });

    auto *extensionManager = profile_->persistentProfile()->extensionManager();
    QObject::connect(
        extensionManager,
        &QWebEngineExtensionManager::installFinished,
        this,
        [this](const QWebEngineExtensionInfo &extension) {
            if (!extension.isLoaded()) {
                status_->setText(QStringLiteral("Extension install failed: ") + extension.error());
                return;
            }
            // Enabling straight from this signal crashes the extension backend,
            // so the profile defers the switch for us.
            profile_->enableExtensionAfterInstall(extension);
            status_->setText(QStringLiteral("MV3 extension loaded and enabled: ") + extension.name());
            QTimer::singleShot(
                qtwebengine::QtBrowserProfile::installSettleDelayMilliseconds + 50,
                this,
                [this] { refreshExtensions(); }
            );
            refreshExtensions();
        }
    );
    QObject::connect(extensionManager, &QWebEngineExtensionManager::uninstallFinished,
                     this, [this](const QWebEngineExtensionInfo &) { refreshExtensions(); });
    QObject::connect(extensionManager, &QWebEngineExtensionManager::loadFinished,
                     this, [this](const QWebEngineExtensionInfo &extension) {
                         profile_->restoreExtensionState(extension);
                         refreshExtensions();
                     });
    QObject::connect(extensionManager, &QWebEngineExtensionManager::unloadFinished,
                     this, [this](const QWebEngineExtensionInfo &) { refreshExtensions(); });
    QTimer::singleShot(0, this, [this, extensionManager] {
        profile_->loadInstalledExtensions();
        for (const auto &extension : extensionManager->extensions())
            profile_->restoreExtensionState(extension);
        refreshExtensions();
    });

    // WebKit parity: user and private downloads are accepted immediately and
    // surfaced through the library instead of an extra confirmation dialog.
    library_.setUserDownloadPrompt({});
    library_.setDownloadsObserver([this] { refreshDownloads(); });
    session_.setPermissionPromptPresenter([this](const controller::PermissionPrompt &prompt) {
        synchronizePermissionPrompt();
        return permissionDialog_ && permissionDialogId_ == prompt.id;
    });
    session_.setPasskeyPromptPresenter(
        [this](const engine::PasskeyRequest &request, const engine::PasskeyControls &controls) {
            presentPasskeyStep(request, controls);
        }
    );
    session_.setDesktopMediaPromptPresenter(
        [this](const engine::DesktopMediaRequest &request, const engine::DesktopMediaControls &controls) {
            presentDesktopMediaPicker(request, controls);
        }
    );
    session_.setCertificatePromptPresenter(
        [this](const engine::CertificateProblem &problem) -> bool {
            const QString host = QString::fromStdString(problem.host);
            // For anything that could be a public website the engine's verdict
            // stands. There is deliberately no way to click through.
            if (!isVisible() || !CertificateTrustStore::isLocal(host)) return false;
            const QString fingerprint = QString::fromStdString(problem.fingerprint);
            if (certificates_.isAccepted(host, fingerprint)) return true;

            const QString unknown = L(QStringLiteral("Unbekannt"), QStringLiteral("Unknown"));
            const QString subject = problem.subject.empty() ? unknown : QString::fromStdString(problem.subject);
            const QString issuer = problem.issuer.empty()
                ? L(QStringLiteral("Selbst ausgestellt oder unbekannt"),
                    QStringLiteral("Self-issued or unknown"))
                : QString::fromStdString(problem.issuer);
            const QString validity = problem.validity.empty()
                ? L(QStringLiteral("Gültigkeit unbekannt"), QStringLiteral("Validity unknown"))
                : QString::fromStdString(problem.validity);
            const QStringList details{
                L(QStringLiteral("Name: %1"), QStringLiteral("Name: %1")).arg(subject),
                L(QStringLiteral("Ausgestellt von: %1"), QStringLiteral("Issued by: %1")).arg(issuer),
                L(QStringLiteral("Gültig: %1"), QStringLiteral("Valid: %1")).arg(validity),
                QStringLiteral("SHA-256: %1").arg(fingerprint),
                QString::fromStdString(problem.description),
                L(QStringLiteral("Vergleiche den Fingerprint mit deinem Server. Die Ausnahme gilt nur für "
                                 "diesen Host und nur bis YOBRO beendet wird."),
                  QStringLiteral("Compare the fingerprint with your server. The exception applies to this "
                                 "host only and lasts until YOBRO quits.")),
            };

            QMessageBox dialog(this);
            dialog.setObjectName(QStringLiteral("certificatePrompt"));
            dialog.setIcon(QMessageBox::Warning);
            dialog.setText(
                L(QStringLiteral("Zertifikat von %1 ist ungültig"), QStringLiteral("Certificate for %1 is invalid"))
                    .arg(host)
            );
            dialog.setInformativeText(details.join(QStringLiteral("\n")));
            auto *proceed = dialog.addButton(
                L(QStringLiteral("Trotzdem fortfahren"), QStringLiteral("Continue anyway")),
                QMessageBox::DestructiveRole
            );
            proceed->setObjectName(QStringLiteral("certificateAccept"));
            auto *cancel = dialog.addButton(L(QStringLiteral("Abbrechen")), QMessageBox::RejectRole);
            cancel->setObjectName(QStringLiteral("certificateReject"));
            dialog.setDefaultButton(cancel);
            dialog.setEscapeButton(cancel);
            dialog.setStyleSheet(sheetStyleSheet(systemPrefersDark()));
            dialog.exec();
            if (dialog.clickedButton() != proceed) return false;
            certificates_.accept(host, fingerprint);
            status_->setText(
                L(QStringLiteral("Zertifikatsausnahme für %1 gilt bis zum Beenden."),
                  QStringLiteral("Certificate exception for %1 applies until you quit."))
                    .arg(host)
            );
            return true;
        }
    );
    session_.setAuthenticationPromptPresenter(
        [this](const engine::AuthenticationChallenge &challenge)
            -> std::optional<engine::AuthenticationCredentials> {
            if (challenge.proxy || !isVisible()) return std::nullopt;
            QDialog dialog;
            dialog.setObjectName(QStringLiteral("httpAuthenticationPrompt"));
            dialog.setWindowTitle(L(QStringLiteral("Website-Anmeldung")));
            dialog.setWindowModality(Qt::ApplicationModal);
            auto *layout = new QVBoxLayout(&dialog);
            auto *description = new QLabel(
                QStringLiteral("%1 verlangt Anmeldedaten.")
                    .arg(QString::fromStdString(challenge.origin)), &dialog
            );
            description->setWordWrap(true);
            layout->addWidget(description);
            if (!challenge.realm.empty()) {
                auto *realm = new QLabel(
                    QStringLiteral("Bereich: %1").arg(QString::fromStdString(challenge.realm)),
                    &dialog
                );
                realm->setWordWrap(true);
                layout->addWidget(realm);
            }
            auto *form = new QFormLayout();
            auto *user = new QLineEdit(&dialog);
            user->setObjectName(QStringLiteral("httpAuthenticationUser"));
            auto *password = new QLineEdit(&dialog);
            password->setObjectName(QStringLiteral("httpAuthenticationPassword"));
            password->setEchoMode(QLineEdit::Password);
            form->addRow(L(QStringLiteral("Benutzername")), user);
            form->addRow(L(QStringLiteral("Passwort")), password);
            layout->addLayout(form);
            auto *buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog
            );
            buttons->button(QDialogButtonBox::Ok)->setText(L(QStringLiteral("Anmelden")));
            buttons->button(QDialogButtonBox::Cancel)->setText(L(QStringLiteral("Abbrechen")));
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            user->setFocus();
            if (dialog.exec() != QDialog::Accepted) return std::nullopt;
            return engine::AuthenticationCredentials{
                .user = user->text().toStdString(),
                .password = password->text().toStdString(),
            };
        }
    );
    QObject::connect(loginFillButton_, &QPushButton::clicked, this, [this] { showLoginSuggestions(true); });
    QObject::connect(appearanceButton_, &QPushButton::clicked, this, [this] { showAppearanceMenu(); });
    installWebAppearanceScript();
    // The blocker has to be in place before the first page can load, so the
    // interceptor is installed on both stores right away.
    adBlockInterceptor_ = new AdBlockInterceptor(this);
    profile_->persistentProfile()->setUrlRequestInterceptor(adBlockInterceptor_);
    profile_->privateProfile()->setUrlRequestInterceptor(adBlockInterceptor_);
    updateAdBlocking(false);
    // Follow the system switching between light and dark while running.
    QObject::connect(
        QGuiApplication::styleHints(),
        &QStyleHints::colorSchemeChanged,
        this,
        [this](Qt::ColorScheme) {
            applyTheme();
            updateWebAppearance();
        }
    );
    installLoginAutofillScript();
    // No poll timer: the isolated world reports focus and submitted forms over
    // its own channel, which attachLoginChannels() binds per page.
    installPasskeyShim();
    installShortcuts();
    session_.setObserver([this] { synchronizeSession(); });
    session_.setAgentEventObserver([this](const yobro::controller::AgentEvent &event) {
        const QString action = QString::fromStdString(event.action);
        const QString detail = QString::fromStdString(event.detail);
        chat_.append(QStringLiteral("action"), action + QStringLiteral(": ") + detail, QString::fromStdString(event.space));
        if (status_)
            status_->setText(L(QStringLiteral("Zuletzt: "), QStringLiteral("Last: ")) + action);
        refreshAgentActivity();
    });
    // Local development verification only; the session additionally requires
    // YOBRO_DEV_CAPTURE=1 before it exposes the command at all.
    session_.setWindowCapture([this](std::string &error) -> std::optional<std::string> {
        return captureWindowImage(error);
    });
    loadSession(initialUrl);
    synchronizeSession();
    updateWebAppearance();
    if (!startupPolicyProblem.isEmpty())
        status_->setText(startupPolicyProblem);
}

SpikeWindow::~SpikeWindow() {
    session_.setObserver({});
    session_.setAgentEventObserver({});
    session_.setWindowCapture({});
    session_.setPermissionPromptPresenter({});
    session_.setAuthenticationPromptPresenter({});
    session_.setCertificatePromptPresenter({});
    session_.setPasskeyPromptPresenter({});
    session_.setDesktopMediaPromptPresenter({});
    session_.setPermissionSurfaceVisible(false);
    if (permissionDialog_) {
        QObject::disconnect(permissionDialog_, nullptr, this, nullptr);
        permissionDialog_->close();
        permissionDialog_.clear();
        permissionDialogId_ = 0;
    }
    for (QPointer<QWidget> surface : {loginSuggestionPopup_, loginSavePrompt_}) {
        if (surface) surface->close();
    }
    loginSuggestionPopup_.clear();
    loginSavePrompt_.clear();
    for (QPointer<QDialog> dialog : {downloadsDialog_, libraryDialog_, extensionsDialog_, importDialog_, quickSwitcherDialog_, settingsDialog_, passwordsDialog_}) {
        if (dialog) {
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->close();
        }
    }
    downloadsDialog_.clear();
    libraryDialog_.clear();
    extensionsDialog_.clear();
    importDialog_.clear();
    quickSwitcherDialog_.clear();
    settingsDialog_.clear();
    passwordsDialog_.clear();
    profileList_ = nullptr;
    savedPasswordsList_ = nullptr;
    passwordVaultNote_ = nullptr;
    passwordVaultPassphrase_ = nullptr;
    passwordVaultNewPassphrase_ = nullptr;
    library_.setDownloadsObserver({});
    library_.setUserDownloadPrompt({});

    // QTabWidget and layouts reparent their page widgets, but BrowserSession's
    // QtBrowserPage remains their sole C++ owner. Detach every engine-owned
    // view before QMainWindow destroys its QObject child tree so the later
    // BrowserSession teardown deletes each QWebEngineView exactly once.
    if (displayedAgentView_) {
        agentWebLayout_->removeWidget(displayedAgentView_);
        displayedAgentView_ = nullptr;
    }
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        QWebEngineView *view = asQtPage(tab.page)->view();
        const int index = tabs_->indexOf(view);
        if (index >= 0)
            tabs_->removeTab(index);
        if (splitTabs_) {
            const int splitIndex = splitTabs_->indexOf(view);
            if (splitIndex >= 0)
                splitTabs_->removeTab(splitIndex);
        }
        view->hide();
        view->setParent(nullptr);
    }
}

void SpikeWindow::installShortcuts() {
    // Every command lives in the menu bar so the menus and the keys cannot
    // drift apart. Qt maps Qt::CTRL to Command on macOS, so these match the
    // WebKit build without binding the physical Control key.
    auto *bar = menuBar();
    QMenu *fileMenu = bar->addMenu(L(QStringLiteral("Datei")));
    QMenu *browserMenu = bar->addMenu(L(QStringLiteral("Browser")));
    QMenu *tabMenu = bar->addMenu(L(QStringLiteral("Tabs")));

    const auto command = [this](
        QMenu *menu,
        const QString &title,
        const QKeySequence &sequence,
        const QString &objectName,
        std::function<void()> handler
    ) {
        auto *action = new QAction(title, this);
        action->setObjectName(objectName);
        if (!sequence.isEmpty()) action->setShortcut(sequence);
        QObject::connect(action, &QAction::triggered, this, std::move(handler));
        menu->addAction(action);
        // Also owned by the window so the key still works while a dialog of
        // this window holds focus.
        addAction(action);
        return action;
    };

    command(fileMenu, L(QStringLiteral("Neuer Tab")), QKeySequence(Qt::CTRL | Qt::Key_T),
            QStringLiteral("newTabAction"), [this] { newUserTab(); });
    // ⌘N is a new note, exactly as in the WebKit build.
    command(fileMenu, L(QStringLiteral("Neue Notiz"), QStringLiteral("New note")),
            QKeySequence(Qt::CTRL | Qt::Key_N),
            QStringLiteral("newNoteAction"), [this] { (void)newNote(); });
    command(fileMenu, L(QStringLiteral("Neuer privater Tab")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
            QStringLiteral("newPrivateTabAction"), [this] { newUserTab({}, true); });
    command(fileMenu, L(QStringLiteral("Tab schließen")), QKeySequence(Qt::CTRL | Qt::Key_W),
            QStringLiteral("closeTabMenuAction"), [this] {
        if (tabs_ && tabs_->currentIndex() >= 0) closeUserTab(tabs_->currentIndex());
    });
    // WebKit offers this without a key so ⇧⌘T can stay on the private tab.
    command(fileMenu, L(QStringLiteral("Geschlossenen Tab wieder öffnen")), {},
            QStringLiteral("reopenClosedTabMenuAction"), [this] { reopenClosedTab(); });
    fileMenu->addSeparator();
    command(fileMenu, L(QStringLiteral("Seite drucken …")), QKeySequence(Qt::CTRL | Qt::Key_P),
            QStringLiteral("printPageAction"), [this] { printActivePage(); });
    fileMenu->addSeparator();
    auto *settingsAction = command(fileMenu, L(QStringLiteral("Einstellungen …")), QKeySequence(Qt::CTRL | Qt::Key_Comma),
            QStringLiteral("settingsAction"), [this] { showSettings(); });
    // macOS moves this into the application menu.
    settingsAction->setMenuRole(QAction::PreferencesRole);

    command(browserMenu, L(QStringLiteral("Erweiterungen …")), {},
            QStringLiteral("extensionsMenuAction"), [this] { showExtensions(); });
    command(browserMenu, L(QStringLiteral("Daten importieren …")), {},
            QStringLiteral("importMenuAction"), [this] { showWebKitImport(); });
    command(browserMenu, L(QStringLiteral("Seitenleiste ein- oder ausblenden")), QKeySequence(Qt::CTRL | Qt::Key_S),
            QStringLiteral("toggleSidebarAction"), [this] { toggleSidebar(); });
    browserMenu->addSeparator();
    command(browserMenu, L(QStringLiteral("Adresse öffnen")), QKeySequence(Qt::CTRL | Qt::Key_L),
            QStringLiteral("focusAddressAction"), [this] {
        address_->setFocus(Qt::ShortcutFocusReason);
        address_->selectAll();
    });
    command(browserMenu, L(QStringLiteral("Schnellsuche")), QKeySequence(Qt::CTRL | Qt::Key_K),
            QStringLiteral("quickSwitcherAction"), [this] { showQuickSwitcher(); });
    command(browserMenu, L(QStringLiteral("Auf Seite suchen")), QKeySequence(Qt::CTRL | Qt::Key_F),
            QStringLiteral("findInPageAction"), [this] { showFindBar(); });
    command(browserMenu, L(QStringLiteral("Nächster Treffer")), QKeySequence(Qt::CTRL | Qt::Key_G),
            QStringLiteral("findNextAction"), [this] { findInPage(false); });
    command(browserMenu, L(QStringLiteral("Vorheriger Treffer")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G),
            QStringLiteral("findPreviousAction"), [this] { findInPage(true); });
    browserMenu->addSeparator();
    command(browserMenu, L(QStringLiteral("Neu laden")), QKeySequence(Qt::CTRL | Qt::Key_R),
            QStringLiteral("reloadAction"), [this] {
        if (auto *page = currentUserPage()) (void)session_.reloadUserTab(page->state().id);
    });
    command(browserMenu, L(QStringLiteral("Zurück")), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft),
            QStringLiteral("backAction"), [this] {
        if (auto *page = currentUserPage()) (void)session_.goBackUserTab(page->state().id);
    });
    command(browserMenu, L(QStringLiteral("Vorwärts")), QKeySequence(Qt::CTRL | Qt::Key_BracketRight),
            QStringLiteral("forwardAction"), [this] {
        if (auto *page = currentUserPage()) (void)session_.goForwardUserTab(page->state().id);
    });
    browserMenu->addSeparator();
    command(browserMenu, L(QStringLiteral("Seite vergrößern")), QKeySequence(Qt::CTRL | Qt::Key_Plus),
            QStringLiteral("zoomInAction"), [this] { changeZoom(1); });
    // Qt reports the unshifted key on many layouts, so ⌘= is bound as well.
    auto *zoomInAlias = new QAction(L(QStringLiteral("Seite vergrößern")), this);
    zoomInAlias->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal));
    QObject::connect(zoomInAlias, &QAction::triggered, this, [this] { changeZoom(1); });
    addAction(zoomInAlias);
    command(browserMenu, L(QStringLiteral("Seite verkleinern")), QKeySequence(Qt::CTRL | Qt::Key_Minus),
            QStringLiteral("zoomOutAction"), [this] { changeZoom(-1); });
    command(browserMenu, L(QStringLiteral("Originalgröße")), QKeySequence(Qt::CTRL | Qt::Key_0),
            QStringLiteral("zoomResetAction"), [this] { changeZoom(0); });
    browserMenu->addSeparator();
    command(browserMenu, L(QStringLiteral("Seite merken")), QKeySequence(Qt::CTRL | Qt::Key_D),
            QStringLiteral("bookmarkPageAction"), [this] { bookmarkCurrentPage(); });
    command(browserMenu, L(QStringLiteral("Lesezeichen")), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B),
            QStringLiteral("bookmarksAction"), [this] { showLibrary(LibrarySection::bookmarks); });
    command(browserMenu, L(QStringLiteral("Verlauf")), QKeySequence(Qt::CTRL | Qt::Key_Y),
            QStringLiteral("historyAction"), [this] { showLibrary(LibrarySection::history); });
    command(browserMenu, QStringLiteral("Downloads"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J),
            QStringLiteral("downloadsAction"), [this] { showDownloads(); });
    // The same key the WebKit build uses for its mail screen.
    command(browserMenu, QStringLiteral("YoBro Mail"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
            QStringLiteral("mailAction"), [this] { showMail(); });
    browserMenu->addSeparator();
    command(browserMenu, L(QStringLiteral("Agentenpanel")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A),
            QStringLiteral("agentPanelAction"), [this] {
        // Drive the sidebar control so its checked state and label stay correct.
        if (auto *toggle = findChild<QPushButton *>(QStringLiteral("agentPaneToggle")))
            toggle->setChecked(!toggle->isChecked());
        else
            setAgentPaneVisible(!agentPaneVisible_);
    });
    command(browserMenu, L(QStringLiteral("Geteilte Ansicht")), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
            QStringLiteral("splitViewAction"), [this] { toggleSplitView(); });

    // ⌘1–⌘8 pick a tab by position, ⌘9 jumps to the last one.
    for (int number = 1; number <= 9; ++number) {
        const QString title = number == 9
            ? L(QStringLiteral("Letzter Tab"))
            : L(QStringLiteral("Tab %1")).arg(number);
        command(
            tabMenu,
            title,
            QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_0 + number)),
            QStringLiteral("selectTab%1Action").arg(number),
            [this, number] { selectVisibleTab(number - 1); }
        );
    }
    tabMenu->addSeparator();
    command(tabMenu, L(QStringLiteral("Tab duplizieren")), {},
            QStringLiteral("duplicateTabMenuAction"), [this] { duplicateCurrentTab(); });
    command(tabMenu, L(QStringLiteral("Tab anpinnen oder lösen")), {},
            QStringLiteral("togglePinnedMenuAction"), [this] { toggleCurrentTabPinned(); });
    command(tabMenu, L(QStringLiteral("Adresse kopieren")), {},
            QStringLiteral("copyAddressMenuAction"), [this] { copyCurrentAddress(); });

    // Escape closes the find bar and is deliberately not a menu command.
    auto *dismissFind = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    dismissFind->setContext(Qt::WindowShortcut);
    QObject::connect(dismissFind, &QShortcut::activated, this, [this] {
        if (findBar_ && findBar_->isVisible()) hideFindBar();
    });
}

void SpikeWindow::buildCompactSidebar(QWidget *parent) {
    compactSidebar_ = new QWidget(parent);
    compactSidebar_->setObjectName(QStringLiteral("compactSidebar"));
    compactSidebar_->setMinimumWidth(80);
    compactSidebar_->setMaximumWidth(80);
    auto *layout = new QVBoxLayout(compactSidebar_);
    layout->setContentsMargins(9, 12, 9, 12);
    layout->setSpacing(6);

    const auto iconButton = [this](
        const QString &glyph,
        const QString &tooltip,
        const QString &objectName,
        std::function<void()> handler
    ) {
        auto *button = new QPushButton(glyph, compactSidebar_);
        button->setObjectName(objectName);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        QObject::connect(button, &QPushButton::clicked, this, std::move(handler));
        return button;
    };

    layout->addWidget(iconButton(QStringLiteral("❯"), L(QStringLiteral("Seitenleiste einblenden · ⌘S")),
        QStringLiteral("compactExpandButton"), [this] { setSidebarCompact(false); }));
    layout->addWidget(iconButton(QStringLiteral("⌕"), L(QStringLiteral("Adresse öffnen · ⌘L")),
        QStringLiteral("compactAddressButton"), [this] {
            address_->setFocus(Qt::ShortcutFocusReason);
            address_->selectAll();
        }));
    layout->addWidget(iconButton(QStringLiteral("◫"), L(QStringLiteral("Agentenpanel · ⇧⌘A")),
        QStringLiteral("compactAgentButton"), [this] {
            if (auto *toggle = findChild<QPushButton *>(QStringLiteral("agentPaneToggle")))
                toggle->setChecked(!toggle->isChecked());
        }));

    auto *spaceButton = iconButton(QStringLiteral("◈"), L(QStringLiteral("Space wechseln")),
        QStringLiteral("compactSpaceButton"), [this] {
            QMenu menu(compactSidebar_);
            for (const QString &space : spaces_) {
                auto *entry = menu.addAction(spaceIcons_.value(space) + QStringLiteral(" ") + space);
                entry->setCheckable(true);
                entry->setChecked(space == activeSpace_);
                connect(entry, &QAction::triggered, this, [this, space] { switchSpace(space); });
            }
            if (auto *button = findChild<QPushButton *>(QStringLiteral("compactSpaceButton")))
                menu.exec(button->mapToGlobal(QPoint(button->width(), 0)));
        });
    layout->addWidget(spaceButton);

    compactTabs_ = new QListWidget(compactSidebar_);
    compactTabs_->setObjectName(QStringLiteral("compactTabs"));
    compactTabs_->setIconSize(QSize(20, 20));
    compactTabs_->setSelectionMode(QAbstractItemView::SingleSelection);
    compactTabs_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QObject::connect(compactTabs_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item) return;
        (void)session_.setActiveUserTab(item->data(Qt::UserRole).toString().toStdString());
    });
    layout->addWidget(compactTabs_, 1);

    auto *newTab = iconButton(QStringLiteral("+"), L(QStringLiteral("Neuer Tab · ⌘T")),
        QStringLiteral("compactNewTabButton"), [this] { newUserTab(); });
    newTab->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(newTab, &QPushButton::customContextMenuRequested, this, [this, newTab](const QPoint &at) {
        QMenu menu(newTab);
        auto *privateTab = menu.addAction(L(QStringLiteral("Neuer privater Tab")));
        connect(privateTab, &QAction::triggered, this, [this] { newUserTab({}, true); });
        menu.exec(newTab->mapToGlobal(at));
    });
    layout->addWidget(newTab);

    layout->addWidget(iconButton(QStringLiteral("◷"), L(QStringLiteral("Verlauf · ⌘Y")),
        QStringLiteral("compactHistoryButton"), [this] { showLibrary(LibrarySection::history); }));
    layout->addWidget(iconButton(QStringLiteral("⇩"), L(QStringLiteral("Downloads · ⇧⌘J")),
        QStringLiteral("compactDownloadsButton"), [this] { showDownloads(); }));
    layout->addWidget(iconButton(QStringLiteral("●"), L(QStringLiteral("Profile und Einstellungen · ⌘,")),
        QStringLiteral("compactProfileButton"), [this] { showSettings(); }));
}

void SpikeWindow::refreshCompactSidebar() {
    if (!compactTabs_ || !compactSidebar_->isVisible()) return;
    compactTabs_->clear();
    const engine::BrowserPage *activePage = session_.activeUserPage();
    // Same ordering rule as the full sidebar: pinned first, then the rest.
    std::vector<controller::SessionTabView> ordered;
    for (const controller::SessionTabView &tab : session_.tabViews()) {
        if (tab.state.owner == engine::PageOwner::agent) continue;
        const auto workspace = workspaceTabs_.find(tab.state.id);
        const QString tabSpace = workspace == workspaceTabs_.end() ? activeSpace_ : workspace->second.space;
        if (!tab.privatePage && tabSpace != activeSpace_) continue;
        ordered.push_back(tab);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [this](const controller::SessionTabView &left, const controller::SessionTabView &right) {
        const auto leftEntry = workspaceTabs_.find(left.state.id);
        const auto rightEntry = workspaceTabs_.find(right.state.id);
        const bool leftPinned = leftEntry != workspaceTabs_.end() && leftEntry->second.pinned;
        const bool rightPinned = rightEntry != workspaceTabs_.end() && rightEntry->second.pinned;
        if (leftPinned != rightPinned) return leftPinned;
        const int leftOrder = leftEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : leftEntry->second.order;
        const int rightOrder = rightEntry == workspaceTabs_.end() ? std::numeric_limits<int>::max() : rightEntry->second.order;
        return leftOrder < rightOrder;
    });
    for (const controller::SessionTabView &tab : ordered) {
        const QString label = titleFor(tab);
        auto *item = new QListWidgetItem(compactTabs_);
        const QIcon favicon = faviconFor(tab);
        if (favicon.isNull()) {
            // Without an icon a short initial keeps the row identifiable.
            const QString fallback = tab.privatePage
                ? QStringLiteral("◌")
                : (label.isEmpty() ? QStringLiteral("◎") : label.left(1).toUpper());
            item->setText(fallback);
            item->setTextAlignment(Qt::AlignCenter);
        } else {
            item->setIcon(favicon);
        }
        item->setToolTip(label.isEmpty()
            ? QString::fromStdString(tab.state.url)
            : label + QStringLiteral("\n") + QString::fromStdString(tab.state.url));
        item->setData(Qt::UserRole, QString::fromStdString(tab.state.id));
        item->setSizeHint(QSize(46, 40));
        if (tab.page == activePage) compactTabs_->setCurrentItem(item);
    }
}

void SpikeWindow::setSidebarCompact(bool compact) {
    if (!workspaceSidebar_ || !compactSidebar_) return;
    workspaceSidebar_->setVisible(!compact);
    compactSidebar_->setVisible(compact);
    if (compact) refreshCompactSidebar();
    status_->setText(compact
        ? L(QStringLiteral("Seitenleiste eingeklappt. ⌘S klappt sie wieder auf."))
        : L(QStringLiteral("Seitenleiste eingeblendet.")));
}

void SpikeWindow::toggleSidebar() {
    if (!workspaceSidebar_) return;
    setSidebarCompact(workspaceSidebar_->isVisible());
}

} // namespace yobro::spike
