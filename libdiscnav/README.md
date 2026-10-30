# libdiscnav

Disc menus for players that take a byte stream. LGPL-2.1-or-later (see
`COPYING`).

DVD navigation on top of libdvdnav: the DVD VM runs inside libdiscnav and
hands the disc out as a series of MPEG-PS byte streams ("segments"), which
a player opens as ordinary files, e.g. libmpv through a `stream_cb`
protocol. Menu graphics (subpictures with the disc's palette and button
highlights) are composited by libdiscnav for the player to draw as an
overlay. It grew out of Playser's spike 0b (`docs/spike-results.md` in the
Playser repository), whose lessons it keeps; Blu-ray navigation is planned.

## Model

- **Segments.** A segment ends at a discontinuity: a jump, a new title set,
  or a timestamp break such as a looping menu's restart. The player then
  opens the next segment, which continues wherever the VM is. Seamless cell
  changes (chapters) stay within one segment.
- **Flush.** A user action that jumps (activating a button, calling a
  menu, skipping a chapter) raises `DISCNAV_EVENT_FLUSH`: open the next
  segment right away instead of letting buffered data play out. Natural
  jumps (the end of a title) play out.
- **Stills.** A still frame keeps `discnav_stream_read` blocked until the
  still times out, the user acts, or the read is cancelled. libdiscnav
  feeds what a demuxer needs to show the held picture first.
- **Tracks.** Streams are chosen by the disc's VM (`discnav_get_tracks`,
  as MPEG-PS stream ids, which is what lavf and mpv call `src-id`). In
  menus libdiscnav draws the subpictures itself, so the player must not;
  in titles the player decodes subtitles, with `discnav_get_palette`.
  A track menu lists the disc's own streams (`discnav_get_streams`:
  language, commentary, captions) and changes them through the disc
  (`discnav_select_stream`), so the choice holds across segments.
- **Back.** `DISCNAV_GO_UP` follows the disc; many discs leave it
  undefined in submenus (relying on a "main menu" button), and then it
  goes to the title set's root menu. At the main menu it's refused.
- **Threads.** Stream functions run on the player's reader thread, the
  rest on any thread. Callbacks come from either, with an internal lock
  held: queue work from them, don't call back into libdiscnav.

## Use with libmpv

```c
mpv_stream_cb_add_ro(mpv, "discnav", nav, open_fn);   // open_fn: discnav_stream_open
// read_fn  -> discnav_stream_read
// cancel_fn -> discnav_stream_cancel
// close_fn -> discnav_stream_close
loadfile discnav://   with per-file options:
    demuxer-lavf-format=mpeg, demuxer-lavf-probesize=1048576,
    demuxer-lavf-analyzeduration=0.5, cache=no, demuxer-readahead-secs=0.5,
    video-latency-hacks=yes, vd-lavc-threads=1, sid=no
```

On `MPV_EVENT_FILE_LOADED` call `discnav_segment_opened`; on
`MPV_EVENT_END_FILE` load `discnav://` again unless a flush already did, or
stop once `discnav_stopped`. Playser's `app/src/DiscNav.cpp` is a complete
example (Qt), including track selection and the menu overlay.

## Build

CMake; needs libdvdnav (and libdvdcss at runtime for encrypted discs).
Builds a static `libdiscnav.a` and installs `discnav.h` and `discnav.pc`.

```sh
cmake -S libdiscnav -B build/discnav -G Ninja && ninja -C build/discnav
```

Tests run through Playser against a synthetic disc:
`tools/make-test-dvd.sh` and `tools/test-dvd-menus.py`.
