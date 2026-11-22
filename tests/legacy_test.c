/* The driver as it was on the calculator on 2026-11-14 (commit 3b467c4:
 * 4 transfers of 8 packets, 48 kHz stereo = 192-byte packets), built from
 * git by the Makefile, against the simulated OS stack. With the controller
 * model that needs one pass of the frame list per start-split it must
 * reproduce the log of a hardware run, snapshot by snapshot: 32 ms of audio per
 * 1036 ms. With a controller that sends a packet's start-splits in its own
 * frame the same code plays continuously: the simulator is not rigged. */
#include "t.h"
#include "ua_os.h"
#include "fake/fake_os.h"
#include "corpus.h"

static struct ring audio;

static void start_at_frame(int model, unsigned frame)
{
    static struct fos_spec s;
    fos_init();
    fos_set_hw(model);
    memset(&s, 0, sizeof s);
    s.vid = 0x0d8c; s.pid = 0x0014; s.cfg = cfg_cm108; s.cfg_len = sizeof cfg_cm108;
    ring_init(&audio);
    T_EQ(ua_init(), 0);
    fos_plug(&s);
    fos_sleep();
    T_EQ(ua_state(), UA_READY);
    T_EQ(ua_desc()->rate, 48000);
    while (fos_frame() % FOS_FRAME_LIST != frame)
        fos_sleep();
    T_EQ(ua_start(&audio), 0);
}

static void stop(void)
{
    /* That driver stopped by an abort and freed its 4 transfers as the
     * abort left them, with TI's "being aborted" mark on each (2026-11-16:
     * the OS's pool hands such a block to the next control, interrupt or
     * bulk transfer, whose abort then never returns). Today's driver takes
     * the mark off, or aborts nothing (app_test: marked_frees stays 0). */
    fos_expect_marked_frees(1);
    T_EQ(ua_shutdown(fos_sleep), 0);
    T_EQ(fos_stats()->marked_frees, 4);
    fos_expect_marked_frees(0);
    T_EQ(fos_stats()->violations, 0);
    if (fos_stats()->violations) printf("    violation: %s\n", fos_stats()->violation);
    T_EQ(fos_stats()->pipes_open, 0);
    T_EQ(fos_stats()->xfers_open, 0);
}

static unsigned completions(void)
{
    struct ua_counters c;
    ua_counters(&c);
    return c.completions;
}

/* The log: the first snapshot right after ua_start at FRINDEX 1020 (frame
 * 516) says 8 siTD; the next one 4.1 s later 12 completions; then one every
 * 5.003 s (FRINDEX differences), always 4 queued and 32 siTD. */
static const unsigned logged[13] = { 12, 32, 52, 69, 89, 109, 128, 148, 168, 185, 205, 225, 244 };

/* Returns how many of the 13 logged snapshots the model gives exactly. */
static unsigned replay(int model)
{
    unsigned start, k, exact = 0;
    struct ua_counters c;
    start_at_frame(model, 510);
    start = fos_frame();
    T_EQ(fos_stats()->sitd_linked, 8);
    for (k = 0; k < 13; k++) {
        unsigned at = start + 4095 + (k * 50033u + 5) / 10, got;
        while (fos_frame() < at)
            fos_sleep();
        got = completions();
        if (got == logged[k]) exact++;
        t_checks++;
        if (got + 1 < logged[k] || got > logged[k] + 1)
            T_FAIL_("model %d snapshot %u: %u completions, the calculator logged %u", model, k, got, logged[k]);
        T_EQ(fos_stats()->sitd_linked, 32);
        ua_counters(&c);
        T_EQ(c.inflight, 4);
        T_EQ(c.errors, 0);
    }
    return exact;
}

static void test_reproduces_the_calculator_log(void)
{
    T_EQ(replay(FOS_HW_FOTG210), 13);
    /* 244 transfers of 8 packets, none finished in the frame it was linked for. */
    T_EQ(fos_stats()->ontime_frames, 0);
    T_TRUE(fos_stats()->late_frames >= 244 * 8);
    stop();
}

static void test_same_code_on_a_split_capable_controller(void)
{
    unsigned start, done;
    start_at_frame(FOS_HW_EHCI, 510);
    start = fos_frame();
    while (fos_frame() < start + 10000)
        fos_sleep();
    done = completions();
    T_TRUE(done >= 1245 && done <= 1250);       /* 10 s of 8 ms transfers */
    T_EQ(fos_stats()->late_frames, 0);
    T_TRUE(fos_stats()->starved_frames <= 4);
    stop();
}

/* The other readings of the log give the same numbers for this code: the
 * log alone cannot tell them from the start-split one. */
static void test_other_controller_models(void)
{
    T_EQ(replay(FOS_HW_SMASK), 13);
    stop();
    T_EQ(replay(FOS_HW_LATE), 13);
    stop();
    T_EQ(replay(FOS_HW_LEAD), 13);
    stop();
}

int main(void)
{
    corpus_fix();
    T_RUN(test_reproduces_the_calculator_log);
    T_RUN(test_same_code_on_a_split_capable_controller);
    T_RUN(test_other_controller_models);
    return T_DONE();
}
