/*
 * libdiscnav: DVD navigation (see discnav.h).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Grown out of spike 0b (docs/spike-results.md); the numbered lessons there
 * are why things are done the way they are here:
 * - A segment ends on HOP_CHANNEL, VTS_CHANGE or a VOBU PTM discontinuity
 *   (a looping menu's "jump cell 1" emits no HOP); seamless cell changes
 *   (chapters) don't end one.
 * - User actions that jump ask the player to flush (DISCNAV_EVENT_FLUSH);
 *   natural jumps let the buffered data play out.
 * - Still frames: a sequence_end_code makes the decoder output the held
 *   picture. MPEG-PS has no header, so the demuxer only stops probing once
 *   the *packets* it read reach its probe size: before the player has
 *   opened the segment, the last NAV pack is repeated (a real packet),
 *   then padding, then the read blocks until the still ends.
 * - Track selection follows the VM: stream changes give physical stream
 *   numbers, which map to MPEG-PS stream ids.
 * - MPEG-PS streams only appear with their first packet, and a player
 *   drops packets of streams it hasn't selected yet: each title segment
 *   starts with an empty subpicture for the VM's subtitle stream, so the
 *   player can select it before real ones arrive.
 */
#define _GNU_SOURCE
#include "discnav.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dvdnav/dvdnav.h>

#include "spu.h"

#define BLOCK 2048
#define STILL_PAD_LIMIT (512 * 1024)
#define STILL_PAD_PROBE_CAP (8 * 1024 * 1024)

struct discnav {
    discnav_event_fn on_event;
    discnav_log_fn on_log;
    void *user;

    pthread_mutex_t lock;   /* guards dvd and the state below */
    pthread_cond_t wake;
    dvdnav_t *dvd;
    char title[64];
    unsigned gen;           /* bumped on every user action */
    bool stopped;
    bool user_jump;         /* a user action may jump: flush on the hop */
    bool opened;            /* the player has opened the current segment */
    bool have_ptm;
    uint32_t last_e_ptm;    /* end PTM of the previous VOBU */
    uint8_t last_pack[14];  /* latest pack header, for injected blocks */
    uint8_t last_nav[BLOCK];/* latest NAV pack (PCI+DSI) */
    bool have_nav;
    bool nav_pending;       /* last_nav opens the next segment */
    int want_aid;
    int want_sid;
    bool sub_forced_only;
    bool in_menu;
    spu_assembler_t spu_asm;

    pthread_mutex_t olock;  /* guards the overlay state; taken after lock */
    spu_image_t spu;
    spu_highlight_t hl;
    uint32_t rgb_clut[16];
    bool have_clut;
    int cur_button;
    int length_title;       /* the title length_secs is for */
    double length_secs;

    /* The disc's main menus, for discs that leave "go up" undefined: the
     * menu cells reached first or by a menu call, and those they lead to
     * by themselves (an intro into its loop). Keys from menu_key(). */
    int vts;                /* the current title set */
    int menu_key;           /* the current menu cell, -1 outside menus */
    int root_keys[16];
    int n_root;
    bool root_pending;      /* the next menu cell is a main menu */
    bool user_moved;        /* a user action since the last cell started */
    bool cell_started;      /* track_menu on the next NAV pack */
};

struct discnav_stream {
    discnav_t *nav;
    uint8_t blk[BLOCK];
    int blk_pos, blk_len;
    bool cancel, eof, got_data;
    bool primed;            /* the subtitle stream was announced (see top) */
    uint8_t held[BLOCK];    /* a disc block held back behind the announcement */
    bool have_held;
    bool in_still;
    int still_len;          /* seconds; 0xff = infinite */
    uint64_t still_start;
    size_t still_pad;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(discnav_t *nav, int level, const char *fmt, ...)
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

static void emit(discnav_t *nav, discnav_event_t ev)
{
    if (nav->on_event)
        nav->on_event(nav->user, ev);
}

/* ---------- overlay state (called with lock held) ---------- */

static void update_overlay(discnav_t *nav)
{
    pci_t *pci = dvdnav_get_current_nav_pci(nav->dvd);
    int32_t cur = 0;
    dvdnav_get_current_highlight(nav->dvd, &cur);
    bool buttons = pci && (pci->hli.hl_gi.hli_ss & 3) && (pci->hli.hl_gi.btn_ns & 0x3f);

    spu_highlight_t hl = { 0 };
    dvdnav_highlight_area_t area;
    if (buttons && cur > 0 && dvdnav_get_highlight_area(pci, cur, 0, &area) == DVDNAV_STATUS_OK)
        hl = (spu_highlight_t){ true, area.sx, area.sy, area.ex, area.ey, area.palette };

    pthread_mutex_lock(&nav->olock);
    bool moved = nav->cur_button != cur;
    bool changed = memcmp(&hl, &nav->hl, sizeof(hl)) != 0;
    nav->cur_button = cur;
    nav->hl = hl;
    pthread_mutex_unlock(&nav->olock);

    if (moved)
        logf_(nav, 1, "highlight -> button %d", cur);
    if (changed)
        emit(nav, DISCNAV_EVENT_OVERLAY);
}

static void clear_spu(discnav_t *nav)
{
    pthread_mutex_lock(&nav->olock);
    bool had = nav->spu.pix != NULL;
    spu_image_free(&nav->spu);
    pthread_mutex_unlock(&nav->olock);
    spu_asm_reset(&nav->spu_asm);
    if (had)
        emit(nav, DISCNAV_EVENT_OVERLAY);
}

/* Pulls the active subpicture stream out of a block and decodes complete
 * units (menus only; titles leave subtitles to the player). */
static void scan_spu(discnav_t *nav, const uint8_t *b)
{
    if (nav->want_sid < 0)
        return;
    int i = 14 + (b[13] & 7);
    while (i + 9 <= BLOCK && !b[i] && !b[i + 1] && b[i + 2] == 1) {
        int end = i + 6 + ((b[i + 4] << 8) | b[i + 5]);
        if (end > BLOCK)
            break;
        if (b[i + 3] == 0xbd) {             /* private stream 1 */
            int p = i + 9 + b[i + 8];       /* skip the MPEG-2 PES header */
            if (p < end && b[p] == nav->want_sid &&
                spu_asm_feed(&nav->spu_asm, b + p + 1, end - p - 1)) {
                spu_image_t img;
                if (spu_decode(nav->spu_asm.buf, nav->spu_asm.size, &img)) {
                    pthread_mutex_lock(&nav->olock);
                    spu_image_free(&nav->spu);
                    nav->spu = img;
                    pthread_mutex_unlock(&nav->olock);
                    logf_(nav, 1, "menu subpicture %dx%d at %d,%d", img.w, img.h, img.x, img.y);
                    emit(nav, DISCNAV_EVENT_OVERLAY);
                }
            }
        }
        i = end;
    }
}

/* After anything that may change the domain. */
static void update_domain(discnav_t *nav)
{
    bool menu = !dvdnav_is_domain_vts(nav->dvd);
    if (menu == nav->in_menu)
        return;
    nav->in_menu = menu;
    logf_(nav, 0, "%s", menu ? "menu" : "title");
    if (!menu)
        clear_spu(nav);
    emit(nav, DISCNAV_EVENT_TRACKS);
}

/* The menu cell playing, from its NAV pack's VOB and cell ids (libdvdnav
 * can't name a PGC before a title set is open); -1 outside menus */
static int menu_key(discnav_t *nav)
{
    dsi_t *dsi = dvdnav_get_current_nav_dsi(nav->dvd);
    if (!nav->in_menu || !dsi)
        return -1;
    return (dvdnav_is_domain_vmgm(nav->dvd) ? 0 : nav->vts) << 24 | dsi->dsi_gi.vobu_vob_idn << 8 |
           dsi->dsi_gi.vobu_c_idn;
}

static bool is_root(discnav_t *nav, int key)
{
    for (int i = 0; i < nav->n_root; i++)
        if (nav->root_keys[i] == key)
            return true;
    return false;
}

/* On the first NAV pack of each cell: follow which menu we're in (see
 * root_keys). */
static void track_menu(discnav_t *nav)
{
    int key = menu_key(nav);
    if (key >= 0 && key != nav->menu_key && !is_root(nav, key) &&
        (nav->root_pending || (!nav->user_moved && is_root(nav, nav->menu_key))) &&
        nav->n_root < (int)(sizeof(nav->root_keys) / sizeof(nav->root_keys[0]))) {
        nav->root_keys[nav->n_root++] = key;
        logf_(nav, 1, "main menu: vob %d cell %d of %s", key >> 8 & 0xffff, key & 0xff,
              key >> 24 ? "a title set" : "the disc");
    }
    if (key >= 0)
        nav->root_pending = false;
    nav->menu_key = key;
    nav->user_moved = false;
}

/* ---------- synthetic MPEG-PS blocks ---------- */

static void put_pack_header(discnav_t *nav, uint8_t *b)
{
    memcpy(b, nav->last_pack, 14);
    if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xba) {
        /* no real pack seen yet: minimal MPEG-2 pack header, SCR 0 */
        static const uint8_t hdr[14] = { 0, 0, 1, 0xba, 0x44, 0, 4, 0, 4, 1,
                                         0x01, 0x89, 0xc3, 0xf8 };
        memcpy(b, hdr, 14);
    }
    b[13] = 0xf8; /* no stuffing */
}

static void put_padding(uint8_t *b, int off)
{
    int len = BLOCK - off - 6;
    b[off] = 0; b[off + 1] = 0; b[off + 2] = 1; b[off + 3] = 0xbe;
    b[off + 4] = len >> 8; b[off + 5] = len & 0xff;
    memset(b + off + 6, 0xff, len);
}

static void make_padding_block(discnav_t *nav, uint8_t *b)
{
    put_pack_header(nav, b);
    put_padding(b, 14);
}

/* Private stream 1 PES carrying an empty subpicture (stop display, no
 * pixels) on stream `id`: makes the stream known without showing anything. */
static void make_spu_announce_block(discnav_t *nav, uint8_t *b, int id)
{
    static const uint8_t spu[] = { 0, 10, 0, 4,          /* size, control offset */
                                   0, 0, 0, 4, 0x02, 0xff }; /* delay 0, next = self, STP_DSP, end */
    uint8_t pes[9 + 1 + sizeof(spu)] = { 0, 0, 1, 0xbd, 0, 3 + 1 + sizeof(spu), 0x81, 0, 0 };
    pes[9] = (uint8_t)id;
    memcpy(pes + 10, spu, sizeof(spu));
    put_pack_header(nav, b);
    memcpy(b + 14, pes, sizeof(pes));
    put_padding(b, 14 + sizeof(pes));
}

/* A video PES with a lone sequence_end_code: makes the decoder emit the
 * picture it holds back, so a still menu actually appears. */
static void make_seq_end_block(discnav_t *nav, uint8_t *b)
{
    static const uint8_t pes[] = { 0, 0, 1, 0xe0, 0, 7, 0x80, 0, 0,
                                   0, 0, 1, 0xb7 };
    put_pack_header(nav, b);
    memcpy(b + 14, pes, sizeof(pes));
    put_padding(b, 14 + sizeof(pes));
}

static int audio_stream_id(discnav_t *nav, int logical, int physical)
{
    if (physical < 0)
        return -1;
    switch (dvdnav_audio_stream_format(nav->dvd, logical)) {
    case DVD_AUDIO_FORMAT_AC3:       return 0x80 + physical;
    case DVD_AUDIO_FORMAT_DTS:       return 0x88 + physical;
    case DVD_AUDIO_FORMAT_LPCM:      return 0xa0 + physical;
    case DVD_AUDIO_FORMAT_MPEG:
    case DVD_AUDIO_FORMAT_MPEG2_EXT: return 0x1c0 + physical;
    }
    return -1;
}

/* ---------- the stream ---------- */

/* The next block: 1 = data in s->blk, 0 = end of segment, -1 = error/cancel. */
static int next_block(discnav_stream_t *s)
{
    discnav_t *nav = s->nav;
    int ret;
    pthread_mutex_lock(&nav->lock);
    for (;;) {
        if (s->cancel) { ret = -1; break; }
        if (s->eof) { ret = 0; break; }
        if (nav->nav_pending) {
            nav->nav_pending = false;
            memcpy(s->blk, nav->last_nav, BLOCK);
            ret = 1;
            break;
        }
        if (s->have_held) {
            s->have_held = false;
            memcpy(s->blk, s->held, BLOCK);
            ret = 1;
            break;
        }

        if (s->in_still) {
            if (s->still_len != 0xff &&
                now_ms() - s->still_start >= (uint64_t)s->still_len * 1000) {
                logf_(nav, 1, "still timed out (%ds)", s->still_len);
                dvdnav_still_skip(nav->dvd);
                s->in_still = false;
                continue;
            }
            /* probe filler before the player has opened the segment; NAV
             * packs count as packets, padding doesn't */
            if (!nav->opened && nav->have_nav && s->still_pad < STILL_PAD_PROBE_CAP) {
                memcpy(s->blk, nav->last_nav, BLOCK);
                s->still_pad += BLOCK;
                ret = 1;
                break;
            }
            if (s->still_pad < STILL_PAD_LIMIT) {
                make_padding_block(nav, s->blk);
                s->still_pad += BLOCK;
                ret = 1;
                break;
            }
            unsigned gen = nav->gen;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            while (gen == nav->gen && !s->cancel &&
                   pthread_cond_timedwait(&nav->wake, &nav->lock, &ts) != ETIMEDOUT)
                ;
            /* then ask dvdnav whether the still is over */
        }

        int32_t ev, len;
        if (dvdnav_get_next_block(nav->dvd, s->blk, &ev, &len) != DVDNAV_STATUS_OK) {
            logf_(nav, 0, "reading failed: %s", dvdnav_err_to_string(nav->dvd));
            ret = -1;
            break;
        }

        if (ev != DVDNAV_STILL_FRAME && ev != DVDNAV_NOP)
            s->in_still = false;

        switch (ev) {
        case DVDNAV_BLOCK_OK:
            memcpy(nav->last_pack, s->blk, 14);
            s->got_data = true;
            if (nav->in_menu)
                scan_spu(nav, s->blk);
            if (!s->primed && !nav->in_menu && nav->want_sid >= 0) {
                memcpy(s->held, s->blk, BLOCK);
                s->have_held = true;
                make_spu_announce_block(nav, s->blk, nav->want_sid);
            }
            s->primed = true;
            ret = 1;
            goto out;
        case DVDNAV_NOP:
            continue;
        case DVDNAV_STILL_FRAME: {
            if (s->in_still)
                continue;
            dvdnav_still_event_t *e = (dvdnav_still_event_t *)s->blk;
            logf_(nav, 1, "still frame, %d%s", e->length, e->length == 0xff ? " (infinite)" : " s");
            s->in_still = true;
            s->still_len = e->length;
            s->still_start = now_ms();
            s->still_pad = BLOCK;
            make_seq_end_block(nav, s->blk);
            ret = 1;
            goto out;
        }
        case DVDNAV_WAIT:
            dvdnav_wait_skip(nav->dvd);
            continue;
        case DVDNAV_HOP_CHANNEL:
        case DVDNAV_VTS_CHANGE:
            if (ev == DVDNAV_VTS_CHANGE)
                nav->vts = ((dvdnav_vts_change_event_t *)s->blk)->new_vtsN;
            logf_(nav, 1, "%s%s%s", ev == DVDNAV_HOP_CHANNEL ? "hop channel" : "vts change",
                  s->got_data ? ": end of segment" : "",
                  s->got_data && nav->user_jump ? ", flush" : "");
            /* a jump leaves the old menu's graphics behind (a looping menu's
             * restart is no hop, so it keeps them) */
            clear_spu(nav);
            update_domain(nav);
            if (s->got_data) {
                if (nav->user_jump)
                    emit(nav, DISCNAV_EVENT_FLUSH);
                nav->user_jump = false;
                s->eof = true;
                ret = 0;
                goto out;
            }
            nav->user_jump = false;
            continue;
        case DVDNAV_CELL_CHANGE:
            update_domain(nav);
            nav->cell_started = true;
            continue;
        case DVDNAV_NAV_PACKET: {
            /* a VOBU whose start doesn't continue the previous one is a
             * timestamp discontinuity: end the segment for fresh timing */
            pci_t *pci = dvdnav_get_current_nav_pci(nav->dvd);
            uint32_t s_ptm = pci->pci_gi.vobu_s_ptm, e_ptm = pci->pci_gi.vobu_e_ptm;
            bool discont = nav->have_ptm && s_ptm != nav->last_e_ptm;
            nav->have_ptm = true;
            nav->last_e_ptm = e_ptm;
            memcpy(nav->last_nav, s->blk, BLOCK);
            nav->have_nav = true;
            if (nav->cell_started)
                track_menu(nav);
            nav->cell_started = false;
            update_overlay(nav);
            if (discont && s->got_data) {
                logf_(nav, 1, "timestamp discontinuity: end of segment");
                nav->nav_pending = true;
                s->eof = true;
                ret = 0;
                goto out;
            }
            /* passed through: a harmless data stream for the demuxer, and
             * still frames reuse them as probe filler */
            memcpy(nav->last_pack, s->blk, 14);
            ret = 1;
            goto out;
        }
        case DVDNAV_HIGHLIGHT:
            update_overlay(nav);
            continue;
        case DVDNAV_SPU_CLUT_CHANGE: {
            const uint32_t *clut = (const uint32_t *)s->blk;
            pthread_mutex_lock(&nav->olock);
            for (int i = 0; i < 16; i++)
                nav->rgb_clut[i] = spu_clut_to_rgb(clut[i]);
            nav->have_clut = true;
            pthread_mutex_unlock(&nav->olock);
            emit(nav, DISCNAV_EVENT_OVERLAY);
            emit(nav, DISCNAV_EVENT_TRACKS);
            continue;
        }
        case DVDNAV_AUDIO_STREAM_CHANGE: {
            dvdnav_audio_stream_change_event_t *e = (void *)s->blk;
            int id = audio_stream_id(nav, e->logical, e->physical);
            if (id != nav->want_aid) {
                logf_(nav, 0, "audio stream %d (id 0x%x)", e->physical, id);
                nav->want_aid = id;
                emit(nav, DISCNAV_EVENT_TRACKS);
            }
            continue;
        }
        case DVDNAV_SPU_STREAM_CHANGE: {
            dvdnav_spu_stream_change_event_t *e = (void *)s->blk;
            /* bit 7 set: subtitles off, only forced subpictures show */
            int phys = e->physical_wide;
            int id = phys >= 0 ? 0x20 + (phys & 0x1f) : -1;
            bool forced = phys < 0 || (phys & 0x80);
            if (id != nav->want_sid || forced != nav->sub_forced_only) {
                logf_(nav, 0, "subpicture stream 0x%x (id 0x%x)%s", phys, id, forced ? ", forced only" : "");
                nav->want_sid = id;
                nav->sub_forced_only = forced;
                emit(nav, DISCNAV_EVENT_TRACKS);
            }
            continue;
        }
        case DVDNAV_STOP:
            logf_(nav, 0, "stop");
            nav->stopped = true;
            s->eof = true;
            emit(nav, DISCNAV_EVENT_STOP);
            ret = 0;
            goto out;
        default:
            continue;
        }
    }
out:
    pthread_mutex_unlock(&nav->lock);
    return ret;
}

discnav_stream_t *discnav_stream_open(discnav_t *nav)
{
    discnav_stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->nav = nav;
    pthread_mutex_lock(&nav->lock);
    nav->opened = false;
    pthread_mutex_unlock(&nav->lock);
    return s;
}

int64_t discnav_stream_read(discnav_stream_t *s, void *buf, uint64_t n)
{
    if (s->blk_pos >= s->blk_len) {
        int r = next_block(s);
        if (r <= 0)
            return r;
        s->blk_pos = 0;
        s->blk_len = BLOCK;
    }
    uint64_t avail = (uint64_t)(s->blk_len - s->blk_pos);
    if (n > avail)
        n = avail;
    memcpy(buf, s->blk + s->blk_pos, n);
    s->blk_pos += (int)n;
    return (int64_t)n;
}

void discnav_stream_cancel(discnav_stream_t *s)
{
    discnav_t *nav = s->nav;
    pthread_mutex_lock(&nav->lock);
    s->cancel = true;
    pthread_cond_broadcast(&nav->wake);
    pthread_mutex_unlock(&nav->lock);
}

void discnav_stream_close(discnav_stream_t *s)
{
    free(s);
}

void discnav_segment_opened(discnav_t *nav)
{
    pthread_mutex_lock(&nav->lock);
    nav->opened = true;
    pthread_cond_broadcast(&nav->wake);
    pthread_mutex_unlock(&nav->lock);
}

bool discnav_stopped(discnav_t *nav)
{
    pthread_mutex_lock(&nav->lock);
    bool stopped = nav->stopped;
    pthread_mutex_unlock(&nav->lock);
    return stopped;
}

/* ---------- input and state ---------- */

bool discnav_action(discnav_t *nav, discnav_action_t a)
{
    static const char *const names[] = { "up", "down", "left", "right", "activate", "root menu",
                                         "title menu", "go up", "previous chapter", "next chapter" };
    pthread_mutex_lock(&nav->lock);
    pci_t *pci = dvdnav_get_current_nav_pci(nav->dvd);
    /* the selected button's action colours, taken before activating */
    spu_highlight_t activated = { 0 };
    int32_t button = 0;
    dvdnav_highlight_area_t area;
    if (a == DISCNAV_ACTIVATE && pci && dvdnav_get_current_highlight(nav->dvd, &button) == DVDNAV_STATUS_OK &&
        button > 0 && dvdnav_get_highlight_area(pci, button, 1, &area) == DVDNAV_STATUS_OK)
        activated = (spu_highlight_t){ true, area.sx, area.sy, area.ex, area.ey, area.palette };
    dvdnav_status_t st = DVDNAV_STATUS_ERR;
    int32_t title, part;
    switch (a) {
    case DISCNAV_UP:           st = dvdnav_upper_button_select(nav->dvd, pci); break;
    case DISCNAV_DOWN:         st = dvdnav_lower_button_select(nav->dvd, pci); break;
    case DISCNAV_LEFT:         st = dvdnav_left_button_select(nav->dvd, pci); break;
    case DISCNAV_RIGHT:        st = dvdnav_right_button_select(nav->dvd, pci); break;
    case DISCNAV_ACTIVATE:     st = dvdnav_button_activate(nav->dvd, pci); break;
    case DISCNAV_ROOT_MENU:    st = dvdnav_menu_call(nav->dvd, DVD_MENU_Root); break;
    case DISCNAV_TITLE_MENU:   st = dvdnav_menu_call(nav->dvd, DVD_MENU_Title); break;
    case DISCNAV_GO_UP:
        st = dvdnav_go_up(nav->dvd);
        /* many discs leave it undefined and rely on a "main menu" button:
         * from a title set's other menus, go to its root menu */
        if (st != DVDNAV_STATUS_OK && nav->in_menu && !dvdnav_is_domain_vmgm(nav->dvd) &&
            nav->menu_key >= 0 && !is_root(nav, nav->menu_key) &&
            !(dvdnav_current_title_info(nav->dvd, &title, &part) == DVDNAV_STATUS_OK && part == DVD_MENU_Root) &&
            dvdnav_menu_call(nav->dvd, DVD_MENU_Root) == DVDNAV_STATUS_OK) {
            logf_(nav, 1, "go up: not on this disc, the main menu instead");
            st = DVDNAV_STATUS_OK;
            nav->root_pending = true;
        }
        break;
    case DISCNAV_PREV_CHAPTER: st = dvdnav_prev_pg_search(nav->dvd); break;
    case DISCNAV_NEXT_CHAPTER: st = dvdnav_next_pg_search(nav->dvd); break;
    }
    bool ok = st == DVDNAV_STATUS_OK;
    logf_(nav, 1, "user: %s%s", (unsigned)a < sizeof(names) / sizeof(names[0]) ? names[a] : "?",
          ok ? "" : " (refused)");
    if (a >= DISCNAV_ACTIVATE && ok) {
        nav->user_jump = true;
        nav->user_moved = true;
        if (a == DISCNAV_ROOT_MENU || a == DISCNAV_TITLE_MENU)
            nav->root_pending = true;
    }
    if (a == DISCNAV_ACTIVATE && ok && activated.valid) {
        /* show the button in its action colours until the jump clears it */
        pthread_mutex_lock(&nav->olock);
        nav->hl = activated;
        pthread_mutex_unlock(&nav->olock);
        emit(nav, DISCNAV_EVENT_OVERLAY);
    } else {
        update_overlay(nav);
    }
    nav->gen++;
    pthread_cond_broadcast(&nav->wake);
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

void discnav_get_tracks(discnav_t *nav, discnav_tracks_t *t)
{
    pthread_mutex_lock(&nav->lock);
    t->audio_id = nav->want_aid;
    t->sub_id = nav->want_sid;
    t->sub_forced_only = nav->sub_forced_only;
    t->in_menu = nav->in_menu;
    pthread_mutex_unlock(&nav->lock);
}

/* Called with lock held: the length of title t, cached. */
static double title_length(discnav_t *nav, int t)
{
    if (t != nav->length_title) {
        uint64_t *times = NULL, duration = 0;
        dvdnav_describe_title_chapters(nav->dvd, t, &times, &duration);
        free(times);
        nav->length_title = t;
        nav->length_secs = duration / 90000.0;
    }
    return nav->length_secs;
}

static void lang_of(uint16_t code, char lang[3])
{
    lang[0] = code >> 8, lang[1] = code & 0xff, lang[2] = 0;
    if (code == 0xffff || lang[0] < 'a' || lang[0] > 'z' || lang[1] < 'a' || lang[1] > 'z')
        lang[0] = 0;
}

int discnav_get_streams(discnav_t *nav, discnav_stream_type_t type, discnav_stream_info_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&nav->lock);
    if (dvdnav_is_domain_vts(nav->dvd) == 1) {
        for (int i = 0; i < (type == DISCNAV_AUDIO ? 8 : 32) && n < max; i++) {
            discnav_stream_info_t *st = &out[n];
            memset(st, 0, sizeof(*st));
            st->index = i;
            st->format = "";
            if (type == DISCNAV_AUDIO) {
                int phys = dvdnav_get_audio_logical_stream(nav->dvd, i); /* logical -> physical */
                audio_attr_t a;
                if (phys < 0 || dvdnav_get_audio_attr(nav->dvd, i, &a) != DVDNAV_STATUS_OK)
                    continue;
                st->id = audio_stream_id(nav, i, phys);
                if (a.lang_type == 1)
                    lang_of(a.lang_code, st->lang);
                st->kind = a.code_extension == 2 ? DISCNAV_KIND_VISUALLY_IMPAIRED
                         : a.code_extension == 3 || a.code_extension == 4 ? DISCNAV_KIND_COMMENTARY
                         : DISCNAV_KIND_NORMAL;
                switch (a.audio_format) {
                case DVD_AUDIO_FORMAT_AC3:       st->format = "ac3"; break;
                case DVD_AUDIO_FORMAT_DTS:       st->format = "dts"; break;
                case DVD_AUDIO_FORMAT_LPCM:      st->format = "lpcm"; break;
                case DVD_AUDIO_FORMAT_MPEG:
                case DVD_AUDIO_FORMAT_MPEG2_EXT: st->format = "mpeg"; break;
                case DVD_AUDIO_FORMAT_SDDS:      st->format = "sdds"; break;
                }
                st->channels = a.channels + 1;
                st->selected = st->id >= 0 && st->id == nav->want_aid;
            } else {
                int phys = dvdnav_get_spu_logical_stream(nav->dvd, i);
                subp_attr_t a;
                if (phys < 0 || dvdnav_get_spu_attr(nav->dvd, i, &a) != DVDNAV_STATUS_OK)
                    continue;
                st->id = 0x20 + (phys & 0x1f);
                if (a.type == 1)
                    lang_of(a.lang_code, st->lang);
                switch (a.code_extension) {
                case 2: case 6: case 14: st->kind = DISCNAV_KIND_LARGE; break;
                case 3: case 7: case 15: st->kind = DISCNAV_KIND_CHILDREN; break;
                case 9:                  st->kind = DISCNAV_KIND_FORCED; break;
                case 13:                 st->kind = DISCNAV_KIND_COMMENTARY; break;
                default:                 st->kind = DISCNAV_KIND_NORMAL; break;
                }
                if (a.code_extension >= 5 && a.code_extension <= 7)
                    st->kind = DISCNAV_KIND_CAPTIONS;
                st->selected = st->id == nav->want_sid && !nav->sub_forced_only;
            }
            n++;
        }
    }
    pthread_mutex_unlock(&nav->lock);
    return n;
}

bool discnav_select_stream(discnav_t *nav, discnav_stream_type_t type, int index)
{
    pthread_mutex_lock(&nav->lock);
    bool ok = dvdnav_is_domain_vts(nav->dvd) == 1;
    if (ok && type == DISCNAV_AUDIO) {
        int phys = index >= 0 ? dvdnav_get_audio_logical_stream(nav->dvd, index) : -1;
        ok = phys >= 0 && dvdnav_set_active_stream(nav->dvd, index, DVD_AUDIO_STREAM) == DVDNAV_STATUS_OK;
        if (ok)
            nav->want_aid = audio_stream_id(nav, index, phys);
    } else if (ok && index < 0) {
        ok = dvdnav_toggle_spu_stream(nav->dvd, 0) == DVDNAV_STATUS_OK;
        if (ok)
            nav->sub_forced_only = true;
    } else if (ok) {
        int phys = dvdnav_get_spu_logical_stream(nav->dvd, index);
        ok = phys >= 0 && dvdnav_set_active_stream(nav->dvd, index, DVD_SUBTITLE_STREAM) == DVDNAV_STATUS_OK &&
             dvdnav_toggle_spu_stream(nav->dvd, 1) == DVDNAV_STATUS_OK;
        if (ok) {
            nav->want_sid = 0x20 + (phys & 0x1f);
            nav->sub_forced_only = false;
        }
    }
    logf_(nav, 0, "user: %s stream %d%s", type == DISCNAV_AUDIO ? "audio" : "subtitle", index, ok ? "" : " (refused)");
    /* tell the player now rather than when the reader gets there */
    if (ok)
        emit(nav, DISCNAV_EVENT_TRACKS);
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

bool discnav_get_position(discnav_t *nav, int *title, double *seconds, double *length)
{
    pthread_mutex_lock(&nav->lock);
    int32_t t = 0, part = 0;
    bool ok = dvdnav_current_title_info(nav->dvd, &t, &part) == DVDNAV_STATUS_OK && t > 0;
    if (ok) {
        *title = t;
        *seconds = dvdnav_get_current_time(nav->dvd) / 90000.0;
        if (length)
            *length = title_length(nav, t);
    }
    pthread_mutex_unlock(&nav->lock);
    return ok;
}

bool discnav_seek(discnav_t *nav, double seconds)
{
    pthread_mutex_lock(&nav->lock);
    int32_t t = 0, part = 0;
    bool ok = dvdnav_current_title_info(nav->dvd, &t, &part) == DVDNAV_STATUS_OK && t > 0;
    if (ok) {
        double length = title_length(nav, t);
        if (length > 1 && seconds > length - 1)
            seconds = length - 1;
        if (seconds < 0)
            seconds = 0;
        ok = dvdnav_time_search(nav->dvd, (uint64_t)(seconds * 90000)) == DVDNAV_STATUS_OK;
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

bool discnav_get_palette(discnav_t *nav, uint32_t rgb[16])
{
    pthread_mutex_lock(&nav->olock);
    bool have = nav->have_clut;
    for (int i = 0; i < 16; i++)
        rgb[i] = nav->rgb_clut[i] & 0xffffff;
    pthread_mutex_unlock(&nav->olock);
    return have;
}

bool discnav_render_overlay(discnav_t *nav, uint8_t *rgba, int fw, int fh)
{
    pthread_mutex_lock(&nav->olock);
    bool have = nav->spu.pix != NULL;
    if (have)
        spu_composite(&nav->spu, nav->rgb_clut, &nav->hl, rgba, fw, fh);
    else
        memset(rgba, 0, (size_t)fw * fh * 4);
    pthread_mutex_unlock(&nav->olock);
    return have;
}

/* ---------- open/close ---------- */

discnav_t *discnav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user)
{
    discnav_t *nav = calloc(1, sizeof(*nav));
    if (!nav)
        return NULL;
    nav->on_event = on_event;
    nav->on_log = on_log;
    nav->user = user;
    nav->want_aid = nav->want_sid = -1;
    nav->menu_key = -1;
    nav->root_pending = true;
    pthread_mutex_init(&nav->lock, NULL);
    pthread_mutex_init(&nav->olock, NULL);
    pthread_cond_init(&nav->wake, NULL);
    if (dvdnav_open(&nav->dvd, device) != DVDNAV_STATUS_OK) {
        logf_(nav, 0, "can't open %s as a DVD", device);
        discnav_close(nav);
        return NULL;
    }
    char l[3] = { 'e', 'n', 0 };
    if (lang && strlen(lang) >= 2)
        memcpy(l, lang, 2);
    dvdnav_set_readahead_flag(nav->dvd, 1);
    dvdnav_menu_language_select(nav->dvd, l);
    dvdnav_audio_language_select(nav->dvd, l);
    dvdnav_spu_language_select(nav->dvd, l);
    dvdnav_set_PGC_positioning_flag(nav->dvd, 1);
    const char *title = NULL;
    if (dvdnav_get_title_string(nav->dvd, &title) == DVDNAV_STATUS_OK && title)
        snprintf(nav->title, sizeof(nav->title), "%s", title);
    logf_(nav, 0, "opened %s \"%s\"", device, nav->title);
    return nav;
}

void discnav_close(discnav_t *nav)
{
    if (!nav)
        return;
    if (nav->dvd)
        dvdnav_close(nav->dvd);
    spu_image_free(&nav->spu);
    pthread_cond_destroy(&nav->wake);
    pthread_mutex_destroy(&nav->olock);
    pthread_mutex_destroy(&nav->lock);
    free(nav);
}

const char *discnav_title(discnav_t *nav)
{
    return nav->title;
}
