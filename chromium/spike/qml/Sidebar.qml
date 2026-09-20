import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The sidebar, ported from the WebKit shell: history controls, brand, the
// address bar with its quiet buttons, mail, the space strip, the tab list
// with its actions, the library strip, the agent card and the profile footer.
// Layout numbers follow the WebKit views (248 frame, 17 gutters, 44 rows).
// Only navigation is wired in this slice; everything else renders statically.
Item {
    id: sidebar

    readonly property bool macChrome: Qt.platform.os === "osx"

    // The address bar shows host or title, like the WebKit closed address state.
    readonly property string addressDisplay: {
        if (Browser.isStartPage) return ""
        if (Browser.pageHost !== "") return Browser.pageHost
        return Browser.tabTitle
    }
    property bool addressEditing: false
    property string addressDraft: ""

    function openAddressEdit() {
        addressDraft = addressDisplay
        addressEditing = true
        addressInput.text = addressDraft
        addressInput.forceActiveFocus()
        addressInput.selectAll()
    }

    function closeAddressEdit() {
        addressEditing = false
        addressDraft = ""
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 17
        anchors.rightMargin: 17
        anchors.topMargin: 10
        anchors.bottomMargin: 12
        spacing: 0

        // ── Traffic-light clearance and history controls ─────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            spacing: 0

            // A fillHeight child inside a nested RowLayout would inflate the
            // row itself, so every child pins its height explicitly.
            Item { Layout.preferredWidth: sidebar.macChrome ? 60 : 4; Layout.preferredHeight: 32 }

            Item { Layout.fillWidth: true; Layout.preferredHeight: 32 }

            ChromeIconButton {
                objectName: "backButton"
                iconName: "chevron-left"
                tip: qsTr("Zurück")
                enabled: Browser.canGoBack
                onClicked: Browser.goBack()
            }
            ChromeIconButton {
                objectName: "forwardButton"
                iconName: "chevron-right"
                tip: qsTr("Vorwärts")
                enabled: Browser.canGoForward
                onClicked: Browser.goForward()
            }
            ChromeIconButton {
                objectName: "reloadButton"
                iconName: Browser.loading ? "x" : "rotate-cw"
                tip: Browser.loading ? qsTr("Laden stoppen") : qsTr("Neu laden")
                enabled: !Browser.isStartPage
                onClicked: Browser.loading ? Browser.stop() : Browser.reload()
            }
            ChromeIconButton {
                objectName: "sidebarToggleButton"
                iconName: "panel-left"
                tip: qsTr("Seitenleiste ein-/ausklappen (⌘S)")
            }
        }

        // ── Brand ────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            spacing: 9

            Item { Layout.preferredWidth: 2 }

            Image {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                source: "qrc:/yobro/YOBROMark.png"
                fillMode: Image.PreserveAspectFit
                mipmap: true
            }
            Text {
                text: "YoBro"
                color: Theme.ink
                font.family: Theme.fontRounded
                font.pixelSize: 29
                font.weight: Font.DemiBold
                font.letterSpacing: -1.4
            }
            Item { Layout.fillWidth: true }
            Text {
                text: "PREVIEW"
                color: Theme.moss
                font.family: Theme.fontMono
                font.pixelSize: 8
                font.weight: Font.Bold
                font.letterSpacing: 1
            }
        }

        // ── Address bar ──────────────────────────────────────────────────
        Rectangle {
            id: addressBar
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.preferredHeight: 48
            radius: 12
            color: sidebar.addressEditing ? Theme.surface : Qt.alpha(Theme.surface, 0.5)
            border.color: Qt.alpha(Theme.ink, 0.05)
            border.width: sidebar.addressEditing ? 0 : 1

            TextField {
                id: addressInput
                anchors.fill: parent
                visible: sidebar.addressEditing
                font.pixelSize: 12
                font.weight: Font.Medium
                font.family: Theme.fontText
                color: Theme.ink
                selectionColor: Theme.selectionWash
                verticalAlignment: TextInput.AlignVCenter
                leftPadding: 16
                rightPadding: 12
                background: null
                onTextChanged: sidebar.addressDraft = text
                onAccepted: {
                    Browser.openAddress(text)
                    sidebar.closeAddressEdit()
                }
                Keys.onEscapePressed: sidebar.closeAddressEdit()
            }

            RowLayout {
                anchors.fill: parent
                spacing: 0
                visible: !sidebar.addressEditing

                Item { Layout.preferredWidth: 4 }

                Item {
                    id: addressClick
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    RowLayout {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 7

                        Image {
                            Layout.preferredWidth: 13
                            Layout.preferredHeight: 13
                            source: Theme.iconUrl(
                                Browser.isStartPage ? "search"
                                : Browser.pageUrl.scheme === "https" ? "lock" : "globe",
                                "accent")
                        }
                        Text {
                            text: sidebar.addressDisplay === ""
                                  ? Theme.loc("Suchen oder URL eingeben", "Search or enter URL")
                                  : sidebar.addressDisplay
                            color: Qt.alpha(Theme.ink, 0.75)
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: sidebar.openAddressEdit() }
                }

                ChromeIconButton {
                    tip: Theme.loc("Gespeicherte Logins ausfüllen", "Fill saved logins")
                    iconName: "key-round"
                    role: "muted"
                    square: 38
                }
                ChromeIconButton {
                    tip: Theme.loc("Webseiten-Darstellung", "Website appearance")
                    iconName: "contrast"
                    role: "muted"
                    square: 38
                }
                ChromeIconButton {
                    tip: Theme.loc("Seitenaktionen und Erweiterungen", "Page actions and extensions")
                    iconName: "sliders-horizontal"
                    role: "muted"
                    square: 40
                    raised: true
                }
                Item { Layout.preferredWidth: 4 }
            }
        }

        // ── Mail ─────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.preferredHeight: 44
            radius: 10
            color: Qt.alpha(Theme.surface, 0.4)

            HoverHandler { cursorShape: Qt.PointingHandCursor }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 11

                Image {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    source: Theme.iconUrl("mail", "ink")
                }
                Text {
                    text: Theme.loc("E-Mail", "Email")
                    color: Theme.ink
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }
                Item { Layout.fillWidth: true }
                Image {
                    Layout.preferredWidth: 10
                    Layout.preferredHeight: 10
                    source: Theme.iconUrl("chevron-right", "muted")
                }
            }
        }

        // ── Space strip ──────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            spacing: 4

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                radius: 8
                color: Qt.alpha(Theme.surface, 0.75)

                HoverHandler { cursorShape: Qt.PointingHandCursor }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 8

                    Image {
                        Layout.preferredWidth: 13
                        Layout.preferredHeight: 13
                        source: Theme.iconUrl("layers", "accent")
                    }
                    Text {
                        text: "Personal"
                        color: Theme.ink
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: String(Browser.tabCount)
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: 9
                    }
                    Image {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        source: Theme.iconUrl("chevron-right", "muted")
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 48
                Layout.preferredHeight: 34
                radius: 8
                color: spaceAdd.hovered ? Theme.hoverWash : Qt.alpha(Theme.surface, 0.3)

                HoverHandler { id: spaceAdd; cursorShape: Qt.PointingHandCursor }

                Row {
                    anchors.centerIn: parent
                    spacing: 3
                    Image {
                        width: 13; height: 13
                        anchors.verticalCenter: parent.verticalCenter
                        source: Theme.iconUrl("plus", "ink")
                    }
                    Image {
                        width: 9; height: 9
                        anchors.verticalCenter: parent.verticalCenter
                        source: Theme.iconUrl("chevron-down", "muted")
                    }
                }
            }
        }

        // ── Tab list ─────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 23
            Layout.leftMargin: 10
            spacing: 0

            Text {
                text: Theme.loc("DEINE TABS", "YOUR TABS")
                color: Qt.alpha(Theme.ink, 0.4)
                font.pixelSize: 9
                font.weight: Font.DemiBold
                font.letterSpacing: 1.4
            }
            Item { Layout.fillWidth: true }
            Text {
                text: String(Browser.tabCount)
                color: Qt.alpha(Theme.ink, 0.4)
                font.family: Theme.fontMono
                font.pixelSize: 10
            }
        }

        TabRow {
            Layout.fillWidth: true
            Layout.topMargin: 9
        }

        SidebarActionRow {
            Layout.fillWidth: true
            Layout.topMargin: 5
            iconName: "plus"
            label: Theme.loc("Neuer Tab", "New tab")
            shortcut: "⌘ T"
            onClicked: Browser.newTab()
        }
        SidebarActionRow {
            Layout.fillWidth: true
            iconName: "file-plus"
            label: Theme.loc("Neue Notiz", "New note")
            shortcut: "⌘ N"
        }
        SidebarActionRow {
            Layout.fillWidth: true
            iconName: "glasses"
            label: Theme.loc("Privater Tab", "Private tab")
            shortcut: "⇧ ⌘ T"
        }

        // ── Flexible space ───────────────────────────────────────────────
        Item { Layout.fillHeight: true; Layout.fillWidth: true }

        // ── Library strip ────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            Layout.bottomMargin: 6
            radius: 10
            color: Qt.alpha(Theme.surface, 0.2)

            RowLayout {
                anchors.fill: parent
                anchors.margins: 3
                spacing: 3

                ChromeIconButton { iconName: "history"; role: "accent"; tip: Theme.loc("Verlauf · ⌘Y", "History · ⌘Y"); fill: true }
                ChromeIconButton { iconName: "bookmark"; role: "accent"; tip: Theme.loc("Lesezeichen · ⌥⌘B", "Bookmarks · ⌥⌘B"); fill: true }
                ChromeIconButton { iconName: "download"; role: "accent"; tip: "Downloads · ⇧⌘J"; fill: true }
            }
        }

        // ── Agent card ───────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: agentColumn.implicitHeight + 26
            radius: 12
            color: Qt.alpha(Theme.surface, 0.32)

            HoverHandler { cursorShape: Qt.PointingHandCursor }

            ColumnLayout {
                id: agentColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 13
                spacing: 9

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: Theme.moss
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        text: Theme.loc("Bereit für deine Agenten", "Ready for your agents")
                        color: Theme.ink
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Image {
                        Layout.preferredWidth: 10
                        Layout.preferredHeight: 10
                        source: Theme.iconUrl("arrow-up-right", "muted")
                    }
                }

                Text {
                    text: Theme.loc("Ein Browser. Für euch beide.", "One browser. For both of you.")
                    color: Qt.alpha(Theme.ink, 0.5)
                    font.pixelSize: 10
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        // ── Profile footer ───────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 6
            Layout.preferredHeight: 50
            radius: 12
            color: Qt.alpha(Theme.surface, 0.38)

            HoverHandler { cursorShape: Qt.PointingHandCursor }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: Qt.alpha(Theme.moss, 0.14)

                    Image {
                        anchors.centerIn: parent
                        width: 17; height: 17
                        source: Theme.iconUrl("circle-user", "accent")
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        text: "Personal"
                        color: Theme.ink
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: Theme.loc("Profil & Einstellungen", "Profile & settings")
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }
                Image {
                    Layout.preferredWidth: 9
                    Layout.preferredHeight: 9
                    source: Theme.iconUrl("chevrons-up-down", "muted")
                }
            }
        }
    }

    component ChromeIconButton: Item {
        id: chromeButton

        property string iconName
        property string role: "ink"
        property string tip
        property int square: 32
        property bool raised: false
        property bool fill: false

        signal clicked()

        Layout.preferredWidth: fill ? 0 : square
        Layout.fillWidth: fill
        Layout.preferredHeight: fill ? 0 : square
        Layout.fillHeight: fill
        opacity: enabled ? 1.0 : 0.35

        Rectangle {
            anchors.fill: parent
            anchors.margins: fill ? 0 : 2
            radius: 9
            color: chromeTap.pressed ? Theme.pressedWash
                 : chromeHover.hovered ? Theme.hoverWash
                 : raised ? Qt.alpha(Theme.ink, 0.06)
                 : "transparent"
        }

        Image {
            anchors.centerIn: parent
            width: 15
            height: 15
            sourceSize: Qt.size(15, 15)
            source: Theme.iconUrl(chromeButton.iconName, chromeButton.role)
        }

        HoverHandler {
            id: chromeHover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            id: chromeTap
            enabled: chromeButton.enabled
            onTapped: chromeButton.clicked()
        }

        ToolTip.visible: chromeHover.hovered && tip !== ""
        ToolTip.delay: 500
        ToolTip.text: tip
    }

    component SidebarActionRow: Rectangle {
        id: actionRow

        property string iconName
        property string label
        property string shortcut

        signal clicked()

        implicitHeight: 44
        radius: 9
        color: actionHover.hovered ? Theme.hoverWash : "transparent"

        HoverHandler { id: actionHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: actionRow.clicked() }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 11
            spacing: 10

            Image {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                source: Theme.iconUrl(actionRow.iconName, "ink")
                opacity: 0.55
            }
            Text {
                text: actionRow.label
                color: Qt.alpha(Theme.ink, 0.55)
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
            Text {
                text: actionRow.shortcut
                visible: text !== ""
                color: Qt.alpha(Theme.ink, 0.5)
                font.family: Theme.fontMono
                font.pixelSize: 10
            }
        }
    }

    component TabRow: Rectangle {
        id: tabRow

        implicitHeight: 44
        radius: 9
        color: Qt.alpha(Theme.surface, 0.75) // the single tab is always active

        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 11
                spacing: 10

                Image {
                    Layout.preferredWidth: 17
                    Layout.preferredHeight: 17
                    source: Theme.iconUrl(Browser.isStartPage ? "circle" : "globe", "accent")
                }
                Text {
                    text: Browser.tabTitle
                    color: Theme.ink
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Item {
                Layout.preferredWidth: 36
                Layout.fillHeight: true

                Image {
                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    source: Theme.iconUrl("x", "muted")
                    visible: tabHover.hovered
                }
                TapHandler {
                    enabled: tabHover.hovered
                    onTapped: Browser.newTab()
                }
            }
        }
    }
}
