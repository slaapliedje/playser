import QtQuick
import DiscPlayer

FocusScope {
    id: root

    required property DiscMonitor monitor
    signal playRequested(var drive)

    readonly property var drives: monitor.drives

    Text {
        id: heading
        anchors { left: parent.left; top: parent.top; margins: Theme.margin }
        text: "Disc Player"
        color: Theme.text
        font.pixelSize: Theme.fontHuge
        font.weight: Font.DemiBold
    }

    ListView {
        id: list
        anchors {
            left: parent.left; right: parent.right; top: heading.bottom; bottom: hints.top
            leftMargin: Theme.margin; rightMargin: Theme.margin; topMargin: Theme.spacing * 2
        }
        orientation: ListView.Horizontal
        spacing: Theme.spacing * 1.5
        focus: true
        model: root.drives
        keyNavigationWraps: false
        highlightMoveDuration: 150

        delegate: DriveCard {
            required property var modelData
            required property int index
            drive: modelData
            focused: ListView.isCurrentItem && root.activeFocus
        }

        Keys.onReturnPressed: {
            const d = root.drives[currentIndex]
            if (d && d.url)
                root.playRequested(d)
        }
    }

    Text {
        anchors.centerIn: parent
        visible: root.drives.length === 0
        text: monitor.available ? "No optical drive found" : "Can't reach udisks2"
        color: Theme.textDim
        font.pixelSize: Theme.fontLarge
    }

    Text {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        text: "Ⓐ Play    ◀ ▶ Choose drive"
        color: Theme.textDim
        font.pixelSize: Theme.fontSmall
    }

    component DriveCard: Rectangle {
        property var drive
        property bool focused: false

        readonly property bool playable: !!drive.url
        readonly property string kindLabel: ({
            "bluray": "Blu-ray", "dvd": "DVD", "cd": "CD", "none": "No disc", "unknown": "Disc"
        })[drive.kind] || drive.kind

        width: 640 * Theme.scale
        height: 360 * Theme.scale
        radius: Theme.radius
        color: focused ? Theme.surfaceRaised : Theme.surface
        border.color: focused ? Theme.focusRing : "transparent"
        border.width: 4 * Theme.scale
        scale: focused ? 1.04 : 1.0
        Behavior on scale { NumberAnimation { duration: 120 } }

        Column {
            anchors { fill: parent; margins: Theme.spacing * 1.5 }
            spacing: Theme.spacing * 0.6

            Rectangle {
                width: kindText.implicitWidth + Theme.spacing
                height: kindText.implicitHeight + Theme.spacing * 0.4
                radius: height / 2
                color: playable ? Theme.accent : Theme.background
                Text {
                    id: kindText
                    anchors.centerIn: parent
                    text: kindLabel
                    color: playable ? Theme.background : Theme.textDim
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }
            }
            Text {
                width: parent.width
                text: drive.hasMedia ? (drive.label || "Untitled disc") : "Insert a disc"
                color: drive.hasMedia ? Theme.text : Theme.textDim
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: drive.model + "  ·  " + drive.device
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }
    }
}
