// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// A Plex library: signing in (a code entered at plex.tv/link), then
// "Continue watching" and the libraries, a poster grid of a library's
// films or shows, a show's seasons and a season's episodes, and a page for
// the film or episode to play (resume or from the beginning). B goes back
// a page; Start signs out.
FocusScope {
    id: root

    required property PlexClient plex
    // drive: PlexClient.playable(); queue: [{ url, title, ratingKey,
    // duration }] (a season's episodes from the one chosen); start: seconds
    signal playRequested(var drive, var queue, real start)
    signal closed()

    visible: false

    // the pages opened to get here, the last one showing:
    // { kind: "home"|"grid"|"list"|"detail", title, subtitle, path, items,
    //   index, item (detail), siblings (detail: the season's episodes) }
    property var pages: []
    readonly property var page: pages.length ? pages[pages.length - 1] : null
    property bool loading: false
    // the latest load: an earlier one's reply is dropped
    property int loadSeq: 0
    property string error: ""
    readonly property bool ready: plex.state === "ready"

    function open() {
        visible = true
        pages = []
        plex.start()
        if (ready)
            showHome()
        forceActiveFocus()
    }

    // back from playing: the page as it was left, its progress refreshed
    function reopen() {
        visible = true
        if (page)
            reload()
        forceActiveFocus()
    }

    function close() {
        plex.cancelSignIn()
        focus = false
        visible = false
        closed()
    }

    Connections {
        target: root.plex
        function onStateChanged() {
            if (root.visible && root.ready && !root.pages.length)
                root.showHome()
        }
    }

    function push(p) {
        p.index = p.index || 0
        pages = pages.concat([p])
        focusPage()
    }

    function back() {
        if (pages.length <= 1) {
            close()
            return
        }
        pages = pages.slice(0, -1)
        loadSeq++
        loading = false
        error = ""
        focusPage()
    }

    // the current page's view, at its remembered row
    function focusPage() {
        Qt.callLater(() => {
            const v = page && page.kind === "grid" ? grid : list
            v.currentIndex = Math.max(0, Math.min(page ? page.index : 0, v.count - 1))
            v.positionViewAtIndex(v.currentIndex, page && page.kind === "grid" ? GridView.Center : ListView.Center)
            v.forceActiveFocus()
        })
    }

    // the current page's items replaced (pages is copied so bindings see it)
    function setItems(p, items) {
        const i = pages.indexOf(p)
        if (i < 0)
            return
        const copy = Object.assign({}, p, { items: items })
        pages = pages.slice(0, i).concat([copy], pages.slice(i + 1))
        if (i === pages.length - 1)
            focusPage()
    }

    function load(p, path, transform) {
        const seq = ++loadSeq
        loading = true
        error = ""
        plex.get(path, (container, message) => {
            if (seq !== loadSeq || pages.indexOf(p) < 0)
                return
            loading = false
            if (!container) {
                error = message
                return
            }
            setItems(p, transform(container))
        })
    }

    function showHome() {
        const p = { kind: "home", title: plex.serverName, subtitle: "Plex", items: [] }
        pages = [p]
        loadHome(p)
        focusPage()
    }

    // "Continue watching" (when there's something), then the film and show
    // libraries
    function loadHome(p) {
        load(p, "/library/sections", c => {
            const libs = (c.Directory || []).filter(d => d.type === "movie" || d.type === "show")
                .map(d => ({ label: d.title, detail: d.type === "movie" ? "Films" : "TV shows",
                             path: "/library/sections/" + d.key + "/all", library: true }))
            plex.get("/library/onDeck", (deck) => {
                const items = deck && deck.Metadata ? deck.Metadata : []
                const current = pages[0]
                if (!items.length || !current || current.kind !== "home")
                    return
                setItems(current, [{ label: "Continue watching", detail: items.length + "",
                                     deck: items }].concat(current.items.filter(r => !r.deck)))
            })
            return libs
        })
    }

    // the current page's content again, keeping its place (after playing)
    function reload() {
        const p = page
        if (p.kind === "home")
            loadHome(p)
        else if (p.kind === "detail")
            load(p, "/library/metadata/" + p.item.ratingKey, c => {
                const item = (c.Metadata || [])[0]
                if (item)
                    p.item = item
                return detailRows(p.item)
            })
        else if (p.path)
            load(p, p.path, c => c.Metadata || [])
    }

    function activate(r) {
        if (!r || loading)
            return
        const p = page
        p.index = p.kind === "grid" ? grid.currentIndex : list.currentIndex
        if (p.kind === "home") {
            if (r.deck)
                push({ kind: "grid", title: "Continue watching", subtitle: plex.serverName, items: r.deck })
            else
                openPath({ kind: "grid", title: r.label, subtitle: plex.serverName, path: r.path })
        } else if (r.type === "show") {
            openPath({ kind: "list", title: r.title, subtitle: r.year || "", path: "/library/metadata/" + r.ratingKey + "/children" })
        } else if (r.type === "season") {
            openPath({ kind: "list", title: r.parentTitle || p.title, subtitle: r.title, path: "/library/metadata/" + r.ratingKey + "/children" })
        } else if (r.type === "movie" || r.type === "episode") {
            const siblings = p.kind === "list" ? p.items.filter(i => i.type === "episode") : [r]
            push({ kind: "detail", item: r, siblings: siblings, items: detailRows(r) })
        } else if (r.play !== undefined) {
            play(p, r.play)
        }
    }

    function openPath(p) {
        p.items = []
        push(p)
        load(page, p.path, c => c.Metadata || [])
    }

    function detailRows(item) {
        const offset = (item.viewOffset || 0) / 1000
        return offset > 0
            ? [{ label: "Resume", detail: Theme.formatTime(offset), play: offset },
               { label: "Play from the beginning", detail: "", play: 0 }]
            : [{ label: "Play", detail: "", play: 0 }]
    }

    function play(p, start) {
        const drive = plex.playable(p.item)
        if (!drive.url) {
            error = "Plex has no file for this"
            return
        }
        const from = Math.max(0, p.siblings.findIndex(s => s.ratingKey === p.item.ratingKey))
        const queue = p.siblings.slice(from).map(s => plex.playable(s)).filter(d => d.url)
            .map(d => ({ url: d.url, title: d.title, ratingKey: d.ratingKey }))
        root.playRequested(drive, queue.length ? queue : [{ url: drive.url, title: drive.title, ratingKey: drive.ratingKey }], start)
    }

    function itemTitle(i) {
        if (i.type === "episode")
            return i.grandparentTitle ? i.grandparentTitle : i.title
        return i.title || ""
    }

    function itemSubtitle(i) {
        if (i.type === "episode")
            return "S" + i.parentIndex + " · E" + i.index + "  " + i.title
        if (i.type === "season")
            return i.leafCount ? i.leafCount + " episodes" : ""
        return i.year ? i.year + "" : ""
    }

    function progressOf(i) {
        return i.viewOffset && i.duration ? i.viewOffset / i.duration : 0
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            back()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu && plex.state !== "signedOut" && plex.state !== "linking") {
            signOutConfirm.open()
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && plex.state === "error") {
            plex.start()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    // signing in, finding the server, or why it can't be reached
    Column {
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: Theme.margin }
        visible: !root.ready
        spacing: Theme.spacing
        Text {
            text: root.plex.state === "linking" ? "Sign in to Plex"
                : root.plex.state === "error" ? root.plex.message
                : "Finding your Plex server…"
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
        }
        Text {
            visible: root.plex.state === "linking"
            width: parent.width
            wrapMode: Text.Wrap
            text: "On your phone or computer, go to <font color=\"" + Theme.accent + "\">plex.tv/link</font> and enter this code:"
            textFormat: Text.StyledText
            color: Theme.textDim
            font.pixelSize: Theme.fontLarge
        }
        Text {
            visible: root.plex.state === "linking"
            text: root.plex.linkCode || "…"
            color: Theme.accent
            font.pixelSize: Theme.fontHuge * 2
            font.weight: Font.Bold
            font.letterSpacing: Theme.fontHuge * 0.3
        }
    }

    Column {
        id: heading
        visible: root.ready
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing * 0.4
        Text {
            width: parent.width
            text: !root.page ? "" : root.page.kind === "detail" ? root.itemTitle(root.page.item) : root.page.title
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: !root.page ? "" : root.page.kind === "detail" ? root.itemSubtitle(root.page.item) : (root.page.subtitle || "")
            color: Theme.textDim
            font.pixelSize: Theme.fontMedium
            elide: Text.ElideRight
        }
    }

    // a library's films or shows, or what's being watched
    GridView {
        id: grid
        visible: root.ready && !!root.page && root.page.kind === "grid"
        anchors {
            left: parent.left; right: parent.right; top: heading.bottom; bottom: hints.top
            leftMargin: Theme.margin; rightMargin: Theme.margin; topMargin: Theme.spacing; bottomMargin: Theme.spacing
        }
        clip: true
        model: visible ? root.page.items : []
        readonly property int columns: 7
        cellWidth: Math.floor(width / columns)
        cellHeight: cellWidth * 1.5 + Theme.fontSmall * 3.2
        highlightMoveDuration: 100
        preferredHighlightBegin: height * 0.15
        preferredHighlightEnd: height * 0.85
        highlightRangeMode: GridView.ApplyRange
        keyNavigationWraps: false

        delegate: Item {
            required property var modelData
            required property int index
            readonly property bool current: GridView.isCurrentItem && grid.activeFocus
            // a show Plex has no poster for (not matched to its listing): a
            // frame from its first episode
            property string fallbackThumb: ""
            Component.onCompleted: {
                if (modelData.type === "show" && !modelData.thumb)
                    root.plex.get("/library/metadata/" + modelData.ratingKey
                                  + "/allLeaves?X-Plex-Container-Start=0&X-Plex-Container-Size=1", c => {
                        const first = c && c.Metadata ? c.Metadata[0] : null
                        if (first && first.thumb)
                            fallbackThumb = first.thumb
                    })
            }
            width: grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                id: poster
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.spacing * 0.3 }
                height: width * 1.5
                radius: Theme.radius / 2
                color: Theme.surface
                border.color: current ? Theme.focusRing : "transparent"
                border.width: 4 * Theme.scale
                scale: current ? 1.05 : 1.0
                z: current ? 1 : 0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Text {
                    anchors { fill: parent; margins: Theme.spacing * 0.5 }
                    visible: image.status !== Image.Ready
                    text: root.itemTitle(modelData)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Image {
                    id: image
                    anchors { fill: parent; margins: parent.border.width }
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    sourceSize: Qt.size(width, height)
                    source: root.plex.imageUrl(modelData.type === "episode" ? (modelData.grandparentThumb || modelData.thumb)
                                                                           : (modelData.thumb || fallbackThumb),
                                               Math.round(width), Math.round(height))
                }
                // how far through it is
                Rectangle {
                    visible: root.progressOf(modelData) > 0
                    anchors { left: image.left; right: image.right; bottom: image.bottom }
                    height: 8 * Theme.scale
                    color: Qt.rgba(0, 0, 0, 0.6)
                    Rectangle {
                        width: parent.width * root.progressOf(modelData)
                        height: parent.height
                        color: Theme.accent
                    }
                }
            }
            Column {
                anchors { left: poster.left; right: poster.right; top: poster.bottom; topMargin: Theme.spacing * 0.4 }
                Text {
                    width: parent.width
                    text: root.itemTitle(modelData)
                    color: current ? Theme.text : Theme.textDim
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.itemSubtitle(modelData)
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall * 0.85
                    elide: Text.ElideRight
                }
            }
        }

        Keys.onReturnPressed: root.activate(root.page.items[currentIndex])
    }

    // the libraries, a show's seasons, a season's episodes, or what to play
    ListView {
        id: list
        visible: root.ready && !!root.page && root.page.kind !== "grid"
        anchors {
            left: parent.left; right: summary.visible ? summary.left : parent.right; top: heading.bottom; bottom: hints.top
            leftMargin: Theme.margin; rightMargin: summary.visible ? Theme.spacing * 2 : Theme.margin
            topMargin: Theme.spacing * 1.5; bottomMargin: Theme.spacing
        }
        clip: true
        model: visible ? root.page.items : []
        spacing: Theme.spacing * 0.3
        highlightMoveDuration: 100
        preferredHighlightBegin: height * 0.3
        preferredHighlightEnd: height * 0.7
        highlightRangeMode: ListView.ApplyRange

        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            readonly property bool episode: modelData.type === "episode"
            width: ListView.view.width
            height: 76 * Theme.scale
            radius: Theme.radius / 2
            color: current ? Theme.surfaceRaised : Theme.surface
            border.color: current ? Theme.focusRing : "transparent"
            border.width: 3 * Theme.scale

            Text {
                anchors { left: parent.left; leftMargin: Theme.spacing; right: detail.left; rightMargin: Theme.spacing
                          verticalCenter: parent.verticalCenter }
                text: modelData.label !== undefined ? modelData.label
                    : episode ? modelData.index + ".  " + modelData.title
                    : modelData.title + (modelData.type === "season" ? "  ›" : "")
                color: Theme.text
                font.pixelSize: Theme.fontMedium
                font.weight: modelData.play !== undefined && index === 0 ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                id: detail
                anchors { right: parent.right; rightMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: modelData.detail !== undefined ? modelData.detail
                    : !episode ? root.itemSubtitle(modelData)
                    : modelData.viewOffset ? "Resume at " + Theme.formatTime(modelData.viewOffset / 1000)
                    : modelData.viewCount ? "Watched"
                    : Theme.formatTime((modelData.duration || 0) / 1000)
                color: episode && modelData.viewOffset ? Theme.accent : Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Keys.onReturnPressed: root.activate(root.page.items[currentIndex])
    }

    // the film or episode: its picture and summary
    Column {
        id: summary
        visible: root.ready && !!root.page && root.page.kind === "detail"
        anchors { right: parent.right; top: heading.bottom; rightMargin: Theme.margin; topMargin: Theme.spacing * 1.5 }
        width: parent.width * 0.45
        spacing: Theme.spacing
        Image {
            width: parent.width
            height: width * 9 / 16
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            sourceSize: Qt.size(width, height)
            source: summary.visible ? root.plex.imageUrl(root.page.item.art || root.page.item.thumb || "", Math.round(width), Math.round(height)) : ""
        }
        Text {
            width: parent.width
            text: !summary.visible ? "" : [root.page.item.type === "movie" ? root.page.item.year : "",
                   Theme.formatTime((root.page.item.duration || 0) / 1000),
                   root.page.item.contentRating || ""].filter(x => x).join("  ·  ")
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
        }
        Text {
            width: parent.width
            text: summary.visible ? (root.page.item.summary || "") : ""
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            maximumLineCount: 10
            elide: Text.ElideRight
        }
    }

    Text {
        anchors.centerIn: parent
        visible: root.ready && (root.loading || root.error !== "" || (!!root.page && !root.page.items.length))
        text: root.error !== "" ? root.error : root.loading ? "Loading…" : "Nothing here"
        color: root.error !== "" ? Theme.warning : Theme.textDim
        font.pixelSize: Theme.fontLarge
    }

    HintBar {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        hints: root.plex.state === "error"
            ? [{ buttons: ["south"], text: "Try again" }, { buttons: ["start"], text: "Sign out" }, { buttons: ["east"], text: "Back" }]
            : root.plex.state === "linking"
            ? [{ buttons: ["east"], text: "Back" }]
            : [{ buttons: ["south"], text: root.page && root.page.kind === "detail" ? "Play" : "Open" },
               { buttons: [root.page && root.page.kind === "grid" ? "dpad" : "dpadV"], text: "Choose" },
               { buttons: ["start"], text: "Sign out" },
               { buttons: ["east"], text: "Back" }]
    }

    ConfirmDialog {
        id: signOutConfirm
        text: "Sign out of Plex?"
        acceptText: "Sign out"
        cancelText: "Stay"
        returnFocus: root
        onAccepted: {
            root.pages = []
            root.plex.signOut()
            root.close()
        }
    }
}
