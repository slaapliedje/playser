// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// A yes/no question over the screen: A accepts, B cancels; focus goes back
// to `returnFocus` either way.
//   ConfirmDialog { text: "Exit Playser?"; acceptText: "Exit"; onAccepted: ... }
FocusScope {
    id: root

    property string text
    property string acceptText: "OK"
    property string cancelText: "Cancel"
    property Item returnFocus
    signal accepted()

    anchors.fill: parent
    visible: false

    function open() {
        visible = true
        forceActiveFocus()
    }
    function close() {
        focus = false
        visible = false
        if (returnFocus)
            returnFocus.forceActiveFocus()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            close()
            root.accepted()
        } else if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            close()
        }
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "#c0000000"
    }
    Rectangle {
        anchors.centerIn: parent
        width: 900 * Theme.scale
        height: column.implicitHeight + Theme.spacing * 4
        radius: Theme.radius
        color: Theme.surface
        Column {
            id: column
            anchors.centerIn: parent
            spacing: Theme.spacing * 2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.text
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
            }
            HintBar {
                anchors.horizontalCenter: parent.horizontalCenter
                hints: [
                    { buttons: ["south"], text: root.acceptText },
                    { buttons: ["east"], text: root.cancelText },
                ]
            }
        }
    }
}
