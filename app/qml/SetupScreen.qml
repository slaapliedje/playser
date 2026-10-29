import QtQuick
import DiscPlayer

// What Blu-ray playback needs, with fixes for anything missing. Shown on
// first run and whenever something isn't ready; Menu opens it from home.
// DVDs need none of this.
FocusScope {
    id: root

    required property SystemCheck check
    signal done()

    visible: false

    function open() {
        check.refresh()
        visible = true
        buttons.forceActiveFocus()
    }

    function close() {
        focus = false
        visible = false
        done()
    }

    Keys.onEscapePressed: close()
    Keys.onBackPressed: close()

    readonly property var items: [
        {
            ok: check.makemkv !== "",
            title: "MakeMKV",
            text: check.makemkv === "flatpak" ? "Using the installed MakeMKV app. It updates on its own."
                : check.makemkv === "bundled" ? "Using the copy built into Disc Player. Tip: install MakeMKV from "
                                                + "Discover in Desktop Mode and Disc Player will use it, so it stays up to date."
                : check.makemkv === "system" ? "Using the MakeMKV installed on this system."
                : "Not found. Install MakeMKV from Discover in Desktop Mode."
        },
        {
            ok: check.hasKey,
            title: "MakeMKV registration key",
            text: check.hasKey ? "Registered."
                : "Needed for Blu-ray. In Desktop Mode, " + (check.makemkv === "flatpak" ? "" : "install MakeMKV from Discover, ")
                  + "open MakeMKV, choose Help → Register and enter your key (buy one at makemkv.com). "
                  + "Disc Player uses it from there."
        },
        {
            ok: check.opticalDrives === 0 ? null : check.scsiGeneric,
            title: "Drive access",
            text: check.opticalDrives === 0 ? "No disc drive connected."
                : check.scsiGeneric ? "MakeMKV can reach " + (check.opticalDrives === 1 ? "the drive." : "the drives.")
                : "SteamOS doesn't load the Linux \"sg\" driver MakeMKV needs. Once, in Desktop Mode, open "
                  + "Konsole and run:\n    sudo modprobe sg\n    echo sg | sudo tee /etc/modules-load.d/sg.conf\n"
                  + "then unplug the drive and plug it back in."
        },
    ]

    Rectangle {
        anchors.fill: parent
        color: Theme.background
    }

    Column {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.margin }
        spacing: Theme.spacing * 1.2

        Text {
            text: "Setup"
            color: Theme.text
            font.pixelSize: Theme.fontHuge
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            text: "Blu-ray and 4K UHD discs need MakeMKV. DVDs play without it."
            color: Theme.textDim
            font.pixelSize: Theme.fontMedium
            wrapMode: Text.Wrap
        }

        Repeater {
            model: root.items
            delegate: Row {
                required property var modelData
                width: parent.width
                spacing: Theme.spacing

                Text {
                    text: modelData.ok === null ? "–" : modelData.ok ? "✓" : "✗"
                    color: modelData.ok === null ? Theme.textDim : modelData.ok ? Theme.accent : "#ff6b6b"
                    font.pixelSize: Theme.fontLarge
                    font.weight: Font.Bold
                    width: Theme.fontLarge
                }
                Column {
                    width: parent.width - Theme.fontLarge - Theme.spacing
                    spacing: Theme.spacing * 0.2
                    Text {
                        text: modelData.title
                        color: Theme.text
                        font.pixelSize: Theme.fontMedium
                        font.weight: Font.DemiBold
                    }
                    Text {
                        width: parent.width
                        text: modelData.text
                        color: Theme.textDim
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        lineHeight: 1.15
                    }
                }
            }
        }
    }

    // Check again | Done, left/right to choose
    ListView {
        id: buttons
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        width: parent.width - 2 * Theme.margin
        height: 72 * Theme.scale
        orientation: ListView.Horizontal
        spacing: Theme.spacing
        model: ["Check again", "Done"]
        currentIndex: 1
        delegate: Rectangle {
            required property string modelData
            required property int index
            readonly property bool current: ListView.isCurrentItem && buttons.activeFocus
            width: label.implicitWidth + Theme.spacing * 2
            height: buttons.height
            radius: Theme.radius / 2
            color: current ? Theme.surfaceRaised : Theme.surface
            border.color: current ? Theme.focusRing : "transparent"
            border.width: 3 * Theme.scale
            Text {
                id: label
                anchors.centerIn: parent
                text: modelData
                color: Theme.text
                font.pixelSize: Theme.fontMedium
            }
        }
        Keys.onReturnPressed: currentIndex === 0 ? root.check.refresh() : root.close()
    }
}
