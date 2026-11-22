/* Truncated, mutated and random input for the descriptor and WAV parsers
 * (built with -fsanitize=address,undefined,alignment). Each input is copied
 * into an exactly-sized heap block so any over-read is caught. */
#include <stdlib.h>
#include "t.h"
#include "uac.h"
#include "wav.h"
#include "wavbuild.h"
#include "corpus.h"

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void sink(void *ctx, const char *l) { (void) ctx; (void) l; }

static void uac_one(const uint8_t *src, unsigned n)
{
    struct uac_info u;
    uint8_t *b = malloc(n ? n : 1);
    uint32_t rate;
    int i;
    memcpy(b, src, n);
    if (uac_parse(b, n, &u) == 0) {
        i = uac_choose(&u, &rate, NULL);
        if (i >= 0) {
            T_TRUE(i < u.nfmt);
            T_TRUE(rate >= 4000 && rate <= 192000);
            T_TRUE(uac_max_packet(&u.fmt[i], rate) <= u.fmt[i].ep_maxpkt);
            T_TRUE(uac_max_packet(&u.fmt[i], rate) <= UAC_PACKET_MAX);
            T_TRUE(u.fmt[i].ep_maxpkt <= UAC_PACKET_MAX);
            uac_feature_unit(&u, &u.fmt[i]);
        }
        T_TRUE(u.nfmt <= UAC_MAX_FMT && u.nunits <= UAC_MAX_UNITS);
    }
    uac_dump(b, n, sink, NULL);
    uac_dump_device(b, n, sink, NULL);
    free(b);
}

struct mem { const uint8_t *p; uint32_t n; };
static unsigned mrd(void *ctx, uint32_t off, void *buf, unsigned n)
{
    struct mem *m = ctx;
    if (off >= m->n) return 0;
    if (n > m->n - off) n = m->n - off;
    memcpy(buf, m->p + off, n);
    return n;
}

static void wav_one(const uint8_t *src, unsigned n)
{
    struct wav_info w;
    struct mem m;
    const char *why;
    uint8_t *b = malloc(n ? n : 1);
    memcpy(b, src, n);
    m.p = b; m.n = n;
    if (wav_parse(mrd, &m, n, &w, &why) == 0) {
        static int16_t out[2 * 4096];
        unsigned len = w.data_len < 4096 ? w.data_len : 4096;
        T_TRUE(w.data_off + w.data_len <= n);
        T_TRUE(w.data_len % w.block_align == 0);
        len -= len % w.block_align;
        T_TRUE(wav_to_s16(&w, b + w.data_off, len, out) == len / w.block_align);
    }
    free(b);
}

static void test_uac_fuzz(void)
{
    const uint8_t *src[] = { cfg_cm108, cfg_headset, cfg_44k, cfg_uac2, cfg_mic };
    unsigned len[] = { sizeof cfg_cm108, sizeof cfg_headset, sizeof cfg_44k, sizeof cfg_uac2, sizeof cfg_mic };
    static uint8_t buf[1024];
    unsigned k, n, it;
    for (k = 0; k < 5; k++)
        for (n = 0; n <= len[k]; n++)
            uac_one(src[k], n);                         /* every truncation */
    for (it = 0; it < 60000; it++) {
        k = rnd() % 5;
        n = len[k];
        memcpy(buf, src[k], n);
        switch (rnd() % 4) {
        case 0: buf[rnd() % n] = (uint8_t) rnd(); break;          /* one byte */
        case 1: { unsigned j, m = 1 + rnd() % 8; for (j = 0; j < m; j++) buf[rnd() % n] = (uint8_t) rnd(); } break;
        case 2: buf[rnd() % n] = (uint8_t) (rnd() % 3); break;    /* tiny lengths */
        default: n = 9 + rnd() % (sizeof buf - 9);                  /* random tail */
            { unsigned j; for (j = len[k] < n ? len[k] : n; j < n; j++) buf[j] = (uint8_t) rnd(); }
            break;
        }
        if (rnd() & 1) { buf[2] = (uint8_t) rnd(); buf[3] = (uint8_t) (rnd() & 7); }
        uac_one(buf, n);
    }
    for (it = 0; it < 20000; it++) {                /* pure noise with a config header */
        n = rnd() % 300;
        for (k = 0; k < n; k++) buf[k] = (uint8_t) rnd();
        if (n >= 2) { buf[0] = 9; buf[1] = 2; }
        uac_one(buf, n);
    }
}

static void test_wav_fuzz(void)
{
    static uint8_t good[4000], buf[4000];
    int16_t s[1000];
    unsigned n, k, it, m;
    for (k = 0; k < 1000; k++) s[k] = (int16_t) rnd();
    n = wav_build(good, s, 400, 2, 16, 22050);
    for (k = 0; k <= n; k++)
        wav_one(good, k);
    for (it = 0; it < 60000; it++) {
        memcpy(buf, good, n);
        m = n;
        switch (rnd() % 4) {
        case 0: buf[rnd() % 48] = (uint8_t) rnd(); break;
        case 1: wb_le32(buf + 4 + 4 * (rnd() % 11), rnd()); break;
        case 2: wb_le32(buf + 16, rnd() % 64); break;           /* fmt chunk size */
        default: m = rnd() % n; break;
        }
        wav_one(buf, m);
    }
    for (it = 0; it < 20000; it++) {
        m = rnd() % 200;
        for (k = 0; k < m; k++) buf[k] = (uint8_t) rnd();
        if (m >= 12) { memcpy(buf, "RIFF", 4); memcpy(buf + 8, "WAVE", 4); }
        if (m >= 20 && (rnd() & 1)) memcpy(buf + 12, "fmt ", 4);
        wav_one(buf, m);
    }
}

int main(void)
{
    corpus_fix();
    T_RUN(test_uac_fuzz);
    T_RUN(test_wav_fuzz);
    return T_DONE();
}
