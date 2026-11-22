/* The main loop: wait for a device, describe it, stream, report. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "player.h"
#include "ua_os.h"
#include "uac.h"
#include "music.h"

static const struct app_platform *P;
static struct ring ring;
static struct player pl;

static uint32_t usb_enter(void) { return P->usb_enter ? P->usb_enter() : 0; }
static void usb_leave(uint32_t saved) { if (P->usb_leave) P->usb_leave(saved); }

static void stop_stream(void)
{
    uint32_t saved = usb_enter();
    ua_stop(P->sleep_tick);
    usb_leave(saved);
}

int app_second_text(char *buf, unsigned size, unsigned t, struct ua_second *s)
{
    unsigned long ok;
    int k;
    ua_take_second(s);
    ok = s->done_ms / UA_NF;
    k = snprintf(buf, size, "t=%us queued %lu ms, played %lu (late %lu), dropped %lu (silence %lu, failed %lu)",
                 t, (unsigned long) s->queued_ms, (unsigned long) s->done_ms, (unsigned long) s->late_ms,
                 (unsigned long) (s->silence_ms + s->failed_ms), (unsigned long) s->silence_ms,
                 (unsigned long) s->failed_ms);
    if (k < 0 || (unsigned) k >= size)
        return s->late_ms != 0;
    if (!s->timed)
        k += snprintf(buf + k, size - (unsigned) k, "; %lu transfers back (no frame clock: no times)", ok);
    else if (!ok)
        k += snprintf(buf + k, size - (unsigned) k, "; no transfer came back");
    else
        k += snprintf(buf + k, size - (unsigned) k, "; %lu transfers back after %lu-%lu ms (avg %lu), %lu-%lu ms "
                      "after their last packet (avg %lu), at most %lu ms apart", ok, (unsigned long) s->turn_min,
                      (unsigned long) s->turn_max, (unsigned long) (s->turn_sum / ok), (unsigned long) s->wait_min,
                      (unsigned long) s->wait_max, (unsigned long) (s->wait_sum / ok), (unsigned long) s->apart_max);
    if (k > 0 && (unsigned) k < size)
        snprintf(buf + k, size - (unsigned) k, "; least queued ahead %ld ms, restarts %lu",
                 s->ahead_min == 0xFFFFFFFFu ? -1L : (long) s->ahead_min, (unsigned long) s->restarts);
    return s->late_ms != 0;
}

/* While holding, lines are collected and written by hold_end() in one
 * piece: a write keeps MAIN away from the OS. */
static int holding;
static char held[6000];
static unsigned held_len;

static void hold_end(void)
{
    holding = 0;
    if (held_len) P->log(held);
    held_len = 0;
}

static void out(const char *line)
{
    unsigned n = (unsigned) strlen(line);
    if (!holding || n + 2 > sizeof held) {
        if (held_len) P->log(held);
        held_len = 0;
        P->log(line);
        return;
    }
    if (held_len + n + 2 > sizeof held) {
        P->log(held);
        held_len = 0;
    }
    if (held_len) held[held_len++] = '\n';
    memcpy(held + held_len, line, n + 1);
    held_len += n;
}

static void L(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void L(const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    out(line);
}

static void emit(void *ctx, const char *line)
{
    (void) ctx;
    out(line);
}

static void regs(const char *when)
{
    char b[512];
    if (!P->usb_regs)
        return;
    b[0] = 0;
    P->usb_regs(b, sizeof b);
    L("USB controller (%s): %s", when, b);
}

static const char *st_text(unsigned s)
{
    static char b[80];
    if (s == 0x100)
        return "the OS has no such call";
    snprintf(b, sizeof b, "status %u %s, OS says \"%s\"", s, ua_status_name(s), ua_errstr(s));
    return b;
}

static void describe_fmt(const struct uac_fmt *f, int i, int chosen)
{
    char rates[90];
    unsigned k, n = 0;
    rates[0] = 0;
    if (f->rate_continuous)
        snprintf(rates, sizeof rates, "%lu..%lu Hz", (unsigned long) f->rate_min, (unsigned long) f->rate_max);
    for (k = 0; k < f->nrates && n + 10 < sizeof rates; k++)
        n += (unsigned) snprintf(rates + n, sizeof rates - n, "%s%lu", k ? "/" : "", (unsigned long) f->rates[k]);
    L("%s stream %d: interface %u alt %u, %s, class %u, %s%s, %u ch, %u-byte samples (%u bits), rates %s",
      chosen ? "*" : " ", i, f->ifnum, f->alt, f->dir_in ? "IN (microphone)" : "OUT (playback)", f->uac_ver,
      f->format_type == 1 ? "type I " : "other type ", f->format_tag == 1 ? "PCM" : f->format_tag == 2 ? "PCM8" : "not PCM",
      f->channels, f->subframe, f->bits, rates[0] ? rates : "none listed");
    L("    endpoint %02x isochronous, sync %s, max packet %u bytes, interval %u%s%s, rate control %s",
      f->ep_addr, uac_sync_name(f->ep_sync), f->ep_maxpkt, f->ep_interval,
      f->fb_addr ? ", feedback endpoint " : "", f->fb_addr ? (f->fb_addr & 0x80 ? "IN" : "OUT") : "",
      f->have_cs_ep ? ((f->cs_ep_attr & 1) ? "yes" : "no") : "unknown");
}

static void describe_device(void)
{
    const struct ua_desc *d = ua_desc();
    const uint8_t *cfg = d->cfg_dev_len >= d->cfg_os_len ? d->cfg_dev : d->cfg_os;
    unsigned len = d->cfg_dev_len >= d->cfg_os_len ? d->cfg_dev_len : d->cfg_os_len, i;
    L("--- device %04x:%04x descriptors ---", d->vid, d->pid);
    uac_dump_device(d->have_dev ? d->dev : NULL, d->have_dev ? 18 : 0, emit, NULL);
    L("configuration: the OS's copy %u bytes, from the device %u bytes%s", d->cfg_os_len, d->cfg_dev_len,
      d->cfg_os_len && d->cfg_dev_len ? (d->cfg_os_len == d->cfg_dev_len && !memcmp(d->cfg_os, d->cfg_dev, d->cfg_os_len)
                                          ? " (identical)" : " (they differ: using the longer)") : "");
    uac_dump(cfg, len, emit, NULL);
    L("--- decoded ---");
    L("audio control: interface %u, class %u (ADC %x.%02x)%s, %u unit(s)%s", d->info.ac_ifnum, d->info.ac_ver,
      d->info.bcd_adc >> 8, d->info.bcd_adc & 0xFF, d->info.any_uac2 ? ", USB Audio Class 2 present" : "",
      d->info.nunits, d->info.truncated ? ", descriptors cut short" : "");
    for (i = 0; i < d->info.nfmt; i++)
        describe_fmt(&d->info.fmt[i], (int) i, (int) i == d->chosen);
    if (d->chosen >= 0) {
        const struct uac_fmt *f = &d->info.fmt[d->chosen];
        const struct uac_unit *fu = uac_feature_unit(&d->info, f);
        L("chosen: stream %d at %lu Hz: packets of %u-%u bytes every 1 ms", d->chosen, (unsigned long) d->rate,
          (unsigned) (d->rate / 1000) * f->channels * f->subframe, uac_max_packet(f, d->rate));
        if (fu)
            L("feature unit %u: master controls %02x, channel 1 controls %02x (bit 0 mute, bit 1 volume)", fu->id,
              fu->ctl_master, fu->ctl_ch1);
        else
            L("feature unit: none on the playback path");
        if (f->ep_sync == UAC_SYNC_ASYNC)
            L("note: asynchronous endpoint; its feedback is not read, the nominal rate is sent");
    } else {
        L("no playable stream: %s", d->why ? d->why : "?");
        if (d->info.any_uac2 && !d->info.n_out)
            L("this looks like a USB Audio Class 2 only device: not supported by this version");
    }
}

static void on_step(const struct ua_event *e)
{
    const struct ua_osinfo *o = ua_osinfo();
    switch (e->step) {
    case S_RESOLVE:
        if (e->status == 0x100) {
            L("OS call missing: syscall %lu (%s)", (unsigned long) e->a, ua_fn_name(e->b));
            return;
        }
        {
            unsigned i;
            for (i = 0; i < UA_NFN; i += 3)
                L("  %s=%08lx %s=%08lx %s=%08lx", ua_fn_name(i), (unsigned long) o->addr[i],
                  i + 1 < UA_NFN ? ua_fn_name(i + 1) : "-", (unsigned long) (i + 1 < UA_NFN ? o->addr[i + 1] : 0),
                  i + 2 < UA_NFN ? ua_fn_name(i + 2) : "-", (unsigned long) (i + 2 < UA_NFN ? o->addr[i + 2] : 0));
            L("usbd_setup_isoc_xfer: %s; words there %08lx %08lx %08lx %08lx, word before %08lx",
              o->isoc_how == 1 ? "in the syscall table" : o->isoc_how == 2 ?
              "not in the table: derived from usbd_setup_xfer (+0xC4 or +0xE0, function start checked)" : "NOT FOUND",
              (unsigned long) o->isoc_words[0], (unsigned long) o->isoc_words[1], (unsigned long) o->isoc_words[2],
              (unsigned long) o->isoc_words[3], (unsigned long) o->word_before);
        }
        return;
    case S_REGISTER:
        L("driver registered with the OS");
        return;
    case S_MATCH_DEV:
        L("OS offers a device %04lx:%04lx (device class %02lx)", (unsigned long) (e->a >> 16),
          (unsigned long) (e->a & 0xFFFF), (unsigned long) e->b);
        return;
    case S_MATCH_IF:
        L("OS offers interface %lu, class %02lx/%02lx/%02lx%s", (unsigned long) (e->b >> 24),
          (unsigned long) (e->b >> 16 & 0xFF), (unsigned long) (e->b >> 8 & 0xFF), (unsigned long) (e->b & 0xFF),
          (e->b >> 16 & 0xFF) == 1 && (e->b >> 8 & 0xFF) == 2 ? " (audio streaming)" : "");
        return;
    case S_ATTACH:
        L("attach: device %04lx:%04lx interface %lu", (unsigned long) (e->a >> 16), (unsigned long) (e->a & 0xFFFF),
          (unsigned long) e->b);
        return;
    case S_CONFIG_DESC:
        L("usbd_get_config_descriptor: %s, %lu bytes", e->b ? "ok" : "NULL", (unsigned long) e->a);
        return;
    case S_CALIBRATE:
        L("control buffer form: %s (GET_DESCRIPTOR %s)", e->a == 1 ? "plain pointer" : e->a == 2 ?
          "TI descriptor {buf,0,0}" : "UNKNOWN", st_text(e->status));
        return;
    case S_PARSE:
        L("parsed %lu bytes: %lu stream(s)%s", (unsigned long) e->a, (unsigned long) e->b,
          e->c ? ", class 2 interfaces present" : "");
        return;
    case S_CHOOSE:
        if (e->status)
            L("no playable stream (%s)", ua_desc()->why);
        else if (e->c == 2)
            L("interface %lu offered, playback is on interface %lu: waiting for that one", (unsigned long) e->b,
              (unsigned long) e->a);
        else
            L("chosen: interface %lu alt %lu at %lu Hz", (unsigned long) (e->b & 0xFF), (unsigned long) (e->b >> 8),
              (unsigned long) e->a);
        return;
    case S_GET_CFG9:
    case S_GET_CFG:
        L("GET_DESCRIPTOR configuration (%s): %s", e->step == S_GET_CFG9 ? "9 bytes" : "all", st_text(e->status));
        return;
    case S_SET_IFACE:
    case S_SET_IFACE_REQ:
        L("%s interface %lu alt %lu: %s", e->step == S_SET_IFACE ? "usbd_set_interface" :
          "SET_INTERFACE request (no usbd_set_interface on this OS)", (unsigned long) e->b, (unsigned long) e->a,
          st_text(e->status));
        return;
    case S_SET_RATE:
        if (e->c == 1)
            L("sample rate: the stream has one fixed rate (%lu Hz), nothing to set", (unsigned long) e->a);
        else
            L("SET_CUR sampling frequency %lu Hz on endpoint %02lx: %s", (unsigned long) e->a, (unsigned long) e->b,
              st_text(e->status));
        return;
    case S_GET_RATE:
        L("GET_CUR sampling frequency: %s, device says %lu Hz", st_text(e->status), (unsigned long) e->a);
        return;
    case S_FU_MUTE:
        if (e->c == 1)
            L("no feature unit: no mute/volume to set");
        else
            L("unmute (feature unit %lu channel %lu): %s", (unsigned long) e->a, (unsigned long) e->b, st_text(e->status));
        return;
    case S_FU_VOL_RANGE:
        L("volume range channel %lu: %ld..%ld (1/256 dB): %s", (unsigned long) e->c, (long) (int16_t) e->a,
          (long) (int16_t) e->b, st_text(e->status));
        return;
    case S_FU_VOL_SET:
        L("volume %ld/256 dB channel %lu: %s", (long) (int16_t) e->a, (unsigned long) e->b, st_text(e->status));
        return;
    case S_OPEN_PIPE:
        L("usbd_open_pipe endpoint %02lx (max packet %lu): %s", (unsigned long) e->a, (unsigned long) e->c,
          st_text(e->status));
        return;
    case S_ALLOC_XFER:
        L("usbd_alloc_xfer x%u: %s", UA_NX, e->status ? "FAILED" : "ok");
        return;
    case S_ISOC_FN:
        L("usbd_setup_isoc_xfer at %08lx (%s)", (unsigned long) e->a, e->b == 1 ? "syscall" : "derived");
        return;
    case S_LAYOUT:
        L("transfer layout probe: length@%d buffer@%d frlengths@%d nframes@%d (words), method %lu: %s",
          (int8_t) (e->a >> 24), (int8_t) (e->a >> 16), (int8_t) (e->a >> 8), (int8_t) e->a, (unsigned long) e->c,
          e->status ? "NOT the expected usbd_setup_isoc_xfer: nothing sent" : "ok");
        return;
    case S_TRANSFER:
        L("usbd_transfer #%lu (%lu bytes, %u packets): %s", (unsigned long) e->a, (unsigned long) e->b, UA_NF,
          st_text(e->status));
        return;
    case S_DETACH:
        L("detach (device unplugged) while %s", ua_state_name((int) e->a));
        return;
    case S_ABORT:
        L("usbd_abort_pipe: %s", st_text(e->status));
        return;
    case S_CLOSE:
        L("usbd_close_pipe: %s", st_text(e->status));
        return;
    case S_STOP:
        L("stream stopped: %s (%lu completions, %lu cancelled)", e->status ? "a transfer did not come back" : "clean",
          (unsigned long) e->a, (unsigned long) e->b);
        return;
    case S_EP_SIZE:
        if (e->status)
            L("wMaxPacketSize in the OS's copy of the descriptors: endpoint not found there, nothing written");
        else
            L("wMaxPacketSize in the OS's copy of the descriptors: %lu, %s %lu (our largest packet: %lu bytes)",
              (unsigned long) e->a, e->a != e->b ? "lowered to" : "left at", (unsigned long) e->b,
              (unsigned long) e->c);
        return;
    default:
        L("step %s: %s", ua_step_name(e->step), st_text(e->status));
    }
}

static void explain_failure(unsigned s, unsigned status)
{
    L("STOPPED at: %s, %s", ua_step_name(s), st_text(status));
    switch (s) {
    case S_OPEN_PIPE:
        L("MEANING: the OS's USB stack refuses an isochronous pipe on this controller (old BSD EHCI code had none).");
        L("The OS path cannot play audio here; fallbacks are in NOTES.txt.");
        break;
    case S_TRANSFER:
        L("MEANING: the pipe opened but the OS refuses isochronous transfers on this controller.");
        L("The OS path cannot play audio here; fallbacks are in NOTES.txt.");
        break;
    case S_ISOC_FN:
        L("MEANING: usbd_setup_isoc_xfer could not be found on this OS version (see the words logged above).");
        break;
    case S_LAYOUT:
        L("MEANING: the function taken for usbd_setup_isoc_xfer did not set up a transfer as expected; nothing was sent.");
        break;
    case S_CHOOSE:
        L("MEANING: this device has no USB Audio Class 1 PCM playback stream this program can use.");
        break;
    case S_CALIBRATE:
        L("MEANING: the control-transfer buffer form could not be learnt; no request with data was sent.");
        break;
    case S_SET_IFACE:
    case S_SET_IFACE_REQ:
        L("MEANING: selecting the streaming alternate setting failed.");
        break;
    case S_ALLOC_XFER:
        L("MEANING: the OS ran out of transfer memory.");
        break;
    default:
        break;
    }
}

static void show_events(void)
{
    struct ua_event e;
    int have_desc = 0;
    while (ua_take_event(&e)) {
        switch (e.code) {
        case EV_STEP: on_step(&e); break;
        /* Save the setup result before the long descriptor dump. */
        case EV_DESC: have_desc = 1; break;
        case EV_FAIL: explain_failure(e.step, e.status); regs("at the failure"); P->flush(); break;
        case EV_FIRST_ERR:
            L("FIRST TRANSFER ERROR: %s, %s, transfer slot %lu, after %lu completions", ua_step_name(e.step),
              st_text(e.status), (unsigned long) e.a, (unsigned long) e.b);
            if (e.step == S_TRANSFER_CB || e.step == S_TRANSFER)
                L("MEANING: the OS refused to queue an isochronous transfer.");
            else
                L("MEANING: isochronous transfers complete with an error on this controller.");
            regs("at the first error");
            P->flush();
            break;
        default: break;
        }
    }
    if (have_desc) {
        describe_device();
        P->flush();
    }
    if (ua_events_lost())
        L("(%lu log events were lost)", (unsigned long) ua_events_lost());
}

int app_run(const struct app_platform *pf)
{
    uint32_t t0, last_s, last_wait_s, start_s = 0, last_completions = 0, still_s = 0;
    unsigned vol = 192, ticks = 0, drain_ticks = 0;
    int st, prev = -1, paused = 0, plugged_before, hint_done = 0, nocomp_told = 0, finished = 0, failed = 0;
    int r, last_port, late_told = 0;

    if (pf->music)
        return music_run(pf);
    P = pf;
    ring_init(&ring);
    L("starting the USB audio driver");
    r = ua_init();
    show_events();
    if (r < 0) {
        L("cannot start: %s", ua_init_text());
        P->flush();
        return 1;
    }
    plugged_before = P->port_connected ? P->port_connected() : -1;
    regs("at start");
    L("Plug in a USB audio device (Esc quits)");
    if (plugged_before == 1)
        L("a device is already on the port (plugged in before start): waiting to see if the OS offers it");
    else if (plugged_before == 0)
        L("nothing on the port yet (plugged in after start)");
    P->flush();
    t0 = last_s = last_wait_s = P->seconds();
    last_port = plugged_before;
    ua_set_volume(vol);

    for (;;) {
        unsigned k;
        uint32_t now;
        P->sleep_tick();
        ticks++;
        show_events();
        st = ua_state();
        now = P->seconds();
        k = P->keys();
        if (k & A_QUIT) {
            L("Esc: quit");
            break;
        }
        if (P->max_seconds && now - t0 >= P->max_seconds) {
            L("time limit");
            break;
        }
        if (st != prev) {
            L("state: %s", ua_state_name(st));
            prev = st;
        }
        if (st == UA_WAITING || st == UA_GONE) {
            int port = P->port_connected ? P->port_connected() : -1;
            if (port != last_port || now - last_wait_s >= 5) {
                L("waiting %lu s: port %s, OS probes %lu (Esc quits)", (unsigned long) (now - t0),
                  port == 1 ? "connected" : port == 0 ? "not connected" : "unknown",
                  (unsigned long) ua_seen_probes());
                regs("waiting");
                P->flush();
                last_port = port;
                last_wait_s = now;
            }
        }
        if (st == UA_WAITING && !hint_done) {
            uint32_t wait = P->unplug_wait_s ? P->unplug_wait_s : 4;
            if (ua_seen_probes() == 0 && now - t0 >= wait) {
                hint_done = 1;
                if (plugged_before == 1 || (P->port_connected && P->port_connected() == 1)) {
                    L("the port is connected but the OS has not offered the device to this driver.");
                    L("Unplug the USB audio device and plug it in again.");
                } else {
                    L("still no device offered: is the OTG adapter in and the device plugged in?");
                }
                regs("waiting");
                P->flush();
            }
        }
        if (st == UA_READY) {
            const struct ua_desc *d = ua_desc();
            uint32_t saved;
            player_init(&pl, d->rate, pf->wav_read, pf->wav_ctx, pf->wav_size);
            ring_init(&ring);
            player_fill(&pl, &ring, RING_FRAMES);
            regs("before streaming");
            L("streaming: %s, %lu ms", player_stage_name(pl.stage), (unsigned long) melody_ms(&pl.mel));
            P->flush();
            ua_set_frame_clock(P->usb_frindex ? P->usb_frindex() : 0);
            saved = usb_enter();
            r = ua_start(&ring);
            usb_leave(saved);
            /* The stream runs from here on: its start goes into the log in
             * one write, not one per line (48 ms are queued). */
            holding = 1;
            show_events();
            if (r == 0)
                L("streaming started: %u transfers of %u ms queued", UA_NX, UA_NF);
            hold_end();
            if (r == 0) {
                P->flush();
                start_s = now;
                last_completions = 0;
                still_s = now;
                nocomp_told = 0;
                late_told = 0;
                finished = 0;
                drain_ticks = 0;
            }
            continue;
        }
        if (st == UA_STREAMING) {
            struct ua_counters c;
            int prev_stage = pl.stage;
            if (k & A_PAUSE) {
                paused = !paused;
                ua_set_pause(paused);
                L(paused ? "paused" : "playing");
            }
            if (k & A_NEXT) {
                player_next(&pl);       /* what is already in the ring (< 0.2 s) still plays */
                L("next: %s", player_stage_name(pl.stage));
            }
            if (k & (A_VOLUP | A_VOLDN)) {
                vol = (k & A_VOLUP) ? (vol + 32 > 256 ? 256 : vol + 32) : (vol < 32 ? 0 : vol - 32);
                ua_set_volume(vol);
                L("volume %u/256", vol);
            }
            if (!paused)
                player_fill(&pl, &ring, RING_FRAMES);
            if (pl.stage != prev_stage) {
                if (pl.stage == PL_WAV)
                    L("music.wav: %lu Hz, %u ch, %u bits, %lu bytes of samples", (unsigned long) pl.wi.rate,
                      pl.wi.channels, pl.wi.bits, (unsigned long) pl.wi.data_len);
                else if (prev_stage == PL_MELODY)
                    L("music.wav: %s", pl.wav_why);
            }
            ua_counters(&c);
            if (now != last_s) {
                /* One write for the second: the totals, the second, the frame list. */
                struct ua_second sec;
                char second[420], sched[260];
                int late = app_second_text(second, sizeof second, (unsigned) (now - start_s), &sec);
                sched[0] = 0;
                if (P->usb_sched)
                    P->usb_sched(sched, sizeof sched);
                L("t=%lus sent %lu ms ok, errors %lu, underruns %lu pkts (%lu frames), gaps %lu, min queued %ld ms, "
                  "in flight %lu, ring %u\n%s%s%s",
                  (unsigned long) (now - start_s), (unsigned long) c.frames_ok, (unsigned long) c.errors,
                  (unsigned long) c.underrun_pkts, (unsigned long) c.underrun_frames, (unsigned long) c.starved,
                  c.min_queued_ms == 0xFFFFFFFFu ? -1L : (long) c.min_queued_ms, (unsigned long) c.inflight,
                  ring_used(&ring), second, sched[0] ? "\n" : "", sched);
                ua_counters_reset_min();
                if (now - start_s == 1) {
                    struct ua_first f[UA_FIRST];
                    char list[UA_FIRST * 12 + 4];
                    unsigned nf = ua_first(f, UA_FIRST), i, at = 0;
                    list[0] = 0;
                    for (i = 0; i < nf && at + 12 < sizeof list; i++)
                        at += (unsigned) snprintf(list + at, sizeof list - at, " %u/%u", f[i].turn, f[i].ahead);
                    L("first transfers back, ms after they were queued / ms queued ahead of them:%s", list);
                }
                if (late && !late_told) {
                    late_told = 1;
                    L(APP_LATE_TEXT);
                    regs("late transfers");
                    P->flush();
                }
                if (now - start_s == 1)
                    regs("after 1 s of streaming");
                if (((now - start_s) % 5) == 0)
                    P->flush();
                last_s = now;
            }
            if (c.completions != last_completions) {
                last_completions = c.completions;
                still_s = now;
            } else if (!nocomp_told && now - still_s >= 3) {
                nocomp_told = 1;
                L("NO COMPLETION for 3 s: %lu transfers queued, none came back (%lu completions in all)",
                  (unsigned long) c.inflight, (unsigned long) c.completions);
                L("MEANING: the OS accepted the transfers but the controller never ran them (no isochronous schedule).");
                regs("no completion");
                P->flush();
            }
            if (c.inflight == 0 && c.completions && !finished) {
                L("no transfer in flight any more: streaming has stopped (errors %lu)", (unsigned long) c.errors);
                finished = 1;
                failed = 1;
                P->flush();
            }
            if (pl.stage == PL_DONE && ring_used(&ring) == 0 && !finished && ++drain_ticks > 6) {
                finished = 1;               /* (the last 48 ms queued have played by now) */
                L("finished playing: %lu frames melody, %lu frames music.wav", (unsigned long) pl.frames_out[0],
                  (unsigned long) pl.frames_out[1]);
                L("RESULT: audio was streamed through the OS's USB stack (%lu ms sent, %lu errors)",
                  (unsigned long) c.frames_ok, (unsigned long) c.errors);
                stop_stream();
                show_events();
                L("Esc quits");
                P->flush();
            }
            continue;
        }
        if (st == UA_FAILED) {
            failed = 1;
        }
        (void) ticks;
    }
    {
        struct ua_counters c;
        ua_counters(&c);
        L("stopping: %lu completions, %lu ms sent, %lu errors, %lu underrun packets", (unsigned long) c.completions,
          (unsigned long) c.frames_ok, (unsigned long) c.errors, (unsigned long) c.underrun_pkts);
        L("closing the pipe and retiring the driver");
        P->flush();
    }
    {
        int pending, told = 0;
        do {
            uint32_t saved = usb_enter();
            pending = ua_shutdown(P->sleep_tick);
            usb_leave(saved);
            show_events();
            if (pending) {
                if (!told) {
                    L("waiting for the OS's USB setup to return; unplug the adapter if it is stuck");
                    P->flush();
                    told = 1;
                }
                P->sleep_tick();
            }
        } while (pending);
    }
    show_events();
    L("driver retired; USB is the OS's again");
    P->flush();
    return failed;
}
