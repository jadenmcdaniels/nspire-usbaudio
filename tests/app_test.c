/* The whole program (src/app.c + ua_os.c + player) against the simulated OS
 * USB stack and simulated USB audio devices (tests/fake). */
#include <stdlib.h>
#include "t.h"
#include "app.h"
#include "ua_os.h"
#include "player.h"
#include "melody.h"
#include "fake/fake_os.h"
#include "corpus.h"
#include "wavbuild.h"
#include "music.h"

static char *logbuf;
static unsigned loglen;
static unsigned tick, unplug_at, plug_at, plug2_at, key_at, key_val;
static unsigned key2_at, key2_val;
static struct fos_spec replug_spec;
static uint8_t *wavfile;
static unsigned wavlen;
static unsigned usb_depth, usb_entries, usb_leaves;
static int long_regs;

static uint32_t tenter(void) { unsigned old = usb_depth; usb_depth++; usb_entries++; return old; }
static void tleave(uint32_t old) { T_EQ(usb_depth, old + 1); usb_depth = old; usb_leaves++; }
static int tirqs(void) { return usb_depth != 0; }

static unsigned log_calls;      /* each one is a file write on the calculator */
static void tlog(const char *l)
{
    unsigned n = (unsigned) strlen(l);
    log_calls++;
    if (loglen + n + 2 < (4u << 20)) {
        memcpy(logbuf + loglen, l, n);
        loglen += n;
        logbuf[loglen++] = '\n';
        logbuf[loglen] = 0;
    }
    if (getenv("UA_SHOW_LOG"))
        printf("  | %s\n", l);
}
static void tflush(void) {}
static void tsleep(void)
{
    fos_sleep();
    tick++;
    if (unplug_at && tick == unplug_at) fos_unplug();
    if (plug_at && tick == plug_at) fos_plug(&replug_spec);
    if (plug2_at && tick == plug2_at) fos_plug(&replug_spec);
}
static uint32_t tsec(void) { return tick / 100; }
static unsigned tkeys(void)
{
    return key_at && tick == key_at ? key_val : key2_at && tick == key2_at ? key2_val : 0;
}
static int tport(void) { return fos_port_connected(); }
static void tregs(char *b, unsigned n)
{
    if (long_regs)
        snprintf(b, n, "%0300u; frame list: 0 empty, 0 iTD, 1024 QH, 0 siTD, 0 FSTN, 0 odd", 0);
    else
        snprintf(b, n, "(simulated)");
}
struct mem { const uint8_t *p; uint32_t n; };
static struct mem wavmem;
static unsigned mrd(void *ctx, uint32_t off, void *buf, unsigned n)
{
    struct mem *m = ctx;
    if (off >= m->n) return 0;
    if (n > m->n - off) n = m->n - off;
    memcpy(buf, m->p + off, n);
    return n;
}

static int has(const char *s) { return strstr(logbuf, s) != NULL; }
#define T_LOG(s) do { t_checks++; if (!has(s)) T_FAIL_("log lacks \"%s\"", s); } while (0)
/* (ua18) a line whose numbers follow the queue's shape (UA_NX, UA_NF) */
#define T_LOGF(...) do { char lf_[320]; snprintf(lf_, sizeof lf_, __VA_ARGS__); T_LOG(lf_); } while (0)
#define UA_DEFAULT_SHAPE (UA_NX == 6 && UA_NF == 8)

static void make_wav(void)
{
    static int16_t lr[2 * 200000], mono[200000];
    struct melody m;
    unsigned n, i;
    melody_init(&m, SONG_TWINKLE, 22050);
    n = melody_render(&m, lr, 200000);
    for (i = 0; i < n; i++)
        mono[i] = lr[2 * i];
    wavfile = malloc(44 + 2 * n);
    wavlen = wav_build(wavfile, mono, n, 1, 16, 22050);
}

static uint32_t tfrindex(void) { return 0xB000001Cu; }

static struct app_platform pf_default(unsigned secs, int with_wav)
{
    struct app_platform pf;
    memset(&pf, 0, sizeof pf);
    pf.log = tlog; pf.flush = tflush; pf.sleep_tick = tsleep; pf.seconds = tsec; pf.keys = tkeys;
    pf.port_connected = tport; pf.usb_regs = tregs; pf.ticks_per_second = 100; pf.max_seconds = secs;
    pf.unplug_wait_s = 4;
    pf.usb_enter = tenter; pf.usb_leave = tleave;
    pf.usb_sched = fos_sched_text;
    pf.usb_frindex = tfrindex;
    if (with_wav) {
        wavmem.p = wavfile; wavmem.n = wavlen;
        pf.wav_read = mrd; pf.wav_ctx = &wavmem; pf.wav_size = wavlen;
    }
    return pf;
}

static void reset(void)
{
    fos_init();
    ua_set_frame_clock(tfrindex());
    fos_set_irq_check(tirqs);
    fos_setup_hook(NULL);
    fos_transfer_hook(NULL);
    usb_depth = usb_entries = usb_leaves = 0;
    long_regs = 0;
    loglen = 0;
    logbuf[0] = 0;
    tick = unplug_at = plug_at = plug2_at = key_at = key_val = 0;
    key2_at = key2_val = 0;
}

static void check_clean_common(unsigned exp_pipes_open, unsigned exp_xfers_open, unsigned exp_pipes_kept)
{
    const struct fos_stats *s = fos_stats();
    fos_late_callback();
    T_EQ(ua_attached(), 0);
    T_EQ(ua_owns_usb(), 0);
    T_EQ(s->violations, 0);
    if (s->violations) printf("    violation: %s\n", s->violation);
    T_EQ(s->pipes_open, exp_pipes_open);
    T_EQ(s->pipes_kept, exp_pipes_kept);
    if (exp_xfers_open != (unsigned)-1)
        T_EQ(s->xfers_open, exp_xfers_open);
    T_EQ(s->ctl_from_main, 0);
    T_EQ(usb_depth, 0);
    T_EQ(usb_entries, usb_leaves);
}

static void check_clean(void)
{
    check_clean_common(0, 0, 0);
}

static void check_clean_detached(unsigned exp_pipes_kept, unsigned exp_xfers_open)
{
    check_clean_common(exp_pipes_kept, exp_xfers_open, exp_pipes_kept);
}

/* Expected bytes: the same player run alone, written with the same volume. */
static unsigned expected(uint32_t rate, const struct dev_fmt *f, unsigned vol, uint8_t *out, unsigned max, int with_wav)
{
    static struct player p;
    static struct ring r;
    static int16_t lr[2 * 1024];
    unsigned n = 0, got;
    player_init(&p, rate, with_wav ? mrd : NULL, &wavmem, wavlen);
    ring_init(&r);
    for (;;) {
        player_fill(&p, &r, 1024);
        got = ring_get(&r, lr, 1024);
        if (!got) break;
        if (n + got * f->channels * f->subframe > max) break;
        n += pcm_write(f, lr, got, vol, out + n);
    }
    return n;
}

static void plug_spec(struct fos_spec *s, uint8_t *cfg, unsigned len, unsigned flags, uint16_t vid, uint16_t pid)
{
    memset(s, 0, sizeof *s);
    s->vid = vid; s->pid = pid; s->cfg = cfg; s->cfg_len = len; s->flags = flags;
}

static void test_cm108_streams(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(22, 1);
    const uint16_t *sizes;
    const uint8_t *bytes;
    static uint8_t exp[4u << 20];
    struct dev_fmt f = { 2, 2, 0 };
    unsigned np, nb, ne, i, bad = 0, primer = 0;
    int r;
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 30;
    replug_spec = s;
    r = app_run(&pf);
    T_EQ(r, 0);
    T_LOG("nothing on the port yet");
    T_LOG("OS offers interface 1, class 01/02/00 (audio streaming)");
    T_LOG("format type I: 2 channel(s), 2-byte subframes, 16 bits");
    T_LOG("rate 44100 Hz");
    T_LOG("endpoint 01 OUT isochronous, sync adaptive");
    /* 44.1 kHz, not 48: its 176/180-byte packets need one start-split each. */
    T_LOG("chosen: interface 1 alt 1 at 44100 Hz");
    T_LOG("SET_CUR sampling frequency 44100 Hz on endpoint 01: status 0 NORMAL_COMPLETION");
    T_LOG("GET_CUR sampling frequency: status 0 NORMAL_COMPLETION, OS says \"NORMAL COMPLETION\", device says 44100 Hz");
    T_LOG("unmute (feature unit 9 channel 0)");
    T_LOG("usbd_open_pipe endpoint 01 (max packet 200): status 0");
    T_LOG("transfer layout probe: length@4 buffer@3 frlengths@13 nframes@14 (words), method 3: ok");
    T_LOG("music.wav: 22050 Hz, 1 ch, 16 bits");
    T_LOG("RESULT: audio was streamed");
    T_LOG("stream stopped: clean");
    T_LOG("driver retired");
    T_EQ(fos_stats()->rate_set, 44100);
    T_EQ(fos_stats()->alt, 1);
    T_EQ(fos_stats()->muted, 0);
    T_EQ(fos_stats()->vol, -0x0A00);
    np = fos_cap_pkts(&sizes);
    nb = fos_cap_bytes(&bytes);
    T_TRUE(np > 16000);
    for (i = 0; i < np; i++)
        bad += sizes[i] != 176 && sizes[i] != 180;
    T_EQ(bad, 0);
    /* The first transfer is silence (the OS links it alone); then the
     * player's output, sample for sample, with no packet missing. */
    for (i = 0; i < UA_NF; i++)
        primer += sizes[i];
    for (i = 0, bad = 0; i < primer; i++)
        bad += bytes[i] != 0;
    T_EQ(bad, 0);
    ne = expected(44100, &f, 192, exp, sizeof exp, 1);
    T_TRUE(ne > 16 * 176400);
    T_TRUE(nb >= primer + ne);
    T_TRUE(memcmp(bytes + primer, exp, ne) == 0);
    for (i = 0; i < ne && bytes[primer + i] == exp[i]; i++) ;
    if (i < ne) printf("    first difference at byte %u of %u\n", i, ne);
    T_LOG("underruns 0 pkts");
    /* Continuous on the controller model that played 1 ms in 32 before. */
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    T_EQ(fos_stats()->sitd_max, UA_NX * UA_NF);
    T_TRUE(fos_stats()->sitd_max <= FOS_SITD_POOL);
    T_TRUE(fos_stats()->fresh_starts <= 2);  /* the first transfer, then the rest: never again */
    check_clean();
}

static void test_44k_pattern(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(4, 0);
    const uint16_t *sizes;
    unsigned np, i, sum = 0, n176 = 0, n180 = 0;
    reset();
    plug_spec(&s, cfg_44k, sizeof cfg_44k, 0, 0x1234, 0x4401);
    fos_plug(&s);
    plug_at = 0;
    tick = 0;
    /* plugged before start: the fake (like the OS) offers it to nobody until replugged */
    fos_sleep();
    unplug_at = 450;
    plug_at = 470;
    replug_spec = s;
    pf.max_seconds = 9;
    app_run(&pf);
    T_LOG("a device is already on the port (plugged in before start)");
    T_LOG("Unplug the USB audio device and plug it in again.");
    T_LOG("sample rate: the stream has one fixed rate (44100 Hz), nothing to set");
    T_LOG("no feature unit");
    T_LOG("feedback endpoint IN");
    T_LOG("note: asynchronous endpoint");
    np = fos_cap_pkts(&sizes);
    T_TRUE(np >= 1000);
    for (i = 0; i < 1000; i++) {
        sum += sizes[i];
        n176 += sizes[i] == 176;
        n180 += sizes[i] == 180;
    }
    T_EQ(sum, 44100 * 4);
    T_EQ(n176, 900);
    T_EQ(n180, 100);
    for (i = 0; i + 10 <= np; i += 10) {
        unsigned j, s10 = 0;
        for (j = 0; j < 10; j++) s10 += sizes[i + j];
        if (s10 != 441 * 4) break;
    }
    T_EQ(i, np - np % 10);
    check_clean();
}

static void test_headset(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    reset();
    plug_spec(&s, cfg_headset, sizeof cfg_headset, FOS_CDESC_SHORT, 0x046d, 0x0a44);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("usbd_get_config_descriptor: ok, 9 bytes");
    T_LOG("they differ: using the longer");
    /* The OS's copy does not hold the endpoint: nothing is written anywhere else. */
    T_LOG("wMaxPacketSize in the OS's copy of the descriptors: endpoint not found there, nothing written");
    T_LOG("stream 1: interface 2 alt 1, IN (microphone)");
    T_LOG("volume range channel 1: -12032..0");
    T_LOG("volume -2560/256 dB channel 1");
    T_LOG("streaming started");
    T_EQ(fos_stats()->vol, -0x0A00);
    check_clean();
}

static void test_isoc_refused(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    int r;
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, FOS_NO_ISOC_OPEN, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    r = app_run(&pf);
    T_EQ(r, 1);
    T_LOG("usbd_open_pipe endpoint 01 (max packet 200): status 10 INVAL, OS says \"INVALID ARGUMENT\"");
    T_LOG("STOPPED at: usbd_open_pipe (isochronous OUT), status 10 INVAL");
    T_LOG("MEANING: the OS's USB stack refuses an isochronous pipe");
    T_EQ(fos_cap_bytes(&(const uint8_t *){ NULL }), 0);
    check_clean();
}

static void test_isoc_xfer_refused(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    int r;
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, FOS_NO_ISOC_XFER, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    r = app_run(&pf);
    T_EQ(r, 1);
    T_LOGF("usbd_transfer #0 (%u bytes, %u packets): status 5 IOERROR", 176u * UA_NF + (UA_NF > 9 ? 4u : 0u), UA_NF);
    T_LOG("FIRST TRANSFER ERROR: usbd_transfer (from main), status 5 IOERROR");
    T_LOG("STOPPED at: usbd_transfer (from main), status 5 IOERROR");
    T_LOG("usbd_close_pipe: status 0");
    check_clean();
}

static void test_completion_errors(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, FOS_ISOC_ERR, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("FIRST TRANSFER ERROR: transfer completion, status 5 IOERROR");
    T_LOG("MEANING: isochronous transfers complete with an error");
    check_clean();
}

static void test_never_done(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(6, 0);
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, FOS_NEVER_DONE, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOGF("NO COMPLETION for 3 s: %u transfers queued", UA_START_QUEUED);
    T_LOG("usbd_abort_pipe: status 0");
    check_clean();
}

static void test_pending_abort(void)
{
    unsigned mode;
    for (mode = 0; mode < 3; mode++) {
        static struct ring pending_audio;
        unsigned entered;
        reset();
        T_EQ(ua_attached(), 0);
        T_EQ(ua_owns_usb(), 0);
        ring_init(&pending_audio);
        T_EQ(ua_init(), 0);
        plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108,
                  FOS_NEVER_DONE | (mode == 2 ? FOS_CLOSE_ERROR :
                                   mode ? FOS_ABORT_ERROR : FOS_ABORT_DELAYED), 0x0d8c, 0x0014);
        fos_plug(&replug_spec);
        tsleep();
        T_EQ(ua_state(), UA_READY);
        T_TRUE(ua_attached());
        T_TRUE(ua_owns_usb());
        entered = tenter();
        T_EQ(ua_start(&pending_audio), 0);
        T_EQ(ua_shutdown(tsleep), 1);
        T_TRUE(ua_owns_usb());
        /* The first deadline expired. Every OS object and the callback
         * trampoline must remain live until cancellation actually arrives. */
        T_EQ(fos_stats()->xfers_open, mode == 2 ? 0 : UA_NX);
        T_EQ(fos_stats()->pipes_open, 1);
        T_EQ(fos_stats()->retire_calls, 0);
        T_EQ(fos_stats()->violations, 0);
        T_EQ(ua_shutdown(tsleep), 0);
        tleave(entered);
        T_EQ(fos_stats()->isoc_cancelled, UA_START_QUEUED);   /* (UA_START_ONE: the first alone was queued) */
        T_EQ(fos_stats()->retire_calls, 1);
        if (mode == 1) T_EQ(fos_stats()->abort_calls, 2);
        if (mode == 2) T_EQ(fos_stats()->close_calls, 2);
        check_clean();
    }
}

static void test_retained_resources_block_offers(void)
{
    static struct ring audio;
    uint32_t saved;
    reset();
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108,
              FOS_NO_ISOC_XFER | FOS_CLOSE_ERROR, 0x0d8c, 0x0014);
    fos_plug(&replug_spec);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    saved = tenter();
    T_EQ(ua_start(&audio), 5);
    T_EQ(ua_state(), UA_FAILED);
    T_EQ(fos_offer_other(0), 0);
    T_TRUE(fos_offer_other(1) != 0); /* attach independently rejects a stale match */
    T_EQ(fos_stats()->xfers_open, UA_NX);
    ua_stop(tsleep);               /* xfers freed, close fails once */
    T_EQ(fos_stats()->xfers_open, 0);
    T_EQ(fos_stats()->pipes_open, 1);
    T_EQ(ua_state(), UA_FAILED);
    T_EQ(fos_offer_other(0), 0);
    T_TRUE(fos_offer_other(1) != 0);
    T_EQ(fos_stats()->pipes_open, 1);
    ua_stop(tsleep);
    T_EQ(fos_stats()->pipes_open, 0);
    T_TRUE(fos_offer_other(0) > 0); /* cleanup restores availability */
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean();
}

/* gpSP's CX II run of 2026-11-15: after a nonzero match, an attach that
 * returns an error makes TI's OS offer nothing more until the adapter is
 * pulled (the fake counts it a violation). A shutdown that begins between
 * the match and the attach: the attach succeeds and takes nothing. */
static void shutdown_between_match_and_attach(void)
{
    T_EQ(ua_shutdown(NULL), 0);
}

static void test_attach_after_match_never_fails(void)
{
    struct fos_spec s;
    unsigned requests;
    reset();
    T_EQ(ua_init(), 0);
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_match_hook(shutdown_between_match_and_attach);
    fos_plug(&s);
    requests = fos_stats()->ctl_requests;
    tsleep();
    fos_match_hook(NULL);
    T_EQ(fos_stats()->violations, 0);
    if (fos_stats()->violations) printf("    violation: %s\n", fos_stats()->violation);
    T_EQ(fos_stats()->attach_fail, 0);
    T_TRUE(fos_stats()->attach_ok >= 1);       /* the OS has it as attached ... */
    T_EQ(fos_stats()->ctl_requests, requests);  /* ... and nothing was sent to it */
    T_EQ(ua_state(), UA_OFF);
    T_EQ(ua_attached(), 0);
    fos_unplug();
    tsleep();
    check_clean();
}

static unsigned stop_in_requeue;
static void stop_during_requeue(void)
{
    stop_in_requeue++;
    fos_requeue_hook(NULL);
    ua_stop(NULL);
    /* busy has cleared, but the callback will still use this xfer. */
    T_EQ(fos_stats()->xfers_open, UA_NX);
    T_EQ(fos_stats()->pipes_open, 1);
    T_EQ(fos_stats()->abort_calls, 0);
}

static void test_stop_during_requeue(void)
{
    static struct ring audio;
    uint32_t saved;
    reset(); stop_in_requeue = 0;
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_plug(&replug_spec);
    tsleep();
    saved = tenter();
    T_EQ(ua_start(&audio), 0);
    fos_requeue_hook(stop_during_requeue);
    tsleep();                       /* the first transfer's 8 frames start 4 frames from now */
    tsleep();
    T_EQ(stop_in_requeue, 1);
    ua_stop(tsleep);
    T_EQ(ua_state(), UA_STOPPED);
    T_EQ(fos_stats()->xfers_open, 0);
    T_EQ(fos_stats()->pipes_open, 0);
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean();
}

/* The driver alone on a plugged CM108, fed a ramp; MAIN stays away from the
 * OS for stall_ms every stall_every ticks (a log write, a redraw). */
static int alone_elsewhere;     /* the OS's pipes do not read the descriptor copy it hands out */
static void stream_alone(int model, unsigned ticks, unsigned stall_every, unsigned stall_ms)
{
    static struct ring audio;
    static int16_t ramp[2 * 512];
    uint32_t saved;
    unsigned t, i, v = 0;
    reset();
    fos_set_hw(model);
    fos_ep_elsewhere(alone_elsewhere);
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_plug(&replug_spec);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    while (ring_free(&audio) >= 512) {
        for (i = 0; i < 2 * 512; i++) ramp[i] = (int16_t) (v++ * 7);
        ring_put(&audio, ramp, 512);
    }
    saved = tenter();
    T_EQ(ua_start(&audio), 0);
    tleave(saved);
    for (t = 1; t <= ticks; t++) {
        while (ring_free(&audio) >= 512) {
            for (i = 0; i < 2 * 512; i++) ramp[i] = (int16_t) (v++ * 7);
            ring_put(&audio, ramp, 512);
        }
        if (stall_every && t % stall_every == 0)
            fos_stall(stall_ms);
        tsleep();
    }
}

static void end_alone(void)
{
    uint32_t saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean();
}

static void test_main_away_from_the_os(void)
{
    struct ua_counters c;
    /* 36 ms every quarter second: 48 ms are queued, the OS wants 4 ahead
     * (ua18: (UA_NX - 1) x UA_NF - 4 for any shape: 28 ms with 3 x 16) */
    stream_alone(FOS_HW_FOTG210, 1000, 25, (UA_NX - 1) * UA_NF - 4);
    ua_counters(&c);
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    T_EQ(c.underrun_pkts, 0);
    T_EQ(c.errors, 0);
    T_TRUE(c.frames_ok > 11000);
    end_alone();
    /* Longer than the queue: a gap the length of the excess, then the OS
     * starts again 4 frames ahead; nothing waits for the frame list to lap. */
    stream_alone(FOS_HW_FOTG210, 1000, 100, 80);
    ua_counters(&c);
    T_TRUE(fos_stats()->starved_frames >= 9 * (80 - (UA_NX - 1) * UA_NF - 10) &&
           fos_stats()->starved_frames <= 10 * (80 - (UA_NX - 1) * UA_NF + 5));
    T_EQ(fos_stats()->late_frames, 0);
    T_EQ(c.errors, 0);
    end_alone();
}

/* ua18: ua_queue_ms against the simulated OS's own count of what it still
 * has to send, and its head against whether a window brings a completion:
 * MAIN away 1-7 ms at a time, a window every third time (gpSP's way). */
static void test_queue_ms(void)
{
    struct ua_counters c0, c1;
    unsigned i, known = 0, long_err = 0, head_ok = 0, head_n = 0, dry = 0, fixes = 0, far = 0;
    int a, h, fa;
    stream_alone(FOS_HW_FOTG210, 30, 0, 0);
    for (i = 0; i < 3000; i++) {
        fos_stall(1 + i % 7);
        if (!ua_queue_ms(&a, &h))
            continue;
        known++;
        fa = (int) fos_wire_left();
        if (a > fa + 1)
            long_err++;             /* never more than the OS has: it must err short */
        T_TRUE(a <= fa + 1);
        T_TRUE(h <= a);
        if (i >= 300 && a < fa - 1)
            far++;                  /* (short by the start's link, until a window says where the head is) */
        T_TRUE(a >= fa - 12);       /* (the start's: the first completion came a sleep (10 ms) late at most) */
        if (a <= 0)
            dry++;
        if (i % 3 == 0) {
            ua_counters(&c0);
            fos_irq_window();
            ua_counters(&c1);
            {
                int a2, h2;
                if (ua_queue_ms(&a2, &h2) && h2 < 0) {
                    int moved = ua_queue_not_done();    /* (gpSP's way: after a window, a head still "done" is not) */
                    T_TRUE(moved >= 0);
                    if (moved > 0) {
                        fixes++;
                        T_EQ(ua_queue_not_done(), 0);
                    }
                }
            }
            if (i >= 300 && (h <= 0 || h >= 2)) {
                head_n++;
                head_ok += (h <= 0) == (c1.completions != c0.completions);
            }
        }
    }
    printf("    queue_ms: known %u of 3000, %u longer than the OS's, %u shorter by 2 or more after the first 300, head "
           "right %u of %u, dry %u, moved on %u times\n", known, long_err, far, head_ok, head_n, dry, fixes);
    T_TRUE(known >= 2990);
    T_EQ(long_err, 0);
    T_EQ(head_ok, head_n);
    T_EQ(dry, 0);
    T_EQ(fos_stats()->starved_frames, 0);
    end_alone();
    /* MAIN away past the queue now and then (the wire runs dry, the OS
     * starts the chain again 4 frames on), from the stream's first frame:
     * never longer than the OS has */
    stream_alone(FOS_HW_FOTG210, 0, 0, 0);
    long_err = 0;
    for (i = 0; i < 3000; i++) {
        fos_stall(i % 5 == 4 ? UA_NX * UA_NF + i % 13 : 1 + i % 7);
        if (ua_queue_ms(&a, &h)) {
            fa = (int) fos_wire_left();
            long_err += a > fa + 1;
            T_TRUE(a <= fa + 1);
            if (getenv("UA_QDBG") && a > fa + 1)
                printf("      i %u a %d h %d fa %d\n", i, a, h, fa);
        }
        if (i % 2 == 0)
            fos_irq_window();
    }
    T_EQ(long_err, 0);
    T_TRUE(fos_stats()->starved_frames > 0);
    end_alone();
    /* gpSP's frames: 15 and 25 ms, a window after some of them */
    stream_alone(FOS_HW_FOTG210, 30, 0, 0);
    long_err = 0;
    for (i = 0; i < 2000; i++) {
        fos_stall(i % 2 ? 25 : 15);
        if (ua_queue_ms(&a, &h)) {
            fa = (int) fos_wire_left();
            long_err += a > fa + 1;
            T_TRUE(a <= fa + 1);
            if (getenv("UA_QDBG") && a > fa + 1)
                printf("      frames: i %u a %d h %d fa %d\n", i, a, h, fa);
            if (a < 36)
                fos_irq_window();
        }
    }
    T_EQ(long_err, 0);
    end_alone();
    /* ua_set_frame_clock(0) turns off the frame clock in tests too; q_unsure
     * backs off on a late completion until 2 * UA_NX more transfers queue. */
    stream_alone(FOS_HW_FOTG210, 30, 0, 0);
    T_EQ(ua_queue_ms(&a, &h), 1);
    ua_set_frame_clock(0);
    T_EQ(ua_queue_ms(&a, &h), 0);
    T_EQ(ua_queue_not_done(), 0);
    ua_set_frame_clock(tfrindex());
    T_EQ(ua_queue_ms(&a, &h), 1);
    fos_stall(UA_NX * UA_NF + UA_LATE_MS + 20);
    fos_irq_window();
    T_EQ(ua_queue_ms(&a, &h), 0);
    for (i = 0; i < 4 * UA_NX; i++) {
        fos_stall(UA_NF);
        fos_irq_window();
    }
    T_EQ(ua_queue_ms(&a, &h), 1);
    end_alone();
    /* not streaming: not known */
    T_EQ(ua_queue_ms(&a, &h), 0);
}

/* What the driver cannot change: a device with 48 kHz stereo only needs
 * two start-splits a packet, and under the two other readings of the
 * calculator's log (fake_os.h) no packet size helps. These show the limit,
 * and that nothing breaks. */
static void test_what_the_queue_cannot_fix(void)
{
    stream_alone(FOS_HW_LATE, 600, 0, 0);
    T_EQ(fos_stats()->ontime_frames, 0);
    T_TRUE(fos_stats()->wire_frames < 6 * (UA_NX * UA_NF + 8));
    end_alone();
    stream_alone(FOS_HW_LEAD, 600, 0, 0);
    T_TRUE(fos_stats()->wire_frames < 6 * (UA_NX * UA_NF + 8));
    end_alone();
    /* A controller that sends every start-split in the packet's frame. */
    stream_alone(FOS_HW_EHCI, 600, 0, 0);
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    T_TRUE(fos_stats()->wire_frames > 5900);
    end_alone();
}

/* The OS picks a descriptor's S-mask by the endpoint's wMaxPacketSize (200
 * on the CM108: two micro-frames) even when the packet needs one
 * start-split. Should the controller want a pass of its frame list per
 * S-mask micro-frame (FOS_HW_SMASK), 44.1 kHz alone would not do: the
 * driver writes the size of its largest packet over wMaxPacketSize in the
 * OS's copy of the configuration descriptor before it opens the pipe. */
static void test_endpoint_size_told_to_the_os(void)
{
    unsigned off, in_device = 0;
    stream_alone(FOS_HW_SMASK, 600, 0, 0);
    T_EQ(fos_os_maxpkt(0x01), 180);             /* the OS's copy: 44.1 kHz stereo's largest packet */
    T_EQ(fos_os_maxpkt(0x82), 100);             /* the microphone's endpoint: untouched */
    for (off = 0; off < sizeof cfg_cm108; off += cfg_cm108[off])
        if (cfg_cm108[off + 1] == 5 && cfg_cm108[off + 2] == 0x01)
            in_device = (unsigned) (cfg_cm108[off + 4] | cfg_cm108[off + 5] << 8);
    T_EQ(in_device, 200);                       /* what the device itself says: untouched */
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    T_TRUE(fos_stats()->wire_frames > 5900);
    end_alone();
    /* If the OS's pipes read the size elsewhere, that controller model
     * still takes two passes: the log would show S-mask 03 with T-count 1. */
    alone_elsewhere = 1;
    stream_alone(FOS_HW_SMASK, 600, 0, 0);
    alone_elsewhere = 0;
    T_EQ(fos_stats()->ontime_frames, 0);
    T_TRUE(fos_stats()->wire_frames < 6 * (UA_NX * UA_NF + 8));
    end_alone();
    /* On the T-count reading the size told to the OS does not matter. */
    alone_elsewhere = 1;
    stream_alone(FOS_HW_FOTG210, 600, 0, 0);
    alone_elsewhere = 0;
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    end_alone();
}

/* The CM108's descriptors with 48 kHz as the only rate: 192-byte packets,
 * two start-splits each, like the run of 2026-11-14. */
static void only_48k(uint8_t *cfg)
{
    unsigned off, j;
    memcpy(cfg, cfg_cm108, sizeof cfg_cm108);
    for (off = 9; off < sizeof cfg_cm108; off += cfg[off]) {
        uint8_t *d = cfg + off;
        if (d[1] == 0x24 && d[2] == 2)
            for (j = 8; j + 3 <= d[0]; j += 3) {
                d[j] = 0x80; d[j + 1] = 0xBB; d[j + 2] = 0x00;
            }
    }
}

static struct app_platform music_setup(unsigned secs, unsigned count);
static unsigned saw_lag;

/* The log of a second, and what it says when the packets go out a lap late. */
static void test_second_lines(void)
{
    static uint8_t cfg[sizeof cfg_cm108];
    struct app_platform pf = pf_default(8, 0);
    unsigned before;
    reset();
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    plug_at = 5;
    T_EQ(app_run(&pf), 0);
    if (UA_START_QUEUED < UA_NX) {
        /* (UA_START_ONE: the first alone, then the others behind it from its completion) */
        T_LOGF("first transfers back, ms after they were queued / ms queued ahead of them: %u/0 %u/0 %u/%u",
               3 + UA_NF, 3 + UA_NF, 3 + 2 * UA_NF, UA_NF);
    } else {
        /* (ua18) the first ones back: 3 + UA_NF ms, then a transfer's more
         * each, behind a transfer's more each; then a full queue's */
        char want[200] = "first transfers back, ms after they were queued / ms queued ahead of them:";
        unsigned k, n = strlen(want);
        for (k = 0; k < UA_NX && n < sizeof want - 16; k++)
            n += (unsigned) snprintf(want + n, sizeof want - n, " %u/%u", 3 + UA_NF * (k + 1), UA_NF * k);
        if (UA_NX < 12)
            snprintf(want + n, sizeof want - n, " %u/%u", UA_NX * UA_NF, (UA_NX - 1) * UA_NF);
        T_LOG(want);
    }
    /* (a second holds 1000 / UA_NF transfers when it divides: else 62 and 63 of 16 ms in turn) */
    if (1000 % UA_NF == 0)
        T_LOGF("queued 1000 ms, played 1000 (late 0), dropped 0 (silence 0, failed 0); %u transfers back after",
               1000u / UA_NF);
    else
        T_LOGF("queued %u ms, played %u (late 0), dropped 0 (silence 0, failed 0); %u transfers back after",
               1000u / UA_NF * UA_NF, 1000u / UA_NF * UA_NF, 1000u / UA_NF);
    T_LOGF(" transfers back after %u-%u ms "
           "(avg %u), 0-0 ms after their last packet (avg 0), at most %u ms apart; least queued ahead %u ms, restarts 0",
           UA_NX * UA_NF, UA_NX * UA_NF, UA_NX * UA_NF, UA_NF, (UA_NX - 1) * UA_NF);
    if (UA_START_QUEUED == UA_NX)
        T_LOGF("siTD %u: %u waiting at +1..+%u (0 more than 64 frames away), 1 sent; first waiting: slot ",
               UA_NX * UA_NF, UA_NX * UA_NF - 1, UA_NX * UA_NF - 1);
    else
        T_LOGF("siTD %u: ", UA_NX * UA_NF);
    T_LOG("176 of 176 bytes left, T-count 1, S-mask 01");
    T_LOG("wMaxPacketSize in the OS's copy of the descriptors: 200, lowered to 180 (our largest packet: 180 bytes)");
    T_TRUE(!has("LATE:"));
    check_clean();

    /* 48 kHz only: two start-splits a packet, one per lap on this controller. */
    only_48k(cfg);
    reset();
    plug_spec(&replug_spec, cfg, sizeof cfg, 0, 0x0d8c, 0x0014);
    plug_at = 5;
    app_run(&pf);
    T_LOG("chosen: interface 1 alt 1 at 48000 Hz");
    T_LOG("LATE: transfers come back about 1024 ms");
    if (UA_DEFAULT_SHAPE)   /* (ua18: this lap's numbers are the 6 x 8 queue's) */
        T_LOG("queued 48 ms, played 48 (late 48), dropped 0 (silence 0, failed 0); 6 transfers back after 1035-1035 ms "
              "(avg 1035), 987-987 ms after their last packet");
    T_LOGF("(%u more than 64 frames away), 0 sent; first far one: slot ", UA_NX * UA_NF);
    T_LOG("4 of 192 bytes left, T-count 1, S-mask 03");
    T_EQ(fos_os_maxpkt(0x01), 192);             /* never below the packets it must carry */
    T_EQ(fos_stats()->ontime_frames, 0);
    check_clean();

    /* The same device on a controller that sends both start-splits in the frame. */
    reset();
    fos_set_hw(FOS_HW_EHCI);
    plug_spec(&replug_spec, cfg, sizeof cfg, 0, 0x0d8c, 0x0014);
    plug_at = 5;
    T_EQ(app_run(&pf), 0);
    T_LOGF("queued %u ms, played %u (late 0)", 1000 % UA_NF ? 1000u / UA_NF * UA_NF : 1000u,
           1000 % UA_NF ? 1000u / UA_NF * UA_NF : 1000u);
    T_TRUE(!has("LATE:"));
    check_clean();

    /* The music player writes the seconds five at a time: about one file
     * write a second would keep it from the OS as often as the old code. */
    pf = music_setup(12, 1);
    before = log_calls;
    T_EQ(app_run(&pf), 0);
    T_LOGF("music: t=3s queued %u ms, played %u (late 0), dropped 0 (silence 0, failed 0); %u transfers back after %u-%u ms",
           1000 % UA_NF ? 1000u / UA_NF * UA_NF : 1000u, 1000 % UA_NF ? 1000u / UA_NF * UA_NF : 1000u, 1000u / UA_NF,
           UA_NX * UA_NF, UA_NX * UA_NF);
    if (UA_START_QUEUED == UA_NX)
        T_LOGF("music: first transfers back, ms after they were queued / ms queued ahead: %u/0 %u/%u", 3 + UA_NF,
               3 + 2 * UA_NF, UA_NF);
    else
        T_LOGF("music: first transfers back, ms after they were queued / ms queued ahead: %u/0 %u/0", 3 + UA_NF,
               3 + UA_NF);
    T_LOG("music: frame ");
    T_LOG("late ms 0 of ");
    T_TRUE(log_calls - before <= 16);
    T_TRUE(!has("LATE:"));
    check_clean();
    pf = music_setup(8, 1);
    only_48k(cfg);
    plug_spec(&replug_spec, cfg, sizeof cfg, 0, 0x0d8c, 0x0014);
    saw_lag = 0;
    app_run(&pf);
    T_LOG("music: LATE: transfers come back about 1024 ms");
    T_EQ(saw_lag, 1);
    check_clean();
}

/* An eight-channel 32-bit format, with configurable packet/rate claims. */
static void wide_format(uint8_t *cfg, unsigned maxpkt, unsigned rate)
{
    unsigned off, iface = 0;
    memcpy(cfg, cfg_cm108, sizeof cfg_cm108);
    for (off = 9; off < sizeof cfg_cm108; off += cfg[off]) {
        uint8_t *d = cfg + off;
        if (d[1] == 4) iface = d[2];
        if (iface != 1) continue;
        if (d[1] == 0x24 && d[2] == 2) {
            unsigned j;
            d[4] = 8; d[5] = 4; d[6] = 32;
            for (j = 8; j + 3 <= d[0]; j += 3) {
                d[j] = (uint8_t) rate;
                d[j + 1] = (uint8_t) (rate >> 8);
                d[j + 2] = (uint8_t) (rate >> 16);
            }
        } else if (d[1] == 5 && d[2] == 1) {
            d[4] = (uint8_t) maxpkt;
            d[5] = (uint8_t) (maxpkt >> 8);
        }
    }
}

static void test_oversized_packets(void)
{
    uint8_t cfg[sizeof cfg_cm108];
    struct app_platform pf = pf_default(2, 0);
    unsigned i;
    reset();
    wide_format(cfg, 2047, 48000);
    plug_spec(&replug_spec, cfg, sizeof cfg, 0, 0x0d8c, 0x0014);
    plug_at = 5;
    T_EQ(app_run(&pf), 1);
    T_EQ(fos_stats()->isoc_submitted, 0);
    T_LOG("unsupported endpoint packet size");
    check_clean();

    for (i = 0; i < 2; i++) {
        static struct ring audio;
        uint32_t saved;
        reset();
        ring_init(&audio);
        wide_format(cfg, UAC_PACKET_MAX, 8000);
        T_EQ(ua_init(), 0);
        plug_spec(&replug_spec, cfg, sizeof cfg, 0, 0x0d8c, 0x0014);
        fos_plug(&replug_spec);
        tsleep();
        T_EQ(ua_state(), UA_READY);
        /* Fault-inject the cached rate after selection: refill must guard
         * DMA packet capacity and the 200-frame PCM scratch independently. */
        ((struct ua_desc *) ua_desc())->rate = i ? 300000 : 48000;
        saved = tenter();
        T_EQ(ua_start(&audio), 10);
        T_EQ(ua_state(), UA_FAILED);
        T_EQ(fos_stats()->isoc_submitted, 0);
        T_EQ(ua_shutdown(tsleep), 0);
        tleave(saved);
        check_clean();
    }
}

static void test_unplug_mid_stream(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(8, 1);
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 5;
    unplug_at = 250;
    replug_spec = s;
    app_run(&pf);
    T_LOG("detach (device unplugged) while streaming");
    T_LOG("usbd_abort_pipe: status 0");
    T_LOG("state: unplugged");
    T_TRUE(fos_stats()->isoc_cancelled > 0);
    check_clean();
}

static void test_replug_after_unplug(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(10, 0);
    const char *p;
    unsigned starts = 0;
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 5;
    unplug_at = 200;
    plug2_at = 400;
    replug_spec = s;
    app_run(&pf);
    T_LOG("state: unplugged");
    for (p = logbuf; (p = strstr(p, "streaming started")) != NULL; p++)
        starts++;
    T_EQ(starts, 2);
    check_clean();
}

static void test_missing_isoc(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    reset();
    fos_remove_syscall(233);
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("usbd_setup_isoc_xfer: NOT FOUND");
    T_LOG("STOPPED at: find usbd_setup_isoc_xfer, the OS has no such call");
    check_clean();
}

static void test_missing_required(void)
{
    struct app_platform pf = pf_default(2, 0);
    int r;
    reset();
    fos_remove_syscall(227);
    r = app_run(&pf);
    T_EQ(r, 1);
    T_LOG("OS call missing: syscall 227 (usbd_open_pipe)");
    T_LOG("cannot start");
}

static void test_raw_buffers(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(3, 0);
    reset();
    fos_set_conv(1);
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("control buffer form: plain pointer");
    T_LOG("streaming started");
    T_LOG("errors 0, underruns 0 pkts");
    check_clean();
}

static void test_uac2_and_mic(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(2, 0);
    reset();
    plug_spec(&s, cfg_uac2, sizeof cfg_uac2, 0, 0x2622, 0x0041);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("audio streaming, class 2");
    T_LOG("no playable stream: USB Audio Class 2 only");
    T_LOG("STOPPED at: choose format");
    check_clean();
    reset();
    plug_spec(&s, cfg_mic, sizeof cfg_mic, 0, 0x1111, 0x2222);
    plug_at = 5;
    replug_spec = s;
    app_run(&pf);
    T_LOG("no playable stream: only microphone (IN) streaming interfaces");
    check_clean();
}

static void test_unsupported_replug(void)
{
    struct app_platform pf = pf_default(2, 0);
    reset();
    plug_spec(&replug_spec, cfg_uac2, sizeof cfg_uac2, 0, 0x2622, 0x0041);
    T_EQ(ua_init(), 0);
    fos_plug(&replug_spec);
    tsleep();
    T_EQ(ua_state(), UA_FAILED);
    T_EQ(fos_stats()->attach_fail, 0);
    fos_unplug();
    tsleep();
    T_EQ(ua_state(), UA_GONE);
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_plug(&replug_spec);
    T_EQ(app_run(&pf), 0);
    T_LOG("streaming started");
    T_EQ(fos_stats()->attach_fail, 0);
    check_clean();
}

static void test_derive(void)
{
    /* CX II 6.4 (from a gpSP log): setup_xfer 10a07a34, get_xfer_status 10a07b54 */
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b54u, 0xC4, 0xE92D4070u, 0xE8BD8FF0u), 0x10a07af8u);
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b54u, 0xC4, 0xE92D0030u, 0x00000040u), 0x10a07af8u); /* leaf push */
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b54u, 0xC4, 0xE59DC000u, 0xE12FFF1Eu), 0x10a07af8u); /* after bx lr */
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b54u, 0xC4, 0xE59DC000u, 0xE5801010u), 0);   /* mid-function */
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b58u, 0xC4, 0xE92D4070u, 0), 0);             /* gap not 0x120 */
    T_EQ(ua_isoc_derive(0x10a07a34u, 0x10a07b54u, 0xD0, 0xE92D4070u, 0), 0);
    /* CX 4.5.0: the table's own address is setup_xfer + 0xC4 */
    T_EQ(ua_isoc_derive(0x104b2aecu, 0x104b2c0cu, 0xC4, 0xE92D4000u, 0), 0x104b2bb0u);
}

static void test_keys(void)
{
    struct fos_spec s;
    struct app_platform pf = pf_default(30, 1);
    reset();
    plug_spec(&s, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x000c);
    plug_at = 5;
    replug_spec = s;
    key_at = 200;
    key_val = A_NEXT;
    app_run(&pf);
    T_LOG("next: music.wav");
    reset();
    plug_at = 5;
    key_at = 300;
    key_val = A_QUIT;
    app_run(&pf);
    T_LOG("Esc: quit");
    T_LOG("stream stopped: clean");
    check_clean();
}

static void test_slow_host_start(void)
{
    struct app_platform pf = pf_default(108, 0);
    reset();
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    plug_at = 10000;           /* observed CX II delays were about 92 seconds */
    long_regs = 1;
    T_EQ(app_run(&pf), 0);
    T_LOG("waiting 95 s: port not connected, OS probes 0 (Esc quits)");
    T_LOG("streaming started");
    T_LOG("; frame list: 0 empty, 0 iTD, 1024 QH, 0 siTD, 0 FSTN, 0 odd");
    check_clean();
    reset();
    pf.max_seconds = 120;
    key_at = 100; key_val = A_QUIT;
    T_EQ(app_run(&pf), 0);
    T_LOG("Esc: quit");
    T_TRUE(tick < 200);        /* no forced two-minute wait on exit */
    T_TRUE(!has("RESULT: audio was streamed"));
    check_clean();
}

static void quit_during_setup(void)
{
    T_EQ(ua_state(), UA_SETUP);
    T_EQ(ua_shutdown(NULL), 1); /* cannot unload callback code while attach runs */
    T_EQ(ua_state(), UA_SETUP);
    fos_setup_hook(NULL);
}
static void test_quit_during_setup(void)
{
    struct app_platform pf = pf_default(2, 0);
    reset();
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_setup_hook(quit_during_setup);
    plug_at = 5;
    app_run(&pf);
    T_EQ(ua_state(), UA_OFF);
    T_TRUE(!has("streaming started"));
    T_EQ(fos_stats()->isoc_submitted, 0);
    check_clean();
}

static void quit_during_control(void)
{
    quit_during_setup();
    fos_control_hook(NULL);
}

static void test_quit_during_control(void)
{
    struct app_platform pf = pf_default(2, 0);
    reset();
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_control_hook(quit_during_control);
    plug_at = 5;
    app_run(&pf);
    T_EQ(ua_state(), UA_OFF);
    T_EQ(fos_stats()->ctl_requests, 1); /* finish this request, issue no more */
    T_EQ(fos_stats()->isoc_submitted, 0);
    check_clean();
}

static unsigned quit_in_completion;
static void quit_during_completion(void)
{
    quit_in_completion++;
    /* Re-queuing is an OS call too; the completion must return before
     * the executable can be unloaded. */
    T_EQ(ua_shutdown(NULL), 1);
    T_EQ(ua_state(), UA_STREAMING);
    fos_transfer_hook(NULL);
}
static void test_quit_during_completion(void)
{
    struct app_platform pf = pf_default(2, 0);
    reset(); quit_in_completion = 0;
    plug_spec(&replug_spec,cfg_cm108,sizeof cfg_cm108,0,0x0d8c,0x0014);
    fos_transfer_hook(quit_during_completion);
    plug_at = 5;
    app_run(&pf);
    T_EQ(quit_in_completion,1);
    T_EQ(ua_state(),UA_OFF);
    check_clean();
}

static void test_result_before_descriptors(void)
{
    struct app_platform pf = pf_default(2, 0);
    const char *result, *dump;
    reset();
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, FOS_NO_ISOC_OPEN, 0x0d8c, 0x0014);
    plug_at = 5;
    T_EQ(app_run(&pf), 1);
    result = strstr(logbuf, "STOPPED at: usbd_open_pipe");
    dump = strstr(logbuf, "--- device");
    T_TRUE(result && dump && result < dump);
    check_clean();
}

static unsigned music_opens, music_indices, saw_finished, saw_bad_file, saw_paused, pause_pos;
static int music_bad_first;
static const char *music_title(unsigned i) { return i ? "Second song" : "First song"; }
static int music_open(unsigned i, struct music_track *t)
{
    music_opens++;
    music_indices |= 1u << i;
    if (music_bad_first && !i) return 0;
    t->read = mrd; t->ctx = &wavmem; t->size = wavlen;
    return 1;
}
static void music_draw(const struct music_view *v)
{
    if (!strncmp(v->status, "Finished", 8)) saw_finished = 1;
    if (!strncmp(v->status, "Cannot read", 11)) saw_bad_file = 1;
    if (strstr(v->status, "sound lags")) saw_lag = 1;
    if (!strcmp(v->status, "Paused")) {
        if (!saw_paused) pause_pos = v->elapsed;
        T_EQ(v->elapsed, pause_pos);
        saw_paused = 1;
    }
    T_TRUE(v->elapsed <= v->duration);
}
static struct music_platform music_pf = {1, music_title, music_open, music_draw};
static struct app_platform music_setup(unsigned secs, unsigned count)
{
    struct app_platform pf = pf_default(secs, 1);
    reset();
    music_pf.count = count;
    pf.music = &music_pf;
    music_opens = music_indices = saw_finished = saw_bad_file = saw_paused = 0;
    music_bad_first = 0;
    plug_spec(&replug_spec, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    plug_at = 5;
    return pf;
}
static void test_music_file_only(void)
{
    struct app_platform pf = music_setup(12, 1);
    static struct player p;
    static struct ring r;
    int16_t lr[2 * 512];
    uint8_t exp[4 * 512];
    const uint8_t *actual;
    struct dev_fmt f = {2, 2, 0};
    const uint16_t *sizes;
    unsigned n, i, primer = 0;
    T_EQ(app_run(&pf), 0);
    T_EQ(music_opens, 1); T_EQ(saw_finished, 1);
    player_init(&p, 44100, mrd, &wavmem, wavlen);
    player_next(&p);
    ring_init(&r); player_fill(&p, &r, 512);
    n = ring_get(&r, lr, 512);
    n = pcm_write(&f, lr, n, 128, exp);
    T_TRUE(fos_cap_pkts(&sizes) > UA_NF);
    for (i = 0; i < UA_NF; i++)
        primer += sizes[i];
    T_TRUE(fos_cap_bytes(&actual) >= primer + n);
    T_MEM(actual + primer, exp, n); /* after the silent first transfer: the song, not Ode to Joy */
    T_EQ(fos_stats()->starved_frames, 0);
    T_EQ(fos_stats()->late_frames, 0);
    check_clean();
}
static void test_music_controls(void)
{
    struct app_platform pf = music_setup(22, 2);
    T_EQ(app_run(&pf), 0);
    T_EQ(music_opens, 2); T_EQ(music_indices, 3); T_EQ(saw_finished, 1);
    check_clean();
    pf = music_setup(22, 1);
    key_at = 100; key_val = A_REPEAT;
    T_EQ(app_run(&pf), 0);
    T_TRUE(music_opens >= 3); T_EQ(saw_finished, 0);
    check_clean();
    pf = music_setup(8, 1);
    key_at = 200; key_val = A_PAUSE;
    key2_at = 400; key2_val = A_PAUSE;
    T_EQ(app_run(&pf), 0);
    T_EQ(saw_paused, 1);
    check_clean();
    pf = music_setup(5, 2);
    music_bad_first = 1;
    key_at = 100; key_val = A_NEXT;
    T_EQ(app_run(&pf), 0);
    T_EQ(saw_bad_file, 1); T_EQ(music_indices, 3);
    check_clean();
    pf = music_setup(3, 1);
    key_at = 100; key_val = A_RESTART;
    T_EQ(app_run(&pf), 0);
    T_EQ(music_opens, 2);
    check_clean();
}

static void test_music_replug_pending_cleanup(void)
{
    const unsigned flags[] = {FOS_ABORT_DELAYED, FOS_ABORT_ERROR, FOS_CLOSE_ERROR};
    const unsigned replug_ticks[] = {250, 110}; /* 250 = after cancel, 110 = before 65 ms cancel lands */
    for (unsigned r = 0; r < sizeof replug_ticks / sizeof replug_ticks[0]; r++) {
        for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++) {
            struct app_platform pf = music_setup(8, 1);
            replug_spec.flags = flags[i];
            unplug_at = 100;
            plug2_at = replug_ticks[r];
            key_at = 400; key_val = A_QUIT;
            T_EQ(app_run(&pf), 0);
            T_EQ(music_opens, 2); /* playback restarts cleanly without exiting or hanging */
            T_TRUE(fos_stats()->isoc_done > 0);
            if (flags[i] == FOS_ABORT_DELAYED)
                T_TRUE(fos_stats()->isoc_cancelled >= UA_NX);
            check_clean_detached(1, flags[i] == FOS_CLOSE_ERROR ? 0 : UA_NX);
        }
    }
}

static void test_abort_marks(void)
{
    /* 2026-11-16: TI's host controller driver marks an isochronous transfer
     * it aborts ("being aborted", byte 84 of its block) and never takes the
     * mark off; a block freed with it goes back to the OS's pool and the
     * next transfer that gets it, of any driver, can never be aborted (the
     * OS task that tries sleeps for good). The fake OS marks as TI does and
     * counts a marked block's free as a violation. */
    static struct ring audio;
    struct fos_spec sp;
    uint32_t saved;
    unsigned off;

    /* The adapter pulled out while it plays: the detach must abort (the
     * device is gone), and every transfer the abort took back is freed
     * without the mark. */
    reset();
    ring_init(&audio);
    plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    T_EQ(ua_init(), 0);
    fos_plug(&sp);
    tsleep();
    saved = tenter();
    T_EQ(ua_start(&audio), 0);
    tleave(saved);
    tsleep();
    tsleep();
    T_EQ(ua_state(), UA_STREAMING);
    fos_unplug();
    tsleep();
    T_EQ(ua_state(), UA_GONE);
    T_EQ(fos_stats()->abort_calls, 1);
    T_EQ(fos_stats()->marked_frees, 0);
    off = ua_osinfo()->marks_off;
    T_TRUE(off >= 1 && off <= UA_NX);       /* the ones the OS still had */
    T_EQ(ua_osinfo()->marks_odd, 0);
    T_EQ(fos_stats()->xfers_open, 0);
    T_EQ(fos_stats()->pipes_open, 0);

    /* Plugged back in and stopped while it plays (the exit, a setting
     * changed): the queued transfers play out, nothing is aborted, nothing
     * is marked. */
    fos_plug(&sp);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    saved = tenter();
    T_EQ(ua_start(&audio), 0);
    tleave(saved);
    tsleep();
    tsleep();
    T_EQ(ua_state(), UA_STREAMING);
    saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    T_EQ(fos_stats()->abort_calls, 1);      /* (the unplug's: none since) */
    T_EQ(ua_osinfo()->drained, 1);
    T_EQ(ua_osinfo()->marks_off, off);
    T_EQ(fos_stats()->marked_frees, 0);
    check_clean();

    /* A stop that cannot wait (ua_stop(NULL), the main loop's on a failed
     * stream) aborts, and takes the marks off as the detach does. */
    reset();
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    fos_plug(&sp);
    tsleep();
    saved = tenter();
    T_EQ(ua_start(&audio), 0);
    tleave(saved);
    tsleep();
    tsleep();
    saved = tenter();
    ua_stop(NULL);
    tleave(saved);
    T_EQ(fos_stats()->abort_calls, 1);
    T_TRUE(ua_osinfo()->marks_off >= 1);
    T_EQ(fos_stats()->marked_frees, 0);
    saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean();
}

static void test_hub_port_moves(void)
{
    /* Moving the sound card across hub ports 6 times (exceeding UA_MEM_SLOTS)
     * must cleanly recycle blocks, restart audio every time, and never
     * call close_pipe/abort_pipe after detach returned 0. */
    static struct ring audio;
    struct fos_spec sp;
    uint32_t saved;
    unsigned n;
    reset();
    ring_init(&audio);
    plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    T_EQ(ua_init(), 0);
    for (n = 0; n < 6; n++) {
        fos_plug(&sp);
        tsleep();
        T_EQ(ua_state(), UA_READY);
        saved = tenter();
        T_EQ(ua_start(&audio), 0);
        tleave(saved);
        tsleep();
        tsleep();
        T_EQ(ua_state(), UA_STREAMING);
        fos_unplug();
        tsleep();
        T_EQ(ua_state(), UA_GONE);
        /* MAIN poll seeing UA_GONE (like nspire_audio.c) calls ua_stop(NULL). */
        saved = tenter();
        ua_stop(NULL);
        tleave(saved);
        T_EQ(ua_state(), UA_GONE);
        T_EQ(fos_stats()->pipes_open, 0);
        T_EQ(fos_stats()->xfers_open, 0);
    }
    saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean();

    /* Also survive UA_MEM_SLOTS - 1 kept-at-detach blocks (e.g. close_pipe
     * failed inside detach) followed by another working port move and clean exit. */
    reset();
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    for (n = 0; n < 3; n++) {
        plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, n == 0 ? FOS_CLOSE_ERROR : 0, 0x0d8c, 0x0014);
        fos_plug(&sp);
        tsleep();
        T_EQ(ua_state(), UA_READY);
        saved = tenter();
        T_EQ(ua_start(&audio), 0);
        tleave(saved);
        tsleep();
        fos_unplug();
        tsleep();
        T_EQ(ua_state(), UA_GONE);
        saved = tenter();
        ua_stop(NULL);
        tleave(saved);
        T_EQ(ua_owns_usb(), 0);
    }
    saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    check_clean_detached(1, 0);
}

/* ---- idle (ua_set_idle / ua_idle) ----
 * The ring is fed a 24-bit frame counter: L its low 16 bits, R = 0x4000 +
 * its top 8: at volume 256 a packet played at full volume has R in
 * 0x4000-0x40FF exactly; a faded one (7/8 at most)
 * never does, silence is 0. */
static struct ring idle_ring;
static unsigned idle_ctr;

static void idle_feed(void)
{
    int16_t lr[2 * 256];
    unsigned i;
    while (ring_free(&idle_ring) >= 256) {
        for (i = 0; i < 256; i++) {
            lr[2 * i] = (int16_t) (uint16_t) idle_ctr++;
            lr[2 * i + 1] = (int16_t) (0x4000 + ((idle_ctr - 1) >> 16 & 0xFF));
        }
        ring_put(&idle_ring, lr, 256);
    }
}

/* MAIN asleep in the OS for ms frames, the ring kept full. */
static void idle_run(unsigned ms)
{
    while (ms--) {
        idle_feed();
        fos_run(1);
    }
}

static void idle_set(int on)
{
    uint32_t saved = tenter();      /* resuming queues transfers: an OS call from MAIN */
    ua_set_idle(on);
    tleave(saved);
}

/* ms until ua_idle(), at most max (max + 1: never). */
static unsigned idle_wait(unsigned max)
{
    unsigned ms = 0;
    while (!ua_idle() && ms <= max) {
        idle_run(1);
        ms++;
    }
    return ms;
}

/* MAIN's IRQ points inside ua_* calls: the USB task runs 3 ms there (a
 * completion, a requeue), as when MAIN is preempted in an IRQ window. */
static void (*idle_irq_hook)(void);
static void preempt_3ms(void) { fos_run(3); }

/* A CM108 streaming the counter for ticks OS ticks. */
static void idle_stream(unsigned ticks, unsigned flags)
{
    struct fos_spec sp;
    uint32_t saved;
    reset();
    ring_init(&idle_ring);
    idle_ctr = 1;
    ua_set_volume(256);
    T_EQ(ua_init(), 0);
    plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, flags, 0x0d8c, 0x0014);
    replug_spec = sp;
    fos_plug(&sp);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    idle_feed();
    fos_main_irq_hook(idle_irq_hook);
    saved = tenter();
    T_EQ(ua_start(&idle_ring), 0);
    tleave(saved);
    idle_run(10 * ticks);
}

static void idle_end(void)
{
    uint32_t saved = tenter();
    T_EQ(ua_shutdown(tsleep), 0);
    tleave(saved);
    ua_set_volume(192);
    check_clean();
}

static int16_t cap_s16(const uint8_t *b) { return (int16_t) (b[0] | b[1] << 8); }

/* Byte offset of captured packet k. */
static unsigned cap_off(unsigned k)
{
    const uint16_t *sizes;
    unsigned i, off = 0;
    (void) fos_cap_pkts(&sizes);
    for (i = 0; i < k; i++)
        off += sizes[i];
    return off;
}

/* Captured packet k: 0 silence, 1 faded, 2 full volume (R 0x40xx), 3 mixed. */
static int cap_kind(unsigned k, int *first_r)
{
    const uint16_t *sizes;
    const uint8_t *bytes;
    unsigned off = cap_off(k), i, zero = 0, full = 0, n;
    (void) fos_cap_pkts(&sizes);
    (void) fos_cap_bytes(&bytes);
    n = sizes[k] / 4;
    for (i = 0; i < n; i++) {
        int16_t r = cap_s16(bytes + off + 4 * i + 2);
        zero += r == 0 && cap_s16(bytes + off + 4 * i) == 0;
        full += (r & 0xFF00) == 0x4000;
    }
    if (first_r)
        *first_r = n ? cap_s16(bytes + off + 2) : 0;
    return zero == n ? 0 : full == n ? 2 : !full && !zero ? 1 : 3;
}

/* Every full-volume frame on the wire continues the counter: nothing played
 * twice, nothing out of order. Returns the number of full-volume packets. */
static unsigned cap_in_order(void)
{
    const uint16_t *sizes;
    const uint8_t *bytes;
    unsigned np = fos_cap_pkts(&sizes), k, i, off = 0, full = 0, bad = 0;
    int have = 0;
    uint32_t last = 0;
    (void) fos_cap_bytes(&bytes);
    for (k = 0; k < np; off += sizes[k], k++) {
        if (cap_kind(k, NULL) != 2)
            continue;
        full++;
        for (i = 0; i < sizes[k] / 4u; i++) {
            uint32_t v = (uint32_t) (uint16_t) cap_s16(bytes + off + 4 * i) |
                         (uint32_t) (cap_s16(bytes + off + 4 * i + 2) & 0xFF) << 16;
            if (have && (((v - last) & 0xFFFFFF) == 0 || ((v - last) & 0xFFFFFF) >= 0x800000))
                bad++;
            last = v;
            have = 1;
        }
    }
    T_EQ(bad, 0);
    return full;
}

static void test_idle_mid_stream(void)
{
    const uint16_t *sizes;
    struct ua_counters c;
    unsigned ms, np, sub, done, wire, k, softs, ka;
    int r[UA_NF];
    idle_stream(30, 0);
    T_EQ(ua_state(), UA_STREAMING);
    T_EQ(ua_idle(), 0);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    idle_set(1);
    T_EQ(ua_idle(), 0);             /* the queued transfers still play */
    /* 48 ms queued, the one fade-out behind them: then nothing. */
    ms = idle_wait(100);
    printf("    idle %u ms after ua_set_idle(1)\n", ms);
    T_TRUE(ms <= UA_NX * UA_NF + UA_NF + 8);
    T_EQ(ua_idle(), 1);
    T_EQ(ua_state(), UA_STREAMING);
    T_EQ(ua_attached(), 1);
    T_EQ(ua_owns_usb(), 1);         /* pipe kept, device claimed */
    T_EQ(ua_busy(), 0);
    T_EQ(fos_stats()->pipes_open, 1);
    T_EQ(fos_stats()->xfers_open, UA_NX);
    T_EQ(fos_stats()->abort_calls, 0);
    T_EQ(fos_stats()->close_calls, 0);
    ua_counters(&c);
    T_EQ(c.inflight, 0);
    /* The last transfer before the gap faded out: one step a packet, the
     * last packet silent; the one before it at full volume. */
    np = fos_cap_pkts(&sizes);
    for (k = 0; k < UA_NF; k++)
        (void) cap_kind(np - UA_NF + k, &r[k]);
    T_EQ(cap_kind(np - UA_NF - 1, NULL), 2);
    T_EQ(cap_kind(np - 1, NULL), 0);
    for (k = 0; k + 1 < UA_NF; k++) {
        T_TRUE(r[k] > r[k + 1]);
        T_TRUE(r[k] < 0x4000);
    }
    /* While idle, 3 seconds, ua_set_idle(1) repeated as a main loop would:
     * nothing but one transfer of silence a second (the keep-alive), each
     * back before the next; no audio on the wire. Without the calls,
     * nothing at all. */
    sub = fos_stats()->isoc_submitted;
    done = fos_stats()->isoc_done;
    wire = fos_stats()->wire_frames;
    softs = fos_stats()->softintrs;
    np = fos_cap_pkts(&sizes);
    ka = 0;
    for (k = 0; k < 300; k++) {
        idle_set(1);
        idle_run(10);
        ka += ua_idle();
    }
    ua_counters(&c);
    printf("    3 s idle: %u keep-alives, %u soft interrupts (rollovers and the keep-alives')\n",
           c.keepalives, fos_stats()->softintrs - softs);
    T_TRUE(c.keepalives >= 2 && c.keepalives <= 3);
    T_TRUE(ka >= 300 - 2 * c.keepalives);      /* idle but for 12-20 ms a keep-alive */
    T_EQ(fos_stats()->isoc_submitted, sub + c.keepalives);
    T_EQ(fos_stats()->isoc_done, done + c.keepalives);
    T_EQ(fos_stats()->wire_frames, wire + UA_NF * c.keepalives);
    T_EQ(fos_cap_pkts(&sizes), np + UA_NF * c.keepalives);
    for (k = np; k < fos_cap_pkts(&sizes); k++)
        T_EQ(cap_kind(k, NULL), 0);
    T_TRUE(fos_stats()->softintrs - softs <= 3 + 2 * c.keepalives);
    sub = fos_stats()->isoc_submitted;
    idle_run(3000);
    T_EQ(fos_stats()->isoc_submitted, sub);
    T_EQ(ua_idle(), 1);
    ua_counters(&c);
    T_EQ(c.starved, 0);             /* draining into idle is not a starved queue */
    T_EQ(cap_in_order() > 0, 1);
    idle_end();
}

static void test_idle_resume(void)
{
    const uint16_t *sizes;
    struct ua_counters c;
    unsigned np0, sub, t0, k, lat_first = 0, lat_audio = 0, starved, np;
    int kind, r[UA_NF];
    idle_stream(30, 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    idle_run(500);
    T_EQ(ua_idle(), 1);
    np0 = fos_cap_pkts(&sizes);
    sub = fos_stats()->isoc_submitted;
    t0 = fos_frame();
    idle_set(0);
    T_EQ(ua_idle(), 0);
    T_EQ(fos_stats()->isoc_submitted, sub + 1);    /* silence; the rest behind it from its completion */
    while (fos_frame() - t0 < 100 && !lat_audio) {
        idle_run(1);
        np = fos_cap_pkts(&sizes);
        if (np > np0 && !lat_first)
            lat_first = fos_frame() - t0;
        for (k = np0; k < np && !lat_audio; k++)
            if (cap_kind(k, NULL) != 0)
                lat_audio = fos_frame() - t0;
    }
    printf("    resume: first packet on the wire %u ms, first audio %u ms after ua_set_idle(0)\n",
           lat_first, lat_audio);
    T_TRUE(lat_first >= 1 && lat_first <= 6);
    T_TRUE(lat_audio >= 1 && lat_audio <= 12 + UA_NF);   /* (the primer's UA_NF ms of silence first) */
    idle_run(100);
    /* Silence first (the transfer the OS links alone), then a fade-in, then
     * the ring at full volume, continuing the counter. */
    for (k = 0; k < UA_NF; k++)
        T_EQ(cap_kind(np0 + k, NULL), 0);
    for (k = 0; k < UA_NF; k++) {
        kind = cap_kind(np0 + UA_NF + k, &r[k]);
        T_TRUE(k == 0 ? kind == 0 : kind == 1);
        if (k)
            T_TRUE(r[k] > r[k - 1]);
    }
    T_EQ(cap_kind(np0 + 2 * UA_NF, NULL), 2);
    /* Then continuous: no frame without a packet, nothing late, the full
     * queue back. */
    starved = fos_stats()->starved_frames;
    ua_counters(&c);
    idle_run(2000);
    T_EQ(fos_stats()->starved_frames, starved);
    T_EQ(fos_stats()->late_frames, 0);
    {
        struct ua_counters c2;
        ua_counters(&c2);
        T_EQ(c2.starved, c.starved);
        T_EQ(c2.inflight, UA_NX);
        T_EQ(c2.underrun_pkts, 0);
        T_TRUE(c2.completions - c.completions >= 2000 / UA_NF - 2);
    }
    T_TRUE(fos_stats()->sitd_max <= UA_NX * UA_NF);
    cap_in_order();
    idle_end();
}

static void test_idle_resume_while_draining(void)
{
    /* Resumed before the queued transfers have played out: nothing stops;
     * the parked ones go in behind them, in order. */
    struct ua_counters c;
    unsigned sub, t;
    idle_stream(30, 0);
    for (t = 1; t < 40; t += 3) {
        idle_set(1);
        idle_run(t);
        idle_set(0);
        idle_run(60);
        ua_counters(&c);
        T_EQ(c.inflight, UA_NX);
        T_EQ(ua_idle(), 0);
    }
    /* Resumed before anything came back: not even a fade. */
    idle_set(1);
    sub = fos_stats()->isoc_submitted;
    idle_set(0);
    T_EQ(fos_stats()->isoc_submitted, sub);
    idle_run(100);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    T_EQ(c.underrun_pkts, 0);
    T_EQ(fos_stats()->violations, 0);
    cap_in_order();
    idle_end();
}

static void test_idle_then_detach(void)
{
    uint32_t saved;
    unsigned sub;
    idle_stream(30, 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    fos_unplug();
    tsleep();
    T_EQ(ua_state(), UA_GONE);
    T_EQ(ua_idle(), 0);
    T_EQ(ua_attached(), 0);
    T_EQ(ua_owns_usb(), 0);
    T_EQ(fos_stats()->pipes_open, 0);
    T_EQ(fos_stats()->xfers_open, 0);
    T_EQ(fos_stats()->marked_frees, 0);
    T_EQ(ua_osinfo()->marks_off, 0);    /* the abort found nothing queued */
    saved = tenter();
    ua_stop(NULL);
    tleave(saved);
    /* Plugged back in (a hub's port power-cycled): it streams again, not idle. */
    fos_plug(&replug_spec);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    sub = fos_stats()->isoc_submitted;
    saved = tenter();
    T_EQ(ua_start(&idle_ring), 0);
    tleave(saved);
    T_EQ(fos_stats()->isoc_submitted, sub + UA_START_QUEUED);
    idle_run(100);
    T_EQ(ua_idle(), 0);
    T_TRUE(fos_stats()->isoc_done > 10);

    /* Unplugged while draining into idle: the detach aborts what is queued,
     * as for a stream. */
    idle_set(1);
    idle_run(5);
    T_EQ(ua_idle(), 0);
    fos_unplug();
    tsleep();
    T_EQ(ua_state(), UA_GONE);
    T_EQ(fos_stats()->marked_frees, 0);
    T_EQ(fos_stats()->xfers_open, 0);
    saved = tenter();
    ua_stop(NULL);
    tleave(saved);

    /* Idle asked while nothing is attached holds for the next device. */
    idle_set(1);
    fos_plug(&replug_spec);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    sub = fos_stats()->isoc_submitted;
    saved = tenter();
    T_EQ(ua_start(&idle_ring), 0);
    tleave(saved);
    T_EQ(fos_stats()->isoc_submitted, sub);
    T_EQ(ua_idle(), 1);
    idle_set(0);
    T_EQ(fos_stats()->isoc_submitted, sub + 1);
    idle_run(100);
    T_EQ(ua_idle(), 0);
    idle_end();
}

static void test_idle_then_stop(void)
{
    uint32_t saved;
    idle_stream(30, 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    saved = tenter();
    ua_stop(tsleep);
    tleave(saved);
    T_EQ(ua_state(), UA_STOPPED);
    T_EQ(ua_idle(), 0);
    T_EQ(fos_stats()->abort_calls, 0);
    T_EQ(ua_osinfo()->drained, 1);
    T_EQ(fos_stats()->pipes_open, 0);
    T_EQ(fos_stats()->xfers_open, 0);
    idle_set(0);                    /* harmless now */
    T_EQ(fos_stats()->isoc_submitted, fos_stats()->isoc_done);
    idle_end();

    /* Shut down while draining: it waits for the queued ones, aborts nothing. */
    idle_stream(30, 0);
    idle_set(1);
    idle_run(3);
    idle_end();
    T_EQ(fos_stats()->abort_calls, 0);

    /* A stop that cannot wait, from idle: nothing is queued to abort. */
    idle_stream(30, 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    saved = tenter();
    ua_stop(NULL);
    tleave(saved);
    T_EQ(fos_stats()->marked_frees, 0);
    T_EQ(ua_osinfo()->marks_off, 0);
    idle_end();
}

static void idle_in_control(void)
{
    ua_set_idle(1);
    T_EQ(ua_busy(), 1);             /* the attach is inside the OS */
    T_EQ(ua_idle(), 0);
    fos_control_hook(NULL);
}

static void test_idle_during_attach(void)
{
    struct fos_spec sp;
    uint32_t saved;
    /* Every state, before anything: harmless. */
    reset();
    ua_set_idle(1);
    T_EQ(ua_idle(), 0);
    ua_set_idle(0);
    ring_init(&idle_ring);
    idle_ctr = 1;
    ua_set_volume(256);
    T_EQ(ua_init(), 0);
    T_EQ(ua_state(), UA_WAITING);
    ua_set_idle(1);
    ua_set_idle(0);
    T_EQ(ua_idle(), 0);
    /* Asked while the attach is inside a control request. */
    fos_control_hook(idle_in_control);
    plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, 0, 0x0d8c, 0x0014);
    fos_plug(&sp);
    tsleep();
    T_EQ(ua_state(), UA_READY);
    T_EQ(ua_idle(), 0);
    idle_feed();
    saved = tenter();
    T_EQ(ua_start(&idle_ring), 0);
    tleave(saved);
    T_EQ(ua_state(), UA_STREAMING);
    T_EQ(ua_idle(), 1);             /* started idle: nothing queued */
    T_EQ(ua_owns_usb(), 1);
    idle_run(500);
    T_EQ(fos_stats()->isoc_submitted, 0);
    T_EQ(fos_stats()->wire_frames, 0);
    /* Resumed: silence, the fade-in, then the ring (what was put before
     * the resume dropped). */
    idle_set(0);
    T_EQ(fos_stats()->isoc_submitted, 1);
    idle_run(200);
    T_EQ(cap_kind(0, NULL), 0);
    T_EQ(cap_kind(UA_NF + 1, NULL), 1);
    T_EQ(cap_kind(2 * UA_NF, NULL), 2);
    T_TRUE(cap_in_order() > 100);
    idle_end();

    /* A device whose setup failed: idle changes nothing. */
    reset();
    T_EQ(ua_init(), 0);
    plug_spec(&sp, cfg_cm108, sizeof cfg_cm108, FOS_NO_ISOC_OPEN, 0x0d8c, 0x0014);
    fos_plug(&sp);
    tsleep();
    T_EQ(ua_state(), UA_FAILED);
    idle_set(1);
    T_EQ(ua_idle(), 0);
    idle_set(0);
    T_EQ(ua_state(), UA_FAILED);
    T_EQ(fos_stats()->isoc_submitted, 0);
    fos_unplug();
    tsleep();
    idle_end();
}

static void test_idle_rapid_toggling(void)
{
    /* Toggled every 1-13 ms for 3 seconds: every transfer queued once at a
     * time (the fake OS flags a resubmission), the counter in order, never
     * more than the queue, and it settles either way. */
    struct ua_counters c;
    unsigned k, seed = 12345, on = 0, pass;
    for (pass = 0; pass < 2; pass++) {
    /* pass 1: preempted at every IRQ point of MAIN's (ua_start's submit
     * loop then sees completions: they wait for requeue_deferred) */
    idle_irq_hook = pass ? preempt_3ms : NULL;
    idle_stream(30, 0);
    for (k = 0; k < 3000;) {
        unsigned ms = 1 + (seed >> 16) % 13;
        seed = seed * 1103515245u + 12345u;
        on ^= 1;
        idle_set((int) on);
        if ((seed >> 20) & 1)
            idle_set((int) on);     /* repeated */
        idle_run(ms);
        k += ms;
    }
    T_EQ(fos_stats()->violations, 0);
    if (fos_stats()->violations) printf("    violation: %s\n", fos_stats()->violation);
    T_TRUE(fos_stats()->sitd_max <= UA_NX * UA_NF);
    idle_set(0);
    idle_run(200);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    T_EQ(c.requeue_fail, 0);
    T_EQ(c.errors, 0);
    T_EQ(ua_idle(), 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    T_EQ(ua_idle(), 1);
    T_EQ(fos_stats()->abort_calls, 0);
    T_TRUE(cap_in_order() > 100);
    idle_irq_hook = NULL;
    idle_end();
    }
}

/* Resume after gap_ms of idle. calls: MAIN calls ua_set_idle(1) every 10 ms
 * (the keep-alive's clock), or never. flr: fos_flr_resched. Returns the ms
 * from ua_set_idle(0) to the first audible packet (~0: never within 9 s);
 * *hole: frames without a packet once audio played (1 s watched). */
static unsigned gap_resume(unsigned gap_ms, int calls, int flr, unsigned *first_pkt, unsigned *hole)
{
    const uint16_t *sizes;
    unsigned np0, t0, k, np, lat_first = 0, lat_audio = 0, st0, late0;
    idle_stream(30, 0);
    fos_flr_resched(flr);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    for (k = 0; k < gap_ms; k += 10) {
        if (calls)
            idle_set(1);
        idle_run(10);
    }
    np0 = fos_cap_pkts(&sizes);
    t0 = fos_frame();
    idle_set(0);
    while (fos_frame() - t0 < 9000 && !lat_audio) {
        idle_run(1);
        np = fos_cap_pkts(&sizes);
        if (np > np0 && !lat_first)
            lat_first = fos_frame() - t0;
        for (k = np0; k < np && !lat_audio; k++)
            if (cap_kind(k, NULL) != 0)
                lat_audio = fos_frame() - t0;
    }
    idle_run(30);                   /* (the silence before the audio is counted when the next packet goes in) */
    st0 = fos_stats()->starved_frames;
    late0 = fos_stats()->late_frames;
    idle_run(1000);
    *hole = fos_stats()->starved_frames - st0 + fos_stats()->late_frames - late0;
    *first_pkt = lat_first;
    T_EQ(fos_stats()->violations, 0);
    idle_end();
    return lat_audio ? lat_audio : ~0u;
}

static void test_idle_long_gaps(void)
{
    /* The OS's last linked frame for the pipe, compared in 16-bit micro-
     * frames (wrap 8.192 s): with the keep-alive it never gets older than
     * about a second, whichever way the soft interrupt treats an empty pipe.
     * Gaps 0-20 s, finer around the wraps (4.096 s and 8.192 s apart). */
    static const unsigned fine[] = { 4000, 4050, 4090, 4100, 4110, 4150, 4200, 6250, 6500, 7000, 7020,
                                     8100, 8150, 8190, 8200, 8250, 12250, 12300, 14500, 15000, 15250,
                                     16300, 16390, 16400, 16500 };
    unsigned g, i, flr, calls, lat, first, hole, worst[2][2] = { { 0, 0 }, { 0, 0 } }, holes[2][2] = { { 0, 0 }, { 0, 0 } };
    for (flr = 0; flr < 2; flr++)
        for (calls = 0; calls < 2; calls++)
            for (i = 0; i < 80 + sizeof fine / sizeof fine[0]; i++) {
                g = i < 80 ? i * 250 : fine[i - 80];
                lat = gap_resume(g, (int) calls, (int) flr, &first, &hole);
                if (!calls && lat > 100)
                    printf("    gap %u ms (flr %u), no calls: first packet %u ms, audio %u ms, hole %u\n", g, flr, first, lat, hole);
                if (calls) {
                    if (lat > 14 + UA_NF || first > 6 || hole)
                        printf("    gap %u ms (flr %u): first packet %u ms, audio %u ms, hole %u\n", g, flr, first, lat, hole);
                    T_TRUE(first <= 6);
                    T_TRUE(lat <= 14 + UA_NF);
                }
                T_EQ(hole, 0);
                if (lat > worst[flr][calls]) worst[flr][calls] = lat;
                holes[flr][calls] += hole;
            }
    printf("    resume after 0-20 s idle: audio within %u ms (keep-alive), %u ms (no ua_set_idle(1) calls); "
           "the OS forgetting an empty pipe's frame: %u / %u ms\n",
           worst[0][1], worst[0][0], worst[1][1], worst[1][0]);
}

/* A detach (and, with replug, the device back at once, which may take the
 * same block) at MAIN's n-th IRQ point inside a ua_* call. */
static unsigned irq_points, irq_detach_at, irq_replug;
static void detach_at_irq_point(void)
{
    if (++irq_points != irq_detach_at)
        return;
    fos_unplug();
    fos_run(0);
    if (irq_replug) {
        fos_plug(&replug_spec);
        fos_run(0);
    }
}

static void test_idle_detach_at_irq_points(void)
{
    uint32_t saved;
    unsigned n, replug, sub, where, hits = 0;
    for (where = 0; where < 3; where++)
        for (replug = 0; replug < 2; replug++)
            for (n = 1; n <= 16; n++) {
                idle_stream(where == 0 ? 0 : 30, 0);
                if (where == 0) {
                    /* ua_start: unstarted; restart it with the hook on */
                    saved = tenter();
                    ua_stop(tsleep);
                    tleave(saved);
                    idle_end();
                    reset();
                    ring_init(&idle_ring);
                    ua_set_volume(256);
                    T_EQ(ua_init(), 0);
                    fos_plug(&replug_spec);
                    tsleep();
                    T_EQ(ua_state(), UA_READY);
                    idle_feed();
                } else {
                    idle_set(1);
                    T_TRUE(idle_wait(100) <= 100);
                    if (where == 2)
                        for (sub = 0; sub < 98; sub++) {    /* the keep-alive's clock runs */
                            idle_set(1);
                            idle_run(10);
                        }
                }
                irq_points = 0;
                irq_detach_at = n;
                irq_replug = replug;
                fos_main_irq_hook(detach_at_irq_point);
                sub = fos_stats()->isoc_submitted;
                saved = tenter();
                if (where == 0)
                    T_EQ(ua_start(&idle_ring), 0);
                else if (where == 1)
                    ua_set_idle(0);
                else
                    for (sub = 0; sub < 3; sub++) {
                        idle_run(10);
                        ua_set_idle(1);
                    }
                tleave(saved);
                fos_main_irq_hook(NULL);
                if (irq_points >= n) {
                    hits++;
                    /* nothing touched the old device's transfers after its
                     * detach, nothing of the new device's was queued */
                    T_EQ(fos_stats()->violations, 0);
                    if (fos_stats()->violations) printf("    violation (%u %u %u): %s\n", where, replug, n, fos_stats()->violation);
                    T_EQ(ua_state(), replug ? UA_READY : UA_GONE);
                    T_EQ(fos_stats()->xfers_open, replug ? UA_NX : 0);
                    T_EQ(ua_idle(), 0);
                }
                tsleep();
                saved = tenter();
                ua_stop(NULL);
                tleave(saved);
                if (replug && ua_state() != UA_GONE) {
                    fos_unplug();
                    tsleep();
                }
                idle_end();
            }
    printf("    a detach at %u IRQ points of ua_start, a resume and a keep-alive\n", hits);
    T_EQ(hits, 2 * ((UA_START_QUEUED + 1) + 2 + 2));   /* ua_start UA_START_QUEUED + 1, a resume 2, a keep-alive 2 */
}

static void test_idle_ring_clear_and_pause(void)
{
    struct ua_counters c;
    unsigned at, k, np0;
    const uint16_t *sizes;
    /* A discard request while draining into idle, and while idle: acknowledged
     * at once by the next ua_set_idle(1) (gpSP waits for it to unpause). */
    idle_stream(30, 0);
    idle_set(1);
    ring_request_clear(&idle_ring);
    T_TRUE(idle_ring.cleared_seq != idle_ring.clear_seq);
    idle_set(1);
    T_EQ(idle_ring.cleared_seq, idle_ring.clear_seq);
    T_EQ(ua_idle(), 0);             /* (still draining) */
    idle_run(2);
    ring_request_clear(&idle_ring);
    idle_run(30);                   /* the completions parking acknowledge it too */
    T_EQ(idle_ring.cleared_seq, idle_ring.clear_seq);
    T_TRUE(idle_wait(100) <= 100);
    ring_request_clear(&idle_ring);
    idle_set(1);
    T_EQ(idle_ring.cleared_seq, idle_ring.clear_seq);
    /* What was put while idle is dropped at the resume: the first audio on
     * the wire is what came after it. */
    idle_run(400);
    T_TRUE(ring_used(&idle_ring) > RING_FRAMES / 2);
    np0 = fos_cap_pkts(&sizes);
    idle_set(0);
    T_EQ(ring_used(&idle_ring), 44100u * UA_NX * UA_NF / 1000u);     /* the newest 48 ms */
    idle_run(100);
    for (k = np0; k < fos_cap_pkts(&sizes) && cap_kind(k, NULL) != 2; k++) ;
    T_TRUE(k < fos_cap_pkts(&sizes));
    idle_end();

    /* Paused and idle together; resumed while paused: silence, and the fade-in
     * waits for the first audio after the pause. */
    idle_stream(30, 0);
    ua_set_pause(1);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    idle_run(300);
    np0 = fos_cap_pkts(&sizes);
    idle_set(0);
    idle_run(200);
    for (k = np0; k < fos_cap_pkts(&sizes); k++)
        T_EQ(cap_kind(k, NULL), 0);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    np0 = fos_cap_pkts(&sizes);
    ua_set_pause(0);
    idle_run(200);
    for (k = np0; k < fos_cap_pkts(&sizes) && cap_kind(k, NULL) == 0; k++) ;
    for (at = 0; at < UA_NF - 1; at++)
        T_EQ(cap_kind(k + at, NULL), 1);
    T_EQ(cap_kind(k + UA_NF - 1, NULL), 2);
    cap_in_order();
    idle_end();
}

static void resume_in_park(void)
{
    ua_set_idle(0);                 /* MAIN, while a completion parks */
    fos_park_hook(NULL);
}

static void test_idle_resume_failures(void)
{
    struct ua_counters c, c0;
    /* MAIN resumes inside the park of the last queued transfer: that
     * completion sees it and queues its transfer (and two parked) after all. */
    idle_stream(30, 0);
    idle_set(1);
    do {
        idle_run(1);
        ua_counters(&c);
    } while (c.inflight > 1);
    fos_park_hook(resume_in_park);
    idle_run(200);
    T_EQ(ua_idle(), 0);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    T_EQ(fos_stats()->violations, 0);
    cap_in_order();
    idle_end();

    /* Resumed while draining, the last queued completion's own requeue
     * fails: the parked ones still go in (no wait for another resume). */
    idle_stream(30, 0);
    idle_set(1);
    do {
        idle_run(1);
        ua_counters(&c);
    } while (c.inflight > 1);
    idle_set(0);
    fos_fail_transfers(1);
    idle_run(100);
    ua_counters(&c);
    T_EQ(c.requeue_fail, 1);
    T_EQ(c.inflight, UA_NX - 1);
    T_EQ(ua_idle(), 0);
    /* A resume while the last one queued is the fade-out is no starved queue
     * (MAIN was not away). */
    c0 = c;
    idle_set(1);
    do {
        idle_run(1);
        ua_counters(&c);
    } while (c.inflight > 1);
    idle_set(0);
    idle_run(200);
    ua_counters(&c);
    T_EQ(c.starved, c0.starved);
    T_TRUE(c.min_queued_ms > 0);
    T_TRUE(c.completions > c0.completions + 160 / UA_NF);   /* (20 of 8 ms in the 200) */
    idle_end();

    /* A stream that lost every transfer (all requeues failed), not idle:
     * ua_set_idle(0) primes it again. */
    idle_stream(30, 0);
    fos_fail_transfers(UA_NX);
    idle_run(100);
    ua_counters(&c);
    T_EQ(c.inflight, 0);
    idle_set(0);
    idle_run(100);
    ua_counters(&c);
    T_EQ(c.inflight, UA_NX);
    T_EQ(ua_state(), UA_STREAMING);
    idle_end();
}

/* A detach where MAIN's held interrupts come back after it set up a
 * transfer (before usbd_transfer), or inside usbd_transfer: the residual
 * window (ua_os.c submit_main). The block is quarantined, never reused; no
 * transfer reaches the dead pipe in the first case; in the second the fake
 * OS, like TI's for an aborting pipe, refuses it. */
static unsigned held_at, held_n, held_replug;
static void detach_now(void)
{
    if (++held_n != held_at)
        return;
    fos_unplug();
    fos_run(0);
    if (held_replug) {
        fos_plug(&replug_spec);
        fos_run(0);
    }
}

static void test_idle_detach_in_transfer_window(void)
{
    uint32_t saved;
    unsigned inside, replug, at, sub, late, hits = 0;
    for (inside = 0; inside < 2; inside++)
        for (replug = 0; replug < 2; replug++)
            for (at = 1; at <= 2; at++) {
                idle_stream(30, 0);
                idle_set(1);
                T_TRUE(idle_wait(100) <= 100);
                for (sub = 0; sub < 98; sub++) {        /* the keep-alive's clock runs */
                    idle_set(1);
                    idle_run(10);
                }
                fos_expect_kept_at_detach(1);
                held_n = 0;
                held_at = at;
                held_replug = replug;
                if (inside)
                    fos_main_transfer_hook(detach_now);
                else
                    fos_held_irq_hook(detach_now);
                sub = fos_stats()->isoc_submitted;
                late = ua_osinfo()->late_detach;
                saved = tenter();
                if (at == 1)
                    ua_set_idle(0);             /* the resume's transfer */
                else
                    for (held_n = 1; held_n == 1 && fos_frame() < 1000000;) {
                        idle_run(10);
                        ua_set_idle(1);         /* the keep-alive's (the 2nd one) */
                    }
                tleave(saved);
                fos_held_irq_hook(NULL);
                fos_main_transfer_hook(NULL);
                T_EQ(held_n, at);
                hits++;
                T_EQ(ua_osinfo()->late_detach, late + 1);
                T_EQ(fos_stats()->isoc_submitted, sub);  /* nothing queued on the dead pipe */
                T_EQ(fos_stats()->dead_transfers, inside);  /* not even tried, but inside the call */
                T_EQ(fos_stats()->violations, 0);
                if (fos_stats()->violations) printf("    violation: %s\n", fos_stats()->violation);
                T_EQ(fos_stats()->pipes_kept, 1);           /* quarantined with its transfers */
                T_EQ(fos_stats()->xfers_kept, UA_NX);
                T_EQ(ua_state(), replug ? UA_READY : UA_GONE);
                if (replug) {
                    /* the new device took another block and streams */
                    saved = tenter();
                    T_EQ(ua_start(&idle_ring), 0);
                    tleave(saved);
                    idle_run(100);
                    T_TRUE(fos_stats()->isoc_done > 10);
                    fos_unplug();
                    tsleep();
                }
                saved = tenter();
                ua_stop(NULL);
                T_EQ(ua_shutdown(tsleep), 0);
                tleave(saved);
                ua_set_volume(192);
                check_clean_detached(1, UA_NX);
            }
    T_EQ(hits, 8);
}

static void test_idle_resume_keeps_priming(void)
{
    /* The producer kept the ring full while idle (gpSP primes while its
     * output is the card) and goes on at the stream's rate after the
     * resume: the newest 48 ms (a full queue) stay, the rest is dropped,
     * and nothing underruns. */
    struct ua_counters c0, c;
    int16_t lr[2 * 64];
    unsigned ms, acc = 0, n, i, keep = 44100u * UA_NX * UA_NF / 1000u;
    idle_stream(30, 0);
    idle_set(1);
    T_TRUE(idle_wait(100) <= 100);
    idle_run(300);
    T_EQ(ring_free(&idle_ring) < 256, 1);
    ua_counters(&c0);
    idle_set(0);
    T_EQ(ring_used(&idle_ring), keep);
    for (ms = 0; ms < 500; ms++) {
        acc += 44100;
        n = acc / 1000;
        acc %= 1000;
        for (i = 0; i < n; i++) {
            lr[2 * i] = (int16_t) (uint16_t) idle_ctr++;
            lr[2 * i + 1] = (int16_t) (0x4000 + ((idle_ctr - 1) >> 16 & 0xFF));
        }
        T_EQ(ring_put(&idle_ring, lr, n), n);
        fos_run(1);
    }
    ua_counters(&c);
    T_EQ(c.underrun_pkts, c0.underrun_pkts);
    T_EQ(c.inflight, UA_NX);
    T_TRUE(cap_in_order() > 100);
    idle_end();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    logbuf = malloc(4u << 20);
    corpus_fix();
    make_wav();
    T_RUN(test_cm108_streams);
    T_RUN(test_44k_pattern);
    T_RUN(test_headset);
    T_RUN(test_isoc_refused);
    T_RUN(test_isoc_xfer_refused);
    T_RUN(test_completion_errors);
    T_RUN(test_never_done);
    T_RUN(test_pending_abort);
    T_RUN(test_retained_resources_block_offers);
    T_RUN(test_attach_after_match_never_fails);
    T_RUN(test_stop_during_requeue);
    T_RUN(test_main_away_from_the_os);
    T_RUN(test_queue_ms);
    T_RUN(test_what_the_queue_cannot_fix);
    T_RUN(test_endpoint_size_told_to_the_os);
    T_RUN(test_second_lines);
    T_RUN(test_oversized_packets);
    T_RUN(test_unplug_mid_stream);
    T_RUN(test_replug_after_unplug);
    T_RUN(test_missing_isoc);
    T_RUN(test_missing_required);
    T_RUN(test_raw_buffers);
    T_RUN(test_uac2_and_mic);
    T_RUN(test_unsupported_replug);
    T_RUN(test_derive);
    T_RUN(test_keys);
    T_RUN(test_slow_host_start);
    T_RUN(test_quit_during_setup);
    T_RUN(test_quit_during_control);
    T_RUN(test_quit_during_completion);
    T_RUN(test_result_before_descriptors);
    T_RUN(test_music_file_only);
    T_RUN(test_music_controls);
    T_RUN(test_music_replug_pending_cleanup);
    T_RUN(test_abort_marks);
    T_RUN(test_hub_port_moves);
    T_RUN(test_idle_mid_stream);
    T_RUN(test_idle_resume);
    T_RUN(test_idle_resume_while_draining);
    T_RUN(test_idle_then_detach);
    T_RUN(test_idle_then_stop);
    T_RUN(test_idle_during_attach);
    T_RUN(test_idle_rapid_toggling);
    T_RUN(test_idle_long_gaps);
    T_RUN(test_idle_detach_at_irq_points);
    T_RUN(test_idle_ring_clear_and_pause);
    T_RUN(test_idle_resume_failures);
    T_RUN(test_idle_detach_in_transfer_window);
    T_RUN(test_idle_resume_keeps_priming);
    fos_fini();
    free(wavfile);
    free(logbuf);
    return T_DONE();
}
