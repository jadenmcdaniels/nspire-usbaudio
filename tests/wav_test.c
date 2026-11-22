#include <stdlib.h>
#include "t.h"
#include "wav.h"
#include "wavbuild.h"

struct mem { const uint8_t *p; uint32_t n; };
static unsigned mrd(void *ctx, uint32_t off, void *buf, unsigned n)
{
    struct mem *m = ctx;
    if (off >= m->n) return 0;
    if (n > m->n - off) n = m->n - off;
    memcpy(buf, m->p + off, n);
    return n;
}

static void test_basic(void)
{
    static uint8_t f[100000];
    int16_t s[2000];
    int16_t out[2 * 1000];
    struct wav_info w;
    struct mem m;
    const char *why;
    unsigned n, i;
    for (i = 0; i < 2000; i++) s[i] = (int16_t) (i * 31 - 30000);
    n = wav_build(f, s, 1000, 2, 16, 44100);
    m.p = f; m.n = n;
    T_EQ(wav_parse(mrd, &m, n, &w, &why), 0);
    T_EQ(w.channels, 2); T_EQ(w.bits, 16); T_EQ(w.rate, 44100); T_EQ(w.data_off, 44); T_EQ(w.data_len, 4000);
    T_EQ(wav_to_s16(&w, f + 44, 4000, out), 1000);
    T_EQ(out[0], s[0]); T_EQ(out[1], s[1]); T_EQ(out[1999], s[1999]);
    n = wav_build(f, s, 2000, 1, 8, 8000);
    m.n = n;
    T_EQ(wav_parse(mrd, &m, n, &w, &why), 0);
    T_EQ(w.channels, 1); T_EQ(w.bits, 8); T_EQ(w.block_align, 1);
    T_EQ(wav_to_s16(&w, f + 44, 1000, out), 1000);
    T_EQ(out[0], out[1]);
    T_EQ(out[0], (int16_t) ((s[0] / 256 - (s[0] % 256 < 0)) * 256));
}

static void test_chunks(void)
{
    /* LIST chunk with odd size before fmt, extensible fmt, data size larger than the file */
    static uint8_t f[4096];
    struct wav_info w;
    struct mem m;
    const char *why;
    unsigned o = 0;
    memcpy(f, "RIFF\0\0\0\0WAVE", 12); o = 12;
    memcpy(f + o, "LIST", 4); wb_le32(f + o + 4, 5); memcpy(f + o + 8, "abcde", 5); o += 8 + 6;
    memcpy(f + o, "fmt ", 4); wb_le32(f + o + 4, 40);
    wb_le16(f + o + 8, 0xFFFE); wb_le16(f + o + 10, 2); wb_le32(f + o + 12, 22050); wb_le32(f + o + 16, 88200);
    wb_le16(f + o + 20, 4); wb_le16(f + o + 22, 16); wb_le16(f + o + 24, 22); wb_le16(f + o + 26, 16);
    wb_le32(f + o + 28, 3); wb_le16(f + o + 32, 1);
    o += 48;
    memcpy(f + o, "data", 4); wb_le32(f + o + 4, 1000000); o += 8;
    o += 403;                       /* 100 frames and 3 stray bytes */
    wb_le32(f + 4, o - 8);
    m.p = f; m.n = o;
    T_EQ(wav_parse(mrd, &m, o, &w, &why), 0);
    T_EQ(w.format, 0xFFFE);
    T_EQ(w.rate, 22050);
    T_EQ(w.data_len, 400);
    /* float format refused */
    wb_le16(f + 12 + 14 + 8, 3);
    T_TRUE(wav_parse(mrd, &m, o, &w, &why) < 0);
    T_TRUE(strstr(why, "PCM") != NULL);
}

static void test_refusals(void)
{
    static uint8_t f[200];
    int16_t s[40] = { 0 };
    struct wav_info w;
    struct mem m;
    const char *why;
    unsigned n = wav_build(f, s, 20, 2, 16, 48000);
    m.p = f; m.n = n;
    wb_le16(f + 34, 24);
    T_TRUE(wav_parse(mrd, &m, n, &w, &why) < 0);
    wb_le16(f + 34, 16);
    wb_le16(f + 22, 3);
    T_TRUE(wav_parse(mrd, &m, n, &w, &why) < 0);
    wb_le16(f + 22, 2);
    wb_le32(f + 24, 500);
    T_TRUE(wav_parse(mrd, &m, n, &w, &why) < 0);
    wb_le32(f + 24, 48000);
    T_EQ(wav_parse(mrd, &m, n, &w, &why), 0);
    memcpy(f + 8, "WAVX", 4);
    T_TRUE(wav_parse(mrd, &m, n, &w, &why) < 0);
}

int main(void)
{
    T_RUN(test_basic);
    T_RUN(test_chunks);
    T_RUN(test_refusals);
    return T_DONE();
}
