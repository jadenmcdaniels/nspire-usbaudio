/* A tiny two-voice synthesiser for the built-in test tunes (public-domain
 * melodies, notes typed in here: no recorded audio). Integer only: 32-bit
 * phase accumulators, a triangle lead, a soft square bass, a linear
 * attack/decay/release envelope so notes don't click. */
#ifndef UA_MELODY_H
#define UA_MELODY_H

#include <stdint.h>

#define SONG_ODE     0          /* Beethoven, "Ode to Joy" theme (~8 s) */
#define SONG_TWINKLE 1          /* "Twinkle, Twinkle, Little Star" (~8 s), used for the test WAV */

struct note { uint8_t midi, eighths; };     /* midi 0 = rest */

struct voice {
    const struct note *notes;
    unsigned count, idx;
    uint32_t left, len;         /* frames left in / length of the current note */
    uint32_t phase, inc;
    uint32_t eighth;            /* frames per eighth note */
    int32_t  amp;               /* peak, 0..32767 */
    uint8_t  square;
};

struct melody {
    uint32_t rate, eighth;      /* frames per eighth note */
    struct voice v[2];
    uint32_t total, done;       /* frames */
};

void     melody_init(struct melody *m, int song, uint32_t rate);
/* Renders up to frames stereo frames; returns how many (0 = the end). */
unsigned melody_render(struct melody *m, int16_t *out_lr, unsigned frames);
uint32_t melody_ms(const struct melody *m);

#endif
