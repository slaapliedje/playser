import QtQuick
import Playser

// What to play from a disc: resume, play all and the episodes of a series,
// or resume / start over for a film, and the disc's extras (from the
// catalogue) on a list of their own. Up/down choose, A plays, B goes back.
FocusScope {
    id: root

    property var drive: null
    readonly property var disc: drive && drive.disc ? drive.disc : ({})
    readonly property var episodes: disc.episodes || []
    readonly property bool series: episodes.length > 1
    readonly property var extras: disc.extras || []
    property var resume: null
    // the extras list rather than the main one
    property bool showingExtras: false

    // index into the play queue (episode), start position in seconds
    signal playRequested(int index, real start)
    // a url with a start position (resuming a film or DVD title)
    signal urlRequested(string url, real start)
    // one of disc.extras
    signal extraRequested(var extra)
    // the disc through its own menus (DVD)
    signal menuRequested()
    signal closed()

    visible: false

    function open(d) {
        drive = d
        resume = Resume.get(d.contentHash || d.label)
        showingExtras = false
        visible = true
        list.currentIndex = 0
        list.forceActiveFocus()
    }

    // back from playing an extra: the list as it was left
    function reopen() {
        visible = true
        list.forceActiveFocus()
    }

    function back() {
        if (showingExtras) {
            showingExtras = false
            list.currentIndex = rows.findIndex(r => r.extras)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            return
        }
        focus = false
        visible = false
        closed()
    }

    function showExtras() {
        showingExtras = true
        list.currentIndex = 0
        list.positionViewAtBeginning()
    }

    // the catalogue's kinds of extra; the generic "Extra" and "Other" go unsaid
    readonly property var typeLabels: ({
        "DeletedScene": "Deleted scene", "Featurette": "Featurette", "Trailer": "Trailer",
        "Interview": "Interview", "Short": "Short", "Music": "Music", "Scene": "Scene"
    })

    readonly property var rows: {
        const r = []
        if (showingExtras) {
            extras.forEach(e => r.push({
                label: e.title || "Untitled", extra: e,
                detail: [typeLabels[e.type], Theme.formatTime(e.seconds)].filter(x => x).join("  ·  ") }))
            return r
        }
        if (resume)
            r.push({ label: "Resume", index: resume.index, start: resume.position,
                     url: series ? "" : (resume.url || ""),
                     detail: (series && episodes[resume.index] ? episodes[resume.index].title + "  ·  " : "")
                             + Theme.formatTime(resume.position) })
        if (series) {
            r.push({ label: "Play all", detail: episodes.length + " episodes", index: 0, start: 0 })
            if (drive && (drive.kind === "dvd" || drive.kind === "bluray"))
                r.push({ label: "Disc menu", detail: "", menu: true })
            if (extras.length)
                r.push({ label: "Extras", detail: extras.length + "", extras: true })
            episodes.forEach((e, i) => r.push({
                label: (i + 1) + ".  " + e.title, detail: Theme.formatTime(e.seconds), index: i, start: 0,
                watching: !!resume && resume.index === i }))
        } else {
            r.push({ label: resume ? "Play from the beginning" : "Play", detail: "", index: 0, start: 0 })
            if (drive && (drive.kind === "dvd" || drive.kind === "bluray"))
                r.push({ label: "Disc menu", detail: "", menu: true })
            if (extras.length)
                r.push({ label: "Extras", detail: extras.length + "", extras: true })
        }
        return r
    }

    Keys.onEscapePressed: back()
    Keys.onBackPressed: back()

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    Column {
        id: heading
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing * 0.4
        Text {
            width: parent.width
            text: root.disc.title || (root.drive ? Theme.titleFromLabel(root.drive.label) : "")
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            text: root.showingExtras ? "Extras" : (root.disc.release || "")
            color: Theme.textDim
            font.pixelSize: Theme.fontMedium
        }
    }

    ListView {
        id: list
        anchors {
            left: parent.left; right: parent.right; top: heading.bottom; bottom: hints.top
            leftMargin: Theme.margin; rightMargin: Theme.margin
            topMargin: Theme.spacing * 1.5; bottomMargin: Theme.spacing
        }
        clip: true
        model: root.rows
        spacing: Theme.spacing * 0.3
        highlightMoveDuration: 100
        preferredHighlightBegin: height * 0.3
        preferredHighlightEnd: height * 0.7
        highlightRangeMode: ListView.ApplyRange

        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool current: ListView.isCurrentItem && list.activeFocus
            width: ListView.view.width
            height: 76 * Theme.scale
            radius: Theme.radius / 2
            color: current ? Theme.surfaceRaised : Theme.surface
            border.color: current ? Theme.focusRing : "transparent"
            border.width: 3 * Theme.scale

            Text {
                anchors { left: parent.left; leftMargin: Theme.spacing; right: detail.left; rightMargin: Theme.spacing
                          verticalCenter: parent.verticalCenter }
                text: modelData.label
                color: modelData.watching ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontMedium
                font.weight: index === 0 && !root.showingExtras ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                id: detail
                anchors { right: parent.right; rightMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: modelData.detail
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Keys.onReturnPressed: {
            const r = root.rows[currentIndex]
            if (!r)
                return
            if (r.extras)
                root.showExtras()
            else if (r.menu)
                root.menuRequested()
            else if (r.url)
                root.urlRequested(r.url, r.start)
            else if (r.extra)
                root.extraRequested(r.extra)
            else
                root.playRequested(r.index, r.start)
        }
    }

    HintBar {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        hints: [
            { buttons: ["south"], text: "Play" },
            { buttons: ["dpadV"], text: "Choose" },
            { buttons: ["east"], text: "Back" },
        ]
    }
}
