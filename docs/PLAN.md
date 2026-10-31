# Playser — Plan

## Goal

A controller-first, 10-foot media player that feels native in SteamOS Game Mode.
Primary use: **physical media** (DVD, Blu-ray, UHD Blu-ray) **with full disc
menus**. Secondary (later): media on a NAS via Plex and Jellyfin.

Differentiators vs. Kodi / Plex HTPC: native Game Mode feel, HDR under
gamescope, and zero-fuss MakeMKV/UHD playback with menus.

## Key decisions

- **Playback engine: libmpv** (render API, `vo=gpu-next`/libplacebo for HDR
  and tone mapping). Not mplayer (unmaintained, no embedding API); VLC was
  considered only for its disc-menu support, which we'll build ourselves.
- **Disc menus: `libdiscnav`**, a standalone LGPL C library that bridges
  libdvdnav/libbluray to libmpv. mpv removed its own dvdnav menu support, so
  upstream inclusion is unlikely; ship it as a reusable library for any libmpv
  app.
- **Decryption:** libdvdcss (DVD CSS); MakeMKV's `libmmbd` as a libaacs /
  libbdplus replacement for libbluray (AACS, AACS 2.0 / UHD). The user supplies
  their own MakeMKV license. UHD requires a LibreDrive-compatible drive.
- **UI: Qt6/QML** on `org.kde.Platform`; gamepad via SDL3 / Steam Input.
- **mpv owns the only window** (`vo=gpu-next`, Vulkan, HDR10). The QML UI is
  rendered off-screen (Qt Quick software backend, 1080p) and composited by
  mpv as an `overlay-add` bitmap, re-encoded to PQ/HLG over HDR video
  (spike 0d). Disc menu graphics will use the same path.
- **Packaging: Flatpak.** GitHub releases first, Flathub as the end goal (free
  app; users buy MakeMKV separately).

## Architecture

```
┌─────────────────────────────────────────────┐
│  UI: Qt6/QML, 10-foot, gamepad-first        │
├─────────────────────────────────────────────┤
│  Sources: Disc (udisks2) │ Plex │ Jellyfin  │
├─────────────────────────────────────────────┤
│  libdiscnav  (menus, nav events, overlays)  │
├─────────────────────────────────────────────┤
│  libmpv, own window: video + overlays (HDR) │
│   └─ libbluray → libmmbd (user's MakeMKV)   │
└─────────────────────────────────────────────┘
```

### libdiscnav design sketch

```
libdvdnav / libbluray ──nav events, stills, highlights──┐
      │ MPEG-PS / M2TS bytes                            │
      ▼                                                 ▼
 mpv stream_cb (discnav:// protocol)          overlay compositor
      │                                     (DVD SPU highlights,
      ▼                                      BD IG / ARGB overlays)
   libmpv ◄──── flush / seek / pause-on-still ──────────┘
```

Hard problems:
- **Nav ↔ playback sync:** mpv's demuxer cache reads ahead of what's on
  screen; menu events must be tied to presentation time (tiny cache plus
  PTS-tagged events).
- **Still frames / looping menus:** hold a frame indefinitely, or loop a
  motion menu, while the user navigates.
- **Overlays:** button highlights drawn by mpv `overlay-add`, encoded in
  the video's colorspace like the UI layer (spike 0d).
- **Discontinuities:** clean flushes on title/menu jumps without mpv
  treating them as EOF.

Menu tiers, in order: DVD (libdvdnav) → BD HDMV (libbluray IG, ARGB
overlays) → BD-J (libbluray + JVM; needed for most modern BDs and nearly
all UHD discs).

## Packaging notes

- Read-only rootfs on SteamOS → Flatpak only.
- `--device=all` for `/dev/sr*`; udisks2 over D-Bus for disc insert events.
- MakeMKV: build `libmmbd` from **makemkv-oss** (LGPL) into the Flatpak;
  fetch the proprietary `makemkvcon` from the official download on the
  user's machine via Flatpak **extra-data** (precedent: Flathub's MakeMKV app).
- LibreDrive: the app detects and reports status (via MakeMKV) but never
  flashes firmware. Point users to MakeMKV's docs/forum; crossflashing
  (e.g. BU50N → BU40N firmware) is the user's own risk.
- libdvdcss on Flathub: precedent exists; Flathub's own `io.mpv.Mpv` builds
  and ships it.
- Lessons from the user's Legion Go 2 setup (`~/dev/legiongo2`):
  - Keep everything in `$HOME` or Flatpak; that's what survives SteamOS
    updates.
  - Steam library art: find a non-Steam shortcut's appid in `shortcuts.vdf`
    (regex `\x02appid\x00(.{4})`, little-endian u32), then write
    `<appid>p.png` 600x900, `<appid>.png` 920x430, `<appid>_hero.png`
    1920x620 and `<appid>_logo.png` to `userdata/<user>/config/grid/`.
    Steam needs a restart to show them.
  - Steam launches apps with `no_new_privs`, so a Flatpak started from
    inside a FUSE AppImage fails silently (`bwrap: Unexpected
    capabilities`). Don't ship as an AppImage.
  - Steam Input exposes virtual pads under varying names ("SteamOS Handheld
    Controller" on the Go 2), and SDL2 and SDL3 button names differ. Map by
    SDL3 gamepad button, never by controller name.
- BD-J needs an OpenJDK runtime in the Flatpak (SDK extension).
- Audio passthrough (TrueHD/Atmos, DTS-HD) via PipeWire is a known risk;
  may need mpv `audio-spdif` with direct ALSA output.

## Test hardware

| Device | Role |
|---|---|
| Garuda desktop (ASUS BW-16D1HT UHD drive, MakeMKV) | Primary dev bench; native spikes before Flatpak |
| SteamOS AMD box | Living-room reference: HDMI, TV/AVR, HDR, passthrough, Game Mode |
| Steam Decks | Handheld + docked, USB drive, OLED HDR, Steam Input |
| Legion Go 2 | Second handheld: controls, screen size, performance |

## Phases

### Phase 0 — Risk spikes (done 2026-10-28; details in `spike-results.md`)
- **0a. ✅** libmpv + libbluray + libmmbd plays a UHD main title via `bd://`
  (Garuda desktop). Integration is just `LIBAACS_PATH`/`LIBBDPLUS_PATH`.
- **0b. ✅** DVD menus via libdvdnav → libmpv `stream_cb`: synthetic fixture
  and a commercial disc (*Airheads*) play end to end, with the disc's own
  button graphics (`libdiscnav/spu.c`) and VM-driven track selection.
- **0c. ✅ / ⚠️** SteamOS box: 4K HDR10 from a Flatpak under gamescope,
  VAAPI decode and decoded multichannel PCM all work. **Bitstream
  passthrough through PipeWire does not** (solved 2026-10-29: see below).

Decisions from Phase 0:
- **Audio:** decoded PCM (up to 7.1) is the default. Passthrough is a later,
  opt-in feature. An audio-output failure must fall back to PCM and never
  freeze playback.
- **Flatpak manifest** needs `org.freedesktop.Platform.VulkanLayer.gamescope`
  (matching branch) and `--filesystem=xdg-run/gamescope-0` for HDR.
- **libdiscnav rules:** no blocking libmpv calls on the render thread;
  `BLOCK_FOR_TARGET_TIME=0`; segment model with flush on user jumps; NAV-pack
  probe filler for stills; `video-latency-hacks`; tracks follow the VM.

### Phase 1 — App shell + menu-less disc playback
Qt6/QML 10-foot UI, gamepad navigation, disc insert detection, main-feature
playback, track/chapter selection, player OSD, first-run MakeMKV setup and
LibreDrive check.

Status (2026-10-28), `app/`:
- ✅ `MpvItem` (libmpv OpenGL render API in Qt Quick; async-only),
  `DiscMonitor` (udisks2), `Gamepad` (SDL3 → key events), home screen,
  player OSD (seek, chapters, progress), audio/subtitle picker. Verified by
  scripted screenshots (`--script`) with the UHD and *Airheads* discs.
- ✅ **HDR architecture** (2026-10-29): mpv owns the window; `UiRenderer`
  renders QML off-screen into a mpv overlay, `OverlayEncoder` re-encodes it
  for PQ/HLG video. Render + encode ≈5 ms per changed frame. Verified on
  the desktop with the DVD and the UHD (SDR display).
  - ✅ Verified on the box in Game Mode via the Flatpak: HDR10 swapchain,
    UI text measured 188 nits (expected ~180 for `#eef1f6` at 203-nit
    white), panels 2–4 nits; OSD, pause, track picker all composited.
  - ✅ Screensaver: Steam's Game Mode screensaver owns
    `org.freedesktop.ScreenSaver`; the app inhibits it while playing
    (`ScreenSaverInhibitor`, verified against KDE; Steam accepts the call).
    A screensaver that is already showing stays above a newly launched game.
  - ✅ **SDR black lift** (2026-10-29): over HDR video on an SDR screen,
    stock mpv took the UI overlay as PQ and tone-mapped it with the video
    (panels 21→40, desaturated; `target-contrast=inf` only got 36). The
    Flatpak's mpv carries `flatpak/patches/mpv-overlay-display-colorspace.patch`:
    `overlay-colorspace=display` composites `overlay-add` bitmaps in the
    display's colorspace, marked as peaking at SDR white (an exact match
    with the video's colorspace would make libplacebo tone-map it
    dynamically with the video: ~62-nit text on the box). `Mpv` then
    encodes the UI for `video-target-params` instead of `video-params`,
    and logs the display (`mpv: display bt.2020 pq 1000 nits`); stock mpv
    rejects the option and keeps the old path. Verified with a synthetic
    HDR10 clip: desktop SDR panel (21,27,39) exact; box HDR panel
    1.9/2.2/4.0 nits, text 173-187, amber 149/101/22 (expected 1.9/2.2/3.9,
    176-186, 149/103/21.5; the old path had amber red at 134).
- ✅ Real controller test (2026-10-31): the user's Steam Controller on the
  box, through Steam Input.
- ✅ "Opening disc…" screen while mpv opens the title; B cancels (the UI
  returns at once; libbluray's open can't be interrupted and ends ~13 s
  later, so end-file events are matched to the current playlist entry).
- ✅ **Main feature via TheDiscDB** (2026-10-29): `DiscMonitor` mounts
  the disc read-only through udisks, hashes it on a worker thread
  (`DiscIndex`) and looks it up in the embedded index
  (`app/data/discdb-index.json.gz`, 5,557 discs, 950 KiB; rebuild with
  `tools/build-discdb-index.py`). Known Blu-rays play their main playlist
  (`bd://mpls/1199`) and show title/release/"4K UHD". Verified with both
  discs. Unknown discs (2026-10-29): the title comes from the Blu-ray's
  `BDMV/META/DL/bdmt_*.xml` (`di:name`), else the volume label tidied
  (`BACK_TO_FUTURE_1` → "Back To Future 1"), and the largest thumbnail
  there is the drive card's art (verified: *Back to the Future*). DVDs (2026-10-29): records name a title set (VTS), which can hold
  several titles; `DiscIndex::dvdTitles` reads the unencrypted IFOs
  (VMG `TT_SRPT`, each VTS's `PTT_SRPT` and PGC playback times) and the
  item gets the title in that set whose length matches (within 5 s or 2%,
  each title used once). mpv counts DVD titles from 0 (`dvd://1` is the
  disc's title 2). Verified on a synthetic 3-title, 2-VTS DVD
  (`build/fixtures/dvd-titles`); not yet on a real catalogued DVD. Extras
  (2026-10-29): the disc screen has an Extras row (the catalogue's
  featurettes, trailers, deleted scenes... in disc order); an extra plays
  without touching the film's resume point and returns to the list. Discs
  with extras now open the disc screen on Play. Verified with the 14
  extras of *Masters of the Universe*. No mounting
  (2026-10-29): discs are read straight from the drive with libudfread
  (vendored in `app/third_party/libudfread`, LGPL) behind `DiscFs`, which
  also reads a mounted directory; the disc is mounted through udisks only
  if the device can't be read. The art is copied to the cache
  (`disc-files/`). Verified: same hash and main feature for the UHD on the
  desktop; on the box (Flatpak) BTTF identified and played from the
  unmounted drive; the synthetic DVD reads the same as folder and ISO. Original notes: (https://thediscdb.com, data repo
  `TheDiscDb/data`, MIT; ~6,150 discs as of 2026-10-29). Each disc record
  has a `ContentHash` and labels playlists `MainMovie` / `Extra` /
  `Episode`. The hash needs no decryption: MD5 over the sizes of
  `BDMV/STREAM/*.m2ts` (DVD: `VIDEO_TS/*.VOB|IFO|BUP`), sorted by bare
  filename (ordinal), each as an 8-byte little-endian integer, uppercase
  hex (reference: `TheDiscDb/optical-disc-manifest`
  `tests/Get-ContentHash.ps1`). Verified: the *Masters of the Universe* UHD
  hashes to `5A8BD9C8…5608`, whose record names `01199.mpls` (2:20:30) as
  `MainMovie`, matching what we found by hand; *Airheads* isn't listed.
  Plan: ship a compact offline index (hash → main playlist, extras with
  names) built from the data repo and refreshed with releases, so no
  network lookup and no privacy question; an optional online refresh
  later. The extras list also gives a usable "Extras" menu where disc menus
  aren't available yet (BD-J). Fallback when the disc is unknown: our own
  heuristic (longest playlist whose clips aren't mostly shared with
  others, chapter count, MakeMKV-style FPL detection), and offer to
  contribute the disc.
- ✅ **Series discs and resume** (2026-10-29): Blu-ray playlists are read
  from the mounted disc (unencrypted `.mpls`), catalogue clips are mapped
  to their one-clip playlists, and episodes listed in disc order
  (`DiscScreen`: Resume / Play all / episodes; films with a saved position
  get Resume / Play from the beginning). The player takes a queue and
  continues to the next episode; position is saved every 10 s and on stop
  per disc (`Resume`, `~/.config/playser/`), cleared after the last
  item ends. Verified: the *Looney Tunes Collector's Choice Vol. 1*
  Blu-ray lists its 19 cartoons on the box; film resume on the desktop
  (`start=` honoured). Each episode reopens the disc (libbluray + AACS
  init), so Play all has a gap between episodes until libdiscnav keeps
  the disc open (Phase 3, Blu-ray navigation).
- ✅ **UI font and controller glyphs** (2026-10-29): Inter 4.1 (OFL,
  variable) is embedded as a Qt resource, so the box (whose runtime only
  has DejaVu Sans) and desktops look the same. Button hints are drawn
  (`ButtonGlyph`, `HintBar`), not font characters, and follow the pad last
  used (SDL3 gamepad type through Steam Input): Xbox letters in colour,
  PlayStation ✕○□△, Nintendo's swapped A/B. Verified on the box (Inter)
  and on the desktop (all three styles).
- ✅ **Audio-output failure no longer freezes the picture** (2026-10-29): a
  watchdog in `Mpv` notices the clock standing still while playing with
  audio (4 s; 8 s while mpv is still starting playback, where a failed
  passthrough start gets stuck) and recovers in steps: passthrough off
  (decode to PCM), reopen the output, then play without sound
  (`ao=null`, retried on the next file), with an on-screen notice.
  `audio-fallback-to-null` covers having no device at all. Verified on the
  box with TrueHD and AC-3 passthrough (`--mpv-option audio-spdif=...`,
  silent synthetic clips): frozen before, now recovers after ~8 s and
  plays to the end; no false trigger in normal playback. The demuxer
  buffer can't tell a slow disc read from a stall (it reads 0 in both),
  so a long disc stall may also trip the first, harmless steps.
- ✅ **Passthrough works on SteamOS** (2026-10-29, spike-results 0c
  update): needed the `spdif` muxer in our FFmpeg, and exclusive use of
  the HDMI sink, which SteamOS's `speech-dispatcher-dummy` stream blocks
  ("cannot link exclusively"). Verified with the box wired straight to the
  VSX-1130: Dolby Digital and Dolby TrueHD shown on the receiver.
- ✅ **Passthrough setting** (2026-10-29): setup screen "Passthrough:
  On/Off" (QtCore Settings `passthrough`) sets mpv `audio-spdif` to
  `ac3,eac3,dts-hd,truehd`; a codec the receiver can't take falls back to
  decoding through the audio watchdog. Turning it on writes
  `~/.config/pipewire/pipewire-pulse.conf.d/90-playser-passthrough.conf`
  (Flatpak grants only that folder): a `pulse.rules` entry giving
  `application.process.binary = "sd_dummy"` `node.autoconnect = false`,
  so only speech-dispatcher's silent dummy stays unconnected (real voices,
  e.g. a screen reader's espeak-ng, still connect; checked with stand-in
  streams). PipeWire reads it at start, so the screen says "restart once"
  until the boot ID changes. Off removes the file. Verified on the box:
  toggled from the setup screen, then TrueHD and AC-3 bitstreamed with no
  test overrides.
- ✅ **Confirmed after a reboot** (2026-10-29): no `sd_dummy` link, and
  *Back to the Future* (Blu-ray, not in TheDiscDB; libbluray picked the
  1:56:05 main title) played its DTS-HD MA 5.1 as bitstream: the VSX-1130
  showed DTS-HD MSTR with L C R SL SR LFE.
- ✅ **Busy output retry** (2026-10-29): Steam's UI audio
  (`steamwebhelper`, "Chromium") holds the sink for ~18 s after any Steam
  sound. When mpv's PipeWire output logs "no target node", Mpv decodes at
  once, then tries passthrough again after 20 s; a second failure leaves it
  decoded with a notice. Verified on the box with a paplay stand-in on HDMI
  removed after 14 s: PCM at 0.2 s, `spdif-truehd` from 20.3 s.
- ✅ **Passthrough after resume** (2026-10-29): resuming *Back to the
  Future* at 37:31 started DTS-HD on an extension packet without its core;
  FFmpeg's spdif muxer rejects it ("stray DTS-HD frame") and mpv 0.41 then
  fails the whole decoder, so the watchdog fell back to decoding. The
  Flatpak's mpv carries `flatpak/patches/mpv-spdif-drop-bad-packets.patch`:
  a rejected packet is dropped, and before any output the muxer starts
  over on the next one. Verified on the box: same resume, passthrough held.
  Worth sending upstream.
- ✅ SteamOS issue drafts (`docs/steamos-issues.md`): load `sg`; a comment
  on #989 about the dummy stream and Steam's UI sounds. Filing is the user's.
- ✅ **4K UHD drive check** (2026-10-29): `SystemCheck::checkUhdDrives()`
  runs `makemkvcon -r info dev:/dev/srN` when a disc goes in (not during
  playback) and when Setup opens. MakeMKV prints `MSG:1011 "Using LibreDrive
  mode (vX)"` before `MSG:3007 "Using direct disc access mode"`, which comes
  either way, so it stops there (~8 s on the box, not the 37 s scan). Only
  trusted with Blu-ray files present (DRV flags & 4), kept per drive
  vendor/model/firmware in settings `uhd/`. Setup shows a "4K UHD discs"
  row; a 4K disc (TheDiscDB, else `index.bdmv` version `INDX0300`) in a
  drive without LibreDrive says so on its card, and Play opens Setup.
  Playback stops a running check first. Verified: ASUS BW-16D1HT
  LibreDrive 06.3 (desktop), Optiarc BC-5500S none (box), and Play pressed
  during a check still opens the disc.
- ✅ **An installed MakeMKV Flatpak is preferred** (2026-10-29): the
  manifest grants read access to `com.makemkv.MakeMKV`'s install folder
  (user and system), and `setupDecryption()` uses its libmmbd/makemkvcon
  (with its lib dir on `LD_LIBRARY_PATH` for makemkvcon), so MakeMKV
  updates from Flathub apply without rebuilding us. Verified on the box:
  Looney Tunes episode 3 plays with Flathub's MakeMKV 2.0.0 (runtime KDE
  5.15-25.08; its libmakemkv resolves to the shared FFmpeg 61). Flatpak
  can't declare app→app dependencies (only runtimes/extensions), so the
  first-run screen should offer to install it. Longer term for Flathub: a
  MakeMKV *extension* of our app, updated by Flathub's bot independently.
- ✅ MakeMKV is also bundled, as the fallback (2026-10-29): makemkv-oss (LGPL) and makemkv-bin
  (redistribution allowed with the EULA referenced; Flathub precedent
  `com.makemkv.MakeMKV`) are Flatpak modules. A Flatpak can't depend on
  another app, only on runtimes/extensions. makemkv-oss 2.0.0 doesn't
  build against FFmpeg 9, so it's built before our FFmpeg and links the
  runtime's (libavcodec 61). libmmbd only looks for makemkvcon in
  /usr/bin and /usr/local/bin, so the app sets `MAKEMKVCON=/app/bin/
  makemkvcon`. Verified on the box: Looney Tunes episode 2 opens in
  7.7 s with only the bundled MakeMKV and the user's key in ~/.MakeMKV.
  (A run of MEDIUM ERROR / NO SEEK COMPLETE on the Optiarc's inner tracks
  was cured by unplugging and replugging it after loading `sg`.)
- ✅ **Setup screen** (2026-10-29, `SetupScreen` + `SystemCheck`): on first
  run, when Blu-ray isn't ready, or via Menu. Checks which MakeMKV is in
  use (installed Flatpak / bundled / system), whether a key is registered
  (never read beyond that), and whether optical drives are reachable via
  /dev/sg*; explains each fix for Desktop Mode (Discover install, Help →
  Register in MakeMKV, the `sg` commands). Choosing a Blu-ray while not
  ready opens it instead of failing. Verified on the box (all ready, 4K)
  and on the desktop with the key missing.
- ✅ LibreDrive check (for UHD) on the setup screen (see above). Learned on the box
  (2026-10-29, MakeMKV 2.0.0 copied in for testing):
  - libbluray appends `.so.0` itself: `LIBAACS_PATH=<dir>/libmmbd`.
  - libmmbd runs `makemkvcon` (found via `MAKEMKVCON`), which needs
    **network** (version check, SDF/key downloads): the manifest needs
    `--share=network`.
  - An unregistered 2.0.0 refuses to run ("version is too old"): setup
    must take the user's key (stored only in `~/.MakeMKV`).
  - **SteamOS doesn't load the `sg` module**, and MakeMKV needs
    `/dev/sg*` ("can't find any usable optical drives"). Setup must detect
    this and show the fix (`sudo modprobe sg`, persisted with
    `/etc/modules-load.d/sg.conf`); udev's uaccess rule then grants the
    session user access.
  - With those, Blu-ray playback works in the Flatpak on the box: the
    Looney Tunes disc opens in ~7.5 s through the Optiarc (no
    LibreDrive), episodes play, resume and Play all (episode 1 → 2,
    7.5 s gap) verified 2026-10-29.
- 🔶 Flatpak manifest `flatpak/io.github.slaapliedje.Playser.yml`
  (KDE 6.11, media modules from `io.mpv.Mpv`).
- ✅ **Name: Playser** (2026-10-29; laser + player, no other app by that
  name on the web, GitHub or Flathub). App ID `io.github.slaapliedje.Playser`,
  binary `playser`, QML module `Playser`, settings `~/.config/playser/`.
  Icon, desktop file and metainfo in `app/data/` (both validate).
  `art/make-art.py` draws the icon and the Steam library images (portrait,
  wide, hero, logo) as SVG and renders them with resvg into `art/out/`;
  installed on the box's shortcut and shown on Steam's home row.
  Needs: libmmbd from the user's MakeMKV inside the sandbox.

Order (proposed 2026-10-29): finish Phase 1, then DVD menus, then BD-J
before HDMV, because both Blu-ray test discs (*Back to the Future*,
*Masters of the Universe*) and nearly all UHDs use BD-J menus (`BDMV/JAR`,
`BDJO`); HDMV waits for an HDMV disc (early Blu-rays, ~2006–2008). GitHub
releases (Phase 6) can start any time after Phase 1; they don't wait for
the phases in between.

### Phase 2 — libdiscnav: DVD menus
Own repo/library from day one (LGPL); integrate into the app. Spike 0b
already plays *Airheads*' menus end to end.
- Library API out of the spike: open, controller input (up/down/left/
  right/select/menu/title), events (title change, still, highlight,
  audio/subtitle stream), menu graphics as bitmaps; libmpv via `stream_cb`
  (`discnav://`).
- App: Play opens the disc's menu (setting: "Go straight to the movie");
  d-pad moves, A selects, Menu button opens the disc menu, View opens the
  title menu. Highlights through a second mpv overlay above the video,
  below the UI.
- Tests: synthetic DVD fixtures (`build/fixtures`) driven by scripted
  input, compared by screenshots.

Status (2026-10-30):
- ✅ `libdiscnav/discnav.{h,c}` (LGPL-2.1+, static, own CMake + pkg-config;
  a Flatpak module): the spike's engine without its globals or SDL. The
  stream calls map 1:1 onto mpv's `stream_cb`; events (overlay, flush,
  tracks, stop) come back through a callback; menu graphics composited
  into an RGBA frame; actions include chapter skip.
- ✅ App: `DiscNav` (QML) registers `discnav://`, reloads segments with the
  spike's bookkeeping, per-file demuxer options (`Mpv::loadFile`), applies
  the VM's tracks, draws the menu graphics as overlay 0 under the UI
  (now overlay 1), encoded like the UI, placed on the video rectangle; the
  audio stall watchdog is off while it plays (stills stop the clock).
- ✅ Player: Play on a DVD opens its menus (with a resume point, the disc
  screen offers Disc menu); in menus d-pad/A work the buttons and B goes
  up (at the top it stops); in titles left/right and LB/RB skip chapters,
  Start opens the disc menu, View (new: `Key_TopMenu`) the title menu.
  No seeking and no resume through menus yet; the OSD shows no times.
- ✅ Verified on the synthetic disc (desktop, also the Flatpak build):
  still menu, highlight moves, motion menu with audio, title, chapter
  skip, menu call from a title, title end back to the menu, B at the top
  menu home. Not yet on a commercial DVD (needs one in a drive).
- ✅ Title subtitles (2026-10-30): mpv decodes them, but through a stream
  it never learns the disc's palette (FFmpeg drew them black and white);
  `DiscNav` passes `discnav_get_palette()` as `sub-lavc-o` `palette`. And
  a subtitle stream only appears with its first packet, which mpv drops
  before the track is selected, so each title segment now starts with an
  empty subpicture announcing the VM's stream. Verified with a forced
  orange/cyan subtitle in the fixture's Title 2 (`tools/make-test-dvd.sh`).
- ✅ Resume through menus (2026-10-30): `discnav_get_position` (title,
  time); stopping during a title (or pressing Start/View in one) saves
  a resume point with its URL (`dvd://N`), which the disc page's Resume
  plays directly. Setup has "DVD menus: On/Off" (off: the disc page opens,
  Play = main feature). Activating a button shows its action colours
  until the jump (seen in a test screenshot: red outline).
- ✅ Seeking in titles through menus (2026-10-30): `discnav_seek`
  (`dvdnav_time_search`) and the title length in `discnav_get_position`;
  left/right seek −10/+30 s, LB/RB skip chapters, and the OSD shows the
  title's time and progress. The menu overlay's rectangle is now worked
  out from `video-params` (dw/dh) and the window, not osd-dimensions'
  margins, which read as "no video" between segments (a stretched
  highlight once).
- ✅ Test flakiness (1 in ~40 runs) was the harness: mpv's screenshot is
  async, so the next scripted key could change the menu first. The
  script runner now waits 300 ms after a shot.
- ⬜ Open: a real commercial DVD on the box; SPU timing (menu graphics
  are shown when decoded, up to the read-ahead early); `DVDNAV_WAIT`
  skipped at once.
- ✅ Tests: `tools/test-dvd-menus.py` drives the app through the test DVD
  (11 checks: highlights, subtitle colours, title end, motion menu,
  chapter skip, menu call, B home) from screenshots and the nav log.

### Phase 3 — Blu-ray navigation and BD-J menus (UHD too)
- Blu-ray navigation mode: libbluray's `bd_play()` + `bd_read_ext()`
  events through `stream_cb` instead of mpv's `bd://`. Keeps the disc open
  (fixes the Play all gap) and is the base for HDMV menus in Phase 4.
- BD-J: bundle a JRE in the Flatpak (OpenJDK SDK extension; ~50 MB) and
  libbluray's `libbluray.jar`. BD-J draws ARGB frames → mpv overlay,
  re-encoded to PQ for UHD (as the UI layer, spike 0d). No BD-Live
  (network) and per-disc persistent storage under the app's data dir.
- Fallback: if the menu doesn't start or stalls, say so and play the main
  feature (TheDiscDB/heuristic), as now.
- Risk: libbluray's BD-J works on many discs, not all; keep a list of
  tested discs.

Status (2026-10-30):
- ✅ Spike 0e: libbluray's BD-J runs under Java 21 (not 24+: no
  SecurityManager; not a headless JRE: libbluray forces AWT on).
- ✅ Flatpak: the openjdk21 SDK extension builds libbluray's BD-J jar
  (Ant included) and a jlink'd JRE goes to `/app/jre` (53 MB; java.rmi is
  needed), `JAVA_HOME=/app/jre`.
- ✅ libdiscnav: a backend per format (`dvd.c`, `bluray.c` behind
  `internal.h`); Blu-ray navigation mode with M2TS segments, BD-J ARGB
  planes as the menu overlay (1920x1080), PIDs and languages from the
  playlist, seek, chapters, pop-up menu, top menu.
- ✅ App: opens in the background, Blu-ray menus read through the disc's
  mount (from the device, BD-J's file cache misses files a disc wants),
  "Disc menus" setting covers Blu-ray, falls back to playing the film if
  the menus don't start. Start = pop-up menu, View = top menu, B closes a
  pop-up.
- ✅ Masters of the Universe UHD (desktop, also the Flatpak build): intro,
  menu over its video, highlight, submenus, Play, the film in 4K with
  seek, chapters, pop-up menu, track picker with the disc's languages.
- ✅ Back to the Future on the box (Flatpak, Game Mode): mounted by the
  app, Universal intro, main menu, highlight, Chapters submenu. The disc's
  BD-Live sockets are refused (its online ticker stays empty).
- ✅ Resume through Blu-ray menus: a film started from the disc's menu
  saves its playlist (`bd://mpls/N`) and resumes there without the menus
  (Legend: stopped at 1:52, resumed at 1:52).
- ✅ Play all on one open of the disc: `discnav_open_playlists()` plays a
  queue of playlists without menus (`bd_select_playlist`, the next on
  END_OF_TITLE or an endless still); the app uses it for a Blu-ray's
  episodes (fallback: one mpv open each). Legend (desktop), extras as
  episodes: one open, the still skipped, LB/RB across episodes, the
  episode's number and time on the OSD, resume saves the episode, the
  end of the queue returns home. Next: Looney Tunes Collector's Choice on
  the box (HDMV, a playlist per cartoon).
- ✅ MakeMKV's helper (makemkvcon guiserver) no longer outlives the app
  when a disc is still opening at exit (quit or SIGTERM).
- ✅ Menu sound effects (2026-10-31): `DISCNAV_EVENT_SOUND` /
  `discnav_take_sound()` (HDMV buttons and BD-J both play from
  `sound.bdmv`); the app plays them through SDL's audio at mpv's volume,
  opening the output per sound, and only with audio decoded (with
  passthrough the sink is mpv's alone, and players leave them out of a
  bitstream). Verified: the playback side (SDL's disk driver: length and
  gain exact; the PipeWire stream comes and goes). Neither Legend nor
  Looney Tunes has a `sound.bdmv`: the disc side waits for a disc that
  does.
- ✅ A slow open says why after 20 s ("A dirty or scratched disc can take
  a minute"): Legend's copy has read errors (MakeMKV's own scan reports
  them) and opens in anything from 0.6 to 60 s. Quitting during a stuck
  open no longer dies of SIGPIPE: SIGPIPE is ignored and the app leaves
  5 s into a teardown that waits on libmmbd.
- ✅ Tested discs: `docs/tested-discs.md`.
- ⬜ The background graphics plane: libbluray doesn't implement it
  (`HStillImageBackgroundConfiguration.displayImage()` and translucent
  `setColor()` are logged stubs). Needs a libbluray patch and a third
  overlay plane; waiting for a disc that uses it.

### Phase 4 — Blu-ray HDMV menus
libbluray runs the HDMV VM and decodes the IG menus; pass its overlays to
mpv, plus pop-up menus during playback. Test disc: *Legend* (Arrow,
Director's Cut; 25 HDMV titles, no BD-J).

Status (2026-10-30):
- ✅ libdiscnav draws the IG plane from libbluray's YUV overlays (RLE
  images with a BT.709 YCbCr palette) into a back buffer, shown on FLUSH,
  through the same overlay as BD-J.
- ✅ Legend (desktop): First Play, main menu over its video, highlight,
  the Setup submenu, Play Film, seek, the pop-up menu over the film, B
  closing it, View to the main menu. (The colour bars at the bottom of
  its menus are in the disc's menu video.)
- ✅ B in a Blu-ray menu (no "go up" on Blu-ray: a submenu has its own
  return button) or a DVD's top menu asks "Leave the disc?".
- ✅ The box (Looney Tunes: Play all on one open).

### Phase 5 — 3D (Blu-ray 3D and side-by-side files)
**On hold** (2026-10-29) until the box moves to a room with a 3D TV: an LG
and a Panasonic plasma (not the living-room 4K LG, which has no 3D).
Requested by a user. Test gear: passive and active 3D glasses, VR
headsets, VITURE XR glasses.
- **Decoding:** 3D Blu-rays carry the second eye as H.264 MVC
  (`BDMV/STREAM/SSIF`, the dependent view in its own stream). FFmpeg (and
  so mpv) doesn't decode MVC (FFmpeg trac #3009, open as of 2025).
  Candidate: **edge264** (software H.264 decoder with MVC; used by
  LibreELEC's 3D builds, draft VLC MR !6630). Spike first: decode a 3D
  Blu-ray's MVC to both views on the box and the Deck at full frame rate,
  and check edge264's licence. Integrate as an FFmpeg decoder in our
  Flatpak build that outputs both eyes as one full side-by-side frame;
  libbluray must deliver the SS playlist's dependent-view stream too.
- **Output modes** (per display, remembered): 2D (left eye only, the
  default), half side-by-side and half top-and-bottom for 3D TVs and
  projectors (the TV is set to the matching 3D mode; works with passive
  and active glasses), full side-by-side 3840×1080 for VITURE and similar
  glasses in their 3D mode, and anaglyph. HDMI 1.4 frame packing
  (1920×2205) needs a stereo display mode that gamescope doesn't offer:
  out of scope unless that changes.
- **UI, subtitles and menus in 3D:** the UI overlay and subtitles are drawn
  once per eye (subtitles at the disc's depth offsets where given, else a
  fixed one); disc menus show in both eyes.
- **Files:** side-by-side / top-and-bottom MKVs (mpv `stereo-mode`, or
  chosen by hand) use the same output modes; NAS sources too (Phase 7).
- **VR headsets** (stretch): a virtual cinema screen through OpenXR
  (SteamVR / Monado), separate from the output modes above.

### Phase 6 — Distribution
Flatpak on GitHub releases, Steam artwork (drawn, see Phase 1) + Steam Input template, then
Flathub submission.

Steam, as a possible second channel (decide later; keep the door open now):
- AI policy: since Valve's 2026-01-16 update, code assistants and other dev
  tools need no disclosure; only player-facing generated content (art,
  audio, text, store/marketing assets) does. Keep store art and icons
  human-made, or disclose. Re-check Valve's Steamworks docs before
  submitting. The current art was drawn by code an assistant wrote, so
  a Steam store page would disclose it or replace it with human-made art.
- Free, open-source non-game software is accepted (OBS, Blender, Krita);
  there is a per-app Steam Direct fee.
- **Biggest risk is disc decryption, not AI.** A Steam build likely must ship
  without libdvdcss and rely only on a user-installed MakeMKV. Ask Valve
  before investing.
- Licensing: the app is GPL-3.0-or-later (`COPYING`; FFmpeg as built is
  GPLv3, mpv and libdvdnav GPL); libdiscnav stays LGPL-2.1-or-later. The Steamworks
  SDK is proprietary, so keep Steam API use in an optional, separately
  loaded module (or a GPL linking exception) and never require it.
- On Steam, the native Steam Input API (action sets, per-device glyphs)
  becomes available; SDL3 stays as the fallback everywhere else.

Input (applies now, whatever the channel):
- Steam Input already sits in front of SDL3 when launched from Steam: it
  presents a virtual pad and tells SDL to ignore the physical device. Keep
  SDL3 as the base and route everything through an input interface, so a
  Steam Input API backend can be added later.
- Ship a recommended Steam Input layout for the non-Steam shortcut
  (Phase 6, alongside the library art), so users remap the usual Steam way
  and extra buttons (back paddles, Go 2 buttons) can be bound without code.
- On-screen button hints use SDL3's gamepad type (Xbox, PlayStation,
  Nintendo, Steam Deck) to pick the right glyphs instead of fixed letters.

### Phase 7 — Plex, then Jellyfin
- Sign-in by on-screen code (plex.tv PIN link; Jellyfin Quick Connect),
  never typing on a controller.
- Library pages with posters, Continue Watching, search with the Steam
  on-screen keyboard; direct play through mpv; report progress back so
  resume follows the user between devices.

### Not yet in a phase
- Settings screen (beyond Setup): output modes, passthrough, menus on/off,
  subtitles default.
- Handheld layout (Deck 1280×800, Legion Go 2) and docked/undocked change.
- Sleep and wake in Game Mode mid-film; eject from the controller; error
  screens for unreadable discs.

## Repo layout

```
app/          Qt6/QML frontend
libdiscnav/   C library (LGPL), split out later
spikes/       throwaway Phase 0 programs
flatpak/      manifest, extra-data MakeMKV module, JDK extension
docs/         this plan, design notes
```
