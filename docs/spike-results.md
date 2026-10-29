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
   the held picture, emit some padding so lavf finishes probing, then block
   in `read_fn` until timeout/user input. Menus without audio need no
   special handling (`--no-menu-audio` turned out unnecessary).
8. **Menu subpicture track:** `sid=auto` won't select it; select it
   explicitly.

Open items (next steps for 0b / Phase 2):
- Real button graphics: mpv renders the SPU without DVD highlight palette
  changes, so the disc's own highlight colours never show. Decode the SPU
  highlight (CLUT + button colour/contrast from PCI) ourselves, or composite
  via `overlay-add` / the QML layer.
- Nav state runs ahead of presentation by the demuxer readahead; tie
  highlight updates to PTS.
- Untested: commercial discs (CSS, multi-angle, timed stills, odd PGC
  programming), widescreen/letterbox button coordinates, PAL.
- `DVDNAV_WAIT` is skipped immediately; should wait for mpv to drain.
