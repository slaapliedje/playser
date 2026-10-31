/*
 * libdiscnav internals: one backend per disc format behind discnav.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Each backend defines its own struct nav_impl and struct nav_stream;
 * discnav.c only passes pointers to them through.
 */
#ifndef DISCNAV_INTERNAL_H
#define DISCNAV_INTERNAL_H

#include "discnav.h"

typedef struct nav_impl nav_impl_t;
typedef struct nav_stream nav_stream_t;

typedef struct {
    discnav_format_t format;
    void (*close)(nav_impl_t *nav);
    const char *(*title)(nav_impl_t *nav);
    nav_stream_t *(*stream_open)(nav_impl_t *nav);
    int64_t (*stream_read)(nav_stream_t *s, void *buf, uint64_t n);
    void (*stream_cancel)(nav_stream_t *s);
    void (*stream_close)(nav_stream_t *s);
    void (*segment_opened)(nav_impl_t *nav);
    bool (*stopped)(nav_impl_t *nav);
    bool (*action)(nav_impl_t *nav, discnav_action_t action);
    void (*get_tracks)(nav_impl_t *nav, discnav_tracks_t *tracks);
    int (*get_streams)(nav_impl_t *nav, discnav_stream_type_t type, discnav_stream_info_t *streams, int max);
    bool (*select_stream)(nav_impl_t *nav, discnav_stream_type_t type, int index);
    bool (*get_position)(nav_impl_t *nav, int *title, double *seconds, double *length);
    int (*get_playlist)(nav_impl_t *nav);
    bool (*seek)(nav_impl_t *nav, double seconds);
    bool (*get_palette)(nav_impl_t *nav, uint32_t rgb[16]);
    bool (*overlay_size)(nav_impl_t *nav, int *w, int *h);
    bool (*render_overlay)(nav_impl_t *nav, uint8_t *rgba, int w, int h);
} nav_backend_t;

nav_impl_t *dvd_open(const char *device, const char *lang,
                     discnav_event_fn on_event, discnav_log_fn on_log, void *user);
extern const nav_backend_t dvd_backend;

nav_impl_t *bd_nav_open(const char *device, const char *lang,
                        discnav_event_fn on_event, discnav_log_fn on_log, void *user);
nav_impl_t *bd_playlists_open(const char *device, const char *lang, const int *playlists, int count,
                              int first, double seconds,
                              discnav_event_fn on_event, discnav_log_fn on_log, void *user);
extern const nav_backend_t bd_backend;

#endif
