import QtQuick
import DiscPlayer

// What to play from a disc: resume, play all and the episodes of a series,
// or resume / start over for a film. Up/down choose, A plays, B goes back.
FocusScope {
    id: root

    property var drive: null
    readonly property var disc: drive && drive.disc ? drive.disc : ({})
    readonly property var episodes: disc.episodes || []
    readonly property bool series: episodes.length > 1
    property var resume: null

    // index into the play queue (episode), start position in seconds
    signal playRequested(int index, real start)
    signal closed()

    visible: false

    function open(d) {
        drive = d
        resume = Resume.get(d.contentHash || d.label)
        visible = true
        list.currentIndex = 0
        list.forceActiveFocus()
    }

    function close() {
        focus = false
        visible = false
        closed()
    }

    readonly property var rows: {
        const r = []
        if (resume)
            r.push({ label: "Resume", index: resume.index, start: resume.position,
                     detail: (series && episodes[resume.index] ? episodes[resume.index].title + "  ·  " : "")
                             + Theme.formatTime(resume.position) })
        if (series) {
            r.push({ label: "Play all", detail: episodes.length + " episodes", index: 0, start: 0 })
            episodes.forEach((e, i) => r.push({
                label: (i + 1) + ".  " + e.title, detail: Theme.formatTime(e.seconds), index: i, start: 0,
                watching: !!resume && resume.index === i }))
        } else {
            r.push({ label: "Play from the beginning", detail: "", index: 0, start: 0 })
        }
        return r
    }

    Keys.onEscapePressed: close()
    Keys.onBackPressed: close()

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
            text: root.disc.title || (root.drive ? root.drive.label : "")
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        Text {
            text: root.disc.release || ""
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
                font.weight: index === 0 ? Font.DemiBold : Font.Normal
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
            if (r)
                root.playRequested(r.index, r.start)
        }
    }

    Text {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        text: "Ⓐ Play    ▲ ▼ Choose    Ⓑ Back"
        color: Theme.textDim
        font.pixelSize: Theme.fontSmall
    }
}
