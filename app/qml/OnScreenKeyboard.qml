// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// Typing with a controller: a server address, a name, a password. The d-pad
// chooses a key and A types it; X deletes, Y switches to capitals and
// symbols, Start is done and B cancels. A keyboard's own typing works too.
FocusScope {
    id: root

    property string label: ""
    property string placeholder: ""
    // show dots, not what's typed
    property bool secret: false
    property string text: ""
    signal accepted(string text)
    signal cancelled()

    property bool shifted: false
    // the key chosen: its row, and the column (of ten) it starts at
    property int row: 1
    property int column: 0

    readonly property var lower: ["1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm.:/"]
    readonly property var upper: ["!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM,;?"]
    // the last row: what each key does, and how many columns it spans
    readonly property var actions: [
        { action: "shift", span: 2 }, { action: "space", span: 4 },
        { action: "delete", span: 2 }, { action: "done", span: 2 },
    ]

    function open(initial) {
        text = initial || ""
        shifted = false
        row = 1
        column = 0
        visible = true
        forceActiveFocus()
    }

    function close() {
        visible = false
        focus = false
    }

    function type(s) {
        text += s
    }

    // the last row's key covering column c
    function actionAt(c) {
        let start = 0
        for (const a of actions) {
            if (c < start + a.span)
                return { action: a.action, start: start }
            start += a.span
        }
        return { action: "done", start: 8 }
    }

    function press() {
        if (row < 4) {
            type((shifted ? upper : lower)[row][column])
            return
        }
        const a = actionAt(column).action
        if (a === "shift")
            shifted = !shifted
        else if (a === "space")
            type(" ")
        else if (a === "delete")
            text = text.slice(0, -1)
        else
            done()
    }

    function done() {
        close()
        accepted(text)
    }

    function move(dr, dc) {
        if (dc !== 0) {
            if (row < 4) {
                column = Math.max(0, Math.min(9, column + dc))
            } else {
                const a = actionAt(column)
                const next = dc > 0 ? a.start + actions.find(x => x.action === a.action).span : a.start - 1
                if (next >= 0 && next <= 9)
                    column = actionAt(next).start
            }
        } else {
            row = Math.max(0, Math.min(4, row + dr))
            if (row === 4)
                column = actionAt(column).start
        }
    }

    Keys.onPressed: event => {
        // a keyboard's typing (the controller's keys carry no text)
        if (event.text.length === 1 && event.text >= " " && event.text !== "\x7f") {
            type(event.text)
            event.accepted = true
            return
        }
        switch (event.key) {
        case Qt.Key_Up: move(-1, 0); break
        case Qt.Key_Down: move(1, 0); break
        case Qt.Key_Left: move(0, -1); break
        case Qt.Key_Right: move(0, 1); break
        case Qt.Key_Return: case Qt.Key_Enter: press(); break
        case Qt.Key_Backspace: text = text.slice(0, -1); break
        case Qt.Key_I: shifted = !shifted; break
        case Qt.Key_Menu: done(); break
        case Qt.Key_Escape: case Qt.Key_Back:
            close()
            cancelled()
            break
        default:
            return
        }
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    Column {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing

        Text {
            text: root.label
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
        }
        // what's typed so far
        Rectangle {
            width: parent.width
            height: field.implicitHeight + Theme.spacing
            radius: Theme.radius / 2
            color: Theme.surface
            border.color: Theme.focusRing
            border.width: 3 * Theme.scale
            Text {
                id: field
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: Theme.spacing }
                text: root.text === "" ? root.placeholder
                    : (root.secret ? "•".repeat(root.text.length) : root.text) + "▏"
                color: root.text === "" ? Theme.textDim : Theme.text
                font.pixelSize: Theme.fontLarge
                elide: Text.ElideLeft
            }
        }

        Item { width: 1; height: Theme.spacing * 0.5 }

        // the keys
        Column {
            id: keys
            width: parent.width
            spacing: Theme.spacing * 0.3
            readonly property real unit: (width - 9 * spacing) / 10

            Repeater {
                model: 4
                delegate: Row {
                    required property int index
                    readonly property int keyRow: index
                    spacing: keys.spacing
                    Repeater {
                        model: 10
                        delegate: Key {
                            required property int index
                            width: keys.unit
                            label: (root.shifted ? root.upper : root.lower)[keyRow][index]
                            current: root.row === keyRow && root.column === index
                        }
                    }
                }
            }
            Row {
                spacing: keys.spacing
                Repeater {
                    model: root.actions
                    delegate: Key {
                        required property var modelData
                        readonly property int start: root.actions.slice(0, root.actions.indexOf(modelData))
                                                         .reduce((n, a) => n + a.span, 0)
                        width: keys.unit * modelData.span + keys.spacing * (modelData.span - 1)
                        label: ({ shift: root.shifted ? "abc" : "ABC  #+=", space: "Space",
                                  delete: "Delete", done: "Done" })[modelData.action]
                        current: root.row === 4 && root.column === start
                        accent: modelData.action === "done"
                    }
                }
            }
        }
    }

    HintBar {
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        hints: [
            { buttons: ["south"], text: "Type" },
            { buttons: ["west"], text: "Delete" },
            { buttons: ["north"], text: root.shifted ? "Lower case" : "Capitals" },
            { buttons: ["start"], text: "Done" },
            { buttons: ["east"], text: "Cancel" },
        ]
    }

    component Key: Rectangle {
        property string label
        property bool current: false
        property bool accent: false
        height: 72 * Theme.scale
        radius: Theme.radius / 2
        color: current ? Theme.surfaceRaised : Theme.surface
        border.color: current ? Theme.focusRing : accent ? Theme.accent : "transparent"
        border.width: 3 * Theme.scale
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: parent.accent ? Theme.accent : Theme.text
            font.pixelSize: Theme.fontMedium
        }
    }
}
