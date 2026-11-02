// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// The user's own videos: places to start from (Videos, Home, SD cards and
// USB drives), then folders. A plays a video, a disc folder (VIDEO_TS,
// BDMV) or disc image as a disc, or opens a folder; B goes up a folder,
// then back to the places, then home.
FocusScope {
    id: root

    // a video, or a disc folder or image as a drive (MediaFiles)
    signal playRequested(var drive)
    signal closed()

    visible: false

    // "" for the places
    property string path: ""
    // where the place being browsed starts: B there goes back to the places
    property string placeRoot: ""
    property string placeName: ""
    property var rows: []
    // the folders opened to get here, with the row chosen in each
    property var trail: []
    // a disc folder or image being read (MediaFiles.openDisc)
    property string opening: ""
    property string message: ""

    function open() {
        trail = []
        show("", 0)
        visible = true
        list.forceActiveFocus()
    }

    // back from playing: the folder as it was left, refreshed
    function reopen() {
        const i = list.currentIndex
        show(path, i)
        visible = true
        list.forceActiveFocus()
    }

    function show(p, index) {
        path = p
        opening = ""
        message = ""
        rows = p === "" ? files.places().map(e => Object.assign({ type: "place" }, e)) : files.list(p)
        list.currentIndex = Math.max(0, Math.min(index, rows.length - 1))
        list.positionViewAtIndex(list.currentIndex, ListView.Center)
    }

    function enter(r) {
        trail = trail.concat([{ path: path, index: list.currentIndex }])
        if (r.type === "place") {
            placeRoot = r.path
            placeName = r.name
        }
        show(r.path, 0)
    }

    function back() {
        if (opening !== "") { // stop waiting for a disc image
            opening = ""
            return
        }
        if (!trail.length) {
            close()
            return
        }
        const t = trail[trail.length - 1]
        trail = trail.slice(0, -1)
        show(t.path, t.index)
    }

    function close() {
        focus = false
        visible = false
        closed()
    }

    function activate(r) {
        if (!r || opening !== "")
            return
        if (r.type === "place" || r.type === "folder") {
            enter(r)
        } else if (r.type === "video") {
            root.playRequested(files.video(r.path))
        } else {
            message = ""
            opening = r.path
            files.openDisc(r.path)
        }
    }

    MediaFiles {
        id: files
        onOpened: (p, drive) => {
            if (p !== root.opening || !root.visible)
                return
            root.opening = ""
            root.playRequested(drive)
        }
        onOpenFailed: p => {
            if (p !== root.opening)
                return
            root.opening = ""
            root.message = "That isn't a DVD or Blu-ray Playser can read"
        }
    }

    function sizeText(bytes) {
        if (!(bytes > 0))
            return ""
        const gb = bytes / 1e9
        return gb >= 1 ? gb.toFixed(1) + " GB" : Math.max(1, Math.round(bytes / 1e6)) + " MB"
    }

    readonly property var typeLabels: ({
        "dvd": "DVD", "bluray": "Blu-ray", "image": "Disc image"
    })

    function detailOf(r) {
        if (r.path === opening)
            return "Reading…"
        if (r.type === "video") {
            const resume = Resume.get("file:" + r.path)
            return resume ? "Resume at " + Theme.formatTime(resume.position) : sizeText(r.size)
        }
        return typeLabels[r.type] || ""
    }

    Keys.onEscapePressed: back()
    Keys.onBackPressed: back()

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    Column {
        id: heading
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing * 0.4
        Text {
            width: parent.width
            text: root.path === "" ? "Files"
                : root.path === root.placeRoot ? root.placeName
                : root.path.split("/").pop()
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            // where this folder is within the place
            text: root.path === "" ? "Videos, folders and disc images"
                : root.placeName + root.path.slice(root.placeRoot.length)
            color: Theme.textDim
            font.pixelSize: Theme.fontMedium
            elide: Text.ElideMiddle
        }
    }

    ListView {
        id: list
        anchors {
            left: parent.left; right: parent.right; top: heading.bottom; bottom: hintRow.top
            leftMargin: Theme.margin; rightMargin: Theme.margin
            topMargin: Theme.spacing * 1.5; bottomMargin: Theme.spacing
        }
        clip: true
        focus: true
        model: root.rows
        spacing: Theme.spacing * 0.3
        highlightMoveDuration: 100
        preferredHighlightBegin: height * 0.3
        preferredHighlightEnd: height * 0.7
        highlightRangeMode: ListView.ApplyRange

        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            readonly property bool container: modelData.type === "place" || modelData.type === "folder"
            width: ListView.view.width
            height: 76 * Theme.scale
            radius: Theme.radius / 2
            color: current ? Theme.surfaceRaised : Theme.surface
            border.color: current ? Theme.focusRing : "transparent"
            border.width: 3 * Theme.scale

            Text {
                anchors { left: parent.left; leftMargin: Theme.spacing; right: detail.left; rightMargin: Theme.spacing
                          verticalCenter: parent.verticalCenter }
                text: modelData.name + (container ? "  ›" : "")
                color: Theme.text
                font.pixelSize: Theme.fontMedium
                font.weight: container ? Font.DemiBold : Font.Normal
                elide: Text.ElideMiddle
            }
            Text {
                id: detail
                anchors { right: parent.right; rightMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: root.detailOf(modelData)
                color: modelData.path === root.opening || text.startsWith("Resume") ? Theme.accent : Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Keys.onReturnPressed: root.activate(root.rows[currentIndex])
    }

    Text {
        anchors.centerIn: list
        visible: root.rows.length === 0
        text: root.path === "" ? "No folders to show" : "No videos here"
        color: Theme.textDim
        font.pixelSize: Theme.fontLarge
    }

    Row {
        id: hintRow
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.margin }
        spacing: Theme.spacing * 2
        HintBar {
            id: hints
            hints: [
                { buttons: ["south"], text: "Open" },
                { buttons: ["dpadV"], text: "Choose" },
                { buttons: ["east"], text: "Back" },
            ]
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.message !== ""
            text: root.message
            color: Theme.warning
            font.pixelSize: Theme.fontSmall
        }
    }
}
