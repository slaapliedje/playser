#include "spu.h"

#include <stdlib.h>
#include <string.h>

void spu_asm_reset(spu_assembler_t *a)
{
    a->size = 0;
    a->have = 0;
}

bool spu_asm_feed(spu_assembler_t *a, const uint8_t *p, int len)
{
    if (a->size == 0 || a->have >= a->size) {
        /* start of a unit: first two bytes are its total size */
        if (len < 2)
            return false;
        a->size = (p[0] << 8) | p[1];
        a->have = 0;
        if (a->size < 4) {
            a->size = 0;
            return false;
        }
    }
    int n = len;
    if (a->have + n > a->size)
        n = a->size - a->have;
    memcpy(a->buf + a->have, p, n);
    a->have += n;
    return a->have == a->size;
}

/* ---------- RLE ---------- */

typedef struct {
    const uint8_t *buf;
    int size;
    int nib;    /* nibble position */
} nibbles_t;

static int get_nibble(nibbles_t *r)
{
    int byte = r->nib >> 1;
    if (byte >= r->size)
        return -1;
    int v = (r->nib & 1) ? r->buf[byte] & 0x0f : r->buf[byte] >> 4;
    r->nib++;
    return v;
}

/* Codes are 1-4 nibbles: the value's leading zero nibbles say how many more
 * to read. Bits 1-0 are the pixel value, the rest the run length; a run of
 * 0 means "to the end of the line". */
static int get_code(nibbles_t *r)
{
    int v = get_nibble(r), n;
    if (v < 0)
        return -1;
    for (int limit = 0x4; v < limit && limit <= 0x40; limit <<= 2) {
        if ((n = get_nibble(r)) < 0)
            return -1;
        v = (v << 4) | n;
    }
    return v;
}

static void decode_field(const uint8_t *buf, int size, int offset,
                         uint8_t *pix, int w, int h, int first_line)
{
    nibbles_t r = { buf, size, offset * 2 };
    for (int y = first_line; y < h; y += 2) {
        int x = 0;
        while (x < w) {
            int code = get_code(&r);
            if (code < 0)
                return;
            int run = code >> 2;
            if (run == 0 || x + run > w)
                run = w - x;
            memset(pix + y * w + x, code & 3, run);
            x += run;
        }
        r.nib = (r.nib + 1) & ~1; /* lines start byte-aligned */
    }
}

/* ---------- control sequences ---------- */

bool spu_decode(const uint8_t *buf, int size, spu_image_t *img)
{
    memset(img, 0, sizeof(*img));
    if (size < 4)
        return false;

    int off_top = -1, off_bot = -1, x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    int dcsq = (buf[2] << 8) | buf[3];

    for (int guard = 0; guard < 64 && dcsq + 4 <= size; guard++) {
        int next = (buf[dcsq + 2] << 8) | buf[dcsq + 3];
        int i = dcsq + 4;
        bool end = false;
        while (!end && i < size) {
            switch (buf[i++]) {
            case 0x00: /* FSTA_DSP: forced start */
                img->forced = true;
                break;
            case 0x01: /* STA_DSP */
            case 0x02: /* STP_DSP: menus stay up; timing is the caller's job */
                break;
            case 0x03: /* SET_COLOR */
                if (i + 2 > size)
                    return false;
                img->color[3] = buf[i] >> 4;
                img->color[2] = buf[i] & 0x0f;
                img->color[1] = buf[i + 1] >> 4;
                img->color[0] = buf[i + 1] & 0x0f;
                i += 2;
                break;
            case 0x04: /* SET_CONTR */
                if (i + 2 > size)
                    return false;
                img->alpha[3] = buf[i] >> 4;
                img->alpha[2] = buf[i] & 0x0f;
                img->alpha[1] = buf[i + 1] >> 4;
                img->alpha[0] = buf[i + 1] & 0x0f;
                i += 2;
                break;
            case 0x05: /* SET_DAREA: 12-bit x1, x2, y1, y2 */
                if (i + 6 > size)
                    return false;
                x0 = (buf[i] << 4) | (buf[i + 1] >> 4);
                x1 = ((buf[i + 1] & 0x0f) << 8) | buf[i + 2];
                y0 = (buf[i + 3] << 4) | (buf[i + 4] >> 4);
                y1 = ((buf[i + 4] & 0x0f) << 8) | buf[i + 5];
                i += 6;
                break;
            case 0x06: /* SET_DSPXA: RLE offsets of the two fields */
                if (i + 4 > size)
                    return false;
                off_top = (buf[i] << 8) | buf[i + 1];
                off_bot = (buf[i + 2] << 8) | buf[i + 3];
                i += 4;
                break;
            case 0x07: /* CHG_COLCON: skip, size includes its own 2 bytes */
                if (i + 2 > size)
                    return false;
                i += (buf[i] << 8) | buf[i + 1];
                break;
            default: /* 0xff CMD_END, or unknown */
                end = true;
                break;
            }
        }
        if (next == dcsq)
            break;
        dcsq = next;
    }

    if (off_top < 0 || off_bot < 0 || x1 < x0 || y1 < y0)
        return false;
    img->x = x0;
    img->y = y0;
    img->w = x1 - x0 + 1;
    img->h = y1 - y0 + 1;
    img->pix = calloc((size_t)img->w * img->h, 1);
    if (!img->pix)
        return false;
    decode_field(buf, size, off_top, img->pix, img->w, img->h, 0);
    decode_field(buf, size, off_bot, img->pix, img->w, img->h, 1);
    return true;
}

void spu_image_free(spu_image_t *img)
{
    free(img->pix);
    img->pix = NULL;
}

/* ---------- compositing ---------- */

static uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

uint32_t spu_clut_to_rgb(uint32_t ycrcb)
{
    int y = ((ycrcb >> 16) & 0xff) - 16;
    int cr = ((ycrcb >> 8) & 0xff) - 128;
    int cb = (ycrcb & 0xff) - 128;
    int r = (298 * y + 409 * cr + 128) >> 8;
    int g = (298 * y - 208 * cr - 100 * cb + 128) >> 8;
    int b = (298 * y + 516 * cb + 128) >> 8;
    return (uint32_t)clamp8(r) << 16 | (uint32_t)clamp8(g) << 8 | clamp8(b);
}

void spu_composite(const spu_image_t *img, const uint32_t rgb_clut[16],
                   const spu_highlight_t *hl, uint8_t *rgba, int fw, int fh)
{
    memset(rgba, 0, (size_t)fw * fh * 4);
    if (!img->pix)
        return;

    uint8_t hl_color[4], hl_alpha[4];
    if (hl && hl->valid) {
        for (int v = 0; v < 4; v++) {
            hl_color[v] = (hl->palette >> (16 + 4 * v)) & 0x0f;
            hl_alpha[v] = (hl->palette >> (4 * v)) & 0x0f;
        }
    }

    for (int y = 0; y < img->h; y++) {
        int fy = img->y + y;
        if (fy < 0 || fy >= fh)
            continue;
        for (int x = 0; x < img->w; x++) {
            int fx = img->x + x;
            if (fx < 0 || fx >= fw)
                continue;
            int v = img->pix[y * img->w + x];
            int ci = img->color[v], a = img->alpha[v];
            if (hl && hl->valid && fx >= hl->sx && fx <= hl->ex &&
                fy >= hl->sy && fy <= hl->ey) {
                ci = hl_color[v];
                a = hl_alpha[v];
            }
            if (!a)
                continue;
            uint32_t rgb = rgb_clut[ci];
            uint8_t *o = rgba + ((size_t)fy * fw + fx) * 4;
            o[0] = rgb >> 16;
            o[1] = rgb >> 8;
            o[2] = rgb;
            o[3] = a * 17;
        }
    }
}
