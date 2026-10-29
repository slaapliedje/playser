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
│  libmpv render API (Vulkan/GL)              │
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
- **Overlays:** button highlights drawn by mpv `overlay-add` or our own
  render layer (QML is likely cleaner).
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
- libdvdcss on Flathub: unresolved; check VLC's Flathub manifest for precedent.
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

### Phase 0 — Risk spikes
- **0a.** libmpv + libbluray + libmmbd plays a UHD main title via `bd://`
  (Garuda desktop).
- **0b.** Menu feasibility: libdvdnav → libmpv via stream_cb, render a DVD
  menu with button highlights, navigate and activate buttons. *Biggest risk.*
- **0c.** Same libmpv app under gamescope on the SteamOS box: HDR output and
  TrueHD/DTS-HD passthrough to an AVR.

### Phase 1 — App shell + menu-less disc playback
Qt6/QML 10-foot UI, gamepad navigation, disc insert detection, main-feature
playback, track/chapter selection, player OSD, first-run MakeMKV setup and
LibreDrive check.

### Phase 2 — libdiscnav: DVD menus
Own repo/library from day one; integrate into the app.

### Phase 3 — libdiscnav: Blu-ray HDMV menus

### Phase 4 — BD-J (bundled JVM), UHD menus

### Phase 5 — Distribution
Flatpak on GitHub releases, Steam artwork + Steam Input template, then
Flathub submission.

### Phase 6 — Plex, then Jellyfin

## Repo layout

```
app/          Qt6/QML frontend
libdiscnav/   C library (LGPL), split out later
spikes/       throwaway Phase 0 programs
flatpak/      manifest, extra-data MakeMKV module, JDK extension
docs/         this plan, design notes
```
