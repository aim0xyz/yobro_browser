#pragma once

#include "spike/AdBlockInterceptor.hpp"
#include "spike/AdBlockSettings.hpp"
#include "spike/BrowserDataImport.hpp"
#include "spike/CertificateTrust.hpp"
#include "spike/ChromeStore.hpp"
#include "spike/LoginChannel.hpp"
#include "spike/NoteEditor.hpp"
#include "spike/NativePasswordImport.hpp"
#include "spike/PageZoomStore.hpp"
#include "spike/SpaceProxyController.hpp"
#include "spike/SpaceProxyStore.hpp"
#include "spike/PasswordVault.hpp"
#include "spike/MailPanel.hpp"
#include "spike/MailStore.hpp"
#include "spike/OnboardingDialog.hpp"
#include "spike/OnboardingProgress.hpp"
#include "spike/SyncAccount.hpp"
#include "spike/PrivacySettings.hpp"
#include "spike/ProfileRegistry.hpp"
#include "spike/SpaceChat.hpp"
#include "spike/SpaceChatPanel.hpp"
#include "spike/WebAppearanceStore.hpp"
#include "yobro/controller/BrowserSession.hpp"
#include "yobro/core/ProfilePaths.hpp"
#include "yobro/core/WebKitImport.hpp"

#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QCloseEvent;
class QDialog;
class QHideEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QMessageBox;
class QPlainTextEdit;
class QPoint;
class QShowEvent;
class QSplitter;
class QTabWidget;
class QTimer;
class QWebChannel;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QVBoxLayout;
class QWebEngineView;

namespace yobro::qtwebengine {
class QtBrowserLibrary;
class QtBrowserPage;
class QtBrowserProfile;
}

namespace yobro::spike {

class BridgePolicyStore;

/// The assistant is fed by the window itself: the tools it may use are notes,
/// tabs and an approved page open, all of which only the window knows about.
class SpikeWindow final : public QMainWindow, public SpaceChatHost {
public:
    using FileRevealHandler = std::function<bool(const QString &path)>;

    SpikeWindow(
        controller::BrowserSession &session,
        qtwebengine::QtBrowserLibrary &library,
        BridgePolicyStore &bridgePolicy,
        core::ProfilePaths paths,
        QString initialUrl = {},
        QString startupPolicyProblem = {}
    );
    ~SpikeWindow() override;

    // Package/protocol harnesses explicitly disable the grant-capable surface.
    void setPermissionSurfaceAllowed(bool allowed);
    void setFileRevealHandler(FileRevealHandler handler);

    /// Asked for another profile in this window. The host tears the session down
    /// and rebuilds it for the requested profile, because storage, agent socket
    /// and extensions all belong to exactly one profile.
    using ProfileSwitchHandler = std::function<void(const std::string &profileId)>;
    void setProfileSwitchHandler(ProfileSwitchHandler handler);

    // The assistant's view of this window. Everything is scoped to the active
    // space, and nothing here changes a page the user is looking at.
    [[nodiscard]] QString space() const override;
    [[nodiscard]] bool agentAllowed() const override;
    [[nodiscard]] QJsonArray inventory() const override;
    [[nodiscard]] QJsonArray listNotes() const override;
    QString createNote(const QString &title, const QString &content) override;
    [[nodiscard]] std::optional<QJsonObject> readNote(const QString &id) const override;
    bool writeNote(const QString &id, const QString &content, const QString &mode) override;
    void readTab(const QString &id, std::function<void(std::optional<QJsonObject>)> done) override;
    void requestOpenUrl(const QUrl &url, std::function<void(std::optional<QString>)> done) override;
    void readMail(int limit, std::function<void(std::optional<QJsonObject>)> done) override;
    void readMailMessage(
        const QString &id,
        std::function<void(std::optional<QJsonObject>)> done
    ) override;

    /// The conversation and the turn driver, so a harness can supply its own
    /// transport instead of reaching a model over the network.
    [[nodiscard]] SpaceChatStore &assistantStore() { return chat_; }
    [[nodiscard]] SpaceChatRunner *assistantRunner() { return chatRunner_; }
    /// The mailbox state, so a harness can supply its own mail transport.
    [[nodiscard]] MailStore &mailStore() { return mail_; }

    /// Shows one step of a passkey conversation. Public because no test machine
    /// has an authenticator that could produce the real states.
    /// Shows the screen and window picker for one `getDisplayMedia` call and
    /// answers before returning. Public because no headless test machine
    /// produces a request on its own.
    void presentDesktopMediaPicker(
        const engine::DesktopMediaRequest &request,
        const engine::DesktopMediaControls &controls
    );
    void presentPasskeyStep(
        const engine::PasskeyRequest &request,
        const engine::PasskeyControls &controls
    );
    /// The wording for a failed passkey request.
    [[nodiscard]] static QString passkeyFailureText(engine::PasskeyFailure failure);
    /// The page the user is looking at. Public so a harness can inspect the
    /// document that the credential surfaces are bound to.
    [[nodiscard]] qtwebengine::QtBrowserPage *visibleUserPage() const { return currentUserPage(); }
    /// Opens the built-in mail window, creating it on first use.
    void showMail();
    /// Opens the first-run setup. Reachable from the settings at any time.
    void showOnboarding();
    /// Opens the setup when this profile has not finished it yet, at most once
    /// per window. Returns true when it was opened.
    bool showOnboardingIfNeeded();

    /// Writes imported data into this profile and returns what happened, one
    /// sentence per line. `problem` is filled instead when nothing was written.
    ///
    /// Shared by the settings dialog and the first-run setup, so both write the
    /// same things in the same way.
    QStringList applyImportedBrowserData(const ImportedBrowserData &data, QString *problem);

    /// The account and the sync cycle, so a harness can supply its own backend.
    [[nodiscard]] SyncController *syncController() { return sync_; }
    /// This window's state in the form that is safe to sync.
    [[nodiscard]] SyncSnapshot buildSyncSnapshot() const;
    /// Writes a merged snapshot back. Returns a problem, or an empty string.
    QString applySyncSnapshot(const SyncSnapshot &snapshot, SyncTabResolution resolution);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    qtwebengine::QtBrowserPage *newUserTab(const QString &url = {}, bool privatePage = false);
    qtwebengine::QtBrowserPage *currentUserPage() const;
    qtwebengine::QtBrowserPage *activeAgentPage(bool createIfMissing = false);
    void closeUserTab(int index);
    void captureClosedPublicTab(qtwebengine::QtBrowserPage *page);
    void reopenClosedTab();
    void updateClosedTabAction();
    void navigateCurrent();
    void sendCurrentToAgent();
    void readAgentPage();
    void installUnpackedExtension();
    void showExtensions();
    void refreshExtensions();
    void loadSession(const QString &initialUrl);
    void saveSession() const;
    void synchronizeSession();
    void synchronizePermissionPrompt();
    void updateNavigationState();
    void showDownloads();
    void refreshDownloads();
    enum class LibrarySection { history, bookmarks };
    void showLibrary(LibrarySection section = LibrarySection::history);
    void refreshLibrary();
    void renameSelectedBookmark();
    void moveSelectedBookmark();
    void changeZoom(int direction);
    void applyStoredZoom();
    void printActivePage();
    void selectVisibleTab(int index);
    void copyCurrentAddress();
    void bookmarkCurrentPage();
    void switchSpace(const QString &space);
    void createSpace();
    void createFolder();
    void renameSpace(const QString &space);
    void setSpaceIcon(const QString &space);
    void renameFolder(const QString &folderId);
    void setFolderColor(const QString &folderId);
    void dissolveFolder(const QString &folderId);
    void showWorkspaceContextMenu(const QPoint &position);
    void moveCurrentTabToFolder();
    [[nodiscard]] QIcon faviconFor(const controller::SessionTabView &tab) const;
    [[nodiscard]] QString titleFor(const controller::SessionTabView &tab) const;
    void refreshWorkspaceSidebar();
    void scheduleWorkspaceOrderCommit();
    void applyWorkspaceOrderFromSidebar();
    void activateWorkspaceItem(QTreeWidgetItem *item);
    void setWorkspaceGroupCollapsed(QTreeWidgetItem *item, bool collapsed);
    void setAgentPaneVisible(bool visible);
    void toggleCurrentTabPinned();
    void showWebKitImport();
    void refreshWebKitImportPreview();
    void commitWebKitImport();
    void applyTheme();
    /// Sets the transient status line and shows it for a few seconds — the
    /// WebKit build has no permanent status bar.
    void showStatus(const QString &text);
    void clearBrowsingData(int rangeIndex, bool includeHistory);
    void installWebAppearanceScript();
    void updateWebAppearance();
    /// Adds a note tab in the active space and shows it.
    NoteEditor *newNote(const QString &title = {}, const QString &html = {});
    /// The note behind a sidebar id, or nothing when the id is a page.
    [[nodiscard]] NoteEditor *noteEditorFor(const QString &id) const;
    /// The id of a note widget, or empty when the widget is not a note.
    [[nodiscard]] QString noteIdFor(const QWidget *widget) const;
    /// Brings a note tab to the front, switching space if needed.
    void activateNote(const QString &id);
    /// Removes a note tab and its stored content.
    void closeNote(const QString &id);
    /// Fetches an extension from the Chrome Web Store, shows what it asks for and
    /// installs it after a confirmation. Returns true when the install started.
    bool installExtensionFromStore(QWidget *parent, const QString &input);
    /// Looks for profiles of other browsers on this Mac.
    void refreshBrowserImportProfiles();
    /// Reads the selected kinds without writing anything.
    void previewBrowserImport();
    /// Writes the previewed data into this profile after a confirmation.
    void commitBrowserImport();

    /// Reads logins out of another browser on this Mac and stores them in this
    /// profile's vault after a confirmation.
    void importPasswordsFromBrowser(
        QWidget *parent,
        const QString &browser,
        const QString &profilePath,
        const QString &primaryPassword
    );
    /// Applies the active space's proxy to the whole application and records a
    /// failure that must block navigation.
    void applySpaceProxy();
    /// Injects the cosmetic and page-level ad filters shared with the WebKit build.
    void installAdBlockScripts();
    /// Makes a passkey request fail fast while the engine cannot answer one.
    /// Does nothing once `QtBrowserProfile::passkeysWork()` is true.
    void installPasskeyShim();
    /// Applies both ad-blocking switches to the interceptor, the injected
    /// scripts and every open page.
    void updateAdBlocking(bool reloadPages);
    void showAppearanceMenu();
    [[nodiscard]] bool systemPrefersDark() const;
    [[nodiscard]] std::string currentHost() const;
    void installLoginAutofillScript();
    void attachLoginChannels();
    void handleLoginEvent(const QString &pageId, const QJsonObject &event);
    void handleLoginFocus();
    void handleLoginCapture(const QString &username, const QString &password);
    void showLoginSuggestions(bool userRequested);
    void fillLogin(const LoginCredential &credential);
    void showSavedPasswords();
    void refreshSavedPasswords();
    void showSettings();
    void refreshProfileList();
    void activateProfile(const QString &profileId);
    bool launchProfileInstance(const QString &profileId);
    void createProfile();
    void renameProfile(const QString &profileId);
    void setProfileIcon(const QString &profileId);
    [[nodiscard]] QString profileLabel(const QString &profileId) const;
    void updateProfileFooter();
    void installShortcuts();
    void toggleSplitView();
    void clearSplitView();
    void toggleSidebar();
    void setSidebarCompact(bool compact);
    void buildCompactSidebar(QWidget *parent);
    void refreshCompactSidebar();
    void duplicateCurrentTab();
    void refreshAgentActivity();
    /// Writes window.png next to the profile. Development verification only.
    [[nodiscard]] std::optional<std::string> captureWindowImage(std::string &error);
    void showFindBar();
    void hideFindBar();
    void findInPage(bool backwards);
    void showQuickSwitcher();

    struct WorkspaceTab {
        QString space;
        QString folder;
        bool pinned = false;
        // Sidebar position within its space; drag and drop rewrites this.
        int order = 0;
    };
    struct WorkspaceFolder {
        QString id;
        QString name;
        QString space;
        QString color;
    };
    /// A note tab. Notes are host-owned widgets in the same tab bar as pages, so
    /// they share the space, folder and order handling through `workspaceTabs_`
    /// but have no engine page behind them.
    struct NoteTab {
        QString id;
        NoteEditor *editor = nullptr;
    };
    struct ClosedTab {
        QString url;
        QString title;
        QString space;
        QString folder;
        bool pinned = false;
    };

    controller::BrowserSession &session_;
    qtwebengine::QtBrowserLibrary &library_;
    BridgePolicyStore &bridgePolicy_;
    qtwebengine::QtBrowserProfile *profile_ = nullptr;
    core::ProfilePaths paths_;
    QTabWidget *tabs_ = nullptr;
    QTabWidget *splitTabs_ = nullptr;
    QSplitter *contentSplitter_ = nullptr;
    std::string splitPageId_;
    QSet<QString> faviconWatched_;
    /// Icons restored from the session, shown until the live favicon arrives.
    std::map<std::string, QIcon, std::less<>> restoredFavicons_;
    /// Titles from the session, shown until the page reports its own again.
    std::map<std::string, QString, std::less<>> restoredTitles_;
    QString restoreSplitUrl_;
    int restoreActiveIndex_ = -1;
    QWidget *workspaceSidebar_ = nullptr;
    QWidget *compactSidebar_ = nullptr;
    QListWidget *compactTabs_ = nullptr;
    QWidget *findBar_ = nullptr;
    QLineEdit *findInput_ = nullptr;
    QLabel *findStatus_ = nullptr;
    QTreeWidget *workspaceTree_ = nullptr;
    QSplitter *mainSplitter_ = nullptr;
    QWidget *agentPane_ = nullptr;
    QComboBox *spacePicker_ = nullptr;
    QComboBox *folderPicker_ = nullptr;
    QPushButton *spaceStripButton_ = nullptr;
    QPushButton *agentPaneToggle_ = nullptr;
    QCheckBox *libraryAccess_ = nullptr;
    QPushButton *agentQuickAccess_ = nullptr;
    QLineEdit *address_ = nullptr;
    QLabel *status_ = nullptr;
    QTimer *statusHideTimer_ = nullptr;
    QLabel *profileStatus_ = nullptr;
    QLabel *agentPlaceholder_ = nullptr;
    QListWidget *agentActivity_ = nullptr;
    QPlainTextEdit *agentOutput_ = nullptr;
    QPushButton *back_ = nullptr;
    QPushButton *forward_ = nullptr;
    QPushButton *reload_ = nullptr;
    QPushButton *sidebarToggle_ = nullptr;
    QPushButton *reopenClosedTabButton_ = nullptr;
    QVBoxLayout *agentWebLayout_ = nullptr;
    QPointer<QWebEngineView> displayedAgentView_;
    QPointer<QMessageBox> permissionDialog_;
    QPointer<QDialog> downloadsDialog_;
    QPointer<QDialog> libraryDialog_;
    QPointer<QDialog> extensionsDialog_;
    QPointer<QDialog> importDialog_;
    QPointer<QDialog> quickSwitcherDialog_;
    QPointer<QDialog> settingsDialog_;
    QListWidget *profileList_ = nullptr;
    QPushButton *profileFooter_ = nullptr;
    PasswordVault passwords_;
    ProfileRegistry profiles_;
    PrivacySettings privacy_;
    ProfileSwitchHandler profileSwitchHandler_;
    PageZoomStore zoom_;
    WebAppearanceStore appearance_;
    AdBlockSettings adBlock_;
    AdBlockInterceptor *adBlockInterceptor_ = nullptr;
    CertificateTrustStore certificates_;
    QComboBox *browserImportPicker_ = nullptr;
    QLabel *browserImportPreview_ = nullptr;
    QPushButton *browserImportCommit_ = nullptr;
    QMap<QString, QCheckBox *> browserImportKinds_;
    std::vector<ImportProfileEntry> browserImportProfiles_;
    /// The last preview, kept so the commit writes exactly what was shown.
    std::optional<ImportedBrowserData> browserImportData_;
    SpaceProxyStore proxies_;
    SpaceChatStore chat_;
    SpaceChatRunner *chatRunner_ = nullptr;
    SpaceChatPanel *chatPanel_ = nullptr;
    MailStore mail_;
    MailPanel *mailPanel_ = nullptr;
    QPointer<QDialog> mailDialog_;
    QPointer<OnboardingDialog> onboardingDialog_;
    SyncController *sync_ = nullptr;
    QLabel *syncStatus_ = nullptr;
    QLabel *syncRecovery_ = nullptr;
    QPointer<QDialog> passkeyDialog_;
    QListWidget *passkeyAccounts_ = nullptr;
    QLineEdit *passkeyPin_ = nullptr;
    QLabel *passkeyMessage_ = nullptr;
    QPushButton *passkeyContinue_ = nullptr;
    QPushButton *passkeyRetry_ = nullptr;
    /// The controls of the request the dialog currently shows.
    engine::PasskeyControls passkeyControls_;
    /// True once the first-run check ran, so showing the window again does not
    /// bring the setup back.
    bool onboardingChecked_ = false;
    /// Non-empty while the active space wants a proxy that could not be applied.
    /// Navigation is refused in that state, because loading without the proxy
    /// would send the traffic out directly.
    QString proxyIsolationFailure_;
    QPushButton *appearanceButton_ = nullptr;
    QTabWidget *libraryEntries_ = nullptr;
    QPointer<QDialog> passwordsDialog_;
    QPointer<QWidget> loginSuggestionPopup_;
    QPointer<QWidget> loginSavePrompt_;
    QListWidget *savedPasswordsList_ = nullptr;
    // Only created for the passphrase-backed password store.
    QLabel *passwordVaultNote_ = nullptr;
    QLineEdit *passwordVaultPassphrase_ = nullptr;
    QLineEdit *passwordVaultNewPassphrase_ = nullptr;
    QPushButton *loginFillButton_ = nullptr;
    QString loginSuggestionOrigin_;
    /// One QWebChannel per non-private user page, keyed by page id.
    std::map<std::string, QPointer<QWebChannel>> loginChannels_;
    QLineEdit *importSource_ = nullptr;
    QLabel *importPreview_ = nullptr;
    QPushButton *commitImport_ = nullptr;
    std::filesystem::path importSourcePath_;
    std::optional<core::WebKitImportPreview> importPlan_;
    QListWidget *extensionsList_ = nullptr;
    QPushButton *toggleExtension_ = nullptr;
    QPushButton *removeExtension_ = nullptr;
    QListWidget *downloadsList_ = nullptr;
    QListWidget *historyList_ = nullptr;
    QListWidget *bookmarksList_ = nullptr;
    QLineEdit *librarySearch_ = nullptr;
    QComboBox *bookmarkFolderFilter_ = nullptr;
    QComboBox *bookmarkSort_ = nullptr;
    std::vector<NoteTab> notes_;
    /// Counts up so every note gets an id of its own within this profile.
    int nextNoteNumber_ = 1;
    QStringList spaces_{QStringLiteral("Personal")};
    QString activeSpace_{QStringLiteral("Personal")};
    QHash<QString, QString> spaceIcons_;
    std::map<std::string, WorkspaceTab, std::less<>> workspaceTabs_;
    std::vector<WorkspaceFolder> workspaceFolders_;
    std::vector<ClosedTab> closedTabs_;
    QSet<QString> collapsedFolderIds_;
    std::uint64_t closedTabUndoToken_ = 0;
    int nextWorkspaceOrder_ = 0;
    bool workspaceOrderCommitPending_ = false;
    QLabel *downloadsEmptyTitle_ = nullptr;
    QLabel *downloadsEmptyDetail_ = nullptr;
    QLabel *downloadsFooter_ = nullptr;
    QPushButton *cancelDownload_ = nullptr;
    QPushButton *revealDownload_ = nullptr;
    FileRevealHandler fileRevealHandler_;
    std::uint64_t permissionDialogId_ = 0;
    bool agentPaneVisible_ = false;
    bool permissionSurfaceAllowed_ = true;
    bool synchronizing_ = false;
};

} // namespace yobro::spike
