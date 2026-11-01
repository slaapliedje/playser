# Tested discs

Commercial discs Playser has been tried with, what worked and what didn't.
libbluray's BD-J runs many discs, not all, so this list is the record of
which ones do. Synthetic test media (`build/clips`, `build/fixtures`,
`tools/test-dvd-menus.py`) isn't listed here.

Drives: **desktop** ASUS BW-16D1HT (LibreDrive 06.3, reads 4K UHD), Garuda
Linux; **box** Sony Optiarc BC-5500S over USB (no LibreDrive), SteamOS in
Game Mode, Flatpak build; **Go 2** Legion Go 2 with the same Optiarc and
VITURE Pro XR glasses, SteamOS 3.9 in Game Mode.

| Disc | Kind | Menus | Where | Result |
|---|---|---|---|---|
| *Airheads* (Fox) | DVD, CSS | DVD | desktop (Optiarc) | Menus end to end with the disc's own cursor; track choice follows the disc. Not in TheDiscDB. |
| *Masters of the Universe* | 4K UHD | BD-J | desktop | Intro, menu over its video, highlight, submenus, Play, the film in 4K with seek, chapters, pop-up menu, track picker. |
| *Back to the Future* (Universal) | Blu-ray | BD-J | box | Mounted by the app, intro, main menu, highlight, Chapters submenu. BD-Live is refused (the online ticker stays empty). DTS-HD MA passthrough. Not in TheDiscDB. |
| *Legend* (Arrow, Director's Cut) | Blu-ray, AACS | HDMV | desktop | First Play, main menu over its video, Setup submenu, Play Film, pop-up menu, resume from the menu's film. Extras as a Play all queue (one endless still skipped). The colour bars under its menus are in the disc's video. MakeMKV reports read errors on this copy: opens take 0.6–60 s. No menu sounds on the disc. |
| *Looney Tunes Collector's Choice Vol. 1* | Blu-ray, AACS | HDMV | box | Its 19 cartoons listed from TheDiscDB, episodes, resume, Play all from the disc page on one open of the disc, RB moving on through cartoons 1–3 with the right titles and tracks. No menu sounds on the disc. |
| *The Hobbit: An Unexpected Journey* Extended Edition, Part 1 (Warner) | Blu-ray 3D, AACS | BD-J | Go 2 (Optiarc on USB-C) | In 3D on VITURE Pro glasses from its menus: intro, 3D check, menu, the film in full side-by-side with seeks and chapters, depth the right way round (the playlist's base view is the right eye). The menus needed a libbluray fix (S3D configurations). The drive needs the Go 2's own USB-C port: on the dock it reports no disc. |

Not yet covered by any disc on hand: Blu-ray menu sound effects
(`BDMV/AUXDATA/sound.bdmv`), BD+ discs, BD-J discs that draw the
background plane (libbluray doesn't implement it), 3D on a 3D TV.
