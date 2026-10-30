/* Spike 0e: libbluray navigation mode (bd_play + bd_read_ext) with BD-J.
 * Logs events and saves the menu graphics (ARGB planes) on flushes.
 *
 *   bdj <disc> <outdir> <seconds> [t:key ...]    keys: up down left right
 *                                                enter menu popup
 * Data is read at roughly playback rate and discarded. */
#include <libbluray/bluray.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *outdir;
static double t0;
static BD_ARGB_BUFFER argb;
static uint32_t *planes[3];
static int pw[3], ph[3];
static int shots;
static double last_shot[3] = { -9, -9, -9 };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9 - t0;
}

static void save(int plane, const char *why)
{
    if (!planes[plane])
        return;
    long opaque = 0;
    for (long i = 0; i < (long)pw[plane] * ph[plane]; i++)
        opaque += (planes[plane][i] >> 24) != 0;
    char path[512];
    snprintf(path, sizeof path, "%s/p%d-%03d-%.1fs.argb", outdir, plane, ++shots, now());
    FILE *f = fopen(path, "wb");
    fwrite(&pw[plane], 4, 1, f);
    fwrite(&ph[plane], 4, 1, f);
    fwrite(planes[plane], 4, (size_t)pw[plane] * ph[plane], f);
    fclose(f);
    printf("%7.2f  saved %s (%s, %ld px drawn)\n", now(), path, why, opaque);
}

static void argb_proc(void *h, const BD_ARGB_OVERLAY *ov)
{
    (void)h;
    if (!ov) {
        printf("%7.2f  argb: close all\n", now());
        return;
    }
    int p = ov->plane;
    switch (ov->cmd) {
    case BD_ARGB_OVERLAY_INIT:
        printf("%7.2f  argb init plane %d %dx%d at %d,%d\n", now(), p, ov->w, ov->h, ov->x, ov->y);
        free(planes[p]);
        planes[p] = calloc((size_t)ov->w * ov->h, 4);
        pw[p] = ov->w, ph[p] = ov->h;
        argb.buf[p] = planes[p];
        argb.width = ov->w, argb.height = ov->h;
        break;
    case BD_ARGB_OVERLAY_CLOSE:
        printf("%7.2f  argb close plane %d\n", now(), p);
        break;
    case BD_ARGB_OVERLAY_DRAW:
        break;
    case BD_ARGB_OVERLAY_FLUSH:
        if (now() - last_shot[p] > 3) {
            last_shot[p] = now();
            save(p, "flush");
        }
        break;
    }
}

static void ov_proc(void *h, const BD_OVERLAY *ov)
{
    (void)h;
    if (ov)
        printf("%7.2f  hdmv overlay plane %d cmd %d %dx%d at %d,%d\n", now(), ov->plane, ov->cmd, ov->w, ov->h, ov->x, ov->y);
}

static const char *ev_name(int e)
{
    static const char *n[] = { "NONE", "ERROR", "READ_ERROR", "ENCRYPTED", "ANGLE", "TITLE", "PLAYLIST", "PLAYITEM",
        "CHAPTER", "PLAYMARK", "END_OF_TITLE", "AUDIO_STREAM", "IG_STREAM", "PG_TEXTST_STREAM", "PIP_PG", "SEC_AUDIO_STREAM",
        "SEC_VIDEO_STREAM", "PG_TEXTST", "PIP_PG_TEXTST", "SEC_AUDIO", "SEC_VIDEO", "SEC_VIDEO_SIZE", "PLAYLIST_STOP",
        "DISCONTINUITY", "SEEK", "STILL", "STILL_TIME", "SOUND_EFFECT", "IDLE", "POPUP", "MENU", "STEREO", "KEY_INTEREST",
        "UO_MASK" };
    return e >= 0 && e < (int)(sizeof n / sizeof *n) ? n[e] : "?";
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: bdj <disc> <outdir> <seconds> [t:key ...]\n");
        return 2;
    }
    outdir = argv[2];
    double secs = atof(argv[3]);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t0 = ts.tv_sec + ts.tv_nsec / 1e9;

    BLURAY *bd = bd_open(argv[1], NULL);
    if (!bd) {
        fprintf(stderr, "bd_open failed\n");
        return 1;
    }
    const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
    printf("%7.2f  opened: bluray %d, aacs %d/%d, bdplus %d/%d, first play %d, top menu %d, hdmv titles %u, bdj titles %u\n",
           now(), di->bluray_detected, di->aacs_detected, di->aacs_handled, di->bdplus_detected, di->bdplus_handled,
           di->first_play_supported, di->top_menu_supported, di->num_hdmv_titles, di->num_bdj_titles);
    printf("%7.2f  bdj: detected %d, jvm %d, handled %d, org %s disc %s\n", now(), di->bdj_detected,
           di->libjvm_detected, di->bdj_handled, di->bdj_org_id, di->bdj_disc_id);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_MENU_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_PG_LANG, "eng");
    bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, "us");
    bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_REGION_CODE, 1); /* A */
    bd_register_overlay_proc(bd, NULL, ov_proc);
    bd_register_argb_overlay_proc(bd, NULL, argb_proc, &argb);
    if (!bd_play(bd)) {
        fprintf(stderr, "bd_play failed\n");
        return 1;
    }
    printf("%7.2f  playing\n", now());

    static unsigned char buf[6144 * 10];
    double bytes = 0;
    int nk = 4;
    while (now() < secs) {
        for (int i = 4; i < argc; i++) {
            double t = atof(argv[i]);
            const char *k = strchr(argv[i], ':');
            if (i == nk && k && now() >= t) {
                k++;
                uint32_t key = !strcmp(k, "up") ? BD_VK_UP : !strcmp(k, "down") ? BD_VK_DOWN
                             : !strcmp(k, "left") ? BD_VK_LEFT : !strcmp(k, "right") ? BD_VK_RIGHT
                             : !strcmp(k, "enter") ? BD_VK_ENTER : !strcmp(k, "menu") ? BD_VK_ROOT_MENU
                             : BD_VK_POPUP;
                int r1 = bd_user_input(bd, -1, key | BD_VK_KEY_PRESSED);
                int r2 = bd_user_input(bd, -1, key | BD_VK_KEY_TYPED);
                int r3 = bd_user_input(bd, -1, key | BD_VK_KEY_RELEASED);
                printf("%7.2f  key %s -> %d %d %d\n", now(), k, r1, r2, r3);
                nk++;
                for (int p = 0; p < 3; p++)
                    last_shot[p] = -9; /* next flush saves */
            }
        }
        BD_EVENT ev;
        int n = bd_read_ext(bd, buf, sizeof buf, &ev);
        while (ev.event != BD_EVENT_NONE) {
            if (ev.event != BD_EVENT_IDLE)
                printf("%7.2f  event %s %u\n", now(), ev_name(ev.event), ev.param);
            if (ev.event == BD_EVENT_ERROR)
                return 1;
            if (!bd_get_event(bd, &ev))
                break;
        }
        if (n < 0) {
            printf("%7.2f  read error\n", now());
            return 1;
        }
        bytes += n;
        if (n == 0)
            usleep(40000);
        /* pace at ~60 Mbit/s */
        double ahead = bytes / 7.5e6 - now();
        if (ahead > 0)
            usleep((useconds_t)(ahead * 1e6));
    }
    for (int p = 0; p < 3; p++)
        save(p, "end");
    printf("%7.2f  %.1f MB read\n", now(), bytes / 1e6);
    bd_close(bd);
    return 0;
}
