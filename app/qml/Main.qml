// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtCore
import Playser

// Root of the UI. It is rendered off-screen and composited by mpv over the
// video (UiRenderer), so transparent areas show the video underneath.
FocusScope {
    id: root

    required property Mpv mpv
    required property Gamepad gamepad

    // the app is quitting: stop playback, which saves the resume point
    function shutdown() {
        if (player.active)
            player.close()
    }

    // --play: a file or URL without going through a drive
    function playUrl(url) {
        setup.visible = false
        player.play({ url: url, label: url.split("/").pop() })
    }

    // --play-menu: a disc, image or folder through its menus
    function playMenu(path) {
        setup.visible = false
        system.stopUhdChecks() // MakeMKV needs the drive to itself, as for Play
        player.playMenu({ device: path, label: path.split("/").pop() })
    }

    // --play-playlists: a Blu-ray's playlists as episodes ("/dev/sr0:12,10")
    function playPlaylists(arg) {
        setup.visible = false
        system.stopUhdChecks()
        const i = arg.lastIndexOf(":")
        const path = arg.slice(0, i)
        player.play({ device: path, kind: "bluray", label: path.split("/").pop() },
                    arg.slice(i + 1).split(",").map(n => ({ url: "bd://mpls/" + n, title: "Playlist " + n })))
    }

    // scale the 10-foot UI from a 1080p design size
    Binding { target: Theme; property: "scale"; value: Math.min(root.width / 1920, root.height / 1080) }
    Binding { target: Theme; property: "buttonStyle"; value: root.gamepad.style }

    DiscMonitor { id: discs }
    // signed in before: find the servers now, so the libraries open at once
    PlexClient {
        id: plex
        Component.onCompleted: if (signedIn) start()
    }
    JellyfinClient {
        id: jellyfin
        Component.onCompleted: if (signedIn) start()
    }
    function libraryServer(name) {
        return name === "plex" ? plex : name === "jellyfin" ? jellyfin : null
    }

    // A disc through its menus. A Blu-ray's are opened from its mount
    // (DiscMonitor.ensureMounted); if it won't mount, the film plays.
    property var menuDrive: null
    function openMenus(drive) {
        // a folder or image needs no mounting
        if (drive.kind !== "bluray" || drive.file) {
            player.playMenu(drive)
            return
        }
        menuDrive = drive
        discs.ensureMounted(drive)
    }
    Connections {
        target: discs
        function onMounted(device, mountPoint) {
            const d = root.menuDrive
            if (!d || d.device !== device)
                return
            root.menuDrive = null
            player.playMenu(d, mountPoint)
        }
        function onMountFailed(device) {
            const d = root.menuDrive
            if (!d || d.device !== device)
                return
            root.menuDrive = null
            player.play(d)
        }
    }
    DiscNav { id: nav; mpv: root.mpv }
    // 3D glasses connected: a 3D logo, standing out of the screen in 3D mode
    StereoBadge { mpv: root.mpv; shown: home.visible && root.mpv.stereoDisplay }
    SystemCheck { id: system }
    // the disc d was taken out of its drive (swapping discs takes it out
    // first)
    function discGone(d) {
        if (!d || !d.device)
            return false
        const now = discs.drives.find(x => x.device === d.device)
        return !!now && !now.hasMedia // not found: an image or folder
    }
    Connections {
        target: discs
        function onDrivesChanged() {
            // ejected while open: back home, letting go of the drive for
            // the next disc
            if (player.active && root.discGone(player.drive))
                player.close(true)
            if (discScreen.visible && root.discGone(discScreen.drive))
                discScreen.close()
            if (root.discGone(root.menuDrive))
                root.menuDrive = null
            // a newly inserted disc lets MakeMKV say whether the drive reads 4K UHD
            if (!player.active)
                system.checkUhdDrives()
        }
    }
    Settings {
        id: settings
        property bool setupSeen: false
        // send Dolby/DTS to the receiver undecoded (setup screen)
        property bool passthrough: false
        // DVDs start with their own menus (setup screen)
        property bool discMenus: true
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
        plex: plex
        jellyfin: jellyfin
        stereoDisplay: root.mpv.stereoDisplay
        focus: !player.active
        onSetupRequested: root.showSetup()
        onFilesRequested: filesScreen.open()
        onLibraryRequested: server => (server === plex ? plexScreen : jellyfinScreen).open()
        onPlayRequested: drive => {
            root.returnTo = null
            root.start(drive)
        }
    }

    // played from the file browser or a library: back there afterwards
    property Item returnTo: null
    function backFromPlaying() {
        if (returnTo)
            returnTo.reopen()
        else
            home.forceActiveFocus()
    }

    // a disc, or a video, disc folder or image from the file browser
    // (drive.file): its page if there's a choice to make, else play it
    function start(drive) {
        if (!drive.file)
            system.stopUhdChecks() // MakeMKV needs the drive to itself
        // a Blu-ray can't play without MakeMKV, nor a 4K disc without a
        // drive it can read them with: say why instead of failing (a
        // folder or image may be decrypted already: let it try)
        if (drive.kind === "bluray" && !drive.file) {
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
        // with disc menus off, the disc's page still offers its menu
        const menus = drive.kind === "dvd" || drive.kind === "bluray"
        if (episodes.length > 1 || extras.length || resume || (menus && !settings.discMenus))
            discScreen.open(drive)
        else if (menus && settings.discMenus)
            root.openMenus(drive) // a disc starts with its own menus
        else
            player.play(drive)
    }

    FilesScreen {
        id: filesScreen
        anchors.fill: parent
        onPlayRequested: drive => {
            visible = false
            root.returnTo = filesScreen
            root.start(drive)
        }
        onClosed: home.forceActiveFocus()
    }

    LibraryScreen {
        id: plexScreen
        anchors.fill: parent
        server: plex
        onPlayRequested: (drive, queue, start) => {
            visible = false
            root.returnTo = plexScreen
            player.play(drive, queue, 0, start)
        }
        onClosed: home.forceActiveFocus()
    }

    LibraryScreen {
        id: jellyfinScreen
        anchors.fill: parent
        server: jellyfin
        signIn: Component {
            JellyfinSignIn {
                server: jellyfin
                onCancelled: jellyfinScreen.close()
            }
        }
        onPlayRequested: (drive, queue, start) => {
            visible = false
            root.returnTo = jellyfinScreen
            player.play(drive, queue, 0, start)
        }
        onClosed: home.forceActiveFocus()
    }

    // the server's "continue watching" and watched marks follow what plays
    // here
    Timer {
        readonly property MediaServer server: player.active && player.drive ? root.libraryServer(player.drive.library) : null
        interval: 10000
        repeat: true
        running: !!server && !player.opening
        onTriggered: server.report(player.entry.itemId, root.mpv.paused ? "paused" : "playing",
                                   root.mpv.position, root.mpv.duration)
    }

    DiscScreen {
        id: discScreen
        anchors.fill: parent
        stereoDisplay: root.mpv.stereoDisplay
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
        onUrlRequested: (url, start) => {
            visible = false
            player.play(drive, [{ url: url, title: "" }], 0, start)
        }
        onMenuRequested: {
            visible = false
            root.openMenus(drive)
        }
        onExtraRequested: extra => {
            visible = false
            player.play(drive, [{ url: extra.url, title: extra.title, extra: true }], 0, 0)
        }
        onClosed: root.backFromPlaying()
    }

    SetupScreen {
        id: setup
        anchors.fill: parent
        check: system
        passthrough: settings.passthrough
        discMenus: settings.discMenus
        onDiscMenusToggled: settings.discMenus = !settings.discMenus
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
        onEntryStopped: (entry, position, duration, ended) => {
            const server = root.libraryServer(drive.library)
            if (!server || !entry.itemId)
                return
            server.report(entry.itemId, "stopped", position, duration)
            if (ended || position > duration * 0.9)
                server.scrobble(entry.itemId)
        }
        onClosed: wasExtra => {
            if (wasExtra)
                discScreen.reopen()
            else
                root.backFromPlaying()
        }
    }
}
