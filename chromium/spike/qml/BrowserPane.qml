import QtQuick
import QtWebEngine

// The content of the paper card: the QML start page while the tab is empty,
// the real WebEngineView once an address is open, plus the transient moss
// load-progress bar at the card's bottom edge.
Item {
    id: pane

    StartPage {
        anchors.fill: parent
        visible: Browser.isStartPage
    }

    WebEngineView {
        id: webView
        anchors.fill: parent
        visible: !Browser.isStartPage
        focus: !Browser.isStartPage
        profile: WebProfile

        onUrlChanged: pane.reportState()
        onTitleChanged: pane.reportState()
        onLoadingChanged: pane.reportState()
        onLoadProgressChanged: pane.reportState()
        onNavigationRequested: function(request) {
            // One window: every navigation stays inside this view.
            request.accept()
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        height: 2
        width: parent.width * Math.max(0.03, Browser.loadProgress / 100)
        color: Theme.moss
        opacity: 0.7
        visible: Browser.loading
    }

    Connections {
        target: Browser
        function onNavigationRequested(action, url) {
            switch (action) {
            case Browser.actionNavigate:
                // An empty URL means "back to the start page" (⌘T).
                webView.url = url.toString().length ? url : "about:blank"
                break
            case Browser.actionBack:
                webView.goBack()
                break
            case Browser.actionForward:
                webView.goForward()
                break
            case Browser.actionReload:
                webView.reload()
                break
            case Browser.actionStop:
                webView.stop()
                break
            }
        }
    }

    Component.onCompleted: {
        pane.reportState()
        Browser.attachView()
    }

    function reportState() {
        Browser.reportPageState(
            webView.url,
            webView.title,
            webView.loading,
            webView.loadProgress,
            webView.canGoBack,
            webView.canGoForward
        )
    }
}
