// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Playser

// A media server's library (Plex, Jellyfin): signing in, then "Continue
// watching" and the libraries, a poster grid of a library's films or shows,
// a show's seasons and a season's episodes, and a page for the film or
// episode to play (resume or from the beginning). Y searches; B goes back
// a page; Start signs out.
FocusScope {
    id: root

    required property MediaServer server
    // the server's own sign-in steps (states this screen doesn't know)
    property Component signIn: null
    // drive: MediaServer.playable(); queue: [{ url, title, itemId }] (a
    // season's episodes from the one chosen); start: seconds
    signal playRequested(var drive, var queue, real start)
    signal closed()

    visible: false

    // the pages opened to get here, the last one showing:
    // { kind: "home"|"grid"|"list"|"detail", title, subtitle, items, index,
    //   library (grid: its id), deck (grid: Continue watching), search (grid:
    //   what was searched for), parent (list: the show or season), item
    //   (detail), siblings (detail: the season's episodes) }
    property var pages: []
    readonly property var page: pages.length ? pages[pages.length - 1] : null
    property bool loading: false
    // the latest load: an earlier one's reply is dropped
    property int loadSeq: 0
    property string error: ""
    readonly property bool ready: server.state === "ready"
    readonly property bool plex: server.service === "Plex"
    // the server's own sign-in step is showing
    readonly property bool ownSignIn: !["signedOut", "linking", "connecting", "ready", "error"].includes(server.state)
    // Plex: the server switched away from ({ id, name }), to go back to if
    // the new one can't be reached
    property var switchedFrom: null

    function open() {
        visible = true
        pages = []
        server.start()
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
        server.cancelSignIn()
        focus = false
        visible = false
        closed()
    }

    Connections {
        target: root.server
        function onStateChanged() {
            if (root.ready)
                root.switchedFrom = null
            if (root.visible && root.ready && !root.pages.length)
                root.showHome()
            if (!root.ready && root.visible)
                Qt.callLater(() => {
                    if (root.visible) // not closed meanwhile
                        (root.ownSignIn && signInView.item ? signInView.item : root).forceActiveFocus()
                })
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
        reload() // what was played meanwhile: progress, Continue watching
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

    // fetch(callback) asks the server; its answer becomes p's items
    function load(p, fetch, transform) {
        const seq = ++loadSeq
        loading = true
        error = ""
        fetch((result, message) => {
            if (seq !== loadSeq || pages.indexOf(p) < 0)
                return
            loading = false
            if (!result) {
                error = message
                return
            }
            setItems(p, transform ? transform(result) : result)
        })
    }

    function showHome() {
        const p = { kind: "home", title: server.serverName, subtitle: server.service, items: [] }
        pages = [p]
        loadHome(p)
        focusPage()
    }

    // "Continue watching" (when there's something), then the film and show
    // libraries, then the other servers there are (Plex: shared ones)
    function loadHome(p) {
        load(p, cb => server.libraries(cb), libs => {
            server.continueWatching(deck => {
                const current = pages[0]
                if (!deck || !deck.length || !current || current.kind !== "home")
                    return
                setItems(current, [{ label: "Continue watching", detail: deck.length + "", deck: true }]
                                  .concat(current.items.filter(r => !r.deck)))
            })
            return libs.map(l => ({ label: l.title, detail: l.type === "movie" ? "Films" : "TV shows", library: l.id }))
                .concat(plex && server.servers.length > 1
                        ? [{ label: "Other servers", detail: server.servers.length - 1 + "", servers: true }] : [])
        })
    }

    // Plex: the servers to switch to
    function serverRows() {
        return server.servers.map(s => ({
            label: s.name,
            detail: (s.current ? "Open  ·  " : !s.online ? "Offline  ·  " : "")
                    + (s.owned ? "Yours" : s.owner ? "Shared by " + s.owner : "Shared"),
            serverId: s.id, current: s.current
        }))
    }

    // the current page's content again, keeping its place (after playing)
    function reload() {
        const p = page
        if (p.kind === "home")
            loadHome(p)
        else if (p.kind === "detail")
            load(p, cb => server.refresh(p.item, cb), item => {
                p.item = item
                return detailRows(item)
            })
        else
            fetchPage(p)
    }

    function fetchPage(p) {
        if (p.deck)
            load(p, cb => server.continueWatching(cb))
        else if (p.search !== undefined)
            load(p, cb => server.search(p.search, cb))
        else if (p.library !== undefined)
            load(p, cb => server.items(p.library, cb))
        else if (p.parent)
            load(p, cb => server.children(p.parent, cb))
    }

    function activate(r) {
        if (!r || loading)
            return
        const p = page
        p.index = p.kind === "grid" ? grid.currentIndex : list.currentIndex
        if (r.servers)
            push({ kind: "list", title: "Plex servers", subtitle: "Yours and those shared with you", items: serverRows() })
        else if (r.serverId !== undefined) {
            if (r.current) {
                back()
                return
            }
            switchedFrom = { id: server.servers.find(s => s.current).id, name: server.serverName }
            pages = [] // the new server's home, once it answers
            server.useServer(r.serverId)
        } else if (p.kind === "home")
            openPage({ kind: "grid", title: r.label, subtitle: server.serverName, deck: !!r.deck, library: r.library })
        else if (r.type === "show")
            openPage({ kind: "list", title: r.title, subtitle: r.year ? r.year + "" : "", parent: r })
        else if (r.type === "season")
            openPage({ kind: "list", title: r.show || p.title, subtitle: r.title, parent: r })
        else if (r.type === "movie" || r.type === "episode") {
            const siblings = p.kind === "list" ? p.items.filter(i => i.type === "episode") : [r]
            push({ kind: "detail", item: r, siblings: siblings, items: detailRows(r) })
            if (p.search !== undefined) // results can leave out what plays
                reload()
        } else if (r.play !== undefined)
            play(p, r.play)
    }

    function openPage(p) {
        p.items = []
        push(p)
        fetchPage(page)
    }

    function search(query) {
        query = query.trim()
        if (query === "")
            return
        if (page.kind !== "detail")
            page.index = page.kind === "grid" ? grid.currentIndex : list.currentIndex
        if (page.search !== undefined) // a new search replaces the last one
            pages = pages.slice(0, -1)
        openPage({ kind: "grid", title: "\u201c" + query + "\u201d", subtitle: "Search " + server.serverName, search: query })
    }

    function detailRows(item) {
        return item.position > 0
            ? [{ label: "Resume", detail: Theme.formatTime(item.position), play: item.position },
               { label: "Play from the beginning", detail: "", play: 0 }]
            : [{ label: "Play", detail: "", play: 0 }]
    }

    function play(p, start) {
        const drive = server.playable(p.item)
        if (!drive.url) {
            error = server.service + " has no file for this"
            return
        }
        const from = Math.max(0, p.siblings.findIndex(s => s.id === p.item.id))
        const queue = p.siblings.slice(from).map(s => server.playable(s)).filter(d => d.url)
            .map(d => ({ url: d.url, title: d.title, itemId: d.itemId }))
        root.playRequested(drive, queue.length ? queue : [{ url: drive.url, title: drive.title, itemId: drive.itemId }], start)
    }

    function itemTitle(i) {
        if (i.type === "episode")
            return i.show || i.title
        return i.title || ""
    }

    function itemSubtitle(i) {
        if (i.type === "episode")
            return "S" + i.season + " · E" + i.number + "  " + i.title
        if (i.type === "season")
            return i.childCount ? i.childCount + " episodes" : ""
        return i.year ? i.year + "" : ""
    }

    function progressOf(i) {
        return i.position && i.duration ? i.position / i.duration : 0
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            back()
            event.accepted = true
        } else if (event.key === Qt.Key_I && ready && page) {
            searchKeyboard.open(page.search || "")
            event.accepted = true
        } else if (event.key === Qt.Key_Menu && server.signedIn) {
            signOutConfirm.open()
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && server.state === "error") {
            server.start()
            event.accepted = true
        } else if (event.key === Qt.Key_I && server.state === "linking" && !plex) {
            server.usePassword()
            event.accepted = true
        } else if (event.key === Qt.Key_I && server.state === "error" && !plex) {
            server.changeServer()
            event.accepted = true
        } else if (event.key === Qt.Key_I && server.state === "error" && switchedFrom) {
            server.useServer(switchedFrom.id)
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
        visible: !root.ready && !root.ownSignIn
        spacing: Theme.spacing
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: root.server.state === "linking" ? "Sign in to " + (root.plex ? "Plex" : root.server.serverName)
                : root.server.state === "error" ? root.server.message
                : root.plex ? "Connecting to Plex…" : "Connecting to Jellyfin…"
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
        }
        Text {
            visible: root.server.state === "linking"
            width: parent.width
            wrapMode: Text.Wrap
            text: root.plex
                ? "On your phone or computer, go to <font color=\"" + Theme.accent + "\">plex.tv/link</font> and enter this code:"
                : "In Jellyfin on your phone or computer, open your profile, choose <font color=\"" + Theme.accent
                  + "\">Quick Connect</font> and enter this code:"
            textFormat: Text.StyledText
            color: Theme.textDim
            font.pixelSize: Theme.fontLarge
        }
        Text {
            visible: root.server.state === "linking"
            text: root.server.linkCode || "…"
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
            // a show with no poster (not matched to its listing): a frame
            // from its first episode
            property string fallbackThumb: ""
            Component.onCompleted: {
                if (modelData.type === "show" && !modelData.thumb)
                    root.server.firstEpisode(modelData, first => {
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
                    source: root.server.imageUrl(modelData.type === "episode" ? (modelData.showThumb || modelData.thumb)
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
                    : episode ? modelData.number + ".  " + modelData.title
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
                    : modelData.position > 0 ? "Resume at " + Theme.formatTime(modelData.position)
                    : modelData.watched ? "Watched"
                    : Theme.formatTime(modelData.duration || 0)
                color: episode && modelData.position > 0 ? Theme.accent : Theme.textDim
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
            source: summary.visible ? root.server.imageUrl(root.page.item.art || root.page.item.thumb || "", Math.round(width), Math.round(height)) : ""
        }
        Text {
            width: parent.width
            text: !summary.visible ? "" : [root.page.item.type === "movie" ? root.page.item.year : "",
                   Theme.formatTime(root.page.item.duration || 0),
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
        text: root.error !== "" ? root.error : root.loading ? "Loading…"
            : root.page && root.page.search !== undefined ? "Nothing found" : "Nothing here"
        color: root.error !== "" ? Theme.warning : Theme.textDim
        font.pixelSize: Theme.fontLarge
    }

    HintBar {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        visible: !root.ownSignIn
        hints: root.server.state === "error"
            ? [{ buttons: ["south"], text: "Try again" }].concat(
                root.plex ? (root.switchedFrom ? [{ buttons: ["north"], text: "Back to " + root.switchedFrom.name }] : [])
                          : [{ buttons: ["north"], text: "Another server" }],
                root.server.signedIn ? [{ buttons: ["start"], text: "Sign out" }] : [],
                [{ buttons: ["east"], text: "Back" }])
            : root.server.state === "linking"
            ? (root.plex ? [] : [{ buttons: ["north"], text: "Use a password" }]).concat([{ buttons: ["east"], text: "Back" }])
            : [{ buttons: ["south"], text: root.page && root.page.kind === "detail" ? "Play" : "Open" },
               { buttons: [root.page && root.page.kind === "grid" ? "dpad" : "dpadV"], text: "Choose" },
               { buttons: ["north"], text: "Search" },
               { buttons: ["start"], text: "Sign out" },
               { buttons: ["east"], text: "Back" }]
    }

    OnScreenKeyboard {
        id: searchKeyboard
        anchors.fill: parent
        visible: false
        label: "Search " + root.server.serverName
        placeholder: "A film, show or episode"
        onAccepted: typed => {
            root.focusPage()
            root.search(typed)
        }
        onCancelled: root.focusPage()
    }

    ConfirmDialog {
        id: signOutConfirm
        text: "Sign out of " + root.server.service + "?"
        acceptText: "Sign out"
        cancelText: "Stay"
        returnFocus: root
        onAccepted: {
            root.pages = []
            root.server.signOut()
            root.close()
        }
    }

    // the server's own sign-in steps (Jellyfin: its address, a password)
    Loader {
        id: signInView
        anchors.fill: parent
        active: root.visible && root.ownSignIn && !!root.signIn
        sourceComponent: root.signIn
        onLoaded: item.forceActiveFocus()
    }
}
