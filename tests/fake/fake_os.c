#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_os.h"
#include "ua_os.h"

#define USBSHIM_TEST 1
#include "usbshim.h"

/* ---- objects ---- */
struct uaa { int port, configno, ifaceno, vendor, product, release; void *device, *iface; int extra[4]; };
struct fdev;
struct fiface { struct fdev *dev; uint8_t ifnum, alt; };
struct fself { struct uaa uaa; int attached; };
struct fxfer;
struct fpipe;
/* One frame of an isochronous transfer in the periodic frame list (the
 * OS's split-isochronous descriptor). */
struct fsitd {
    struct fsitd *next;             /* older descriptors linked for the same frame */
    struct fxfer *x;
    struct fpipe *p;
    unsigned off, bytes, left;      /* its packet in the transfer's data; bytes still to send */
    unsigned tcount;                /* start-splits still to send (the OS: ceil(bytes / 188)) */
    unsigned splits, visits;        /* micro-frames in its S-mask (wMaxPacketSize / 188 + 1); passes that sent or counted */
    unsigned slot, linked_at, lead; /* frame list index; frame clock and frames ahead when linked */
    uint8_t used, active, ioc, err, waited;
};
#define FOS_MAXF 64
struct fpipe {
    struct fdev *dev;
    uint8_t ep, type;
    uint16_t maxpkt;
    struct fxfer *q[16];
    unsigned qn;
    unsigned dev_flags;
    int open, running, aborting, detached;
    unsigned cancel_ticks;
    unsigned gap;                       /* frames with no packet since the last one went */
    /* isochronous: the scheduler's state (microframes, 16 bits, like the OS) */
    struct fsitd pool[FOS_SITD_POOL];
    unsigned nused;
    int have_last;
    uint16_t last, last_ioc;
    unsigned submitted;
    int primed;
};
/* Opaque host object: the driver must use accessors, never assume its layout. */
struct fxfer {
    uintptr_t pipe, priv, buffer, length, actlen, flags, timeout, status, callback, done;
    uintptr_t request[2];
    uintptr_t frlengths, nframes;
    uintptr_t pad[40];
    struct fdev *dev;
    unsigned dev_flags;
    int queued, magic, detached;
    unsigned sched, reaped, seq, err;   /* frames linked / reaped; order on the pipe */
    struct fsitd *d[FOS_MAXF];
    uint8_t *data;                      /* the OS copies the packets when the transfer is queued */
    uint8_t mark;                       /* TI's byte 84: bit 0 "being aborted", set by an isochronous
                                           abort and cleared only by the next isochronous submission */
};
#define XMAGIC 0x5846
struct fdev {
    struct fos_spec spec;
    uint8_t cdesc[1024];
    uint8_t ddesc[18];
    struct fiface ifs[8];
    struct fself selfs[8];
    unsigned nif;
    struct fself devself;
    int present;
    uint32_t rate;
};

static struct fdev *dev;
static int pending_plug, pending_unplug;
static usbshim_fn drv[3];
static usbshim_cb completion_fn, last_callback;
static int registered, retired;
static int conv = 2;
static int in_usb;
static int (*irq_check)(void);
static void (*setup_hook)(void);
static void (*control_hook)(void);
static void (*transfer_hook)(void);
static void (*requeue_hook)(void);
static void (*main_irq_hook)(void);
static void (*park_hook)(void);
static void (*held_irq_hook)(void);
static void (*main_transfer_hook)(void);
static int kept_expected;
#define FOS_KEPT_OK 0x10000   /* (pipe/xfer: kept across a detach, as the test expects) */
static unsigned fail_xfers;
static int flr_resched = 1;
static void (*match_hook)(void);
static int stalled;                         /* fos_stall_device: the device takes no packet */
static unsigned removed[8], nremoved;
static struct fos_stats st;
static struct fpipe *pipes[8];
static struct fxfer *xfers[64];
static uint16_t *cap_sizes;
static uint8_t *cap_bytes;
static unsigned cap_np, cap_nb;
/* the host controller */
static unsigned clock_ms;                   /* frames since fos_init; FRINDEX = clock_ms * 8 (14 bits) */
static uint16_t uf_base;                    /* the OS adds 8192 microframes at each rollover interrupt */
static int irq_flr, irq_usbint;             /* raised by the controller, taken when the USB task runs */
static struct fsitd *flist[FOS_FRAME_LIST];
static int hw_model, ep_elsewhere;

static void violate(const char *what)
{
    st.violations++;
    if (st.violations == 1)
        snprintf(st.violation, sizeof st.violation, "%s", what);
}

const struct fos_stats *fos_stats(void) { return &st; }
unsigned fos_cap_pkts(const uint16_t **s) { *s = cap_sizes; return cap_np; }
unsigned fos_cap_bytes(const uint8_t **b) { *b = cap_bytes; return cap_nb; }
void fos_set_conv(int c) { conv = c; }
void fos_set_hw(int model) { hw_model = model; }
void fos_ep_elsewhere(int on) { ep_elsewhere = on; }
unsigned fos_frame(void) { return clock_ms; }
uint32_t ua_fake_frame(void) { return clock_ms & 2047u; }
void fos_set_irq_check(int (*enabled)(void)) { irq_check = enabled; }
void fos_setup_hook(void (*hook)(void)) { setup_hook = hook; }
void fos_control_hook(void (*hook)(void)) { control_hook = hook; }
void fos_transfer_hook(void (*hook)(void)) { transfer_hook = hook; }
void fos_requeue_hook(void (*hook)(void)) { requeue_hook = hook; }
void fos_match_hook(void (*hook)(void)) { match_hook = hook; }
void fos_stall_device(int on) { stalled = on; }
void ua_fake_before_requeue(void) { if (requeue_hook) requeue_hook(); }
void fos_main_irq_hook(void (*hook)(void)) { main_irq_hook = hook; }
void ua_fake_main_irq(void) { if (main_irq_hook) main_irq_hook(); }
void fos_flr_resched(int on) { flr_resched = on; }
void fos_park_hook(void (*hook)(void)) { park_hook = hook; }
void ua_fake_park(void) { if (park_hook) park_hook(); }
void fos_fail_transfers(unsigned n) { fail_xfers = n; }
void fos_held_irq_hook(void (*hook)(void)) { held_irq_hook = hook; }
void ua_fake_irq_held(void) { if (held_irq_hook) held_irq_hook(); }
void fos_main_transfer_hook(void (*hook)(void)) { main_transfer_hook = hook; }
void fos_expect_kept_at_detach(int yes) { kept_expected = yes; }
void fos_remove_syscall(unsigned nr) { if (nremoved < 8) removed[nremoved++] = nr; }
int fos_port_connected(void) { return dev && dev->present; }

/* ---- shim stand-in ---- */
struct usbshim { int dummy; };
static struct usbshim the_shim;
usbshim_t *usbshim_register(const char *name, usbshim_fn m, usbshim_fn a, usbshim_fn d, unsigned sz)
{
    (void) name; (void) sz;
    drv[0] = m; drv[1] = a; drv[2] = d;
    registered = 1;
    retired = 0;
    return &the_shim;
}
void usbshim_retire(usbshim_t *s)
{
    (void) s;
    st.retire_calls++;
    retired = 1;
}
static void completion_stub(void *x, void *priv, unsigned status)
{
    if (!retired && completion_fn) completion_fn(x, priv, status);
}
usbshim_cb usbshim_callback(usbshim_t *s, usbshim_cb fn)
{
    (void)s;
    completion_fn = fn;
    return completion_stub;
}
void fos_late_callback(void)
{
    if (last_callback) last_callback(NULL, NULL, 19);
}

/* ---- descriptor helpers ---- */
static const uint8_t *dev_cdesc(const struct fdev *d)
{
    return d->spec.cfg && d->spec.cfg_len <= sizeof d->cdesc ? d->cdesc : d->spec.cfg;
}

static const uint8_t *find_iface(const struct fdev *d, unsigned ifnum, unsigned alt, unsigned *rest)
{
    const uint8_t *cfg = dev_cdesc(d);
    unsigned off = 0;
    while (off + 2 <= d->spec.cfg_len && cfg[off] >= 2) {
        const uint8_t *p = cfg + off;
        if (p[1] == 4 && p[2] == ifnum && p[3] == alt) {
            *rest = d->spec.cfg_len - off;
            return p;
        }
        off += p[0];
    }
    return NULL;
}

/* wMaxPacketSize of the first endpoint descriptor with this address in cfg. */
static unsigned cfg_maxpkt(const uint8_t *cfg, unsigned len, unsigned ep)
{
    unsigned off = 0;
    while (cfg && off + 2 <= len && cfg[off] >= 2) {
        const uint8_t *p = cfg + off;
        if (p[1] == 5 && p[0] >= 7 && p[2] == ep)
            return (unsigned) (p[4] | p[5] << 8) & 0x7FFu;
        off += p[0];
    }
    return 0;
}

unsigned fos_os_maxpkt(unsigned ep)
{
    return dev ? cfg_maxpkt(dev_cdesc(dev), dev->spec.cfg_len, ep) : 0;
}

static const uint8_t *find_ep(const struct fdev *d, unsigned ifnum, unsigned alt, unsigned ep)
{
    unsigned rest, off;
    const uint8_t *p = find_iface(d, ifnum, alt, &rest);
    if (!p)
        return NULL;
    for (off = p[0]; off + 2 <= rest && p[off] >= 2; off += p[off]) {
        if (p[off + 1] == 4)
            break;
        if (p[off + 1] == 5 && p[off + 2] == ep)
            return p + off;
    }
    return NULL;
}

/* ---- the OS's functions ---- */
static void *f_ivars(void *self) { return &((struct fself *) self)->uaa; }
static const uint8_t *f_iface_desc(void *iface)
{
    struct fiface *i = iface;
    unsigned rest;
    return find_iface(i->dev, i->ifnum, i->alt, &rest);
}
static uint8_t short_cfg[9];
static const uint8_t *f_config_desc(void *d)
{
    (void) d;
    if (!dev) return NULL;
    if (dev->spec.flags & FOS_CDESC_SHORT) {
        memcpy(short_cfg, dev->spec.cfg, 9);
        short_cfg[2] = 9; short_cfg[3] = 0;
        return short_cfg;
    }
    return dev_cdesc(dev);
}
static const uint8_t *f_dev_desc(void *d) { (void) d; return dev ? dev->ddesc : NULL; }
static unsigned f_iface2dev(void *iface, void **d) { *d = ((struct fiface *) iface)->dev; return 0; }

static uint8_t *data_ptr(void *data)
{
    if (!data) return NULL;
    return conv == 2 ? ((struct { uint8_t *buf; int z1, z2; } *) data)->buf : (uint8_t *) data;
}

static unsigned f_do_request(void *d, void *req, void *data)
{
    const uint8_t *r = req;
    uint8_t *b = data_ptr(data);
    unsigned type = r[0], rq = r[1], val = r[2] | r[3] << 8, idx = r[4] | r[5] << 8, len = r[6] | r[7] << 8;
    struct fdev *fd = d;
    (void) idx;
    st.ctl_requests++;
    if (control_hook) control_hook();
    if (!in_usb) {
        st.ctl_from_main++;
        violate("control request from MAIN");
    }
    if (!fd || fd != dev || !dev->present)
        return 5;
    if (len && !b)
        return 10;
    if (type == 0x80 && rq == 6) {
        const uint8_t *src = val == 0x0100 ? dev->ddesc : val == 0x0200 ? dev->spec.cfg : NULL;
        unsigned n = val == 0x0100 ? 18 : dev->spec.cfg_len;
        if (!src) return 17;
        memcpy(b, src, len < n ? len : n);
        return len > n ? 20 : 0;
    }
    if (type == 0x01 && rq == 0x0B) { st.alt = val; return 0; }
    if (type == 0x22 && rq == 1 && val == 0x0100 && len == 3) {
        dev->rate = b[0] | b[1] << 8 | (uint32_t) b[2] << 16;
        st.rate_set = dev->rate;
        return 0;
    }
    if (type == 0xA2 && rq == 0x81 && val == 0x0100 && len == 3) {
        b[0] = (uint8_t) dev->rate; b[1] = (uint8_t) (dev->rate >> 8); b[2] = (uint8_t) (dev->rate >> 16);
        return 0;
    }
    if (type == 0x21 && rq == 1 && (val >> 8) == 1 && len == 1) { st.muted = b[0]; return 0; }
    if (type == 0x21 && rq == 1 && (val >> 8) == 2 && len == 2) { st.vol = (int16_t) (b[0] | b[1] << 8); return 0; }
    if (type == 0xA1 && (val >> 8) == 2 && len == 2) {
        int16_t v = rq == 0x82 ? -0x2F00 : rq == 0x83 ? 0 : 0x0100;
        b[0] = (uint8_t) v; b[1] = (uint8_t) ((uint16_t) v >> 8);
        return 0;
    }
    return 17;
}

static unsigned f_set_iface(void *iface, int alt)
{
    struct fiface *i = iface;
    unsigned rest;
    if (!in_usb) violate("set_interface from MAIN");
    if (setup_hook) setup_hook();
    if (!find_iface(i->dev, i->ifnum, (unsigned) alt, &rest)) return 10;
    i->alt = (uint8_t) alt;
    st.alt = (unsigned) alt;
    return 0;
}

static unsigned f_open_pipe(void *iface, unsigned ep, unsigned flags, void **pipe)
{
    struct fiface *i = iface;
    const uint8_t *e = find_ep(i->dev, i->ifnum, i->alt, ep);
    struct fpipe *p;
    unsigned k;
    (void) flags;
    if (!e) return 4;                           /* NO SUCH ENTRY: not in the current alt */
    if ((e[3] & 3) == 1 && (dev->spec.flags & FOS_NO_ISOC_OPEN)) return 10;
    p = calloc(1, sizeof *p);
    p->dev = i->dev;
    p->dev_flags = i->dev->spec.flags;
    p->ep = (uint8_t) ep;
    p->type = e[3] & 3;
    p->maxpkt = (uint16_t) ((e[4] | e[5] << 8) & 0x7FF);
    if (ep_elsewhere)                           /* the pipe's descriptor is not the copy the driver can reach */
        p->maxpkt = (uint16_t) cfg_maxpkt(dev->spec.cfg, dev->spec.cfg_len, ep);
    p->open = 1;
    for (k = 0; k < 8; k++)
        if (!pipes[k]) { pipes[k] = p; break; }
    st.pipes_open++;
    *pipe = p;
    return 0;
}

static int pipe_ok(struct fpipe *p)
{
    unsigned k;
    for (k = 0; k < 8; k++)
        if (pipes[k] == p && p->open) return 1;
    return 0;
}

/* ---- the isochronous scheduler (see fake_os.h) ---- */

/* 10a0d910: FRINDEX as microframes since the last rollover the OS has
 * counted, plus 4 frames. */
static uint16_t now_uf(void)
{
    return (uint16_t) (uf_base + (clock_ms % FOS_FRAME_LIST) * 8u + 32u);
}

static void unlink_sitd(struct fsitd *d)
{
    struct fsitd **at = &flist[d->slot];
    while (*at && *at != d) at = &(*at)->next;
    if (*at) *at = d->next;
    d->used = d->active = 0;
    d->p->nused--;
    st.sitd_linked--;
}

/* 10a0db0c: link every queued frame that fits. */
static void f_schedule(struct fpipe *p)
{
    unsigned k = 0;
    if (p->have_last && ((uint16_t) (p->last + 8 - now_uf()) & 0x8000u))
        p->have_last = 0;                       /* the next slot has passed: start again */
    while (k < p->qn) {
        struct fxfer *x = p->q[k];
        const uint16_t *fl = (const uint16_t *) x->frlengths;
        uint16_t now, sl;
        struct fsitd *d = NULL;
        unsigned i, cur;
        if (x->sched == x->nframes) { k++; continue; }
        now = now_uf();
        if (p->have_last)
            sl = (uint16_t) (p->last + 8);
        else {
            sl = (uint16_t) (now / 8 * 8);
            if ((uint16_t) (sl - now) & 0x8000u)
                sl = (uint16_t) (sl + 8);
        }
        if ((int) sl - (int) now >= (FOS_FRAME_LIST - 4) * 8)
            return;                             /* the frame list reaches no further */
        for (i = 0; i < FOS_SITD_POOL && !d; i++)
            if (!p->pool[i].used) d = &p->pool[i];
        if (!d)
            return;                             /* all 58 descriptors are in the list */
        if (!p->have_last) st.fresh_starts++;
        memset(d, 0, sizeof *d);
        d->x = x;
        d->p = p;
        for (i = 0; i < x->sched; i++) d->off += fl[i];
        d->bytes = d->left = fl[x->sched];
        d->tcount = (d->bytes + FOS_SPLIT - 1) / FOS_SPLIT;     /* 10a0de4c: from the packet's length */
        /* 10a0de28: the S-mask is a table entry picked by wMaxPacketSize /
         * 188 of the pipe's endpoint descriptor, whatever the packet's
         * length. The table is outside the capture; one micro-frame more
         * per 188 bytes (01, 03, 07 ...) is assumed. */
        d->splits = p->maxpkt / FOS_SPLIT + 1u;
        d->slot = (sl >> 3) % FOS_FRAME_LIST;
        d->linked_at = clock_ms;
        cur = clock_ms % FOS_FRAME_LIST;
        d->lead = (d->slot + FOS_FRAME_LIST - cur) % FOS_FRAME_LIST;
        d->ioc = x->sched + 1 == x->nframes || (int) sl - (int) p->last_ioc > 400;
        d->used = d->active = 1;
        d->next = flist[d->slot];               /* in front of what the slot already holds */
        flist[d->slot] = d;
        x->d[x->sched++] = d;
        p->nused++;
        p->last = sl;
        p->have_last = 1;
        if (d->ioc) p->last_ioc = sl;
        if (++st.sitd_linked > st.sitd_max) st.sitd_max = st.sitd_linked;
    }
}

/* usb_transfer_complete (10a080d4): the first queued transfer leaves the
 * queue, its callback runs, then the pipe goes on (usbd_start_next). */
static void complete(struct fpipe *p, unsigned status)
{
    struct fxfer *x = p->q[0];
    unsigned k;
    void (*cb)(void *, void *, unsigned) = (void (*)(void *, void *, unsigned)) x->callback;
    for (k = 1; k < p->qn; k++) p->q[k - 1] = p->q[k];
    p->qn--;
    for (k = x->reaped; k < x->sched; k++) unlink_sitd(x->d[k]);    /* (a cancelled transfer) */
    x->reaped = x->sched;
    x->queued = 0;
    x->status = status;
    if (status == 0) st.isoc_done++;
    if (status == 19) st.isoc_cancelled++;
    {
        int was = in_usb;
        in_usb = 1;
        cb(x, (void *) x->priv, status);
        in_usb = was;
    }
    if (status == 19 || !p->qn)
        p->running = 0;
    else
        f_schedule(p);
}

/* 10a0f144: the soft interrupt's isochronous part, for one pipe. */
static void softintr(struct fpipe *p)
{
    for (;;) {
        struct fxfer *x = p->qn ? p->q[0] : NULL;
        if (!p->nused || !x || !x->sched)
            break;
        while (x->reaped < x->sched) {
            struct fsitd *d = x->d[x->reaped];
            if (d->active)
                goto link;                      /* the first transfer is not finished: nothing behind it is looked at */
            if (d->err) x->err = 1;
            unlink_sitd(d);
            x->reaped++;
        }
        if (x->nframes > x->reaped)
            break;                              /* (only part of it was linked) */
        complete(p, x->err ? 5 : 0);
    }
link:
    /* (fos_flr_resched(0): only a pipe with something queued is looked at,
     * so an idle pipe keeps its last linked frame however old it gets) */
    if (flr_resched || p->qn)
        f_schedule(p);
}

/* The controller's interrupt, when the CPU takes it. */
static void irq_service(void)
{
    unsigned k;
    int was = in_usb;
    if (!irq_flr && !irq_usbint)
        return;
    if (irq_flr)
        uf_base = (uint16_t) (uf_base + FOS_FRAME_LIST * 8u);
    irq_flr = irq_usbint = 0;
    st.softintrs++;
    in_usb = 1;
    for (k = 0; k < 8; k++)
        if (pipes[k] && pipes[k]->type == 1 && pipes[k]->open)
            softintr(pipes[k]);
    in_usb = was;
}

/* One frame (1 ms) of the bus. */
static void hw_frame(void)
{
    struct fsitd *d;
    struct fpipe *sent_on = NULL;
    unsigned k;
    clock_ms++;
    for (d = flist[clock_ms % FOS_FRAME_LIST]; d; d = d->next) {
        unsigned n = d->left;
        if (!d->active || !dev || !dev->present || (dev->spec.flags & FOS_NEVER_DONE) || stalled)
            continue;
        int wait = 0, split = hw_model == FOS_HW_FOTG210 || hw_model == FOS_HW_SMASK;
        if (split && n > FOS_SPLIT) n = FOS_SPLIT;                         /* one start-split a pass */
        if (hw_model == FOS_HW_LATE && !d->waited) wait = 1;
        if (hw_model == FOS_HW_LEAD && !d->waited && d->lead < 12) wait = 1;
        if (wait)
            d->waited = 1;
        else {
            if (d->left || !d->visits) {        /* (a pass for an S-mask bit with nothing left sends nothing) */
                if (cap_np < FOS_CAP_PKTS) cap_sizes[cap_np++] = (uint16_t) n;
                if (cap_nb + n <= FOS_CAP_BYTES) {
                    memcpy(cap_bytes + cap_nb, d->x->data + d->off + (d->bytes - d->left), n);
                    cap_nb += n;
                }
                sent_on = d->p;
                st.wire_frames++;
                if (d->x->seq >= 1) d->p->primed = 1;
            }
            d->left -= n;
            d->visits++;
            if (d->tcount) d->tcount--;
            if (!d->left && (hw_model != FOS_HW_SMASK || d->visits >= d->splits)) {
                d->active = 0;
                d->err = (dev->spec.flags & FOS_ISOC_ERR) != 0;
                if (clock_ms - d->linked_at >= FOS_FRAME_LIST) st.late_frames++;
                else st.ontime_frames++;
                if (d->ioc) irq_usbint = 1;
            }
        }
    }
    /* A frame with no packet once the stream plays is a gap in the sound
     * only if a packet follows it: counted when the next transfer is
     * queued (f_transfer). The frames after a stream's last packet, while
     * a stop lets the queue play out before it closes the pipe (ua_stop,
     * 2026-11-16: no abort there), are its end, not a gap. */
    for (k = 0; k < 8; k++)
        if (pipes[k] && pipes[k]->type == 1 && pipes[k]->primed && pipes[k] != sent_on && dev && dev->present)
            pipes[k]->gap++;
    if (clock_ms % FOS_FRAME_LIST == 0)
        irq_flr = 1;
}

/* tests/legacy_test.c: the driver as it was on 2026-11-14 freed its aborted
 * transfers with the mark on (counted, not a violation there). */
static int marks_expected;
void fos_expect_marked_frees(int yes) { marks_expected = yes; }

/* (ua_os.c, UA_HOST_TEST) where this OS's transfers keep TI's mark */
uint8_t *ua_fake_xfer_mark(void *xfer)
{
    return &((struct fxfer *) xfer)->mark;
}

static void cancel_all(struct fpipe *p)
{
    unsigned k;
    p->aborting = 1;
    for (k = 0; k < p->qn; k++)
        p->q[k]->mark |= 1;             /* TI's isochronous abort: set, never cleared (10a0f588) */
    while (p->qn) complete(p, 19);
    p->aborting = 0;
    p->have_last = 0;
}

static unsigned f_abort(void *pipe)
{
    struct fpipe *p = pipe;
    if (!in_usb && irq_check && !irq_check()) violate("abort with CPU IRQs masked");
    if (!pipe_ok(p)) { violate("abort of a closed pipe"); return 10; }
    if (p->detached) { violate("abort of a pipe after detach returned"); return 10; }
    st.abort_calls++;
    if ((p->dev_flags & FOS_ABORT_ERROR) && st.abort_calls == 1)
        return 5;
    if ((p->dev_flags & FOS_ABORT_DELAYED) && p->qn) {
        if (!p->cancel_ticks) p->cancel_ticks = 65;
        return 0;
    }
    cancel_all(p);
    return 0;
}

static unsigned f_close(void *pipe)
{
    struct fpipe *p = pipe;
    unsigned k;
    if (!in_usb && irq_check && !irq_check()) violate("close with CPU IRQs masked");
    if (!pipe_ok(p)) { violate("close of a closed pipe"); return 10; }
    if (p->detached) { violate("close of a pipe after detach returned"); return 10; }
    if (p->qn) violate("close with transfers queued");
    st.close_calls++;
    if ((p->dev_flags & FOS_CLOSE_ERROR) && st.close_calls == 1)
        return 5;
    for (k = 0; k < FOS_SITD_POOL; k++)
        if (p->pool[k].used) unlink_sitd(&p->pool[k]);
    for (k = 0; k < 8; k++) if (pipes[k] == p) pipes[k] = NULL;
    p->open = 0;
    free(p);
    st.pipes_open--;
    return 0;
}

static void *f_alloc_xfer(void *d)
{
    struct fxfer *x = calloc(1, sizeof *x);
    struct fdev *fd = d;
    unsigned k;
    x->dev = fd;
    x->dev_flags = fd ? fd->spec.flags : 0;
    x->magic = XMAGIC;
    for (k = 0; k < 64; k++)
        if (!xfers[k]) { xfers[k] = x; break; }
    st.xfers_open++;
    return x;
}
static unsigned f_free_xfer(void *xv)
{
    struct fxfer *x = xv;
    unsigned k;
    if (!x || x->magic != XMAGIC) { violate("free of a bad xfer"); return 10; }
    if (x->queued) violate("free of a queued xfer");
    if (x->detached) { violate("free of an xfer after detach returned"); return 10; }
    /* TI's pool hands the block to the next transfer as it is; aborting that
     * one then never returns (10a10908) */
    if (x->mark & 1) {
        st.marked_frees++;
        if (!marks_expected)
            violate("an aborted isochronous xfer was freed with TI's abort mark still on it");
    }
    for (k = 0; k < 64; k++)
        if (xfers[k] == x) { xfers[k] = NULL; break; }
    x->magic = 0;
    free(x->data);
    free(x);
    st.xfers_open--;
    return 0;
}
static void f_setup_xfer(void *xv, void *pipe, void *priv, void *buf, uint32_t len, unsigned flags,
                         uint32_t timeout, void *cb)
{
    struct fxfer *x = xv;
    if (x->queued) violate("setup of a queued xfer");
    last_callback = (usbshim_cb)cb;
    x->pipe = (uintptr_t) pipe; x->priv = (uintptr_t) priv; x->buffer = (uintptr_t) buf; x->length = len;
    x->actlen = 0; x->flags = flags; x->timeout = timeout; x->status = 1; x->callback = (uintptr_t) cb;
    x->frlengths = 0; /* TI setup_xfer clears the isoc field. */
}
/* The argument order and zero length come from the captured CX II code,
 * not from the SDK's incompatible NetBSD prototype. */
static void f_setup_isoc(void *xv, void *pipe, void *priv, void *buf, uint16_t *fl,
                          uint32_t n, unsigned flags, void *cb)
{
    struct fxfer *x = xv;
    if (x->queued) violate("setup of a queued xfer");
    last_callback = (usbshim_cb)cb;
    x->pipe = (uintptr_t) pipe; x->priv = (uintptr_t) priv; x->buffer = (uintptr_t) buf;
    x->length = 0; x->actlen = 0;
    x->flags = flags | 4; x->timeout = 0; x->status = 1; x->callback = (uintptr_t) cb;
    x->frlengths = (uintptr_t) fl; x->nframes = n;
}
static void f_xfer_status(void *xv, void **priv, void **buf, uint32_t *count, uint8_t *status)
{
    struct fxfer *x = xv;
    if (priv) *priv = (void *)x->priv;
    if (buf) *buf = (void *)x->buffer;
    if (count) *count = (uint32_t)x->actlen;
    if (status) *status = (uint8_t)x->status;
}

/* usbd_transfer (10a084e4) on an isochronous pipe: sum the frame lengths,
 * copy the packets, queue; only a pipe that is not running links frames
 * here (10a0e080), otherwise the soft interrupt does. */
static unsigned f_transfer(void *xv)
{
    struct fxfer *x = xv;
    struct fpipe *p = (struct fpipe *) x->pipe;
    uint16_t *fl = (uint16_t *) x->frlengths;
    unsigned i, sum = 0;
    if (!in_usb && irq_check && !irq_check()) violate("transfer with CPU IRQs masked");
    if (!in_usb) irq_service();                 /* MAIN has just unmasked the CPU's interrupts */
    if (!in_usb && main_transfer_hook) main_transfer_hook();
    if (in_usb && transfer_hook) transfer_hook();
    const uint8_t *src;
    if (!x || x->magic != XMAGIC) { violate("transfer of a bad xfer"); return 10; }
    if (x->queued) { violate("resubmission of a queued xfer"); return 10; }
    if (!pipe_ok(p)) { violate("transfer on a closed pipe"); return 10; }
    if (p->detached) st.dead_transfers++;  /* (its device's detach has returned: refused below) */
    if (p->aborting) return 19;
    if (!dev || !dev->present || p->dev != dev) return 5;
    if (p->type != 1) return 10;
    if (dev->spec.flags & FOS_NO_ISOC_XFER) return 5;
    if (fail_xfers) { fail_xfers--; return 5; }
    if (!fl || !x->nframes) { violate("isochronous transfer without frames"); return 10; }
    if (x->nframes > FOS_MAXF) { violate("more frames in a transfer than the simulator holds"); return 10; }
    if (!x->buffer) { violate("isochronous transfer with no buffer (NetBSD would copy from address 0)"); return 10; }
    for (i = 0; i < x->nframes; i++) {
        if (fl[i] > p->maxpkt) violate("packet larger than wMaxPacketSize");
        sum += fl[i];
    }
    if (x->length != 0) violate("isoc length was not zero after setup");
    x->length = sum; /* the TI transfer function computes the sum */
    {
        struct { uint8_t *ptr; int z1, z2; } *desc = (void *)x->buffer;
        if (desc->z1 || desc->z2) { violate("transfer buffer is not a TI descriptor"); return 10; }
        src = desc->ptr;
    }
    /* Transfer uses a descriptor independently of the control convention. */
    if (p->qn >= 16) return 7;
    x->data = realloc(x->data, sum ? sum : 1);
    memcpy(x->data, src, sum);
    x->sched = x->reaped = x->err = 0;
    x->mark = 0;                        /* the isochronous submission clears it (10a0f8b0) */
    st.starved_frames += p->gap;        /* frames that went by empty before this packet: a gap */
    p->gap = 0;
    x->seq = p->submitted++;
    p->q[p->qn++] = x;
    x->queued = 1;
    st.isoc_submitted++;
    x->status = 2;
    if (!p->running) {
        p->running = 1;
        f_schedule(p);
    }
    return 2; /* the captured OS queues asynchronously with status 2 */
}

static const char *f_errstr(unsigned s)
{
    /* Literal capture, independent of the production enum. */
    static const char *const n[] = {
        "NORMAL COMPLETION", "OPERATION NOT STARTED", "OPERATION IN PROGRESS",
        "OPERATION NOT PERMITTED", "NO SUCH ENTRY", "INPUT/OUTPUT ERROR",
        "DEVICE NOT CONFIGURED", "FAILED ALLOCATING MEMORY", "RESOURCE IS BUSY",
        "NO SUCH DEVICE", "INVALID ARGUMENT", "OPERATION NOT SUPPORTED",
        "OPERATION TIMED OUT", "DEVICE IS SUSPENDED", "GENERAL-PURPOSE ERROR",
        "LOGICAL TEST FAILURE", "INCORRECT STATE", "PIPE IS STALLED",
        "INVALID PARAMETER", "OPERATION ABORTED", "SHORT TRANSFER",
    };
    return s < 21 ? n[s] : "unknown";
}

static unsigned f_register(void) { return 0; }

int ua_fake_has(unsigned nr)
{
    unsigned k;
    for (k = 0; k < nremoved; k++)
        if (removed[k] == nr) return 0;
    return ua_fake_addr(nr) != 0;
}

uintptr_t ua_fake_addr(unsigned nr)
{
    switch (nr) {
    case 260: return (uintptr_t) f_ivars;
    case 248: return (uintptr_t) f_iface_desc;
    case 249: return (uintptr_t) f_config_desc;
    case 250: return (uintptr_t) f_dev_desc;
    case 245: return (uintptr_t) f_do_request;
    case 240: return (uintptr_t) f_iface2dev;
    case 251: return (uintptr_t) f_set_iface;
    case 227: return (uintptr_t) f_open_pipe;
    case 228: return (uintptr_t) f_close;
    case 236: return (uintptr_t) f_abort;
    case 230: return (uintptr_t) f_alloc_xfer;
    case 231: return (uintptr_t) f_free_xfer;
    case 232: return (uintptr_t) f_setup_xfer;
    case 233: return (uintptr_t) f_setup_isoc;
    case 234: return (uintptr_t) f_xfer_status;
    case 229: return (uintptr_t) f_transfer;
    case 254: return (uintptr_t) f_errstr;
    case 258: return (uintptr_t) f_register;
    }
    return 0;
}

/* The line main.c's usb_sched() writes on the calculator, from the model. */
void fos_sched_text(char *buf, unsigned size)
{
    unsigned cur = clock_ms % FOS_FRAME_LIST, i, n = 0, waiting = 0, far = 0, sent = 0, lo = 0, hi = 0;
    const struct fsitd *sample = NULL;
    for (i = 0; i < FOS_FRAME_LIST; i++) {
        const struct fsitd *d;
        for (d = flist[(cur + i) % FOS_FRAME_LIST]; d; d = d->next) {
            n++;
            if (!d->active) { sent++; continue; }
            if (!waiting++) lo = i;
            hi = i;
            if (i > 64 && !far++) sample = d;
            if (!sample) sample = d;
        }
    }
    i = (unsigned) snprintf(buf, size, "frame %u, siTD %u: %u waiting at +%u..+%u (%u more than 64 frames away), %u sent",
                            cur, n, waiting, lo, hi, far, sent);
    if (sample && i < size)
        snprintf(buf + i, size - i, "; first %s: slot %u, %u of %u bytes left, T-count %u, S-mask %02x, IOC %u",
                 far ? "far one" : "waiting", sample->slot, sample->left, sample->bytes, sample->tcount,
                 (1u << sample->splits) - 1u, sample->ioc);
}

/* ---- the USB task ---- */
static void do_plug(void)
{
    unsigned off = 0, n = 0, best_i = 0;
    int best = 0;
    if (!registered || retired) return;     /* offered to nobody: the OS's own drivers only */
    in_usb = 1;
    dev->devself.uaa.vendor = dev->spec.vid;
    dev->devself.uaa.product = dev->spec.pid;
    dev->devself.uaa.device = dev;
    dev->devself.uaa.iface = NULL;
    st.probes_dev++;
    drv[0](&dev->devself);
    while (off + 2 <= dev->spec.cfg_len && dev->spec.cfg[off] >= 2) {
        const uint8_t *p = dev->spec.cfg + off;
        if (p[1] == 4 && p[3] == 0 && n < 8) {
            struct fiface *fi = &dev->ifs[n];
            struct fself *s = &dev->selfs[n];
            int score;
            fi->dev = dev; fi->ifnum = p[2]; fi->alt = 0;
            s->uaa.vendor = dev->spec.vid; s->uaa.product = dev->spec.pid;
            s->uaa.device = dev; s->uaa.iface = fi; s->uaa.ifaceno = p[2];
            n++;
            if (retired) continue;
            st.probes_iface++;
            score = drv[0](s);
            if (score > 0) {
                if (match_hook) match_hook();
                if (drv[1](s) == 0) { s->attached = 1; st.attach_ok++; }
                else {
                    /* gpSP's CX II run of 2026-11-15: after this TI's OS
                     * offers no further device until the adapter is pulled */
                    st.attach_fail++;
                    violate("attach returned an error after a nonzero match (the OS then offers nothing more)");
                }
            }
            if (score > best) { best = score; best_i = n - 1; }
        }
        off += p[0];
    }
    (void) best_i;
    dev->nif = n;
    in_usb = 0;
}

static void do_unplug(void)
{
    unsigned i, k;
    in_usb = 1;
    dev->present = 0;
    for (i = 0; i < dev->nif; i++)
        if (dev->selfs[i].attached) {
            dev->selfs[i].attached = 0;
            st.detaches++;
            if (!retired) drv[2](&dev->selfs[i]);
        }
    for (k = 0; k < 8; k++)
        if (pipes[k] && pipes[k]->dev == dev) {
            /* Once detach returned 0, TI's OS frees device/iface: abort/close
             * must never touch this pipe again. Only faulted cancels/closes may
             * remain kept across unplug. */
            pipes[k]->detached = 1;
            pipes[k]->dev = NULL;
            if (kept_expected)
                pipes[k]->dev_flags |= FOS_KEPT_OK;
            if (!(pipes[k]->dev_flags & (FOS_ABORT_DELAYED | FOS_ABORT_ERROR | FOS_CLOSE_ERROR | FOS_KEPT_OK)))
                violate("a pipe is still open after detach");
            else
                st.pipes_kept++;
        }
    for (k = 0; k < 64; k++)
        if (xfers[k] && xfers[k]->dev == dev) {
            xfers[k]->detached = 1;
            xfers[k]->dev = NULL;
            if (kept_expected)
                xfers[k]->dev_flags |= FOS_KEPT_OK;
            if (!(xfers[k]->dev_flags & (FOS_ABORT_DELAYED | FOS_ABORT_ERROR | FOS_KEPT_OK)))
                violate("an xfer is still open after detach");
            else
                st.xfers_kept++;
        }
    in_usb = 0;
}

void fos_run(unsigned ms)
{
    unsigned k;
    if (pending_plug) { pending_plug = 0; if (dev) { dev->present = 1; do_plug(); } }
    if (pending_unplug) { pending_unplug = 0; if (dev && dev->present) do_unplug(); }
    for (k = 0; k < 8; k++) {
        struct fpipe *p = pipes[k];
        if (p && p->cancel_ticks && !--p->cancel_ticks)
            cancel_all(p);
    }
    irq_service();                          /* whatever was raised while MAIN was busy */
    while (ms--) {
        hw_frame();
        irq_service();
    }
}

void fos_sleep(void)
{
    fos_run(10);
}

void fos_irq_window(void)
{
    irq_service();
}

unsigned fos_frames_ahead(void)
{
    unsigned k, i, n = 0;
    for (k = 0; k < 8; k++) {
        struct fpipe *p = pipes[k];
        if (!p || p->type != 1 || !p->open)
            continue;
        for (i = 0; i < FOS_SITD_POOL; i++)
            if (p->pool[i].used && p->pool[i].active)
                n++;
        for (i = 0; i < p->qn; i++)
            n += (unsigned) p->q[i]->nframes - p->q[i]->sched;
    }
    return n;
}

/* ua18: ms until the last packet handed to the OS has gone out: its frame
 * list slot past the controller's frame, the frames of what is queued and
 * not linked after it (what ua_queue_ms estimates). */
unsigned fos_wire_left(void)
{
    unsigned k, i, n = 0, unl = 0, cur = clock_ms % FOS_FRAME_LIST, d;
    for (k = 0; k < 8; k++) {
        struct fpipe *p = pipes[k];
        if (!p || p->type != 1 || !p->open)
            continue;
        for (i = 0; i < FOS_SITD_POOL; i++)
            if (p->pool[i].used && p->pool[i].active) {
                d = (p->pool[i].slot + FOS_FRAME_LIST - cur) % FOS_FRAME_LIST + 1;
                if (d > n) n = d;
            }
        for (i = 0; i < p->qn; i++)
            unl += (unsigned) p->q[i]->nframes - p->q[i]->sched;
    }
    return n + unl;
}

void fos_stall(unsigned ms)
{
    while (ms--)
        hw_frame();
}

static void free_fake_handles(int check_violations)
{
    unsigned k, j;
    for (k = 0; k < 8; k++)
        if (pipes[k]) {
            struct fpipe *p = pipes[k];
            if (check_violations &&
                (!p->detached || !(p->dev_flags & (FOS_ABORT_DELAYED | FOS_ABORT_ERROR | FOS_CLOSE_ERROR | FOS_KEPT_OK))))
                violate("pipe left open at the end");
            for (j = 0; j < FOS_SITD_POOL; j++)
                if (p->pool[j].used) unlink_sitd(&p->pool[j]);
            free(p);
            pipes[k] = NULL;
        }
    for (k = 0; k < 64; k++)
        if (xfers[k]) {
            struct fxfer *x = xfers[k];
            if (check_violations &&
                (!x->detached || !(x->dev_flags & (FOS_ABORT_DELAYED | FOS_ABORT_ERROR | FOS_KEPT_OK))))
                violate("xfer left open at the end");
            free(x->data);
            free(x);
            xfers[k] = NULL;
        }
}

void fos_init(void)
{
    free_fake_handles(0);
    memset(&st, 0, sizeof st);
    st.muted = -1;
    st.vol = 0x7FFF;
    registered = retired = 0;
    completion_fn = last_callback = NULL;
    setup_hook = control_hook = transfer_hook = requeue_hook = match_hook = main_irq_hook = park_hook = held_irq_hook = main_transfer_hook = NULL;
    kept_expected = 0;
    flr_resched = 1;
    fail_xfers = 0;
    stalled = 0;
    nremoved = 0;
    conv = 2;
    pending_plug = pending_unplug = 0;
    memset(flist, 0, sizeof flist);
    clock_ms = 0;
    uf_base = 0;
    irq_flr = irq_usbint = 0;
    hw_model = FOS_HW_FOTG210;
    ep_elsewhere = 0;
    if (!cap_sizes) cap_sizes = malloc(FOS_CAP_PKTS * sizeof *cap_sizes);
    if (!cap_bytes) cap_bytes = malloc(FOS_CAP_BYTES);
    cap_np = cap_nb = 0;
    free(dev);
    dev = NULL;
}

void fos_fini(void)
{
    free_fake_handles(1);
    free(dev);
    dev = NULL;
    free(cap_sizes); cap_sizes = NULL;
    free(cap_bytes); cap_bytes = NULL;
}

int fos_plug(const struct fos_spec *spec)
{
    free(dev);
    dev = calloc(1, sizeof *dev);
    dev->spec = *spec;
    if (spec->cfg && spec->cfg_len <= sizeof dev->cdesc)
        memcpy(dev->cdesc, spec->cfg, spec->cfg_len);
    dev->ddesc[0] = 18; dev->ddesc[1] = 1; dev->ddesc[2] = 0x10; dev->ddesc[3] = 1; dev->ddesc[7] = 64;
    dev->ddesc[8] = (uint8_t) spec->vid; dev->ddesc[9] = (uint8_t) (spec->vid >> 8);
    dev->ddesc[10] = (uint8_t) spec->pid; dev->ddesc[11] = (uint8_t) (spec->pid >> 8); dev->ddesc[17] = 1;
    dev->rate = 48000;
    pending_plug = 1;
    return 0;
}

void fos_unplug(void) { pending_unplug = 1; }

/* A second device's AS interface, valid only for this synchronous call.
 * These tests must reject attach while the original device owns resources;
 * a match-only call may also check that cleanup permits a future offer. */
int fos_offer_other(int force_attach)
{
    struct fdev other = *dev;
    struct fiface iface = { &other, 1, 0 };
    struct fself self;
    int r, was = in_usb;
    memset(&self, 0, sizeof self);
    other.spec.vid ^= 1;
    self.uaa.vendor = other.spec.vid;
    self.uaa.product = other.spec.pid;
    self.uaa.device = &other;
    self.uaa.iface = &iface;
    in_usb = 1;
    r = drv[force_attach ? 1 : 0](&self);
    in_usb = was;
    return r;
}
