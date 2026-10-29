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
