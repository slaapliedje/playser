import QtQuick
import DiscPlayer

FocusScope {
    id: root

    required property Mpv mpv
    property bool active: false
    property var drive: null
    // what plays, in order: [{ url, title }] (several for a series' episodes);
    // at the end of one entry the next starts
    property var queue: []
    property int index: 0
    readonly property var entry: queue[index] || ({})
    readonly property string resumeKey: drive ? (drive.contentHash || drive.label || "") : ""
    // the catalogue's title when the disc is known, else the volume label
    readonly property string displayTitle: !drive ? ""
        : drive.disc && drive.disc.title ? drive.disc.title : (drive.label || "")
    // the episode's title for a series, else empty
    readonly property string entryTitle: queue.length > 1 ? (entry.title || "") : ""
    // from play() until mpv has opened the title; a UHD takes ~20 s
    property bool opening: false
    signal closed()

    // q: the queue (default: the disc's own url); i: entry; start: seconds
    function play(d, q, i, start) {
        drive = d
        queue = q || [{ url: d.url, title: displayTitle }]
        active = true
        forceActiveFocus()
        loadEntry(i || 0, start || 0)
    }

    function loadEntry(i, start) {
        index = i
        opening = true
        mpv.load(entry.url, drive.options || {}, start)
        showOsd()
    }

    function saveResume() {
        if (!opening && root.mpv.duration > 0)
            Resume.save(resumeKey, { index: index, position: Math.floor(root.mpv.position), title: entry.title || "" })
    }

    function close() {
        saveResume()
        mpv.stop()
        active = false
        opening = false
        picker.visible = false
        closed()
    }

    function showOsd() {
        osd.opacity = 1
        hideTimer.restart()
    }

    function seek(secs) {
        mpv.command(["seek", secs, "relative"])
        showOsd()
    }

    visible: active

    Connections {
        target: root.mpv
        function onFileLoaded() {
            root.opening = false
            root.showOsd()
        }
        function onAudioProblem(message) {
            notice.text = message
            noticeBox.opacity = 1
            noticeTimer.restart()
        }
        function onEndFile(reason) {
            if (!root.active)
                return
            if (reason === "eof" && root.index + 1 < root.queue.length) {
                root.loadEntry(root.index + 1, 0)
            } else if (reason === "eof") {
                Resume.clear(root.resumeKey) // watched to the end
                root.opening = true // keep close() from saving the final position
                root.close()
            } else if (reason === "error") {
                root.close()
            }
        }
    }

    Timer {
        interval: 10000
        repeat: true
        running: root.active && !root.opening
        onTriggered: root.saveResume()
    }

    Timer {
        id: hideTimer
        interval: 4000
        onTriggered: if (!root.mpv.paused) osd.opacity = 0
    }

    Keys.onPressed: event => {
        switch (event.key) {
        case Qt.Key_Escape:
        case Qt.Key_Back:
            root.close(); break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            root.mpv.command(["cycle", "pause"]); root.showOsd(); break
        case Qt.Key_Left:
            root.seek(-10); break
        case Qt.Key_Right:
            root.seek(30); break
        case Qt.Key_PageUp:
            root.mpv.command(["add", "chapter", -1]); root.showOsd(); break
        case Qt.Key_PageDown:
            root.mpv.command(["add", "chapter", 1]); root.showOsd(); break
        case Qt.Key_I:
        case Qt.Key_Menu:
            picker.open(); break
        default:
            root.showOsd(); return
        }
        event.accepted = true
    }

    // ---- opening ----
    Rectangle {
        anchors.fill: parent
        visible: root.opening
        color: Theme.background

        Column {
            anchors.centerIn: parent
            spacing: Theme.spacing

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Opening " + (root.drive && root.drive.disc && root.drive.disc.format === "UHD" ? "4K UHD"
                                    : { "bluray": "Blu-ray", "dvd": "DVD" }[root.drive ? root.drive.kind : ""] || "disc")
                      + ".".repeat(dots.count)
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.entryTitle || root.displayTitle
                color: Theme.textDim
                font.pixelSize: Theme.fontMedium
            }
            HintBar {
                anchors.horizontalCenter: parent.horizontalCenter
                hints: [{ buttons: ["east"], text: "Cancel" }]
            }
        }
        Timer {
            id: dots
            property int count: 0
            interval: 500
            repeat: true
            running: root.opening
            onTriggered: count = (count + 1) % 4
        }
    }

    // ---- on-screen display ----
    Item {
        id: osd
        anchors.fill: parent
        opacity: 1
        visible: opacity > 0 && !root.opening // hidden items stop re-rendering the UI layer
        Behavior on opacity { NumberAnimation { duration: 250 } }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 300 * Theme.scale
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "#e0000000" }
            }
        }

        Column {
            anchors {
                left: parent.left; right: parent.right; bottom: parent.bottom
                margins: Theme.margin
            }
            spacing: Theme.spacing * 0.6

            Text {
                width: parent.width
                text: (root.displayTitle || root.mpv.mediaTitle)
                      + (root.entryTitle ? "  ·  " + (root.index + 1) + ". " + root.entryTitle : "")
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Rectangle {
                width: parent.width
                height: 10 * Theme.scale
                radius: height / 2
                color: "#40ffffff"
                Rectangle {
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                    width: root.mpv.duration > 0
                           ? parent.width * Math.min(1, root.mpv.position / root.mpv.duration) : 0
                }
            }

            Item {
                width: parent.width
                height: timeText.implicitHeight
                Row {
                    spacing: Theme.spacing * 0.5
                    // pause: two bars (no font has a dependable ⏸)
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.mpv.paused
                        spacing: Theme.fontSmall * 0.18
                        Repeater {
                            model: 2
                            Rectangle {
                                width: Theme.fontSmall * 0.22
                                height: Theme.fontSmall * 0.75
                                color: Theme.text
                            }
                        }
                    }
                    Text {
                        id: timeText
                        text: (root.mpv.paused ? "" : "▶  ")
                              + Theme.formatTime(root.mpv.position) + " / " + Theme.formatTime(root.mpv.duration)
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                    }
                }
                Text {
                    anchors.right: parent.right
                    visible: root.mpv.chapters.length > 0
                    text: "Chapter " + (root.mpv.chapter + 1) + " / " + root.mpv.chapters.length
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall
                }
            }

            HintBar {
                textSize: Theme.fontSmall * 0.85
                hints: [
                    { buttons: ["south"], text: root.mpv.paused ? "Play" : "Pause" },
                    { buttons: ["dpadH"], text: "Seek" },
                    { buttons: ["lb", "rb"], text: "Chapter" },
                    { buttons: ["north"], text: "Audio & subtitles" },
                    { buttons: ["east"], text: "Stop" },
                ]
            }
        }
    }

    // ---- notices (e.g. audio fallback) ----
    Rectangle {
        id: noticeBox
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.margin }
        width: Math.min(notice.implicitWidth, parent.width - 4 * Theme.margin) + Theme.spacing * 2
        height: notice.height + Theme.spacing
        radius: Theme.radius / 2
        color: Theme.surfaceRaised
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
        Text {
            id: notice
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 4 * Theme.margin)
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }
        Timer {
            id: noticeTimer
            interval: 8000
            onTriggered: noticeBox.opacity = 0
        }
    }

    TrackPicker {
        id: picker
        anchors.fill: parent
        mpv: root.mpv
        onDismissed: { root.forceActiveFocus(); root.showOsd() }
    }
}
