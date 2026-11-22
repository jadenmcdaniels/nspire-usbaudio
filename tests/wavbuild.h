/* Writes a canonical 44-byte-header PCM WAV into out (tests and tools/mkwav). */
#ifndef UA_WAVBUILD_H
#define UA_WAVBUILD_H

#include <stdint.h>
#include <string.h>

static void wb_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }
static void wb_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

/* samples: interleaved, channels x frames, as 16-bit; bits 8 stores them unsigned. Returns bytes. */
static unsigned wav_build(uint8_t *out, const int16_t *samples, unsigned frames, unsigned channels, unsigned bits,
                          uint32_t rate)
{
    unsigned bps = bits / 8, data = frames * channels * bps, i;
    memcpy(out, "RIFF", 4);
    wb_le32(out + 4, 36 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    wb_le32(out + 16, 16);
    wb_le16(out + 20, 1);
    wb_le16(out + 22, (uint16_t) channels);
    wb_le32(out + 24, rate);
    wb_le32(out + 28, rate * channels * bps);
    wb_le16(out + 32, (uint16_t) (channels * bps));
    wb_le16(out + 34, (uint16_t) bits);
    memcpy(out + 36, "data", 4);
    wb_le32(out + 40, data);
    for (i = 0; i < frames * channels; i++) {
        if (bps == 1)
            out[44 + i] = (uint8_t) ((samples[i] >> 8) + 128);
        else
            wb_le16(out + 44 + 2 * i, (uint16_t) samples[i]);
    }
    return 44 + data;
}

#endif
