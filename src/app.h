/* The program's main loop, shared by the calculator build (main.c) and the
 * PC test against the simulated OS USB stack (tests/app_test.c). MAIN only. */
#ifndef UA_APP_H
#define UA_APP_H

#include <stdint.h>
#include "wav.h"

#define A_QUIT  0x01
#define A_PAUSE 0x02
#define A_NEXT  0x04
#define A_VOLUP 0x08
#define A_VOLDN 0x10
#define A_PREV 0x20
#define A_REPEAT 0x40
#define A_RESTART 0x80

struct music_platform;

enum app_port {
    PORT_UNKNOWN, PORT_EMPTY, PORT_PC, PORT_ADAPTER, PORT_DEVICE, PORT_STOPPED
};

struct app_platform {
    void (*log)(const char *line);          /* the log file and the screen */
    void (*flush)(void);                    /* write the log out now */
    void (*sleep_tick)(void);               /* give the OS one tick */
    uint32_t (*seconds)(void);              /* a seconds clock */
    unsigned (*keys)(void);                 /* A_* pressed since the last call */
    /* Direct OS USB calls may wait for interrupts. The calculator unmasks
     * CPU IRQs around them while keeping the keypad line private. */
    uint32_t (*usb_enter)(void);
    void (*usb_leave)(uint32_t saved);
    int (*port_connected)(void);            /* host role + device on the port: 1, 0, -1 unknown */
    int (*port_state)(void);                /* gpSP-style ID/session/line observation */
    void (*usb_regs)(char *buf, unsigned size);  /* host controller registers, read only */
    /* The isochronous descriptors in the controller's frame list, read
     * only: how many, where from the current frame, sent or not. */
    void (*usb_sched)(char *buf, unsigned size);
    /* The address of the controller's FRINDEX register, 0 unknown; asked
     * when a device is there (the controller is clocked and in host mode). */
    uint32_t (*usb_frindex)(void);
    unsigned ticks_per_second;              /* sleep_tick calls per second (about) */
    wav_read_fn wav_read;                   /* NULL: no WAV */
    void *wav_ctx;
    uint32_t wav_size;
    uint32_t max_seconds;                   /* 0 = no limit (tests set one) */
    uint32_t unplug_wait_s;                 /* seconds without an offer before "replug" (4) */
    const struct music_platform *music;     /* NULL: diagnostic tune + WAV test */
};

/* Returns 0 after a normal run, 1 if it stopped at a failure. */
int app_run(const struct app_platform *pf);

/* One second of streaming as a log line (MAIN): t seconds after the start;
 * takes the driver's ua_second. Returns 1 if a transfer came back late.
 *   queued/played/late/dropped: milliseconds of audio;
 *   "back after a-b ms": queued -> completed (the queue's length when all
 *   is well); "a-b ms after their last packet": how long a finished
 *   transfer waited before its replacement was queued. */
struct ua_second;
int app_second_text(char *buf, unsigned size, unsigned t, struct ua_second *s);
/* The lines that explain late transfers (once per stream). */
#define APP_LATE_TEXT \
    "LATE: transfers come back about 1024 ms (one lap of the controller's frame list) after they were queued.\n" \
    "MEANING: the controller did not send those packets in the frames the OS put them in, as on 2026-11-14;\n" \
    "the siTD line shows how far away the waiting ones are, with which T-count and how many bytes left."

#endif
