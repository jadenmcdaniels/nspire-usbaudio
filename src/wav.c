#include <string.h>
#include "wav.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

#define FAIL(code, text) do { if (why) *why = (text); return (code); } while (0)

int wav_parse(wav_read_fn rd, void *ctx, uint32_t size, struct wav_info *w, const char **why)
{
    uint8_t h[40];
    uint32_t off = 12, riff_end;
    int have_fmt = 0;

    memset(w, 0, sizeof *w);
    if (size < 12 || rd(ctx, 0, h, 12) != 12)
        FAIL(-1, "file too short for a WAV header");
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        FAIL(-2, "not a RIFF/WAVE file");
    riff_end = rd32(h + 4);
    riff_end = riff_end > size - 8 ? size : riff_end + 8;  /* a lying RIFF size: trust the file */
    while (off <= riff_end - 8 && off <= size - 8) {
        uint32_t id_len, clen, avail;
        if (rd(ctx, off, h, 8) != 8)
            FAIL(-3, "truncated chunk header");
        clen = rd32(h + 4);
        avail = size - (off + 8);
        if (!memcmp(h, "fmt ", 4)) {
            unsigned n = clen < sizeof h ? (unsigned) clen : (unsigned) sizeof h;
            if (clen < 16 || avail < 16)
                FAIL(-4, "fmt chunk too short");
            if (n > avail)
                n = (unsigned) avail;
            if (rd(ctx, off + 8, h, n) != n)
                FAIL(-4, "fmt chunk truncated");
            w->format = rd16(h);
            w->channels = rd16(h + 2);
            w->rate = rd32(h + 4);
            w->block_align = rd16(h + 12);
            w->bits = rd16(h + 14);
            if (w->format == 0xFFFE) {
                if (n < 26 || rd16(h + 24) != 1)
                    FAIL(-5, "extensible WAV that isn't PCM");
            } else if (w->format != 1) {
                FAIL(-5, "not PCM (compressed or float WAV)");
            }
            if (w->channels < 1 || w->channels > 2)
                FAIL(-6, "only mono or stereo");
            if (w->bits != 8 && w->bits != 16)
                FAIL(-7, "only 8 or 16 bits per sample");
            if (w->rate < 1000 || w->rate > 192000)
                FAIL(-8, "sample rate out of range");
            if (w->block_align != w->channels * (w->bits / 8))
                FAIL(-9, "block align doesn't match channels and bits");
            have_fmt = 1;
        } else if (!memcmp(h, "data", 4)) {
            if (!have_fmt)
                FAIL(-10, "data chunk before fmt chunk");
            w->data_off = off + 8;
            w->data_len = clen < avail ? clen : avail;
            w->data_len -= w->data_len % w->block_align;
            if (!w->data_len)
                FAIL(-11, "no samples");
            if (why)
                *why = "ok";
            return 0;
        }
        id_len = clen + (clen & 1);
        if (id_len > size || off + 8 > size - id_len)
            break;
        off += 8 + id_len;
    }
    FAIL(have_fmt ? -12 : -13, have_fmt ? "no data chunk" : "no fmt chunk");
}

unsigned wav_to_s16(const struct wav_info *w, const uint8_t *in, unsigned in_bytes, int16_t *out)
{
    unsigned n = w->block_align ? in_bytes / w->block_align : 0, i;
    for (i = 0; i < n; i++) {
        int16_t l, r;
        if (w->bits == 8) {
            l = (int16_t) (((int) in[0] - 128) * 256);
            r = w->channels == 2 ? (int16_t) (((int) in[1] - 128) * 256) : l;
        } else {
            l = (int16_t) rd16(in);
            r = w->channels == 2 ? (int16_t) rd16(in + 2) : l;
        }
        out[2 * i] = l;
        out[2 * i + 1] = r;
        in += w->block_align;
    }
    return n;
}
