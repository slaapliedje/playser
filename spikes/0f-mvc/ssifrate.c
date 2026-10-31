// Spike: time reading a 3D clip's interleaved SSIF file, decrypted.
#include <libbluray/bluray.h>
#include <libbluray/filesystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv)
{
    BLURAY *bd = bd_open(argv[1], NULL);
    BD_FILE_H *f = bd ? bd_open_file_dec(bd, argv[2]) : NULL;
    if (!f) { fprintf(stderr, "open failed\n"); return 1; }
    int64_t size = f->seek(f, 0, SEEK_END), want = atoll(argv[3]) << 20;
    f->seek(f, (int64_t)(size * atof(argv[4])) / 6144 * 6144, SEEK_SET);
    static unsigned char buf[6144]; // decrypted reads: one aligned unit at a time
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    int64_t got = 0; long n0x1011 = 0, n0x1012 = 0;
    while (got < want) {
        int64_t r = f->read(f, buf, sizeof buf);
        if (r <= 0) break;
        for (int i = 0; i + 192 <= r; i += 192) {
            int pid = ((buf[i + 5] & 0x1f) << 8) | buf[i + 6];
            n0x1011 += pid == 0x1011; n0x1012 += pid == 0x1012;
        }
        got += r;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = b.tv_sec - a.tv_sec + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("size %lld, read %lld MB in %.1f s = %.1f MB/s (%.0f Mb/s); packets base %ld dep %ld\n",
           (long long)size, (long long)(got >> 20), s, got / s / 1e6, got * 8 / s / 1e6, n0x1011, n0x1012);
    f->close(f); bd_close(bd);
    return 0;
}
