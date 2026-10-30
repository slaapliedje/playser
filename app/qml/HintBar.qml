import QtQuick
import Playser

// Controller hints along the bottom of a screen:
//   HintBar { hints: [{ buttons: ["south"], text: "Play" }, ...] }
Row {
    id: root

    property var hints: []
    property real textSize: Theme.fontSmall

    spacing: Theme.spacing * 1.5

    Repeater {
        model: root.hints
        Row {
            required property var modelData
            spacing: Theme.spacing * 0.35
            Repeater {
                model: modelData.buttons
                ButtonGlyph {
                    required property string modelData
                    anchors.verticalCenter: parent.verticalCenter
                    button: modelData
                    size: root.textSize * 1.35
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.text
                color: Theme.textDim
                font.pixelSize: root.textSize
            }
        }
    }
}
