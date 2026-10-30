/*
 * libdiscnav: the public API, dispatched to a backend per disc format
 * (dvd.c, bluray.c). See discnav.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#define _GNU_SOURCE
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

struct discnav {
    const nav_backend_t *be;
    nav_impl_t *impl;
};

struct discnav_stream {
    const nav_backend_t *be;
    nav_stream_t *impl;
};

static bool exists(const char *dir, const char *name)
{
    char path[4096];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

discnav_t *discnav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user)
{
    discnav_t *nav = calloc(1, sizeof(*nav));
    if (!nav)
        return NULL;
    /* a directory says what it is; a device or image is tried as a DVD
     * first, which fails fast, unlike a Blu-ray's open (AACS) */
    struct stat st;
    bool dir = stat(device, &st) == 0 && S_ISDIR(st.st_mode);
    bool bd = dir && (exists(device, "BDMV") || exists(device, "index.bdmv"));
    if (!bd && (nav->impl = dvd_open(device, lang, on_event, on_log, user)))
        nav->be = &dvd_backend;
    else if ((!dir || bd) && (nav->impl = bd_nav_open(device, lang, on_event, on_log, user)))
        nav->be = &bd_backend;
    if (!nav->impl) {
        free(nav);
        return NULL;
    }
    return nav;
}

void discnav_close(discnav_t *nav)
{
    if (!nav)
        return;
    nav->be->close(nav->impl);
    free(nav);
}

discnav_format_t discnav_format(discnav_t *nav) { return nav->be->format; }
const char *discnav_title(discnav_t *nav) { return nav->be->title(nav->impl); }

discnav_stream_t *discnav_stream_open(discnav_t *nav)
{
    discnav_stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->be = nav->be;
    if (!(s->impl = nav->be->stream_open(nav->impl))) {
        free(s);
        return NULL;
    }
    return s;
}

int64_t discnav_stream_read(discnav_stream_t *s, void *buf, uint64_t n) { return s->be->stream_read(s->impl, buf, n); }
void discnav_stream_cancel(discnav_stream_t *s) { s->be->stream_cancel(s->impl); }

void discnav_stream_close(discnav_stream_t *s)
{
    s->be->stream_close(s->impl);
    free(s);
}

void discnav_segment_opened(discnav_t *nav) { nav->be->segment_opened(nav->impl); }
bool discnav_stopped(discnav_t *nav) { return nav->be->stopped(nav->impl); }
bool discnav_action(discnav_t *nav, discnav_action_t a) { return nav->be->action(nav->impl, a); }
void discnav_get_tracks(discnav_t *nav, discnav_tracks_t *t) { nav->be->get_tracks(nav->impl, t); }

int discnav_get_streams(discnav_t *nav, discnav_stream_type_t type, discnav_stream_info_t *out, int max)
{
    return nav->be->get_streams(nav->impl, type, out, max);
}

bool discnav_select_stream(discnav_t *nav, discnav_stream_type_t type, int index)
{
    return nav->be->select_stream(nav->impl, type, index);
}

bool discnav_get_position(discnav_t *nav, int *title, double *seconds, double *length)
{
    return nav->be->get_position(nav->impl, title, seconds, length);
}

bool discnav_seek(discnav_t *nav, double seconds) { return nav->be->seek(nav->impl, seconds); }
bool discnav_get_palette(discnav_t *nav, uint32_t rgb[16]) { return nav->be->get_palette(nav->impl, rgb); }
bool discnav_overlay_size(discnav_t *nav, int *w, int *h) { return nav->be->overlay_size(nav->impl, w, h); }

bool discnav_render_overlay(discnav_t *nav, uint8_t *rgba, int w, int h)
{
    return nav->be->render_overlay(nav->impl, rgba, w, h);
}
