import QtQuick
import DiscPlayer

FocusScope {
    id: root

    required property DiscMonitor monitor
    signal playRequested(var drive)
    signal setupRequested()

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Menu) {
            root.setupRequested()
            event.accepted = true
        }
    }

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

    HintBar {
        id: hints
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        hints: [
            { buttons: ["south"], text: "Play" },
            { buttons: ["dpadH"], text: "Choose drive" },
            { buttons: ["start"], text: "Setup" },
        ]
    }

    component DriveCard: Rectangle {
        property var drive
        property bool focused: false

        readonly property bool playable: !!drive.url
        readonly property var disc: drive.disc || ({})
        readonly property bool known: !!disc.title
        readonly property string kindLabel: disc.format === "UHD" ? "4K UHD" : ({
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

        // the disc's own art (Blu-ray metadata), darkened behind the text;
        // a Canvas because the software renderer can't clip to round corners
        Canvas {
            id: art
            readonly property string source: drive.hasMedia && disc.art ? "file://" + encodeURI(disc.art) : ""
            anchors { fill: parent; margins: parent.border.width }
            visible: source !== ""
            onSourceChanged: if (source) loadImage(source); else requestPaint()
            onImageLoaded: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (!source || !isImageLoaded(source))
                    return
                const r = Theme.radius - parent.border.width
                ctx.beginPath()
                ctx.roundedRect(0, 0, width, height, r, r)
                ctx.clip()
                ctx.drawImage(source, 0, 0, width, height)
                const shade = ctx.createLinearGradient(0, 0, width, 0)
                shade.addColorStop(0, Qt.rgba(0.04, 0.06, 0.09, 0.92))
                shade.addColorStop(0.6, Qt.rgba(0.04, 0.06, 0.09, 0.75))
                shade.addColorStop(1, Qt.rgba(0.04, 0.06, 0.09, 0.35))
                ctx.fillStyle = shade
                ctx.fillRect(0, 0, width, height)
            }
            onWidthChanged: requestPaint()
        }

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
                text: !drive.hasMedia ? "Insert a disc"
                      : known ? disc.title
                      : (Theme.titleFromLabel(drive.label) || "Untitled disc")
                color: drive.hasMedia ? Theme.text : Theme.textDim
                font.pixelSize: Theme.fontLarge
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: drive.hasMedia && (known || drive.identifying)
                // the release name usually carries its year; add the film's if not
                text: drive.identifying ? "Identifying…"
                      : (disc.year && !String(disc.release).includes(disc.year) ? disc.year + "  ·  " : "")
                        + (disc.release || "")
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
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
