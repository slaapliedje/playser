// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// Jellyfin's own sign-in steps: choosing the server (one found on this
// network, or its address typed in), and a name and password when Quick
// Connect isn't used. B cancels.
FocusScope {
    id: root

    required property JellyfinClient server
    signal cancelled()

    readonly property bool address: server.state === "address"
    property string user: ""
    property string password: ""

    // the rows to choose from: { label, detail, action, value }
    readonly property var rows: address
        ? server.servers.map(s => ({ label: s.name, detail: s.address, action: "server", value: s.address }))
            .concat([{ label: "Type an address…", detail: "", action: "type" }])
        : [{ label: "Name", detail: user || "Choose", action: "user" },
           { label: "Password", detail: password ? "•".repeat(password.length) : "None", action: "password" },
           { label: "Sign in", detail: "", action: "signIn" }]
            .concat(server.quickConnect ? [{ label: "Use Quick Connect instead", detail: "", action: "quickConnect" }] : [])
            .concat([{ label: "Another server", detail: "", action: "change" }])

    onAddressChanged: list.currentIndex = 0
    Component.onCompleted: {
        if (server.users.length)
            user = server.users[0]
    }
    Connections {
        target: root.server
        function onStateChanged() {
            if (!root.user && root.server.users.length)
                root.user = root.server.users[0]
        }
    }

    function activate(r) {
        if (r.action === "server")
            server.useServer(r.value)
        else if (r.action === "type")
            keyboard.edit("address", "Server address", "For example 192.168.1.20 or nas.local:8096", false, "")
        else if (r.action === "user")
            keyboard.edit("user", "Name", "", false, user)
        else if (r.action === "password")
            keyboard.edit("password", "Password", "", true, "")
        else if (r.action === "signIn")
            server.signIn(user, password)
        else if (r.action === "quickConnect")
            server.useQuickConnect()
        else if (r.action === "change")
            server.changeServer()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            root.cancelled()
            event.accepted = true
        } else if (event.key === Qt.Key_I && root.address) {
            root.server.discover()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    Column {
        id: heading
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing * 0.4
        Text {
            text: root.address ? "Jellyfin" : "Sign in to " + root.server.serverName
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: root.address
                ? (root.server.servers.length ? "Choose your server, or type its address"
                                              : "No server found on this network yet. Type its address, as you would in a browser")
                : root.server.address
            color: Theme.textDim
            font.pixelSize: Theme.fontMedium
        }
    }

    ListView {
        id: list
        anchors {
            left: parent.left; right: parent.right; top: heading.bottom; bottom: message.top
            leftMargin: Theme.margin; rightMargin: Theme.margin
            topMargin: Theme.spacing * 1.5; bottomMargin: Theme.spacing
        }
        clip: true
        focus: true
        model: root.rows
        spacing: Theme.spacing * 0.3

        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            width: ListView.view.width
            height: 76 * Theme.scale
            radius: Theme.radius / 2
            color: current ? Theme.surfaceRaised : Theme.surface
            border.color: current ? Theme.focusRing : "transparent"
            border.width: 3 * Theme.scale
            Text {
                anchors { left: parent.left; leftMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: modelData.label
                color: Theme.text
                font.pixelSize: Theme.fontMedium
                font.weight: modelData.action === "signIn" ? Font.DemiBold : Font.Normal
            }
            Text {
                anchors { right: parent.right; rightMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: modelData.detail
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Keys.onReturnPressed: root.activate(root.rows[currentIndex])
    }

    Text {
        id: message
        anchors { left: parent.left; right: parent.right; bottom: hints.top; margins: Theme.margin }
        visible: text !== ""
        text: root.server.message
        color: root.server.message.endsWith("…") ? Theme.textDim : Theme.warning
        font.pixelSize: Theme.fontMedium
        wrapMode: Text.Wrap
    }

    HintBar {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        hints: [{ buttons: ["south"], text: "Choose" }, { buttons: ["dpadV"], text: "Move" }]
            .concat(root.address ? [{ buttons: ["north"], text: "Look again" }] : [])
            .concat([{ buttons: ["east"], text: "Back" }])
    }

    OnScreenKeyboard {
        id: keyboard
        anchors.fill: parent
        visible: false
        // what's being typed: "address", "user", "password"
        property string field: ""
        function edit(f, label, placeholder, secret, initial) {
            field = f
            keyboard.label = label
            keyboard.placeholder = placeholder
            keyboard.secret = secret
            open(initial)
        }
        onAccepted: typed => {
            keyboard.text = "" // not kept once used
            list.forceActiveFocus()
            const row = list.currentIndex
            if (field === "address")
                root.server.useServer(typed)
            else if (field === "user")
                root.user = typed
            else
                root.password = typed
            list.currentIndex = row // the rows were remade
        }
        onCancelled: {
            keyboard.text = ""
            list.forceActiveFocus()
        }
    }
}
