# SteamOS Media Player — Plan

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
  - ⬜ Known: on SDR displays, PQ overlays get mpv's black-point lift (dark
    panels ~21→41); matters for HDR content on a Deck LCD.
- ⬜ Real controller test (Steam Input on the box, Legion Go 2).
- ⬜ "Opening disc…" state: a UHD takes ~22 s to open (spin-up + MakeMKV
  + 55 playlists) and currently shows black.
- ✅ "Opening disc…" screen while mpv opens the title; B cancels (the UI
  returns at once; libbluray's open can't be interrupted and ends ~13 s
  later, so end-file events are matched to the current playlist entry).
- ✅ **Main feature via TheDiscDB** (2026-10-29): `DiscMonitor` mounts
  the disc read-only through udisks, hashes it on a worker thread
  (`DiscIndex`) and looks it up in the embedded index
  (`app/data/discdb-index.json.gz`, 5,557 discs, 950 KiB; rebuild with
  `tools/build-discdb-index.py`). Known Blu-rays play their main playlist
  (`bd://mpls/1199`) and show title/release/"4K UHD". Verified with both
  discs. Still to do: DVD main title (records name VTS numbers, match by
  duration), an Extras screen, the unknown-disc heuristic, and a
  libudfread reader so discs needn't be mounted. Original notes: (https://thediscdb.com, data repo
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
  per disc (`Resume`, `~/.config/discplayer/`), cleared after the last
  item ends. Verified: the *Looney Tunes Collector's Choice Vol. 1*
  Blu-ray lists its 19 cartoons on the box; film resume on the desktop
  (`start=` honoured). Each episode reopens the disc (libbluray + AACS
  init), so Play all has a gap between episodes until libdiscnav keeps
  the disc open (Phase 3).
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
  `~/.config/pipewire/pipewire-pulse.conf.d/90-discplayer-passthrough.conf`
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
- ⬜ Steam's UI audio (`steamwebhelper`, "Chromium") holds the sink for
  ~18 s after any Steam sound, which blocks passthrough in that window;
  retry passthrough once after a short wait before falling back to
  decoding. Add the dummy-stream blocker to the SteamOS issue.
- ⬜ UHD vs Blu-ray detection (LibreDrive check on the setup screen).
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
- ⬜ LibreDrive check (for UHD) on the setup screen. Learned on the box
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
- 🔶 Flatpak manifest `flatpak/io.github.slaapliedje.DiscPlayer.yml`
  (KDE 6.11, media modules from `io.mpv.Mpv`). App ID is provisional.
  Needs: libmmbd from the user's MakeMKV inside the sandbox.

### Phase 2 — libdiscnav: DVD menus
Own repo/library from day one; integrate into the app.

### Phase 3 — libdiscnav: Blu-ray HDMV menus

### Phase 4 — BD-J (bundled JVM), UHD menus

### Phase 5 — Distribution
Flatpak on GitHub releases, Steam artwork + Steam Input template, then
Flathub submission.

Steam, as a possible second channel (decide later; keep the door open now):
- AI policy: since Valve's 2026-01-16 update, code assistants and other dev
  tools need no disclosure; only player-facing generated content (art,
  audio, text, store/marketing assets) does. Keep store art and icons
  human-made, or disclose. Re-check Valve's Steamworks docs before
  submitting.
- Free, open-source non-game software is accepted (OBS, Blender, Krita);
  there is a per-app Steam Direct fee.
- **Biggest risk is disc decryption, not AI.** A Steam build likely must ship
  without libdvdcss and rely only on a user-installed MakeMKV. Ask Valve
  before investing.
- Licensing: the app will be GPL (libmpv as packaged is GPL). The Steamworks
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
  (Phase 5, alongside the library art), so users remap the usual Steam way
  and extra buttons (back paddles, Go 2 buttons) can be bound without code.
- On-screen button hints use SDL3's gamepad type (Xbox, PlayStation,
  Nintendo, Steam Deck) to pick the right glyphs instead of fixed letters.

### Phase 6 — Plex, then Jellyfin

## Repo layout

```
app/          Qt6/QML frontend
libdiscnav/   C library (LGPL), split out later
spikes/       throwaway Phase 0 programs
flatpak/      manifest, extra-data MakeMKV module, JDK extension
docs/         this plan, design notes
```
