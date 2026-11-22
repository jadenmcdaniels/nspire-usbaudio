/* A simulated TI-Nspire OS USB host stack for src/ua_os.c on a PC, modelled
 * on nspire-usbpad's tests/os_fake (NetBSD probe order, TI buffer
 * descriptors, usage checks) and the captured TI eight-argument isoc ABI.
 *
 * Isochronous pipes follow the scheduler read out of the CX II OS
 * 6.4.0.74 (a capture of the OS code 109c0000-10a40000; FOTG210 driver 10a0d9xx-10a0f5xx):
 *   - usbd_transfer only queues; the pipe "runs" from its first transfer
 *     and frames are linked into the 1024-entry periodic frame list by one
 *     function (10a0db0c), called for the first transfer and afterwards
 *     from the soft interrupt only;
 *   - a frame goes right after the last one linked, or 4 frames after the
 *     current one when that slot has passed; never 1020 frames or more
 *     ahead; each frame takes one of the pipe's 58 descriptors;
 *   - the last frame of a transfer asks for an interrupt; the soft
 *     interrupt (that interrupt or the frame-list rollover) reaps finished
 *     frames of the FIRST queued transfer only, completes it (callback),
 *     links what is queued and goes on with the next transfer;
 *   - a packet takes one start-split per 188 bytes of its length
 *     (T-count), in the micro-frames of an S-mask that the OS picks by the
 *     endpoint descriptor's wMaxPacketSize / 188 instead.
 * What the controller then does is NOT in the capture. Five models, all of
 * which give the hardware log for the earlier driver
 * (192-byte packets, wMaxPacketSize 200: 32 ms of audio per 1036 ms; see
 * tests/legacy_test.c):
 *   FOS_HW_FOTG210  one start-split of a descriptor per pass of the frame
 *                   list, finished when its bytes are out (T-count): the
 *                   default; a packet of at most 188 bytes takes one pass;
 *   FOS_HW_SMASK    the same, but a descriptor is finished only after as
 *                   many passes as its S-mask has micro-frames: one pass
 *                   needs wMaxPacketSize below 188 as well;
 *   FOS_HW_LATE     every descriptor is sent one pass late;
 *   FOS_HW_LEAD     descriptors linked fewer than 12 frames ahead wait a
 *                   pass (the OS links 4 ahead).
 * The driver can do nothing about the last two. FOS_HW_EHCI sends all
 * start-splits in the descriptor's frame, as a controller with a
 * transaction translator does: the same driver code is continuous there.
 *
 * The "USB task" runs inline in fos_sleep() (one 10 ms OS tick); fos_stall()
 * is the main program busy with interrupts masked. */
#ifndef FAKE_OS_H
#define FAKE_OS_H

#include <stdint.h>

#define FOS_NO_ISOC_OPEN  0x01  /* usbd_open_pipe on an isochronous endpoint: USBD_INVAL */
#define FOS_NO_ISOC_XFER  0x02  /* usbd_transfer on an isochronous pipe: USBD_IOERROR */
#define FOS_ISOC_ERR      0x04  /* isochronous transfers complete with USBD_IOERROR */
#define FOS_NEVER_DONE    0x08  /* isochronous transfers never complete */
#define FOS_CDESC_SHORT   0x10  /* usbd_get_config_descriptor gives only 9 bytes */
#define FOS_ABORT_DELAYED 0x20 /* cancellation callbacks arrive after the first stop deadline */
#define FOS_ABORT_ERROR   0x40 /* first abort fails; later retry can cancel */
#define FOS_CLOSE_ERROR   0x80 /* first close fails; the pipe must be retained */

enum { FOS_HW_FOTG210, FOS_HW_EHCI, FOS_HW_LATE, FOS_HW_LEAD, FOS_HW_SMASK };

#define FOS_FRAME_LIST 1024     /* periodic frame list entries */
#define FOS_SITD_POOL  58       /* descriptors of a full-speed isochronous pipe */
#define FOS_SPLIT      188      /* bytes per start-split */

struct fos_spec {
    uint16_t vid, pid;
    const uint8_t *cfg;
    unsigned cfg_len;
    unsigned flags;
};

void fos_init(void);
void fos_fini(void);
int  fos_plug(const struct fos_spec *spec);     /* offered at the next fos_sleep */
void fos_unplug(void);                          /* detach at the next fos_sleep */
void fos_sleep(void);                           /* one OS tick: 10 ms of USB time */
/* MAIN asleep in the OS for ms milliseconds (a TCC_Sleep that comes back at
 * the OS's next tick, wherever that falls: 0 to a tick): what fos_sleep does
 * (fos_sleep is fos_run(10)), with ms frames of the bus. For a caller that
 * keeps its own time and runs the bus masked (fos_stall) in between. */
void fos_run(unsigned ms);
/* MAIN takes the CPU's interrupts for a moment (gpSP's IRQ window): the
 * controller's interrupt is serviced, the completions run (the CX II's
 * R11u run: they do in such a window); no attach, detach or plug: those
 * are the USB task's (fos_run). */
void fos_irq_window(void);
/* Packets (ms of audio) handed to the OS and not yet on the wire: the
 * descriptors still to send and the frames of queued transfers not linked. */
unsigned fos_frames_ahead(void);
unsigned fos_wire_left(void);                   /* ua18: ms until the last packet handed to the OS is out */
void fos_stall(unsigned ms);                    /* MAIN busy, interrupts masked: the bus runs, the USB task does not */
void fos_set_hw(int model);                     /* FOS_HW_*; fos_init selects FOS_HW_FOTG210 */
/* The OS's copy of the configuration descriptor (what
 * usbd_get_config_descriptor returns, writable like the real one): the
 * wMaxPacketSize it now holds for endpoint ep. */
unsigned fos_os_maxpkt(unsigned ep);
/* 1: pipes take wMaxPacketSize from somewhere the driver cannot reach
 * (should the real OS not read the copy it hands out). */
void fos_ep_elsewhere(int on);
unsigned fos_frame(void);                       /* milliseconds (USB frames) since fos_init */
void fos_remove_syscall(unsigned nr);
void fos_set_conv(int conv);                    /* control requests only: 1 raw, 2 TI (default); transfers always TI */
void fos_set_irq_check(int (*enabled)(void));    /* direct MAIN USB calls must allow IRQs */
void fos_setup_hook(void (*hook)(void));         /* simulate MAIN while attach is blocked */
void fos_control_hook(void (*hook)(void));       /* MAIN while a control request is blocked */
void fos_transfer_hook(void (*hook)(void));      /* MAIN while a completion requeues */
void fos_requeue_hook(void (*hook)(void));       /* MAIN after completion cleared busy */
/* MAIN's interrupts on for a moment inside a ua_* call (an IRQ point, where
 * the USB task can run: a detach, a completion). */
void fos_main_irq_hook(void (*hook)(void));
/* 1 (fos_init): the soft interrupt runs the scheduler for every isochronous
 * pipe, an empty one too, which forgets a last linked frame that has
 * passed. 0: only for a pipe with something queued (nothing shows TI's
 * 10a0f144 does more): an idle pipe's last linked frame gets as old as the
 * idle, and the 16-bit micro-frame comparison wraps every 8.192 s. */
void fos_flr_resched(int on);
/* MAIN inside a completion that is parking its transfer (idle), between
 * marking it parked and looking at idle again. */
void fos_park_hook(void (*hook)(void));
/* MAIN's IRQ point right after the interrupts it held while it set up a
 * transfer come back, just before usbd_transfer (the window a detach can
 * still take). */
void fos_held_irq_hook(void (*hook)(void));
/* MAIN inside usbd_transfer (its interrupts on): the USB task may run. */
void fos_main_transfer_hook(void (*hook)(void));
/* 1: a pipe or xfer still open after a detach is kept (counted), not a
 * violation: the residual window the test drives. fos_init clears it. */
void fos_expect_kept_at_detach(int yes);
void fos_fail_transfers(unsigned n);             /* the next n usbd_transfer calls: USBD_IOERROR */
/* Between a nonzero match and its attach (another offer, a shutdown begun):
 * an attach that then returns an error is a violation, as on TI's OS (after
 * one the OS offers no further device until the adapter is pulled). */
void fos_match_hook(void (*hook)(void));
/* 1: the device takes no packet and the OS sees no detach (gpSP's R11u log,
 * seconds 130-135: the sound card pulled behind a kept adapter, no
 * completion for 6 s), 0: it takes them again. fos_init clears it. */
void fos_stall_device(int on);
int  fos_offer_other(int force_attach);         /* distinct offer, expected to be refused if resources live */
void fos_late_callback(void);                   /* after retirement: must hit the resident stub */
int  fos_port_connected(void);
/* The periodic schedule as the calculator's log line shows it (MAIN). */
void fos_sched_text(char *buf, unsigned size);
uint32_t ua_fake_frame(void);                   /* FRINDEX >> 3, for ua_os.c */

struct fos_stats {
    unsigned probes_dev, probes_iface, attach_ok, attach_fail, detaches;
    unsigned ctl_requests, ctl_from_main;
    unsigned alt;                               /* current alt of the streaming interface */
    uint32_t rate_set;                          /* SET_CUR sampling frequency */
    int      muted;                             /* last mute value (-1 never set) */
    int      vol;                               /* last volume (1/256 dB), or 0x7FFF never */
    unsigned isoc_submitted, isoc_done, isoc_cancelled;
    unsigned starved_frames;                    /* frames with no packet once the second transfer plays */
    unsigned wire_frames;                       /* frames in which the device got a packet */
    unsigned ontime_frames, late_frames;        /* descriptors finished in their pass / a pass or more late */
    unsigned fresh_starts;                      /* the scheduler restarted 4 frames from now */
    unsigned softintrs;
    unsigned sitd_linked, sitd_max;             /* descriptors in the frame list now / at most */
    unsigned pipes_open, xfers_open;
    unsigned pipes_kept, xfers_kept;
    unsigned abort_calls, close_calls, retire_calls;
    unsigned dead_transfers;    /* usbd_transfer on a pipe whose device's detach had returned */
    unsigned marked_frees;      /* xfers freed with TI's abort mark still set (2026-11-16) */
    unsigned violations;
    char     violation[200];
};
const struct fos_stats *fos_stats(void);
/* 2026-11-16: an xfer freed with TI's abort mark on is a violation, unless
 * the test says the driver under test is one that did that (legacy_test). */
void fos_expect_marked_frees(int yes);

/* What the device received: every packet's size and the bytes, in the order
 * they went over the bus. */
#define FOS_CAP_PKTS  20000
#define FOS_CAP_BYTES (4u << 20)
unsigned fos_cap_pkts(const uint16_t **sizes);
unsigned fos_cap_bytes(const uint8_t **bytes);

#endif
