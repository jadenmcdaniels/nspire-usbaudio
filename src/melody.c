#include <string.h>
#include "melody.h"

/* MIDI note numbers: C4 = 60. Durations in eighth notes. */
#define C3 48
#define D3 50
#define E3 52
#define F3 53
#define G2 43
#define G3 55
#define A2 45
#define A3 57
#define C4 60
#define D4 62
#define E4 64
#define F4 65
#define G4 67
#define A4 69

static const struct note ode_lead[] = {
    {E4,2},{E4,2},{F4,2},{G4,2}, {G4,2},{F4,2},{E4,2},{D4,2}, {C4,2},{C4,2},{D4,2},{E4,2}, {E4,3},{D4,1},{D4,4},
    {E4,2},{E4,2},{F4,2},{G4,2}, {G4,2},{F4,2},{E4,2},{D4,2}, {C4,2},{C4,2},{D4,2},{E4,2}, {D4,3},{C4,1},{C4,4},
};
static const struct note ode_bass[] = {
    {C3,8},{G2,8},{C3,8},{G2,8}, {C3,8},{G2,8},{A2,4},{G2,4},{C3,8},
};
static const struct note tw_lead[] = {
    {C4,2},{C4,2},{G4,2},{G4,2},{A4,2},{A4,2},{G4,4}, {F4,2},{F4,2},{E4,2},{E4,2},{D4,2},{D4,2},{C4,4},
    {G4,2},{G4,2},{F4,2},{F4,2},{E4,2},{E4,2},{D4,4}, {G4,2},{G4,2},{F4,2},{F4,2},{E4,2},{E4,2},{D4,4},
};
static const struct note tw_bass[] = {
    {C3,8},{F3,4},{C3,4}, {F3,4},{C3,4},{G3,4},{C3,4}, {C3,4},{F3,4},{C3,4},{G3,4}, {C3,4},{F3,4},{C3,4},{G3,4},
};

/* C4..B4 in millihertz (equal temperament, A4 = 440 Hz). */
static const uint32_t oct4_mhz[12] = {
    261626, 277183, 293665, 311127, 329628, 349228, 369994, 391995, 415305, 440000, 466164, 493883,
};

static uint32_t note_inc(uint8_t midi, uint32_t rate)
{
    int oct = midi / 12 - 5;                /* MIDI 60..71 is octave 4 */
    uint32_t mhz = oct4_mhz[midi % 12];
    uint64_t num;
    while (oct > 0) { mhz <<= 1; oct--; }
    while (oct < 0) { mhz >>= 1; oct++; }
    num = (uint64_t) mhz << 32;
    return (uint32_t) (num / ((uint64_t) rate * 1000u));
}

static void voice_start(struct voice *v, uint32_t eighth, uint32_t rate)
{
    const struct note *n = &v->notes[v->idx];
    v->len = v->left = n->eighths * eighth;
    v->inc = n->midi ? note_inc(n->midi, rate) : 0;
}

static uint32_t voice_total(const struct note *n, unsigned count, uint32_t eighth)
{
    uint32_t t = 0;
    unsigned i;
    for (i = 0; i < count; i++)
        t += n[i].eighths * eighth;
    return t;
}

void melody_init(struct melody *m, int song, uint32_t rate)
{
    memset(m, 0, sizeof *m);
    if (rate < 1000)
        rate = 1000;
    m->rate = rate;
    m->eighth = rate / 8;                   /* 125 ms: quarter = 250 ms (240 per minute) */
    if (song == SONG_TWINKLE) {
        m->v[0].notes = tw_lead;  m->v[0].count = sizeof tw_lead / sizeof tw_lead[0];
        m->v[1].notes = tw_bass;  m->v[1].count = sizeof tw_bass / sizeof tw_bass[0];
        m->eighth = rate * 13 / 100;        /* 130 ms */
    } else {
        m->v[0].notes = ode_lead; m->v[0].count = sizeof ode_lead / sizeof ode_lead[0];
        m->v[1].notes = ode_bass; m->v[1].count = sizeof ode_bass / sizeof ode_bass[0];
    }
    m->v[0].eighth = m->v[1].eighth = m->eighth;
    m->v[0].amp = 9000;
    m->v[1].amp = 3500;
    m->v[1].square = 1;
    voice_start(&m->v[0], m->eighth, rate);
    voice_start(&m->v[1], m->eighth, rate);
    m->total = voice_total(m->v[0].notes, m->v[0].count, m->eighth);
}

uint32_t melody_ms(const struct melody *m)
{
    return (uint32_t) ((uint64_t) m->total * 1000u / m->rate);
}

/* Envelope 0..256: 5 ms attack, decay to 70 % over 80 ms, 30 ms release. */
static int32_t env(const struct voice *v, uint32_t rate)
{
    uint32_t pos = v->len - v->left, att = rate / 200, dec = rate / 12, rel = rate / 33;
    int32_t e;
    if (pos < att)
        e = (int32_t) (pos * 256 / att);
    else if (pos < att + dec)
        e = 256 - (int32_t) ((pos - att) * 77 / dec);
    else
        e = 179;
    if (v->left < rel)
        e = e * (int32_t) v->left / (int32_t) rel;
    return e;
}

static int32_t voice_sample(struct voice *v, uint32_t rate)
{
    int32_t s = 0;
    if (v->idx >= v->count)
        return 0;
    if (v->inc) {
        uint32_t t = v->phase >> 16;
        if (v->square)
            s = (t < 32768 ? 16384 : -16384);
        else
            s = (int32_t) (t < 32768 ? t : 65535 - t) * 2 - 32768;
        s = (s * v->amp) >> 15;
        s = (s * env(v, rate)) >> 8;
        v->phase += v->inc;
    }
    if (--v->left == 0) {
        v->idx++;
        if (v->idx < v->count)
            voice_start(v, v->eighth, rate);
    }
    return s;
}

unsigned melody_render(struct melody *m, int16_t *out, unsigned frames)
{
    unsigned i;
    for (i = 0; i < frames && m->done < m->total; i++, m->done++) {
        int32_t a = voice_sample(&m->v[0], m->rate), b = voice_sample(&m->v[1], m->rate);
        int32_t l = a + (b * 3 >> 2), r = a + b;    /* bass a little right of centre */
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[2 * i] = (int16_t) l;
        out[2 * i + 1] = (int16_t) r;
    }
    return i;
}
