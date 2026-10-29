/*
 * Spike 0b: DVD menus through libdvdnav -> libmpv (stream_cb).
 *
 * libdvdnav drives the VM and hands us 2048-byte MPEG-PS blocks, which we
 * feed to mpv through a custom "discnav://" stream. Button rectangles from the
 * current PCI are drawn on top of mpv's output; arrows / gamepad move the
 * highlight, Enter / A activates.
 *
 * Discontinuities (HOP_CHANNEL, VTS_CHANGE) end the current mpv "file" with
 * EOF; the main loop then reloads discnav://, which continues from wherever
 * the dvdnav VM is. The dvdnav handle outlives individual mpv streams.
 *
 * Still frames: inject an MPEG sequence_end_code so the decoder outputs the
 * last picture, emit a little padding so lavf finishes probing, then block
 * until the still times out or the user does something.
 *
 * Known gaps (deliberate for the spike): button graphics come from mpv's
 * own SPU rendering with a default palette, highlights are our rectangles,
 * and nav state runs ahead of presentation by the demuxer readahead.
 */
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <dvdnav/dvdnav.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>

#define BLOCK 2048
#define STILL_PAD_LIMIT (512 * 1024)
#define MAX_BUTTONS 36

/* ---------- logging ---------- */

static uint64_t t0_ms;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%7.3f nav] ", (now_ms() - t0_ms) / 1000.0);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------- shared state ---------- */

typedef struct { int sx, sy, ex, ey; } rect_t;

static struct {
    pthread_mutex_t lock;   /* guards dvd and everything below */
    pthread_cond_t wake;
    dvdnav_t *dvd;
    unsigned gen;           /* bumped on every user nav action */
    bool stopped;           /* DVDNAV_STOP reached */
    bool user_jump;         /* user action may cause a jump: flush mpv on the hop */
    bool have_ptm;
    uint32_t last_e_ptm;    /* end PTM of the previous VOBU, for discontinuity checks */
    uint8_t last_pack[14];  /* most recent pack header, reused for injected blocks */
} N = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static struct {
    pthread_mutex_t lock;
    int n;
    rect_t btn[MAX_BUTTONS];
    int cur;                /* 1-based, 0 = none */
} O = { .lock = PTHREAD_MUTEX_INITIALIZER };

static Uint32 ev_redraw, ev_mpv, ev_shot, ev_flush;

/* Called with N.lock held. Snapshot the button layout for the renderer. */
static void update_overlay(void)
{
    pci_t *pci = dvdnav_get_current_nav_pci(N.dvd);
    int32_t cur = 0;
    dvdnav_get_current_highlight(N.dvd, &cur);

    pthread_mutex_lock(&O.lock);
    O.n = 0;
    if (pci && (pci->hli.hl_gi.hli_ss & 3)) {
        int n = pci->hli.hl_gi.btn_ns & 0x3f;
        for (int i = 0; i < n && i < MAX_BUTTONS; i++) {
            btni_t *b = &pci->hli.btnit[i];
            O.btn[O.n++] = (rect_t){ b->x_start, b->y_start, b->x_end, b->y_end };
        }
    }
    bool changed = O.cur != cur;
    O.cur = cur;
    pthread_mutex_unlock(&O.lock);

    if (changed)
        logf_("highlight -> button %d", cur);
    SDL_Event e = { .type = ev_redraw };
    SDL_PushEvent(&e);
}

/* ---------- synthetic MPEG-PS blocks ---------- */

static void put_pack_header(uint8_t *b)
{
    memcpy(b, N.last_pack, 14);
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

static void make_padding_block(uint8_t *b)
{
    put_pack_header(b);
    put_padding(b, 14);
}

/* Video PES with a lone sequence_end_code: makes libavcodec emit the
 * picture it is holding back, so a still menu actually appears. */
static void make_seq_end_block(uint8_t *b)
{
    static const uint8_t pes[] = { 0, 0, 1, 0xe0, 0, 7, 0x80, 0, 0,
                                   0, 0, 1, 0xb7 };
    put_pack_header(b);
    memcpy(b + 14, pes, sizeof(pes));
    put_padding(b, 14 + sizeof(pes));
}

/* ---------- mpv stream_cb ---------- */

typedef struct {
    uint8_t blk[BLOCK];
    int blk_pos, blk_len;
    bool cancel, eof, got_data;
    bool in_still;
    int still_len;          /* seconds; 0xff = infinite */
    uint64_t still_start;
    size_t still_pad;
} stream_t;

/* Produce the next block. 1 = data in s->blk, 0 = EOF, -1 = error/cancel. */
static int next_block(stream_t *s)
{
    int ret;
    pthread_mutex_lock(&N.lock);
    for (;;) {
        if (s->cancel) { ret = -1; break; }
        if (s->eof) { ret = 0; break; }

        if (s->in_still) {
            if (s->still_len != 0xff &&
                now_ms() - s->still_start >= (uint64_t)s->still_len * 1000) {
                logf_("still timed out (%ds)", s->still_len);
                dvdnav_still_skip(N.dvd);
                s->in_still = false;
                continue;
            }
            if (s->still_pad < STILL_PAD_LIMIT) {
                make_padding_block(s->blk);
                s->still_pad += BLOCK;
                ret = 1;
                break;
            }
            unsigned gen = N.gen;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            while (gen == N.gen && !s->cancel &&
                   pthread_cond_timedwait(&N.wake, &N.lock, &ts) != ETIMEDOUT)
                ;
            /* fall through: ask dvdnav whether the still is over */
        }

        int32_t ev, len;
        if (dvdnav_get_next_block(N.dvd, s->blk, &ev, &len) != DVDNAV_STATUS_OK) {
            logf_("get_next_block: %s", dvdnav_err_to_string(N.dvd));
            ret = -1;
            break;
        }

        if (ev != DVDNAV_STILL_FRAME && ev != DVDNAV_NOP)
            s->in_still = false;

        switch (ev) {
        case DVDNAV_BLOCK_OK:
            memcpy(N.last_pack, s->blk, 14);
            s->got_data = true;
            ret = 1;
            goto out;
        case DVDNAV_NOP:
            continue;
        case DVDNAV_STILL_FRAME: {
            if (s->in_still)
                continue;
            dvdnav_still_event_t *e = (dvdnav_still_event_t *)s->blk;
            logf_("still frame, length %d%s", e->length,
                  e->length == 0xff ? " (infinite)" : "s");
            s->in_still = true;
            s->still_len = e->length;
            s->still_start = now_ms();
            s->still_pad = BLOCK;
            make_seq_end_block(s->blk);
            ret = 1;
            goto out;
        }
        case DVDNAV_WAIT:
            dvdnav_wait_skip(N.dvd);
            continue;
        case DVDNAV_HOP_CHANNEL:
        case DVDNAV_VTS_CHANGE:
            logf_("%s%s%s", ev == DVDNAV_HOP_CHANNEL ? "hop channel" : "vts change",
                  s->got_data ? " -> end segment" : " (segment start, ignored)",
                  s->got_data && N.user_jump ? ", flush" : "");
            if (s->got_data) {
                /* user-initiated: drop mpv's buffered old content right away
                 * instead of letting it play out */
                if (N.user_jump) {
                    SDL_Event fe = { .type = ev_flush };
                    SDL_PushEvent(&fe);
                }
                N.user_jump = false;
                s->eof = true;
                ret = 0;
                goto out;
            }
            N.user_jump = false;
            continue;
        case DVDNAV_CELL_CHANGE: {
            dvdnav_cell_change_event_t *e = (dvdnav_cell_change_event_t *)s->blk;
            logf_("cell change: cell %d pg %d (%s)", e->cellN, e->pgN,
                  dvdnav_is_domain_vts(N.dvd) ? "title" : "menu");
            continue;
        }
        case DVDNAV_NAV_PACKET: {
            /* A VOBU whose start PTM doesn't continue the previous one is a
             * timestamp discontinuity (e.g. a looping menu's "jump cell 1",
             * which emits no HOP). mpv can't digest that mid-file, so end the
             * segment and let the reload start fresh timing. */
            pci_t *pci = dvdnav_get_current_nav_pci(N.dvd);
            uint32_t s_ptm = pci->pci_gi.vobu_s_ptm, e_ptm = pci->pci_gi.vobu_e_ptm;
            bool discont = N.have_ptm && s_ptm != N.last_e_ptm;
            N.have_ptm = true;
            N.last_e_ptm = e_ptm;
            update_overlay();
            if (discont && s->got_data) {
                logf_("pts discontinuity -> end segment");
                s->eof = true;
                ret = 0;
                goto out;
            }
            continue;
        }
        case DVDNAV_HIGHLIGHT:
            update_overlay();
            continue;
        case DVDNAV_SPU_CLUT_CHANGE:
            logf_("spu clut change");
            continue;
        case DVDNAV_SPU_STREAM_CHANGE:
        case DVDNAV_AUDIO_STREAM_CHANGE:
            continue;
        case DVDNAV_STOP:
            logf_("stop");
            N.stopped = true;
            s->eof = true;
            ret = 0;
            goto out;
        default:
            logf_("unhandled event %d", ev);
            continue;
        }
    }
out:
    pthread_mutex_unlock(&N.lock);
    return ret;
}

static int64_t read_fn(void *cookie, char *buf, uint64_t nbytes)
{
    stream_t *s = cookie;
    if (s->blk_pos >= s->blk_len) {
        int r = next_block(s);
        if (r <= 0)
            return r;
        s->blk_pos = 0;
        s->blk_len = BLOCK;
    }
    uint64_t n = s->blk_len - s->blk_pos;
    if (n > nbytes)
        n = nbytes;
    memcpy(buf, s->blk + s->blk_pos, n);
    s->blk_pos += n;
    return n;
}

static void cancel_fn(void *cookie)
{
    stream_t *s = cookie;
    pthread_mutex_lock(&N.lock);
    s->cancel = true;
    pthread_cond_broadcast(&N.wake);
    pthread_mutex_unlock(&N.lock);
}

static void close_fn(void *cookie)
{
    free(cookie);
}

static int open_fn(void *user, char *uri, mpv_stream_cb_info *info)
{
    (void)user; (void)uri;
    stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return MPV_ERROR_LOADING_FAILED;
    info->cookie = s;
    info->read_fn = read_fn;
    info->close_fn = close_fn;
    info->cancel_fn = cancel_fn;
    return 0;
}

/* ---------- user actions ---------- */

enum action { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_ACTIVATE, A_ROOT_MENU,
              A_TITLE_MENU, A_GO_UP };

static void nav_action(enum action a)
{
    static const char *names[] = { "up", "down", "left", "right", "activate",
                                   "root menu", "title menu", "go up" };
    pthread_mutex_lock(&N.lock);
    pci_t *pci = dvdnav_get_current_nav_pci(N.dvd);
    dvdnav_status_t st = DVDNAV_STATUS_OK;
    switch (a) {
    case A_UP:         st = dvdnav_upper_button_select(N.dvd, pci); break;
    case A_DOWN:       st = dvdnav_lower_button_select(N.dvd, pci); break;
    case A_LEFT:       st = dvdnav_left_button_select(N.dvd, pci); break;
    case A_RIGHT:      st = dvdnav_right_button_select(N.dvd, pci); break;
    case A_ACTIVATE:   st = dvdnav_button_activate(N.dvd, pci); break;
    case A_ROOT_MENU:  st = dvdnav_menu_call(N.dvd, DVD_MENU_Root); break;
    case A_TITLE_MENU: st = dvdnav_menu_call(N.dvd, DVD_MENU_Title); break;
    case A_GO_UP:      st = dvdnav_go_up(N.dvd); break;
    }
    logf_("user: %s%s", names[a], st == DVDNAV_STATUS_OK ? "" : " (refused)");
    if (a >= A_ACTIVATE && st == DVDNAV_STATUS_OK)
        N.user_jump = true;
    update_overlay();
    N.gen++;
    pthread_cond_broadcast(&N.wake);
    pthread_mutex_unlock(&N.lock);
}

static void nav_mouse(int vx, int vy, bool click)
{
    pthread_mutex_lock(&N.lock);
    pci_t *pci = dvdnav_get_current_nav_pci(N.dvd);
    if (click)
        dvdnav_mouse_activate(N.dvd, pci, vx, vy);
    else
        dvdnav_mouse_select(N.dvd, pci, vx, vy);
    update_overlay();
    if (click) {
        N.user_jump = true;
        N.gen++;
        pthread_cond_broadcast(&N.wake);
    }
    pthread_mutex_unlock(&N.lock);
}

/* ---------- rendering ---------- */

typedef struct { double x, y, sx, sy; bool ok; } vmap_t;

/* Observed via mpv_observe_property. The main thread is also the render
 * thread, and a synchronous mpv_get_property() there deadlocks: the core can
 * be waiting on us to render while we wait on the core. */
enum { P_VW, P_VH, P_ML, P_MR, P_MT, P_MB, P_COUNT };
static const char *const prop_names[P_COUNT] = {
    "video-params/w", "video-params/h", "osd-dimensions/ml",
    "osd-dimensions/mr", "osd-dimensions/mt", "osd-dimensions/mb",
};
static double props[P_COUNT];

/* Mapping from DVD storage coords (e.g. 720x480) to window pixels. */
static vmap_t video_map(mpv_handle *mpv, int win_w, int win_h)
{
    (void)mpv;
    vmap_t m = { 0 };
    double vw = props[P_VW], vh = props[P_VH];
    double ml = props[P_ML], mr = props[P_MR], mt = props[P_MT], mb = props[P_MB];
    if (vw <= 0 || vh <= 0)
        return m;
    m.x = ml;
    m.y = mt;
    m.sx = (win_w - ml - mr) / vw;
    m.sy = (win_h - mt - mb) / vh;
    m.ok = true;
    return m;
}

static void draw_rect(float x0, float y0, float x1, float y1, bool fill)
{
    glBegin(fill ? GL_QUADS : GL_LINE_LOOP);
    glVertex2f(x0, y0); glVertex2f(x1, y0);
    glVertex2f(x1, y1); glVertex2f(x0, y1);
    glEnd();
}

static void draw_overlay(vmap_t m, int w, int h)
{
    if (!m.ok)
        return;
    /* mpv leaves its own GL state behind; reset what fixed-function needs */
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(2);

    pthread_mutex_lock(&O.lock);
    for (int i = 0; i < O.n; i++) {
        rect_t r = O.btn[i];
        float x0 = m.x + r.sx * m.sx, y0 = m.y + r.sy * m.sy;
        float x1 = m.x + r.ex * m.sx, y1 = m.y + r.ey * m.sy;
        if (i + 1 == O.cur) {
            glColor4f(1, 0.85f, 0.1f, 0.30f);
            draw_rect(x0, y0, x1, y1, true);
            glColor4f(1, 0.85f, 0.1f, 0.95f);
        } else {
            glColor4f(1, 1, 1, 0.35f);
        }
        draw_rect(x0, y0, x1, y1, false);
    }
    pthread_mutex_unlock(&O.lock);
    glDisable(GL_BLEND);
}

/* Write the back buffer as a binary PPM (rows flipped to top-down). */
static void save_framebuffer(const char *path, int w, int h)
{
    uint8_t *px = malloc((size_t)w * h * 3);
    FILE *f = px ? fopen(path, "wb") : NULL;
    if (!f) {
        free(px);
        logf_("shot: cannot write %s", path);
        return;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    fclose(f);
    free(px);
}

/* ---------- glue ---------- */

static void *get_proc(void *ctx, const char *name)
{
    (void)ctx;
    return (void *)SDL_GL_GetProcAddress(name);
}

static void on_mpv_wakeup(void *ctx)
{
    (void)ctx;
    SDL_Event e = { .type = ev_mpv };
    SDL_PushEvent(&e);
}

static void on_mpv_render_update(void *ctx)
{
    (void)ctx;
    SDL_Event e = { .type = ev_redraw };
    SDL_PushEvent(&e);
}

static void load_segment(mpv_handle *mpv, bool no_menu_audio)
{
    if (no_menu_audio) {
        pthread_mutex_lock(&N.lock);
        bool menu = !dvdnav_is_domain_vts(N.dvd);
        pthread_mutex_unlock(&N.lock);
        mpv_set_property_string(mpv, "aid", menu ? "no" : "auto");
    }
    const char *cmd[] = { "loadfile", "discnav://", NULL };
    mpv_command_async(mpv, 0, cmd);
}

static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

/* ---------- scripted input (--script "2,down,enter,1.5,shot,quit") ---------- */

static void *script_thread(void *arg)
{
    static const struct { const char *name; SDL_Keycode key; } keys[] = {
        { "up", SDLK_UP }, { "down", SDLK_DOWN }, { "left", SDLK_LEFT },
        { "right", SDLK_RIGHT }, { "enter", SDLK_RETURN }, { "menu", SDLK_M },
        { "title", SDLK_T }, { "back", SDLK_BACKSPACE }, { "quit", SDLK_Q },
    };
    char *script = strdup(arg), *save = NULL;
    for (char *tok = strtok_r(script, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *end;
        double secs = strtod(tok, &end);
        if (end != tok && *end == '\0') {
            SDL_Delay((Uint32)(secs * 1000));
            continue;
        }
        SDL_Event e = { 0 };
        if (!strcmp(tok, "shot")) {
            e.type = ev_shot;
        } else {
            e.type = SDL_EVENT_KEY_DOWN;
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
                if (!strcmp(tok, keys[i].name))
                    e.key.key = keys[i].key;
            if (!e.key.key) {
                logf_("script: unknown token '%s'", tok);
                continue;
            }
        }
        logf_("script: %s", tok);
        SDL_PushEvent(&e);
    }
    free(script);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/sr0";
    bool no_menu_audio = false;
    const char *script = NULL, *mpv_log = NULL, *shot_dir = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-menu-audio"))
            no_menu_audio = true;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script = argv[++i];
        else if (!strcmp(argv[i], "--mpv-log") && i + 1 < argc)
            mpv_log = argv[++i];
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc)
            shot_dir = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [--no-menu-audio] [--script STEPS] [--mpv-log FILE]\n"
                            "       [--shot-dir DIR] [device|iso|VIDEO_TS dir]\n", argv[0]);
            return 2;
        } else
            dev = argv[i];
    }
    t0_ms = now_ms();

    if (dvdnav_open(&N.dvd, dev) != DVDNAV_STATUS_OK)
        die("dvdnav_open failed (no DVD in drive?)");
    char lang[] = "en";
    dvdnav_set_readahead_flag(N.dvd, 1);
    dvdnav_menu_language_select(N.dvd, lang);
    dvdnav_audio_language_select(N.dvd, lang);
    dvdnav_spu_language_select(N.dvd, lang);
    dvdnav_set_PGC_positioning_flag(N.dvd, 1);
    const char *title = NULL;
    dvdnav_get_title_string(N.dvd, &title);
    logf_("opened %s: \"%s\"", dev, title ? title : "");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        die(SDL_GetError());
    ev_redraw = SDL_RegisterEvents(4);
    ev_flush = ev_redraw + 3;
    ev_mpv = ev_redraw + 1;
    ev_shot = ev_redraw + 2;

    SDL_Window *win = SDL_CreateWindow("dvdmenu spike", 1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win)
        die(SDL_GetError());
    SDL_GLContext glctx = SDL_GL_CreateContext(win);
    if (!glctx)
        die(SDL_GetError());
    SDL_GL_SetSwapInterval(1);

    mpv_handle *mpv = mpv_create();
    if (!mpv)
        die("mpv_create");
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto-safe");
    mpv_set_option_string(mpv, "idle", "yes");
    mpv_set_option_string(mpv, "terminal", "yes");
    mpv_set_option_string(mpv, "msg-level", "all=status");
    mpv_set_option_string(mpv, "cache", "no");
    mpv_set_option_string(mpv, "demuxer-readahead-secs", "0.5");
    mpv_set_option_string(mpv, "demuxer-lavf-format", "mpeg");
    mpv_set_option_string(mpv, "demuxer-lavf-probesize", "262144");
    mpv_set_option_string(mpv, "demuxer-lavf-analyzeduration", "0.5");
    mpv_set_option_string(mpv, "sid", "1");
    if (mpv_log)
        mpv_set_option_string(mpv, "log-file", mpv_log);
    if (mpv_initialize(mpv) < 0)
        die("mpv_initialize");
    if (mpv_stream_cb_add_ro(mpv, "discnav", NULL, open_fn) < 0)
        die("mpv_stream_cb_add_ro");

    int adv = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,
          &(mpv_opengl_init_params){ .get_proc_address = get_proc } },
        { MPV_RENDER_PARAM_ADVANCED_CONTROL, &adv },
        /* never let a render call sleep until a frame's target time: the
         * render thread is our UI thread */
        { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &(int){ 0 } },
        { 0 }
    };
    mpv_render_context *rctx;
    if (mpv_render_context_create(&rctx, mpv, params) < 0)
        die("mpv_render_context_create");
    for (int i = 0; i < P_COUNT; i++)
        mpv_observe_property(mpv, i + 1, prop_names[i], MPV_FORMAT_DOUBLE);
    mpv_set_wakeup_callback(mpv, on_mpv_wakeup, NULL);
    mpv_render_context_set_update_callback(rctx, on_mpv_render_update, NULL);

    load_segment(mpv, no_menu_audio);

    pthread_t script_tid;
    if (script)
        pthread_create(&script_tid, NULL, script_thread, (void *)script);

    bool quit = false;
    int errors = 0, shots = 0;
    bool shot_pending = false, load_pending = true;
    while (!quit) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e))
            die(SDL_GetError());
        bool redraw = false;
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);

        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit = true;
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            redraw = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
            case SDLK_UP:        nav_action(A_UP); break;
            case SDLK_DOWN:      nav_action(A_DOWN); break;
            case SDLK_LEFT:      nav_action(A_LEFT); break;
            case SDLK_RIGHT:     nav_action(A_RIGHT); break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:  nav_action(A_ACTIVATE); break;
            case SDLK_M:         nav_action(A_ROOT_MENU); break;
            case SDLK_T:         nav_action(A_TITLE_MENU); break;
            case SDLK_BACKSPACE: nav_action(A_GO_UP); break;
            case SDLK_F:
                SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                break;
            case SDLK_SPACE: {
                const char *cmd[] = { "cycle", "pause", NULL };
                mpv_command_async(mpv, 0, cmd);
                break;
            }
            case SDLK_Q:
            case SDLK_ESCAPE:    quit = true; break;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_OpenGamepad(e.gdevice.which))
                logf_("gamepad connected");
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:    nav_action(A_UP); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  nav_action(A_DOWN); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  nav_action(A_LEFT); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: nav_action(A_RIGHT); break;
            case SDL_GAMEPAD_BUTTON_SOUTH:      nav_action(A_ACTIVATE); break;
            case SDL_GAMEPAD_BUTTON_EAST:       nav_action(A_GO_UP); break;
            case SDL_GAMEPAD_BUTTON_START:      nav_action(A_ROOT_MENU); break;
            case SDL_GAMEPAD_BUTTON_BACK:       nav_action(A_TITLE_MENU); break;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            vmap_t m = video_map(mpv, w, h);
            if (!m.ok)
                break;
            float scale = SDL_GetWindowPixelDensity(win);
            float px = (e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x) * scale;
            float py = (e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y) * scale;
            nav_mouse((int)((px - m.x) / m.sx), (int)((py - m.y) / m.sy),
                      e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        default:
            if (e.type == ev_flush) {
                logf_("flush: replacing segment");
                load_pending = true;
                load_segment(mpv, no_menu_audio);
            } else if (e.type == ev_shot) {
                /* captured from the framebuffer after the next redraw, so it
                 * includes our overlay (mpv's own screenshot would not) */
                shot_pending = true;
                redraw = true;
            } else if (e.type == ev_redraw) {
                uint64_t flags = mpv_render_context_update(rctx);
                (void)flags;
                redraw = true;
            } else if (e.type == ev_mpv) {
                for (;;) {
                    mpv_event *me = mpv_wait_event(mpv, 0);
                    if (me->event_id == MPV_EVENT_NONE)
                        break;
                    if (me->event_id == MPV_EVENT_LOG_MESSAGE)
                        continue;
                    if (me->event_id == MPV_EVENT_SHUTDOWN) {
                        quit = true;
                        break;
                    }
                    if (me->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                        mpv_event_property *p = me->data;
                        uint64_t i = me->reply_userdata - 1;
                        if (i < P_COUNT)
                            props[i] = p->format == MPV_FORMAT_DOUBLE ? *(double *)p->data : 0;
                        redraw = true;
                    }
                    if (me->event_id == MPV_EVENT_FILE_LOADED) {
                        logf_("mpv: segment loaded");
                        load_pending = false;
                    }
                    if (me->event_id == MPV_EVENT_END_FILE) {
                        mpv_event_end_file *ef = me->data;
                        logf_("mpv: segment ended (reason %d)", ef->reason);
                        if (N.stopped) {
                            quit = true;
                            break;
                        }
                        errors = ef->reason == MPV_END_FILE_REASON_ERROR ? errors + 1 : 0;
                        if (errors > 5)
                            die("too many consecutive load errors");
                        /* a flush already queued the next segment; reloading
                         * again would cancel it and lose its first blocks */
                        if (!load_pending || errors) {
                            load_pending = true;
                            load_segment(mpv, no_menu_audio);
                        }
                    }
                }
            }
        }

        if (redraw) {
            mpv_render_param rp[] = {
                { MPV_RENDER_PARAM_OPENGL_FBO,
                  &(mpv_opengl_fbo){ .fbo = 0, .w = w, .h = h } },
                { MPV_RENDER_PARAM_FLIP_Y, &(int){ 1 } },
                { 0 }
            };
            mpv_render_context_render(rctx, rp);
            draw_overlay(video_map(mpv, w, h), w, h);
            if (shot_pending) {
                shot_pending = false;
                char path[4096];
                snprintf(path, sizeof(path), "%s/shot-%02d.ppm", shot_dir, ++shots);
                save_framebuffer(path, w, h);
                pthread_mutex_lock(&O.lock);
                logf_("shot %s: %d buttons, current %d", path, O.n, O.cur);
                pthread_mutex_unlock(&O.lock);
            }
            SDL_GL_SwapWindow(win);
            mpv_render_context_report_swap(rctx);
        }
    }

    mpv_render_context_free(rctx);
    mpv_terminate_destroy(mpv);
    dvdnav_close(N.dvd);
    SDL_GL_DestroyContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
