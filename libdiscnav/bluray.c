/*
 * libdiscnav: the Blu-ray backend, on libbluray's navigation mode (see
 * discnav.h).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * From spike 0e (docs/spike-results.md):
 * - bd_play() runs the disc from First Play (HDMV or BD-J; BD-J needs a
 *   Java 21 JRE and libbluray's jar); bd_read_ext() hands out the selected
 *   playlist as 6144-byte aligned units of M2TS (192-byte packets), events
 *   first: when one is pending no data comes back.
 * - BD-J draws into ARGB planes the player allocates (BD_ARGB_BUFFER);
 *   libbluray tracks the dirty area and announces a finished frame with
 *   FLUSH. The overlay callbacks come on libbluray's BD-J thread, possibly
 *   while it holds its own lock, which the reader thread waits for inside
 *   bd_read_ext() with ours held: they only take the overlay lock.
 * - HDMV menus (the IG plane) come as palette images, RLE-compressed with
 *   a YCbCr palette, drawn here into a back buffer and shown on FLUSH. They
 *   come from inside bd_read_ext() or bd_user_input(), our lock held.
 *   libbluray leaves subtitles (PG) to the player unless told to decode them.
 * - A segment ends where timestamps start over: a new playlist, a seek
 *   (including a looping menu's restart), a discontinuity or a stopped
 *   playlist.
 * - "In a menu" (the d-pad goes to the disc): the top menu, First Play,
 *   or graphics on screen (a pop-up menu over the film). BD-J discs keep
 *   BD_EVENT_MENU on through their titles, so it only counts for HDMV.
 * - Without menus (discnav_open_playlists) the disc isn't started:
 *   bd_select_playlist() plays each playlist of the queue in turn, the
 *   next on END_OF_TITLE, one segment each, on the one open disc.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libbluray/bluray.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>

#define UNIT 6144
#define IDLE_WAIT_MS 40

enum { PLANE_PG, PLANE_IG, PLANES };

struct nav_impl {
    discnav_event_fn on_event;
    discnav_log_fn on_log;
    void *user;

    pthread_mutex_t lock;       /* guards bd and the state below */
    pthread_cond_t wake;
    BLURAY *bd;
    char title[64];
    unsigned gen;               /* bumped on every user action */
    bool stopped;
    bool user_jump;             /* a user action may jump: flush on the break */
    unsigned title_num;         /* BD_EVENT_TITLE: 0 top menu, 0xffff First Play */
    bool menu;                  /* BD_EVENT_MENU: an HDMV menu is shown */
    bool bdj;                   /* BD-J menus: BD_EVENT_MENU stays on in titles */
    bool in_menu;               /* as last told (DISCNAV_EVENT_TRACKS) */
    int playlist;
    int playitem;
    int chapter;                /* 1-based */
    BLURAY_TITLE_INFO *info;    /* the playlist's, for PIDs, languages, length */
    int audio_num;              /* 1-based stream numbers in the play item's table */
    int pg_num;
    bool pg_on;
    int want_aid, want_sid;
    uint64_t still_until;       /* a timed still ends (ms), 0: none */
    bool title_ended;           /* END_OF_TITLE seen, nothing read since */
    int *queue;                 /* without menus: the playlists to play */
    int queue_len, queue_pos;
    bool restart;               /* a queued playlist started: new segment */

    pthread_mutex_t olock;      /* guards the planes; never taken with lock wanted */
    BD_ARGB_BUFFER argb;        /* libbluray draws into argb.buf[plane] */
    uint32_t *front[PLANES];    /* the last flushed frames */
    uint32_t *hdmv[PLANES];     /* HDMV: the frame being drawn */
    int pw, ph;
    bool graphics;              /* something visible in the IG plane */
};

struct nav_stream {
    nav_impl_t *nav;
    uint8_t buf[UNIT];
    int pos, len;
    bool cancel, eof, got_data;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(nav_impl_t *nav, int level, const char *fmt, ...)
{
    if (!nav->on_log)
        return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    nav->on_log(nav->user, level, msg);
}

static void emit(nav_impl_t *nav, discnav_event_t ev)
{
    if (nav->on_event)
        nav->on_event(nav->user, ev);
}

/* ---------- graphics (libbluray's BD-J thread; overlay lock only) ---------- */

static bool plane_visible(const uint32_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] >> 24)
            return true;
    return false;
}

/* All planes closed. */
static void close_planes(nav_impl_t *nav)
{
    pthread_mutex_lock(&nav->olock);
    for (int p = 0; p < PLANES; p++) {
        if (nav->front[p])
            memset(nav->front[p], 0, (size_t)nav->pw * nav->ph * 4);
        if (nav->hdmv[p])
            memset(nav->hdmv[p], 0, (size_t)nav->pw * nav->ph * 4);
    }
    bool was = nav->graphics;
    nav->graphics = false;
    pthread_mutex_unlock(&nav->olock);
    emit(nav, DISCNAV_EVENT_OVERLAY);
    if (was)
        emit(nav, DISCNAV_EVENT_TRACKS);
}

/* After a command on plane p (olock held; released here). */
static void plane_changed(nav_impl_t *nav, int p, bool changed)
{
    bool shown = nav->graphics;
    if (changed && p == PLANE_IG)
        nav->graphics = nav->front[p] && plane_visible(nav->front[p], (size_t)nav->pw * nav->ph);
    bool flipped = shown != nav->graphics;
    pthread_mutex_unlock(&nav->olock);
    if (flipped)
        logf_(nav, 1, "graphics %s (plane %d)", nav->graphics ? "shown" : "gone", p);
    if (changed)
        emit(nav, DISCNAV_EVENT_OVERLAY);
    if (flipped)
        emit(nav, DISCNAV_EVENT_TRACKS); /* in_menu follows the graphics */
}

/* A plane's front buffer at w x h, cleared (olock held). */
static bool init_front(nav_impl_t *nav, int p, int w, int h)
{
    uint32_t *front = calloc((size_t)w * h, 4);
    if (!front)
        return false;
    free(nav->front[p]);
    nav->front[p] = front;
    nav->pw = w;
    nav->ph = h;
    return true;
}

static void argb_overlay(void *handle, const BD_ARGB_OVERLAY *ov)
{
    nav_impl_t *nav = handle;
    if (!ov) {
        close_planes(nav);
        return;
    }
    int p = ov->plane;
    if (p >= PLANES) {
        if (ov->cmd == BD_ARGB_OVERLAY_INIT)
            logf_(nav, 1, "background graphics plane not shown");
        return;
    }
    bool changed = false;
    pthread_mutex_lock(&nav->olock);
    switch (ov->cmd) {
    case BD_ARGB_OVERLAY_INIT: {
        uint32_t *back = calloc((size_t)ov->w * ov->h, 4);
        if (!back || !init_front(nav, p, ov->w, ov->h)) {
            free(back);
            break;
        }
        free(nav->argb.buf[p]);
        nav->argb.buf[p] = back;
        nav->argb.width = ov->w;
        nav->argb.height = ov->h;
        changed = true;
        break;
    }
    case BD_ARGB_OVERLAY_CLOSE:
        if (nav->front[p])
            memset(nav->front[p], 0, (size_t)nav->pw * nav->ph * 4);
        changed = true;
        break;
    case BD_ARGB_OVERLAY_FLUSH: {
        /* copy what changed since the last frame */
        uint32_t *back = nav->argb.buf[p], *front = nav->front[p];
        if (!back || !front)
            break;
        int x0 = nav->argb.dirty[p].x0, y0 = nav->argb.dirty[p].y0;
        int x1 = nav->argb.dirty[p].x1, y1 = nav->argb.dirty[p].y1;
        if (x1 >= nav->pw)
            x1 = nav->pw - 1;
        if (y1 >= nav->ph)
            y1 = nav->ph - 1;
        for (int y = y0; y <= y1 && x0 <= x1; y++)
            memcpy(front + (size_t)y * nav->pw + x0, back + (size_t)y * nav->pw + x0, (size_t)(x1 - x0 + 1) * 4);
        changed = true;
        break;
    }
    default:
        break;
    }
    plane_changed(nav, p, changed); /* unlocks */
}

/* A palette entry (limited range, BT.709 as HD video) as ARGB. */
static uint32_t ycbcr_argb(const BD_PG_PALETTE_ENTRY *e)
{
    double y = 1.164 * (e->Y - 16), cb = e->Cb - 128, cr = e->Cr - 128;
    double rgb[3] = { y + 1.793 * cr, y - 0.213 * cb - 0.533 * cr, y + 2.112 * cb };
    uint32_t v = (uint32_t)e->T << 24;
    for (int i = 0; i < 3; i++)
        v |= (uint32_t)(rgb[i] < 0 ? 0 : rgb[i] > 255 ? 255 : (int)(rgb[i] + 0.5)) << (16 - 8 * i);
    return v;
}

/* Decodes an RLE image (runs per line, each line ending in a zero-length
 * run) into dst at x, y, clipped to the plane. */
static void draw_rle(uint32_t *dst, int pw, int ph, const BD_OVERLAY *ov)
{
    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = i == 0xff ? 0 : ycbcr_argb(&ov->palette[i]);
    const BD_PG_RLE_ELEM *e = ov->img;
    for (int y = 0; y < ov->h; y++) {
        uint32_t *row = ov->y + y < ph ? dst + (size_t)(ov->y + y) * pw : NULL;
        for (int x = 0; x < ov->w; e++) {
            if (!e->len)
                continue; /* a stray line end */
            int n = e->len < ov->w - x ? e->len : ov->w - x;
            for (int i = 0; i < n && row && ov->x + x + i < pw; i++)
                row[ov->x + x + i] = lut[e->color & 0xff];
            x += n;
        }
        if (!e->len)
            e++;
    }
}

/* HDMV menus: libbluray's graphics controller, our lock held */
static void yuv_overlay(void *handle, const BD_OVERLAY *ov)
{
    nav_impl_t *nav = handle;
    if (!ov) {
        close_planes(nav);
        return;
    }
    int p = ov->plane;
    if (p >= PLANES)
        return;
    bool changed = false;
    pthread_mutex_lock(&nav->olock);
    size_t n = (size_t)nav->pw * nav->ph;
    switch (ov->cmd) {
    case BD_OVERLAY_INIT: {
        uint32_t *back = calloc((size_t)ov->w * ov->h, 4);
        if (!back || !init_front(nav, p, ov->w, ov->h)) {
            free(back);
            break;
        }
        free(nav->hdmv[p]);
        nav->hdmv[p] = back;
        changed = true;
        break;
    }
    case BD_OVERLAY_CLOSE:
        if (nav->front[p])
            memset(nav->front[p], 0, n * 4);
        if (nav->hdmv[p])
            memset(nav->hdmv[p], 0, n * 4);
        changed = true;
        break;
    case BD_OVERLAY_CLEAR:
    case BD_OVERLAY_HIDE:
        if (nav->hdmv[p])
            memset(nav->hdmv[p], 0, n * 4);
        break;
    case BD_OVERLAY_WIPE:
        for (int y = ov->y; nav->hdmv[p] && y < ov->y + ov->h && y < nav->ph; y++)
            for (int x = ov->x; x < ov->x + ov->w && x < nav->pw; x++)
                nav->hdmv[p][(size_t)y * nav->pw + x] = 0;
        break;
    case BD_OVERLAY_DRAW:
        if (nav->hdmv[p] && ov->img && ov->palette)
            draw_rle(nav->hdmv[p], nav->pw, nav->ph, ov);
        break;
    case BD_OVERLAY_FLUSH:
        if (nav->hdmv[p] && nav->front[p]) {
            memcpy(nav->front[p], nav->hdmv[p], n * 4);
            changed = true;
        }
        break;
    }
    plane_changed(nav, p, changed); /* unlocks */
}

/* ---------- navigation state (lock held) ---------- */

static bool graphics_shown(nav_impl_t *nav)
{
    pthread_mutex_lock(&nav->olock);
    bool g = nav->graphics;
    pthread_mutex_unlock(&nav->olock);
    return g;
}

static bool menu_now(nav_impl_t *nav)
{
    return nav->title_num == 0 || nav->title_num == 0xffff || (nav->menu && !nav->bdj) || graphics_shown(nav);
}

static const BLURAY_CLIP_INFO *clip(nav_impl_t *nav)
{
    if (!nav->info || !nav->info->clip_count)
        return NULL;
    int i = nav->playitem >= 0 && (unsigned)nav->playitem < nav->info->clip_count ? nav->playitem : 0;
    return &nav->info->clips[i];
}

/* The PIDs for the selected stream numbers, from the play item's table. */
static void update_tracks(nav_impl_t *nav)
{
    const BLURAY_CLIP_INFO *c = clip(nav);
    int aid = -1, sid = -1;
    if (c && nav->audio_num >= 1 && nav->audio_num <= c->audio_stream_count)
        aid = c->audio_streams[nav->audio_num - 1].pid;
    if (c && nav->pg_num >= 1 && nav->pg_num <= c->pg_stream_count)
        sid = c->pg_streams[nav->pg_num - 1].pid;
    bool menu = menu_now(nav);
    if (aid != nav->want_aid || sid != nav->want_sid || menu != nav->in_menu) {
        if (menu != nav->in_menu)
            logf_(nav, 0, "%s (title %u, menu %d, graphics %d)", menu ? "menu" : "title", nav->title_num, nav->menu,
                  graphics_shown(nav));
        if (aid != nav->want_aid || sid != nav->want_sid)
            logf_(nav, 0, "streams: audio 0x%x, subtitles 0x%x%s", aid, sid, nav->pg_on ? "" : " (forced only)");
        nav->want_aid = aid;
        nav->want_sid = sid;
        nav->in_menu = menu;
        emit(nav, DISCNAV_EVENT_TRACKS);
    }
}

static void load_playlist(nav_impl_t *nav, int playlist)
{
    if (nav->info)
        bd_free_title_info(nav->info);
    nav->info = bd_get_playlist_info(nav->bd, playlist, 0);
    nav->playlist = playlist;
    nav->playitem = 0;
}

/* Without menus: plays queue[pos] from `seconds` (lock held, or opening). */
static bool start_playlist(nav_impl_t *nav, int pos, double seconds)
{
    int playlist = nav->queue[pos];
    if (!bd_select_playlist(nav->bd, (uint32_t)playlist)) {
        logf_(nav, 0, "can't play %05d.mpls", playlist);
        return false;
    }
    load_playlist(nav, playlist);
    nav->queue_pos = pos;
    nav->title_num = (unsigned)pos + 1;
    nav->chapter = 1;
    if (seconds > 0)
        bd_seek_time(nav->bd, (uint64_t)(seconds * 90000));
    nav->restart = true;
    logf_(nav, 0, "playlist %05d.mpls (%d of %d)", playlist, pos + 1, nav->queue_len);
    nav->want_aid = -2; /* another title: tell the player even if the PIDs match */
    update_tracks(nav);
    return true;
}

/* ---------- the stream ---------- */

/* Waits a little for time to pass or the user to act (lock held). */
static void wait_a_little(nav_impl_t *nav, nav_stream_t *s)
{
    unsigned gen = nav->gen;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += IDLE_WAIT_MS * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    while (gen == nav->gen && !s->cancel && pthread_cond_timedwait(&nav->wake, &nav->lock, &ts) != ETIMEDOUT)
        ;
}

/* Without menus, the current playlist is done: on to the next, or stop
 * after the last. True if it ends the segment now (the next one's start
 * ends it when the reader gets there). */
static bool queue_next(nav_impl_t *nav)
{
    if (nav->queue_pos + 1 < nav->queue_len && start_playlist(nav, nav->queue_pos + 1, 0))
        return false;
    logf_(nav, 0, "end of the queue");
    nav->stopped = true;
    emit(nav, DISCNAV_EVENT_STOP);
    return true;
}

/* Handles one event; true if it ends the segment. */
static bool handle_event(nav_impl_t *nav, nav_stream_t *s, const BD_EVENT *ev, bool *wait)
{
    switch (ev->event) {
    case BD_EVENT_ERROR:
        logf_(nav, 0, "error %u%s", ev->param, ev->param == BD_ERROR_BDJ ? " (BD-J)" : "");
        nav->stopped = true;
        emit(nav, DISCNAV_EVENT_STOP);
        return true;
    case BD_EVENT_ENCRYPTED:
        logf_(nav, 0, "can't decrypt the disc (%u)", ev->param);
        nav->stopped = true;
        emit(nav, DISCNAV_EVENT_STOP);
        return true;
    case BD_EVENT_READ_ERROR:
        logf_(nav, 0, "read error");
        return false;
    case BD_EVENT_TITLE:
        if (nav->queue)
            return false; /* the queue numbers its titles (start_playlist) */
        logf_(nav, 1, "title %u", ev->param);
        nav->title_num = ev->param;
        update_tracks(nav);
        return false;
    case BD_EVENT_PLAYLIST:
        if (nav->queue)
            return false; /* start_playlist() chose it and ends the segment */
        logf_(nav, 1, "playlist %05u.mpls%s", ev->param, s->got_data ? ": end of segment" : "");
        load_playlist(nav, (int)ev->param);
        update_tracks(nav);
        return true;
    case BD_EVENT_PLAYITEM:
        nav->playitem = (int)ev->param;
        update_tracks(nav);
        return false;
    case BD_EVENT_CHAPTER:
        nav->chapter = (int)ev->param;
        return false;
    case BD_EVENT_SEEK:
    case BD_EVENT_DISCONTINUITY:
    case BD_EVENT_PLAYLIST_STOP:
        logf_(nav, 1, "%s%s", ev->event == BD_EVENT_SEEK ? "seek" : ev->event == BD_EVENT_DISCONTINUITY
                                                           ? "discontinuity" : "playlist stopped",
              s->got_data ? ": end of segment" : "");
        return true;
    case BD_EVENT_END_OF_TITLE:
        if (nav->queue)
            return queue_next(nav);
        /* repeated on every read until the disc moves on (BD-J decides) */
        if (!nav->title_ended)
            logf_(nav, 1, "end of title");
        nav->title_ended = true;
        *wait = true;
        return false;
    case BD_EVENT_STILL:
        logf_(nav, 1, "still %s", ev->param ? "on" : "off");
        return false;
    case BD_EVENT_STILL_TIME:
        /* repeated on every read while the still lasts; without menus
         * nothing would end an infinite one (bd_read_skip_still() only
         * ends timed ones): the playlist is done */
        if (!ev->param && nav->queue) {
            logf_(nav, 1, "still without end: next playlist");
            return queue_next(nav);
        }
        if (ev->param && !nav->still_until)
            nav->still_until = now_ms() + ev->param * 1000u;
        if (nav->still_until && now_ms() >= nav->still_until) {
            nav->still_until = 0;
            bd_read_skip_still(nav->bd);
        } else {
            *wait = true;
        }
        return false;
    case BD_EVENT_IDLE:
        *wait = true;
        return false;
    case BD_EVENT_AUDIO_STREAM:
        nav->audio_num = ev->param == 0xff ? 0 : (int)ev->param;
        update_tracks(nav);
        return false;
    case BD_EVENT_PG_TEXTST_STREAM:
        nav->pg_num = ev->param == 0xfff ? 0 : (int)ev->param;
        update_tracks(nav);
        return false;
    case BD_EVENT_PG_TEXTST:
        nav->pg_on = ev->param != 0;
        nav->want_sid = -2; /* forced-only changed: tell the player */
        update_tracks(nav);
        return false;
    case BD_EVENT_MENU:
        nav->menu = ev->param != 0;
        update_tracks(nav);
        return false;
    case BD_EVENT_POPUP:
        logf_(nav, 1, "pop-up menu %savailable", ev->param ? "" : "not ");
        return false;
    case BD_EVENT_UO_MASK_CHANGED:
    case BD_EVENT_KEY_INTEREST_TABLE:
    case BD_EVENT_SOUND_EFFECT:
    case BD_EVENT_ANGLE:
    default:
        return false;
    }
}

/* The next unit: 1 = data in s->buf, 0 = end of segment, -1 = error/cancel. */
static int next_unit(nav_stream_t *s)
{
    nav_impl_t *nav = s->nav;
    int ret;
    pthread_mutex_lock(&nav->lock);
    for (;;) {
        if (s->cancel) { ret = -1; break; }
        if (s->eof) { ret = 0; break; }
        if (nav->restart && s->got_data) {
            /* the next playlist of the queue starts in a segment of its own */
            if (nav->user_jump)
                emit(nav, DISCNAV_EVENT_FLUSH);
            nav->user_jump = false;
            nav->restart = false;
            s->eof = true;
            ret = 0;
            break;
        }
        nav->restart = false;
        BD_EVENT ev;
        int n = bd_read_ext(nav->bd, s->buf, UNIT, &ev);
        bool end = false, wait = n == 0 && ev.event == BD_EVENT_NONE; /* nothing happened: don't spin */
        while (ev.event != BD_EVENT_NONE) {
            /* a break before any data just starts this segment there */
            if (handle_event(nav, s, &ev, &wait) && s->got_data)
                end = true;
            if (!bd_get_event(nav->bd, &ev))
                break;
        }
        if (nav->stopped) {
            s->eof = true;
            ret = 0;
            break;
        }
        if (end) {
            if (nav->user_jump)
                emit(nav, DISCNAV_EVENT_FLUSH);
            nav->user_jump = false;
            s->eof = true;
            ret = 0;
            break;
        }
        if (n < 0) {
            logf_(nav, 0, "reading failed");
            ret = -1;
            break;
        }
        if (n > 0) {
            nav->still_until = 0;
            nav->title_ended = false;
            s->got_data = true;
            s->len = n;
            ret = 1;
            break;
        }
        if (wait)
            wait_a_little(nav, s);
    }
    pthread_mutex_unlock(&nav->lock);
    return ret;
}

static nav_stream_t *bdn_stream_open(nav_impl_t *nav)
{
    nav_stream_t *s = calloc(1, sizeof(*s));
    if (s)
        s->nav = nav;
    return s;
}

static int64_t bdn_stream_read(nav_stream_t *s, void *buf, uint64_t n)
{
    if (s->pos >= s->len) {
        int r = next_unit(s);
        if (r <= 0)
            return r;
        s->pos = 0;
    }
    uint64_t avail = (uint64_t)(s->len - s->pos);
    if (n > avail)
        n = avail;
    memcpy(buf, s->buf + s->pos, n);
    s->pos += (int)n;
    return (int64_t)n;
}

static void bdn_stream_cancel(nav_stream_t *s)
{
    nav_impl_t *nav = s->nav;
    pthread_mutex_lock(&nav->lock);
    s->cancel = true;
    pthread_cond_broadcast(&nav->wake);
    pthread_mutex_unlock(&nav->lock);
}

static void bdn_stream_close(nav_stream_t *s)
{
    free(s);
}

static void bdn_segment_opened(nav_impl_t *nav)
{
}

static bool bdn_stopped(nav_impl_t *nav)
{
    pthread_mutex_lock(&nav->lock);
    bool stopped = nav->stopped;
    pthread_mutex_unlock(&nav->lock);
    return stopped;
}

/* ---------- input and state ---------- */

static bool key(nav_impl_t *nav, uint32_t k)
{
    /* BD-J applications listen for presses and releases, HDMV for the key */
    return bd_user_input(nav->bd, -1, k | BD_VK_KEY_PRESSED) >= 0 &&
           bd_user_input(nav->bd, -1, k | BD_VK_KEY_TYPED) >= 0 &&
           bd_user_input(nav->bd, -1, k | BD_VK_KEY_RELEASED) >= 0;
}

static bool bdn_action(nav_impl_t *nav, discnav_action_t a)
{
    static const char *const names[] = { "up", "down", "left", "right", "activate", "root menu",
                                         "title menu", "go up", "previous chapter", "next chapter", "pop-up menu" };
    pthread_mutex_lock(&nav->lock);
    bool ok = false;
    switch (a) {
    case DISCNAV_UP:          ok = key(nav, BD_VK_UP); break;
    case DISCNAV_DOWN:        ok = key(nav, BD_VK_DOWN); break;
    case DISCNAV_LEFT:        ok = key(nav, BD_VK_LEFT); break;
    case DISCNAV_RIGHT:       ok = key(nav, BD_VK_RIGHT); break;
    case DISCNAV_ACTIVATE:    ok = key(nav, BD_VK_ENTER); break;
    case DISCNAV_ROOT_MENU:
    case DISCNAV_TITLE_MENU:  ok = key(nav, BD_VK_ROOT_MENU); break;
    case DISCNAV_POPUP_MENU:  ok = key(nav, BD_VK_POPUP); break;
    case DISCNAV_GO_UP:
        /* Blu-ray has no "go up": over a title it closes the pop-up menu */
        ok = nav->title_num != 0 && nav->title_num != 0xffff && graphics_shown(nav) && key(nav, BD_VK_POPUP);
        break;
    case DISCNAV_PREV_CHAPTER:
        /* without menus, past the first chapter goes to the previous title */
        if (nav->chapter > 1)
            ok = bd_seek_chapter(nav->bd, (unsigned)nav->chapter - 2) >= 0;
        else if (nav->queue && nav->queue_pos > 0)
            ok = start_playlist(nav, nav->queue_pos - 1, 0);
        break;
    case DISCNAV_NEXT_CHAPTER:
        if (nav->info && (unsigned)nav->chapter < nav->info->chapter_count)
            ok = bd_seek_chapter(nav->bd, (unsigned)nav->chapter) >= 0;
        else if (nav->queue && nav->queue_pos + 1 < nav->queue_len)
            ok = start_playlist(nav, nav->queue_pos + 1, 0);
        break;
    }
    logf_(nav, 1, "user: %s%s", (unsigned)a < sizeof(names) / sizeof(names[0]) ? names[a] : "?",
          ok ? "" : " (refused)");
    if (ok && a >= DISCNAV_ACTIVATE)
        nav->user_jump = true;
    nav->gen++;
    pthread_cond_broadcast(&nav->wake);
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

static void bdn_get_tracks(nav_impl_t *nav, discnav_tracks_t *t)
{
    pthread_mutex_lock(&nav->lock);
    t->audio_id = nav->want_aid;
    t->sub_id = nav->want_sid;
    t->sub_forced_only = !nav->pg_on;
    update_tracks(nav); /* in case the graphics changed since */
    t->in_menu = nav->in_menu;
    pthread_mutex_unlock(&nav->lock);
}

static const char *audio_format(uint8_t coding)
{
    switch (coding) {
    case BLURAY_STREAM_TYPE_AUDIO_LPCM:      return "lpcm";
    case BLURAY_STREAM_TYPE_AUDIO_AC3:       return "ac3";
    case BLURAY_STREAM_TYPE_AUDIO_DTS:       return "dts";
    case BLURAY_STREAM_TYPE_AUDIO_TRUHD:     return "truehd";
    case BLURAY_STREAM_TYPE_AUDIO_AC3PLUS:
    case BLURAY_STREAM_TYPE_AUDIO_AC3PLUS_SECONDARY: return "eac3";
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD:
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD_MASTER:
    case BLURAY_STREAM_TYPE_AUDIO_DTSHD_SECONDARY: return "dtshd";
    case BLURAY_STREAM_TYPE_AUDIO_MPEG1:
    case BLURAY_STREAM_TYPE_AUDIO_MPEG2:     return "mpeg";
    }
    return "";
}

static int bdn_get_streams(nav_impl_t *nav, discnav_stream_type_t type, discnav_stream_info_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&nav->lock);
    const BLURAY_CLIP_INFO *c = menu_now(nav) ? NULL : clip(nav);
    int count = !c ? 0 : type == DISCNAV_AUDIO ? c->audio_stream_count : c->pg_stream_count;
    for (int i = 0; i < count && n < max; i++) {
        const BLURAY_STREAM_INFO *si = type == DISCNAV_AUDIO ? &c->audio_streams[i] : &c->pg_streams[i];
        discnav_stream_info_t *st = &out[n++];
        memset(st, 0, sizeof(*st));
        st->index = i + 1;
        st->id = si->pid;
        memcpy(st->lang, si->lang, 3);
        st->kind = DISCNAV_KIND_NORMAL;
        if (type == DISCNAV_AUDIO) {
            st->format = audio_format(si->coding_type);
            st->channels = si->format == BLURAY_AUDIO_FORMAT_MONO ? 1 : si->format == BLURAY_AUDIO_FORMAT_STEREO ? 2 : 0;
            st->selected = i + 1 == nav->audio_num;
        } else {
            st->format = "";
            st->selected = nav->pg_on && i + 1 == nav->pg_num;
        }
    }
    pthread_mutex_unlock(&nav->lock);
    return n;
}

static bool bdn_select_stream(nav_impl_t *nav, discnav_stream_type_t type, int index)
{
    pthread_mutex_lock(&nav->lock);
    bool ok = !menu_now(nav);
    if (ok && type == DISCNAV_AUDIO) {
        bd_select_stream(nav->bd, BLURAY_AUDIO_STREAM, (uint32_t)index, 1);
        nav->audio_num = index;
    } else if (ok && index < 0) {
        bd_select_stream(nav->bd, BLURAY_PG_TEXTST_STREAM, (uint32_t)(nav->pg_num > 0 ? nav->pg_num : 1), 0);
        nav->pg_on = false;
    } else if (ok) {
        bd_select_stream(nav->bd, BLURAY_PG_TEXTST_STREAM, (uint32_t)index, 1);
        nav->pg_num = index;
        nav->pg_on = true;
    }
    logf_(nav, 0, "user: %s stream %d%s", type == DISCNAV_AUDIO ? "audio" : "subtitle", index, ok ? "" : " (refused)");
    if (ok) {
        nav->want_sid = -2; /* forced-only may have changed */
        update_tracks(nav);
    }
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

static bool bdn_get_position(nav_impl_t *nav, int *title, double *seconds, double *length)
{
    pthread_mutex_lock(&nav->lock);
    bool ok = !menu_now(nav) && nav->info;
    if (ok) {
        *title = (int)nav->title_num;
        *seconds = bd_tell_time(nav->bd) / 90000.0;
        if (length)
            *length = nav->info->duration / 90000.0;
    }
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

static int bdn_get_playlist(nav_impl_t *nav)
{
    pthread_mutex_lock(&nav->lock);
    int playlist = !menu_now(nav) && nav->info ? nav->playlist : -1;
    pthread_mutex_unlock(&nav->lock);
    return playlist;
}

static bool bdn_seek(nav_impl_t *nav, double seconds)
{
    pthread_mutex_lock(&nav->lock);
    bool ok = !menu_now(nav) && nav->info;
    if (ok) {
        double length = nav->info->duration / 90000.0;
        if (length > 1 && seconds > length - 1)
            seconds = length - 1;
        if (seconds < 0)
            seconds = 0;
        ok = bd_seek_time(nav->bd, (uint64_t)(seconds * 90000)) >= 0;
    }
    logf_(nav, 1, "user: seek to %.1f s%s", seconds, ok ? "" : " (refused)");
    if (ok) {
        nav->user_jump = true;
        nav->gen++;
        pthread_cond_broadcast(&nav->wake);
    }
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

static bool bdn_get_palette(nav_impl_t *nav, uint32_t rgb[16])
{
    return false;
}

static bool bdn_overlay_size(nav_impl_t *nav, int *w, int *h)
{
    pthread_mutex_lock(&nav->olock);
    *w = nav->pw ? nav->pw : 1920;
    *h = nav->ph ? nav->ph : 1080;
    pthread_mutex_unlock(&nav->olock);
    return true;
}

/* source-over with straight alpha, a over d */
static inline void blend(uint8_t *d, uint32_t a)
{
    unsigned sa = a >> 24;
    if (!sa)
        return;
    unsigned sr = a >> 16 & 0xff, sg = a >> 8 & 0xff, sb = a & 0xff;
    if (sa == 255 || !d[3]) {
        d[0] = sr, d[1] = sg, d[2] = sb, d[3] = sa;
        return;
    }
    unsigned da = d[3] * (255 - sa) / 255, oa = sa + da;
    d[0] = (sr * sa + d[0] * da) / oa;
    d[1] = (sg * sa + d[1] * da) / oa;
    d[2] = (sb * sa + d[2] * da) / oa;
    d[3] = oa;
}

static bool bdn_render_overlay(nav_impl_t *nav, uint8_t *rgba, int w, int h)
{
    memset(rgba, 0, (size_t)w * h * 4);
    pthread_mutex_lock(&nav->olock);
    bool any = false;
    if (w == nav->pw && h == nav->ph) {
        for (int p = 0; p < PLANES; p++) {
            const uint32_t *src = nav->front[p];
            if (!src)
                continue;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (src[i] >> 24) {
                    blend(rgba + i * 4, src[i]);
                    any = true;
                }
            }
        }
    }
    pthread_mutex_unlock(&nav->olock);
    return any;
}

/* ---------- open/close ---------- */

/* ISO 639-1 to the ISO 639-2 codes libbluray wants */
static const char *lang3(const char *lang)
{
    static const char *const map[][2] = {
        { "en", "eng" }, { "fr", "fra" }, { "de", "deu" }, { "es", "spa" }, { "it", "ita" }, { "nl", "nld" },
        { "pt", "por" }, { "sv", "swe" }, { "da", "dan" }, { "fi", "fin" }, { "no", "nor" }, { "ru", "rus" },
        { "pl", "pol" }, { "ja", "jpn" }, { "ko", "kor" }, { "zh", "zho" },
    };
    for (size_t i = 0; lang && i < sizeof(map) / sizeof(map[0]); i++)
        if (!strncmp(lang, map[i][0], 2))
            return map[i][1];
    return "eng";
}

static void bdn_close(nav_impl_t *nav);

/* Opens the disc and sets the player up; menus: its menus will run. */
static nav_impl_t *open_disc(const char *device, const char *lang, bool menus,
                             discnav_event_fn on_event, discnav_log_fn on_log, void *user)
{
    nav_impl_t *nav = calloc(1, sizeof(*nav));
    if (!nav)
        return NULL;
    nav->on_event = on_event;
    nav->on_log = on_log;
    nav->user = user;
    nav->want_aid = nav->want_sid = -1;
    nav->title_num = 0xffff;
    nav->in_menu = true;
    nav->playitem = -1;
    pthread_mutex_init(&nav->lock, NULL);
    pthread_mutex_init(&nav->olock, NULL);
    pthread_cond_init(&nav->wake, NULL);

    nav->bd = bd_open(device, NULL);
    const BLURAY_DISC_INFO *di = nav->bd ? bd_get_disc_info(nav->bd) : NULL;
    if (!di || !di->bluray_detected) {
        logf_(nav, 0, "can't open %s as a Blu-ray", device);
        bdn_close(nav);
        return NULL;
    }
    if (di->aacs_detected && !di->aacs_handled) {
        logf_(nav, 0, "can't decrypt %s (AACS error %d)", device, di->aacs_error_code);
        bdn_close(nav);
        return NULL;
    }
    if (menus && di->bdj_detected && !di->bdj_handled) {
        logf_(nav, 0, "%s has BD-J menus, but %s", device,
              di->libjvm_detected ? "libbluray's BD-J jar is missing" : "there is no usable Java (21)");
        bdn_close(nav);
        return NULL;
    }
    nav->bdj = menus && di->bdj_detected;
    const char *l = lang3(lang);
    bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_MENU_LANG, l);
    bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, l);
    bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_PG_LANG, l);
    bd_set_player_setting_str(nav->bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, "us");
    bd_set_player_setting(nav->bd, BLURAY_PLAYER_SETTING_REGION_CODE, 1); /* A */
    bd_register_overlay_proc(nav->bd, nav, yuv_overlay);
    bd_register_argb_overlay_proc(nav->bd, nav, argb_overlay, &nav->argb);

    const char *name = di->disc_name ? di->disc_name : di->udf_volume_id;
    snprintf(nav->title, sizeof(nav->title), "%s", name ? name : "");
    logf_(nav, 0, "opened %s \"%s\" (%s%s)", device, nav->title,
          !menus ? "no menus" : di->bdj_detected ? "BD-J menus" : "HDMV menus", di->aacs_detected ? ", AACS" : "");
    return nav;
}

nav_impl_t *bd_nav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user)
{
    nav_impl_t *nav = open_disc(device, lang, true, on_event, on_log, user);
    if (nav && !bd_play(nav->bd)) {
        logf_(nav, 0, "the disc doesn't start");
        bdn_close(nav);
        return NULL;
    }
    return nav;
}

nav_impl_t *bd_playlists_open(const char *device, const char *lang, const int *playlists, int count,
                              int first, double seconds,
                              discnav_event_fn on_event, discnav_log_fn on_log, void *user)
{
    if (count < 1 || first < 0 || first >= count)
        return NULL;
    nav_impl_t *nav = open_disc(device, lang, false, on_event, on_log, user);
    if (!nav)
        return NULL;
    nav->in_menu = false;
    nav->queue = malloc(sizeof(int) * count);
    if (nav->queue) {
        memcpy(nav->queue, playlists, sizeof(int) * count);
        nav->queue_len = count;
    }
    bd_get_event(nav->bd, NULL); /* events (streams, chapters) without bd_play() */
    if (!nav->queue || !start_playlist(nav, first, seconds)) {
        bdn_close(nav);
        return NULL;
    }
    return nav;
}

static void bdn_close(nav_impl_t *nav)
{
    if (!nav)
        return;
    if (nav->bd)
        bd_close(nav->bd);
    if (nav->info)
        bd_free_title_info(nav->info);
    free(nav->queue);
    for (int p = 0; p < PLANES; p++) {
        free(nav->argb.buf[p]);
        free(nav->front[p]);
        free(nav->hdmv[p]);
    }
    pthread_cond_destroy(&nav->wake);
    pthread_mutex_destroy(&nav->olock);
    pthread_mutex_destroy(&nav->lock);
    free(nav);
}

static const char *bdn_title(nav_impl_t *nav)
{
    return nav->title;
}

const nav_backend_t bd_backend = {
    .format = DISCNAV_BLURAY,
    .close = bdn_close,
    .title = bdn_title,
    .stream_open = bdn_stream_open,
    .stream_read = bdn_stream_read,
    .stream_cancel = bdn_stream_cancel,
    .stream_close = bdn_stream_close,
    .segment_opened = bdn_segment_opened,
    .stopped = bdn_stopped,
    .action = bdn_action,
    .get_tracks = bdn_get_tracks,
    .get_streams = bdn_get_streams,
    .select_stream = bdn_select_stream,
    .get_position = bdn_get_position,
    .get_playlist = bdn_get_playlist,
    .seek = bdn_seek,
    .get_palette = bdn_get_palette,
    .overlay_size = bdn_overlay_size,
    .render_overlay = bdn_render_overlay,
};
