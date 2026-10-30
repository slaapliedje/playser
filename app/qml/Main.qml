import QtQuick
import QtCore
import Playser

// Root of the UI. It is rendered off-screen and composited by mpv over the
// video (UiRenderer), so transparent areas show the video underneath.
FocusScope {
    id: root

    required property Mpv mpv
    required property Gamepad gamepad

    // --play: a file or URL without going through a drive
    function playUrl(url) {
        setup.visible = false
        player.play({ url: url, label: url.split("/").pop() })
    }

    // scale the 10-foot UI from a 1080p design size
    Binding { target: Theme; property: "scale"; value: Math.min(root.width / 1920, root.height / 1080) }
    Binding { target: Theme; property: "buttonStyle"; value: root.gamepad.style }

    DiscMonitor { id: discs }
    SystemCheck { id: system }
    // a newly inserted disc lets MakeMKV say whether the drive reads 4K UHD
    Connections {
        target: discs
        function onDrivesChanged() { if (!player.active) system.checkUhdDrives() }
    }
    Settings {
        id: settings
        property bool setupSeen: false
        // send Dolby/DTS to the receiver undecoded (setup screen)
        property bool passthrough: false
    }

    // codecs sent undecoded when on; one the receiver can't take fails to
    // open and Mpv's audio watchdog falls back to decoding
    Binding { target: root.mpv; property: "passthrough"; value: settings.passthrough ? "ac3,eac3,dts-hd,truehd" : "" }

    function showSetup() {
        setup.open()
    }

    Component.onCompleted: {
        // first run, or something Blu-ray playback needs went missing
        if (!settings.setupSeen || (system.opticalDrives > 0 && !system.ready))
            showSetup()
    }

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
        check: system
        focus: !player.active
        onSetupRequested: root.showSetup()
        onPlayRequested: drive => {
            system.stopUhdChecks() // MakeMKV needs the drive to itself
            // a Blu-ray can't play without MakeMKV, nor a 4K disc without a
            // drive it can read them with: say why instead of failing
            if (drive.kind === "bluray") {
                system.refresh()
                const uhd = system.uhdDrives[drive.device] || {}
                if (!system.ready || (drive.disc && drive.disc.format === "UHD" && uhd.state === "no")) {
                    root.showSetup()
                    return
                }
            }
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
                ? episodes.map(e => ({ url: e.url, title: e.title }))
                : null, index, start)
        }
        onClosed: home.forceActiveFocus()
    }

    SetupScreen {
        id: setup
        anchors.fill: parent
        check: system
        passthrough: settings.passthrough
        onPassthroughToggled: {
            settings.passthrough = !settings.passthrough
            system.setPassthroughRule(settings.passthrough)
        }
        onDone: {
            settings.setupSeen = true
            home.forceActiveFocus()
        }
    }

    PlayerScreen {
        id: player
        anchors.fill: parent
        mpv: root.mpv
        onClosed: home.forceActiveFocus()
    }
}
