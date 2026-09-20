#include "spike/qml/QmlBrowserModel.hpp"

#include "spike/Localization.hpp"

#include <QUrlQuery>

namespace yobro::spike {

QmlBrowserModel::QmlBrowserModel(QString initialUrl, QObject *parent)
    : QObject(parent),
      initialUrl_(std::move(initialUrl)) {}

int QmlBrowserModel::actionNavigate() const { return static_cast<int>(NavigationAction::Navigate); }
int QmlBrowserModel::actionBack() const { return static_cast<int>(NavigationAction::Back); }
int QmlBrowserModel::actionForward() const { return static_cast<int>(NavigationAction::Forward); }
int QmlBrowserModel::actionReload() const { return static_cast<int>(NavigationAction::Reload); }
int QmlBrowserModel::actionStop() const { return static_cast<int>(NavigationAction::Stop); }

QString QmlBrowserModel::tabTitle() const {
    if (!title_.isEmpty()) return title_;
    const QString host = url_.host();
    if (!host.isEmpty()) return host;
    return L(QStringLiteral("Neuer Tab"), QStringLiteral("New tab"));
}

QUrl QmlBrowserModel::pageUrl() const { return url_; }
bool QmlBrowserModel::loading() const { return loading_; }
int QmlBrowserModel::loadProgress() const { return loadProgress_; }
bool QmlBrowserModel::canGoBack() const { return canGoBack_; }
bool QmlBrowserModel::canGoForward() const { return canGoForward_; }
int QmlBrowserModel::tabCount() const { return 1; }

QString QmlBrowserModel::pageHost() const { return url_.host(); }

bool QmlBrowserModel::isStartPage() const {
    return url_.isEmpty() || url_ == QUrl(QStringLiteral("about:blank"));
}

void QmlBrowserModel::openAddress(const QString &input) {
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) return;
    QUrl target;
    if (trimmed.contains(QLatin1Char(' ')) && !trimmed.contains(QStringLiteral("://"))) {
        target = QUrl(QStringLiteral("https://duckduckgo.com/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), trimmed);
        target.setQuery(query);
    } else {
        target = QUrl::fromUserInput(trimmed);
    }
    if (!target.isValid() || (target.scheme() != QStringLiteral("http") && target.scheme() != QStringLiteral("https"))) {
        status_ = L(QStringLiteral("Nur gültige HTTP- und HTTPS-Adressen werden unterstützt."),
                    QStringLiteral("Only valid HTTP and HTTPS addresses are supported."));
        emit statusNotice(status_);
        return;
    }
    status_.clear();
    requestNavigation(NavigationAction::Navigate, target);
}

void QmlBrowserModel::goBack() { requestNavigation(NavigationAction::Back, {}); }
void QmlBrowserModel::goForward() { requestNavigation(NavigationAction::Forward, {}); }
void QmlBrowserModel::reload() { requestNavigation(NavigationAction::Reload, {}); }
void QmlBrowserModel::stop() { requestNavigation(NavigationAction::Stop, {}); }

void QmlBrowserModel::newTab() { requestNavigation(NavigationAction::Navigate, {}); }

void QmlBrowserModel::requestNavigation(NavigationAction action, const QUrl &url) {
    emit navigationRequested(static_cast<int>(action), url);
}

void QmlBrowserModel::reportPageState(
    const QUrl &url,
    const QString &title,
    bool loading,
    int progress,
    bool canGoBack,
    bool canGoForward
) {
    // about:blank is the start page's underlying address; the shell shows it
    // as "no page", like the WebKit app shows an empty URL.
    const QUrl effective = url == QUrl(QStringLiteral("about:blank")) ? QUrl() : url;
    const bool changed = url_ != effective || title_ != title || loading_ != loading ||
                         loadProgress_ != progress || canGoBack_ != canGoBack ||
                         canGoForward_ != canGoForward;
    if (!changed) return;
    url_ = effective;
    title_ = title;
    loading_ = loading;
    loadProgress_ = progress;
    canGoBack_ = canGoBack;
    canGoForward_ = canGoForward;
    emit pageStateChanged();
}

void QmlBrowserModel::attachView() {
    if (!initialUrl_.isEmpty()) {
        const QUrl target = QUrl::fromUserInput(initialUrl_);
        initialUrl_.clear();
        if (target.isValid())
            requestNavigation(NavigationAction::Navigate, target);
    }
}

} // namespace yobro::spike
