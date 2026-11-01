/*
 * libdiscnav: Blu-ray 3D. libbluray plays the base view's clip; this reads
 * the dependent view's (another file) alongside and merges it into the base
 * view's stream, each picture's dependent access unit in the same PES as
 * its base one, for an MVC decoder (FFmpeg's libedge264mvc).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * From spike 0f (docs/spike-results.md):
 * - A 3D playlist names its dependent-view clips in an SS sub-path (type
 *   8) of its extension data; the base view is PID 0x1011, the dependent
 *   0x1012, with the same PTS per picture. On disc both clips are
 *   interleaved (the SSIF); read separately in small pieces the drive seeks
 *   between them and falls behind, so the dependent clip is read ahead in
 *   large chunks.
 * - Decrypted files (bd_open_file_dec) read in whole 6144-byte units.
 * - FFmpeg's h264 parser (mpegts) starts a new frame at an SEI, SPS, PPS or
 *   AUD after slices: the dependent view's SEI and parameter sets go before
 *   the base view's slices, or each picture is split across two packets
 *   and gets the wrong timestamp.
 */
#define _GNU_SOURCE
#include "mvc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libbluray/filesystem.h>

#define UNIT 6144
#define PKT 192
#define BASE_PID 0x1011
#define DEP_PID 0x1012
#define CHUNK (8 << 20)        /* dependent clip read ahead, ~5 s of a film */
#define LOCKED_UNITS 170       /* units read per lock taken (~1 MB) */
#define MAX_QUEUE 8192          /* a chunk of a still scene holds hundreds */
#define TS_SECOND 90000

/* DISCNAV_MVC_DEBUG=1: the merge's decisions on stderr */
static bool debug_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("DISCNAV_MVC_DEBUG") && *getenv("DISCNAV_MVC_DEBUG");
    return on;
}
#define DEBUG(...) do { if (debug_on()) fprintf(stderr, "mvc: " __VA_ARGS__); } while (0)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

int mvc_dep_clips(BLURAY *bd, int playlist, char clips[][6], int max)
{
    char path[64];
    snprintf(path, sizeof(path), "BDMV/PLAYLIST/%05d.mpls", playlist);
    void *data = NULL;
    int64_t size = 0;
    if (!bd_read_file(bd, path, &data, &size))
        return 0;
    const uint8_t *p = data;
    int found = 0;
    if (size < 20 || memcmp(p, "MPLS", 4))
        goto done;
    /* ExtensionData: entries of (ID1, ID2, start, length); SubPath
     * entries extension is 2.2 */
    uint32_t ext = be32(p + 16);
    if (!ext || ext + 12 > size || !be32(p + ext))
        goto done;
    int entries = p[ext + 11];
    for (int e = 0; e < entries && ext + 12 + (e + 1) * 12 <= size; e++) {
        const uint8_t *en = p + ext + 12 + e * 12;
        if (be16(en) != 2 || be16(en + 2) != 2)
            continue;
        int64_t at = (int64_t)ext + be32(en + 4);
        if (at + 6 > size)
            goto done;
        int subpaths = be16(p + at + 4);
        at += 6;
        for (int s = 0; s < subpaths && at + 10 <= size; s++) {
            int64_t next = at + 4 + be32(p + at);
            int type = p[at + 5], items = p[at + 9];
            int64_t it = at + 10;
            for (int i = 0; i < items && it + 7 <= size && it + 7 <= next; i++) {
                if (type == 8 && i < max) {
                    memcpy(clips[i], p + it + 2, 5);
                    clips[i][5] = 0;
                    found = i + 1;
                }
                it += 2 + be16(p + it);
            }
            at = next;
        }
    }
done:
    free(data);
    return found;
}

/* ---------- byte buffers ---------- */

typedef struct {
    uint8_t *p;
    size_t len, cap, pos; /* pos: taken from the front */
} buf_t;

static void buf_add(buf_t *b, const void *data, size_t n)
{
    if (b->pos && b->pos == b->len)
        b->pos = b->len = 0;
    if (b->len + n > b->cap) {
        if (b->pos) { /* drop what was taken */
            memmove(b->p, b->p + b->pos, b->len - b->pos);
            b->len -= b->pos;
            b->pos = 0;
        }
        if (b->len + n > b->cap) {
            size_t cap = b->cap ? b->cap : 65536;
            while (cap < b->len + n)
                cap *= 2;
            uint8_t *q = realloc(b->p, cap);
            if (!q)
                return;
            b->p = q;
            b->cap = cap;
        }
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

/* ---------- PES and NAL units ---------- */

/* PTS and DTS of a PES starting at p (n bytes); false without a PTS. */
static bool pes_times(const uint8_t *p, size_t n, int64_t *pts, int64_t *dts)
{
    if (n < 14 || p[0] || p[1] || p[2] != 1 || !(p[7] & 0x80))
        return false;
    const uint8_t *h = p + 9;
    *pts = (int64_t)(h[0] & 0x0e) << 29 | h[1] << 22 | (h[2] & 0xfe) << 14 | h[3] << 7 | h[4] >> 1;
    *dts = *pts;
    if ((p[7] & 0xc0) == 0xc0 && n >= 19) {
        h += 5;
        *dts = (int64_t)(h[0] & 0x0e) << 29 | h[1] << 22 | (h[2] & 0xfe) << 14 | h[3] << 7 | h[4] >> 1;
    }
    return true;
}

static size_t pes_header_len(const uint8_t *p, size_t n)
{
    return n >= 9 ? 9u + p[8] : n;
}

static size_t next_start(const uint8_t *p, size_t from, size_t n)
{
    for (size_t i = from; i + 3 <= n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return n;
}

enum { NALS_BEFORE_VCL, NALS_FROM_VCL, NALS_NOT_VCL, NALS_VCL };

static bool is_vcl(int t) { return t == 1 || t == 5 || t == 20; }

/* Appends some of the NAL units of an access unit (with start codes). */
static void add_nals(buf_t *out, const uint8_t *au, size_t n, int which)
{
    bool vcl_seen = false;
    size_t at = next_start(au, 0, n);
    while (at < n) {
        size_t end = next_start(au, at + 3, n);
        int t = au[at + 3] & 0x1f;
        vcl_seen |= is_vcl(t);
        bool want = which == NALS_BEFORE_VCL ? !vcl_seen
                  : which == NALS_FROM_VCL ? vcl_seen
                  : which == NALS_NOT_VCL ? !is_vcl(t) && t != 24 /* the BD view delimiter */
                  : is_vcl(t) || t == 24;
        if (want)
            buf_add(out, au + at, end - at);
        at = end;
    }
}

/* ---------- the dependent clip ---------- */

typedef struct {
    int64_t pts, dts;
    uint8_t *data; /* the access unit (PES payload) */
    size_t len;
} au_t;

struct mvc {
    BLURAY *bd;
    void (*lock)(void *);
    void (*unlock)(void *);
    void *user;

    /* base view */
    buf_t out;
    buf_t vpes;                 /* the video PES being collected */
    uint8_t vtp[4];             /* its first packet's TP_extra_header */
    uint8_t cc;                 /* our continuity counter for BASE_PID */

    /* dependent view */
    char clip[6];
    BD_FILE_H *dep;
    int64_t dep_size;
    bool positioned;            /* read from where the base view is */
    int64_t retry_dts;          /* lost it: base view alone until this DTS */
    bool eof;
    buf_t dpes;                 /* its PES being collected */
    au_t *queue;                /* its access units read ahead, in decoding order */
    int queued, queue_cap;
    uint8_t *chunk;
};

mvc_t *mvc_new(BLURAY *bd, void (*read_lock)(void *), void (*read_unlock)(void *), void *user)
{
    mvc_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->chunk = malloc(LOCKED_UNITS * UNIT);
    if (!m->chunk) {
        free(m);
        return NULL;
    }
    m->bd = bd;
    m->lock = read_lock;
    m->unlock = read_unlock;
    m->user = user;
    return m;
}

static void clear_queue(mvc_t *m)
{
    for (int i = 0; i < m->queued; i++)
        free(m->queue[i].data);
    m->queued = 0;
    m->dpes.len = m->dpes.pos = 0;
}

static void close_dep(mvc_t *m)
{
    if (m->dep) {
        m->lock(m->user);
        m->dep->close(m->dep);
        m->unlock(m->user);
        m->dep = NULL;
    }
    clear_queue(m);
}

void mvc_free(mvc_t *m)
{
    if (!m)
        return;
    close_dep(m);
    free(m->out.p);
    free(m->vpes.p);
    free(m->dpes.p);
    free(m->queue);
    free(m->chunk);
    free(m);
}

void mvc_reset(mvc_t *m)
{
    m->vpes.len = m->vpes.pos = 0;
    m->positioned = false;
    m->retry_dts = 0;
    m->eof = false;
    clear_queue(m);
}

/* Called with the caller's lock held: opens the file directly. */
void mvc_set_clip(mvc_t *m, const char *clip)
{
    if (!clip)
        clip = "";
    if (!strcmp(clip, m->clip) && (m->dep || !*clip)) {
        mvc_reset(m);
        return;
    }
    if (m->dep) {
        m->dep->close(m->dep);
        m->dep = NULL;
    }
    mvc_reset(m);
    snprintf(m->clip, sizeof(m->clip), "%s", clip);
    if (*clip) {
        char path[64];
        snprintf(path, sizeof(path), "BDMV/STREAM/%s.m2ts", clip);
        m->dep = bd_open_file_dec(m->bd, path);
        m->dep_size = m->dep ? m->dep->seek(m->dep, 0, SEEK_END) : 0;
        if (m->dep)
            m->dep->seek(m->dep, 0, SEEK_SET);
    }
}

/* Queues the dependent view's completed PES. */
static void dep_pes_done(mvc_t *m)
{
    const uint8_t *p = m->dpes.p;
    size_t n = m->dpes.len;
    int64_t pts, dts;
    if (n && pes_times(p, n, &pts, &dts)) {
        if (m->queued == MAX_QUEUE) { /* never matched: drop the oldest */
            free(m->queue[0].data);
            memmove(m->queue, m->queue + 1, (MAX_QUEUE - 1) * sizeof(au_t));
            m->queued--;
        }
        if (m->queued == m->queue_cap) {
            int cap = m->queue_cap ? m->queue_cap * 2 : 256;
            au_t *q = realloc(m->queue, (size_t)cap * sizeof(au_t));
            if (!q) {
                m->dpes.len = m->dpes.pos = 0;
                return;
            }
            m->queue = q;
            m->queue_cap = cap;
        }
        size_t h = pes_header_len(p, n);
        au_t *a = &m->queue[m->queued];
        a->data = malloc(n - h);
        if (a->data) {
            memcpy(a->data, p + h, n - h);
            a->len = n - h;
            a->pts = pts;
            a->dts = dts;
            m->queued++;
        }
    }
    m->dpes.len = m->dpes.pos = 0;
}

/* Splits n bytes of the dependent clip into its view's PES. */
static void dep_parse(mvc_t *m, const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i + PKT <= n; i += PKT) {
        const uint8_t *ts = buf + i + 4;
        if (ts[0] != 0x47 || (((ts[1] & 0x1f) << 8) | ts[2]) != DEP_PID)
            continue;
        int afc = (ts[3] >> 4) & 3;
        const uint8_t *p = ts + 4;
        if (afc & 2)
            p += 1 + p[0];
        if (!(afc & 1) || p >= ts + 188)
            continue;
        if ((ts[1] & 0x40) && m->dpes.len)
            dep_pes_done(m);
        buf_add(&m->dpes, p, (size_t)(ts + 188 - p));
    }
}

/* Reads and parses up to `want` bytes of the dependent clip, taking the
 * lock per ~1 MB. Returns the bytes read. */
static size_t dep_read(mvc_t *m, size_t want)
{
    size_t total = 0;
    while (total < want && !m->eof) {
        size_t got = 0;
        m->lock(m->user);
        for (int u = 0; u < LOCKED_UNITS && total + got < want; u++) {
            if (m->dep->read(m->dep, m->chunk + got, UNIT) != UNIT) {
                m->eof = true;
                break;
            }
            got += UNIT;
        }
        m->unlock(m->user);
        dep_parse(m, m->chunk, got);
        total += got;
    }
    return total;
}

/* The DTS of the first dependent-view PES at or after `offset`, or -1. */
static int64_t dep_probe(mvc_t *m, int64_t offset)
{
    offset = offset / UNIT * UNIT;
    if (offset < 0)
        offset = 0;
    clear_queue(m);
    m->eof = false;
    m->lock(m->user);
    m->dep->seek(m->dep, offset, SEEK_SET);
    m->unlock(m->user);
    for (int tries = 0; tries < 6 && !m->eof; tries++) {
        dep_read(m, 64 * UNIT);
        if (m->queued)
            return m->queue[0].dts;
    }
    return -1;
}

/* Positions the dependent clip a little before `dts`: an interpolation
 * search over the file (DTS grows with the offset). */
static void dep_seek(mvc_t *m, int64_t dts)
{
    int64_t lo = 0, hi = m->dep_size;
    int64_t dlo = dep_probe(m, 0), dhi = -1;
    int64_t start = 0;
    if (dlo >= 0 && dts > dlo + 3 * TS_SECOND) {
        dhi = dep_probe(m, hi - (16 << 20)); /* near the end, past its last few seconds */
        hi -= 16 << 20;
        for (int i = 0; i < 8 && dhi > dlo; i++) {
            int64_t at = lo + (int64_t)((double)(dts - dlo) / (dhi - dlo) * (hi - lo)) - (1 << 20);
            if (at <= lo)
                break;
            int64_t d = dep_probe(m, at);
            if (d < 0 || d >= dts) {
                hi = at;
                dhi = d < 0 ? dhi : d;
            } else {
                lo = at;
                dlo = d;
                if (dts - d < 3 * TS_SECOND)
                    break;
            }
        }
        start = lo;
    }
    clear_queue(m);
    m->eof = false;
    m->lock(m->user);
    m->dep->seek(m->dep, start / UNIT * UNIT, SEEK_SET);
    m->unlock(m->user);
    m->positioned = true;
    DEBUG("dependent clip from %lld (for DTS %lld)\n", (long long)start, (long long)dts);
}

/* The dependent access unit for the base picture with these times, taken
 * off the queue (the caller frees ->data), or NULL. */
static bool dep_find(mvc_t *m, int64_t pts, int64_t dts, au_t *out)
{
    if (!m->dep)
        return false;
    if (m->retry_dts && dts < m->retry_dts && dts > m->retry_dts - 20 * TS_SECOND)
        return false;
    m->retry_dts = 0;
    if (!m->positioned)
        dep_seek(m, dts);
    for (int reads = 0; reads < 4; reads++) {
        for (int i = 0; i < m->queued; i++) {
            if (m->queue[i].pts != pts)
                continue;
            *out = m->queue[i];
            for (int j = 0; j < i; j++) /* behind the base view: never needed */
                free(m->queue[j].data);
            memmove(m->queue, m->queue + i + 1, (size_t)(m->queued - i - 1) * sizeof(au_t));
            m->queued -= i + 1;
            return true;
        }
        if (m->queued && m->queue[m->queued - 1].dts > dts + 2 * TS_SECOND) {
            DEBUG("no dependent view for PTS %lld DTS %lld: %d queued, PTS %lld..%lld, DTS %lld..%lld\n",
                  (long long)pts, (long long)dts, m->queued, (long long)m->queue[0].pts,
                  (long long)m->queue[m->queued - 1].pts, (long long)m->queue[0].dts,
                  (long long)m->queue[m->queued - 1].dts);
            return false; /* it isn't there */
        }
        if (m->eof || !dep_read(m, CHUNK)) {
            DEBUG("dependent clip ended (PTS %lld)\n", (long long)pts);
            return false;
        }
        DEBUG("read ahead for PTS %lld: %d queued, DTS %lld..%lld\n", (long long)pts, m->queued,
              m->queued ? (long long)m->queue[0].dts : -1LL, m->queued ? (long long)m->queue[m->queued - 1].dts : -1LL);
    }
    /* lost: the base view alone for a while, then look again (searching
     * for every picture would starve the stream) */
    m->positioned = false;
    m->retry_dts = dts + 10 * TS_SECOND;
    DEBUG("lost the dependent view at PTS %lld; trying again in 10 s\n", (long long)pts);
    return false;
}

/* ---------- the base view ---------- */

/* Writes payload as packets of BASE_PID, the first starting a PES. */
static void emit_pes(mvc_t *m, const uint8_t *pes, size_t n)
{
    size_t pos = 0;
    bool first = true;
    while (pos < n) {
        uint8_t pkt[PKT];
        memcpy(pkt, m->vtp, 4);
        uint8_t *ts = pkt + 4;
        size_t left = n - pos, af = left < 184 ? 184 - left : 0, take = 184 - af;
        ts[0] = 0x47;
        ts[1] = (first ? 0x40 : 0) | BASE_PID >> 8;
        ts[2] = BASE_PID & 0xff;
        ts[3] = (af ? 0x30 : 0x10) | (m->cc++ & 15);
        uint8_t *q = ts + 4;
        if (af) {
            q[0] = (uint8_t)(af - 1);
            if (af > 1) {
                q[1] = 0;
                memset(q + 2, 0xff, af - 2);
            }
            q += af;
        }
        memcpy(q, pes + pos, take);
        pos += take;
        buf_add(&m->out, pkt, PKT);
        first = false;
    }
}

/* The collected base-view PES, with its dependent access unit. */
static void finish_vpes(mvc_t *m)
{
    const uint8_t *p = m->vpes.p;
    size_t n = m->vpes.len;
    if (!n)
        return;
    int64_t pts, dts;
    au_t dep;
    if (!pes_times(p, n, &pts, &dts) || !dep_find(m, pts, dts, &dep)) {
        emit_pes(m, p, n); /* the base view alone (a decoder repeats it) */
        m->vpes.len = 0;
        return;
    }
    size_t h = pes_header_len(p, n);
    buf_t merged = { 0 };
    buf_add(&merged, p, h);
    merged.p[4] = merged.p[5] = 0; /* video PES: unbounded length */
    add_nals(&merged, p + h, n - h, NALS_BEFORE_VCL);
    add_nals(&merged, dep.data, dep.len, NALS_NOT_VCL);
    add_nals(&merged, p + h, n - h, NALS_FROM_VCL);
    add_nals(&merged, dep.data, dep.len, NALS_VCL);
    emit_pes(m, merged.p, merged.len);
    free(merged.p);
    free(dep.data);
    m->vpes.len = 0;
}

void mvc_feed(mvc_t *m, const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i + PKT <= n; i += PKT) {
        const uint8_t *pkt = buf + i, *ts = pkt + 4;
        if (ts[0] != 0x47 || (((ts[1] & 0x1f) << 8) | ts[2]) != BASE_PID || !m->dep) {
            buf_add(&m->out, pkt, PKT);
            continue;
        }
        int afc = (ts[3] >> 4) & 3;
        const uint8_t *p = ts + 4;
        if (afc & 2) {
            if (p[0] && (p[1] & 0x10)) { /* keep a PCR, in a packet of its own */
                uint8_t pcr[PKT];
                memcpy(pcr, pkt, 4);
                uint8_t *t = pcr + 4;
                t[0] = 0x47;
                t[1] = BASE_PID >> 8;
                t[2] = BASE_PID & 0xff;
                t[3] = 0x20 | ((m->cc - 1) & 15); /* no payload: counter unchanged */
                memset(t + 4, 0xff, 184);
                memcpy(t + 4, p, 1 + (size_t)p[0]);
                t[4] = 183;
                buf_add(&m->out, pcr, PKT);
            }
            p += 1 + p[0];
        }
        if (!(afc & 1) || p >= ts + 188)
            continue;
        if (ts[1] & 0x40) {
            finish_vpes(m);
            memcpy(m->vtp, pkt, 4);
        }
        buf_add(&m->vpes, p, (size_t)(ts + 188 - p));
    }
}

void mvc_flush(mvc_t *m)
{
    finish_vpes(m);
}

size_t mvc_take(mvc_t *m, uint8_t *dst, size_t n)
{
    size_t avail = m->out.len - m->out.pos;
    if (n > avail)
        n = avail;
    memcpy(dst, m->out.p + m->out.pos, n);
    m->out.pos += n;
    return n;
}
