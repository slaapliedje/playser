/*
 * DVD subpicture (SPU) decoding and menu-highlight compositing.
 *
 * An SPU unit is a 2-bit run-length-encoded bitmap plus a control sequence
 * that assigns each of the four pixel values a CLUT index (0-15) and a
 * contrast/alpha (0-15). Menu highlights replace those four colour/alpha
 * pairs inside the selected button's rectangle.
 */
#ifndef DISCNAV_SPU_H
#define DISCNAV_SPU_H

#include <stdbool.h>
#include <stdint.h>

#define SPU_MAX_SIZE 65536

/* Reassembles SPU units from PES payloads (substream id byte removed). */
typedef struct {
    uint8_t buf[SPU_MAX_SIZE];
    int size;   /* expected total size of the unit being assembled, 0 = idle */
    int have;
} spu_assembler_t;

typedef struct {
    int x, y, w, h;     /* display area in frame coordinates */
    uint8_t *pix;       /* w*h pixel values 0..3 */
    uint8_t color[4];   /* CLUT index per pixel value */
    uint8_t alpha[4];   /* 0 (transparent) .. 15 (opaque) per pixel value */
    bool forced;
} spu_image_t;

/* Highlight state for one button: rectangle plus the packed btn_coli word
 * (bits 31-16 colour nibbles e2,e1,p,b; bits 15-0 contrast nibbles). */
typedef struct {
    bool valid;
    int sx, sy, ex, ey;
    uint32_t palette;
} spu_highlight_t;

void spu_asm_reset(spu_assembler_t *a);

/* Feed one PES payload. Returns true when a complete unit is in a->buf
 * (length a->size); the next feed starts a new unit. */
bool spu_asm_feed(spu_assembler_t *a, const uint8_t *p, int len);

/* Decode a complete unit. On success fills *img (img->pix is malloc'd). */
bool spu_decode(const uint8_t *buf, int size, spu_image_t *img);

void spu_image_free(spu_image_t *img);

/* Convert a CLUT entry (0x00YYCrCb, BT.601 limited range) to 0xAARRGGBB
 * with alpha 0. */
uint32_t spu_clut_to_rgb(uint32_t ycrcb);

/* Composite img into a frame-sized RGBA buffer (byte order R,G,B,A,
 * straight alpha), applying hl inside its rectangle. The buffer is cleared
 * first. rgb_clut holds spu_clut_to_rgb() of the 16 CLUT entries. */
void spu_composite(const spu_image_t *img, const uint32_t rgb_clut[16],
                   const spu_highlight_t *hl, uint8_t *rgba, int fw, int fh);

#endif
