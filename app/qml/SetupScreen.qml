import QtQuick
import Playser

// What Blu-ray playback needs, with fixes for anything missing. Shown on
// first run and whenever something isn't ready; Menu opens it from home.
// DVDs need none of this.
FocusScope {
    id: root

    required property SystemCheck check
    property bool passthrough: false
    signal done()
    signal passthroughToggled()

    visible: false

    function open() {
        check.refresh()
        check.checkUhdDrives()
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

    // one sentence per drive about 4K UHD discs
    function uhdText() {
        const drives = Object.values(check.uhdDrives)
        if (drives.length === 0)
            return "No disc drive connected."
        return drives.map(d =>
            d.state === "yes" ? "The " + d.name + " can play them (LibreDrive " + d.version + ")."
            : d.state === "no" ? "The " + d.name + " can't: 4K discs need a drive MakeMKV can use in "
                                 + "LibreDrive mode (the MakeMKV forum lists them). Blu-rays and DVDs still play."
            : d.state === "checking" ? "Checking the " + d.name + "…"
            : "Put a Blu-ray or 4K disc in the " + d.name + " to check it.").join(" ")
    }
    function uhdOk() {
        const states = Object.values(check.uhdDrives).map(d => d.state)
        return states.length === 0 ? null : states.includes("yes") ? true : states.includes("no") ? false : null
    }

    readonly property var items: [
        {
            ok: check.makemkv !== "",
            title: "MakeMKV",
            text: check.makemkv === "flatpak" ? "Using the installed MakeMKV app. It updates on its own."
                : check.makemkv === "bundled" ? "Using the copy built into Playser. Tip: install MakeMKV from "
                                                + "Discover in Desktop Mode and Playser will use it, so it stays up to date."
                : check.makemkv === "system" ? "Using the MakeMKV installed on this system."
                : "Not found. Install MakeMKV from Discover in Desktop Mode."
        },
        {
            ok: check.hasKey,
            title: "MakeMKV registration key",
            text: check.hasKey ? "Registered."
                : "Needed for Blu-ray. In Desktop Mode, " + (check.makemkv === "flatpak" ? "" : "install MakeMKV from Discover, ")
                  + "open MakeMKV, choose Help → Register and enter your key (buy one at makemkv.com). "
                  + "Playser uses it from there."
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
        {
            ok: root.uhdOk(),
            title: "4K UHD discs (optional)",
            text: root.uhdText()
        },
        {
            ok: !root.passthrough ? null : !check.passthroughRule ? false
                : check.passthroughRulePending ? null : true,
            title: "Audio passthrough (optional)",
            text: !root.passthrough
                ? "Off: audio is decoded and sent as PCM (up to 7.1), which works with any TV or receiver. "
                  + "Turn on to send Dolby and DTS tracks to your receiver as they are, for Atmos and DTS:X."
                : !check.passthroughRule
                ? "On, but Playser couldn't change the audio setting it needs, so audio will be decoded instead."
                : check.passthroughRulePending
                ? "On. Restart once to finish: SteamOS keeps a silent stream on the audio output, "
                  + "and the change that moves it applies after a restart."
                : "On: Dolby and DTS go to your receiver as they are. If it can't take one, that track is decoded instead."
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
                    color: modelData.ok === null ? Theme.textDim : modelData.ok ? Theme.accent : Theme.warning
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

    // Check again | Passthrough | Done, left/right to choose
    ListView {
        id: buttons
        anchors { left: parent.left; bottom: parent.bottom; margins: Theme.margin }
        width: parent.width - 2 * Theme.margin
        height: 72 * Theme.scale
        orientation: ListView.Horizontal
        spacing: Theme.spacing
        model: ["Check again", "passthrough", "Done"]
        currentIndex: 2
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
                text: modelData !== "passthrough" ? modelData
                    : root.passthrough ? "Passthrough: On" : "Passthrough: Off"
                color: Theme.text
                font.pixelSize: Theme.fontMedium
            }
        }
        Keys.onReturnPressed: {
            if (currentIndex === 0) {
                root.check.refresh()
                root.check.checkUhdDrives()
            }
            else if (currentIndex === 1)
                root.passthroughToggled()
            else
                root.close()
        }
    }
}
