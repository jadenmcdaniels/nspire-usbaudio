#include <string.h>
#include "player.h"

#define CHUNK 4096u                 /* bytes read from the WAV at a time */

void player_init(struct player *p, uint32_t out_rate, wav_read_fn rd, void *ctx, uint32_t file_size)
{
    memset(p, 0, sizeof *p);
    p->out_rate = out_rate;
    p->rd = rd;
    p->rdctx = ctx;
    p->file_size = file_size;
    p->wav_why = rd ? "not opened yet" : "no file";
    melody_init(&p->mel, SONG_ODE, out_rate);
    p->stage = PL_MELODY;
}

static void start_wav(struct player *p)
{
    p->stage = PL_WAV;
    if (!p->rd) {
        p->stage = PL_DONE;
        return;
    }
    p->wav_ok = wav_parse(p->rd, p->rdctx, p->file_size, &p->wi, &p->wav_why) == 0;
    if (!p->wav_ok) {
        p->stage = PL_DONE;
        return;
    }
    rs_init(&p->rs, p->wi.rate, p->out_rate);
    p->wav_pos = 0;
    p->pend_n = p->pend_at = 0;
}

void player_next(struct player *p)
{
    if (p->stage == PL_MELODY)
        start_wav(p);
    else
        p->stage = PL_DONE;
}

static unsigned fill_wav(struct player *p, int16_t *out, unsigned max)
{
    static uint8_t raw[CHUNK];
    unsigned made = 0;
    while (made < max) {
        unsigned used, n;
        if (p->pend_at >= p->pend_n) {
            uint32_t left = p->wi.data_len - p->wav_pos, want, got;
            unsigned frames_cap = (unsigned) (sizeof p->pend / sizeof p->pend[0] / 2);
            if (!left)
                break;
            want = CHUNK - CHUNK % p->wi.block_align;
            if (want > frames_cap * p->wi.block_align)
                want = frames_cap * p->wi.block_align;
            if (want > left)
                want = left;
            got = p->rd(p->rdctx, p->wi.data_off + p->wav_pos, raw, want);
            got -= got % p->wi.block_align;
            if (!got) {
                p->read_errors++;
                p->wav_pos = p->wi.data_len;    /* end it */
                break;
            }
            p->wav_pos += got;
            p->pend_n = wav_to_s16(&p->wi, raw, got, p->pend);
            p->pend_at = 0;
        }
        used = p->pend_n - p->pend_at;
        n = rs_run(&p->rs, p->pend + 2 * p->pend_at, &used, out + 2 * made, max - made);
        p->pend_at += used;
        made += n;
        if (!n && !used)
            break;
    }
    return made;
}

unsigned player_fill(struct player *p, struct ring *r, unsigned max)
{
    static int16_t buf[2 * 512];
    unsigned total = 0;
    while (p->stage != PL_DONE && total < max) {
        unsigned room = ring_free(r), want, got;
        if (room > max - total)
            room = max - total;
        if (room == 0)
            break;
        want = room < 512 ? room : 512;
        got = p->stage == PL_MELODY ? melody_render(&p->mel, buf, want) : fill_wav(p, buf, want);
        if (!got) {
            player_next(p);
            continue;
        }
        ring_put(r, buf, got);
        p->frames_out[p->stage] += got;
        total += got;
    }
    return total;
}

const char *player_stage_name(int stage)
{
    return stage == PL_MELODY ? "built-in melody" : stage == PL_WAV ? "music.wav" : "done";
}
