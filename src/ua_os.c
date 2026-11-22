/* USB audio through the TI-Nspire OS's USB host stack.
 *
 * CX II OS 6.4's captured functions differ from the Ndless/NetBSD headers:
 * setup_isoc takes (xfer, pipe, priv, buffer, lengths, nframes, flags, cb),
 * transfer returns 2 when queued, and get_xfer_status writes a byte status.
 * Transfer buffers are TI descriptors even when control requests use raw
 * pointers. See tests/ti_abi_fixture.S and NOTES.txt for the captured ABI.
 * Validate the isoc implementation before calling a derived OS address;
 * validate the resulting transfer through the OS accessor, never by
 * scanning or overwriting an opaque OS allocation.
 */
#include <stdlib.h>
#include <string.h>
#include "ua_os.h"

#if defined(UA_HOST_TEST)
#define USBSHIM_TEST 1
#elif !defined(_TINSPIRE)
#error "build with nspire-gcc (_TINSPIRE) or define UA_HOST_TEST"
#endif
#include "usbshim.h"
#include "usbshim_usb.h"
/* 2026-11-16: the host controller's pool of free transfer blocks (an
 * nspire-usbshim with usbshim_xfer.h; without it nothing is cleaned). */
#if defined(__has_include)
#if __has_include("usbshim_xfer.h")
#include "usbshim_xfer.h"
#define HAVE_XFER_POOL 1
#endif
#endif

#ifdef _TINSPIRE
#include <os.h>
#define OS_ADDR(nr, out)                                                        \
    do {                                                                        \
        register uintptr_t r0_ __asm__("r0");                                   \
        __asm__ __volatile__("swi %[n]" : "=r"(r0_)                             \
                             : [n] "i"(__SYSCALLS_ISVAR | (nr))                 \
                             : "r1", "r2", "r3", "r12", "lr", "memory", "cc");  \
        (out) = r0_;                                                            \
    } while (0)
#else
#pragma weak ua_fake_has
#pragma weak ua_fake_addr
#pragma weak ua_fake_before_requeue
#pragma weak ua_fake_main_irq
#pragma weak ua_fake_park
#pragma weak ua_fake_irq_held
#pragma weak ua_fake_frame
#pragma weak usbshim_register
#pragma weak usbshim_retire
#endif

#define barrier() __asm__ __volatile__("" ::: "memory")

/* ---- the OS's functions ---------------------------------------------------- */

#define FNS(X)                                                              \
    X(IVARS,       260, "device_get_ivars",                1)              \
    X(IFACE_DESC,  248, "usbd_get_interface_descriptor",   1)              \
    X(CONFIG_DESC, 249, "usbd_get_config_descriptor",      1)              \
    X(DEV_DESC,    250, "usbd_get_device_descriptor",      0)              \
    X(DO_REQUEST,  245, "usbd_do_request",                 1)              \
    X(IFACE2DEV,   240, "usbd_interface2device_handle",    0)              \
    X(SET_IFACE,   251, "usbd_set_interface",              0)              \
    X(OPEN_PIPE,   227, "usbd_open_pipe",                  1)              \
    X(CLOSE_PIPE,  228, "usbd_close_pipe",                 1)              \
    X(ABORT_PIPE,  236, "usbd_abort_pipe",                 1)              \
    X(ALLOC_XFER,  230, "usbd_alloc_xfer",                 1)              \
    X(FREE_XFER,   231, "usbd_free_xfer",                  1)              \
    X(SETUP_XFER,  232, "usbd_setup_xfer",                 1)              \
    X(SETUP_ISOC,  233, "usbd_setup_isoc_xfer",            0)              \
    X(XFER_STATUS, 234, "usbd_get_xfer_status",            1)              \
    X(TRANSFER,    229, "usbd_transfer",                   1)              \
    X(ERRSTR,      254, "usbd_errstr",                     1)              \
    X(REGISTER,    258, "usb_register_driver",             1)

enum {
#define X(id, nr, name, req) F_##id,
    FNS(X)
#undef X
    F_COUNT
};
typedef char fn_count_ok[F_COUNT == UA_NFN ? 1 : -1];
static const uint16_t fn_nr[F_COUNT] = {
#define X(id, nr, name, req) nr,
    FNS(X)
#undef X
};
static const uint8_t fn_req[F_COUNT] = {
#define X(id, nr, name, req) req,
    FNS(X)
#undef X
};
static const char *const fn_names[F_COUNT] = {
#define X(id, nr, name, req) name,
    FNS(X)
#undef X
};
const char *ua_fn_name(unsigned i) { return i < F_COUNT ? fn_names[i] : NULL; }

typedef void *os_dev;
typedef void *os_iface;
typedef void *os_pipe;
typedef void *os_xfer;
typedef void (*os_cb)(os_xfer xfer, void *priv, unsigned status);

static struct {
    void *(*ivars)(device_t self);
    const uint8_t *(*iface_desc)(os_iface iface);
    const uint8_t *(*config_desc)(os_dev dev);
    const uint8_t *(*dev_desc)(os_dev dev);
    unsigned (*do_request)(os_dev dev, void *req, void *data);
    unsigned (*iface2dev)(os_iface iface, os_dev *dev);
    unsigned (*set_iface)(os_iface iface, int alt);
    unsigned (*open_pipe)(os_iface iface, unsigned ep, unsigned flags, os_pipe *pipe);
    unsigned (*close_pipe)(os_pipe pipe);
    unsigned (*abort_pipe)(os_pipe pipe);
    os_xfer (*alloc_xfer)(os_dev dev);
    unsigned (*free_xfer)(os_xfer xfer);
    void (*setup_xfer)(os_xfer xfer, os_pipe pipe, void *priv, void *buf, uint32_t len,
                       unsigned flags, uint32_t timeout, os_cb cb);
    void (*setup_isoc)(os_xfer xfer, os_pipe pipe, void *priv, void *buf, uint16_t *frlengths,
                       uint32_t nframes, unsigned flags, os_cb cb);
    void (*xfer_status)(os_xfer xfer, void **priv, void **buf, uint32_t *count, uint8_t *status);
    unsigned (*transfer)(os_xfer xfer);
    const char *(*errstr)(unsigned status);
} api;

#define ST(x)        ((unsigned) (x) & 0xFFu)
#define OS_OK          USBSHIM_USB_OK
#define OS_NOT_STARTED USBSHIM_USB_NOT_STARTED
#define OS_INPROG      USBSHIM_USB_IN_PROGRESS
#define OS_INVAL       USBSHIM_USB_INVALID_ARGUMENT
#define OS_CANCELLED   USBSHIM_USB_ABORTED
#define OS_IOERROR     USBSHIM_USB_IO_ERROR
#define OS_NOMEM       USBSHIM_USB_NO_MEMORY
#define OS_TIMEOUT     USBSHIM_USB_TIMEOUT
#define OS_SHORT       USBSHIM_USB_SHORT_TRANSFER
#define OS_EXCLUSIVE 0x01
#define OS_ENXIO     6
#define MATCH_SCORE  4              /* UMATCH_IFACECLASS_IFACESUBCLASS */
#define VID_TI       0x0451
#define SOFTC_SIZE   8
#define CTL_BUF      1024

struct os_attach_arg {              /* the first 8 words of the OS's usb_attach_arg */
    int port, configno, ifaceno, vendor, product, release;
    void *device, *iface;
};
struct ti_buf { uint8_t *buf; int zero1, zero2; };

static struct ua_osinfo osi;

/* ---- OS-visible memory (malloc'd in MAIN) ----------------------------------- */

#define PKT_BYTES ((UA_NF * UA_PKT_MAX + 31) & ~31)
enum { M_FREE = 0, M_USED, M_KEPT };
#define UA_DRAIN_TICKS 16           /* OS ticks a stop waits for the queued transfers to play out */
#define UA_MEM_SLOTS 4
#define UA_KEPT_MAX  32               /* quarantined pools kept reachable (each detach in MAIN's window adds one) */

struct os_mem;
struct slot {
    uint8_t idx;
    volatile uint8_t busy;          /* queued with the OS */
    volatile uint8_t deferred;      /* completed while MAIN was still submitting */
    uint16_t t_queued;              /* the controller's frame when it was queued */
    uint16_t t_end;                 /* ua18: the frame after its last packet, as the OS links it (chain_end) */
    uint16_t slack;                 /* ua18: how much later than t_end it may be, at the most (ms) */
    uint8_t pending;                /* ua18: queued by MAIN behind a running pipe: linked at the next soft interrupt */
    uint16_t ahead;                 /* packets queued before it then */
    os_xfer xfer;
    uint32_t bytes;
    struct os_mem *mem;             /* owning block */
    uint8_t aborted;                /* queued with the OS when its pipe was aborted */
    uint8_t mark_pre;               /* TI's mark byte then (0xFF: not read) */
    volatile uint8_t parked;        /* idle: came back and was not queued again (ua_set_idle) */
    int8_t  fade;                   /* the next refill: +1 fades in, -1 fades out, 0 neither */
};

struct os_mem {
    uint8_t  pkt[UA_NX][PKT_BYTES];
    uint16_t frlen[UA_NX][16];      /* UA_NF used; 32 bytes each */
    uint8_t  ctl[CTL_BUF];
    struct ti_buf pkt_desc[UA_NX];
    struct ti_buf ctl_desc;
    os_pipe  pipe;
    uint8_t  req[8];
    struct slot slots[UA_NX];
    volatile uint8_t mstate;        /* M_FREE / M_USED / M_KEPT */
    volatile uint8_t detached;      /* detach_body returned: never abort/close/free again */
    volatile uint8_t close_tok;
    volatile uint8_t free_tok;
    volatile uint8_t abort_err;
    volatile uint8_t in_cb;         /* cb_isoc currently running in this block */
    uint8_t  pad[2];
} __attribute__((aligned(32)));

static struct os_mem *mem_pool;
static struct os_mem *volatile mem;
static void *mem_raw;
static void *volatile kept_mem[UA_KEPT_MAX];
static unsigned kept_n;

/* ---- state ---------------------------------------------------------------------- */

/* Timing for the log. Written where a transfer is queued and in its
 * completion; MAIN takes a copy with interrupts off. A lost update costs a
 * number in the log, nothing else. */
static uintptr_t frame_reg;
static struct ua_second sec;
static struct ua_first first[UA_FIRST];
static volatile unsigned nfirst;
static uint16_t t_last_done;
static uint8_t have_last_done;

static volatile int state;
static volatile unsigned fail_step, fail_status;
static usbshim_t *shim;
static usbshim_cb completion;
static void cb_isoc(os_xfer xfer, void *priv, unsigned status);
static int any_busy(void);
static int resources_owned(void);
static const char *init_text = "not initialised";
static volatile uint8_t up, closing, stopping, submitted_all, paused;
static volatile uint8_t idle_req;   /* ua_set_idle(1): completions park their transfers */
static volatile uint8_t fade_out;   /* idle asked: the first completion queues one fade-out */
static volatile uint8_t fade_in;    /* resumed after a fade-out: the next packets of audio fade in */
static uint8_t fade_pos;            /* ... of them faded in so far (refill's) */
/* Bumped by every detach and every attach that takes a block: MAIN reads it
 * with mem and checks it with interrupts off before it touches a transfer
 * again (a detach can run at any IRQ point of MAIN's: it frees the
 * transfers, and the next attach may take the same block). */
static volatile uint32_t mem_gen;
/* Idle keep-alive (ua_set_idle): MAIN's frame clock readings while idle. */
#define UA_KEEPALIVE_MS    1000     /* one transfer of silence at least this often while idle */
#define UA_KEEPALIVE_CALLS 64       /* without a frame clock: every so many ua_set_idle(1) calls */
#define UA_RESUME_KEEP_MS  (UA_NX * UA_NF)  /* the ring's newest audio kept at a resume: a full queue's worth */
static uint32_t ka_prev, ka_elapsed;
static unsigned ka_calls;
static uint8_t ka_have;
static volatile unsigned attaching;
static volatile unsigned callbacks;
static volatile unsigned volume = 192;
static volatile uint32_t probes;
static device_t att_self;
static device_t matched_self;      /* the last offer match_body answered nonzero (attach_refuse) */
static os_dev att_dev;
static os_iface att_iface;
static struct ring *ring;
static struct pkt_sched sched;
static struct dev_fmt dfmt;
static unsigned bytes_per_frame;
static uint8_t first_err_sent;
static struct ua_desc desc;
static struct ua_counters cnt;
static int16_t tmp_lr[2 * 200];
static int16_t tmp_main[2 * 200];

/* event ring: USB (and MAIN) write, MAIN reads */
#define EVN 64
static struct ua_event evq[EVN];
static volatile uint32_t ev_wr, ev_rd, ev_lost;

static uint8_t xchg8(volatile uint8_t *p, uint8_t v)
{
#if defined(__arm__)
    uint32_t old;
    __asm__ __volatile__("swpb %0, %2, [%1]" : "=&r"(old) : "r"(p), "r"((uint32_t) v) : "memory");
    return (uint8_t) old;
#else
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

/* Posting may happen from MAIN and from a callback that interrupts it; a
 * lost race costs one event (counted), never memory safety. */
static void post(unsigned code, unsigned step, unsigned status, uint32_t a, uint32_t b, uint32_t c)
{
    static volatile uint8_t posting;
    uint32_t w;
    if (xchg8(&posting, 1)) {
        ev_lost++;
        return;
    }
    w = ev_wr;
    if (w - ev_rd >= EVN) {
        ev_lost++;
    } else {
        struct ua_event *e = &evq[w % EVN];
        e->code = (uint16_t) code;
        e->step = (uint16_t) step;
        e->status = status;
        e->a = a;
        e->b = b;
        e->c = c;
        barrier();
        ev_wr = w + 1;
    }
    barrier();
    posting = 0;
}

int ua_take_event(struct ua_event *e)
{
    uint32_t r = ev_rd;
    if (r == ev_wr)
        return 0;
    barrier();
    *e = evq[r % EVN];
    barrier();
    ev_rd = r + 1;
    return 1;
}
uint32_t ua_events_lost(void) { return ev_lost; }

static void step(unsigned s, unsigned status, uint32_t a, uint32_t b, uint32_t c)
{
    post(EV_STEP, s, status, a, b, c);
}

static void fail(unsigned s, unsigned status, uint32_t a)
{
    fail_step = s;
    fail_status = status;
    barrier();
    state = UA_FAILED;
    post(EV_FAIL, s, status, a, 0, 0);
}

const struct ua_desc *ua_desc(void) { return &desc; }
const struct ua_osinfo *ua_osinfo(void) { return &osi; }
int ua_state(void) { return state; }
int ua_attached(void) { return att_self != NULL; }
static int slot_any_busy(const struct os_mem *m)
{
    unsigned i;
    if (!m)
        return 0;
    for (i = 0; i < UA_NX; i++)
        if (m->slots[i].busy)
            return 1;
    return 0;
}

static int slot_any_parked(const struct os_mem *m)
{
    unsigned i;
    if (!m)
        return 0;
    for (i = 0; i < UA_NX; i++)
        if (m->slots[i].parked)
            return 1;
    return 0;
}

static int slot_any_xfer(const struct os_mem *m)
{
    unsigned i;
    if (!m)
        return 0;
    for (i = 0; i < UA_NX; i++)
        if (m->slots[i].xfer)
            return 1;
    return 0;
}

static int any_busy(void)
{
    return slot_any_busy(mem);
}

static int any_mem_busy(void)
{
    unsigned i;
    if (!mem_pool)
        return 0;
    for (i = 0; i < UA_MEM_SLOTS; i++) {
        const struct os_mem *m = &mem_pool[i];
        if (m->mstate != M_FREE && (m->in_cb || slot_any_busy(m)))
            return 1;
    }
    return 0;
}

static int any_kept_busy(void)
{
    unsigned i;
    if (!mem_pool)
        return 0;
    for (i = 0; i < UA_MEM_SLOTS; i++) {
        const struct os_mem *m = &mem_pool[i];
        if (m->mstate == M_KEPT && (m->in_cb || slot_any_busy(m)))
            return 1;
    }
    return 0;
}

static int has_free_mem(void)
{
    unsigned i;
    if (!mem_pool)
        return 0;
    for (i = 0; i < UA_MEM_SLOTS; i++)
        if (mem_pool[i].mstate == M_FREE)
            return 1;
    return 0;
}

static struct os_mem *alloc_mem(void)
{
    unsigned i, j;
    if (!mem_pool)
        return NULL;
    for (i = 0; i < UA_MEM_SLOTS; i++) {
        struct os_mem *m = &mem_pool[i];
        if (m->mstate != M_FREE)
            continue;
        m->pipe = NULL;
        m->detached = 0;
        m->close_tok = 0;
        m->free_tok = 0;
        m->abort_err = 0;
        m->in_cb = 0;
        for (j = 0; j < UA_NX; j++) {
            memset(&m->slots[j], 0, sizeof m->slots[j]);
            m->slots[j].idx = (uint8_t) j;
            m->slots[j].mem = m;
        }
        m->mstate = M_USED;
        return m;
    }
    return NULL;
}

static void release_mem_slot(struct os_mem *m)
{
    if (!m)
        return;
    if (m == mem)
        mem = NULL;
    m->pipe = NULL;
    m->mstate = M_FREE;
}

static int resources_owned(void)
{
    struct os_mem *m = mem;
    if (m && (m->pipe || slot_any_busy(m) || slot_any_xfer(m)))
        return 1;
    if (!has_free_mem())
        return 1;
    return 0;
}

int ua_owns_usb(void)
{
    struct os_mem *m = mem;
    if (state == UA_OFF)
        return 0;
    if (att_self || callbacks || attaching || any_mem_busy())
        return 1;
    if (m && m->pipe)
        return 1;
    return 0;
}
/* Hub (2026-11-16): a callback of this driver is inside the OS's USB task
 * now (MAIN sees the count only while one is suspended in an OS call: an
 * attach's requests, a detach's abort): it is the OS's time that ends it. */
int ua_busy(void) { return state != UA_OFF && (attaching || callbacks); }
uint32_t ua_seen_probes(void) { return probes; }
void ua_failure(unsigned *s, unsigned *st)
{
    if (s) *s = fail_step;
    if (st) *st = fail_status;
}
void ua_set_volume(unsigned v) { volume = v > 256 ? 256 : v; }
void ua_set_pause(int p) { paused = p ? 1 : 0; }
void ua_dev_fmt(struct dev_fmt *f) { *f = dfmt; }

void ua_counters(struct ua_counters *c)
{
    struct os_mem *m = mem;
    unsigned i, n = 0;
    *c = cnt;
    if (m)
        for (i = 0; i < UA_NX; i++)
            n += m->slots[i].busy;
    c->inflight = n;
}
void ua_counters_reset_min(void) { cnt.min_queued_ms = 0xFFFFFFFFu; }

#ifdef UA_HOST_TEST
static uint8_t frame_off;          /* (tests: ua_set_frame_clock(0) asked for none) */
#endif
void ua_set_frame_clock(uint32_t frindex_addr)
{
    frame_reg = frindex_addr & ~(uintptr_t) 3;
#ifdef UA_HOST_TEST
    frame_off = !frindex_addr;
#endif
}

static int frame_clock(void)
{
#ifdef UA_HOST_TEST
    return ua_fake_frame != NULL && !frame_off;
#else
    return frame_reg != 0;
#endif
}

/* FRINDEX >> 3: the frame the controller is in. USB and MAIN; one read of a
 * register the OS's own driver reads the same way. */
static uint32_t frame_now(void)
{
#ifdef UA_HOST_TEST
    return ua_fake_frame ? ua_fake_frame() % UA_FRAME_WRAP : 0;
#else
    return frame_reg ? (*(volatile const uint32_t *) frame_reg >> 3) % UA_FRAME_WRAP : 0;
#endif
}

/* ua18 (ua_queue_ms): where the OS links a transfer queued now, read from
 * its code (NOTES.txt "One lap late"): right after the last packet linked,
 * or 4 frames from now when that frame has passed (fewer than 4 left); the
 * frame after its last packet. What a completion queues is linked as its
 * soft interrupt returns: exact. What MAIN queues behind a running pipe
 * (ua_start's, requeue_deferred's) is linked at the next soft interrupt,
 * some time before the next completion's: taken as linked now (short), its
 * slack how much later it may be (set at that completion: slack_at_link);
 * what is queued behind it inherits that. ua_queue_not_done takes the
 * slack back as MAIN learns it. The estimate errs short, never long. */
/* A transfer back far from where it was taken to end (before it: the
 * estimate was long; UA_LATE_MS after it, or an end that can't be: a device
 * that stopped taking packets, a frame list lapped, a frame count that went
 * round while it waited) leaves the OS's chain where this can't follow: not
 * known until twice the queue has been queued again. */
static volatile uint8_t q_unsure;
static uint16_t chain_end(const struct os_mem *m, const struct slot *sl, uint32_t now, uint16_t *slack)
{
    unsigned i;
    uint32_t last = 0, d;
    const struct slot *tail = NULL;
    for (i = 0; i < UA_NX; i++) {
        const struct slot *o = &m->slots[i];
        if (o == sl || !o->busy)
            continue;
        d = ((uint32_t) o->t_end - now) % UA_FRAME_WRAP;
        if (d && d < UA_FRAME_WRAP / 2 && (!tail || d > last)) {
            last = d;
            tail = o;
        }
    }
    /* (the OS compares with FRINDEX + 4 frames, 10a0d910: a chain with less
     * than that left has passed, and starts again 4 frames on; one whose
     * end may be later is taken as running on) */
    if (tail && (last >= 4 || tail->slack)) {
        *slack = tail->slack;
        return (uint16_t) ((now + last + UA_NF) % UA_FRAME_WRAP);
    }
    *slack = 0;
    return (uint16_t) ((now + 4u + UA_NF) % UA_FRAME_WRAP);
}

/* USB, a completion (its soft interrupt): what MAIN queued behind the
 * running pipe is linked by now at the latest: where it was taken (right
 * behind the chain), or, if this soft interrupt links it, 4 frames from now
 * at the earliest when the chain has passed: as late as that, and what is
 * queued behind it with it. */
static void slack_at_link(struct os_mem *m, uint32_t now)
{
    unsigned i;
    int32_t first = 0, d;
    int any = 0;
    for (i = 0; i < UA_NX; i++) {
        const struct slot *o = &m->slots[i];
        if (o->busy && o->pending) {
            d = (int32_t) (((uint32_t) o->t_end - UA_NF - now) % UA_FRAME_WRAP);   /* where it was taken to start */
            if (d >= (int32_t) (UA_FRAME_WRAP / 2))
                d -= (int32_t) UA_FRAME_WRAP;
            if (!any++ || d < first)
                first = d;
        }
    }
    for (i = 0; any && i < UA_NX; i++) {
        struct slot *o = &m->slots[i];
        if (o->busy && o->pending) {
            int32_t late = 4 - first;
            o->slack = (uint16_t) (o->slack + (late > 0 ? (late < 1024 ? late : 1024) : 0));
            o->pending = 0;
        }
    }
}

static void second_reset(void)
{
    memset(&sec, 0, sizeof sec);
    sec.turn_min = sec.wait_min = sec.ahead_min = 0xFFFFFFFFu;
}

unsigned ua_first(struct ua_first *out, unsigned max)
{
    unsigned n = nfirst, i;
    if (n > UA_FIRST) n = UA_FIRST;
    if (n > max) n = max;
    for (i = 0; i < n; i++)
        out[i] = first[i];
    return n;
}

const char *ua_state_name(int st)
{
    static const char *const n[] = { "off", "waiting for a device", "setting up", "ready", "streaming",
                                     "failed", "unplugged", "stopped" };
    return st >= 0 && st <= UA_STOPPED ? n[st] : "?";
}

const char *ua_step_name(unsigned s)
{
    static const char *const n[S_COUNT] = {
        "-", "resolve OS functions", "register driver", "device-level probe", "interface probe",
        "attach", "device_get_ivars", "usbd_interface2device_handle", "usbd_get_config_descriptor",
        "usbd_do_request buffer form (GET_DESCRIPTOR device)", "GET_DESCRIPTOR configuration (9 bytes)",
        "GET_DESCRIPTOR configuration (all)", "parse descriptors", "choose format", "usbd_set_interface",
        "SET_INTERFACE request", "SET_CUR sampling frequency", "GET_CUR sampling frequency",
        "feature unit mute off", "feature unit volume range", "feature unit volume set",
        "usbd_open_pipe (isochronous OUT)", "usbd_alloc_xfer", "find usbd_setup_isoc_xfer",
        "transfer layout probe", "usbd_transfer (from main)", "usbd_transfer (from callback)",
        "transfer completion", "no completion", "detach", "usbd_abort_pipe", "usbd_close_pipe", "stop",
        "endpoint size told to the OS",
    };
    return s < S_COUNT ? n[s] : "?";
}

const char *ua_status_name(unsigned s)
{
    static const char *const n[] = {
        "NORMAL_COMPLETION", "NOT_STARTED", "IN_PROGRESS", "NOT_PERMITTED", "NO_ENTRY",
        "IOERROR", "NOT_CONFIGURED", "NOMEM", "IN_USE", "NO_DEVICE", "INVAL", "NOT_SUPPORTED",
        "TIMEOUT", "SUSPENDED", "ERROR", "TEST_FAILED", "INCORRECT_STATE", "STALLED",
        "INVALID_PARAMETER", "CANCELLED", "SHORT_XFER",
    };
    return s < sizeof n / sizeof n[0] ? n[s] : s == 0x100 ? "(OS call missing)" : "(unknown)";
}

const char *ua_errstr(unsigned s)
{
    if (api.errstr && s < USBSHIM_USB_STATUS_COUNT) {
        const char *t = api.errstr(s);
        if (t && (uintptr_t) t >= 0x1000)
            return t;
    }
    return ua_status_name(s);
}

/* ---- resolving (MAIN) ------------------------------------------------------------ */

static int is_push(uint32_t w)
{
    return (w & 0xFFFF0000u) == 0xE92D0000u ||                     /* stmfd sp!, {...} */
           (w & 0xFFFF0FFFu) == 0xE52D0004u;                       /* str rX, [sp, #-4]! */
}

static int is_return(uint32_t w)
{
    return w == 0xE12FFF1Eu ||                                      /* bx lr */
           w == 0xE1A0F00Eu ||                                      /* mov pc, lr */
           (w & 0xFFFF8000u) == 0xE8BD8000u ||                      /* ldmfd sp!, {..., pc} */
           w == 0xE49DF004u;                                        /* ldr pc, [sp], #4 */
}

uint32_t ua_isoc_derive(uint32_t setup, uint32_t status, uint32_t off, uint32_t w0, uint32_t wprev)
{
    if (!setup || (setup & 3) || status - setup != 0x120 || (off != 0xC4 && off != 0xE0))
        return 0;
    /* A function starts there: it saves registers (a leaf like this one may
     * not save lr), or the word before it ends the previous function. */
    if (is_push(w0) || is_return(wprev))
        return setup + off;
    return 0;
}

/* Exact position-independent leaf captured on CX II 6.4. A merely plausible
 * function start is insufficient when the SDK prototype is wrong. */
int ua_isoc_signature(const uint32_t *words, unsigned count)
{
    static const uint32_t expected[] = {
        0xe5802004u, 0xe1dd20b8u, 0xe5801000u, 0xe59d1004u,
        0xe5d0c044u, 0xe3822004u, 0xe1c021b8u, 0xe3a02001u,
        0xe5801038u, 0xe5c02020u, 0xe59d100cu, 0xe59d2000u,
        0xe31c0008u, 0x0580300cu, 0xe3ccc001u, 0xe3a03000u,
        0xe580301cu, 0xe5801024u, 0xe5c0c044u, 0xe5802034u,
        0xe5803010u, 0xe5803014u, 0xe12fff1eu,
    };
    return words && count == sizeof expected / sizeof expected[0] &&
           memcmp(words, expected, sizeof expected) == 0;
}

static uintptr_t resolve_one(unsigned i)
{
#ifdef _TINSPIRE
    uintptr_t a = 0;
    switch (i) {
#define X(id, nr, name, req) case F_##id: if (_nl_hassyscall(nr)) OS_ADDR(nr, a); break;
    FNS(X)
#undef X
    default:
        break;
    }
    return (a & 3) ? 0 : a;
#else
    if (!ua_fake_has || !ua_fake_addr || !ua_fake_has(fn_nr[i]))
        return 0;
    return ua_fake_addr(fn_nr[i]);
#endif
}

#ifdef _TINSPIRE
static uint32_t peek(uintptr_t a)
{
    if (a < 0x10000000u || a >= 0x14000000u || (a & 3))
        return 0;
    return *(volatile const uint32_t *) a;
}
#endif

static int resolve(void)
{
    uintptr_t a[F_COUNT];
    unsigned i;
    for (i = 0; i < F_COUNT; i++) {
        a[i] = resolve_one(i);
        osi.addr[i] = (uint32_t) a[i];
    }
    osi.isoc_how = a[F_SETUP_ISOC] ? 1 : 0;
#ifdef _TINSPIRE
    if (!a[F_SETUP_ISOC] && a[F_SETUP_XFER] && a[F_XFER_STATUS]) {
        static const uint32_t offs[2] = { 0xC4, 0xE0 };    /* CX 4.5 / CX II 5.x tables; CX 4.0-4.4 tables */
        for (i = 0; i < 2 && !a[F_SETUP_ISOC]; i++) {
            uint32_t c = ua_isoc_derive((uint32_t) a[F_SETUP_XFER], (uint32_t) a[F_XFER_STATUS], offs[i],
                                        peek(a[F_SETUP_XFER] + offs[i]), peek(a[F_SETUP_XFER] + offs[i] - 4));
            if (c) {
                a[F_SETUP_ISOC] = c;
                osi.addr[F_SETUP_ISOC] = c;
                osi.isoc_how = 2;
            }
        }
    }
    {
        uintptr_t at = a[F_SETUP_ISOC] ? a[F_SETUP_ISOC] : (a[F_SETUP_XFER] ? a[F_SETUP_XFER] + 0xC4 : 0);
        if (at) {
            for (i = 0; i < 4; i++)
                osi.isoc_words[i] = peek(at + 4 * i);
            osi.word_before = peek(at - 4);
        }
    }
    if (a[F_SETUP_ISOC]) {
        uint32_t words[23];
        for (i = 0; i < 23; i++)
            words[i] = peek(a[F_SETUP_ISOC] + 4 * i);
        if (!ua_isoc_signature(words, 23)) {
            a[F_SETUP_ISOC] = 0; /* unsupported ABI: do not call it */
            osi.isoc_how = 0;
        }
    }
#endif
    for (i = 0; i < F_COUNT; i++)
        if (fn_req[i] && !a[i]) {
            step(S_RESOLVE, 0x100, fn_nr[i], i, 0);
            init_text = "the OS lacks a USB call this program needs";
            return 0;
        }
#define BIND(field, id) api.field = (__typeof__(api.field)) a[F_##id]
    BIND(ivars, IVARS);
    BIND(iface_desc, IFACE_DESC);
    BIND(config_desc, CONFIG_DESC);
    BIND(dev_desc, DEV_DESC);
    BIND(do_request, DO_REQUEST);
    BIND(iface2dev, IFACE2DEV);
    BIND(set_iface, SET_IFACE);
    BIND(open_pipe, OPEN_PIPE);
    BIND(close_pipe, CLOSE_PIPE);
    BIND(abort_pipe, ABORT_PIPE);
    BIND(alloc_xfer, ALLOC_XFER);
    BIND(free_xfer, FREE_XFER);
    BIND(setup_xfer, SETUP_XFER);
    BIND(setup_isoc, SETUP_ISOC);
    BIND(xfer_status, XFER_STATUS);
    BIND(transfer, TRANSFER);
    BIND(errstr, ERRSTR);
#undef BIND
    if (!usbshim_usb_status_abi(api.errstr)) {
        step(S_RESOLVE, 0x101, 254, F_ERRSTR, 0);
        init_text = "the OS's USB status ABI is not the verified TI convention";
        return 0;
    }
    return 1;
}

/* ---- control requests (USB task, from attach) ------------------------------------ */

static void *ctl_data(unsigned len)
{
    if (!len || !mem)
        return NULL;
    mem->ctl_desc.buf = mem->ctl + 512;
    mem->ctl_desc.zero1 = mem->ctl_desc.zero2 = 0;
    return osi.ctl_conv == 2 ? (void *) &mem->ctl_desc : (void *) mem->ctl;
}
static uint8_t *ctl_bytes(void) { return osi.ctl_conv == 2 ? mem->ctl + 512 : mem->ctl; }

/* type/req/val/idx as in the setup packet; data in/out through ctl_bytes()
 * (len <= 512). Returns the usbd_status (SHORT_XFER on reads counts as ok). */
static unsigned control(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, uint16_t len)
{
    unsigned s;
    if (closing || stopping)
        return OS_CANCELLED;
    if (!mem)
        return OS_INVAL;
    if (len && osi.ctl_conv == 0)
        return 0x100;               /* data stage with an unknown buffer form: never */
    if (len > 512)
        return OS_INVAL;
    mem->req[0] = type;
    mem->req[1] = req;
    mem->req[2] = (uint8_t) val;
    mem->req[3] = (uint8_t) (val >> 8);
    mem->req[4] = (uint8_t) idx;
    mem->req[5] = (uint8_t) (idx >> 8);
    mem->req[6] = (uint8_t) len;
    mem->req[7] = (uint8_t) (len >> 8);
    if (type & 0x80)
        memset(ctl_bytes(), 0, len);
    s = ST(api.do_request(att_dev, mem->req, ctl_data(len)));
    if (s == OS_SHORT && (type & 0x80))
        s = OS_OK;
    return s;
}

/* nspire-usbpad's probe: the buffer starts with a TI descriptor pointing 512
 * bytes on; a plain-pointer OS writes the 18 bytes at +0, a descriptor OS at
 * +512. Both stay inside our buffer. */
static void calibrate(void)
{
    struct ti_buf *d = (struct ti_buf *) (void *) mem->ctl;
    const uint8_t *raw = mem->ctl, *far = mem->ctl + 512;
    unsigned s;
    if (closing || stopping)
        return;
    memset(mem->ctl, 0, CTL_BUF);
    d->buf = mem->ctl + 512;
    d->zero1 = d->zero2 = 0;
    memcpy(mem->req, "\x80\x06\x00\x01\x00\x00\x12\x00", 8);
    s = ST(api.do_request(att_dev, mem->req, mem->ctl));
    if (s == OS_OK || s == OS_SHORT) {
        if (raw[0] == 18 && raw[1] == 1 && (raw[8] | raw[9] << 8) == desc.vid)
            osi.ctl_conv = 1;
        else if (far[0] == 18 && far[1] == 1 && (far[8] | far[9] << 8) == desc.vid)
            osi.ctl_conv = 2;
    }
    step(S_CALIBRATE, s, osi.ctl_conv, 0, 0);
    if (osi.ctl_conv && !desc.have_dev) {
        memcpy(desc.dev, osi.ctl_conv == 2 ? far : raw, 18);
        desc.have_dev = 1;
    }
}

static void snapshot_os_cfg(void)
{
    const uint8_t *cfg = api.config_desc(att_dev);
    unsigned len = 0;
    if (cfg && cfg[0] >= 9 && cfg[1] == 2) {
        len = (unsigned) (cfg[2] | cfg[3] << 8);
        if (len > sizeof desc.cfg_os)
            len = sizeof desc.cfg_os;
        memcpy(desc.cfg_os, cfg, len);
    }
    desc.cfg_os_len = (uint16_t) len;
    step(S_CONFIG_DESC, cfg ? 0 : 0x100, len, cfg ? 1 : 0, 0);
}

/* The one write this driver makes into the OS's own data: two bytes.
 *
 * The OS takes a full-speed OUT descriptor's T-count from the packet's
 * length ((len + 187) / 188, 10a0de4c) but its S-mask from a table entry
 * picked by wMaxPacketSize / 188 of the pipe's endpoint descriptor
 * (10a0de28..10a0de48), read again for every packet. The CM108 says 200
 * for an alternate setting that serves 48 kHz (192 bytes) and 44.1 kHz
 * (176/180): entry 1, presumably two micro-frames, although a 180-byte
 * packet needs one start-split. Whether the CX II's controller minds is
 * not known (NOTES.txt, 2026-11-14); in case it does, wMaxPacketSize in
 * the OS's copy of the configuration descriptor is lowered to the largest
 * packet of the chosen rate before the pipe is opened: entry 0.
 *
 * Only inside the buffer usbd_get_config_descriptor returns, within the
 * length that descriptor states, only the chosen alternate setting's
 * endpoint, only downwards, never below a packet we send. It is not put
 * back: the copy dies with the device at unplug, the OS has no use for
 * this endpoint, and this program finds 44.1 kHz again from the device's
 * own descriptors. That the OS's pipes read this copy is an assumption
 * (the BSD stack this one comes from does); the event says what was found. */
static void fit_os_ep_maxpkt(os_dev dev, const struct uac_fmt *f, uint32_t rate)
{
    uint8_t *cfg = (uint8_t *) (uintptr_t) api.config_desc(dev);
    unsigned want = uac_max_packet(f, rate), len, off = 0, in_alt = 0, was = 0, now = 0, found = 0;
#ifdef _TINSPIRE
    if ((uintptr_t) cfg < 0x10000000u || (uintptr_t) cfg >= 0x14000000u - sizeof desc.cfg_os)
        cfg = NULL;
#endif
    if (cfg && want && cfg[0] >= 9 && cfg[1] == 2) {
        len = (unsigned) (cfg[2] | cfg[3] << 8);
        if (len > sizeof desc.cfg_os)
            len = sizeof desc.cfg_os;
        while (off + 2 <= len && cfg[off] >= 2 && off + cfg[off] <= len) {
            uint8_t *d = cfg + off;
            if (d[1] == 4 && d[0] >= 9)
                in_alt = d[2] == f->ifnum && d[3] == f->alt;
            else if (in_alt && d[1] == 5 && d[0] >= 7 && d[2] == f->ep_addr) {
                unsigned cur = (unsigned) (d[4] | d[5] << 8);
                found = 1;
                was = now = cur & 0x7FFu;
                if (was > want) {
                    unsigned patched = (cur & ~0x7FFu) | want;
                    d[4] = (uint8_t) patched;
                    d[5] = (uint8_t) (patched >> 8);
                    now = want;
                }
                break;
            }
            off += d[0];
        }
    }
    step(S_EP_SIZE, found ? 0 : 4, was, now, want);
}

static void fetch_cfg(void)
{
    unsigned s, total;
    desc.cfg_dev_len = 0;
    s = control(0x80, 6, 0x0200, 0, 9);
    step(S_GET_CFG9, s, 0, 0, 0);
    if (s != OS_OK)
        return;
    total = ctl_bytes()[2] | ctl_bytes()[3] << 8;
    if (total < 9)
        return;
    if (total > 512)
        total = 512;                /* control() limit; dumps say if cut */
    s = control(0x80, 6, 0x0200, 0, (uint16_t) total);
    step(S_GET_CFG, s, total, 0, 0);
    if (s != OS_OK)
        return;
    memcpy(desc.cfg_dev, ctl_bytes(), total);
    desc.cfg_dev_len = (uint16_t) total;
}

static void set_rate(const struct uac_fmt *f)
{
    unsigned s;
    uint8_t *b;
    desc.rate_dev = 0;
    if (!(f->have_cs_ep && (f->cs_ep_attr & 1)) && !(f->rate_continuous || f->nrates > 1)) {
        step(S_SET_RATE, 0, desc.rate, 0, 1);   /* one fixed rate: nothing to set (c = 1) */
        return;
    }
    b = ctl_bytes();
    b[0] = (uint8_t) desc.rate;
    b[1] = (uint8_t) (desc.rate >> 8);
    b[2] = (uint8_t) (desc.rate >> 16);
    s = control(0x22, 0x01, 0x0100, f->ep_addr, 3);
    step(S_SET_RATE, s, desc.rate, f->ep_addr, 0);
    s = control(0xA2, 0x81, 0x0100, f->ep_addr, 3);
    if (s == OS_OK) {
        b = ctl_bytes();
        desc.rate_dev = b[0] | (uint32_t) b[1] << 8 | (uint32_t) b[2] << 16;
    }
    step(S_GET_RATE, s, desc.rate_dev, 0, 0);
}

static int16_t s16(const uint8_t *b) { return (int16_t) (b[0] | b[1] << 8); }

static void feature_unit(const struct uac_fmt *f)
{
    const struct uac_unit *fu = uac_feature_unit(&desc.info, f);
    uint16_t idx;
    unsigned s, ch, chans[2], nch = 0, i;
    uint8_t *b;
    desc.fu_id = -1;
    if (!fu || desc.info.ac_ifnum == 0xFF) {
        step(S_FU_MUTE, 0, 0, 0, 1);    /* c = 1: no feature unit */
        return;
    }
    desc.fu_id = (int8_t) fu->id;
    idx = (uint16_t) (fu->id << 8 | desc.info.ac_ifnum);
    /* mute: master if it has one, else channels 1 and 2 */
    if (fu->ctl_master & 1) chans[nch++] = 0;
    else {
        if (fu->ctl_ch1 & 1) chans[nch++] = 1;
        if (fu->ctl_ch2 & 1) chans[nch++] = 2;
    }
    for (i = 0; i < nch; i++) {
        ctl_bytes()[0] = 0;
        s = control(0x21, 0x01, (uint16_t) (0x0100 | chans[i]), idx, 1);
        step(S_FU_MUTE, s, fu->id, chans[i], 0);
    }
    nch = 0;
    if (fu->ctl_master & 2) chans[nch++] = 0;
    else {
        if (fu->ctl_ch1 & 2) chans[nch++] = 1;
        if (fu->ctl_ch2 & 2) chans[nch++] = 2;
    }
    if (!nch)
        return;
    ch = chans[0];
    s = control(0xA1, 0x82, (uint16_t) (0x0200 | ch), idx, 2);    /* GET_MIN */
    desc.vol_min = s == OS_OK ? s16(ctl_bytes()) : (int16_t) -0x3000;
    if (s == OS_OK) {
        s = control(0xA1, 0x83, (uint16_t) (0x0200 | ch), idx, 2);    /* GET_MAX */
        desc.vol_max = s == OS_OK ? s16(ctl_bytes()) : 0;
    }
    step(S_FU_VOL_RANGE, s, (uint32_t) (uint16_t) desc.vol_min, (uint32_t) (uint16_t) desc.vol_max, ch);
    if (s != OS_OK) {
        desc.vol_min = -0x3000;     /* -48 dB .. 0 dB */
        desc.vol_max = 0;
    }
    /* moderate: 10 dB under the maximum, not under the minimum */
    {
        int32_t v = (int32_t) desc.vol_max - 0x0A00;
        if (v < desc.vol_min) v = desc.vol_min;
        if (v > desc.vol_max) v = desc.vol_max;
        desc.vol_set = (int16_t) v;
    }
    for (i = 0; i < nch; i++) {
        b = ctl_bytes();
        b[0] = (uint8_t) desc.vol_set;
        b[1] = (uint8_t) ((uint16_t) desc.vol_set >> 8);
        s = control(0x21, 0x01, (uint16_t) (0x0200 | chans[i]), idx, 2);
        step(S_FU_VOL_SET, s, (uint32_t) (uint16_t) desc.vol_set, chans[i], 0);
    }
}

/* ---- callbacks (USB) ---------------------------------------------------------------- */

/* TI's mark on an aborted transfer (2026-11-16; read from the CX II's OS
 * 6.4.0.74 code, and the same in its copy of the older CX's host controller
 * driver: 10a0b630 / 10a0b958).
 *
 * The host controller driver's abort of an ISOCHRONOUS transfer sets bit 0
 * of byte 84 of the transfer's block ("being aborted": 10a0f578-10a0f588)
 * and never clears it; only the next isochronous submission of that block
 * does (10a0f8b0). usbd_free_xfer hands the block back to the controller's
 * pool as it is (10a0d358) and usbd_alloc_xfer hands it out again as it is
 * (10a0f84c): to the next control request, a controller's interrupt pipe, a
 * hub's status pipe, a drive's bulk transfer. Nothing happens until THAT
 * transfer is aborted (a device pulled out, a request's 5 s limit): the
 * abort finds bit 0 set, takes it for an abort already under way, and waits
 * for its end, which never comes (10a10908-10a1093c: jmsleep(xfer + 96, 0)
 * while the bit is set). The OS task that called it is gone for good: the
 * one that explores the port (nothing is offered ever again; the session's
 * "USB problem" until the adapter comes out, the calculator frozen after
 * the exit), or the one that ends requests at their time limit (a request
 * that fails then never returns: R12's "do_request never returned").
 *
 * Every unplug of the sound adapter, and every stop that aborts, left up to
 * UA_NX such blocks in the OS's pool, for the rest of the OS's uptime. So:
 *  - a stop that can wait lets the queued transfers play out and aborts
 *    nothing (ua_stop);
 *  - where an abort is needed (the device is gone, or a transfer did not
 *    come back), the byte is read before the abort and after it, and where
 *    it was 0 while the transfer was queued and reads 1 after (bit 1, "a
 *    second abort waits", allowed) it is put back to 0 before the block is
 *    freed. That is the mark and nothing else: a block whose byte reads
 *    anything else is another OS's and is freed as it is (counted).
 * The byte is 84 on the calculator; the PC tests' fake OS says where its
 * transfers keep theirs. */
#ifdef UA_HOST_TEST
uint8_t *ua_fake_xfer_mark(void *xfer);
#define XFER_MARK(x) ((volatile uint8_t *) ua_fake_xfer_mark(x))
#define xfer_readable(x) ((x) != NULL)
#else
#define XFER_MARK(x) ((volatile uint8_t *) (x) + 84)
#define xfer_readable(x) ((uintptr_t) (x) >= 0x10000000u && (uintptr_t) (x) < 0x14000000u - 136u && \
                          !((uintptr_t) (x) & 3u))
#endif

/* Before the pipe's abort: which transfers the OS has, and their byte. */
static void mark_note(struct os_mem *m)
{
    unsigned i;
    for (i = 0; i < UA_NX; i++) {
        struct slot *sl = &m->slots[i];
        sl->aborted = sl->xfer && sl->busy;
        sl->mark_pre = sl->aborted && xfer_readable(sl->xfer) ? *XFER_MARK(sl->xfer) : 0xFF;
    }
}

/* Before the block is freed: the abort's mark off it. */
static void mark_off(struct slot *sl)
{
    volatile uint8_t *b;
    if (!sl->xfer || !sl->aborted || !xfer_readable(sl->xfer))
        return;
    sl->aborted = 0;
    b = XFER_MARK(sl->xfer);
    if (sl->mark_pre == 0 && (*b & 0xFDu) == 1) {
        *b = 0;
        osi.marks_off++;
    } else if (*b != sl->mark_pre) {
        osi.marks_odd++;            /* not TI's mark as it was read: nothing written */
    }
}

/* Free the transfers and close the pipe, once: MAIN (stop) and USB (detach)
 * may both try, m->free_tok picks one. Returns 1 when no pipe/xfer remains. */
static int free_all_mem(struct os_mem *m, int from_detach)
{
    unsigned i;
    if (!m)
        return 1;
    /* Once detach returned 0 to TI's OS, never call close_pipe or free_xfer
     * from MAIN: TI has torn down device/iface. */
    if (m->detached && !from_detach)
        return 0;
    /* An abort return or elapsed timeout does not return DMA ownership.
     * Only the completion callbacks clear busy. */
    if (m->in_cb || slot_any_busy(m) || m->abort_err)
        return 0;
    if (xchg8(&m->free_tok, 1))
        return !m->pipe && !slot_any_xfer(m);
    if (m->in_cb || slot_any_busy(m)) {
        m->free_tok = 0;
        return 0;
    }
    for (i = 0; i < UA_NX; i++) {
        if (m->slots[i].xfer) {
            mark_off(&m->slots[i]);     /* TI's abort mark never goes back into its pool */
            api.free_xfer(m->slots[i].xfer);
        }
        m->slots[i].xfer = NULL;
        m->slots[i].busy = 0;
    }
    if (m->pipe) {
        unsigned s = ST(api.close_pipe(m->pipe));
        step(S_CLOSE, s, 0, 0, 0);
        if (s != OS_OK) {
            /* Inside detach a failed close_pipe must never be retried (and on
             * TI OS usbd_close_pipe decrements refcnt unconditionally); quarantine
             * the block as KEPT. While still attached (from_detach == 0), allow
             * ua_stop() retry when no transfers are queued. */
            if (!from_detach)
                m->free_tok = 0;
            return 0;
        }
        m->pipe = NULL;
    }
    return 1;
}

/* Abort the pipe; MAIN (stop) and USB (detach) may race: m->close_tok decides. */
static int close_stream_mem(struct os_mem *m, int from_detach)
{
    if (!m || (m->detached && !from_detach))
        return 0;
    if (xchg8(&m->close_tok, 1))
        return 0;
    stopping = 1;
    barrier();
    if (m->pipe) {
        unsigned s;
        mark_note(m);
        s = ST(api.abort_pipe(m->pipe));
        step(S_ABORT, s, 0, 0, 0);
        if (s != OS_OK) {
            m->abort_err = 1;
            if (!from_detach)
                m->close_tok = 0;   /* a later shutdown attempt while attached may retry */
        } else {
            m->abort_err = 0;
        }
    }
    return 1;
}

static int match_body(device_t self)
{
    const struct os_attach_arg *u = api.ivars(self);
    const uint8_t *id;
    probes++;
    if (!up || closing || !u)
        return 0;
    if (!u->iface) {
        const uint8_t *dd = (u->device && api.dev_desc) ? api.dev_desc(u->device) : NULL;
        step(S_MATCH_DEV, 0, (uint32_t) (uint16_t) u->vendor << 16 | (uint16_t) u->product,
             dd && dd[1] == 1 ? dd[4] : 0xFFu, 0);
        return 0;
    }
    id = api.iface_desc(u->iface);
    if (!id || id[0] < 9 || id[1] != 4)
        return 0;
    step(S_MATCH_IF, 0, (uint32_t) (uint16_t) u->vendor << 16 | (uint16_t) u->product,
         (uint32_t) id[2] << 24 | (uint32_t) id[5] << 16 | id[6] << 8 | id[7], 0);
    if ((uint16_t) u->vendor == VID_TI || id[5] != 1 || id[6] != 2)
        return 0;
    if (state != UA_WAITING && state != UA_GONE && state != UA_FAILED)
        return 0;                   /* one device at a time */
    if (resources_owned())
        return 0;                   /* an old device still owns transfer memory */
    if (state == UA_FAILED && att_dev && u->device == att_dev && desc.vid == (uint16_t) u->vendor &&
        desc.pid == (uint16_t) u->product)
        return 0;                   /* this device already failed: its other interfaces too */
    matched_self = self;
    return MATCH_SCORE;
}

/* gpSP's CX II run of 2026-11-15 (R11u): after a match that returned
 * nonzero, one attach that returns an error makes TI's OS offer no further
 * device until the whole adapter is pulled (the pad driver's error on the
 * sound card's HID interface: no controller for the rest of the session).
 * So an attach this driver matched never fails: what it cannot use it
 * keeps quietly (nothing taken, att_self not set; detach of it is a no-op).
 * Only an attach the OS calls without our match (a stale or a forced one,
 * tests/app_test.c test_retained_resources_block_offers) is refused. */
static int attach_refuse(device_t self)
{
    if (self != matched_self)
        return OS_ENXIO;
    matched_self = NULL;
    step(S_ATTACH, OS_ENXIO, 0, 0, 2);   /* c = 2: matched, kept quietly unused */
    return 0;
}

static int os_cb_match(device_t self)
{
    int r;
    callbacks++;
    barrier();
    r = match_body(self);
    barrier();
    callbacks--;
    return r;
}

static int attach_body(device_t self)
{
    const struct os_attach_arg *u = api.ivars(self);
    const uint8_t *id;
    const struct uac_fmt *f;
    os_dev dev = NULL;
    struct os_mem *m;
    const uint8_t *cfg;
    unsigned cfg_len, s, i;
    int same_dev;

    if (!up || closing || !u || !u->iface)
        return attach_refuse(self);
    /* Recheck independently of match: another offer or a failed close
     * can still own these slots by the time the OS calls attach. */
    if ((state != UA_WAITING && state != UA_GONE && state != UA_FAILED) || resources_owned())
        return attach_refuse(self);
    id = api.iface_desc(u->iface);
    if (!id || id[1] != 4)
        return attach_refuse(self);
    if (!api.iface2dev || ST(api.iface2dev(u->iface, &dev)) != OS_OK || !dev)
        dev = u->device;
    if (!dev)
        return attach_refuse(self);
#ifdef HAVE_XFER_POOL
    /* Marks an earlier abort left in the OS's pool (an older build of this
     * driver, in this OS's uptime): off before this device's requests and
     * transfers take their blocks (usbshim_xfer.h). */
    {
        int n = usbshim_xfer_pool_clean(dev);
        if (n > 0)
            osi.marks_off += (uint32_t) n;
    }
#endif
    /* Release an empty idle block kept only across a UA_FAILED whose stream
     * resources were already freed via ua_stop(). */
    if (mem && !mem->pipe && !slot_any_busy(mem) && !slot_any_xfer(mem))
        release_mem_slot(mem);
    m = alloc_mem();
    if (!m)
        return attach_refuse(self);
    if (self == matched_self)
        matched_self = NULL;
    same_dev = dev == att_dev && desc.vid == (uint16_t) u->vendor && desc.pid == (uint16_t) u->product;
    state = UA_SETUP;
    mem = m;
    mem_gen++;
    att_self = self;
    att_dev = dev;
    att_iface = u->iface;
    stopping = 0;
    submitted_all = 0;
    first_err_sent = 0;
    step(S_ATTACH, 0, (uint32_t) (uint16_t) u->vendor << 16 | (uint16_t) u->product, id[2], same_dev);

    if (!same_dev) {
        memset(&desc, 0, sizeof desc);
        desc.vid = (uint16_t) u->vendor;
        desc.pid = (uint16_t) u->product;
        desc.chosen = -1;
        if (api.dev_desc) {
            const uint8_t *dd = api.dev_desc(dev);
            if (dd && dd[0] >= 18 && dd[1] == 1) {
                memcpy(desc.dev, dd, 18);
                desc.have_dev = 1;
            }
        }
        snapshot_os_cfg();
        if (!osi.ctl_conv)
            calibrate();
        if (osi.ctl_conv)
            fetch_cfg();
    }
    desc.ifnum = id[2];
    /* Parse the fuller of the two copies. */
    cfg = desc.cfg_dev_len >= desc.cfg_os_len ? desc.cfg_dev : desc.cfg_os;
    cfg_len = desc.cfg_dev_len >= desc.cfg_os_len ? desc.cfg_dev_len : desc.cfg_os_len;
    s = (unsigned) uac_parse(cfg, cfg_len, &desc.info) & 0xFF;
    step(S_PARSE, s ? OS_INVAL : 0, cfg_len, desc.info.nfmt, desc.info.any_uac2);
    desc.chosen = uac_choose(&desc.info, &desc.rate, &desc.why);
    if (!same_dev)
        post(EV_DESC, 0, 0, 0, 0, 0);
    if (desc.chosen < 0) {
        step(S_CHOOSE, OS_INVAL, 0, 0, desc.info.any_uac2);
        fail(S_CHOOSE, OS_INVAL, 0);
        /* Retain this recognised interface quietly until physical detach
         * so its cleanup keeps a matching lifecycle callback. */
        return 0;
    }
    f = &desc.info.fmt[desc.chosen];
    if (f->ifnum != id[2]) {
        step(S_CHOOSE, 0, f->ifnum, id[2], 2);    /* c = 2: not this interface */
        release_mem_slot(m);
        att_self = NULL;
        state = UA_WAITING;
        return 0;                  /* no resources; the playback interface follows */
    }
    step(S_CHOOSE, 0, desc.rate, (uint32_t) f->alt << 8 | f->ifnum, desc.chosen);
    if (!osi.ctl_conv) {
        fail(S_CALIBRATE, OS_IOERROR, 0);
        return 0;                   /* ours, so detach still comes */
    }
    if (closing || stopping)
        return 0;

    /* alternate setting: the OS's call refreshes its endpoint list */
    if (api.set_iface) {
        s = ST(api.set_iface(u->iface, f->alt));
        step(S_SET_IFACE, s, f->alt, f->ifnum, 0);
    } else {
        s = control(0x01, 0x0B, f->alt, f->ifnum, 0);
        step(S_SET_IFACE_REQ, s, f->alt, f->ifnum, 0);
    }
    if (s != OS_OK) {
        fail(api.set_iface ? S_SET_IFACE : S_SET_IFACE_REQ, s, f->alt);
        return 0;
    }
    if (closing || stopping)
        return 0;
    set_rate(f);
    feature_unit(f);
    if (closing || stopping)
        return 0;

    dfmt.channels = f->channels;
    dfmt.subframe = f->subframe;
    dfmt.unsigned8 = f->format_tag == 2;
    bytes_per_frame = (unsigned) f->channels * f->subframe;

    fit_os_ep_maxpkt(dev, f, desc.rate);
    m->pipe = NULL;
    s = ST(api.open_pipe(u->iface, f->ep_addr, OS_EXCLUSIVE, &m->pipe));
    step(S_OPEN_PIPE, s, f->ep_addr, (uint32_t) (uintptr_t) m->pipe, f->ep_maxpkt);
    if (s != OS_OK || !m->pipe) {
        m->pipe = NULL;
        fail(S_OPEN_PIPE, s ? s : OS_INVAL, f->ep_addr);
        return 0;
    }
    for (i = 0; i < UA_NX; i++) {
        if (closing || stopping)
            return 0;
        m->slots[i].idx = (uint8_t) i;
        m->slots[i].busy = m->slots[i].deferred = 0;
        m->slots[i].mem = m;
        m->slots[i].xfer = api.alloc_xfer(dev);
        if (!m->slots[i].xfer) {
            step(S_ALLOC_XFER, OS_NOMEM, i, 0, 0);
            free_all_mem(m, 0);
            fail(S_ALLOC_XFER, OS_NOMEM, i);
            return 0;
        }
    }
    step(S_ALLOC_XFER, 0, UA_NX, 0, 0);
    if (closing || stopping)
        return 0;
    barrier();
    state = UA_READY;
    return 0;
}

static int os_cb_attach(device_t self)
{
    int r;
    callbacks++;
    attaching++;
    barrier();
    r = attach_body(self);
    barrier();
    attaching--;
    callbacks--;
    return r;
}

static int detach_body(device_t self)
{
    struct os_mem *m;
    if (self != att_self || !att_self)
        return 0;
    step(S_DETACH, 0, (uint32_t) state, 0, 0);
    mem_gen++;
    stopping = 1;
    idle_req = fade_out = fade_in = 0;  /* the next device starts streaming unless idle is asked again */
    barrier();
    m = mem;
    if (m) {
        close_stream_mem(m, 1);     /* aborts (cancelled callbacks run now) unless MAIN did */
        if (free_all_mem(m, 1)) {   /* close pipe + free xfers before detach returns 0 */
            m->detached = 1;
            release_mem_slot(m);
        } else {
            /* Quarantine any unclosed pipe/xfers until late callbacks land and
             * never call abort_pipe/close_pipe again once detach returns 0. */
            m->detached = 1;
            m->mstate = M_KEPT;
            if (m == mem)
                mem = NULL;
        }
    }
    att_self = NULL;
    att_dev = NULL;                 /* a new device must refresh its descriptors */
    att_iface = NULL;
    if (self == matched_self)
        matched_self = NULL;
    barrier();
    state = UA_GONE;
    return 0;
}

static int os_cb_detach(device_t self)
{
    int r;
    callbacks++;
    barrier();
    r = detach_body(self);
    barrier();
    callbacks--;
    return r;
}

/* Fill slot k's packets from the ring (or silence). Callers keep it to one
 * context at a time (see ua_start). primer: packets of silence that leave
 * the ring alone. */
static uint32_t refill(struct slot *sl, int16_t *lr, int primer)
{
    struct os_mem *m = sl->mem;
    uint8_t *out = m->pkt[sl->idx];
    uint16_t *fl = m->frlen[sl->idx];
    uint32_t bytes = 0;
    unsigned f;
    int fade = sl->fade;
    sl->fade = 0;
    sl->bytes = 0;
    if (!ring || dfmt.channels < 1 || dfmt.channels > 8 || dfmt.subframe < 1 || dfmt.subframe > 4 ||
        bytes_per_frame != (unsigned) dfmt.channels * dfmt.subframe)
        return 0;
    /* Even while paused the consumer acknowledges discard requests. MAIN
     * never resets rd or overwrites frames the USB task might be reading. */
    if (paused)
        (void)ring_get(ring, NULL, 0);
    /* Idle (ua_set_idle): the last transfer before the gap fades out, the
     * first UA_NF packets of audio after it fade in (the first silent), one
     * step per packet (1 ms); a packet the ring left empty is no step. */
    for (f = 0; f < UA_NF; f++) {
        unsigned n = pkt_next(&sched), got = 0, b;
        unsigned vol = fade < 0 ? volume * (UA_NF - 1 - f) / UA_NF : volume;
        /* Selection already enforces this, but no descriptor or future
         * caller may write beyond the PCM scratch or this DMA slot. */
        if (n > 200 || n > UA_PKT_MAX / bytes_per_frame ||
            n * bytes_per_frame > sizeof m->pkt[0] - bytes)
            return 0;
        if (!paused && !primer) {
            got = ring_get(ring, lr, n);
            if (got < n) {
                cnt.underrun_pkts++;
                cnt.underrun_frames += n - got;
                sec.silence_ms++;
            }
            if (got && fade_in && fade >= 0) {
                vol = volume * fade_pos / UA_NF;
                if (++fade_pos >= UA_NF)
                    fade_in = 0;
            }
        }
        b = pcm_write(&dfmt, lr, got, vol, out + bytes);
        b += pcm_silence(&dfmt, n - got, out + bytes + b);
        fl[f] = (uint16_t) b;
        bytes += b;
    }
    sl->bytes = bytes;
    return bytes;
}

static void *buf_arg(const struct slot *sl)
{
    struct os_mem *m = sl->mem;
    m->pkt_desc[sl->idx].buf = m->pkt[sl->idx];
    m->pkt_desc[sl->idx].zero1 = m->pkt_desc[sl->idx].zero2 = 0;
    return &m->pkt_desc[sl->idx];
}

static void cb_isoc(os_xfer xfer, void *priv, unsigned status);

static void setup_slot(struct slot *sl)
{
    struct os_mem *m = sl->mem;
    api.setup_isoc(sl->xfer, m->pipe, sl, buf_arg(sl), m->frlen[sl->idx],
                   UA_NF, 0, completion);
}

static unsigned submit(struct slot *sl)
{
    struct os_mem *m = sl->mem;
    unsigned s, i, ahead = 0;
    for (i = 0; i < UA_NX; i++)
        if (&m->slots[i] != sl && m->slots[i].busy)
            ahead += UA_NF;
    sl->ahead = (uint16_t) ahead;
    sl->t_queued = (uint16_t) frame_now();
    sl->t_end = chain_end(m, sl, sl->t_queued, &sl->slack);
    sl->pending = 0;                /* (the soft interrupt links it after this callback) */
    if (q_unsure)
        q_unsure--;
    sl->busy = 1;
    barrier();
    s = ST(api.transfer(sl->xfer));
    if (s != OS_OK && s != OS_INPROG) {
        sl->busy = 0;
        return s;
    }
    cnt.queued_frames += UA_NF;
    sec.queued_ms += UA_NF;
    return OS_OK;
}

static void first_error(unsigned st_step, unsigned status, unsigned slot)
{
    if (first_err_sent)
        return;
    first_err_sent = 1;
    post(EV_FIRST_ERR, st_step, status, slot, cnt.completions, 0);
}

static void requeue(struct slot *sl)
{
    struct os_mem *m = sl->mem;
    unsigned s;
    if (stopping || closing || !m || m != mem || m->detached || state != UA_STREAMING)
        return;
#ifdef UA_HOST_TEST
    /* MAIN can preempt here, after the callback cleared busy and passed
     * its stop checks but before it uses the transfer again. */
    if (ua_fake_before_requeue)
        ua_fake_before_requeue();
#endif
    if (stopping || closing || !m || m != mem || m->detached || state != UA_STREAMING)
        return;
    if (!refill(sl, tmp_lr, 0)) {
        cnt.requeue_fail++;
        cnt.errors++;
        first_error(S_TRANSFER_CB, OS_INVAL, sl->idx);
        fail(S_TRANSFER_CB, OS_INVAL, sl->idx);
        return;
    }
    setup_slot(sl);
    s = submit(sl);
    if (s != OS_OK) {
        cnt.requeue_fail++;
        cnt.errors++;
        first_error(S_TRANSFER_CB, s, sl->idx);
    }
}

static void isoc_body(os_xfer xfer, void *priv, unsigned status)
{
    struct slot *sl = priv;
    struct os_mem *m;
    unsigned i, others = 0, st = ST(status), took;
    int ramp;
    (void) xfer;
    if (!mem_pool || !sl)
        return;
    m = sl->mem;
    if (!m || m < mem_pool || m >= mem_pool + UA_MEM_SLOTS)
        return;
    if (sl->idx >= UA_NX || &m->slots[sl->idx] != sl)
        return;
    m->in_cb++;
    barrier();
    if (m->detached || m != mem) {
        if (st == OS_CANCELLED)
            cnt.cancelled++;
        barrier();
        sl->busy = 0;
        barrier();
        m->in_cb--;
        return;
    }
    cnt.completions++;
    sec.completions++;
    /* Draining into idle, or filling up again after it: a short queue is
     * meant, not MAIN away (no starved/restart/least-ahead count). */
    ramp = idle_req || slot_any_parked(m);
    for (i = 0; i < UA_NX; i++)
        if (&m->slots[i] != sl && m->slots[i].busy)
            others++;
    if (st == OS_OK) {
        cnt.frames_ok += UA_NF;
        sec.done_ms += UA_NF;
        if (frame_clock()) {
            uint32_t now = frame_now();
            uint32_t turn = (now - sl->t_queued) % UA_FRAME_WRAP;
            uint32_t due = (uint32_t) sl->ahead + UA_NF;
            uint32_t wait = turn > due ? turn - due : 0;
            unsigned n = nfirst;
            if (wait > UA_LATE_MS) {
                cnt.late_frames += UA_NF;
                sec.late_ms += UA_NF;
                q_unsure = 2 * UA_NX;   /* ua18 */
            }
            sec.turn_sum += turn;
            if (turn < sec.turn_min) sec.turn_min = turn;
            if (turn > sec.turn_max) sec.turn_max = turn;
            sec.wait_sum += wait;
            if (wait < sec.wait_min) sec.wait_min = wait;
            if (wait > sec.wait_max) sec.wait_max = wait;
            if (have_last_done) {
                uint32_t apart = (now - t_last_done) % UA_FRAME_WRAP;
                if (apart > sec.apart_max) sec.apart_max = apart;
            }
            t_last_done = (uint16_t) now;
            have_last_done = 1;
            if (n < UA_FIRST) {
                first[n].turn = (uint16_t) turn;
                first[n].ahead = sl->ahead;
                barrier();
                nfirst = n + 1;
            }
        }
        if (!ramp && others * UA_NF < sec.ahead_min)
            sec.ahead_min = others * UA_NF;
    } else if (st == OS_CANCELLED) {
        cnt.cancelled++;
    } else {
        cnt.errors++;
        sec.failed_ms += UA_NF;
        first_error(S_COMPLETE, st, sl->idx);
    }
    if (others == 0 && st == OS_OK && !stopping && !ramp) {
        cnt.starved++;
        sec.restarts++;
    }
    if (!ramp && others * UA_NF < cnt.min_queued_ms)
        cnt.min_queued_ms = others * UA_NF;
    barrier();
    sl->busy = 0;
    if (frame_clock()) {
        uint32_t c = frame_now();
        int32_t d = (int32_t) (((uint32_t) sl->t_end - c) % UA_FRAME_WRAP);
        if (d >= (int32_t) (UA_FRAME_WRAP / 2))
            d -= (int32_t) UA_FRAME_WRAP;
        if (st == OS_OK && (d > 1 || d < -(int32_t) UA_LATE_MS))
            q_unsure = 2 * UA_NX;   /* ua18: back far from where it was taken to end */
        slack_at_link(m, c);        /* ua18: what MAIN queued behind it is linked by now */
    }
    if (stopping || closing || st == OS_CANCELLED || state != UA_STREAMING) {
        m->in_cb--;
        return;
    }
    if (st != OS_OK && cnt.errors > 50) {
        m->in_cb--;
        return;                     /* stop hammering a failing pipe: MAIN sees no inflight */
    }
    if (idle_req) {
        if (xchg8(&fade_out, 0)) {
            sl->fade = -1;          /* the last one before the gap: fades out */
        } else {
            sl->parked = 1;         /* idle: not queued again; ua_set_idle(0) takes it */
            barrier();
#ifdef UA_HOST_TEST
            if (ua_fake_park)
                ua_fake_park();     /* MAIN resumes here (the re-check below) */
#endif
            if (idle_req || !xchg8(&sl->parked, 0)) {
                /* Nothing plays from the ring now: a discard request is
                 * acknowledged at once (gpSP waits for it). */
                if (ring)
                    (void) ring_get(ring, NULL, 0);
                m->in_cb--;
                return;
            }
        }
    }
    sl->deferred = 1;
    barrier();
    if (!submitted_all) {
        m->in_cb--;
        return;                     /* MAIN requeues it after its submit loop */
    }
    if (xchg8(&sl->deferred, 0))
        requeue(sl);
    /* Resumed while transfers were still queued (or after a keep-alive):
     * what was parked goes in behind this one, here, in the order the audio
     * comes from the ring; two a completion at most (the OS's time), the
     * queue full again within three. Also when this one's own requeue
     * failed: a parked transfer must not wait for a resume that came. */
    took = 0;
    for (i = 0; i < UA_NX && took < 2 && !idle_req; i++)
        if (xchg8(&m->slots[i].parked, 0)) {
            requeue(&m->slots[i]);
            took++;
        }
    m->in_cb--;
}

static void cb_isoc(os_xfer xfer, void *priv, unsigned status)
{
    callbacks++;
    barrier();
    isoc_body(xfer, priv, status);
    barrier();
    callbacks--;
}

/* ---- MAIN ------------------------------------------------------------------------- */

static void probe_layout(struct slot *sl)
{
    void *priv = NULL, *buf = NULL;
    uint32_t count = UINT32_MAX;
    uint8_t status = 0xFF;
    setup_slot(sl);
    api.xfer_status(sl->xfer, &priv, &buf, &count, &status);
    osi.lay_method = 0;
    if (priv == sl && buf == buf_arg(sl) && count == 0 && status == OS_NOT_STARTED) {
        /* These offsets describe the captured function, not guessed reads. */
        osi.lay_buf = 3;
        osi.lay_len = 4;
        osi.lay_frlen = 13;
        osi.lay_nframes = 14;
        osi.lay_method = 3;       /* TI's eight-argument isoc setup */
    }
    step(S_LAYOUT, osi.lay_method ? OS_OK : OS_INVAL,
         (uint32_t) (uint8_t) osi.lay_len << 24 | (uint32_t) (uint8_t) osi.lay_buf << 16 |
         (uint32_t) (uint8_t) osi.lay_frlen << 8 | (uint8_t) osi.lay_nframes,
         count, osi.lay_method);
}

static uint32_t irq_off(void)
{
#if defined(__arm__) && defined(_TINSPIRE)
    uint32_t old, tmp;
    __asm__ __volatile__("mrs %0, cpsr\n orr %1, %0, #0xC0\n msr cpsr_c, %1" : "=r"(old), "=r"(tmp) :: "memory");
    return old;
#else
    return 0;
#endif
}
static void irq_restore(uint32_t old)
{
#if defined(__arm__) && defined(_TINSPIRE)
    __asm__ __volatile__("msr cpsr_c, %0" :: "r"(old) : "memory");
#else
    (void) old;
#endif
}

void ua_take_second(struct ua_second *s)
{
    uint32_t old = irq_off();
    *s = sec;
    second_reset();
    irq_restore(old);
    s->timed = (uint8_t) frame_clock();
}

/* ua18: the queue as the frame clock sees it (ua_os.h). */
int ua_queue_ms(int *ahead, int *head)
{
    struct os_mem *m = mem;
    uint32_t now, old;
    unsigned i, n = 0;
    int a = 0, h = 0, d;
    if (!frame_clock() || !m || state != UA_STREAMING || idle_req || stopping || closing || q_unsure)
        return 0;
    old = irq_off();
    if (m != mem || m->detached) {
        irq_restore(old);
        return 0;
    }
    now = frame_now();
    for (i = 0; i < UA_NX; i++) {
        const struct slot *sl = &m->slots[i];
        if (!sl->busy)
            continue;
        d = (int) (((uint32_t) sl->t_end - now) % UA_FRAME_WRAP);
        if (d >= (int) (UA_FRAME_WRAP / 2))
            d -= (int) UA_FRAME_WRAP;     /* passed */
        if (d > (int) (UA_NX * UA_NF + 8) || d < -(int) UA_LATE_MS) {
            irq_restore(old);
            return 0;               /* (an end that can't be: a count gone round, a lapped list) */
        }
        if (!n || d > a) a = d;
        if (!n || d < h) h = d;
        n++;
    }
    irq_restore(old);
    if (!n)
        return 0;                   /* (nothing queued: a stream starting over, or lost) */
    *ahead = a;
    *head = h;
    return 1;
}

/* ua18 (MAIN): the OS's completions ran (an interrupt window, a sleep) and
 * the head transfer's did not: it is not done, so it ends this frame at the
 * earliest. Every transfer's end moves on by as much, within its slack. */
int ua_queue_not_done(void)
{
    struct os_mem *m = mem;
    uint32_t now, old;
    unsigned i, moved = 0;
    int h = 0, d, n = 0, by;
    if (!frame_clock() || !m || state != UA_STREAMING)
        return 0;
    old = irq_off();
    if (m != mem || m->detached) {
        irq_restore(old);
        return 0;
    }
    now = frame_now();
    for (i = 0; i < UA_NX; i++) {
        const struct slot *sl = &m->slots[i];
        if (!sl->busy)
            continue;
        d = (int) (((uint32_t) sl->t_end - now) % UA_FRAME_WRAP);
        if (d >= (int) (UA_FRAME_WRAP / 2))
            d -= (int) UA_FRAME_WRAP;
        if (!n++ || d < h) h = d;
    }
    by = n && h < 0 ? -h : 0;
    for (i = 0; by && i < UA_NX; i++) {
        struct slot *sl = &m->slots[i];
        unsigned k = sl->busy ? ((unsigned) by < sl->slack ? (unsigned) by : sl->slack) : 0;
        sl->t_end = (uint16_t) ((sl->t_end + k) % UA_FRAME_WRAP);
        sl->slack = (uint16_t) (sl->slack - k);
        if (k > moved)
            moved = k;
    }
    irq_restore(old);
    return (int) moved;
}

static int requeue_deferred(struct os_mem *m, uint32_t gen);

#define GONE_ST 0x200u              /* submit_main: the block is not this device's any more */

/* MAIN: is m still the block MAIN read with gen? Interrupts off. */
static int main_valid(const struct os_mem *m, uint32_t gen)
{
    return gen == mem_gen && !stopping && !closing && m == mem && !m->detached && m->pipe;
}

/* An IRQ point of MAIN's (interrupts on): the USB task may run here. */
static void main_irq_point(void)
{
#ifdef UA_HOST_TEST
    if (ua_fake_main_irq)
        ua_fake_main_irq();
#endif
}

/* MAIN: set slot sl up and queue it, unless a detach came since gen was read
 * (it freed the transfers; the next attach may own the block): GONE_ST,
 * nothing touched. The check, the setup and busy are done with interrupts
 * off, the OS call with them as the caller had them.
 *
 * The residual window (known): usbd_transfer must not run masked, so the
 * interrupts held during the setup come in between irq_restore and the
 * call. A detach there finds busy set, so it neither closes the pipe nor
 * frees the transfers: the block is quarantined (M_KEPT, detached) and
 * never reused or freed by MAIN. The generation is read again right before
 * the call (nothing in between): a detach before it, and the transfer is
 * not made. Only a detach inside the call itself remains: then the
 * transfer goes to the pipe of a device whose detach has returned, a pipe
 * this driver never closed. TI's usbd_transfer returns 19 at once while its
 * pipe's aborting flag is set (10a084e4; usbd_abort_pipe, 10a07bc8, sets
 * it, and the detach aborts the pipe first); whether the flag is still set
 * after the abort returns is not known (NetBSD clears it). If it is not,
 * the transfer is queued on that orphaned pipe: its completion (an error,
 * or none) only touches the quarantined block. Either way the pipe and
 * the six transfers stay with the OS until it reboots (a leak, no
 * corruption). */
static unsigned submit_main(struct os_mem *m, struct slot *sl, uint32_t gen)
{
    unsigned s, i, ahead = 0;
    uint32_t old;
    main_irq_point();
    old = irq_off();
    if (!main_valid(m, gen) || !sl->xfer) {
        irq_restore(old);
        return GONE_ST;
    }
    setup_slot(sl);
    for (i = 0; i < UA_NX; i++)
        if (&m->slots[i] != sl && m->slots[i].busy)
            ahead += UA_NF;
    sl->ahead = (uint16_t) ahead;
    sl->t_queued = (uint16_t) frame_now();
    sl->t_end = chain_end(m, sl, sl->t_queued, &sl->slack);
    sl->pending = ahead != 0;       /* a pipe not running links it at once; else at its next soft interrupt */
    sl->busy = 1;                   /* a detach from here on keeps the block (free_all_mem) */
    barrier();
    irq_restore(old);
#ifdef UA_HOST_TEST
    if (ua_fake_irq_held)
        ua_fake_irq_held();         /* the interrupts held during the setup */
#endif
    if (gen != mem_gen) {           /* a detach came: no transfer; the block stays quarantined */
        sl->busy = 0;
        osi.late_detach++;
        return GONE_ST;
    }
    s = ST(api.transfer(sl->xfer));
    if (gen != mem_gen) {           /* a detach inside the call: never touch the pipe again */
        if (s != OS_OK && s != OS_INPROG)
            sl->busy = 0;           /* not queued */
        osi.late_detach++;          /* (queued: busy stays; the block stays quarantined) */
        return GONE_ST;
    }
    if (s != OS_OK && s != OS_INPROG) {
        sl->busy = 0;
        return s;
    }
    cnt.queued_frames += UA_NF;
    sec.queued_ms += UA_NF;
    return OS_OK;
}

/* MAIN: refill slot sl from the ring with interrupts off (the callbacks
 * share the schedule and the ring), unless the block went away. 1 ok, 0 the
 * format is broken, -1 gone. */
static int refill_main(struct os_mem *m, struct slot *sl, uint32_t gen, int primer)
{
    uint32_t old, bytes;
    main_irq_point();
    old = irq_off();
    if (!main_valid(m, gen)) {
        irq_restore(old);
        return -1;
    }
    bytes = refill(sl, tmp_main, primer);
    irq_restore(old);
    return bytes ? 1 : 0;
}

int ua_start(struct ring *r)
{
    struct os_mem *m = mem;
    uint32_t gen = mem_gen, old;
    unsigned i, s;
    if (state != UA_READY || !m || m->detached)
        return OS_INVAL;
    if (!api.setup_isoc || !completion) {
        step(S_ISOC_FN, 0x100, 0, 0, 0);
        fail(S_ISOC_FN, 0x100, 0);
        return 0x100;
    }
    step(S_ISOC_FN, 0, osi.addr[F_SETUP_ISOC], osi.isoc_how, 0);
    ring = r;
    pkt_init(&sched, desc.rate);
    osi.buf_conv = 2;              /* independent of control-request calibration */
    cnt.min_queued_ms = 0xFFFFFFFFu;
    second_reset();
    q_unsure = 0;
    nfirst = 0;
    have_last_done = 0;
    /* The OS links the first transfer alone, 4 frames from now; the others
     * only once that one has gone out (4 frames and a soft interrupt later).
     * So the first one carries silence: the song starts in one piece.
     * ua18, UA_START_ONE (gpSP's build): only it is queued here; its
     * completion queues the others from the ring, two a completion
     * (isoc_body, as after a resume): where the OS links them is then known
     * (ua_queue_ms), not "at some soft interrupt before that completion".
     * The wire the same (the OS links them at that completion either way). */
    fade_in = 0;
    for (i = 0; i < UA_NX; i++) {
        m->slots[i].busy = m->slots[i].deferred = m->slots[i].parked = 0;
        m->slots[i].fade = 0;
        m->slots[i].slack = 0;
        m->slots[i].pending = 0;
        if (!refill(&m->slots[i], tmp_main, i == 0 || idle_req || UA_START_QUEUED < UA_NX)) {
            fail(S_TRANSFER, OS_INVAL, i);
            return OS_INVAL;
        }
    }
    main_irq_point();
    old = irq_off();
    if (!main_valid(m, gen)) {
        irq_restore(old);
        return 0;                   /* detached meanwhile: its transfers are gone */
    }
    probe_layout(&m->slots[0]);
    irq_restore(old);
    if (!osi.lay_method) {
        fail(S_LAYOUT, OS_INVAL, 0);
        return OS_INVAL;
    }
    if (idle_req) {
        /* Started idle: nothing is queued; ua_set_idle(0) primes the stream. */
        for (i = 0; i < UA_NX; i++)
            m->slots[i].parked = 1;
        fade_out = 0;
        ka_have = 0;
        ka_elapsed = ka_calls = 0;
        submitted_all = 1;
        barrier();
        state = UA_STREAMING;
        return 0;
    }
    submitted_all = 0;
    for (i = UA_START_QUEUED; i < UA_NX; i++)
        m->slots[i].parked = 1;     /* (UA_START_ONE: the first one's completion takes them) */
    state = UA_STREAMING;
    barrier();
    for (i = 0; i < UA_START_QUEUED; i++) {
        s = submit_main(m, &m->slots[i], gen);
        if (s == GONE_ST)
            return 0;
        step(S_TRANSFER, s, i, m->slots[i].bytes, 0);
        if (s != OS_OK) {
            first_error(S_TRANSFER, s, i);
            close_stream_mem(m, 0);
            fail(S_TRANSFER, s, i);
            return (int) s;
        }
    }
    barrier();
    submitted_all = 1;
    barrier();
    return requeue_deferred(m, gen);
}

/* MAIN, after its submit loop: the transfers that came back during it
 * (submitted_all 0) go in again, in the order they were submitted. */
static int requeue_deferred(struct os_mem *m, uint32_t gen)
{
    unsigned i, s;
    int r;
    for (i = 0; i < UA_NX; i++) {
        if (!xchg8(&m->slots[i].deferred, 0))
            continue;
        r = refill_main(m, &m->slots[i], gen, 0);
        if (r < 0)
            return 0;
        if (!r) {
            close_stream_mem(m, 0);
            fail(S_TRANSFER, OS_INVAL, i);
            return OS_INVAL;
        }
        s = submit_main(m, &m->slots[i], gen);
        if (s == GONE_ST)
            return 0;
        if (s != OS_OK) {
            cnt.requeue_fail++;
            first_error(S_TRANSFER, s, i);
        }
    }
    return 0;
}

/* ---- idle (2026-11-18) -------------------------------------------------------------
 * A card nobody listens to (gpSP's output on Bluetooth, its menus) streamed
 * silence: a completion every 8 ms, 27-30 of the OS's interrupt windows a
 * second. Idle lets the queued transfers play out (the last one fading out)
 * and queues nothing more: the pipe stays open, the device claimed, nothing
 * is aborted or closed (no TI abort mark: mark_off's comment).
 *
 * The OS links a pipe's next packet right after the last one it linked, or
 * 4 frames from now when that frame has passed (10a0db0c), comparing 16-bit
 * micro-frame counts: a last linked frame more than ~4.1 s old (modulo
 * 8.192 s) reads as ahead, not passed. Whether the soft interrupt (10a0f144)
 * forgets an empty pipe's last frame is not known (NOTES.txt 2026-11-18).
 * So while idle, MAIN's ua_set_idle(1) calls queue one transfer of silence
 * every UA_KEEPALIVE_MS (one completion a second instead of 125): the last
 * linked frame never gets old. Resuming queues one transfer of silence
 * (prime; if the OS links it late, it is silence that waits); its
 * completion queues the parked ones behind it from the ring, emptied at the
 * resume of what was put while idle, the first audio faded in. */

static int quiet(const struct os_mem *m)
{
    return m && !m->detached && !m->in_cb && !slot_any_busy(m);
}

int ua_idle(void)
{
    struct os_mem *m = mem;
    return idle_req && state == UA_STREAMING && quiet(m);
}

/* MAIN, nothing queued, not idle: one transfer of silence goes in (the OS
 * links it alone, 4 frames from now, or wherever a stale last frame puts it:
 * then it is silence that waits); its completion queues the rest behind it,
 * two a completion (isoc_body), from what the producer has put by then.
 * The rest: the parked transfers, and any a failed requeue left out, unless
 * the pipe has failed too often (a recovery when a stream lost them all). */
static void prime(struct os_mem *m, uint32_t gen)
{
    unsigned i, first = UA_NX, s;
    int r;
    for (i = 0; i < UA_NX; i++) {
        struct slot *sl = &m->slots[i];
        if (!sl->xfer || sl->busy || (!sl->parked && cnt.errors > 50))
            continue;
        if (first == UA_NX) {
            first = i;
            sl->parked = 0;
        } else {
            sl->parked = 1;
        }
    }
    if (first == UA_NX)
        return;
    m->slots[first].deferred = 0;
    r = refill_main(m, &m->slots[first], gen, 1);
    if (r < 0)
        return;
    if (!r) {
        close_stream_mem(m, 0);
        fail(S_TRANSFER, OS_INVAL, first);
        return;
    }
    s = submit_main(m, &m->slots[first], gen);
    if (s == GONE_ST)
        return;
    if (s != OS_OK) {
        cnt.requeue_fail++;
        first_error(S_TRANSFER, s, first);
        if (m == mem && !m->detached)
            m->slots[first].parked = 1;     /* the next ua_set_idle(0) tries again */
    }
}

/* MAIN, idle and nothing queued: one transfer of silence now and then, so
 * the OS's last linked frame for this pipe stays recent (see above). */
static void keep_alive(struct os_mem *m, uint32_t gen)
{
    unsigned i;
    if (frame_clock()) {
        uint32_t now = frame_now();
        if (ka_have)
            ka_elapsed += (now - ka_prev) % UA_FRAME_WRAP;
        ka_prev = now;
        ka_have = 1;
        if (ka_elapsed < UA_KEEPALIVE_MS)
            return;
    } else if (++ka_calls < UA_KEEPALIVE_CALLS) {
        return;
    }
    ka_elapsed = ka_calls = 0;
    ka_have = 0;
    fade_out = 0;                   /* (its completion parks it: nothing to fade) */
    for (i = 0; i < UA_NX; i++)
        if (xchg8(&m->slots[i].parked, 0))
            break;
    if (i == UA_NX)
        return;
    if (refill_main(m, &m->slots[i], gen, 1) == 1) {
        unsigned s = submit_main(m, &m->slots[i], gen);
        if (s == OS_OK) {
            cnt.keepalives++;
            return;
        }
        if (s == GONE_ST)
            return;
        cnt.requeue_fail++;
    }
    if (m == mem && !m->detached)
        m->slots[i].parked = 1;
}

void ua_set_idle(int on)
{
    struct os_mem *m = mem;
    uint32_t gen = mem_gen, old;
    if (on) {
        if (!idle_req) {
            fade_out = state == UA_STREAMING && m && slot_any_busy(m);   /* audio still queued: fade it out */
            ka_have = 0;
            ka_elapsed = ka_calls = 0;
            barrier();
            idle_req = 1;
            return;
        }
        if (state != UA_STREAMING || !m)
            return;
        /* Nothing plays from the ring now (draining, or idle): MAIN
         * acknowledges a discard request (ring_request_clear) at once. */
        old = irq_off();
        if (ring && main_valid(m, gen))
            (void) ring_get(ring, NULL, 0);
        irq_restore(old);
        if (!quiet(m)) {
            ka_have = 0;            /* the time counts from a quiet pipe */
            ka_elapsed = 0;
        } else if (!stopping && !closing) {
            keep_alive(m, gen);
        }
        return;
    }
    if (idle_req) {
        int faded = !xchg8(&fade_out, 0);   /* a fade-out went (or the stream began idle) */
        barrier();
        idle_req = 0;
        barrier();
        if (faded && state == UA_STREAMING && m) {
            /* Start from silence and fresh audio: what the producer put
             * while nobody played it is dropped but its newest
             * UA_RESUME_KEEP_MS (its priming: what the primer's completion
             * queues at once), the next audio fades in. */
            old = irq_off();
            if (ring && main_valid(m, gen))
                (void) ring_drop_older(ring, desc.rate * UA_RESUME_KEEP_MS / 1000u);
            fade_pos = 0;
            barrier();
            fade_in = 1;
            irq_restore(old);
        }
    }
    /* Transfers still queued (resumed while draining, or a keep-alive):
     * their completions take the parked ones (isoc_body). Nothing queued:
     * MAIN primes, also as a recovery when a stream lost them all. */
    if (state != UA_STREAMING || stopping || closing || !quiet(m))
        return;
    prime(m, gen);
}

void ua_stop(void (*sleep_tick)(void))
{
    int st = state, n;
    struct os_mem *m;
    if (st != UA_STREAMING && st != UA_READY && st != UA_FAILED && st != UA_SETUP &&
        st != UA_GONE && st != UA_STOPPED)
        return;
    /* An entered completion can have busy=0 and still be about to set up
     * and submit its next transfer. Stop future requeues now, but do not
     * abort/free its objects until that callback has returned. */
    stopping = 1;
    barrier();
    if (attaching || callbacks)
        return;
    if (st == UA_GONE)
        return;                    /* detach_body already released or quarantined its block */
    m = mem;
    if (!m || m->detached)
        return;
    if (!att_self && !m->pipe && !slot_any_busy(m) && !slot_any_xfer(m))
        return;
    /* 2026-11-16: no abort where the queued transfers can play out (48 ms
     * of sound at most; none is queued again: stopping): an abort leaves
     * TI's mark on each of them (mark_off's comment). Only what has not come
     * back after UA_DRAIN_TICKS of the OS's time (a device that is gone and
     * not detached yet, a stalled one) is aborted. */
    for (n = 0; n < UA_DRAIN_TICKS && sleep_tick && slot_any_busy(m) && m == mem && !m->detached &&
         state != UA_GONE; n++)
        sleep_tick();
    if (m != mem || m->detached || state == UA_GONE)
        return;                    /* OS detached it while waiting: do not touch its pipe/xfers */
    if (slot_any_busy(m) || !sleep_tick)
        close_stream_mem(m, 0);    /* (ua_start's failure path may have aborted already) */
    else
        osi.drained++;
    for (n = 0; n < 50 && sleep_tick && slot_any_busy(m) && m == mem && !m->detached && state != UA_GONE; n++)
        sleep_tick();
    if (m != mem || m->detached || state == UA_GONE)
        return;                    /* OS detached it while waiting: do not touch its pipe/xfers */
    step(S_STOP, slot_any_busy(m) ? OS_TIMEOUT : 0, cnt.completions, cnt.cancelled, 0);
    if (slot_any_busy(m))
        return;                    /* retry while all transfer storage stays alive */
    if (free_all_mem(m, 0) && !att_self)
        release_mem_slot(m);
    if (state != UA_GONE && state != UA_FAILED)
        state = UA_STOPPED;
}

int ua_init(void)
{
    if (state != UA_OFF)
        return 0;
    memset(&osi, 0, sizeof osi);
    memset(&cnt, 0, sizeof cnt);
    second_reset();
    nfirst = 0;
    have_last_done = 0;
    memset(&desc, 0, sizeof desc);
    desc.chosen = -1;
    osi.lay_buf = osi.lay_len = osi.lay_frlen = osi.lay_nframes = -1;
    ev_wr = ev_rd = ev_lost = 0;
    att_self = NULL;
    att_dev = NULL;
    att_iface = NULL;
    matched_self = NULL;
    closing = stopping = 0;
    attaching = callbacks = 0;
    paused = 0;
    idle_req = fade_out = fade_in = 0;
    probes = 0;
    if (!resolve()) {
        return -1;
    }
    step(S_RESOLVE, 0, osi.isoc_how, 0, 0);
    mem_raw = malloc(sizeof *mem_pool * UA_MEM_SLOTS + 32);
    if (!mem_raw) {
        init_text = "not enough memory";
        return -2;
    }
    mem_pool = (struct os_mem *) (((uintptr_t) mem_raw + 31) & ~(uintptr_t) 31);
    memset(mem_pool, 0, sizeof *mem_pool * UA_MEM_SLOTS);
    mem = NULL;
#ifdef UA_HOST_TEST
    if (!usbshim_register) {
        init_text = "no shim";
        free(mem_raw);
        mem_raw = NULL;
        mem_pool = NULL;
        return -3;
    }
#endif
    up = 1;
    state = UA_WAITING;
    barrier();
    shim = usbshim_register("usbaudio", os_cb_match, os_cb_attach, os_cb_detach, SOFTC_SIZE);
    if (!shim) {
        up = 0;
        state = UA_OFF;
        init_text = "could not register the driver (memory)";
        free(mem_raw);
        mem_raw = NULL;
        mem_pool = NULL;
        return -2;
    }
    completion = usbshim_callback(shim, cb_isoc);
    if (!completion) {
        up = 0;
        usbshim_retire(shim);
        shim = NULL;
        state = UA_OFF;
        free(mem_raw);
        mem_raw = NULL;
        mem_pool = NULL;
        init_text = "could not protect the transfer callback";
        return -2;
    }
    step(S_REGISTER, 0, 0, 0, 0);
    init_text = "ok";
    return 0;
}

const char *ua_init_text(void) { return init_text; }

int ua_shutdown(void (*sleep_tick)(void))
{
    int n, kept = 0;
    unsigned i;
    uint32_t saved;
    if (state == UA_OFF)
        return 0;
    /* Block new attaches before yielding. Retiring the trampoline only
     * stops future entries; it cannot cancel a callback already inside
     * the OS, which must return before our code or buffers go away. */
    closing = 1;
    up = 0;
    barrier();
    if (callbacks)
        return 1;
    ua_stop(sleep_tick);
    /* Wait briefly for already-detached KEPT slots whose delayed cancellations
     * are about to land cleanly before retiring the trampoline. */
    for (n = 0; n < 50 && sleep_tick && !att_self && any_kept_busy() && !callbacks; n++)
        sleep_tick();
    /* Close the entry race as well: a callback suspended inside any OS
     * call must return before its code is unloaded, not just attach. */
    saved = irq_off();
    if (callbacks || any_busy() || (mem && mem->pipe)) {
        irq_restore(saved);
        return 1;
    }
    if (shim)
        usbshim_retire(shim);
    irq_restore(saved);
    for (n = 0; n < 10; n++)
        if (sleep_tick)
            sleep_tick();
    if (mem_pool) {
        for (i = 0; i < UA_MEM_SLOTS; i++)
            if (mem_pool[i].mstate == M_KEPT)
                kept = 1;
    }
    /* mem_raw stays allocated if any KEPT block/pipe/transfer remains with the OS */
    if (!kept && !any_mem_busy()) {
        free(mem_raw);
    } else if (kept_n < UA_KEPT_MAX) {
        kept_mem[kept_n++] = mem_raw;
    }
    mem_raw = NULL;
    mem_pool = NULL;
    mem = NULL;
    shim = NULL;
    att_self = NULL;
    att_dev = NULL;
    state = UA_OFF;
#ifdef UA_HOST_TEST
    frame_off = 0;
#endif
    return 0;
}
