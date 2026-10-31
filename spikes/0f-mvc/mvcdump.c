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

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: mvcdump dev base.m2ts dep.m2ts frames [skip-frac]\n"); return 2; }
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
        fwrite(b.au, 1, b.aulen, stdout);
        fwrite(d.au, 1, d.aulen, stdout);
        out++;
        have_b = next_au(&b); have_d = next_au(&d);
    }
    fprintf(stderr, "%ld pairs, %ld unpaired dropped\n", out, dropped);
    b.f->close(b.f); d.f->close(d.f);
    bd_close(bd);
    return 0;
}
