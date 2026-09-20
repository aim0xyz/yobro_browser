import QtQuick

// The YoBro product shell in Qt Quick: chrome gradient, sidebar and the
// paper content card, mirroring the WebKit `BrowserShell` layout. All colours
// come from the shared Theme layer; nothing here hardcodes a palette value.
Window {
    id: shell

    width: 1360
    height: 880
    minimumWidth: 960
    minimumHeight: 640
    visible: true
    title: qsTr("YoBro")
    color: Theme.chromeBottom

    Rectangle {
        anchors.fill: parent
        // The WebKit shell draws LinearGradient(chromeTop → chromeBottom)
        // from topLeading to bottomTrailing; a vertical two-stop gradient is
        // the closest QML primitive and keeps the same identity.
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.chromeTop }
            GradientStop { position: 1.0; color: Theme.chromeBottom }
        }
    }

    Sidebar {
        id: sidebar
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 248
    }

    // The content card: paper with a hairline border, floating on the chrome
    // with the same 10pt inset the WebKit workspace uses.
    Rectangle {
        id: contentCard
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 10
        radius: 14
        color: Theme.paper
        border.color: Theme.borderHairline
        border.width: 1
        clip: true

        BrowserPane {
            anchors.fill: parent
        }
    }
}
