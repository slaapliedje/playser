import QtQuick
import Playser

// One controller button, drawn for the pad in use (Theme.buttonStyle) rather
// than taken from a font. button names SDL's positions, so "south" is A on
// Xbox, ✕ on PlayStation and B on Nintendo: what the app maps to confirm.
//   south east west north   face buttons
//   lb rb                   shoulders
//   start                   Menu / Options / +
//   dpad dpadH dpadV        d-pad, all arms or left/right or up/down
Item {
    id: root

    property string button: "south"
    property real size: Theme.fontSmall * 1.35

    readonly property string style: Theme.buttonStyle
    readonly property bool face: ["south", "east", "west", "north"].includes(button)
    readonly property bool shoulder: button === "lb" || button === "rb"
    readonly property bool dpad: button.startsWith("dpad")

    implicitHeight: size
    implicitWidth: shoulder || button === "start" ? size * 1.6 : size

    readonly property var letters: ({
        xbox: { south: "A", east: "B", west: "X", north: "Y", lb: "LB", rb: "RB" },
        nintendo: { south: "B", east: "A", west: "Y", north: "X", lb: "L", rb: "R" },
        playstation: { lb: "L1", rb: "R1" },
    })
    readonly property var colors: ({
        xbox: { south: "#5dbb46", east: "#e04a3f", west: "#3c8de0", north: "#f2c230" },
        playstation: { south: "#8fb4ec", east: "#ec6f7c", west: "#dc93d8", north: "#45c4a8" },
    })
    readonly property color ink: (colors[style] || {})[button] || Theme.text

    // face buttons and shoulders: a filled shape with a label
    Rectangle {
        anchors.fill: parent
        visible: !root.dpad
        radius: root.face ? width / 2 : height * 0.3
        color: Theme.surfaceRaised
        border.color: Qt.darker(Theme.textDim, 1.3)
        border.width: Math.max(1, root.size * 0.06)
    }
    Text {
        anchors.centerIn: parent
        visible: !root.dpad && root.button !== "start" && !(root.style === "playstation" && root.face)
        text: (root.letters[root.style] || {})[root.button] || ""
        color: root.ink
        font.pixelSize: root.size * (root.shoulder ? 0.5 : 0.62)
        font.weight: Font.Bold
    }

    // Menu / Options / +: three lines
    Column {
        anchors.centerIn: parent
        visible: root.button === "start"
        spacing: root.size * 0.1
        Repeater {
            model: 3
            Rectangle {
                width: root.size * 0.6
                height: Math.max(1, root.size * 0.09)
                radius: height / 2
                color: Theme.text
            }
        }
    }

    // PlayStation symbols and the d-pad are drawn
    Canvas {
        id: canvas
        anchors.fill: parent
        visible: root.dpad || (root.style === "playstation" && root.face)
        onPaint: {
            const ctx = getContext("2d")
            const s = width
            ctx.reset()
            ctx.lineWidth = s * 0.09
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            if (root.dpad) {
                // a plus; the arms in use lit, the others dim
                const arm = s * 0.34, half = s * 0.17
                const lit = { up: root.button !== "dpadH", down: root.button !== "dpadH",
                              left: root.button !== "dpadV", right: root.button !== "dpadV" }
                const arms = {
                    up: [s / 2 - half, 0, half * 2, arm], down: [s / 2 - half, s - arm, half * 2, arm],
                    left: [0, s / 2 - half, arm, half * 2], right: [s - arm, s / 2 - half, arm, half * 2],
                }
                const dim = Qt.darker(Theme.textDim, 1.9)
                ctx.fillStyle = Theme.text
                ctx.fillRect(s / 2 - half, s / 2 - half, half * 2, half * 2)
                for (const k in arms) {
                    ctx.fillStyle = lit[k] ? Theme.text : dim
                    ctx.fillRect(arms[k][0], arms[k][1], arms[k][2], arms[k][3])
                }
                return
            }
            ctx.strokeStyle = root.ink
            const c = s / 2, r = s * 0.22
            ctx.beginPath()
            if (root.button === "south") {
                ctx.moveTo(c - r, c - r); ctx.lineTo(c + r, c + r)
                ctx.moveTo(c + r, c - r); ctx.lineTo(c - r, c + r)
            } else if (root.button === "east") {
                ctx.arc(c, c, r, 0, 2 * Math.PI)
            } else if (root.button === "west") {
                ctx.rect(c - r * 0.9, c - r * 0.9, r * 1.8, r * 1.8)
            } else {
                ctx.moveTo(c, c - r * 1.05)
                ctx.lineTo(c + r * 1.1, c + r * 0.8)
                ctx.lineTo(c - r * 1.1, c + r * 0.8)
                ctx.closePath()
            }
            ctx.stroke()
        }
        Connections {
            target: root
            function onStyleChanged() { canvas.requestPaint() }
            function onButtonChanged() { canvas.requestPaint() }
        }
    }
}
