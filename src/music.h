#ifndef UA_MUSIC_H
#define UA_MUSIC_H

#include "wav.h"

struct music_track {
    wav_read_fn read;
    void *ctx;
    uint32_t size;
};
struct music_view {
    const char *title, *status, *detail;
    unsigned index, count, elapsed, duration, volume, repeat;
    unsigned playing, connected, error;
};
/* Library and display are MAIN-only. Opening another track closes the
 * previous file; the USB callback only sees the already-produced PCM. */
struct music_platform {
    unsigned count;
    const char *(*title)(unsigned index);
    int (*open)(unsigned index, struct music_track *track);
    void (*draw)(const struct music_view *view);
};
struct app_platform;
int music_run(const struct app_platform *pf);

#endif
