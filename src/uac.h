/* USB Audio Class descriptors: parse a configuration descriptor set, pick a
 * playback (PCM OUT) alternate setting and sample rate, and decode every
 * descriptor as text for the log.
 *
 * Pure code: no allocation, no OS call, byte-wise reads only (the
 * calculator traps on unaligned word access), so it may run in the OS's USB
 * task as well as in the main program. Class 1 (UAC1) is supported; class 2
 * (UAC2, interface protocol 0x20) is recognised and decoded but never chosen.
 */
#ifndef UA_UAC_H
#define UA_UAC_H

#include <stdint.h>

#define UAC_MAX_FMT    12
#define UAC_MAX_RATES  8
#define UAC_MAX_UNITS  16
#define UAC_PACKET_MAX 1023     /* supported full-speed packet/storage limit */
/* The calculator OS's full-speed isochronous scheduler asks its controller
 * for one start-split per 188 bytes of a packet (T-count; OS 6.4.0.74 code
 * at 10a0de4c). On the CX II (FOTG210, no transaction translator) the run
 * of 2026-11-14 with 192-byte packets (48 kHz stereo 16-bit) finished every
 * packet exactly one pass of the 1024-entry frame list late: 32 ms of
 * audio per 1036 ms. A packet of at most this many bytes needs one
 * start-split, so such a rate is preferred (44.1 kHz stereo: 176/180). */
#define UAC_ONE_SPLIT  188

/* Endpoint sync kinds (bmAttributes bits 3:2). */
#define UAC_SYNC_NONE     0
#define UAC_SYNC_ASYNC    1
#define UAC_SYNC_ADAPTIVE 2
#define UAC_SYNC_SYNC     3

/* One AS alternate setting with an isochronous data endpoint. */
struct uac_fmt {
    uint8_t  ifnum, alt;
    uint8_t  uac_ver;           /* 1 or 2 */
    uint8_t  dir_in;            /* 1: IN (a microphone), 0: OUT (playback) */
    uint16_t format_tag;        /* UAC1 wFormatTag (1 PCM, 2 PCM8); UAC2: 1 if bmFormats has PCM */
    uint8_t  format_type;       /* 1 = type I */
    uint8_t  channels, subframe, bits;
    uint8_t  terminal_link;
    uint8_t  rate_continuous;   /* 1: [rate_min, rate_max] */
    uint8_t  nrates;
    uint32_t rate_min, rate_max;
    uint32_t rates[UAC_MAX_RATES];
    uint8_t  ep_addr, ep_attr, ep_interval, ep_refresh, ep_synch_addr;
    uint16_t ep_maxpkt;         /* bytes per (micro)frame, bits 10:0 */
    uint8_t  ep_sync;           /* UAC_SYNC_* */
    uint8_t  cs_ep_attr;        /* class-specific endpoint bmAttributes: bit 0 sampling
                                   frequency control, bit 1 pitch, bit 7 max packets only */
    uint8_t  have_cs_ep;
    uint8_t  fb_addr;           /* explicit feedback endpoint in the same alt, 0 = none */
};

struct uac_unit {
    uint8_t id, subtype, source; /* subtype: 2 IT, 3 OT, 4 mixer, 5 selector, 6 feature, 0x0A clock */
    uint16_t term_type;          /* terminals */
    uint8_t ctl_master, ctl_ch1, ctl_ch2;   /* feature unit (UAC1): bit 0 mute, bit 1 volume */
};

struct uac_info {
    uint8_t  audio;             /* an audio control or streaming interface was seen */
    uint8_t  ac_ifnum;          /* audio control interface number (0xFF none) */
    uint8_t  ac_ver;            /* 1, 2, 0 = no AC header */
    uint16_t bcd_adc;
    uint8_t  any_uac2;          /* some interface is class 2 (protocol 0x20 / bcdADC 2.00) */
    uint8_t  n_out, n_in;       /* streaming alts by direction (all classes) */
    uint8_t  nfmt;
    struct uac_fmt fmt[UAC_MAX_FMT];
    uint8_t  nunits;
    struct uac_unit unit[UAC_MAX_UNITS];
    uint8_t  truncated;         /* the walk stopped at a bad descriptor */
};

/* 0 on success (even with no audio in it), -1 if cfg isn't a configuration. */
int uac_parse(const uint8_t *cfg, unsigned len, struct uac_info *out);

/* The sample rate this program would use with f (48 kHz, 44.1 kHz, ... first
 * that the alt supports and whose packets fit wMaxPacketSize; one whose
 * packets fit UAC_ONE_SPLIT before any other), 0 = none. */
uint32_t uac_pick_rate(const struct uac_fmt *f);

/* Bytes in the largest packet at rate (ceil(rate / 1000) frames). */
unsigned uac_max_packet(const struct uac_fmt *f, uint32_t rate);

/* Index of the playback alt to use (UAC1, PCM type I, 1-8 channels, 1-4 byte
 * subframes, a usable rate), -1 = none. why (may be NULL) gets a reason. */
int uac_choose(const struct uac_info *u, uint32_t *rate, const char **why);

/* The feature unit on the path from fmt's input terminal to an output, and
 * the channel to address for mute/volume: 0 master, else 1 (each of 1..n).
 * NULL = none. */
const struct uac_unit *uac_feature_unit(const struct uac_info *u, const struct uac_fmt *f);

/* Text: one call per output line (no newline), hex dump then decoded fields.
 * Lines are at most UAC_LINE-1 characters. */
#define UAC_LINE 120
typedef void (*uac_emit)(void *ctx, const char *line);
void uac_dump(const uint8_t *cfg, unsigned len, uac_emit emit, void *ctx);
void uac_dump_device(const uint8_t *dd, unsigned len, uac_emit emit, void *ctx);
const char *uac_sync_name(uint8_t sync);

#endif
