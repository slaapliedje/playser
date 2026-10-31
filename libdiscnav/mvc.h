/*
 * libdiscnav: Blu-ray 3D, the dependent view merged into the base view's
 * stream (see mvc.c).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef DISCNAV_MVC_H
#define DISCNAV_MVC_H

#include <stddef.h>
#include <stdint.h>

#include <libbluray/bluray.h>

/* The dependent-view clip ("00050") of each play item of a playlist, from
 * its SS sub-path (type 8). Returns how many play items have one (0: a 2D
 * playlist). */
int mvc_dep_clips(BLURAY *bd, int playlist, char clips[][6], int max);

typedef struct mvc mvc_t;

/* `read_lock`/`read_unlock` are taken around every read of the dependent
 * clip, which goes through `bd` (the caller's lock for it). */
mvc_t *mvc_new(BLURAY *bd, void (*read_lock)(void *), void (*read_unlock)(void *), void *user);
void mvc_free(mvc_t *m);

/* The dependent clip of the play item now playing ("" or NULL: none, the
 * base view passes through). Called with the caller's lock held. Starting
 * over (a new clip, or reset) finds the dependent view again at the base
 * view's next picture. */
void mvc_set_clip(mvc_t *m, const char *clip);
void mvc_reset(mvc_t *m);

/* Takes base-view M2TS (whole 192-byte packets) and queues it with each
 * video PES followed by the dependent view's matching access unit. */
void mvc_feed(mvc_t *m, const uint8_t *buf, size_t n);
/* The video PES still being collected, unmerged (the segment ended). */
void mvc_flush(mvc_t *m);
/* Hands out up to n bytes of the result; returns how many. */
size_t mvc_take(mvc_t *m, uint8_t *dst, size_t n);

#endif
