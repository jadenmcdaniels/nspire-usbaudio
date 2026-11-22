#include <stdlib.h>
#include "t.h"
#include "uac.h"
#include "corpus.h"

static unsigned lines;
static void count(void *ctx, const char *l)
{
    (void) ctx;
    lines++;
    if (getenv("UA_SHOW_DUMP"))
        printf("%s\n", l);
}

static void test_cm108(void)
{
    struct uac_info u;
    uint32_t rate;
    int i;
    const struct uac_unit *fu;
    T_EQ(uac_parse(cfg_cm108, sizeof cfg_cm108, &u), 0);
    T_EQ(u.audio, 1);
    T_EQ(u.ac_ifnum, 0);
    T_EQ(u.ac_ver, 1);
    T_EQ(u.nfmt, 2);
    T_EQ(u.n_out, 1);
    T_EQ(u.n_in, 1);
    T_EQ(u.truncated, 0);
    i = uac_choose(&u, &rate, NULL);
    T_EQ(i, 0);
    T_EQ(rate, 44100);              /* 180-byte packets: one start-split; 48 kHz would be 192 */
    T_EQ(u.fmt[0].ifnum, 1);
    T_EQ(u.fmt[0].alt, 1);
    T_EQ(u.fmt[0].channels, 2);
    T_EQ(u.fmt[0].subframe, 2);
    T_EQ(u.fmt[0].bits, 16);
    T_EQ(u.fmt[0].nrates, 2);
    T_EQ(u.fmt[0].rates[1], 44100);
    T_EQ(u.fmt[0].ep_addr, 0x01);
    T_EQ(u.fmt[0].ep_sync, UAC_SYNC_ADAPTIVE);
    T_EQ(u.fmt[0].ep_maxpkt, 200);
    T_EQ(u.fmt[0].cs_ep_attr & 1, 1);
    T_EQ(u.fmt[0].terminal_link, 1);
    T_EQ(u.fmt[1].dir_in, 1);
    T_EQ(u.fmt[1].ep_sync, UAC_SYNC_ASYNC);
    fu = uac_feature_unit(&u, &u.fmt[0]);
    T_TRUE(fu != NULL);
    if (fu) {
        T_EQ(fu->id, 9);
        T_EQ(fu->ctl_master, 1);
        T_EQ(fu->ctl_ch1, 2);
        T_EQ(fu->ctl_ch2, 2);
    }
    T_EQ(uac_max_packet(&u.fmt[0], 48000), 192);
    lines = 0;
    uac_dump(cfg_cm108, sizeof cfg_cm108, count, NULL);
    T_TRUE(lines > 40);
}

static void test_headset(void)
{
    struct uac_info u;
    uint32_t rate;
    const struct uac_unit *fu;
    int i;
    T_EQ(uac_parse(cfg_headset, sizeof cfg_headset, &u), 0);
    T_EQ(u.nfmt, 2);
    i = uac_choose(&u, &rate, NULL);
    T_EQ(i, 0);
    T_EQ(rate, 44100);
    T_EQ(u.fmt[0].nrates, 4);
    T_EQ(u.fmt[0].ep_sync, UAC_SYNC_SYNC);
    T_EQ(u.fmt[0].ep_addr, 0x02);
    T_EQ(u.fmt[0].cs_ep_attr & 1, 1);
    T_EQ(u.fmt[1].ifnum, 2);
    T_EQ(u.fmt[1].dir_in, 1);
    fu = uac_feature_unit(&u, &u.fmt[0]);
    T_TRUE(fu != NULL);
    if (fu) {
        T_EQ(fu->id, 2);
        T_EQ(fu->ctl_master, 1);
        T_EQ(fu->ctl_ch1, 2);
    }
}

static void test_44k(void)
{
    struct uac_info u;
    uint32_t rate;
    T_EQ(uac_parse(cfg_44k, sizeof cfg_44k, &u), 0);
    T_EQ(u.nfmt, 1);
    T_EQ(uac_choose(&u, &rate, NULL), 0);
    T_EQ(rate, 44100);
    T_EQ(u.fmt[0].ep_sync, UAC_SYNC_ASYNC);
    T_EQ(u.fmt[0].fb_addr, 0x83);
    T_EQ(u.fmt[0].ep_maxpkt, 180);
    T_EQ(u.fmt[0].cs_ep_attr, 0);
    T_TRUE(uac_feature_unit(&u, &u.fmt[0]) == NULL);
    T_EQ(uac_max_packet(&u.fmt[0], 44100), 180);
}

static void test_uac2(void)
{
    struct uac_info u;
    uint32_t rate;
    const char *why = NULL;
    T_EQ(uac_parse(cfg_uac2, sizeof cfg_uac2, &u), 0);
    T_EQ(u.any_uac2, 1);
    T_EQ(u.ac_ver, 2);
    T_EQ(u.nfmt, 1);
    T_EQ(u.fmt[0].uac_ver, 2);
    T_EQ(u.fmt[0].channels, 2);
    T_EQ(u.fmt[0].subframe, 4);
    T_EQ(u.fmt[0].bits, 24);
    T_EQ(u.fmt[0].fb_addr, 0x81);
    T_EQ(uac_choose(&u, &rate, &why), -1);
    T_TRUE(why && strstr(why, "Class 2"));
    lines = 0;
    uac_dump(cfg_uac2, sizeof cfg_uac2, count, NULL);
    T_TRUE(lines > 15);
}

static void test_mic_only(void)
{
    struct uac_info u;
    const char *why = NULL;
    T_EQ(uac_parse(cfg_mic, sizeof cfg_mic, &u), 0);
    T_EQ(u.n_out, 0);
    T_EQ(u.n_in, 1);
    T_EQ(uac_choose(&u, NULL, &why), -1);
    T_TRUE(why && strstr(why, "microphone"));
}

static void test_rate_fit(void)
{
    /* a 48 kHz-capable alt whose endpoint only fits 44.1 kHz packets */
    struct uac_fmt f;
    memset(&f, 0, sizeof f);
    f.channels = 2; f.subframe = 2; f.nrates = 2; f.rates[0] = 48000; f.rates[1] = 44100; f.ep_maxpkt = 180;
    T_EQ(uac_pick_rate(&f), 44100);
    f.ep_maxpkt = 100;
    T_EQ(uac_pick_rate(&f), 22050 * 0);    /* neither fits, no other rate listed */
    f.rate_continuous = 1; f.rate_min = 8000; f.rate_max = 96000; f.nrates = 0; f.ep_maxpkt = 196;
    T_EQ(uac_pick_rate(&f), 44100);
}

/* The OS splits a full-speed packet into 188-byte start-splits and the CX
 * II's controller sent one per pass of its frame list (NOTES.txt). */
static void test_one_start_split(void)
{
    struct uac_fmt f;
    struct uac_info u;
    uint32_t rate;
    memset(&f, 0, sizeof f);
    f.channels = 2; f.subframe = 2; f.ep_maxpkt = 200;
    f.nrates = 2; f.rates[0] = 48000; f.rates[1] = 44100;
    T_EQ(uac_pick_rate(&f), 44100);
    T_TRUE(uac_max_packet(&f, 44100) <= UAC_ONE_SPLIT);
    f.nrates = 1;                           /* 48 kHz only: still usable, with two start-splits */
    T_EQ(uac_pick_rate(&f), 48000);
    f.channels = 1;                         /* mono 48 kHz is 96 bytes */
    f.nrates = 2;
    T_EQ(uac_pick_rate(&f), 48000);
    f.channels = 2; f.subframe = 3;         /* 24-bit stereo: 24 kHz is the fastest in one split */
    f.nrates = 0; f.rate_continuous = 1; f.rate_min = 8000; f.rate_max = 48000; f.ep_maxpkt = 300;
    T_EQ(uac_pick_rate(&f), 24000);
    T_EQ(uac_max_packet(&f, 32000), 192);
    /* A one-split mono alternate beats a stereo one that needs two. */
    memset(&u, 0, sizeof u);
    u.audio = 1; u.n_out = 2; u.nfmt = 2;
    u.fmt[0].uac_ver = u.fmt[1].uac_ver = 1;
    u.fmt[0].format_type = u.fmt[1].format_type = 1;
    u.fmt[0].format_tag = u.fmt[1].format_tag = 1;
    u.fmt[0].subframe = u.fmt[1].subframe = 2;
    u.fmt[0].ep_maxpkt = u.fmt[1].ep_maxpkt = 200;
    u.fmt[0].nrates = u.fmt[1].nrates = 1;
    u.fmt[0].rates[0] = u.fmt[1].rates[0] = 48000;
    u.fmt[0].channels = 2;
    u.fmt[1].channels = 1;
    T_EQ(uac_choose(&u, &rate, NULL), 1);
    T_EQ(rate, 48000);
}

static void test_garbage(void)
{
    struct uac_info u;
    uint8_t b[16] = { 9, 2, 16, 0, 1, 1, 0, 0x80, 50, 0, 4, 0, 0, 0, 0, 0 };   /* zero-length descriptor */
    T_EQ(uac_parse(b, sizeof b, &u), 0);
    T_EQ(u.truncated, 1);
    T_EQ(uac_parse(b, 5, &u), -1);
    T_EQ(uac_parse(NULL, 0, &u), -1);
}

static void test_packet_storage_limit(void)
{
    struct uac_info u;
    uint32_t rate;
    const char *why;
    T_EQ(uac_parse(cfg_cm108, sizeof cfg_cm108, &u), 0);
    u.fmt[0].channels = 8;
    u.fmt[0].subframe = 4;
    u.fmt[0].bits = 32;
    u.fmt[0].ep_maxpkt = 2047;
    /* The descriptor claims room for 1536 bytes, but our DMA slots do not. */
    T_EQ(uac_max_packet(&u.fmt[0], 48000), 1536);
    T_EQ(uac_pick_rate(&u.fmt[0]), 0);
    T_EQ(uac_choose(&u, &rate, &why), -1);
    T_TRUE(strstr(why, "packet size") != NULL);
    u.fmt[0].ep_maxpkt = UAC_PACKET_MAX;
    T_EQ(uac_choose(&u, &rate, &why), -1);
    u.fmt[0].nrates = 1;
    u.fmt[0].rates[0] = 8000;
    T_EQ(uac_choose(&u, &rate, &why), 0);
    T_EQ(rate, 8000);
    T_EQ(uac_max_packet(&u.fmt[0], rate), 256);
}

int main(void)
{
    corpus_fix();
    T_RUN(test_cm108);
    T_RUN(test_headset);
    T_RUN(test_44k);
    T_RUN(test_uac2);
    T_RUN(test_mic_only);
    T_RUN(test_rate_fit);
    T_RUN(test_one_start_split);
    T_RUN(test_garbage);
    T_RUN(test_packet_storage_limit);
    return T_DONE();
}
