// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

FocusScope {
    id: root

    required property Mpv mpv
    // plays a disc through its menus (playMenu), or a Blu-ray's episodes
    // on one open of the disc (play)
    property DiscNav nav
    readonly property bool navigating: !!nav && nav.active
    readonly property bool discMenus: navigating && nav.menus
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
        : drive.disc && drive.disc.title ? drive.disc.title : Theme.titleFromLabel(drive.label)
    // the episode's title for a series, or the extra's; else empty
    readonly property string entryTitle: queue.length > 1 || entry.extra ? (entry.title || "") : ""
    // where play() started the queue, for falling back to mpv
    property real queueStart: 0
    // from play() until mpv has opened the title; a UHD takes ~20 s
    property bool opening: false
    // what playMenu() read the disc from, for opening it again
    property string menuPath: ""
    // where it was when the system went to sleep, until it wakes: { place,
    // menus, index }; a drive or a stream doesn't carry on after sleep, so
    // waking opens it again there
    property var sleptAt: null
    // from sleep until a little after waking: the drive may seem empty a
    // moment as it comes back, which isn't the disc being taken out
    property bool waking: false
    // wasExtra: it played one of the disc's extras
    signal closed(bool wasExtra)
    // an entry played through mpv stopped where it was (position, of
    // duration, in seconds), or at its end (ended): for Plex's progress
    signal entryStopped(var entry, real position, real duration, bool ended)

    // q: the queue (default: the disc's own url); i: entry; start: seconds
    function play(d, q, i, start) {
        drive = d
        queue = q || [{ url: d.url, title: displayTitle }]
        active = true
        forceActiveFocus()
        // a Blu-ray's episodes play on one open of the disc (each open
        // would redo AACS, seconds to a minute); mpv opens anything else
        const playlists = d.kind === "bluray" && queue.length > 1 ? queue.map(e => {
            const m = /^bd:\/\/mpls\/(\d+)$/.exec(e.url || "")
            return m ? parseInt(m[1]) : -1
        }) : []
        queueStart = start || 0
        if (playlists.length && playlists.every(p => p >= 0)) {
            index = i || 0
            opening = true
            nav.openPlaylists(d.device, playlists, index, start || 0)
        } else {
            loadEntry(i || 0, start || 0)
        }
    }

    // the disc from its start, menus and all (DVD, Blu-ray), read from path
    // (default: the device); opening continues in the background
    // (onOpenFailed)
    function playMenu(d, path) {
        drive = d
        menuPath = path || ""
        queue = [{ url: "discnav://", title: displayTitle }]
        index = 0
        active = true
        opening = true
        forceActiveFocus()
        nav.open(path || d.device)
    }

    function showNotice(message) {
        notice.text = message
        noticeBox.opacity = 1
        noticeTimer.restart()
    }

    function loadEntry(i, start) {
        index = i
        opening = true
        mpv.load(entry.url, drive.options || {}, start)
        showOsd()
    }

    // where it's playing: { index, position, url, title }, or null (still
    // opening, or in a disc's menu). url: what to play to come back (a
    // title through menus comes back as that title, without the menus: a
    // DVD's title, a Blu-ray's playlist)
    function place() {
        if (opening)
            return null
        if (navigating) {
            const p = nav.position()
            if (!p.title) // nothing to keep in a menu
                return null
            if (!nav.menus)
                return { index: index, position: Math.floor(p.seconds), url: entry.url, title: entry.title || "" }
            const url = nav.format === "bluray" ? "bd://mpls/" + p.playlist
                                                : "dvd://" + (p.title - 1) // mpv counts titles from 0
            const item = ((drive.disc && drive.disc.items) || []).find(i => i.url === url)
            return { index: 0, position: Math.floor(p.seconds), url: url, title: item ? item.title : "" }
        }
        if (root.mpv.duration > 0)
            return { index: index, position: Math.floor(root.mpv.position), url: entry.url, title: entry.title || "" }
        return null
    }

    function saveResume() {
        if (entry.extra || (drive && drive.library)) // a media server keeps its own
            return
        const p = place()
        if (p)
            Resume.save(resumeKey, p)
    }

    // the system is going to sleep: pause, and keep the place
    function sleep() {
        if (!active || sleptAt)
            return
        waking = true
        wakingTimer.stop()
        mpv.setMpvProperty("pause", true)
        saveResume()
        sleptAt = { place: place(), menus: navigating && nav.menus, index: index }
    }

    // and woke: open it again where it was, paused (in a disc's menu, or
    // still opening, it starts over)
    function wake() {
        const s = sleptAt
        sleptAt = null
        wakingTimer.restart()
        if (!s || !active)
            return
        const d = drive, q = queue, p = s.place
        if (navigating)
            nav.close() // what was open can't read the drive any more
        if (s.menus && !p) {
            mpv.setMpvProperty("pause", false) // a menu paused wouldn't move
            playMenu(d, menuPath)
            return
        }
        if (s.menus)
            play(d, [{ url: p.url, title: p.title }], 0, p.position)
        else
            play(d, q, p ? p.index : s.index, p ? p.position : queueStart)
        showNotice("Paused while the system slept")
    }
    Timer {
        id: wakingTimer
        interval: 20000
        onTriggered: root.waking = false
    }

    // removed: the disc was taken out, so there's no position to read
    // and nothing to go back to
    function close(removed) {
        if (!removed)
            saveResume()
        if (!opening && !navigating && root.mpv.duration > 0)
            entryStopped(entry, root.mpv.position, root.mpv.duration, false)
        const wasNavigating = navigating
        if (nav)
            nav.close() // stops mpv too; also drops an open still under way
        if (!wasNavigating)
            mpv.stop()
        active = false
        opening = false
        picker.visible = false
        closed(!removed && !!entry.extra)
    }

    function showOsd() {
        if (navigating && nav.inMenu)
            return // the disc's menu is the display
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
        function onAudioProblem(message) { root.showNotice(message) }
        function onEndFile(reason) {
            // DiscNav loads the next segment; asleep, waking opens it again
            if (!root.active || root.navigating || root.sleptAt)
                return
            if (reason === "eof")
                root.entryStopped(root.entry, root.mpv.duration, root.mpv.duration, true)
            if (reason === "eof" && root.index + 1 < root.queue.length) {
                root.loadEntry(root.index + 1, 0)
            } else if (reason === "eof") {
                if (!root.entry.extra)
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

    Connections {
        target: root.nav
        // a menu can come before any video (a BD-J menu over a still
        // background): show it, or the opening screen would cover it
        function onGraphicsChanged() {
            if (root.nav.graphics && root.opening && root.navigating)
                root.opening = false
        }
        function onStopped() {
            if (!root.active || root.sleptAt) // asleep: waking opens it again
                return
            if (!root.nav.menus && root.index === root.queue.length - 1) {
                Resume.clear(root.resumeKey) // watched to the end
                root.opening = true // keep close() from saving the final position
            }
            root.close()
        }
        // episodes on one open of the disc: which one plays
        function onTitleChanged() {
            if (root.active && !root.nav.menus && root.nav.title > 0) {
                root.index = root.nav.title - 1
                root.showOsd()
            }
        }
        // the menus didn't start (e.g. a BD-J disc the menus can't run):
        // play the film the usual way if there's one
        function onOpenFailed() {
            if (!root.active)
                return
            if (!root.nav.menus) { // episodes: one at a time through mpv
                root.loadEntry(root.index, root.queueStart)
                return
            }
            const d = root.drive
            if (d && d.url) {
                root.play(d)
                root.showNotice("The disc's menus didn't start: playing the film")
            } else {
                root.opening = false
                root.active = false
                root.closed(false)
            }
        }
        function onInMenuChanged() {
            if (root.nav.inMenu)
                osd.opacity = 0
            else
                root.showOsd()
        }
    }

    // where a title played through the disc's menus is: { title, seconds,
    // length }, polled while the OSD shows it
    property var navPosition: ({})
    Timer {
        interval: 500
        repeat: true
        triggeredOnStart: true
        running: root.navigating && !root.nav.inMenu && osd.visible
        onTriggered: root.navPosition = root.nav.position()
    }

    function navSeek(delta) {
        const p = nav.position()
        if (p.title)
            nav.seek(p.seconds + delta)
        showOsd()
    }

    // Through the disc's menus: d-pad and A work its buttons, B goes to the
    // menu above (DVD), or asks before leaving the disc where there's none
    // (a DVD's top menu, any Blu-ray menu: their submenus have their own
    // return buttons); during a title left/right seek,
    // LB/RB skip chapters. Start opens the disc menu (a Blu-ray's pop-up
    // menu), View the title menu (a Blu-ray's top menu).
    function navKey(key) {
        if (nav.inMenu) {
            switch (key) {
            case Qt.Key_Up: nav.action("up"); return true
            case Qt.Key_Down: nav.action("down"); return true
            case Qt.Key_Left: nav.action("left"); return true
            case Qt.Key_Right: nav.action("right"); return true
            case Qt.Key_Return:
            case Qt.Key_Enter: nav.action("activate"); return true
            case Qt.Key_Escape:
            case Qt.Key_Back: if (!nav.action("back")) leaveConfirm.open(); return true
            }
        } else {
            switch (key) {
            case Qt.Key_Left: navSeek(-10); return true
            case Qt.Key_Right: navSeek(30); return true
            case Qt.Key_PageUp: nav.action("previous"); showOsd(); return true
            case Qt.Key_PageDown: nav.action("next"); showOsd(); return true
            }
        }
        if (!nav.menus)
            return false // episodes without menus: Start is the track picker, as ever
        const bluray = nav.format === "bluray"
        switch (key) { // leaving a title: keep its position first
        case Qt.Key_Menu: saveResume(); nav.action(bluray ? "popup" : "menu"); return true
        case Qt.Key_TopMenu: saveResume(); nav.action(bluray ? "menu" : "title"); return true
        }
        return false
    }

    Keys.onPressed: event => {
        if (navigating && navKey(event.key)) {
            event.accepted = true
            return
        }
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
                                    : root.drive && root.drive.disc && root.drive.disc.stereo ? "Blu-ray 3D"
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
            // a disc with read errors takes the drive a minute of retries
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: slow.shown
                text: "Still reading the disc. A dirty or scratched disc can take a minute."
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
            HintBar {
                anchors.horizontalCenter: parent.horizontalCenter
                hints: [{ buttons: ["east"], text: "Cancel" }]
            }
        }
        Timer {
            id: slow
            property bool shown: false
            interval: 20000
            running: root.opening
            onRunningChanged: if (running) shown = false
            onTriggered: shown = true
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
                      + (root.entryTitle ? "  ·  " + (root.queue.length > 1 ? (root.index + 1) + ". " : "") + root.entryTitle : "")
                color: Theme.text
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            // (through a disc's menus mpv plays segments, so the times come
            // from the disc: navPosition)
            Rectangle {
                width: parent.width
                height: 10 * Theme.scale
                radius: height / 2
                color: "#40ffffff"
                Rectangle {
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                    width: root.navigating
                           ? (root.navPosition.length > 0 ? parent.width * Math.min(1, root.navPosition.seconds / root.navPosition.length) : 0)
                           : root.mpv.duration > 0 ? parent.width * Math.min(1, root.mpv.position / root.mpv.duration) : 0
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
                              + (root.navigating ? Theme.formatTime(root.navPosition.seconds || 0) + " / " + Theme.formatTime(root.navPosition.length || 0)
                                                 : Theme.formatTime(root.mpv.position) + " / " + Theme.formatTime(root.mpv.duration))
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                    }
                }
                Text {
                    anchors.right: parent.right
                    visible: root.mpv.chapters.length > 0 && !root.navigating
                    text: "Chapter " + (root.mpv.chapter + 1) + " / " + root.mpv.chapters.length
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall
                }
            }

            HintBar {
                textSize: Theme.fontSmall * 0.85
                hints: root.discMenus ? [
                    { buttons: ["south"], text: root.mpv.paused ? "Play" : "Pause" },
                    { buttons: ["dpadH"], text: "Seek" },
                    { buttons: ["lb", "rb"], text: "Chapter" },
                    { buttons: ["start"], text: "Disc menu" },
                    { buttons: ["north"], text: "Audio & subtitles" },
                    { buttons: ["east"], text: "Stop" },
                ] : [
                    { buttons: ["south"], text: root.mpv.paused ? "Play" : "Pause" },
                    { buttons: ["dpadH"], text: "Seek" },
                    { buttons: ["lb", "rb"], text: "Chapter" },
                    { buttons: ["north"], text: "Audio & subtitles" },
                    { buttons: ["east"], text: "Stop" },
                ]
            }
        }
    }

    ConfirmDialog {
        id: leaveConfirm
        z: 10
        text: "Leave the disc?"
        acceptText: "Leave"
        cancelText: "Stay"
        returnFocus: root
        onAccepted: root.close()
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
        nav: root.nav
        onDismissed: { root.forceActiveFocus(); root.showOsd() }
    }
}
