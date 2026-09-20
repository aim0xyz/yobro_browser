import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// The built-in start page, ported from the WebKit `NewTabPage`: radial moss
// glow over paper, the YoBro mark, the wordmark, and the search card.
Item {
    id: startPage

    readonly property bool compact: width < 620 || height < 620

    Image {
        id: glow
        anchors.fill: parent
        source: "image://yobroglow/" + Math.round(startPage.width) + "x" + Math.round(startPage.height)
                + "?dark=" + (Theme.dark ? "1" : "0")
        cache: false
        asynchronous: false
    }

    Column {
        anchors.centerIn: parent
        spacing: startPage.compact ? 14 : 22

        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: startPage.compact ? 6 : 10

            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: startPage.compact ? 52 : 82
                height: width
                source: "qrc:/yobro/YOBROMark.png"
                fillMode: Image.PreserveAspectFit
                antialiasing: true
                mipmap: true
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "YoBro"
                color: Theme.ink
                font.family: Theme.fontRounded
                font.pixelSize: startPage.compact ? 30 : 42
                font.weight: Font.DemiBold
                font.letterSpacing: startPage.compact ? -1.2 : -1.7
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Theme.loc("Wohin geht es als Nächstes?", "Where to next?")
                color: Qt.alpha(Theme.ink, 0.5)
                font.pixelSize: 13
            }
        }

        // The search card wrapper leaves room for the soft shadow the WebKit
        // card casts (black 10%, radius 24, offset y 10).
        Item {
            width: card.width + 96
            height: card.height + 96
            anchors.horizontalCenter: parent.horizontalCenter

            MultiEffect {
                anchors.fill: parent
                source: card
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.10)
                shadowBlur: 0.55
                shadowVerticalOffset: 10
            }

            Rectangle {
                id: card
                width: Math.min(540, Math.max(220, startPage.width - 32))
                height: cardColumn.implicitHeight + 32
                anchors.centerIn: parent
                radius: 18
                color: Qt.alpha(Theme.surface, 0.72)
                border.color: Qt.alpha(Theme.moss, addressField.focus ? 0.5 : 0.16)
                border.width: addressField.focus ? 1.5 : 1

                Column {
                    id: cardColumn
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10

                    TextField {
                        id: addressField
                        width: parent.width
                        height: 48
                        placeholderText: Theme.loc("Suchen oder URL eingeben", "Search or enter URL")
                        font.pixelSize: 16
                        font.family: Theme.fontText
                        color: Theme.ink
                        placeholderTextColor: Qt.alpha(Theme.ink, 0.35)
                        selectionColor: Theme.selectionWash
                        verticalAlignment: TextInput.AlignVCenter
                        leftPadding: 40
                        rightPadding: 14
                        background: Rectangle {
                            radius: 12
                            color: Theme.field
                            border.color: Qt.alpha(Theme.moss, addressField.focus ? 0.5 : 0.16)
                            border.width: addressField.focus ? 1.5 : 1
                        }

                        Image {
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            source: Theme.iconUrl("search", "accent")
                        }

                        onAccepted: Browser.openAddress(text)
                    }

                    Text {
                        text: Theme.loc("Adresse eingeben oder direkt im Web suchen · Enter zum Öffnen",
                                "Enter an address or search the web · Press Enter to open")
                        color: Qt.alpha(Theme.ink, 0.45)
                        font.pixelSize: 10
                        leftPadding: 4
                    }
                }
            }
        }
    }

    Component.onCompleted: addressField.forceActiveFocus()
}
