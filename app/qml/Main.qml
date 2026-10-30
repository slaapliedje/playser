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

    // --play-menu: a DVD image or folder through its menus
    function playMenu(path) {
        setup.visible = false
        player.playMenu({ device: path, kind: "dvd", label: path.split("/").pop() })
    }

    // scale the 10-foot UI from a 1080p design size
    Binding { target: Theme; property: "scale"; value: Math.min(root.width / 1920, root.height / 1080) }
    Binding { target: Theme; property: "buttonStyle"; value: root.gamepad.style }

    DiscMonitor { id: discs }
    DiscNav { id: nav; mpv: root.mpv }
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
            const extras = drive.disc && drive.disc.extras ? drive.disc.extras : []
            const resume = Resume.get(drive.contentHash || drive.label)
            if (episodes.length > 1 || extras.length || resume)
                discScreen.open(drive)
            else if (drive.kind === "dvd")
                player.playMenu(drive) // a DVD starts with its own menus
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
        // watching an extra leaves the film's resume point alone, and
        // returns here
        onMenuRequested: {
            visible = false
            player.playMenu(drive)
        }
        onExtraRequested: extra => {
            visible = false
            player.play(drive, [{ url: extra.url, title: extra.title, extra: true }], 0, 0)
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
        nav: nav
        onClosed: wasExtra => {
            if (wasExtra)
                discScreen.reopen()
            else
                home.forceActiveFocus()
        }
    }
}
