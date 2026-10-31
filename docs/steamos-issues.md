# SteamOS issue drafts

Drafts for https://github.com/ValveSoftware/SteamOS/issues (Valve takes
issues there, not pull requests). Not filed: they go out under the user's
GitHub account. Searched 2026-10-31: no existing issue about `sg`, MakeMKV
or optical drives; #989 covers HDMI passthrough.

## New issue: load the `sg` module

**Title:** Load the `sg` kernel module so optical drives work with Blu-ray software (MakeMKV)

#### Your system information

* SteamOS version: 3.10 (build 20261026.1000), kernel 7.2.7-valve2-2-neptune
* Opted into Steam client beta?: No
* Opted into SteamOS beta?: No
* Hardware: desktop PC running SteamOS, Sony Optiarc BC-5500S Blu-ray drive over USB

#### Please describe your issue in as much detail as possible:

SteamOS ships the SCSI generic driver (`/lib/modules/.../kernel/drivers/scsi/sg.ko.zst`)
but never loads it, so there are no `/dev/sg*` nodes. Blu-ray software that
talks to the drive through SCSI generic can't see any drive: MakeMKV (Flathub
`com.makemkv.MakeMKV`) reports "can't find any usable optical drives", and
media players that decrypt Blu-rays through MakeMKV's libmmbd (libbluray
loads it) can't open a disc. DVDs and plain data discs are unaffected,
since they go through `/dev/sr*`.

Loading the module is all it takes: udev's existing `uaccess` rule then
gives the session user access to the drive's `/dev/sg*`, and MakeMKV works
from Flatpak with no further changes. Most desktop distributions (Arch,
Fedora, Ubuntu) load `sg` by default.

The workaround needs a terminal and root, which the Steam Machine's
living-room users won't have:

    sudo modprobe sg
    echo sg | sudo tee /etc/modules-load.d/sg.conf

Please add `sg` to `/usr/lib/modules-load.d/`, or load it when an optical
drive appears.

#### Steps for reproducing this issue:

1. Connect a Blu-ray drive and insert a Blu-ray.
2. Install MakeMKV from Discover and open it.
3. It reports no usable optical drives; `ls /dev/sg*` finds nothing.
4. Run `sudo modprobe sg` and unplug and replug the drive: MakeMKV now sees the drive and reads the disc.

## Comment on #989 (passthrough over HDMI)

Title of #989: "Passthrough surround sound audio formats over HDMI".

> A data point for whoever picks this up: passthrough itself works on
> SteamOS 3.10 through PipeWire. On a desktop running SteamOS and connected
> to a Pioneer VSX-1130 receiver, mpv (with FFmpeg's `spdif` muxer)
> bitstreams Dolby Digital, TrueHD and DTS-HD MA, and the receiver shows
> each format. Two things get in the way, and both are SteamOS defaults
> rather than missing features:
>
> 1. **speech-dispatcher's dummy output stays linked to the HDMI sink.**
>    A passthrough stream needs the sink to itself, so WirePlumber refuses
>    it ("prepare-link.lua: target is already linked, cannot link
>    exclusively") and the player's audio stalls. That matches the freeze
>    in the original report. The stream is `speech-dispatcher-dummy`
>    (`application.process.binary = "sd_dummy"`): it never makes a sound,
>    yet it permanently holds the sink. Moving it to another sink
>    (`pactl move-sink-input`) makes passthrough work at once. As a
>    workaround we ship a `pipewire-pulse.conf.d` rule that leaves
>    `sd_dummy` unconnected (`node.autoconnect = false`), but that only
>    takes effect after PipeWire restarts. Flatpak apps get WirePlumber's
>    restricted access, so they can't move the stream themselves.
>    Not linking the dummy output by default (or linking it only while
>    the screen reader is on) would fix this for every app.
> 2. **Steam's own UI sounds hold the sink for about 18 s** after each
>    sound (`steamwebhelper`), so a passthrough stream started right after
>    navigating Steam has no target. Apps can retry, but releasing the sink
>    sooner would help.
