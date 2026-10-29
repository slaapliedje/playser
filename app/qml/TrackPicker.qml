import QtQuick
import DiscPlayer

// Audio and subtitle selection panel. Left/right switches column, up/down
// moves, A selects, B closes.
FocusScope {
    id: root

    required property Mpv mpv
    signal dismissed()

    visible: false

    function open() {
        visible = true
        audioList.currentIndex = Math.max(0, indexOfSelected(audioModel))
        subList.currentIndex = Math.max(0, indexOfSelected(subModel))
        audioList.forceActiveFocus()
    }

    function close() {
        // drop focus first: the parent FocusScope would otherwise hand
        // focus straight back to this (hidden) child
        focus = false
        visible = false
        dismissed()
    }

    function trackLabel(t) {
        const parts = []
        if (t.lang) parts.push(t.lang.toUpperCase())
        if (t.title) parts.push(t.title)
        if (t.codec) parts.push(t.codec.toUpperCase())
        if (t["demux-channel-count"]) parts.push(t["demux-channel-count"] + "ch")
        return parts.length ? parts.join("  ·  ") : "Track " + t.id
    }

    function indexOfSelected(model) {
        for (let i = 0; i < model.length; i++)
            if (model[i].selected) return i
        return -1
    }

    readonly property var audioModel: mpv.tracks.filter(t => t.type === "audio")
                                          .map(t => ({ id: t.id, label: trackLabel(t), selected: t.selected }))
    readonly property var subModel: [{ id: "no", label: "Off", selected: !mpv.tracks.some(t => t.type === "sub" && t.selected) }]
        .concat(mpv.tracks.filter(t => t.type === "sub")
                .map(t => ({ id: t.id, label: trackLabel(t) + (t.forced ? "  (forced)" : ""), selected: t.selected })))

    Keys.onEscapePressed: close()
    Keys.onBackPressed: close()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_I || event.key === Qt.Key_Menu) {
            close()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#c0000000"
    }

    readonly property alias audioList: audioCol.list
    readonly property alias subList: subCol.list

    Row {
        anchors.centerIn: parent
        spacing: Theme.spacing * 2

        TrackColumn {
            id: audioCol
            title: "Audio"
            model: root.audioModel
            list.KeyNavigation.right: subCol.list
            onChosen: id => root.mpv.setMpvProperty("aid", id)
        }
        TrackColumn {
            id: subCol
            title: "Subtitles"
            model: root.subModel
            list.KeyNavigation.left: audioCol.list
            onChosen: id => root.mpv.setMpvProperty("sid", id)
        }
    }

    component TrackColumn: Rectangle {
        id: col
        property string title
        property var model
        readonly property alias list: view
        signal chosen(var id)

        width: 760 * Theme.scale
        height: 640 * Theme.scale
        radius: Theme.radius
        color: Theme.surface
        border.color: view.activeFocus ? Theme.focusRing : "transparent"
        border.width: 3 * Theme.scale

        Text {
            id: heading
            anchors { left: parent.left; top: parent.top; margins: Theme.spacing * 1.2 }
            text: col.title
            color: Theme.text
            font.pixelSize: Theme.fontMedium
            font.weight: Font.DemiBold
        }
        ListView {
            id: view
            anchors {
                left: parent.left; right: parent.right; top: heading.bottom; bottom: parent.bottom
                margins: Theme.spacing
            }
            clip: true
            model: col.model
            delegate: rowDelegate
            Keys.onReturnPressed: if (col.model[currentIndex]) col.chosen(col.model[currentIndex].id)
        }
    }

    Component {
        id: rowDelegate
        Rectangle {
            required property var modelData
            required property int index
            width: ListView.view.width
            height: 72 * Theme.scale
            radius: Theme.radius / 2
            color: ListView.isCurrentItem && ListView.view.activeFocus ? Theme.surfaceRaised : "transparent"
            Text {
                anchors { left: parent.left; leftMargin: Theme.spacing; verticalCenter: parent.verticalCenter }
                text: (modelData.selected ? "● " : "○ ") + modelData.label
                color: modelData.selected ? Theme.accent : Theme.text
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
