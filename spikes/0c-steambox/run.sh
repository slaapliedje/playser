#!/bin/bash
# Spike 0c launcher: add this once to Steam as a non-Steam game, then switch
# test cases remotely with:  echo <case> > ~/discnav-test/case
# Each run writes ~/discnav-test/logs/<time>-<case>.{log,env}.
dir="$HOME/discnav-test"
case=$(cat "$dir/case" 2>/dev/null || echo hdr-pcm)
mkdir -p "$dir/logs"
stamp="$dir/logs/$(date +%H%M%S)-$case"
env | sort >"$stamp.env"

# common: fullscreen, Vulkan gpu-next, quit at the end of the 60 s clip
opts=(--no-config --fs --vo=gpu-next --gpu-api=vulkan --hwdec=vaapi
      --keep-open=no --osd-level=1 --log-file="$stamp.log" --msg-level=all=v)

case "$case" in
hdr-pcm)        # HDR10 out, TrueHD decoded to PCM by mpv
    opts+=(--target-colorspace-hint=yes --ao=pipewire) ;;
sdr-tonemap)    # control: no HDR hint, mpv tone-maps to SDR
    opts+=(--target-colorspace-hint=no --ao=pipewire) ;;
hdr-overlay)    # spike 0d: SDR UI bitmap composited by mpv over HDR video
    opts+=(--target-colorspace-hint=yes --ao=pipewire --script="$dir/overlay.lua"
           --script-opts=overlay-file="$dir/overlay.bgra") ;;
hdr-overlay-pq) # same, bitmap pre-encoded as BT.2020 PQ (mpv blends in the video's space)
    opts+=(--target-colorspace-hint=yes --ao=pipewire --script="$dir/overlay.lua"
           --script-opts=overlay-file="$dir/overlay-pq.bgra") ;;
app-hdr)        # the app itself (Flatpak) playing the HDR clip; extra args
                # (e.g. --script ...) from ~/discnav-test/app-args
    flatpak run --filesystem="$dir" \
        --env=PLAYSER_MPV_LOG="$stamp.log" --env=QT_FORCE_STDERR_LOGGING=1 \
        io.github.slaapliedje.Playser --fullscreen --play "$dir/motu-uhd-60s.mkv" \
        $(cat "$dir/app-args" 2>/dev/null) >"$stamp.out" 2>&1
    echo "exit $?" >>"$stamp.out"
    exit ;;
app)            # the app (Flatpak) at its home screen, for disc tests
    # test-only: MakeMKV copied from the desktop into $dir/makemkv (the real
    # app will fetch it via extra-data); makemkvcon keeps state in ~/.MakeMKV
    mk="$dir/makemkv"
    [ -d "$mk" ] && set -- --share=network --filesystem="$mk":ro --filesystem=~/.MakeMKV:create \
        --env=LD_LIBRARY_PATH=/app/lib:"$mk/lib" --env=MAKEMKVCON="$mk/bin/makemkvcon" \
        --env=LIBAACS_PATH="$mk/lib/libmmbd" --env=LIBBDPLUS_PATH="$mk/lib/libmmbd"
    flatpak run "$@" $(cat "$dir/flatpak-args" 2>/dev/null) --filesystem="$dir/logs" --env=PLAYSER_MPV_LOG="$stamp.log" --env=QT_FORCE_STDERR_LOGGING=1 \
        io.github.slaapliedje.Playser --fullscreen $(cat "$dir/app-args" 2>/dev/null) >"$stamp.out" 2>&1
    echo "exit $?" >>"$stamp.out"
    exit ;;
pass-pipewire)  # bitstream TrueHD/E-AC-3/AC-3 through PipeWire
    opts+=(--target-colorspace-hint=yes --ao=pipewire --audio-spdif=ac3,eac3,truehd) ;;
pass-alsa)      # bitstream straight to the HDMI ALSA device, bypassing PipeWire
    opts+=(--target-colorspace-hint=yes --ao=alsa --audio-device="$(cat "$dir/alsa-device" 2>/dev/null)"
           --audio-spdif=ac3,eac3,truehd) ;;
*)
    echo "unknown case: $case" >"$stamp.log"; exit 1 ;;
esac

# gamescope's Vulkan WSI layer (needed for HDR from an X11 client) talks to
# gamescope over its private socket; Flatpak only shares wayland-0 by default
flatpak run --filesystem="$dir" --filesystem=xdg-run/gamescope-0 \
    io.mpv.Mpv "${opts[@]}" "$dir/motu-uhd-60s.mkv" >"$stamp.out" 2>&1
echo "exit $?" >>"$stamp.out"
