# Phase 0 spike results

## 0a — UHD playback via libbluray + libmmbd (2026-10-28, Garuda desktop)

**Result: works.** No code needed beyond stock mpv.

Setup:
- Drive: ASUS BW-16D1HT, firmware 3.10, LibreDrive v06.3 active (per MakeMKV).
- Disc: *Masters of the Universe* UHD (`index.bdmv` = `INDX0300`, AACS 2.0,
  85 GB of streams).
- MakeMKV 2.0.0 (Arch `makemkv` package), `/usr/lib/libmmbd.so.0`; no system
  libaacs/libbdplus installed.
- mpv 0.41.0, libbluray 1.5.1.

Command:
```
LIBAACS_PATH=libmmbd LIBBDPLUS_PATH=libmmbd \
  mpv --no-config --vo=null --ao=null --hwdec=no \
      --bluray-device=/dev/sr0 --length=15 bd://
```

Observed:
- libbluray loaded libmmbd through the env vars; decryption transparent.
- 55 playlists (heavy obfuscation); mpv's default "longest title" pick chose
  the real feature, `01199.mpls` (2:20:30). Don't rely on that in general —
  we still need our own main-feature heuristic.
- Video: HEVC 3840x2160 23.976, `yuv420p10 bt.2020/pq` (HDR10), plus a second
  HEVC 1920x1080 stream — the Dolby Vision enhancement layer (dual-layer
  profile 7). Base layer + HDR10 is what we get today.
- Audio: TrueHD 8ch (likely Atmos) default, plus AC3/DTS tracks.
- 15 s decoded with zero decode errors; clean EOF.
- PGS subtitle streams report "unspecified size" during probe — normal until
  first subtitle packet.

Not yet checked: on-screen HDR output, hardware decode (vaapi), passthrough
(→ 0c on the SteamOS box), and behaviour inside the Flatpak sandbox.

Product implications:
- `LIBAACS_PATH` / `LIBBDPLUS_PATH` is the integration mechanism; the Flatpak
  sets these and points them at our bundled libmmbd.
- Dolby Vision profile 7 FEL is a later question; HDR10 base layer is fine
  for v1.

## 0b — DVD menus via libdvdnav → libmpv stream_cb (2026-10-28, Garuda)

**Result: the approach works.** On the synthetic fixture
(`tools/make-test-dvd.sh`): still menu without audio, looping motion menu
with audio, button navigation, activation, root/title menu calls, seamless
multi-chapter title, and title-end return to menu all behave correctly.
Overlay rectangles line up exactly with the buttons.

Background: mpv still ships a `dvdnav://` protocol, but it only plays titles
(exposed as editions) and skips menus entirely — confirmed on the fixture.

Run it: `spikes/0b-dvdmenu/dvdmenu [--script STEPS] [--mpv-log F]
[--shot-dir D] <device|iso|VIDEO_TS>`. `--script` drives it headlessly,
e.g. `"2,shot,down,enter,3,shot,quit"`; shots are framebuffer captures that
include our overlay.

Measured latencies (button press → new segment loaded):

| Action | Latency |
|---|---|
| Still menu → title | ~115 ms |
| Root menu from inside a title | ~145 ms |
| Motion menu → title | ~140 ms |
| Title menu from a title | ~175 ms |
| Motion-menu loop (PTS discontinuity) | ~50 ms reload gap |

Lessons — these are design rules for `libdiscnav` and the app:
1. **Protocol name:** mpv already owns `dvdnav://`; ours is `discnav://`.
2. **Never call blocking libmpv APIs from the render thread.** Both
   `mpv_get_property` and a synchronous `screenshot-to-file` deadlocked
   (core waits on render thread, render thread waits on core). Use
   `mpv_observe_property` + async commands.
3. **Set `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME = 0`.** Otherwise a timestamp
   jump makes the render call sleep for seconds and freezes the UI.
4. **Segment model:** one dvdnav handle outlives many mpv "files". A segment
   ends on HOP_CHANNEL, VTS_CHANGE, or a VOBU PTM discontinuity
   (`vobu_s_ptm != previous vobu_e_ptm`); a looping menu's `jump cell 1`
   emits *no* HOP, so the PTM check is required. Seamless cell changes
   (chapters) must not end a segment.
5. **User-initiated jumps flush;** natural ones play out. Without the flush,
   "root menu" mid-title waited ~1.4 s for buffered video to drain.
6. **Reload bookkeeping:** a flush already queues the next segment, so the
   old segment's END_FILE must not trigger a second load (it would cancel
   the new segment and drop its first blocks, e.g. a still menu's picture).
7. **Still frames:** inject a `sequence_end_code` PES so libavcodec outputs
   the held picture, then block in `read_fn` until timeout/user input.
   Menus without audio need no special handling. (See the Airheads
   section for what a *one-picture* still additionally needs.)
8. **Track selection follows the VM**, not mpv: see Airheads section.

### Commercial disc: *Airheads* (Fox, 16:9, CSS) via the Optiarc USB drive

Works end to end: CSS via libdvdcss, two 7 s timed-still warning screens,
Fox logo, trailer, Menu during trailer → root menu (via an intro clip),
16:9 anamorphic main menu with correctly aligned highlights, Play → logo
→ film with AC3 4ch audio, Menu from the film back to the main menu.

The synthetic disc hid four problems, all from real menus/stills being a
**single picture** and from MPEG-PS having **no header**:

9. **lavf probing never ends on a one-picture still.** MPEG-PS is
   `AVFMTCTX_NOHEADER`, so `avformat_find_stream_info` doesn't stop when
   everything is known; it stops only at `probesize` (counted in *packets*
   read), `analyzeduration` (packet timestamps), or EOF. Padding is not a
   packet, so it read until our padding cap and the warning screen appeared
   only when its still timed out. `fpsprobesize`/`max_ts_probe` and a PSM
   don't help (verified offline with ffprobe on a dumped segment,
   `DISCNAV_DUMP=file`). Fix: pass NAV packs through to mpv (lavf turns
   them into a harmless dvd_nav data stream), and during a still, *before*
   mpv has opened the segment, repeat the last NAV pack as filler — those
   count toward probesize.
10. **probesize must be ≥ ~1 MB** so the first audio packet is seen before
    probing ends (256 KB lost the audio track once NAV packs counted).
11. **mpv holds the first frame until a second arrives** (to know its
    duration) → a one-picture still never shows. `video-latency-hacks=yes`
    fixes it. Also `vd-lavc-threads=1` (frame threading adds delay that
    nothing drains during a still; SD MPEG-2 doesn't need threads).
12. **Streams appear after playback starts** (no header), so mpv never
    auto-selects them, and `sid=1` hit a closed-caption track. Track
    selection now follows the VM: `DVDNAV_AUDIO_STREAM_CHANGE` /
    `SPU_STREAM_CHANGE` give physical stream numbers → PS stream ids (AC3
    0x80+n, DTS 0x88+n, LPCM 0xA0+n, MPEG 0x1C0+n, SPU 0x20+n) = mpv
    `src-id`; the main thread observes `track-list` and selects the match
    whenever it appears. SPU "off" (bit 7) → `sub-forced-events-only` so
    forced subtitles and menu graphics still show.

### Real button graphics — first `libdiscnav` code (`libdiscnav/spu.{c,h}`)

In menu domains we decode the active subpicture stream ourselves and
composite it as an RGBA overlay (the libbluray model); mpv's `sid` is turned
off there. Titles still use mpv for subtitles.

- `spu_asm_feed` reassembles SPU units from private-stream-1 PES payloads;
  `spu_decode` parses the control sequence (SET_COLOR/CONTR/DAREA/DSPXA,
  forced flag) and the interlaced 2-bit RLE.
- `spu_composite` maps pixel values through the disc's CLUT
  (`DVDNAV_SPU_CLUT_CHANGE`, YCrCb → RGB) and, inside the selected button's
  rectangle (`dvdnav_get_highlight_area`), swaps in the PCI highlight
  colour/contrast nibbles.
- Verified: synthetic disc (yellow outline moves with selection, still and
  motion menus) and *Airheads* (the authored red lightning-bolt cursor moves
  PLAY → LANGUAGE SELECTION → SCENE SELECTION → EXTRA FEATURES). The old
  rectangles are now a debug toggle (`D`).
- Fixture bug found on the way: ffmpeg `drawbox` on a transparent canvas
  leaves alpha 0, so spumux saw invisible buttons and dvdauthor wrote an
  all-transparent highlight palette. The script now keys out opaque black.

Open items (next steps for 0b / Phase 2):
- Activated-button ("action") colours aren't shown; only the select state.
- SPU timing: menu SPUs are shown as soon as they're decoded (ahead of the
  picture by the readahead); STP_DSP and SPU PTS are ignored.
- Title subtitles still go through mpv, which has no CLUT → likely wrong
  colours. Either render them with `spu.c` too (needs PTS sync to mpv's
  playback time) or pass the CLUT to mpv's decoder.
- Nav state runs ahead of presentation by the demuxer readahead; tie
  highlight updates to PTS.
- Untested: commercial discs (CSS, multi-angle, timed stills, odd PGC
  programming), widescreen/letterbox button coordinates, PAL.
- `DVDNAV_WAIT` is skipped immediately; should wait for mpv to drain.
- One flush took ~1 s to be handled on the main thread (Airheads run 6,
  not reproduced since); instrument the render loop to find the stall.
- Bare `[e] mpeg2video: Warning MVs not available` / "No video PTS" at some
  segment starts (segment begins on a non-I-frame or PTS-less packet).

## 0c — SteamOS box: HDR, hwdec, audio (2026-10-28)

Box: SteamOS 3.10, RX 7900 XT, gamescope 3.17, HDMI → LG TV (4K60, HDR
on in Steam) → Pioneer VSX-1130 over ARC. Player: Flathub `io.mpv.Mpv`
0.41 (Flatpak, runtime 26.08) launched as a non-Steam game via
`spikes/0c-steambox/run.sh` (switchable test cases, logs per run).
Clip: 60 s of the *Masters of the Universe* UHD (HEVC 10-bit HDR10 PQ +
TrueHD 8ch), cut with `ffmpeg -playlist 1199 -ss 1200 -i bluray:/dev/sr0
-c copy` (mpv `--stream-record` dropped almost all TrueHD packets).

**Works:**
- **HDR10 from a Flatpak under gamescope.** mpv `vo=gpu-next`,
  `gpu-api=vulkan`, `target-colorspace-hint=yes` picks
  `A2B10G10R10 + VK_COLOR_SPACE_HDR10_ST2084_EXT` at 3840x2160. gamescope
  keeps the output in HDR the whole session, so the TV shows no badge
  change when playback starts.
- **VAAPI hardware decode** of 4K HEVC Main10 (`vaapi[p010]`), no drops.
- **Decoded audio**: TrueHD → 7.1 PCM → PipeWire → TV (stereo downmix to
  the receiver over ARC). Flawless per user.

**Flatpak requirements found (our manifest must have these):**
- Runtime extension `org.freedesktop.Platform.VulkanLayer.gamescope`
  matching the runtime branch — without it gamescope can't do HDR for an
  X11 Vulkan client.
- `--filesystem=xdg-run/gamescope-0`: the WSI layer talks to gamescope on
  that socket. Without it: "CreateSwapchainKHR: Creating swapchain for
  non-Gamescope swapchain. Hooking has failed somewhere!"
- Steam's default Game Resolution for non-Steam shortcuts was 1080p; set
  per-shortcut to Native (the app should document/detect this).

**Fails: bitstream passthrough (TrueHD) through PipeWire.**
mpv `--audio-spdif=truehd --ao=pipewire` → stream goes `paused → error:
no target node available`; mpv then stalls and the **picture freezes**.
Ruled out, in order:
1. Sink profile: was `hdmi-stereo`; switched to `hdmi-surround71-extra3`.
2. Sink capability: node advertises IEC958 PCM/AC3/EAC3/TrueHD.
3. Clock rates: `clock.allowed-rates` was `[48000]`; widened at runtime to
   include 192000.
4. Competing stream: SteamOS keeps `speech-dispatcher-dummy` linked to the
   HDMI sink; moved it off. Still no target.
Direct ALSA (`--ao=alsa`, `hdmi:CARD=HDMI,DEV=3`) fails with "Device or
resource busy" — PipeWire holds the device; grabbing it would take audio
away from Steam, so not a product option.

Product implications:
- **Audio-output failure must not freeze playback**: detect AO errors and
  fall back to decoding (PCM).
- Passthrough is not a given on SteamOS; ship decoded multichannel PCM as
  the default, passthrough as an opt-in once it works.
- In this home setup lossless passthrough is moot anyway: ARC to the
  VSX-1130 carries at most DD/DD+ (receiver showed stereo PCM).

Next for passthrough: try AC-3 (48 kHz, 2ch IEC958) to separate "any
encoded stream" from "HBR TrueHD"; run WirePlumber with debug logging
during a failing attempt; compare a host-side (non-Flatpak) client to
rule out Flatpak/portal access rules on encoded nodes.

**Update 2026-10-29: passthrough works, with the dummy stream moved.**
Box now connected straight to the VSX-1130 over HDMI (ELD: 8ch LPCM to
192 kHz, AC-3, E-AC-3, DTS, DTS-HD, TrueHD; EDID passes 4K60 + HDR10/PQ).
- Our Flatpak's FFmpeg had `--disable-muxers`, which drops the `spdif`
  muxer mpv's `spdif_*` "decoders" need: passthrough never even opened an
  AO. Fixed (`--enable-muxer=spdif`).
- Then `no target node available` again, from both `ao=pipewire` and
  `ao=pulse`. WirePlumber debug log (`wpctl set-log-level 5`): the target
  is picked correctly (`hdmi-surround71-extra3`, `can_passthrough:true`),
  then `prepare-link.lua: target is already linked, cannot link
  exclusively`. Passthrough needs the sink to itself, and the only other
  link was `speech-dispatcher-dummy`.
- With `speech-dispatcher-dummy` moved to another sink (`pactl
  move-sink-input`), AC-3 (48 kHz 2ch) and TrueHD (192 kHz 8ch HBR) both
  played to the end; the receiver showed DOLBY DIGITAL and Dolby TrueHD.
  No `clock.allowed-rates` change was needed. (The 0c attempt that also
  moved the dummy stream still lacked the `spdif` muxer.)
- Flatpak clients get WirePlumber's `steamos-restricted` access, so the
  app can't move another client's stream itself. Games never need this
  (they output mixed PCM), so Steam has no passthrough API.
- Watchdog check: before the fix, both clips stalled and recovered to
  decoded PCM after ~8 s (the receiver showed PCM).

Box state after testing (reverted 2026-10-28): `clock.allowed-rates` back
to `[48000]`; `speech-dispatcher-dummy` back on the HDMI sink; HDMI card
profile set to `hdmi-surround-extra3` (5.1, matching the user's speakers)
as the default sink. The test harness stays in `~/discnav-test` on the box
(launcher shortcut "run.sh" in Steam).

## 0d — SDR UI over HDR video via `overlay-add` (2026-10-29, SteamOS box)

Question: if mpv owns an HDR10 Vulkan swapchain, can the app's UI (QML
rendered off-screen) be composited by mpv with `overlay-add`, at the right
brightness? Test: a 3840x400 premultiplied BGRA bar (`spikes/0d-hdr-overlay/`)
over the UHD clip, case `hdr-overlay` / `hdr-overlay-pq` in `run.sh`.
Measured from `gamescopectl screenshot /tmp/x.avif` (10-bit BT.2020 PQ
capture of the real output; a `.png` screenshot is gamescope's SDR
conversion and hides the problem).

**Plain sRGB bitmap: wrong.** White 990 nits (≈5x Steam's UI), grey 119 →
66 nits, amber shifted. Cause (`video/out/vo_gpu_next.c`, `SUBBITMAP_BGRA`):
mpv assumes BGRA overlays are in the **video's** colorspace ("Infer bitmap
colorspace from source"), sensible for PQ-authored PGS subtitles, so sRGB
codes were read as PQ codes and blending happens on PQ values.

**Pre-encoded bitmap: right.** `make-bgra.py --pq` converts sRGB → linear →
BT.2020 → PQ with SDR white at 203 nits (BT.2408), premultiplied on the
encoded values. Measured: white 202 nits, grey 37 nits (expected 37.4),
amber hue intact. Edges clean, placement correct.

Remaining caveat: alpha blends in PQ space, so partial transparency over
dark content comes out darker than in sRGB (25% white over black: ~1 nit
instead of ~10). Fine for scrims and antialiased text; design the OSD with
mostly opaque elements and don't rely on faint translucent fills.

Product implications:
- The app must encode overlays in the video's colorspace: sRGB for SDR
  video, BT.2020 PQ for HDR10/DV (HLG would need its own transfer).
  Re-encode when `video-params` change (menu → feature on a disc).
- SDR white level should follow the system's SDR-in-HDR brightness
  (gamescope setting) rather than a fixed 203, if it can be queried.
- The same path carries `libdiscnav` menu graphics (DVD SPU, BD IG/BD-J
  ARGB), which are SDR BT.709 and need the same conversion over HDR.

## 0e — BD-J menus via libbluray navigation mode (2026-10-30, Garuda)

Question: does libbluray run a modern disc's BD-J menu, and in which
environment? `spikes/0e-bdj/bdj.c`: `bd_open` + `bd_play` (First Play),
`bd_read_ext` paced at ~60 Mbit/s with the data discarded, events logged,
ARGB planes (`bd_register_argb_overlay_proc` with an app-allocated
`BD_ARGB_BUFFER`) saved on flushes, keys through `bd_user_input`
(pressed + typed + released). Disc: *Masters of the Universe* UHD
(AACS 2 via libmmbd, LibreDrive), 88 HDMV + 2 BD-J titles, First Play BD-J.

**Java version matters.**
- Java 27 (the desktop's default): fails. libbluray needs a
  SecurityManager; Java 18–23 still allow its workaround, Java 24+ removed
  it ("Failed initializing SecurityManager", "Can't play BD-J title").
- A headless JRE fails too: libbluray starts the JVM with
  `-Djava.awt.headless=false` (and its own `BDToolkit`), so AWT loads
  `libawt_xawt.so`. A full JRE is needed, though no X server is used.
- Java 21 works: the Flathub extension
  `org.freedesktop.Sdk.Extension.openjdk21//25.08` (our runtime's branch)
  has a full JDK (runs on the host too, `JAVA_HOME=<ext>/jvm/openjdk-21`),
  Ant for building `libbluray-j2se.jar`, and `install.sh`, which copies a
  JRE to `/app/jre`.

**With Java 21 it works end to end.** BD-J starts in ~0.1 s, shows a
loading bar, plays its intro playlist (1163), then the main menu over a
looping background playlist (149): Play / Scene Selections / Audio /
Subtitles / Special Features drawn into the IG plane (1920x1080 ARGB).
Right moves the highlight, Enter on Audio opens the disc's audio list
(nine languages). Harmless noise: the disc's Java logs
`StreamNotAvailableException` while nothing reads the streams.

Product implications:
- The Flatpak bundles the openjdk21 extension's JRE (`/app/jre`) and
  builds libbluray with `-Dbdj_jar=enabled` using its JDK and Ant;
  `JAVA_HOME=/app/jre` at run time. Stay on Java 21 (LTS) until
  libbluray stops needing the SecurityManager.
- Navigation needs `bd_play` + `bd_read_ext` feeding the player (as
  libdiscnav does for DVDs): the menus are playlists with video behind
  the graphics.

## 0f — 3D Blu-ray: H.264 MVC decoding (2026-10-31, Legion Go 2)

Question: can the Go 2 decode both eyes of a 3D Blu-ray in real time, and
read them off the disc fast enough? Disc: *The Hobbit: An Unexpected
Journey* Extended Edition part 1 (3D, BD-J), in the Optiarc on the Go 2's
USB-C port (the dock's ports can't power it: "No medium found"). The 3D
film is playlists 00100/00101, clips 00049 (base view, PID 0x1011, 26.3
GB) and 00050 (dependent view, PID 0x1012, 9.7 GB), interleaved on disc
as `SSIF/00049.ssif` (36.0 GB, 1:39:05, about 48 Mb/s on average).

- **FFmpeg 9.0.2 doesn't decode MVC**: its mpegts demuxer exposes the
  dependent view (stream type 0x20) as plain H.264, and nothing decodes it.
- **edge264-mvc** (https://github.com/jens-duttke/edge264-mvc, commit
  4555276, BSD-3-Clause; a fork of edge264, whose README says edge264
  itself fails on 3D Blu-ray) decodes both views and returns them paired
  in display order. Checked on the desktop with the ITU's MVC conformance
  stream MVCDS1 (290 frames, two distinct camera views side by side).
- `spikes/0f-mvc/mvcdump.c` reads the two clips decrypted through
  libbluray (`bd_open_file_dec`; reads must be whole 6144-byte units)
  and writes one Annex B stream, each base access unit followed by the
  dependent one with the same PTS. Built in the KDE 6.11 SDK, run inside
  the Playser Flatpak with the app's MakeMKV setup (`LIBAACS_PATH`,
  `MAKEMKVCON`, `LD_LIBRARY_PATH` as in `setupDecryption()`).
- **Decoding speed on the Go 2 (Ryzen Z2 Extreme, 16 threads)**, 2 minutes
  from 30 % into the film (2880 pairs of 1080p24 frames, 566 MB):
  4.9 s with all cores (about 24x real time, 73 s of CPU), 35 s on one
  core (about 3.4x real time), 0.6–1.2 GB of memory. Output checked as
  side-by-side 3840x1080: both eyes correct, with parallax.
- **Reading**: the separate clips read at only about 2/3 of real time (the
  drive seeks between them); the interleaved `SSIF` reads at 8.8 MB/s
  (70 Mb/s, `spikes/0f-mvc/ssifrate.c`), about 1.45x the film's average.

Product implications:
- 3D is feasible on the Go 2: one core decodes it with room to spare, so
  multithreading is optional.
- Read the `SSIF` (both views interleaved), not the two clips. libbluray's
  playback (`bd_read`) reads only the base clip, so 3D playback needs our
  own SSIF reader in libdiscnav (extents from the clip info), or a
  libbluray patch.
- Integration: an FFmpeg decoder wrapping edge264-mvc that outputs full
  side-by-side frames (3840x1080) for mpv, which then plays it as
  ordinary video; half side-by-side and top-and-bottom are a scale away.
  The XR glasses' 3D mode takes full side-by-side directly.

### 0f, part 2 — 3D on the VITURE Pro XR glasses (2026-10-31, Go 2)

- The glasses' Mode button (press and hold) switches 2D/3D. In 3D mode
  their EDID offers only 3840x1080 (2D: 1920x1080 at 60/90/120 Hz), and
  they show the left half to the left eye: full side-by-side.
- Under Game Mode a non-Steam shortcut has no Game Resolution setting and
  no Scaling Mode was offered, and a fullscreen window gets Steam's 16:9
  size (1920x1080; 1280x720 without fullscreen): fitted into 3840x1080 it
  sits in the middle, each eye sees half of its view shifted inwards and
  the images don't fuse. A **3840x1080 window** (`geometry=3840x1080`,
  not fullscreen) is shown 1:1 (`gamescopectl screenshot` 3840x1080): each
  eye gets its full 1920x1080 view.
- Eye order, checked by the user with generated test images (labelled
  eyes, a red square in front and a green one behind): left half = left
  eye. The Hobbit's 3D playlists set `mvc_base_view_r_flag` 1, yet base
  view on the left is correct; don't swap from that flag until another
  disc explains it (libedge264mvc keeps a `base_view_right` option).
- mpv only uses the decoder with `vd=libedge264mvc` and `hwdec=no` (with
  hwdec it swaps in the hardware h264 decoder: one eye). A raw `.264`
  needs `demuxer-lavf-format=h264`.
- In 3D mode a 2D interface is split between the eyes and looks flipped
  with both open: the UI must be drawn once per eye whenever the display
  is side by side.
