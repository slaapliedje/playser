import QtQuick
import DiscPlayer

FocusScope {
    id: root

    required property MpvItem mpv
    property bool active: false
    property var drive: null
    signal closed()

    function play(d) {
        drive = d
        active = true
        forceActiveFocus()
        mpv.load(d.url, d.options || {})
        showOsd()
    }

    function close() {
        mpv.stop()
        active = false
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
        function onEndFile(reason) {
            if (root.active && (reason === "eof" || reason === "error"))
                root.close()
        }
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

    // ---- on-screen display ----
    Item {
        id: osd
        anchors.fill: parent
        opacity: 1
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
                text: root.mpv.mediaTitle || (root.drive ? root.drive.label : "")
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
                Text {
                    id: timeText
                    text: (root.mpv.paused ? "❚❚  " : "▶  ")
                          + Theme.formatTime(root.mpv.position) + " / " + Theme.formatTime(root.mpv.duration)
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                }
                Text {
                    anchors.right: parent.right
                    visible: root.mpv.chapters.length > 0
                    text: "Chapter " + (root.mpv.chapter + 1) + " / " + root.mpv.chapters.length
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall
                }
            }

            Text {
                text: "Ⓐ Pause    ◀ ▶ Seek    LB RB Chapter    Ⓨ Audio & subtitles    Ⓑ Stop"
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall * 0.85
            }
        }
    }

    TrackPicker {
        id: picker
        anchors.fill: parent
        mpv: root.mpv
        onDismissed: { root.forceActiveFocus(); root.showOsd() }
    }
}
