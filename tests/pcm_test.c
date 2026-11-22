#include <stdlib.h>
#include "t.h"
#include "pcm.h"
#include "melody.h"

static void test_packets(void)
{
    struct pkt_sched s;
    unsigned i, sum, n44 = 0, n45 = 0;
    pkt_init(&s, 48000);
    for (i = 0; i < 1000; i++)
        T_EQ(pkt_next(&s), 48);
    pkt_init(&s, 44100);
    for (i = 0, sum = 0; i < 10; i++) {
        unsigned n = pkt_next(&s);
        sum += n;
        n44 += n == 44;
        n45 += n == 45;
    }
    T_EQ(sum, 441);
    T_EQ(n44, 9);
    T_EQ(n45, 1);
    for (i = 0, sum = 441; i < 990; i++)
        sum += pkt_next(&s);
    T_EQ(sum, 44100);
    pkt_init(&s, 22050);
    for (i = 0, sum = 0; i < 1000; i++) {
        unsigned n = pkt_next(&s);
        T_TRUE(n == 22 || n == 23);
        sum += n;
    }
    T_EQ(sum, 22050);
    pkt_init(&s, 11025);
    for (i = 0, sum = 0; i < 100000; i++)
        sum += pkt_next(&s);
    T_EQ(sum, 1102500);             /* no drift over 100 s */
}

static void test_ring(void)
{
    static struct ring r;
    int16_t a[2 * 3000], b[2 * 3000];
    unsigned i, got, total = 0;
    ring_init(&r);
    for (i = 0; i < 6000; i++)
        a[i] = (int16_t) i;
    T_EQ(ring_put(&r, a, 3000), 3000);
    T_EQ(ring_used(&r), 3000);
    got = ring_get(&r, b, 3000);
    T_EQ(got, 3000);
    T_MEM(a, b, sizeof a);
    /* wrap many times */
    for (i = 0; i < 50; i++) {
        T_EQ(ring_put(&r, a, 3000), 3000);
        total += ring_get(&r, b, 3000);
        T_MEM(a, b, sizeof a);
    }
    T_EQ(total, 150000);
    for (i = 0, total = 0; i < RING_FRAMES / 3000 + 2; i++)
        total += ring_put(&r, a, 3000);
    T_EQ(total, RING_FRAMES);
    T_EQ(ring_free(&r), 0);
    ring_request_clear(&r);
    T_EQ(ring_get(&r, NULL, 0), 0); /* paused USB consumer applies it */
    T_EQ(ring_used(&r), 0);
    T_EQ(ring_put(&r, a, 100), 100);
    uint32_t before = r.rd;
    ring_request_clear(&r);
    T_EQ(r.rd, before);            /* producer never changes consumer's cursor */
    T_EQ(ring_put(&r, a + 200, 80), 80);
    T_EQ(ring_get(&r, b, 300), 80);
    T_MEM(b, a + 200, 80 * 4);     /* only the newer audio survives */
    T_EQ(ring_get(&r, b, 1), 0);
    ring_init(&r);
    r.wr = r.rd = UINT32_MAX - 31;
    T_EQ(ring_put(&r, a, 100), 100);
    ring_request_clear(&r);       /* the target and counters wrap */
    T_EQ(ring_put(&r, a + 200, 100), 100);
    T_EQ(ring_get(&r, b, 200), 100);
    T_MEM(b, a + 200, 400);
    /* An old clear target is inert when read/write counters wrap again. */
    r.wr = r.rd = r.clear_wr - 10;
    T_EQ(ring_put(&r, a, 100), 100);
    T_EQ(ring_get(&r, b, 100), 100);
    T_MEM(b, a, 400);
}

static unsigned run_rs(uint32_t in_rate, uint32_t out_rate, unsigned in_frames, int16_t *in, int16_t *out,
                       unsigned out_max)
{
    struct resampler r;
    unsigned made = 0, at = 0;
    rs_init(&r, in_rate, out_rate);
    while (at < in_frames) {
        unsigned n = in_frames - at < 333 ? in_frames - at : 333;   /* odd chunks */
        unsigned used = n;
        made += rs_run(&r, in + 2 * at, &used, out + 2 * made, out_max - made);
        at += used;
        if (!used && made >= out_max)
            break;
    }
    return made;
}

static void test_ring_skip(void)
{
    static struct ring r;
    int16_t lr[2 * 8] = { 0 }, out[2 * 8];
    unsigned i;
    ring_init(&r);
    T_EQ(ring_skip(&r), 0);
    for (i = 0; i < 8; i++) lr[2 * i] = (int16_t) (i + 1);
    T_EQ(ring_put(&r, lr, 8), 8);
    ring_request_clear(&r);
    T_EQ(ring_put(&r, lr, 3), 3);
    T_EQ(ring_skip(&r), 3);         /* the clear applied first, then the rest dropped */
    T_EQ(r.cleared_seq, r.clear_seq);
    T_EQ(ring_used(&r), 0);
    T_EQ(ring_put(&r, lr + 2 * 4, 2), 2);
    T_EQ(ring_get(&r, out, 8), 2);
    T_EQ(out[0], 5);
    T_EQ(ring_put(&r, lr, 8), 8);
    T_EQ(ring_drop_older(&r, 10), 0);
    T_EQ(ring_drop_older(&r, 3), 5);    /* the newest 3 stay */
    T_EQ(ring_get(&r, out, 8), 3);
    T_EQ(out[0], 6);
}

static void test_resampler(void)
{
    static int16_t in[2 * 48000], out[2 * 110000];
    unsigned i, n, zc;
    /* DC stays DC */
    for (i = 0; i < 22050; i++) {
        in[2 * i] = 12345;
        in[2 * i + 1] = -20000;
    }
    n = run_rs(22050, 48000, 22050, in, out, 110000);
    T_TRUE(n >= 47990 && n <= 48000);
    for (i = 0; i < n; i++)
        if (out[2 * i] != 12345 || out[2 * i + 1] != -20000)
            break;
    T_EQ(i, n);
    /* full scale square: no overflow, stays in range */
    for (i = 0; i < 22050; i++) {
        in[2 * i] = (i & 8) ? 32767 : -32768;
        in[2 * i + 1] = (i & 3) ? -32768 : 32767;
    }
    n = run_rs(22050, 44100, 22050, in, out, 110000);
    T_TRUE(n >= 44090 && n <= 44100);
    for (i = 0; i < 8 && out[2 * i] == -32768; i++)
        ;
    T_EQ(i, 8);                     /* half rate: each input frame twice, no wrap-around */
    /* a ramp at 22.05 kHz doubled to 44.1 kHz is a ramp with half the step */
    for (i = 0; i < 1000; i++)
        in[2 * i] = in[2 * i + 1] = (int16_t) (i * 60 - 30000);
    n = run_rs(22050, 44100, 1000, in, out, 110000);
    for (i = 1; i + 1 < n; i++)
        if (out[2 * i] - out[2 * i - 2] != 30)
            break;
    T_EQ(i + 1, n);
    /* a 1 kHz square at 48 kHz down to 44.1 kHz keeps 2000 zero crossings per second */
    for (i = 0; i < 48000; i++)
        in[2 * i] = in[2 * i + 1] = ((i / 24) & 1) ? 10000 : -10000;
    n = run_rs(48000, 44100, 48000, in, out, 110000);
    T_TRUE(n >= 44090 && n <= 44100);
    for (i = 1, zc = 0; i < n; i++)
        zc += (out[2 * i] >= 0) != (out[2 * i - 2] >= 0);
    T_TRUE(zc >= 1995 && zc <= 2001);
    /* 8 kHz up to 48 kHz */
    n = run_rs(8000, 48000, 8000, in, out, 110000);
    T_TRUE(n >= 47990 && n <= 48000);
    /* odd rates */
    n = run_rs(11025, 32000, 11025, in, out, 110000);
    T_TRUE(n >= 31990 && n <= 32000);
}

static void test_write(void)
{
    int16_t lr[4] = { 0x1234, -2, 0x7FFF, -32768 };
    uint8_t o[64];
    struct dev_fmt f = { 2, 2, 0 };
    T_EQ(pcm_write(&f, lr, 2, 256, o), 8);
    T_EQ(o[0], 0x34); T_EQ(o[1], 0x12); T_EQ(o[2], 0xFE); T_EQ(o[3], 0xFF);
    T_EQ(o[6], 0x00); T_EQ(o[7], 0x80);
    f.channels = 1;
    T_EQ(pcm_write(&f, lr, 2, 256, o), 4);
    T_EQ((int16_t) (o[0] | o[1] << 8), (0x1234 - 2) >> 1);
    f.channels = 2; f.subframe = 3;
    T_EQ(pcm_write(&f, lr, 1, 256, o), 6);
    T_EQ(o[0], 0); T_EQ(o[1], 0x34); T_EQ(o[2], 0x12);
    f.subframe = 4;
    T_EQ(pcm_write(&f, lr, 1, 256, o), 8);
    T_EQ(o[2], 0x34); T_EQ(o[3], 0x12);
    f.subframe = 1; f.unsigned8 = 1;
    T_EQ(pcm_write(&f, lr, 1, 256, o), 2);
    T_EQ(o[0], 0x92);
    f.channels = 6; f.subframe = 2; f.unsigned8 = 0;
    T_EQ(pcm_write(&f, lr, 1, 128, o), 12);
    T_EQ((int16_t) (o[0] | o[1] << 8), 0x1234 / 2);
    T_EQ(o[4] | o[5] | o[10] | o[11], 0);
    f.channels = 2; f.subframe = 1; f.unsigned8 = 1;
    T_EQ(pcm_silence(&f, 3, o), 6);
    T_EQ(o[5], 0x80);
}

static void test_melody(void)
{
    static int16_t buf[2 * 4096];
    struct melody m;
    unsigned total = 0, n, i, loud = 0;
    int song;
    for (song = 0; song < 2; song++) {
        melody_init(&m, song, 48000);
        T_TRUE(melody_ms(&m) >= 7500 && melody_ms(&m) <= 9000);
        total = 0;
        while ((n = melody_render(&m, buf, 4096)) > 0) {
            for (i = 0; i < 2 * n; i++)
                if (buf[i] > 4000 || buf[i] < -4000)
                    loud++;
            total += n;
        }
        T_EQ(total, m.total);
        T_TRUE(loud > 10000);
    }
    melody_init(&m, SONG_ODE, 44100);
    T_TRUE(melody_ms(&m) >= 7900 && melody_ms(&m) <= 8100);
}

int main(void)
{
    T_RUN(test_packets);
    T_RUN(test_ring);
    T_RUN(test_ring_skip);
    T_RUN(test_resampler);
    T_RUN(test_write);
    T_RUN(test_melody);
    return T_DONE();
}
