/*
 * libdiscnav: disc menus for players that take a byte stream.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The disc's navigation runs here (DVD: libdvdnav; Blu-ray: libbluray,
 * HDMV and BD-J) and hands out the disc as one byte stream per "segment"
 * (DVD: MPEG-PS; Blu-ray: M2TS, 192-byte packets), which a player (e.g.
 * libmpv through a stream_cb protocol) opens as a file. A segment ends at
 * a discontinuity (a jump, a new title set or playlist, a looping menu's
 * restart); the player then opens the next one, which continues wherever
 * the disc is. Menu graphics (DVD subpictures with the disc's palette and
 * button highlights, Blu-ray graphics planes) are composited here and
 * drawn by the player as an overlay.
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
typedef enum { DISCNAV_DVD, DISCNAV_BLURAY } discnav_format_t;
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

/* Opens a DVD or Blu-ray (device, image, or a directory holding VIDEO_TS
 * or BDMV). `lang` is the preferred menu/audio/subtitle language ("en").
 * NULL on failure. A Blu-ray's open can take seconds (AACS): call it off
 * the UI thread. */
discnav_t *discnav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user);
void discnav_close(discnav_t *nav);

discnav_format_t discnav_format(discnav_t *nav);

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
    DISCNAV_POPUP_MENU,   /* Blu-ray: the pop-up menu over the film */
} discnav_action_t;

/* False if the disc refused (e.g. no button here, menu not available). */
bool discnav_action(discnav_t *nav, discnav_action_t action);

/* ---- state ---- */

typedef struct {
    /* the streams the disc selected, as the demuxer's stream ids (what
     * lavf/mpv call src-id); -1 for none. DVD (MPEG-PS stream ids): AC3
     * 0x80+n, DTS 0x88+n, LPCM 0xa0+n, MPEG audio 0x1c0+n, subpicture
     * 0x20+n. Blu-ray: MPEG-TS PIDs (audio 0x1100+n, PG 0x1200+n). */
    int audio_id;
    int sub_id;
    /* subtitles are off: show only forced subpictures */
    bool sub_forced_only;
    /* in a menu: discnav draws the subpictures (discnav_render_overlay),
     * so the player must not */
    bool in_menu;
} discnav_tracks_t;

void discnav_get_tracks(discnav_t *nav, discnav_tracks_t *tracks);

typedef enum { DISCNAV_AUDIO, DISCNAV_SUBTITLES } discnav_stream_type_t;

/* what the disc says a stream is for */
typedef enum {
    DISCNAV_KIND_NORMAL,
    DISCNAV_KIND_COMMENTARY,
    DISCNAV_KIND_VISUALLY_IMPAIRED,
    DISCNAV_KIND_LARGE,
    DISCNAV_KIND_CHILDREN,
    DISCNAV_KIND_CAPTIONS,
    DISCNAV_KIND_FORCED,
} discnav_kind_t;

typedef struct {
    int index;           /* the disc's stream number, for discnav_select_stream */
    int id;              /* the demuxer's stream id, as in discnav_tracks_t */
    /* "en" (DVD: ISO 639-1) or "eng" (Blu-ray: ISO 639-2), "" if the disc
     * doesn't say */
    char lang[4];
    discnav_kind_t kind;
    /* audio: "ac3", "eac3", "dts", "dtshd", "truehd", "lpcm", "mpeg",
     * "sdds"; else "" */
    const char *format;
    int channels;        /* audio */
    bool selected;       /* subtitles: shown (not just forced ones) */
} discnav_stream_info_t;

/* The current title's audio or subtitle streams, up to max of them;
 * returns how many. 0 in a menu. */
int discnav_get_streams(discnav_t *nav, discnav_stream_type_t type, discnav_stream_info_t *streams, int max);

/* Makes the disc play stream `index` (discnav_stream_info_t.index); for
 * subtitles -1 turns them off (forced subpictures still show). Expect
 * DISCNAV_EVENT_TRACKS. False in a menu or if the disc has no such stream. */
bool discnav_select_stream(discnav_t *nav, discnav_stream_type_t type, int index);

/* Where playback is: the title (1-based, as the disc numbers them), the
 * time into it and its length (0 if unknown; length may be NULL). False
 * in a menu. Runs ahead of what's on screen by the player's read-ahead. */
bool discnav_get_position(discnav_t *nav, int *title, double *seconds, double *length);

/* Blu-ray: the playlist playing (00042.mpls is 42), the unit a player
 * opens a title by without menus (e.g. mpv's bd://mpls/42). -1 in a menu
 * or on a DVD. */
int discnav_get_playlist(discnav_t *nav);

/* Jumps to `seconds` into the current title (a user jump: expect a
 * flush). False in a menu or if the disc refuses. */
bool discnav_seek(discnav_t *nav, double seconds);

/* The current subpicture palette as 0xRRGGBB, for a player that decodes
 * title subtitles itself. False before the disc has set one. */
bool discnav_get_palette(discnav_t *nav, uint32_t rgb[16]);

/* The size discnav_render_overlay wants, if the disc's graphics have
 * their own (Blu-ray: 1920x1080, drawn over the whole picture). False if
 * they're at the video's storage size (DVD, e.g. 720x480). */
bool discnav_overlay_size(discnav_t *nav, int *w, int *h);

/* Composites the menu graphics into a w x h buffer (bytes R,G,B,A,
 * straight alpha; see discnav_overlay_size). False if there are none (the
 * buffer is then cleared). */
bool discnav_render_overlay(discnav_t *nav, uint8_t *rgba, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
