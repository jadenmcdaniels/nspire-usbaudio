/* Exercise the actual calculator glue: the old fake-USB suite never built
 * main.c, so it missed masked IRQs, blocking key polling and buffered logs.
 * Only MMIO, CPSR, the OS sleep target and Ndless screen/keys are replaced. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>
#include "t.h"
#define main calculator_main
#include "../src/main.c"
#undef main

static struct { uint32_t at, value; } mmio[128];
static unsigned nmmio, fake_keys, sleep_calls, unmask_calls, restore_calls, invalid_writes;
static uint32_t cpsr;
static const char *fake_documents = "/tmp/";
static unsigned lcd_frames;
static int lcd_mode;
#ifdef UA_MUSIC_PLAYER
static uint16_t displayed[SCREEN_W * SCREEN_H];
#endif
int is_cx2;

static void set(uint32_t at, uint32_t value)
{
    unsigned i;
    for (i = 0; i < nmmio; i++)
        if (mmio[i].at == at) { mmio[i].value = value; return; }
    if (nmmio >= sizeof mmio / sizeof mmio[0]) abort();
    mmio[nmmio].at = at; mmio[nmmio++].value = value;
}
uint32_t ua_test_read(uint32_t at)
{
    unsigned i;
    for (i = 0; i < nmmio; i++)
        if (mmio[i].at == at) return mmio[i].value;
    return 0;
}
void ua_test_write(uint32_t at, uint32_t value)
{
    unsigned i;
#ifdef UA_MUSIC_PLAYER
    if (at == LCD_BASE) { set(at, value); return; }
#endif
    if (at == IRQ_ENABLE) { set(at, w32(at) | value); return; }
    if (at == IRQ_DISABLE) { set(IRQ_ENABLE, w32(IRQ_ENABLE) & ~value); return; }
    for (i = 0; i < sizeof owned_regs / sizeof owned_regs[0]; i++)
        if (at == owned_regs[i]) { set(at, value); return; }
    invalid_writes++;
}
uint32_t ua_test_irq_unmask(void)
{
    uint32_t old = cpsr;
    unmask_calls++;
    cpsr &= ~0x80u;
    return old;
}
void ua_test_irq_restore(uint32_t value) { restore_calls++; cpsr = value; }
static void fake_sleep(unsigned ticks)
{
    T_EQ(ticks, 1);
    T_EQ(cpsr & 0x80, 0);
    T_EQ(w32(IRQ_ENABLE) & KEYPAD_IRQ, 0);
    sleep_calls++;
    /* OS time may turn its keypad/cursor on again. */
    set(IRQ_ENABLE, w32(IRQ_ENABLE) | KEYPAD_IRQ);
    set(LCD_CURSOR, 1);
    set(LCD_CURSOR_XY, 20);
#ifdef UA_MUSIC_PLAYER
    /* The document task can redraw the shared framebuffer during USB time. */
    memset(displayed, 0, sizeof displayed);
    if (display_owned) set(LCD_BASE, display_os_base);
#endif
}
void (*ua_test_bind_sleep(uint32_t at))(unsigned)
{
    T_EQ(at, 0x103b11d0u);
    return fake_sleep;
}
const char *get_documents_dir(void) { return fake_documents; }
static int lcd_kind;            /* what Ndless says the LCD is; SCR_TYPE_INVALID: it does not say */
int lcd_type(void) { return lcd_kind; }
void lcd_init(int mode) { lcd_mode = mode; }
void lcd_blit(const void *pixels, int mode)
{
    T_TRUE(pixels != NULL);
    T_EQ(mode, SCR_320x240_565);
    lcd_frames++;
#ifdef UA_MUSIC_PLAYER
    memcpy(displayed, pixels, sizeof displayed);
#endif
}
unsigned hwtype(void) { return 1; }
unsigned nl_hwsubtype(void) { return is_cx2 ? 2 : 0; }
unsigned nl_ndless_rev(void) { return 2023; }
int isKeyPressed(int key) { return (fake_keys >> key) & 1; }
int any_key_pressed(void) { return fake_keys != 0; }
int nio_init(nio_console *c, int a, int b, int d, int e, int f, int g, int h)
{ (void)c; (void)a; (void)b; (void)d; (void)e; (void)f; (void)g; (void)h; return 0; }
void nio_set_default(nio_console *c) { (void)c; }
void nio_fputs(const char *s, nio_console *c) { (void)s; (void)c; }
void nio_fputc(char ch, nio_console *c) { (void)ch; (void)c; }
void nio_free(nio_console *c) { (void)c; }
int app_run(const struct app_platform *p) { (void)p; return 0; }

static void reset(void)
{
    unsigned i;
    nmmio = sleep_calls = unmask_calls = restore_calls = invalid_writes = 0;
    fake_keys = 0;
    lcd_kind = SCR_TYPE_INVALID;
    cpsr = 0xD3;             /* Ndless starts programs with IRQs masked. */
    keys_owned = 0; tcc_sleep = NULL; sleeps = spins = 0;
#ifdef UA_MUSIC_PLAYER
    display_owned = display_valid = 0;
#endif
    is_cx2 = 1;
    set(IRQ_ENABLE, TICK_IRQ | KEYPAD_IRQ | 2u);
    set(TICK_CONTROL, 0xE6);
    for (i = 0; i < sizeof owned_regs / sizeof owned_regs[0]; i++)
        set(owned_regs[i], 0x101u + i);
    /* The validated sleep's LDR literal and Nucleus current task. */
    set(0x103b11e8u, 0xe59f0000u);
    set(0x103b11f0u, 0x11001000u);
    set(0x11001000u, 0x11002000u);
    set(0x1100200cu, TASK_MAGIC);
    set(0x11002038u, 0);
}

static void test_masked_irq_sleep_and_handback(void)
{
    unsigned model, i;
    for (model = 0; model < 2; model++) {
        reset(); is_cx2 = (int) model;
        take_keys();
        T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
        sleep_tick();
        T_EQ(sleep_calls, 1); T_EQ(sleeps, 1); T_EQ(spins, 0);
        T_EQ(unmask_calls, 1); T_EQ(restore_calls, 1); T_EQ(cpsr, 0xD3);
        T_EQ(w32(IRQ_ENABLE) & KEYPAD_IRQ, 0);
        T_EQ(w32(LCD_CURSOR) & 1, 0);
        if (is_cx2) T_EQ(w32(LCD_CURSOR_XY), 0x01C001C0u);
        /* Hand-back restores controller state, not just the IRQ line. */
        set(owned_regs[0], 99);
        give_keys_back();
        for (i = 0; i < sizeof owned_regs / sizeof owned_regs[0]; i++)
            T_EQ(w32(owned_regs[i]), 0x101u + i);
        T_EQ(w32(IRQ_ENABLE), TICK_IRQ | KEYPAD_IRQ | 2u);
        sleep_tick();              /* must never yield after hand-back */
        T_EQ(sleep_calls, 1); T_EQ(invalid_writes, 0);
    }
    reset(); take_keys(); cpsr = 0x53;
    T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
    sleep_tick();
    T_EQ(cpsr, 0x53);             /* also preserve an initially enabled IRQ */
    give_keys_back();
    reset(); take_keys();
    {
        uint32_t saved = usb_enter();
        T_EQ(cpsr & 0x80, 0);
        T_EQ(w32(IRQ_ENABLE) & KEYPAD_IRQ, 0);
        set(IRQ_ENABLE, w32(IRQ_ENABLE) | KEYPAD_IRQ);
        usb_leave(saved);
        T_EQ(cpsr, 0xD3);
        T_EQ(w32(IRQ_ENABLE) & KEYPAD_IRQ, 0);
    }
    give_keys_back();
}

static void test_invalid_sleep_context(void)
{
    reset(); take_keys();
    set(TICK_CONTROL, 0x46);
    T_TRUE(os_sleep_enable(0x103b11d0u) != NULL);
    sleep_tick(); T_EQ(sleep_calls, 0);
    set(TICK_CONTROL, 0xE6); set(IRQ_ENABLE, 2u);
    T_TRUE(os_sleep_enable(0x103b11d0u) != NULL);
    set(IRQ_ENABLE, TICK_IRQ); set(0x1100200cu, 0);
    T_TRUE(os_sleep_enable(0x103b11d0u) != NULL);
    set(0x1100200cu, TASK_MAGIC);
    T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
    set(0x11002038u, 1);         /* task context changed after discovery */
    sleep_tick(); T_EQ(sleep_calls, 0);
    give_keys_back();
}

static void test_keys_and_bounded_release(void)
{
    reset(); take_keys();
    T_EQ(keys(), 0);
    fake_keys = 1u << KEY_NSPIRE_ESC;
    T_EQ(keys(), A_QUIT); T_EQ(keys(), 0);
    fake_keys = 0; T_EQ(keys(), 0);
    fake_keys = 1u << KEY_NSPIRE_ENTER;
    T_EQ(keys(), A_PAUSE);
    fake_keys = 0; T_EQ(keys(), 0);
    T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
    wait_keys_released(); T_EQ(sleep_calls, 5);
    fake_keys = 1u << KEY_NSPIRE_ESC;
    wait_keys_released(); T_EQ(sleep_calls, 155); /* finite even with a frozen RTC */
    give_keys_back();
}

static void test_log_survives_without_flush(void)
{
    char tmp[] = "/tmp/usbaudio-log.XXXXXX", buf[128];
    FILE *f;
    reset();
    T_TRUE(mkdtemp(tmp) != NULL);
    snprintf(path, sizeof path, "%s/log", tmp);
    llen = 0; lfirst = 1; lbad = 0; have_csl = 0;
    log_line("before OS call");
    /* Read from another handle, with no caller flush or normal shutdown. */
    f = fopen(path, "rb"); T_TRUE(f != NULL);
    if (f) { T_TRUE(fgets(buf, sizeof buf, f) != NULL); T_STR(buf, "before OS call\n"); fclose(f); }
    log_line("after OS call");
    f = fopen(path, "rb"); T_TRUE(f != NULL);
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0;
        T_STR(buf, "before OS call\nafter OS call\n"); fclose(f);
    }
    T_EQ(lbad, 0); T_EQ(llen, 0);
    unlink(path); rmdir(tmp);
}

static void test_port_observation(void)
{
    reset();
    set(USB_BASE,0x01000010);
    set(USB_BASE+0x80,(1u<<21)|(1u<<20)|(1u<<16));
    T_EQ(port_state(),PORT_EMPTY);
    set(USB_BASE+0x80,(1u<<21)|(1u<<20)|(1u<<17));
    T_EQ(port_state(),PORT_PC);
    /* Adapter ID arrived before the OS switches its current role. */
    set(USB_BASE+0x80,(1u<<20)|(1u<<19));
    T_EQ(port_state(),PORT_ADAPTER);
    set(USB_BASE+0x30,0x800);
    T_EQ(port_state(),PORT_STOPPED); T_EQ(port_connected(),1);
    set(USB_BASE+0x10,1);
    T_EQ(port_state(),PORT_DEVICE);
    set(USB_BASE+0x80,0xffffffffu);
    T_EQ(port_state(),PORT_UNKNOWN);
    reset(); is_cx2=0;
    set(USB_BASE,0xfa05); set(USB_BASE+0x100,0x01000040);
    set(USB_BASE+0x1a4,0x120);
    T_EQ(port_state(),PORT_EMPTY);
    set(USB_BASE+0x1a4,0x920);
    T_EQ(port_state(),PORT_PC);
    set(USB_BASE+0x1a4,0x220);
    T_EQ(port_state(),PORT_ADAPTER);
    set(USB_BASE+0x184,0x400);
    T_EQ(port_state(),PORT_STOPPED);
    set(USB_BASE+0x140,1);
    T_EQ(port_state(),PORT_DEVICE);
    set(0x900b0018u,0x20);
    T_EQ(port_state(),PORT_UNKNOWN);
    T_EQ(invalid_writes,0);  /* detection never changes USB registers */
}

#ifdef UA_MUSIC_PLAYER
static void test_private_display(void)
{
    static uint16_t landscape[SCREEN_W * SCREEN_H], expected[SCREEN_W * SCREEN_H];
    struct music_view view = {0};
    view.title = "Private display"; view.status = "Playing";
    /* 0, 1: the LCD timing registers say landscape / portrait and Ndless
     * says nothing; 2: only Ndless knows (portrait); 3: they disagree and
     * Ndless, whose own blit depends on it, is believed (portrait). */
    for (unsigned c = 0; c < 4; c++) {
        unsigned portrait = c != 0;
        reset();
        set(LCD_BASE, 0xA8000000u);
        set(LCD_CONTROL, 6u << 1);
        if (c != 2) {
            set(0xC0000008u, (c == 1 ? 239u : 319u) << 16);
            set(0xC0000004u, c == 1 ? 319u : 239u);
        }
        if (c >= 2) lcd_kind = SCR_240x320_565;
        take_keys(); display_begin();
        music_draw(&view);
        T_TRUE(display_owned); T_TRUE(display_valid);
        T_EQ(display_portrait, portrait);
        screen_render(landscape, &view);
        for (unsigned y = 0; y < SCREEN_H; y++)
            for (unsigned x = 0; x < SCREEN_W; x++)
                expected[portrait ? x * SCREEN_H + (SCREEN_H - 1u - y) : y * SCREEN_W + x] = landscape[y * SCREEN_W + x];
        T_MEM(display_frames[display_front], expected, sizeof expected);
        unsigned before = lcd_frames, front = display_front;
        T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
        T_EQ(display_restores, 0);
        sleep_tick();             /* OS redraws its own buffer and changes the base */
        T_EQ(w32(LCD_BASE), (uint32_t)(uintptr_t)display_frames[front]);
        T_EQ(display_restores, 1); /* counted for the log: how often the OS fights for the LCD */
        music_draw(&view);
        T_EQ(lcd_frames, before); /* no repeated copies into the OS's screen */
        T_EQ(display_front, front);
        T_MEM(display_frames[front], expected, sizeof expected);
        display_end();
        T_EQ(w32(LCD_BASE), 0xA8000000u);
        T_EQ(display_owned, 0);
        give_keys_back();
        T_EQ(invalid_writes, 0);
    }
    /* Nothing says what the LCD is, or it is not in 16-bit mode: the
     * screen stays shared with the OS and LCD_BASE is never written. */
    for (unsigned c = 0; c < 2; c++) {
        reset();
        set(LCD_BASE, 0xA8000000u);
        set(LCD_CONTROL, (c ? 4u : 6u) << 1);
        if (c) lcd_kind = SCR_240x320_565;
        lcd_mode = SCR_TYPE_INVALID;
        take_keys(); display_begin();
        T_EQ(display_owned, 0);
        T_EQ(lcd_mode, SCR_320x240_565);
        music_draw(&view);
        T_EQ(w32(LCD_BASE), 0xA8000000u);
        display_end();
        give_keys_back();
        T_EQ(invalid_writes, 0);
    }
}

/* The frame-list reader of the log, on a made-up CX II schedule: three
 * split-isochronous descriptors, one sent, one waiting 3 frames ahead, one
 * 200 frames away with 4 of its bytes left. Nothing is written. */
static void test_schedule_text(void)
{
    char text[400];
    const uint32_t list = 0x13f02000u, sitd = 0x13f10000u;
    reset();
    T_EQ(usb_frindex(), 0);                         /* controller not recognised */
    usb_sched(text, sizeof text);
    T_STR(text, "siTD: controller not recognised");
    set(USB_BASE, 0x01000010u);
    T_EQ(usb_frindex(), USB_BASE + 0x1C);
    set(USB_BASE + 0x10, 0x00080031u);              /* USBCMD: run, 1024 entries */
    set(USB_BASE + 0x1C, (2048u + 100u) << 3 | 5);  /* FRINDEX: frame 100 of the list, second lap */
    usb_sched(text, sizeof text);
    T_STR(text, "siTD: no frame list at 00000000");
    set(USB_BASE + 0x24, list);
    usb_sched(text, sizeof text);
    T_STR(text, "frame 100, siTD 0: 0 waiting at +0..+0 (0 more than 64 frames away), 0 sent");
    set(list + 4 * 5, 1);                           /* an empty entry */
    set(list + 4 * 7, 0x13f20000u | 2);             /* a queue head: not ours */
    set(list + 4 * 102, sitd | 4);
    set(sitd, 1);
    set(sitd + 12, 176u << 16);                     /* sent: not active */
    set(list + 4 * 103, (sitd + 64) | 4);
    set(sitd + 64, 1);
    set(sitd + 64 + 8, 0x01);
    set(sitd + 64 + 12, 176u << 16 | 0x80);
    set(sitd + 64 + 20, 0x13f31001u);
    set(list + 4 * 300, (sitd + 128) | 4);
    set(sitd + 128, 1);
    set(sitd + 128 + 8, 0x03);
    set(sitd + 128 + 12, 0x80000000u | 4u << 16 | 0x80);
    set(sitd + 128 + 20, 0x13f31000u | 1u << 3 | 1);
    usb_sched(text, sizeof text);
    T_STR(text, "frame 100, siTD 3: 2 waiting at +3..+200 (1 more than 64 frames away), 1 sent; "
                "first far one: slot 300, status 80, 4 bytes left, T-count 1, TP 1, S-mask 03, C-prog 00, IOC 1");
    set(sitd + 128 + 12, 4u << 16);                 /* the far one went out */
    usb_sched(text, sizeof text);
    T_STR(text, "frame 100, siTD 3: 1 waiting at +3..+3 (0 more than 64 frames away), 2 sent; "
                "first waiting: slot 103, status 80, 176 bytes left, T-count 1, TP 0, S-mask 01, C-prog 00, IOC 0");
    reset(); is_cx2 = 0;
    set(USB_BASE, 0xfa05); set(USB_BASE + 0x100, 0x01000040);
    T_EQ(usb_frindex(), USB_BASE + 0x14C);
    T_EQ(invalid_writes, 0);
}

static void test_graphical_exit(void)
{
    char tmp[] = "/tmp/music-main.XXXXXX", documents[80], logfile[128], subdir[128];
    struct music_view view = {0};
    unsigned before;
    static uint16_t expected[SCREEN_W * SCREEN_H];
    reset();
    T_TRUE(mkdtemp(tmp) != NULL);
    snprintf(documents, sizeof documents, "%s/", tmp);
    fake_documents = documents;
    lcd_frames = 0;
    llen = 0; lfirst = 1; lbad = 0;
    keys();
    fake_keys = 1u << KEY_NSPIRE_ESC;
    /* No valid sleep signature: the real error UI must still allow Esc
     * and return the LCD and keypad to the OS. */
    T_EQ(calculator_main(), 0);
    T_EQ(lcd_mode, SCR_TYPE_INVALID);
    T_TRUE(lcd_frames >= 2);
    T_EQ(keys_owned, 0);
    T_EQ(w32(IRQ_ENABLE) & KEYPAD_IRQ, KEYPAD_IRQ);
    T_EQ(invalid_writes, 0);
    snprintf(logfile,sizeof logfile,"%susbaudio/musicplayer_log.txt.tns",documents);
    snprintf(subdir,sizeof subdir,"%susbaudio",documents);
    unlink(logfile); rmdir(subdir); rmdir(tmp);
    fake_documents = "/tmp/";

    reset();
    take_keys();
    view.title = "Dreams and Nightmares - Meek Mill";
    view.status = "Playing";
    view.detail = "Esc exits";
    view.elapsed = 230; view.duration = 231; view.volume = 100;
    view.connected = view.playing = 1;
    before = lcd_frames;
    music_draw(&view);
    T_EQ(lcd_frames,before+1);
    memcpy(expected, displayed, sizeof expected);
    T_TRUE(os_sleep_enable(0x103b11d0u) == NULL);
    sleep_tick();             /* OS overwrites the screen while status stays unchanged */
    music_draw(&view);
    T_EQ(lcd_frames,before+2);
    T_MEM(displayed, expected, sizeof expected);
    view.status = "Audio could not start"; view.error = 1;
    view.playing = 0;
    music_draw(&view);
    T_EQ(lcd_frames,before+3);
    give_keys_back();
}
#endif

int main(void)
{
    T_RUN(test_masked_irq_sleep_and_handback);
    T_RUN(test_invalid_sleep_context);
    T_RUN(test_keys_and_bounded_release);
    T_RUN(test_log_survives_without_flush);
    T_RUN(test_port_observation);
#ifdef UA_MUSIC_PLAYER
    T_RUN(test_schedule_text);
    T_RUN(test_graphical_exit);
    T_RUN(test_private_display);
#endif
    return T_DONE();
}
