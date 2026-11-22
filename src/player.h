/* What plays: the built-in melody, then the WAV file if there is one,
 * converted to the device's rate and pushed into the ring (MAIN only). */
#ifndef UA_PLAYER_H
#define UA_PLAYER_H

#include <stdint.h>
#include "melody.h"
#include "pcm.h"
#include "wav.h"

enum { PL_MELODY = 0, PL_WAV, PL_DONE };

struct player {
    int stage;
    uint32_t out_rate;
    struct melody mel;
    /* WAV */
    wav_read_fn rd;
    void *rdctx;
    uint32_t file_size;
    struct wav_info wi;
    int wav_ok;
    const char *wav_why;
    uint32_t wav_pos;           /* bytes of data consumed */
    struct resampler rs;
    int16_t pend[2 * 1024];     /* converted input not yet resampled */
    unsigned pend_n, pend_at;
    uint32_t frames_out[2];     /* per stage */
    uint32_t read_errors;
};

/* rd may be NULL (no WAV). */
void player_init(struct player *p, uint32_t out_rate, wav_read_fn rd, void *ctx, uint32_t file_size);
/* Fills the ring (up to max frames); returns frames added. Moves to the
 * next stage by itself at the end of one. */
unsigned player_fill(struct player *p, struct ring *r, unsigned max);
void player_next(struct player *p);
const char *player_stage_name(int stage);

#endif
