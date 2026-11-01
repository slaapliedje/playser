# Playser

A controller-first disc player for the TV, made for SteamOS Game Mode. Put in
a DVD, Blu-ray or 4K UHD disc and play it with its menus, in HDR, with
surround sound sent to your receiver as-is if you like.

Early days: it runs on the author's SteamOS box and desktop, and the discs it
has been tried with are listed in [docs/tested-discs.md](docs/tested-discs.md).
Flatpak bundles are on the [releases page](https://github.com/slaapliedje/playser/releases).

- **Discs:** DVD menus through libdvdnav; Blu-ray menus (HDMV and BD-J)
  through libbluray; both by way of [libdiscnav](libdiscnav/), our menu
  library for libmpv players.
- **Playback:** libmpv with `gpu-next`, HDR10 under gamescope, audio
  passthrough (Dolby, DTS, TrueHD, DTS-HD).
- **3D:** Blu-ray 3D on XR glasses (VITURE) in full side-by-side, decoded
  with [edge264-mvc](https://github.com/jens-duttke/edge264-mvc); Playser
  follows the glasses between 2D and 3D and draws its interface for each
  eye.
- **Disc info:** titles, episodes and art from [TheDiscDB](https://thediscdb.com).
- **Controller:** SDL3 and Steam Input; the whole UI works from a gamepad.

Blu-ray and 4K UHD discs need [MakeMKV](https://www.makemkv.com) and your own
MakeMKV key (UHD also needs a LibreDrive-compatible drive). DVDs play without
it. On SteamOS, MakeMKV also needs the `sg` kernel module loaded; the app's
Setup screen explains how.

## Building

The Flatpak is the supported build:

    flatpak-builder --user --install --force-clean build/flatpak \
        flatpak/io.github.slaapliedje.Playser.yml

It needs the KDE 6.11 runtime and SDK and the OpenJDK 21 SDK extension (for
BD-J menus). For HDR under gamescope, also install
`org.freedesktop.Platform.VulkanLayer.gamescope`. See
[docs/PLAN.md](docs/PLAN.md) for the design and progress.

## Licence

Playser is GPL-3.0-or-later ([COPYING](COPYING)). libdiscnav is
LGPL-2.1-or-later ([libdiscnav/COPYING](libdiscnav/COPYING)). Bundled
components keep their own licences, installed under `share/licenses`.
MakeMKV's `makemkvcon` is proprietary and covered by its own EULA.
