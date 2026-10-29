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
