import QtQuick
import QtQuick.Window
import DiscPlayer

Window {
    id: win

    property bool startFullscreen: false

    width: 1280
    height: 720
    visible: true
    visibility: startFullscreen ? Window.FullScreen : Window.Windowed
    color: Theme.background
    title: "Disc Player"

    // scale the 10-foot UI with the window, from a 1080p design size
    Binding { target: Theme; property: "scale"; value: Math.min(win.width / 1920, win.height / 1080) }

    DiscMonitor { id: discs }

    MpvItem {
        id: mpv
        anchors.fill: parent
        visible: player.active
    }

    HomeScreen {
        id: home
        anchors.fill: parent
        visible: !player.active
        monitor: discs
        focus: !player.active
        onPlayRequested: drive => player.play(drive)
    }

    PlayerScreen {
        id: player
        anchors.fill: parent
        mpv: mpv
        onClosed: home.forceActiveFocus()
    }
}
