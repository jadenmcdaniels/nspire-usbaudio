/* Integer-only audio plumbing (the calculator has no FPU):
 *   - the per-frame packet size schedule (48 kHz: 48 every ms; 44.1 kHz:
 *     nine of 44 and one of 45 every 10 ms),
 *   - a single-producer single-consumer ring of 16-bit stereo frames (the
 *     main program fills it, the OS's USB task empties it),
 *   - a linear-interpolation resampler (Q16 phase),
 *   - the writer that turns 16-bit stereo into the device's format.
 * Pure code: safe in the OS's USB task. */
#ifndef UA_PCM_H
#define UA_PCM_H

#include <stdint.h>

/* ---- packet schedule ---- */
struct pkt_sched {
    uint32_t rate;              /* samples per second */
    uint32_t acc;               /* remainder, in 1/1000 samples */
};
void pkt_init(struct pkt_sched *s, uint32_t rate);
unsigned pkt_next(struct pkt_sched *s);     /* sample frames in the next 1 ms packet */

/* ---- ring of stereo frames (L, R int16) ---- */
#ifndef RING_FRAMES
#define RING_FRAMES 32768u      /* power of two: 0.68 s at 48 kHz, room for a slow log write */
#endif
#if !RING_FRAMES || (RING_FRAMES & (RING_FRAMES - 1))
#error RING_FRAMES must be a nonzero power of two
#endif
struct ring {
    int16_t buf[RING_FRAMES * 2];
    volatile uint32_t wr, rd; /* free-running frame counters: wr by the producer only, rd by the consumer only */
    volatile uint32_t clear_wr, clear_seq, cleared_seq;
};
void ring_init(struct ring *r);
/* Producer asks to discard everything written so far. The consumer applies
 * it in ring_get, preserving frames put after the request. Unlike ring_init
 * this is safe while the USB task owns the ring. */
void ring_request_clear(struct ring *r);
unsigned ring_used(const struct ring *r);
unsigned ring_free(const struct ring *r);
unsigned ring_put(struct ring *r, const int16_t *lr, unsigned frames);   /* frames taken */
unsigned ring_get(struct ring *r, int16_t *lr, unsigned frames);         /* frames given */
/* Consumer: applies a discard request and drops everything in the ring
 * (the frames the producer put before the call). Returns frames dropped. */
unsigned ring_skip(struct ring *r);
/* The same, but the newest keep frames stay (the producer's priming). */
unsigned ring_drop_older(struct ring *r, unsigned keep);

/* ---- resampler: stereo int16 in, stereo int16 out ---- */
struct resampler {
    uint32_t step;              /* Q16 input frames per output frame */
    uint32_t phase;             /* Q16 position between prev (0) and cur (1<<16) */
    int16_t  prev[2], cur[2];
    uint8_t  primed;
};
void rs_init(struct resampler *r, uint32_t in_rate, uint32_t out_rate);
/* Consumes up to *in_frames input frames, writes up to out_max output frames.
 * Returns frames written; *in_frames is set to the frames consumed. */
unsigned rs_run(struct resampler *r, const int16_t *in, unsigned *in_frames, int16_t *out, unsigned out_max);

/* ---- device format ---- */
struct dev_fmt {
    uint8_t channels;           /* 1..8 */
    uint8_t subframe;           /* bytes per sample, 1..4 */
    uint8_t unsigned8;          /* PCM8 (format tag 2): 8-bit samples are unsigned */
};
/* Writes frames stereo frames into out in the device's format, scaled by
 * vol (0..256, 256 = unchanged). Returns bytes written. */
unsigned pcm_write(const struct dev_fmt *f, const int16_t *lr, unsigned frames, unsigned vol, uint8_t *out);
/* Silence in the device's format. */
unsigned pcm_silence(const struct dev_fmt *f, unsigned frames, uint8_t *out);

#endif
