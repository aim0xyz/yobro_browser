#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

namespace yobro::spike {

/// The browser state the QML shell reads, and the few validated actions it may
/// perform. This is the first slice of the plan's `BrowserUiModel` bridge:
/// one real tab, real address navigation, page title and load state.
///
/// The QML `WebEngineView` is the web process face; it reports its page state
/// back through `reportPageState`, while every state-changing action flows the
/// other way: chrome → `openAddress`/`goBack`/… → `navigationRequested` → the
/// view applies it. QML never invents navigation decisions on its own.
class QmlBrowserModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString tabTitle READ tabTitle NOTIFY pageStateChanged)
    Q_PROPERTY(QUrl pageUrl READ pageUrl NOTIFY pageStateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY pageStateChanged)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY pageStateChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY pageStateChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY pageStateChanged)
    /// Host of the open page, empty on the start page.
    Q_PROPERTY(QString pageHost READ pageHost NOTIFY pageStateChanged)
    /// True while the tab shows the built-in start page (no web content yet).
    Q_PROPERTY(bool isStartPage READ isStartPage NOTIFY pageStateChanged)
    /// Exactly one tab exists in this first slice.
    Q_PROPERTY(int tabCount READ tabCount CONSTANT)

public:
    /// The navigation actions the view can be asked to apply. They are also
    /// exposed as plain int properties because QML cannot read Q_ENUM values
    /// through a context-property instance.
    enum class NavigationAction { Navigate, Back, Forward, Reload, Stop };
    Q_ENUM(NavigationAction)
    Q_PROPERTY(int actionNavigate READ actionNavigate CONSTANT)
    Q_PROPERTY(int actionBack READ actionBack CONSTANT)
    Q_PROPERTY(int actionForward READ actionForward CONSTANT)
    Q_PROPERTY(int actionReload READ actionReload CONSTANT)
    Q_PROPERTY(int actionStop READ actionStop CONSTANT)

    explicit QmlBrowserModel(QString initialUrl, QObject *parent = nullptr);

    [[nodiscard]] QString tabTitle() const;
    [[nodiscard]] QUrl pageUrl() const;
    [[nodiscard]] bool loading() const;
    [[nodiscard]] int loadProgress() const;
    [[nodiscard]] bool canGoBack() const;
    [[nodiscard]] bool canGoForward() const;
    [[nodiscard]] QString pageHost() const;
    [[nodiscard]] bool isStartPage() const;
    [[nodiscard]] int tabCount() const;
    [[nodiscard]] int actionNavigate() const;
    [[nodiscard]] int actionBack() const;
    [[nodiscard]] int actionForward() const;
    [[nodiscard]] int actionReload() const;
    [[nodiscard]] int actionStop() const;

    /// Normalises the typed text exactly like the widget shell's address field:
    /// words without a scheme become a DuckDuckGo search, everything else goes
    /// through `QUrl::fromUserInput`, and only http(s) is accepted.
    Q_INVOKABLE void openAddress(const QString &input);
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void stop();
    /// Starts the tab over: back to the start page, like a fresh ⌘T.
    Q_INVOKABLE void newTab();
    /// The view reports its page state here after every change.
    Q_INVOKABLE void reportPageState(
        const QUrl &url,
        const QString &title,
        bool loading,
        int progress,
        bool canGoBack,
        bool canGoForward
    );
    /// The QML pane calls this once it exists so a startup URL is applied.
    Q_INVOKABLE void attachView();

    [[nodiscard]] bool statusVisible() const { return !status_.isEmpty(); }
    [[nodiscard]] QString status() const { return status_; }

signals:
    void pageStateChanged();
    void navigationRequested(int action, const QUrl &url);
    void statusNotice(const QString &text);

private:
    void requestNavigation(NavigationAction action, const QUrl &url);

    QString initialUrl_;
    QString status_;
    QString title_;
    QUrl url_;
    bool loading_ = false;
    int loadProgress_ = 0;
    bool canGoBack_ = false;
    bool canGoForward_ = false;
};

} // namespace yobro::spike
