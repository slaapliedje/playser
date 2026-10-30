/*
 * libdiscnav: disc menus for players that take a byte stream.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * DVD navigation on top of libdvdnav: the DVD VM runs here and hands out
 * the disc as one MPEG-PS byte stream per "segment", which a player (e.g.
 * libmpv through a stream_cb protocol) opens as a file. A segment ends at
 * a discontinuity (a jump, a new title set, a looping menu's restart); the
 * player then opens the next one, which continues wherever the VM is. Menu
 * graphics (subpictures with the disc's palette and button highlights) are
 * composited here and drawn by the player as an overlay.
 *
 * Threads: the stream functions run on the player's reader thread, the
 * rest on any other; all calls are serialised internally. Callbacks come
 * from either thread with an internal lock held: they must not call back
 * into discnav, only queue work.
 */
#ifndef DISCNAV_H
#define DISCNAV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct discnav discnav_t;
typedef struct discnav_stream discnav_stream_t;

typedef enum {
    /* the menu graphics or the highlight changed: call discnav_render_overlay */
    DISCNAV_EVENT_OVERLAY,
    /* a user action jumped elsewhere: open the next segment now instead of
     * letting the player's buffered data play out */
    DISCNAV_EVENT_FLUSH,
    /* the VM chose other streams, moved between menu and title, or the
     * subpicture palette changed: call discnav_get_tracks and
     * discnav_get_palette */
    DISCNAV_EVENT_TRACKS,
    /* playback reached the end of the disc's programming */
    DISCNAV_EVENT_STOP,
} discnav_event_t;

typedef void (*discnav_event_fn)(void *user, discnav_event_t event);
/* level 0: notable, 1: detail (every highlight move, segment, still) */
typedef void (*discnav_log_fn)(void *user, int level, const char *message);

/* Opens a DVD (device, image or VIDEO_TS directory). `lang` is the
 * preferred menu/audio/subtitle language ("en"). NULL on failure. */
discnav_t *discnav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user);
void discnav_close(discnav_t *nav);

/* The disc's volume title, or "" */
const char *discnav_title(discnav_t *nav);

/* ---- the byte stream: one discnav_stream per player file (segment) ---- */

discnav_stream_t *discnav_stream_open(discnav_t *nav);
/* Up to n bytes; 0 at the end of the segment, -1 on error or cancel. May
 * block (still frames) until the still ends, a user action or cancel. */
int64_t discnav_stream_read(discnav_stream_t *s, void *buf, uint64_t n);
/* Makes a blocked discnav_stream_read return -1 (any thread). */
void discnav_stream_cancel(discnav_stream_t *s);
void discnav_stream_close(discnav_stream_t *s);
/* The player has finished opening (probing) the current segment. */
void discnav_segment_opened(discnav_t *nav);
/* True once DISCNAV_EVENT_STOP happened. */
bool discnav_stopped(discnav_t *nav);

/* ---- input ---- */

typedef enum {
    DISCNAV_UP, DISCNAV_DOWN, DISCNAV_LEFT, DISCNAV_RIGHT,
    DISCNAV_ACTIVATE,
    DISCNAV_ROOT_MENU,    /* the disc's main menu */
    DISCNAV_TITLE_MENU,   /* the title (top) menu */
    DISCNAV_GO_UP,        /* the menu "above" this one, per the disc */
    DISCNAV_PREV_CHAPTER,
    DISCNAV_NEXT_CHAPTER,
} discnav_action_t;

/* False if the disc refused (e.g. no button here, menu not available). */
bool discnav_action(discnav_t *nav, discnav_action_t action);

/* ---- state ---- */

typedef struct {
    /* MPEG-PS stream ids the VM selected (what lavf/mpv call src-id):
     * AC3 0x80+n, DTS 0x88+n, LPCM 0xa0+n, MPEG audio 0x1c0+n, subpicture
     * 0x20+n; -1 for none */
    int audio_id;
    int sub_id;
    /* subtitles are off: show only forced subpictures */
    bool sub_forced_only;
    /* in a menu: discnav draws the subpictures (discnav_render_overlay),
     * so the player must not */
    bool in_menu;
} discnav_tracks_t;

void discnav_get_tracks(discnav_t *nav, discnav_tracks_t *tracks);

/* The current subpicture palette as 0xRRGGBB, for a player that decodes
 * title subtitles itself. False before the disc has set one. */
bool discnav_get_palette(discnav_t *nav, uint32_t rgb[16]);

/* Composites the menu graphics into a frame-sized buffer (fw x fh, the
 * video's storage size, e.g. 720x480; bytes R,G,B,A, straight alpha).
 * False if there are none (the buffer is then cleared). */
bool discnav_render_overlay(discnav_t *nav, uint8_t *rgba, int fw, int fh);

#ifdef __cplusplus
}
#endif

#endif
