/* usbaudio.tns: log file, screen, keys and OS time. USB registers are read
 * only. Keypad/cursor ownership follows gpSP's corrected OS hand-back. */
#include <os.h>
#include <nspireio/nspireio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "ua_os.h"
#ifdef UA_MUSIC_PLAYER
#include "library.h"
#include "screen.h"
#endif

#ifndef UA_BUILD
#define UA_BUILD "dev"
#endif

#ifdef UA_HOST_TEST
#include "platform_fake.h"
#define w32 ua_test_read
#define put32 ua_test_write
#define irq_unmask ua_test_irq_unmask
#define irq_restore ua_test_irq_restore
#else
static uint32_t w32(uint32_t a) { return *(volatile const uint32_t *) (uintptr_t) a; }
static void put32(uint32_t a, uint32_t v) { *(volatile uint32_t *) (uintptr_t) a = v; }
static uint32_t irq_unmask(void)
{
    uint32_t old, on;
    __asm__ __volatile__("mrs %0, cpsr\n\tbic %1, %0, #0x80\n\tmsr cpsr_c, %1"
                         : "=&r"(old), "=&r"(on) : : "memory");
    return old;
}
static void irq_restore(uint32_t old)
{
    __asm__ __volatile__("msr cpsr_c, %0" : : "r"(old) : "memory");
}
#endif

#define IRQ_ENABLE  0xDC000010u
#define IRQ_DISABLE 0xDC000014u
#define KEYPAD_IRQ  (1u << 16)
#define TICK_IRQ    (1u << 19)
#define TICK_CONTROL 0x900D0008u
#define LCD_CURSOR 0xC0000C00u
#define LCD_CURSOR_XY 0xC0000C10u
#ifdef UA_MUSIC_PLAYER
#define LCD_BASE 0xC0000010u
#define LCD_CONTROL 0xC0000018u
static uint16_t display_frames[2][SCREEN_W * SCREEN_H] __attribute__((aligned(32)));
static uint32_t display_os_base, display_shown;
static unsigned display_owned, display_valid, display_front, display_portrait;
static unsigned display_cols, display_rows, display_restores;
static int display_type;
/* The LCD shows our own buffer; the OS goes on drawing into its own,
 * unseen (the Documents screen it repaints while it gets time flashed
 * through the player when both drew into one buffer). Should the OS point
 * the LCD back at its buffer while it runs, this puts ours back after
 * every sleep and counts it: the count goes into the log at exit. */
static void keep_display(void)
{
    if (display_owned && display_valid) {
        uint32_t at = (uint32_t)(uintptr_t)display_frames[display_front];
        if (w32(LCD_BASE) != at) {
            if (display_shown == at) display_restores++;
            put32(LCD_BASE, at);
        }
        display_shown = at;
    }
}
/* A private buffer needs the LCD's pixel layout. Ndless knows it for every
 * model (lcd_type: what its own lcd_blit converts to); the LCD
 * controller's timing registers are the second source, for an Ndless that
 * does not say. Without either the screen is shared with the OS as before
 * (and can flash). */
static void display_begin(void)
{
    display_cols = ((w32(0xC0000008u) >> 16) & 0x3FFu) + 1;
    display_rows = (w32(0xC0000004u) & 0x3FFu) + 1;
    display_type = (int) lcd_type();
    display_os_base = w32(LCD_BASE);
    display_valid = display_front = display_restores = 0;
    display_shown = 0;
    if (display_type == SCR_240x320_565 || display_type == SCR_320x240_565) {
        display_portrait = display_type == SCR_240x320_565;
        display_owned = 1;
    } else {
        display_portrait = display_cols == 240 && display_rows == 320;
        display_owned = display_portrait || (display_cols == 320 && display_rows == 240);
    }
    if (((w32(LCD_CONTROL) >> 1) & 7u) != 6)
        display_owned = 0;                  /* not 16 bits a pixel right now */
    if (!display_owned) lcd_init(SCR_320x240_565);
}
static void display_present(const uint16_t *pixels)
{
    unsigned back = display_front ^ 1u;
    uint16_t *native = display_frames[back];
    if (!display_owned) { lcd_blit((void *)pixels, SCR_320x240_565); return; }
    if (display_portrait) {
        for (unsigned y = 0; y < SCREEN_H; y++)
            for (unsigned x = 0; x < SCREEN_W; x++)
                /* As gpSP draws it (nspire_blend.c): landscape pixel (x, y)
                 * is at position 239 - y of LCD line x. Without the flip the
                 * CX II showed the screen mirrored and upside down. */
                native[x * SCREEN_H + (SCREEN_H - 1u - y)] = pixels[y * SCREEN_W + x];
    } else memcpy(native, pixels, sizeof display_frames[0]);
#if !defined(UA_HOST_TEST)
    /* Like gpSP: clean the private buffer before the LCD reads it. */
    for (uintptr_t p = (uintptr_t)native; p < (uintptr_t)(native + SCREEN_W * SCREEN_H); p += 32)
        __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 1" :: "r"(p) : "memory");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
#endif
    display_front = back;
    display_valid = 1;
    keep_display();
}
static void display_end(void)
{
    if (display_owned) {
        display_owned = display_valid = 0;
        put32(LCD_BASE, display_os_base);
    } else lcd_init(SCR_TYPE_INVALID);
}
#endif
static const uint32_t owned_regs[] = {
    0x900E0000u, 0x900E0004u, 0x900E000Cu, 0x900E0040u,
    LCD_CURSOR_XY, LCD_CURSOR
};
static uint32_t saved_regs[sizeof owned_regs / sizeof owned_regs[0]];
static int keys_owned;

static void keep_keys(void)
{
    if (!keys_owned)
        return;
    if (w32(IRQ_ENABLE) & KEYPAD_IRQ)
        put32(IRQ_DISABLE, KEYPAD_IRQ);
    if (is_cx2 && w32(LCD_CURSOR_XY) != 0x01C001C0u)
        put32(LCD_CURSOR_XY, 0x01C001C0u);
    if (w32(LCD_CURSOR) & 1)
        put32(LCD_CURSOR, w32(LCD_CURSOR) & ~1u);
#ifdef UA_MUSIC_PLAYER
    keep_display();
#endif
}

static void take_keys(void)
{
    unsigned i;
    for (i = 0; i < sizeof owned_regs / sizeof owned_regs[0]; i++)
        saved_regs[i] = w32(owned_regs[i]);
    keys_owned = 1;
    keep_keys();
}

/* Last operation before return: no OS sleeps or file/screen calls after
 * this. Ndless waits for held keys before returning to the document UI. */
static void give_keys_back(void)
{
    unsigned i;
    keys_owned = 0;
    for (i = 0; i < sizeof owned_regs / sizeof owned_regs[0]; i++)
        if (w32(owned_regs[i]) != saved_regs[i])
            put32(owned_regs[i], saved_regs[i]);
    put32(IRQ_ENABLE, KEYPAD_IRQ);
}

static uint32_t usb_enter(void)
{
    keep_keys();
    return irq_unmask();
}
static void usb_leave(uint32_t saved)
{
    irq_restore(saved);
    keep_keys();
}

/* ---- log ---- */
static char dir[200], path[260], wav_path[260];
static char lbuf[24576];
static unsigned llen;
static int lfirst = 1, lbad;
static nio_console csl;
static int have_csl;

static void log_flush(void)
{
    FILE *f;
    if (!llen && !lfirst)
        return;
    f = fopen(path, lfirst ? "w" : "a");
    if (!f || fwrite(lbuf, 1, llen, f) != llen)
        lbad = 1;
    if (f && fclose(f) != 0)
        lbad = 1;
    lfirst = 0;
    llen = 0;
}

static void log_line(const char *s)
{
    unsigned n = (unsigned) strlen(s);
    if (llen + n + 2 > sizeof lbuf)
        log_flush();
    if (n + 2 <= sizeof lbuf) {
        memcpy(lbuf + llen, s, n);
        llen += n;
        lbuf[llen++] = '\n';
    }
    /* Close each line before drawing it or entering another OS call: a
     * reset must not lose the last completed setup/teardown diagnostic. */
    log_flush();
    keep_keys();
#ifndef UA_MUSIC_PLAYER          /* the music screen is reserved for playback controls */
    if (have_csl) {
        nio_fputs(s, &csl);
        nio_fputc('\n', &csl);
    }
#endif
}

/* ---- OS sleep: Nucleus TCC_Sleep found by its code (from nspire-usbpad
 * tools/padtest.c, which took the facts from ND Screen Mirror / gpSP) ---- */
#define OS_CODE_START 0x10000000u
#define OS_CODE_END   0x11000000u
#define SLEEP_WORDS   16u
#define SLEEP_HASH    0x13e2fde1u
#define TASK_MAGIC    0x5441534bu
static void (*tcc_sleep)(unsigned ticks);
static uint32_t sleep_task_var, sleeps, spins;

static int sleep_body(uint32_t at)
{
    uint32_t hash = 2166136261u, i;
    for (i = 0; i < SLEEP_WORDS; i++) {
        uint32_t op = w32(at + i * 4);
        if ((op & 0x0e000000u) == 0x0a000000u) {
            int32_t off = ((int32_t) ((op & 0x00ffffffu) ^ 0x00800000u) - 0x00800000) * 4;
            uint32_t target = at + i * 4 + 8 + (uint32_t) off;
            if ((op & 0x01000000u) || target < at || target >= at + SLEEP_WORDS * 4)
                op &= 0xff000000u;
        }
        hash = (hash ^ op) * 16777619u;
    }
    return hash == SLEEP_HASH;
}
static uint32_t pc_literal(uint32_t pc)
{
    uint32_t op = w32(pc);
    if ((op & 0xffff0000u) == 0xe59f0000u) return w32(pc + 8 + (op & 0xfff));
    if ((op & 0xffff0000u) == 0xe51f0000u) return w32(pc + 8 - (op & 0xfff));
    return 0;
}
static int in_ram(uint32_t at, uint32_t bytes)
{
    return !(at & 3) && at >= 0x10000000u && at < 0x14000000u && 0x14000000u - at >= bytes;
}
static int task_ok(void)
{
    uint32_t task;
    if (!in_ram(sleep_task_var, 4)) return 0;
    task = w32(sleep_task_var);
    return in_ram(task, 0x44) && w32(task + 0x0c) == TASK_MAGIC && w32(task + 0x38) == 0;
}
static int tick_ready(void)
{
    return (w32(IRQ_ENABLE) & TICK_IRQ) && (w32(TICK_CONTROL) & 0xA0u) == 0xA0u;
}
/* Install only after the signature scan has found exactly one function. */
static const char *os_sleep_enable(uint32_t found)
{
    tcc_sleep = NULL;
    sleep_task_var = pc_literal(found + 0x18);
    if (!task_ok()) return "the current task doesn't look like a Nucleus task";
    if (!tick_ready()) return "the OS tick timer or its interrupt is off";
#ifdef UA_HOST_TEST
    tcc_sleep = ua_test_bind_sleep(found);
#else
    tcc_sleep = (void (*)(unsigned)) (uintptr_t) found;
#endif
    return NULL;
}
static const char *os_sleep_find(uint32_t *at_out)
{
    uint32_t at, found = 0, n = 0;
    tcc_sleep = NULL;
    for (at = OS_CODE_START; at < OS_CODE_END; at += 4)
        if (w32(at) == 0xe92d4010u && w32(at + 4) == 0xe24dd008u && w32(at + 8) == 0xe1a04000u &&
            sleep_body(at) && !n++)
            found = at;
    *at_out = found;
    if (n != 1)
        return n ? "TCC_Sleep's code is in more than one place" : "TCC_Sleep's code not found";
    return os_sleep_enable(found);
}
static void sleep_tick(void)
{
    if (keys_owned && tcc_sleep && tick_ready() && task_ok()) {
        uint32_t old;
        keep_keys();
        old = irq_unmask();
        tcc_sleep(1);
        irq_restore(old);
        keep_keys();
        sleeps++;
        return;
    }
    {   /* no OS sleep: spin ~1 ms (the OS's USB work then never runs) */
        static volatile uint32_t sink;
        uint32_t i;
        for (i = 0; i < 20000; i++) sink++;
        spins++;
    }
}

/* ---- clock, keys ---- */
static uint32_t seconds(void) { return w32(0x90090000u); }

static unsigned keys(void)
{
    static unsigned prev;
    unsigned now = 0, edge;
    /* get_event can block with an empty queue. Read levels only; the OS's
     * keypad IRQ stays masked until give_keys_back(). */
    keep_keys();
    if (isKeyPressed(KEY_NSPIRE_ESC)) now |= A_QUIT;
    if (isKeyPressed(KEY_NSPIRE_ENTER) || isKeyPressed(KEY_NSPIRE_P)) now |= A_PAUSE;
    if (isKeyPressed(KEY_NSPIRE_TAB)) now |= A_NEXT;
#ifdef UA_MUSIC_PLAYER
    if (isKeyPressed(KEY_NSPIRE_RIGHT)) now |= A_NEXT;
    if (isKeyPressed(KEY_NSPIRE_LEFT)) now |= A_PREV;
    if (isKeyPressed(KEY_NSPIRE_R)) now |= A_REPEAT;
    if (isKeyPressed(KEY_NSPIRE_DEL)) now |= A_RESTART;
#endif
    if (isKeyPressed(KEY_NSPIRE_PLUS) || isKeyPressed(KEY_NSPIRE_UP)) now |= A_VOLUP;
    if (isKeyPressed(KEY_NSPIRE_MINUS) || isKeyPressed(KEY_NSPIRE_DOWN)) now |= A_VOLDN;
    edge = now & ~prev;
    prev = now;
    return edge;
}

static void wait_keys_released(void)
{
    unsigned quiet = 0, n;
    uint32_t start = seconds();
    for (n = 0; n < 150 && quiet < 5 && seconds() - start < 2; n++) {
        sleep_tick();
        quiet = any_key_pressed() ? 0 : quiet + 1;
    }
}

/* ---- the host controller, read only ---- */
#define USB_BASE 0xB0000000u
static int usb_kind(void)
{
    if (is_cx2)
        return (w32(USB_BASE) & 0xFFFF00FFu) == 0x01000010u ? 2 : 0;
    if (w32(0x900B0018u) & 0x20)
        return 0;
    return (w32(USB_BASE) & 0xFFFF) == 0xFA05 && (w32(USB_BASE + 0x100) & 0xFFFF00FFu) == 0x01000040u ? 1 : 0;
}

static int port_state(void)
{
    uint32_t otg, port, cmd;
    switch (usb_kind()) {
    case 2:
        otg = w32(USB_BASE + 0x80);
        if (!otg || otg == 0xFFFFFFFFu) return PORT_UNKNOWN;
        /* gpSP uses the ID pin, not the current role: the OS can still
         * be a device while the adapter is already physically present. */
        if (otg & (1u << 21))
            return otg & (1u << 17) ? PORT_PC : PORT_EMPTY;
        port = w32(USB_BASE + 0x30);
        cmd = w32(USB_BASE + 0x10);
        break;
    case 1:
        otg = w32(USB_BASE + 0x1A4);
        if (!otg || otg == 0xFFFFFFFFu || !(otg & (1u << 5))) return PORT_UNKNOWN;
        if (otg & (1u << 8))
            return otg & (1u << 11) ? PORT_PC : PORT_EMPTY;
        port = w32(USB_BASE + 0x184);
        cmd = w32(USB_BASE + 0x140);
        break;
    default:
        return PORT_UNKNOWN;
    }
    /* A pull-up on J/K is visible before the OS latches CCS (gpSP PAD3). */
    if (!(port & 0xC01u)) return PORT_ADAPTER;
    return (cmd & 1) ? PORT_DEVICE : PORT_STOPPED;
}
static int port_connected(void)
{
    int state = port_state();
    return state == PORT_UNKNOWN ? -1 : state == PORT_DEVICE || state == PORT_STOPPED;
}
/* Like gpSP's port observer, sample at ~100 ms instead of every poll of
 * the keypad. Ignore a single transient while the OS gates/switches USB. */
static int observed_port(void)
{
    static uint32_t sampled;
    static int started, last = PORT_UNKNOWN, candidate = PORT_UNKNOWN;
    int current;
    if (started && sleeps - sampled < 10) return last;
    sampled = sleeps;
    current = port_state();
    if (!started || current == candidate) last = current;
    candidate = current;
    started = 1;
    return last;
}

static void usb_regs(char *b, unsigned size)
{
    uint32_t cmd, sts, fr, plb, port, base;
    unsigned kind = (unsigned) usb_kind(), i, n, cnt[4] = { 0, 0, 0, 0 }, term = 0, other = 0, entries;
    int k;
    if (kind == 2) {
        base = USB_BASE + 0x10;
        cmd = w32(base); sts = w32(base + 4); fr = w32(base + 0xC); plb = w32(base + 0x14);
        port = w32(USB_BASE + 0x30);
    } else if (kind == 1) {
        base = USB_BASE + 0x140;
        cmd = w32(base); sts = w32(base + 4); fr = w32(base + 0xC); plb = w32(base + 0x14);
        port = w32(USB_BASE + 0x184);
    } else {
        snprintf(b, size, "controller not recognised (or gated)");
        return;
    }
    entries = 1024u >> ((cmd >> 2) & 3);
    if ((cmd >> 2 & 3) == 3) entries = 1024;
    k = snprintf(b, size, "%s USBCMD %08lx (run %lu, periodic %lu, async %lu, list %u) USBSTS %08lx "
                 "(periodic running %lu) FRINDEX %04lx PERIODICLISTBASE %08lx PORTSC %08lx, sleeps %lu spins %lu",
                 kind == 2 ? "FOTG210" : "ChipIdea", (unsigned long) cmd, (unsigned long) (cmd & 1),
                 (unsigned long) (cmd >> 4 & 1), (unsigned long) (cmd >> 5 & 1), entries, (unsigned long) sts,
                 (unsigned long) (sts >> 14 & 1), (unsigned long) (fr & 0x3FFF), (unsigned long) plb,
                 (unsigned long) port, (unsigned long) sleeps, (unsigned long) spins);
    plb &= ~0xFFFu;
    if (k > 0 && (unsigned) k < size && in_ram(plb, entries * 4)) {
        for (i = 0; i < entries; i++) {
            uint32_t e = w32(plb + 4 * i);
            if (e & 1) term++;
            else if (in_ram(e & ~0x1Fu, 4)) cnt[(e >> 1) & 3]++;
            else other++;
        }
        n = (unsigned) k;
        snprintf(b + n, size - n, "; frame list: %u empty, %u iTD, %u QH, %u siTD, %u FSTN, %u odd",
                 term, cnt[0], cnt[1], cnt[2], cnt[3], other);
    }
}

/* Where the controller's frame counter is, for the driver's timing (a read
 * of that one register per transfer); 0 if the controller is not known. */
static uint32_t usb_frindex(void)
{
    switch (usb_kind()) {
    case 2: return USB_BASE + 0x1C;
    case 1: return USB_BASE + 0x14C;
    default: return 0;
    }
}

/* The OS's isochronous descriptors (siTD) in the periodic frame list, read
 * only: how many, where they are counted from the frame the controller is
 * in, and what it has done with them. EHCI 3.4: word 2 = micro-frame masks,
 * word 3 = IOC, bytes still to send, C-prog mask, status (bit 7 active),
 * word 5 = buffer page 1 with TP and T-count (start-splits still to send).
 * The driver keeps 48 packets queued and the OS links them in consecutive
 * frames from 4 ahead: a waiting descriptor more than 64 frames away was
 * linked for a frame that has passed and was not (all) sent in it; it
 * waits until the frame list comes round again, 1024 ms after its frame. */
static void usb_sched(char *b, unsigned size)
{
    uint32_t fr, plb, cmd, base, sample = 0;
    unsigned kind = (unsigned) usb_kind(), entries, cur, i, n = 0, waiting = 0, far = 0, sent = 0, lo = 0, hi = 0;
    unsigned sample_slot = 0;
    int k;
    if (kind == 2) base = USB_BASE + 0x10;
    else if (kind == 1) base = USB_BASE + 0x140;
    else {
        snprintf(b, size, "siTD: controller not recognised");
        return;
    }
    cmd = w32(base); fr = w32(base + 0xC); plb = w32(base + 0x14) & ~0xFFFu;
    entries = 1024u >> ((cmd >> 2) & 3);
    if ((cmd >> 2 & 3) == 3) entries = 1024;
    if (!in_ram(plb, entries * 4)) {
        snprintf(b, size, "siTD: no frame list at %08lx", (unsigned long) plb);
        return;
    }
    cur = (unsigned) (fr >> 3) % entries;
    for (i = 0; i < entries; i++) {
        unsigned slot = (cur + i) % entries, hops = 0;
        uint32_t e = w32(plb + 4 * slot);
        while (!(e & 1) && ((e >> 1) & 3) == 2 && in_ram(e & ~0x1Fu, 36) && hops++ < 8) {
            uint32_t at = e & ~0x1Fu, state = w32(at + 12);
            n++;
            if (!(state & 0x80)) sent++;
            else {
                if (!waiting++) lo = i;
                hi = i;
                if ((i > 64 && !far++) || !sample) { sample = at; sample_slot = slot; }
            }
            e = w32(at);
        }
    }
    k = snprintf(b, size, "frame %u, siTD %u: %u waiting at +%u..+%u (%u more than 64 frames away), %u sent",
                 cur, n, waiting, lo, hi, far, sent);
    if (sample && k > 0 && (unsigned) k < size) {
        uint32_t state = w32(sample + 12), page1 = w32(sample + 20), masks = w32(sample + 8);
        snprintf(b + k, size - (unsigned) k, "; first %s: slot %u, status %02lx, %lu bytes left, T-count %lu, TP %lu, "
                 "S-mask %02lx, C-prog %02lx, IOC %lu", far ? "far one" : "waiting", sample_slot,
                 (unsigned long) (state & 0xFF), (unsigned long) (state >> 16 & 0x3FF), (unsigned long) (page1 & 7),
                 (unsigned long) (page1 >> 3 & 3), (unsigned long) (masks & 0xFF), (unsigned long) (state >> 8 & 0xFF),
                 (unsigned long) (state >> 31));
    }
}

/* ---- WAV file ---- */
static FILE *wav;
static unsigned wav_read(void *ctx, uint32_t off, void *buf, unsigned n)
{
    (void) ctx;
    if (!wav || fseek(wav, (long) off, SEEK_SET) != 0)
        return 0;
    return (unsigned) fread(buf, 1, n, wav);
}

static const char *docs_dir(void)
{
    return nl_hassyscall(get_documents_dir) ? get_documents_dir() : "/documents/";
}

#ifdef UA_MUSIC_PLAYER
static void music_draw(const struct music_view *v)
{
    static uint16_t pixels[SCREEN_W * SCREEN_H];
    static char previous[700];
    char key[700];
    snprintf(key,sizeof key,"%s|%s|%s|%u,%u,%u,%u,%u,%u,%u,%u,%u",
             v->title ? v->title : "",v->status ? v->status : "",v->detail ? v->detail : "",
             v->index,v->count,v->elapsed,v->duration,v->volume,v->repeat,v->playing,v->connected,v->error);
    if (strcmp(key, previous) || (display_owned && !display_valid)) {
        strcpy(previous,key);
        screen_render(pixels,v);
        display_present(pixels);
    } else if (!display_owned) {
        /* Compatibility fallback still shares the OS framebuffer. */
        display_present(pixels);
    }
    keep_keys();
}
#endif

int main(void)
{
    struct app_platform pf;
    const char *why;
    uint32_t at = 0, wav_size = 0;
    char line[400];
    int r;
#ifdef UA_MUSIC_PLAYER
    struct music_platform music;
#endif

    take_keys();
    snprintf(dir, sizeof dir, "%susbaudio", docs_dir());
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/"
#ifdef UA_MUSIC_PLAYER
             "musicplayer_log.txt.tns",
#else
             "usbaudio_log.txt.tns",
#endif
             dir);
    snprintf(wav_path, sizeof wav_path, "%s/music.wav.tns", dir);
#ifdef UA_MUSIC_PLAYER
    display_begin();
    {
        struct music_view starting = {0};
        starting.title = "Your music";
        starting.status = "Starting...";
        starting.detail = "Connect your USB audio adapter";
        music_draw(&starting);
    }
#else
    have_csl = nio_init(&csl, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, NIO_COLOR_BLACK, NIO_COLOR_WHITE, TRUE);
    if (have_csl)
        nio_set_default(&csl);
#endif

    snprintf(line, sizeof line, "usbaudio %s: USB audio through the OS's USB stack (experiment)", UA_BUILD);
    log_line(line);
    snprintf(line, sizeof line, "calculator: %s, hwtype %u subtype %u, OS id word %08lx, Ndless r%u",
             is_cx2 ? "CX II" : "CX", hwtype(), nl_hwsubtype(), (unsigned long) w32(0x10000020u), nl_ndless_rev());
    log_line(line);
#ifdef UA_MUSIC_PLAYER
    snprintf(line, sizeof line, "display: %s, %s, original base %08lx; Ndless LCD type %d, LCD timing %ux%u, "
             "control %08lx", display_owned ? "private framebuffer" : "shared with the OS (it can flash)",
             display_portrait ? "portrait" : "landscape", (unsigned long)display_os_base, display_type,
             display_cols, display_rows, (unsigned long)w32(LCD_CONTROL));
    log_line(line);
#endif
    why = os_sleep_find(&at);
    snprintf(line, sizeof line, "OS sleep: %s (TCC_Sleep at %08lx)", why ? why : "found", (unsigned long) at);
    log_line(line);
    snprintf(line, sizeof line, "log: %s", path);
    log_line(line);
    wav = fopen(wav_path, "rb");
    if (wav && fseek(wav, 0, SEEK_END) == 0) {
        long sz = ftell(wav);
        wav_size = sz > 0 ? (uint32_t) sz : 0;
    }
    snprintf(line, sizeof line, "music file %s: %s (%lu bytes)", wav_path, wav ? "found" : "not there",
             (unsigned long) wav_size);
    log_line(line);
    log_flush();

    memset(&pf, 0, sizeof pf);
    pf.log = log_line;
    pf.flush = log_flush;
    pf.sleep_tick = sleep_tick;
    pf.seconds = seconds;
    pf.keys = keys;
    pf.usb_enter = usb_enter;
    pf.usb_leave = usb_leave;
    pf.port_connected = port_connected;
    pf.port_state = observed_port;
    pf.usb_regs = usb_regs;
    pf.usb_sched = usb_sched;
    pf.usb_frindex = usb_frindex;
    pf.ticks_per_second = 100;
    pf.unplug_wait_s = 4;
    if (wav && wav_size) {
        pf.wav_read = wav_read;
        pf.wav_size = wav_size;
    }
#ifdef UA_MUSIC_PLAYER
    music.count = library_scan(docs_dir());
    music.title = library_title;
    music.open = library_open;
    music.draw = music_draw;
    pf.music = &music;
#endif
    if (why) {
        log_line("cannot run USB tasks without a working OS sleep; driver not started");
#ifdef UA_MUSIC_PLAYER
        struct music_view error = {0};
        error.title = "Music could not start";
        error.status = "USB service unavailable";
        error.detail = "Press Esc to return to Documents";
        error.error = 1;
        music_draw(&error);
        while (!(keys() & A_QUIT)) sleep_tick();
#endif
        r = 1;
    } else {
        r = app_run(&pf);
    }

    snprintf(line, sizeof line, "exit (%s): %lu OS sleeps, %lu spins; "
             "waiting for all keys to be released", r ? "stopped at a failure" : "normal",
             (unsigned long) sleeps, (unsigned long) spins);
    log_line(line);
#ifdef UA_MUSIC_PLAYER
    snprintf(line, sizeof line, "display: the OS pointed the LCD back at its own screen %u times while it ran%s",
             display_restores, display_owned ? "" : " (not counted: the screen was shared)");
    log_line(line);
#endif
    log_flush();
    wait_keys_released();
    if (wav)
        fclose(wav);
#ifdef UA_MUSIC_PLAYER
    library_close();
#endif
    log_line(lbad ? "bye (log write failed; file may be incomplete)" : "bye (returning keys to the OS)");
    log_flush();
    if (have_csl)
        nio_free(&csl);
#ifdef UA_MUSIC_PLAYER
    display_end();
#endif
    give_keys_back();
    return 0;
}
