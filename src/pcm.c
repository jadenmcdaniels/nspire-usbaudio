#include <string.h>
#include "pcm.h"

#define barrier() __asm__ __volatile__("" ::: "memory")

void pkt_init(struct pkt_sched *s, uint32_t rate)
{
    s->rate = rate;
    s->acc = 0;
}

unsigned pkt_next(struct pkt_sched *s)
{
    unsigned n;
    s->acc += s->rate;
    n = s->acc / 1000u;
    s->acc -= n * 1000u;
    return n;
}

void ring_init(struct ring *r)
{
    r->wr = r->rd = 0;
    r->clear_wr = r->clear_seq = r->cleared_seq = 0;
}

void ring_request_clear(struct ring *r)
{
    r->clear_wr = r->wr;
    barrier();
    r->clear_seq++;
}

unsigned ring_used(const struct ring *r)
{
    return (unsigned) (r->wr - r->rd);
}

unsigned ring_free(const struct ring *r)
{
    return RING_FRAMES - ring_used(r);
}

unsigned ring_put(struct ring *r, const int16_t *lr, unsigned frames)
{
    uint32_t w = r->wr;
    unsigned n = ring_free(r), i;
    if (frames < n)
        n = frames;
    for (i = 0; i < n; i++) {
        uint32_t k = ((w + i) & (RING_FRAMES - 1)) * 2;
        r->buf[k] = lr[2 * i];
        r->buf[k + 1] = lr[2 * i + 1];
    }
    barrier();                  /* the data before the index */
    r->wr = w + n;
    return n;
}

unsigned ring_get(struct ring *r, int16_t *lr, unsigned frames)
{
    uint32_t rd = r->rd;
    uint32_t requested = r->clear_seq;
    if (requested != r->cleared_seq) {
        barrier();
        uint32_t target = r->clear_wr;
        /* A request posted before this packet may already be behind rd.
         * Sequence numbers keep an old target inert across counter wrap. */
        if ((uint32_t)(target - rd) <= RING_FRAMES)
            rd = target;
        r->rd = rd;
        r->cleared_seq = requested;
    }
    unsigned n = ring_used(r), i;
    if (frames < n)
        n = frames;
    barrier();
    for (i = 0; i < n; i++) {
        uint32_t k = ((rd + i) & (RING_FRAMES - 1)) * 2;
        lr[2 * i] = r->buf[k];
        lr[2 * i + 1] = r->buf[k + 1];
    }
    barrier();
    r->rd = rd + n;
    return n;
}

unsigned ring_drop_older(struct ring *r, unsigned keep)
{
    unsigned n;
    (void) ring_get(r, NULL, 0);
    n = ring_used(r);
    if (n <= keep)
        return 0;
    n -= keep;
    barrier();
    r->rd += n;
    return n;
}

unsigned ring_skip(struct ring *r)
{
    return ring_drop_older(r, 0);
}

void rs_init(struct resampler *r, uint32_t in_rate, uint32_t out_rate)
{
    memset(r, 0, sizeof *r);
    if (!in_rate || !out_rate)
        in_rate = out_rate = 1;
    /* (in << 16) / out without 64-bit division: in <= 192000 < 2^18, so
     * split the shift. step = in * 65536 / out. */
    {
        uint32_t q = in_rate / out_rate, rem = in_rate % out_rate, frac = 0, i;
        for (i = 0; i < 16; i++) {          /* long division, one bit at a time */
            rem <<= 1;
            frac <<= 1;
            if (rem >= out_rate) {
                rem -= out_rate;
                frac |= 1;
            }
        }
        r->step = (q << 16) | frac;
    }
    if (!r->step)
        r->step = 1;
}

unsigned rs_run(struct resampler *r, const int16_t *in, unsigned *in_frames, int16_t *out, unsigned out_max)
{
    unsigned used = 0, made = 0, avail = *in_frames;
    if (!r->primed) {
        if (!avail) {
            *in_frames = 0;
            return 0;
        }
        r->prev[0] = r->cur[0] = in[0];
        r->prev[1] = r->cur[1] = in[1];
        used = 1;
        r->primed = 1;
        r->phase = 0x10000u;        /* the loop loads the next frame as cur first */
    }
    for (;;) {
        /* Advance until the output position lies between prev and cur. */
        while (r->phase >= 0x10000u) {
            if (used >= avail)
                goto done;
            r->prev[0] = r->cur[0];
            r->prev[1] = r->cur[1];
            r->cur[0] = in[2 * used];
            r->cur[1] = in[2 * used + 1];
            used++;
            r->phase -= 0x10000u;
        }
        if (made >= out_max)
            break;
        {
            int32_t f = (int32_t) (r->phase >> 1);  /* Q15 */
            out[2 * made] = (int16_t) (r->prev[0] + (((int32_t) (r->cur[0] - r->prev[0]) * f) >> 15));
            out[2 * made + 1] = (int16_t) (r->prev[1] + (((int32_t) (r->cur[1] - r->prev[1]) * f) >> 15));
        }
        made++;
        r->phase += r->step;
    }
done:
    *in_frames = used;
    return made;
}

static void put_sample(const struct dev_fmt *f, int32_t s, uint8_t *o)
{
    switch (f->subframe) {
    case 1:
        o[0] = (uint8_t) ((s >> 8) ^ (f->unsigned8 ? 0x80 : 0));
        break;
    case 2:
        o[0] = (uint8_t) s;
        o[1] = (uint8_t) (s >> 8);
        break;
    case 3:
        o[0] = 0;
        o[1] = (uint8_t) s;
        o[2] = (uint8_t) (s >> 8);
        break;
    default:
        o[0] = o[1] = 0;
        o[2] = (uint8_t) s;
        o[3] = (uint8_t) (s >> 8);
        break;
    }
}

unsigned pcm_write(const struct dev_fmt *f, const int16_t *lr, unsigned frames, unsigned vol, uint8_t *out)
{
    unsigned i, c, sf = f->subframe;
    uint8_t *o = out;
    if (vol > 256)
        vol = 256;
    for (i = 0; i < frames; i++) {
        int32_t l = ((int32_t) lr[2 * i] * (int32_t) vol) >> 8;
        int32_t r = ((int32_t) lr[2 * i + 1] * (int32_t) vol) >> 8;
        if (f->channels == 1) {
            put_sample(f, (l + r) >> 1, o);
            o += sf;
            continue;
        }
        put_sample(f, l, o);
        put_sample(f, r, o + sf);
        o += 2 * sf;
        for (c = 2; c < f->channels; c++, o += sf)
            put_sample(f, 0, o);
    }
    return (unsigned) (o - out);
}

unsigned pcm_silence(const struct dev_fmt *f, unsigned frames, uint8_t *out)
{
    unsigned n = frames * f->channels * f->subframe;
    memset(out, f->subframe == 1 && f->unsigned8 ? 0x80 : 0, n);
    return n;
}
