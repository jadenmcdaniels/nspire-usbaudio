/* WAV (RIFF) parsing and sample conversion. Integer only, byte-wise reads.
 * The file is read through a callback so the parser needs no buffer of the
 * whole header and works on any chunk layout (LIST/fact/... before fmt or
 * data, odd-sized chunks padded to even). */
#ifndef UA_WAV_H
#define UA_WAV_H

#include <stdint.h>

struct wav_info {
    uint16_t format;            /* 1 PCM (or 0xFFFE with a PCM sub-format) */
    uint16_t channels;          /* 1 or 2 */
    uint16_t bits;              /* 8 or 16 */
    uint16_t block_align;       /* bytes per frame */
    uint32_t rate;              /* 1000..192000 */
    uint32_t data_off, data_len;/* data chunk, clipped to the file and to whole frames */
};

/* Reads n bytes at off into buf; returns bytes read (< n at end of file). */
typedef unsigned (*wav_read_fn)(void *ctx, uint32_t off, void *buf, unsigned n);

/* 0 = ok; otherwise a negative code, *why says what is wrong. */
int wav_parse(wav_read_fn rd, void *ctx, uint32_t file_size, struct wav_info *w, const char **why);

/* Converts whole frames of raw data into 16-bit stereo; returns frames. */
unsigned wav_to_s16(const struct wav_info *w, const uint8_t *in, unsigned in_bytes, int16_t *out_lr);

#endif
