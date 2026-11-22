/* USB audio playback through the TI-Nspire OS's own USB host stack (a
 * NetBSD/FreeBSD-style usbdi), registered through nspire-usbshim so the
 * driver survives the program's exit harmlessly.
 *
 * Contexts: ua_* functions are MAIN only. The OS calls match/attach/detach
 * and the transfer completions in its USB task ("USB"); those never make an
 * Ndless syscall (no malloc, no printf, no file I/O): they call only OS
 * functions resolved to addresses in ua_init() and pure code. They report
 * to MAIN through an event ring (ua_take_event).
 *
 * Streaming: NX transfers of NF one-millisecond packets each are queued;
 * each completion refills its transfer from the ring and queues it again.
 *
 * How the OS runs an isochronous pipe (read from the CX II OS 6.4.0.74 code,
 * NOTES.txt "2026-11-14: one lap late"): usbd_transfer only queues. The OS
 * links the queued packets into the controller's 1024-entry frame list, one
 * per frame, right after the last packet linked (or 4 frames from now when
 * that slot has passed), from its soft interrupt: when the last packet of
 * the FIRST queued transfer has gone out, or at the frame-list rollover.
 * Only the first queued transfer is ever looked at, so the completions come
 * in the order queued. A pipe has 58 packet descriptors: more than 58
 * packets queued just wait. The completion callback runs in that soft
 * interrupt, and what it queues is linked before the interrupt returns: the
 * stream is continuous while at least one packet is still ahead of the
 * controller then. The soft interrupt runs only while MAIN sleeps in the OS
 * (TCC_Sleep) or is inside a USB call. */
#ifndef UA_OS_H
#define UA_OS_H

#include <stdint.h>
#include "pcm.h"
#include "uac.h"

/* 6 x 8 = 48 packets queued: 40 ms are still ahead when a transfer comes
 * back, which is how long MAIN may stay away from the OS (a log write, a
 * screen redraw, a file read) before the device hears a gap; 48 of the
 * pipe's 58 descriptors. */
#ifndef UA_NX
#define UA_NX 6                 /* transfers in flight */
#endif
/* (2026-11-19, ua18) overridable as UA_NX is: gpSP builds 3 x 16 (its
 * Makefile's GPSP_UA_NF / GPSP_UA_NX), half the completions of 7 x 8 for the
 * same 48 descriptors. 16 at the most: a transfer's packet lengths are 32
 * bytes of its block (ua_os.c struct os_mem frlen). Everything else follows
 * UA_NF: the packet buffers, the counts, the fades (a step a packet), the
 * keep-alive (one transfer of silence), the resume's primer (one transfer)
 * and what it keeps (UA_NX * UA_NF ms). NOTES.txt "ua18". */
#ifndef UA_NF
#define UA_NF 8                 /* packets (USB frames, 1 ms) per transfer */
#endif
/* ua18: UA_START_ONE: ua_start queues the first transfer (silence) alone
 * and its completion the others (gpSP's build: ua_queue_ms then knows where
 * each is linked); else all at once, as before. */
#ifdef UA_START_ONE
#define UA_START_QUEUED 1
#else
#define UA_START_QUEUED UA_NX
#endif
#define UA_OS_SITDS 58          /* the OS's descriptors per full-speed isochronous pipe */
typedef char ua_queue_fits[UA_NX * UA_NF <= UA_OS_SITDS ? 1 : -1];
typedef char ua_nf_fits[UA_NF >= 1 && UA_NF <= 16 ? 1 : -1];
typedef char ua_nx_fits[UA_NX >= 2 ? 1 : -1];
#define UA_PKT_MAX UAC_PACKET_MAX

enum ua_state {
    UA_OFF = 0,                 /* not initialised / shut down */
    UA_WAITING,                 /* registered, no device yet */
    UA_SETUP,                   /* attach is configuring a device */
    UA_READY,                   /* pipe open, transfers allocated: ua_start() */
    UA_STREAMING,
    UA_FAILED,                  /* stopped at a step: ua_failure() */
    UA_GONE,                    /* the device was unplugged */
    UA_STOPPED,                 /* ua_stop() closed the stream */
};

/* Steps: what each event's status belongs to. */
enum ua_step {
    S_NONE = 0, S_RESOLVE, S_REGISTER, S_MATCH_DEV, S_MATCH_IF, S_ATTACH, S_IVARS, S_IFACE2DEV,
    S_CONFIG_DESC, S_CALIBRATE, S_GET_CFG9, S_GET_CFG, S_PARSE, S_CHOOSE, S_SET_IFACE,
    S_SET_IFACE_REQ, S_SET_RATE, S_GET_RATE, S_FU_MUTE, S_FU_VOL_RANGE, S_FU_VOL_SET,
    S_OPEN_PIPE, S_ALLOC_XFER, S_ISOC_FN, S_LAYOUT, S_TRANSFER, S_TRANSFER_CB, S_COMPLETE,
    S_NO_COMPLETION, S_DETACH, S_ABORT, S_CLOSE, S_STOP,
    S_EP_SIZE,                  /* a: wMaxPacketSize in the OS's descriptor copy, b: now, c: our largest packet;
                                   status 4: the endpoint was not found there, nothing written */
    S_COUNT
};
const char *ua_step_name(unsigned step);

/* Event codes. */
enum {
    EV_STEP = 1,                /* step done: status = usbd_status (0 ok), a/b/c per step */
    EV_DESC,                    /* descriptors snapshotted: ua_desc() */
    EV_FAIL,                    /* streaming/setup stopped at step with status */
    EV_FIRST_ERR,               /* first transfer error: step, status, a = slot */
};
struct ua_event {
    uint16_t code, step;
    uint32_t status, a, b, c;
};
int ua_take_event(struct ua_event *e);      /* 1 = got one */
uint32_t ua_events_lost(void);

/* What attach saw (stable once EV_DESC was posted, until the next device). */
struct ua_desc {
    uint16_t vid, pid;
    uint8_t  ifnum;             /* the interface the OS offered */
    uint8_t  dev[18];
    uint8_t  have_dev;
    uint16_t cfg_os_len, cfg_dev_len;       /* the OS's copy, the one fetched with GET_DESCRIPTOR */
    uint8_t  cfg_os[1024], cfg_dev[1024];
    struct uac_info info;
    int      chosen;            /* index in info.fmt, -1 none */
    const char *why;            /* why nothing was chosen */
    uint32_t rate;              /* the rate we asked for */
    uint32_t rate_dev;          /* GET_CUR read back, 0 = unknown */
    int8_t   fu_id;             /* feature unit used, -1 none */
    int16_t  vol_min, vol_max, vol_set;     /* 1/256 dB */
};
const struct ua_desc *ua_desc(void);

struct ua_counters {
    uint32_t completions, frames_ok, errors, cancelled;
    uint32_t underrun_pkts, underrun_frames;    /* the ring was empty: silence sent */
    uint32_t starved;           /* a completion found no other transfer queued: a gap */
    uint32_t min_queued_ms;     /* least audio still queued at a completion since the
                                   last ua_counters_reset_min(), ~0 = no completion */
    uint32_t requeue_fail;
    uint32_t inflight;
    uint32_t queued_frames;     /* packets handed to the OS */
    uint32_t late_frames;       /* packets of transfers that came back UA_LATE_MS late or more */
    uint32_t keepalives;        /* transfers of silence queued while idle (ua_set_idle) */
};
void ua_counters(struct ua_counters *c);
void ua_counters_reset_min(void);

/* ---- timing, for the log ----
 * Times are the host controller's frame counter (FRINDEX >> 3: one per
 * millisecond, 0..2047), read in the completion callback and when a transfer
 * is queued: one register read, no OS call. A transfer queued behind `ahead`
 * milliseconds of audio should come back ahead + UA_NF ms later; what it
 * takes longer is how long its last packet had been out before the callback
 * queued its replacement ("wait"). On the 2026-11-14 run that was 1024 ms:
 * one lap of the frame list. */
#define UA_LATE_MS 100          /* a transfer that waited longer counts as late */
#define UA_FRAME_WRAP 2048u     /* (so a time above 2047 ms shows as less) */
/* MAIN, any time before ua_start: the address of the controller's FRINDEX
 * register (read only; 0 = no timing). */
void ua_set_frame_clock(uint32_t frindex_addr);

/* ua18 (MAIN, interrupts held or not): the queue as the frame clock sees
 * it, for a caller that serves the OS's completions itself (gpSP's interrupt
 * windows). *ahead: ms until the last queued packet has gone out (the wire
 * runs dry then unless a completion queued more); *head: ms until the first
 * queued transfer's last packet has gone out (<= 0: its completion waits for
 * the OS). From each transfer's frame as the OS links it (right after the
 * last linked packet, or 4 frames on when that has passed): an estimate that
 * errs short. 1 known; 0 not (no frame clock, not streaming, idle, nothing
 * queued): the caller keeps its fixed limit, (UA_NX - 1) x UA_NF ms. */
int ua_queue_ms(int *ahead, int *head);
/* ua18: the caller ran the OS's completions (an interrupt window, a sleep)
 * and *head still says < 0 (the head transfer's did not come): what MAIN
 * queued behind a running pipe went out later than taken; the estimate
 * moves on to *head 0, never past what that can account for. Returns the
 * ms it moved: 0 says the estimate stands (the completions did not run). */
int ua_queue_not_done(void);
#define UA_HAS_QUEUE_MS 1

struct ua_second {              /* since the last ua_take_second() */
    uint32_t queued_ms;         /* packets handed to the OS */
    uint32_t done_ms;           /* packets of transfers that came back without an error */
    uint32_t late_ms;           /* ... of those, in transfers that came back late */
    uint32_t silence_ms;        /* packets of silence sent because the ring was empty */
    uint32_t failed_ms;         /* packets of transfers that came back with an error */
    uint32_t completions;
    uint32_t turn_min, turn_max, turn_sum;      /* queued -> back, ms */
    uint32_t wait_min, wait_max, wait_sum;      /* last packet out -> back and replaced, ms */
    uint32_t apart_max;         /* longest time between two completions, ms */
    uint32_t ahead_min;         /* least audio still queued when one came back, ms */
    uint32_t restarts;          /* came back with nothing else queued: the OS starts over 4 frames on */
    uint8_t  timed;             /* 0: no frame clock, the times are not there */
};
void ua_take_second(struct ua_second *s);

/* The first transfers back after ua_start, in order. */
#define UA_FIRST 12
struct ua_first { uint16_t turn, ahead; };
unsigned ua_first(struct ua_first *out, unsigned max);

/* How the OS's functions were found, for the log. */
struct ua_osinfo {
    uint32_t addr[20];          /* in ua_fn_name() order, 0 = absent */
    uint8_t  isoc_how;          /* 0 none, 1 syscall table, 2 derived from usbd_setup_xfer */
    uint32_t isoc_words[4];     /* first words at the isoc function (derived or not) */
    uint32_t word_before;       /* the word just before it */
    uint8_t  ctl_conv;          /* 0 unknown, 1 raw pointer, 2 TI descriptor {buf,0,0} */
    uint8_t  buf_conv;          /* the form used for isochronous data */
    int8_t   lay_buf, lay_len, lay_frlen, lay_nframes; /* word offsets in the xfer, -1 unknown */
    uint8_t  lay_method;        /* 3: verified TI eight-argument setup, no opaque writes */
    /* 2026-11-16 (appended): TI's mark on aborted isochronous transfers
     * (ua_os.c mark_off): blocks it was taken off before they were freed;
     * blocks whose byte read neither as before the abort nor as the mark
     * (nothing written); stops that aborted nothing because every queued
     * transfer played out. */
    uint32_t marks_off, marks_odd, drained;
    /* 2026-11-18 (appended): a detach in the window between MAIN's setup of
     * a transfer and its usbd_transfer call (ua_os.c submit_main) */
    uint32_t late_detach;
};
#define UA_HAS_MARKS 1
const struct ua_osinfo *ua_osinfo(void);
const char *ua_fn_name(unsigned i);         /* NULL past the end */
#define UA_NFN 18

/* Exposed for tests: the derivation of usbd_setup_isoc_xfer's address
 * where the syscall table lacks it. setup/status: usbd_setup_xfer and
 * usbd_get_xfer_status (0x120 apart in every table); off: 0xC4 or 0xE0;
 * w0: the word at setup + off, wprev: the word before it. Returns the
 * candidate or 0. */
uint32_t ua_isoc_derive(uint32_t setup, uint32_t status, uint32_t off, uint32_t w0, uint32_t wprev);

int ua_isoc_signature(const uint32_t *words, unsigned count);

int  ua_init(void);             /* 0 ok, <0: ua_init_text() */
const char *ua_init_text(void);
int  ua_state(void);
int  ua_attached(void);          /* includes a claimed device whose setup failed */
int  ua_owns_usb(void);          /* attached, pipe retained, transfer or callback in flight */
int  ua_busy(void);              /* hub: a callback of this driver is inside the OS now */
#define UA_HAS_BUSY 1
const char *ua_state_name(int st);
void ua_failure(unsigned *step, unsigned *status);  /* where UA_FAILED stopped */
/* Starts streaming from r (the ring the main loop fills). 0 ok, else the
 * usbd_status of the failing call (also posted as EV_FAIL). */
int  ua_start(struct ring *r);
void ua_set_volume(unsigned vol);           /* 0..256 software volume */
void ua_set_pause(int paused);
/* Idle (MAIN, any state). 1: queue no new isochronous transfer; those
 * queued play out (the last one fades out), then nothing is on the bus and
 * no completion comes. The pipe stays open and the device claimed: no
 * abort, no close (ua_owns_usb() stays 1).
 * Call ua_set_idle(1) again from the main loop while idle (every pass is
 * fine): about once a second it queues one transfer of silence (a
 * keep-alive: one completion a second instead of 125), so the OS's last
 * linked frame for the pipe never gets old; without these calls a resume
 * after more than ~4 s of idle may start up to about a second (in the
 * simulator up to 2 s) late, as silence. The calls also acknowledge at
 * once a discard request (ring_request_clear) made while idle or draining.
 * 0: resume: one transfer of silence goes in (the OS links it alone), and
 * its completion queues the rest from the ring; what the producer put in
 * the ring before the resume is dropped but its newest UA_NX * UA_NF ms (a
 * full queue: its priming plays without an underrun), the first audio
 * after it fades in. In the simulator the first packet is on the wire 4 ms after the
 * call, the first audible one 16 ms (tests/app_test.c test_idle_resume).
 * Calling it with 0 again while streaming is harmless; it also re-primes a
 * stream that lost all its transfers.
 * Known residual window (ua_os.c submit_main): a detach that lands inside
 * MAIN's usbd_transfer call of a resume or keep-alive leaves that block,
 * its pipe and its six transfers quarantined for good (a leak, never
 * reused); the transfer is refused if TI's pipe is still marked aborting.
 * A detach anywhere else in MAIN's calls is handled (ua_osinfo late_detach).
 * A detach clears idle: after a re-attach (a hub's port power-cycled) the
 * card streams until the caller asks for idle again, so re-assert idle
 * after every attach. Idle asked after the detach (before or during the
 * next attach, or before ua_start) holds: ua_start then queues nothing. */
void ua_set_idle(int idle);
/* 1 while idle and fully drained: streaming, nothing queued with the OS, no
 * completion running. A hub's liveness check that counts completions must
 * not take a card for dead while this says 1 (its completions stop). */
int  ua_idle(void);
#define UA_HAS_IDLE 1
/* Stop streaming and close the pipe; sleep_tick lets the OS run while
 * waiting for cancelled transfers (MAIN). If a callback is already active,
 * its storage stays alive; call again after yielding to finish cleanup. */
void ua_stop(void (*sleep_tick)(void));
/* Stop + retire. Returns 1 while callbacks or OS resources remain: yield
 * and retry, without unloading the program/callback code. 0 = retired. */
int ua_shutdown(void (*sleep_tick)(void));
const char *ua_status_name(unsigned status); /* our table of usbd_status names */
const char *ua_errstr(unsigned status);      /* the OS's usbd_errstr (MAIN), or ours */
struct dev_fmt;
void ua_dev_fmt(struct dev_fmt *f);          /* the device's sample format */
uint32_t ua_seen_probes(void);               /* match calls (any level) since init */

#ifdef UA_HOST_TEST
int       ua_fake_has(unsigned nr);
uintptr_t ua_fake_addr(unsigned nr);
void      ua_fake_before_requeue(void);
uint32_t  ua_fake_frame(void);              /* FRINDEX >> 3 of the simulated controller */
void      ua_fake_main_irq(void);           /* an IRQ point inside a ua_* call */
void      ua_fake_park(void);               /* a completion parking its transfer */
void      ua_fake_irq_held(void);           /* MAIN's held interrupts, just before usbd_transfer */
#endif

#endif
