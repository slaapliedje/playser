// Spike: read a 3D Blu-ray's base (PID 0x1011) and dependent (0x1012) views
// from their two clips, decrypted by libbluray, and write one Annex B stream
// with each base access unit followed by its dependent one (what an MVC
// decoder expects). Usage: mvcdump <device> <base.m2ts> <dep.m2ts> <frames> [skip-frac]
#include <libbluray/bluray.h>
#include <libbluray/filesystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BD_FILE_H *f;
    int pid;
    uint8_t *pes; size_t len, cap; // PES being collected
    uint8_t *au; size_t aulen;     // last complete PES payload
    uint8_t hdr[64]; size_t hdrlen; // and its PES header
    int64_t pts;
    int eof;
    uint8_t unit[6144]; int upos, ulen; // decrypted reads come in aligned units
} Track;

static void append(Track *t, const uint8_t *p, size_t n)
{
    if (t->len + n > t->cap) { t->cap = (t->len + n) * 2; t->pes = realloc(t->pes, t->cap); }
    memcpy(t->pes + t->len, p, n); t->len += n;
}

// Turns the collected PES into an access unit; returns 1 if there was one.
static int finish(Track *t)
{
    if (t->len < 9 || t->pes[0] || t->pes[1] || t->pes[2] != 1) { t->len = 0; return 0; }
    const uint8_t *h = t->pes;
    size_t hl = 9 + h[8];
    t->pts = -1;
    if (h[7] & 0x80)
        t->pts = ((int64_t)(h[9] & 0x0e) << 29) | (h[10] << 22) | ((h[11] & 0xfe) << 14) | (h[12] << 7) | (h[13] >> 1);
    if (hl > sizeof t->hdr) { t->len = 0; return 0; }
    memcpy(t->hdr, t->pes, hl);
    t->hdrlen = hl;
    free(t->au);
    t->aulen = t->len - hl;
    t->au = malloc(t->aulen);
    memcpy(t->au, t->pes + hl, t->aulen);
    t->len = 0;
    return 1;
}

// Reads source packets until a whole PES of the track's PID is done.
static int next_au(Track *t)
{
    uint8_t sp[192];
    while (!t->eof) {
        if (t->upos >= t->ulen) {
            t->ulen = (int)t->f->read(t->f, t->unit, sizeof t->unit);
            t->upos = 0;
            if (t->ulen < 192) { t->eof = 1; return finish(t); }
        }
        memcpy(sp, t->unit + t->upos, 192);
        t->upos += 192;
        const uint8_t *ts = sp + 4;
        if (ts[0] != 0x47) continue;
        int pid = ((ts[1] & 0x1f) << 8) | ts[2];
        if (pid != t->pid) continue;
        int pusi = ts[1] & 0x40, afc = (ts[3] >> 4) & 3;
        const uint8_t *p = ts + 4;
        if (afc & 2) p += 1 + p[0];
        if (!(afc & 1) || p >= ts + 188) continue;
        int done = 0;
        if (pusi && t->len) done = finish(t);
        append(t, p, ts + 188 - p);
        if (done) return 1;
    }
    return 0;
}

// MPEG-TS output (-t): one H.264 stream (PID 0x1011) whose PES packets carry
// a base access unit followed by its dependent one, with the base's PTS/DTS
static uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffff;
    while (n--) {
        c ^= (uint32_t)*p++ << 24;
        for (int i = 0; i < 8; i++)
            c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : c << 1;
    }
    return c;
}

static uint8_t cc[0x2000];

static void ts_section(int pid, const uint8_t *sec, size_t n)
{
    uint8_t pkt[188];
    memset(pkt, 0xff, sizeof pkt);
    pkt[0] = 0x47; pkt[1] = 0x40 | pid >> 8; pkt[2] = pid; pkt[3] = 0x10 | (cc[pid]++ & 15);
    pkt[4] = 0; // pointer field
    memcpy(pkt + 5, sec, n);
    uint32_t crc = crc32_mpeg(sec, n);
    pkt[5 + n] = crc >> 24; pkt[6 + n] = crc >> 16; pkt[7 + n] = crc >> 8; pkt[8 + n] = crc;
    fwrite(pkt, 1, 188, stdout);
}

static void ts_tables(void)
{
    const uint8_t pat[] = { 0x00, 0xb0, 13, 0, 1, 0xc1, 0, 0, 0, 1, 0xe1, 0x00 };
    const uint8_t pmt[] = { 0x02, 0xb0, 18, 0, 1, 0xc1, 0, 0, 0xf0 | 0x10, 0x11, 0xf0, 0,
                            0x1b, 0xf0 | 0x10, 0x11, 0xf0, 0 };
    ts_section(0, pat, sizeof pat);
    ts_section(0x100, pmt, sizeof pmt);
}

// Offset of the next 00 00 01 at or after `from`, or n.
static size_t next_start(const uint8_t *p, size_t from, size_t n)
{
    for (size_t i = from; i + 3 <= n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1)
            return i;
    return n;
}

// Appends the NAL units of au[0..n) (with their start codes) whose type
// passes `want`, to out.
static size_t copy_nals(uint8_t *out, const uint8_t *au, size_t n, int (*want)(int))
{
    size_t len = 0, at = next_start(au, 0, n);
    while (at < n) {
        size_t end = next_start(au, at + 3, n);
        if (want(au[at + 3] & 0x1f)) {
            memcpy(out + len, au + at, end - at);
            len += end - at;
        }
        at = end;
    }
    return len;
}

static int is_vcl(int t) { return t == 1 || t == 5 || t == 20; }
static int not_vcl(int t) { return !is_vcl(t) && t != 24; }
static int is_vcl_or_delim(int t) { return is_vcl(t) || t == 24; }
static int before_vcl_done;
static int base_head(int t) { if (is_vcl(t)) before_vcl_done = 1; return !before_vcl_done; }
static int base_tail(int t) { if (is_vcl(t)) before_vcl_done = 1; return before_vcl_done; }

// One access unit as FFmpeg's h264 parser keeps it whole: it starts a new
// frame at an SEI, SPS, PPS or AUD after slices, so the dependent view's
// SEI and parameter sets go before the base view's slices, its slices
// (type 20, and the Blu-ray delimiter, type 24) after them.
static void ts_pes(const Track *b, const Track *d)
{
    const int pid = 0x1011;
    size_t n = b->hdrlen, pos = 0;
    uint8_t *pes = malloc(b->hdrlen + b->aulen + d->aulen);
    memcpy(pes, b->hdr, b->hdrlen);
    pes[4] = pes[5] = 0; // video PES: unbounded length
    before_vcl_done = 0;
    n += copy_nals(pes + n, b->au, b->aulen, base_head);
    n += copy_nals(pes + n, d->au, d->aulen, not_vcl);
    before_vcl_done = 0;
    n += copy_nals(pes + n, b->au, b->aulen, base_tail);
    n += copy_nals(pes + n, d->au, d->aulen, is_vcl_or_delim);
    int64_t dts = b->pts;
    if ((b->hdr[7] & 0xc0) == 0xc0) {
        const uint8_t *h = b->hdr + 14;
        dts = ((int64_t)(h[0] & 0x0e) << 29) | (h[1] << 22) | ((h[2] & 0xfe) << 14) | (h[3] << 7) | (h[4] >> 1);
    }
    int first = 1;
    while (pos < n) {
        uint8_t pkt[188];
        pkt[0] = 0x47; pkt[1] = (first ? 0x40 : 0) | pid >> 8; pkt[2] = pid & 0xff;
        // adaptation field: a PCR in the first packet, stuffing in the last
        size_t af = first ? 8 : 0;
        if (n - pos < 184 - af)
            af += 184 - af - (n - pos);
        size_t take = 184 - af;
        pkt[3] = (af ? 0x30 : 0x10) | (cc[pid]++ & 15);
        uint8_t *q = pkt + 4;
        if (af) {
            q[0] = af - 1;
            if (af > 1) {
                memset(q + 1, 0xff, af - 1);
                q[1] = first ? 0x10 : 0x00;
                if (first && af >= 8) {
                    int64_t pcr = dts > 9000 ? dts - 9000 : 0; // 100 ms ahead of decoding
                    q[2] = pcr >> 25; q[3] = pcr >> 17; q[4] = pcr >> 9; q[5] = pcr >> 1;
                    q[6] = (pcr & 1) << 7 | 0x7e; q[7] = 0;
                }
            }
            q += af;
        }
        memcpy(q, pes + pos, take);
        pos += take;
        fwrite(pkt, 1, 188, stdout);
        first = 0;
    }
    free(pes);
}

int main(int argc, char **argv)
{
    int ts = argc > 1 && !strcmp(argv[1], "-t");
    if (ts) { argv++; argc--; }
    if (argc < 5) { fprintf(stderr, "usage: mvcdump [-t] dev base.m2ts dep.m2ts frames [skip-frac]\n"); return 2; }
    BLURAY *bd = bd_open(argv[1], NULL);
    if (!bd) { fprintf(stderr, "open failed\n"); return 1; }
    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    fprintf(stderr, "aacs %d/%d, 3D content %d\n", di->aacs_detected, di->aacs_handled, di->content_exist_3D);
    if (di->aacs_detected && !di->aacs_handled) { fprintf(stderr, "AACS not handled\n"); return 1; }
    char path[256];
    Track b = { .pid = 0x1011 }, d = { .pid = 0x1012 };
    snprintf(path, sizeof path, "BDMV/STREAM/%s", argv[2]); b.f = bd_open_file_dec(bd, path);
    snprintf(path, sizeof path, "BDMV/STREAM/%s", argv[3]); d.f = bd_open_file_dec(bd, path);
    if (!b.f || !d.f) { fprintf(stderr, "can't open the clips\n"); return 1; }
    if (argc > 5) {
        double frac = atof(argv[5]);
        int64_t bs = b.f->seek(b.f, 0, SEEK_END), ds = d.f->seek(d.f, 0, SEEK_END);
        fprintf(stderr, "clip sizes %lld %lld\n", (long long)bs, (long long)ds);
        b.f->seek(b.f, (int64_t)(bs * frac) / 6144 * 6144, SEEK_SET);
        d.f->seek(d.f, (int64_t)(ds * frac) / 6144 * 6144, SEEK_SET);
        next_au(&b); next_au(&d); // drop partial PES
    }
    long frames = atol(argv[4]), out = 0, dropped = 0;
    int have_b = next_au(&b), have_d = next_au(&d);
    while (have_b && have_d && out < frames) {
        if (b.pts < d.pts) { have_b = next_au(&b); dropped++; continue; }
        if (d.pts < b.pts) { have_d = next_au(&d); dropped++; continue; }
        if (ts) {
            if (out % 24 == 0)
                ts_tables();
            ts_pes(&b, &d);
        } else {
            fwrite(b.au, 1, b.aulen, stdout);
            fwrite(d.au, 1, d.aulen, stdout);
        }
        out++;
        have_b = next_au(&b); have_d = next_au(&d);
    }
    fprintf(stderr, "%ld pairs, %ld unpaired dropped\n", out, dropped);
    b.f->close(b.f); d.f->close(d.f);
    bd_close(bd);
    return 0;
}
