/* Music UI state and playback, using the same tested USB/PCM path as the
 * diagnostic app. The stream stays open between songs and sends silence
 * while paused/finished; no ring indices are reset under a live consumer. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "music.h"
#include "player.h"
#include "ua_os.h"

static struct ring audio;
static struct player song;

static uint32_t enter(const struct app_platform *p) { return p->usb_enter ? p->usb_enter() : 0; }
static void leave(const struct app_platform *p, uint32_t v) { if (p->usb_leave) p->usb_leave(v); }

/* Log lines are collected and written together: each write is a file
 * open/append/close during which MAIN is away from the OS, and only 48 ms
 * of audio are queued. The per-second lines go out every five seconds;
 * everything else at once, with what was waiting, in order. */
static char block[8192];
static unsigned block_len;

static void block_write(const struct app_platform *p)
{
    if (block_len) p->log(block);
    block_len = 0;
}

static void block_add(const struct app_platform *p, const char *line)
{
    unsigned n = (unsigned) strlen(line);
    if (n + 2 > sizeof block)
        return;
    if (block_len + n + 2 > sizeof block)
        block_write(p);             /* never lose a line */
    if (block_len) block[block_len++] = '\n';
    memcpy(block + block_len, line, n + 1);
    block_len += n;
}

static void say(const struct app_platform *p, const char *line)
{
    block_add(p, line);
    block_write(p);
}

/* (All the events of a pass go out in one write: the eight of ua_start
 * arrive when the stream has just begun.) */
static void events(const struct app_platform *p)
{
    struct ua_event e;
    char line[180];
    int any = 0;
    while (ua_take_event(&e)) {
        snprintf(line, sizeof line, "music: %s, status %lu (%s), event %u; %lu %lu %lu",
                 ua_step_name(e.step), (unsigned long)e.status, ua_status_name(e.status), e.code,
                 (unsigned long)e.a, (unsigned long)e.b, (unsigned long)e.c);
        block_add(p, line);
        any = 1;
    }
    if (any) block_write(p);
}

static void stream_log(const struct app_platform *p)
{
    struct ua_counters c;
    char line[400];
    ua_counters(&c);
    snprintf(line,sizeof line,"music: USB completed %lu, audio ms %lu, errors %lu, cancelled %lu, "
             "queued %lu, underrun packets %lu, gaps %lu, late ms %lu of %lu queued",
             (unsigned long)c.completions,(unsigned long)c.frames_ok,(unsigned long)c.errors,
             (unsigned long)c.cancelled,(unsigned long)c.inflight,
             (unsigned long)c.underrun_pkts,(unsigned long)c.starved,
             (unsigned long)c.late_frames,(unsigned long)c.queued_frames);
    block_add(p, line);
    if (p->usb_regs) { p->usb_regs(line,sizeof line); block_add(p, line); }
    block_write(p);
}

/* One second of the stream: what was queued, played, late, dropped, when
 * the transfers came back, and where the OS has put the packets. Returns 1
 * if a transfer came back late. */
static int second_log(const struct app_platform *p, unsigned t)
{
    struct ua_second sec;
    char line[520];
    int late;
    memcpy(line, "music: ", 8);
    late = app_second_text(line + 7, sizeof line - 7, t, &sec);
    block_add(p, line);
    if (p->usb_sched) {
        memcpy(line, "music: ", 8);
        p->usb_sched(line + 7, sizeof line - 7);
        block_add(p, line);
    }
    if (t == 1) {
        struct ua_first f[UA_FIRST];
        unsigned n = ua_first(f, UA_FIRST), i, at;
        at = (unsigned) snprintf(line, sizeof line,
                                 "music: first transfers back, ms after they were queued / ms queued ahead:");
        for (i = 0; i < n && at + 12 < sizeof line; i++)
            at += (unsigned) snprintf(line + at, sizeof line - at, " %u/%u", f[i].turn, f[i].ahead);
        block_add(p, line);
    }
    return late;
}

static const char *failure_detail(void)
{
    unsigned step, status;
    ua_failure(&step, &status);
    (void)status;
    switch (step) {
    case S_LAYOUT: case S_ISOC_FN: return "Audio transfer setup failed";
    case S_OPEN_PIPE: return "The calculator refused the audio connection";
    case S_TRANSFER: return "The calculator refused audio packets";
    case S_CHOOSE: return "This device has no supported audio output";
    case S_SET_IFACE: case S_SET_IFACE_REQ: return "Could not select the audio output";
    case S_CALIBRATE: return "The audio adapter did not answer";
    default: return "Reconnect the adapter to try again";
    }
}

static int load(const struct app_platform *p, unsigned index, uint32_t rate, uint32_t *start)
{
    struct music_track t;
    char line[180];
    *start = audio.wr;
    if (!p->music->open(index, &t))
        return 0;
    player_init(&song, rate, t.read, t.ctx, t.size);
    player_next(&song);         /* start the file immediately: no test melody */
    snprintf(line, sizeof line, "music: open %s: %s", p->music->title(index),
             song.wav_ok ? "PCM WAV" : song.wav_why);
    say(p, line);
    if (!song.wav_ok)
        return 0;
    player_fill(&song, &audio, RING_FRAMES);
    return song.read_errors == 0;
}

int music_run(const struct app_platform *p)
{
    const struct music_platform *m = p->music;
    struct music_view v = {0};
    uint32_t began = p->seconds(), start = 0, last_done = 0, last_progress = began, last_log = began;
    uint32_t stream_began = began, last_second = began;
    unsigned index = 0, volume = 128, repeat = 0, draining = 0;
    int paused = 0, finished = 0, bad_file = 0, stream_error = 0, failure = 0;
    int late_told = 0, lagging = 0;
    int init = ua_init(), previous_port = -1;
    uint32_t port_since = began;
    block_len = 0;
    ring_init(&audio);
    memset(&song, 0, sizeof song);
    events(p);
    if (init < 0) {
        say(p, ua_init_text());
        failure = 1;
    }
    ua_set_volume(volume);
    for (;;) {
        unsigned k;
        uint32_t now;
        int st, change = 0, port;
        struct ua_counters c;
        p->sleep_tick();
        k = p->keys();
        now = p->seconds();
        events(p);
        st = ua_state();
        if (st == UA_GONE) {
            /* Detach may leave an abort or close pending. Keep servicing
             * it so those old resources cannot block the next adapter. */
            uint32_t saved = enter(p);
            ua_stop(NULL);
            leave(p, saved);
        }
        port = p->port_state ? p->port_state() : PORT_UNKNOWN;
        if (port != previous_port) {
            char line[80];
            snprintf(line,sizeof line,"music: USB port state %d -> %d",previous_port,port);
            say(p, line);
            previous_port = port;
            port_since = now;
        }
        if ((k & A_QUIT) || (p->max_seconds && now - began >= p->max_seconds))
            break;
        if (k & A_REPEAT) repeat = !repeat;
        if (k & A_VOLUP) volume = volume > 240 ? 256 : volume + 16;
        if (k & A_VOLDN) volume = volume < 16 ? 0 : volume - 16;
        ua_set_volume(volume);
        if (m->count) {
            if (k & A_NEXT) { index = (index + 1) % m->count; change = 1; }
            if (k & A_PREV) { index = (index + m->count - 1) % m->count; change = 1; }
            if (k & A_RESTART) change = 1;
            if (k & A_PAUSE) {
                if (finished || bad_file) { change = 1; paused = 0; }
                else paused = !paused;
            }
        }
        if (st == UA_READY) {
            uint32_t saved;
            ring_init(&audio);  /* no consumer yet; old device is gone */
            bad_file = m->count ? !load(p, index, ua_desc()->rate, &start) : 1;
            finished = stream_error = draining = 0;
            ua_set_pause(paused || bad_file);
            ua_set_frame_clock(p->usb_frindex ? p->usb_frindex() : 0);
            saved = enter(p);
            if (ua_start(&audio) != 0) failure = 1;
            leave(p, saved);
            st = ua_state();
            ua_counters(&c);
            last_done = c.completions;
            last_progress = now;
            stream_began = last_second = last_log = now;
            late_told = lagging = 0;
        } else if (st == UA_STREAMING && change) {
            bad_file = !load(p, index, ua_desc()->rate, &start);
            finished = draining = 0;
        }
        if (st == UA_STREAMING) {
            ua_set_pause(paused || finished || bad_file || stream_error);
            if (!paused && !finished && !bad_file && !stream_error) {
                player_fill(&song, &audio, RING_FRAMES);
                if (song.read_errors) bad_file = 1;
                if (song.stage == PL_DONE && ring_used(&audio) == 0) {
                    if (++draining >= 6) {  /* also let the queued USB packets drain */
                        if (repeat || index + 1 < m->count) {
                            if (!repeat) index++;
                            bad_file = !load(p, index, ua_desc()->rate, &start);
                            draining = 0;
                        } else finished = 1;
                    }
                } else draining = 0;
            }
            ua_counters(&c);
            if (c.completions != last_done) { last_done = c.completions; last_progress = now; }
            if ((!c.inflight && c.completions) || now - last_progress >= 3 || c.errors) {
                if (!stream_error) {
                    block_add(p, "music: USB transfers failed or stopped completing");
                    stream_log(p);
                }
                stream_error = failure = 1;
            }
            if (now != last_second) {
                last_second = now;
                lagging = second_log(p, (unsigned)(now - stream_began));
                if (lagging && !late_told) {
                    late_told = 1;
                    block_add(p, "music: " APP_LATE_TEXT);
                }
            }
            if (now - last_log >= 5) { stream_log(p); last_log = now; }
        } else if (block_len) {
            block_write(p);         /* the stream ended (unplugged): what was not written yet */
        }
        if (st == UA_FAILED) failure = 1;
        v.title = m->count ? m->title(index) : "No songs found";
        v.count = m->count; v.index = index;
        v.volume = volume * 100 / 256; v.repeat = repeat;
        v.duration = (!bad_file && song.wav_ok) ? song.wi.data_len / song.wi.block_align / song.wi.rate : 0;
        v.elapsed = 0;
        if (st == UA_STREAMING && !bad_file && song.out_rate) {
            int32_t consumed = (int32_t)(audio.rd - start);
            if (consumed > 0) v.elapsed = (uint32_t)consumed / song.out_rate;
            if (v.elapsed > v.duration) v.elapsed = v.duration;
        }
        v.status = init < 0 ? "USB driver could not start" : !m->count ? "No songs found" :
            st == UA_WAITING || st == UA_GONE ?
                (port == PORT_PC ? "Computer cable connected" :
                 port == PORT_ADAPTER ? "USB adapter detected" :
                 port == PORT_STOPPED ? "Audio device detected" :
                 port == PORT_DEVICE ? "Connecting audio adapter..." : "Connect USB audio adapter") :
            st == UA_SETUP ? "Connecting audio adapter..." : st == UA_FAILED ? "Audio could not start" :
            stream_error ? "USB audio stalled; reconnect" : bad_file ? "Cannot read PCM WAV; choose next" :
            finished ? "Finished - Enter to replay" : paused ? "Paused" :
            lagging ? "Playing - sound lags, see log" : "Playing";
        v.connected = st == UA_READY || st == UA_STREAMING || st == UA_FAILED || st == UA_SETUP;
        v.error = init < 0 || st == UA_FAILED || stream_error || bad_file;
        v.playing = st == UA_STREAMING && !paused && !finished && !bad_file && !stream_error && last_done;
        if (st == UA_STREAMING && !last_done && !v.error) v.status = "Starting playback...";
        v.detail = init < 0 ? "Press Esc to return to Documents" :
            st == UA_FAILED ? failure_detail() : stream_error ? "No audio is being sent. Esc exits." :
            !m->count ? "Add .wav.tns songs to My Documents/music" :
            st == UA_WAITING || st == UA_GONE ?
                (port == PORT_PC ? "Replace the PC cable with your audio adapter" :
                 port == PORT_ADAPTER ? "Plug the sound card into the USB adapter" :
                 port == PORT_STOPPED ? "Waiting for the calculator's USB host" :
                 port == PORT_DEVICE && now-port_since >= 4 ? "Unplug and reconnect the sound card" :
                 "Plug headphones into the sound card") :
            st == UA_SETUP ? "Preparing the audio connection" :
            "R repeat    Del restart    Esc exit";
        if (m->draw) m->draw(&v);
    }
    block_add(p, "music: stopping USB playback");
    stream_log(p);
    for (;;) {
        uint32_t saved = enter(p);
        int pending = ua_shutdown(p->sleep_tick);
        leave(p, saved);
        events(p);
        if (!pending) break;
        v.status = "Unplug adapter to finish closing";
        if (m->draw) m->draw(&v);
        p->sleep_tick();
    }
    p->flush();
    return failure;
}
