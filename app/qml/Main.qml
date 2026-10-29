import QtQuick
import DiscPlayer

// Root of the UI. It is rendered off-screen and composited by mpv over the
// video (UiRenderer), so transparent areas show the video underneath.
FocusScope {
    id: root

    required property Mpv mpv

    // --play: a file or URL without going through a drive
    function playUrl(url) {
        player.play({ url: url, label: url.split("/").pop() })
    }

    // scale the 10-foot UI from a 1080p design size
    Binding { target: Theme; property: "scale"; value: Math.min(root.width / 1920, root.height / 1080) }

    DiscMonitor { id: discs }

    // Steam's Game Mode screensaver would otherwise cover the film
    ScreenSaverInhibitor { inhibited: player.active && !root.mpv.paused }

    Rectangle {
        anchors.fill: parent
        visible: !player.active
        color: Theme.background
    }

    HomeScreen {
        id: home
        anchors.fill: parent
        visible: !player.active
        monitor: discs
        focus: !player.active
        onPlayRequested: drive => {
            const episodes = drive.disc && drive.disc.episodes ? drive.disc.episodes : []
            if (episodes.length > 1 || Resume.get(drive.contentHash || drive.label))
                discScreen.open(drive)
            else
                player.play(drive)
        }
    }

    DiscScreen {
        id: discScreen
        anchors.fill: parent
        onPlayRequested: (index, start) => {
            const d = drive
            const episodes = d.disc && d.disc.episodes ? d.disc.episodes : []
            visible = false
            player.play(d, episodes.length > 1
                ? episodes.map(e => ({ url: "bd://mpls/" + parseInt(e.playlist), title: e.title }))
                : null, index, start)
        }
        onClosed: home.forceActiveFocus()
    }

    PlayerScreen {
        id: player
        anchors.fill: parent
        mpv: root.mpv
        onClosed: home.forceActiveFocus()
    }
}
