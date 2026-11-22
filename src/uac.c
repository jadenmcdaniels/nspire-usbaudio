/* USB Audio Class descriptor parsing and decoding (see uac.h).
 * References: USB Device Class Definition for Audio Devices 1.0 (1998) and
 * its Audio Data Formats 1.0; Audio 2.0 (2006) for recognising class 2. */
#include <stdio.h>
#include <string.h>
#include "uac.h"

#define DT_DEVICE    1
#define DT_CONFIG    2
#define DT_INTERFACE 4
#define DT_ENDPOINT  5
#define DT_IAD       11
#define DT_CS_IFACE  0x24
#define DT_CS_EP     0x25

static uint16_t rd16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static uint32_t rd24(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16; }
static uint32_t rd32(const uint8_t *p) { return rd24(p) | (uint32_t) p[3] << 24; }

struct walk {
    uint8_t ifnum, alt, cls, sub, proto;
    int     cur;                /* fmt being built, -1 none */
};

static struct uac_unit *add_unit(struct uac_info *u, uint8_t id, uint8_t subtype)
{
    struct uac_unit *n;
    if (u->nunits >= UAC_MAX_UNITS)
        return NULL;
    n = &u->unit[u->nunits++];
    memset(n, 0, sizeof *n);
    n->id = id;
    n->subtype = subtype;
    return n;
}

static void ac_desc(struct uac_info *u, const struct walk *w, const uint8_t *d, unsigned l)
{
    struct uac_unit *n;
    uint8_t st = d[2];
    int v2 = w->proto == 0x20 || u->ac_ver == 2;
    if (st == 1 && l >= 5) {                    /* header */
        u->bcd_adc = rd16(d + 3);
        u->ac_ver = u->bcd_adc >= 0x0200 ? 2 : 1;
        if (u->ac_ver == 2)
            u->any_uac2 = 1;
    } else if (st == 2 && l >= 6) {             /* input terminal */
        if ((n = add_unit(u, d[3], st)))
            n->term_type = rd16(d + 4);
    } else if (st == 3 && l >= 8) {             /* output terminal */
        if ((n = add_unit(u, d[3], st))) {
            n->term_type = rd16(d + 4);
            n->source = d[7];
        }
    } else if ((st == 4 || st == 5) && l >= 6 && d[4] >= 1) {   /* mixer, selector */
        if ((n = add_unit(u, d[3], st)))
            n->source = d[5];
    } else if (st == 6 && l >= 6) {             /* feature unit */
        if (!(n = add_unit(u, d[3], st)))
            return;
        n->source = d[4];
        if (v2) {                               /* UAC2: 4-byte bmaControls, 2 bits each */
            unsigned c;
            for (c = 0; c < 3 && 5 + 4 * c + 4 <= l; c++) {
                uint32_t m = rd32(d + 5 + 4 * c);
                uint8_t b = (uint8_t) (((m & 3) ? 1 : 0) | ((m & 0xC) ? 2 : 0));
                if (c == 0) n->ctl_master = b;
                else if (c == 1) n->ctl_ch1 = b;
                else n->ctl_ch2 = b;
            }
        } else {
            unsigned cs = d[5];
            if (cs >= 1 && l >= 6 + cs) {
                n->ctl_master = d[6];
                if (l >= 6 + 2 * cs)
                    n->ctl_ch1 = d[6 + cs];
                if (l >= 6 + 3 * cs)
                    n->ctl_ch2 = d[6 + 2 * cs];
            }
        }
    } else if (st == 0x0A && v2 && l >= 4) {    /* UAC2 clock source */
        add_unit(u, d[3], st);
    }
}

static void as_desc(struct uac_info *u, const struct walk *w, const uint8_t *d, unsigned l)
{
    struct uac_fmt *f;
    unsigned i;
    if (w->cur < 0)
        return;
    f = &u->fmt[w->cur];
    if (d[2] == 1) {                            /* AS_GENERAL */
        if (f->uac_ver == 2 && l >= 11) {
            f->terminal_link = d[3];
            f->format_type = d[5];
            f->format_tag = (d[6] & 1) ? 1 : 0;
            f->channels = d[10];
        } else if (l >= 7) {
            f->terminal_link = d[3];
            f->format_tag = rd16(d + 5);
        }
    } else if (d[2] == 2 && l >= 4) {           /* FORMAT_TYPE */
        f->format_type = d[3];
        if (f->uac_ver == 2) {
            if (l >= 6) {
                f->subframe = d[4];
                f->bits = d[5];
            }
            return;
        }
        if (d[3] != 1 || l < 8)
            return;
        f->channels = d[4];
        f->subframe = d[5];
        f->bits = d[6];
        if (d[7] == 0) {
            if (l >= 14) {
                f->rate_continuous = 1;
                f->rate_min = rd24(d + 8);
                f->rate_max = rd24(d + 11);
            }
        } else {
            for (i = 0; i < d[7] && 8 + 3 * i + 3 <= l && f->nrates < UAC_MAX_RATES; i++)
                f->rates[f->nrates++] = rd24(d + 8 + 3 * i);
        }
    }
}

static void end_alt(struct uac_info *u, struct walk *w)
{
    if (w->cur >= 0) {
        struct uac_fmt *f = &u->fmt[w->cur];
        if (f->ep_addr) {
            if (f->dir_in) u->n_in++;
            else u->n_out++;
            u->nfmt++;
        }
    }
    w->cur = -1;
}

int uac_parse(const uint8_t *cfg, unsigned len, struct uac_info *u)
{
    struct walk w;
    unsigned off;

    memset(u, 0, sizeof *u);
    u->ac_ifnum = 0xFF;
    memset(&w, 0, sizeof w);
    w.cur = -1;
    if (!cfg || len < 9 || cfg[0] < 9 || cfg[1] != DT_CONFIG)
        return -1;
    if (rd16(cfg + 2) < len)
        len = rd16(cfg + 2);
    for (off = cfg[0]; off + 2 <= len; off += cfg[off]) {
        const uint8_t *d = cfg + off;
        unsigned l = d[0];
        if (l < 2 || off + l > len) {
            u->truncated = 1;
            break;
        }
        if (d[1] == DT_INTERFACE && l >= 9) {
            end_alt(u, &w);
            w.ifnum = d[2];
            w.alt = d[3];
            w.cls = d[5];
            w.sub = d[6];
            w.proto = d[7];
            if (w.cls != 1)
                continue;
            u->audio = 1;
            if (w.proto == 0x20)
                u->any_uac2 = 1;
            if (w.sub == 1) {
                u->ac_ifnum = w.ifnum;
            } else if (w.sub == 2 && u->nfmt < UAC_MAX_FMT) {
                struct uac_fmt *f = &u->fmt[u->nfmt];
                memset(f, 0, sizeof *f);
                f->ifnum = w.ifnum;
                f->alt = w.alt;
                f->uac_ver = w.proto == 0x20 ? 2 : 1;
                w.cur = u->nfmt;
            }
        } else if (d[1] == DT_CS_IFACE && l >= 3 && w.cls == 1) {
            if (w.sub == 1)
                ac_desc(u, &w, d, l);
            else if (w.sub == 2)
                as_desc(u, &w, d, l);
        } else if (d[1] == DT_ENDPOINT && l >= 7 && w.cur >= 0) {
            struct uac_fmt *f = &u->fmt[w.cur];
            uint8_t attr = d[3];
            if ((attr & 3) != 1)
                continue;                       /* not isochronous */
            if (((attr >> 4) & 3) == 1 || (f->ep_addr && f->ep_synch_addr && d[2] == f->ep_synch_addr)) {
                f->fb_addr = d[2];              /* explicit feedback (UAC1: named by bSynchAddress) */
                continue;
            }
            if (f->ep_addr)
                continue;                       /* first data endpoint wins */
            f->ep_addr = d[2];
            f->dir_in = (d[2] & 0x80) ? 1 : 0;
            f->ep_attr = attr;
            f->ep_sync = (attr >> 2) & 3;
            f->ep_maxpkt = rd16(d + 4) & 0x7FF;
            f->ep_interval = d[6];
            if (l >= 9) {
                f->ep_refresh = d[7];
                f->ep_synch_addr = d[8];
            }
        } else if (d[1] == DT_CS_EP && l >= 4 && w.cur >= 0 && d[2] == 1) {
            u->fmt[w.cur].cs_ep_attr = d[3];
            u->fmt[w.cur].have_cs_ep = 1;
        }
    }
    end_alt(u, &w);
    /* A slot taken by an alt with no data endpoint (alt 0) is reused, so only
     * the first nfmt entries are real: compact (end_alt counted them in order). */
    return 0;
}

unsigned uac_max_packet(const struct uac_fmt *f, uint32_t rate)
{
    return (unsigned) ((rate + 999) / 1000) * f->channels * f->subframe;
}

static int rate_ok(const struct uac_fmt *f, uint32_t r)
{
    unsigned i;
    unsigned bytes = uac_max_packet(f, r);
    if (bytes > f->ep_maxpkt || bytes > UAC_PACKET_MAX)
        return 0;
    if (f->rate_continuous)
        return r >= f->rate_min && r <= f->rate_max;
    for (i = 0; i < f->nrates; i++)
        if (f->rates[i] == r)
            return 1;
    return 0;
}

uint32_t uac_pick_rate(const struct uac_fmt *f)
{
    static const uint32_t pref[] = { 48000, 44100, 32000, 24000, 22050, 16000, 96000, 88200, 11025, 8000 };
    unsigned i;
    if (!f->channels || !f->subframe)
        return 0;
    /* First a rate whose packets need one start-split (see UAC_ONE_SPLIT). */
    for (i = 0; i < sizeof pref / sizeof pref[0]; i++)
        if (rate_ok(f, pref[i]) && uac_max_packet(f, pref[i]) <= UAC_ONE_SPLIT)
            return pref[i];
    for (i = 0; i < sizeof pref / sizeof pref[0]; i++)
        if (rate_ok(f, pref[i]))
            return pref[i];
    for (i = 0; i < f->nrates; i++)             /* an odd discrete rate */
        if (f->rates[i] >= 4000 && f->rates[i] <= 192000 && rate_ok(f, f->rates[i]))
            return f->rates[i];
    if (f->rate_continuous && f->rate_min >= 4000 && f->rate_min <= 192000 && rate_ok(f, f->rate_min))
        return f->rate_min;
    return 0;
}

int uac_choose(const struct uac_info *u, uint32_t *rate, const char **why)
{
    int best = -1, best_score = -1;
    unsigned i;
    const char *reason = "no audio streaming interface";
    if (rate)
        *rate = 0;
    if (u->n_out == 0 && u->audio)
        reason = u->n_in ? "only microphone (IN) streaming interfaces" : "no streaming endpoint";
    for (i = 0; i < u->nfmt; i++) {
        const struct uac_fmt *f = &u->fmt[i];
        uint32_t r;
        int score;
        if (f->dir_in)
            continue;
        if (f->uac_ver != 1) {
            reason = "USB Audio Class 2 only (not supported yet)";
            continue;
        }
        if (f->format_type != 1 || (f->format_tag != 1 && f->format_tag != 2)) {
            reason = "no PCM type I format";
            continue;
        }
        if (f->channels < 1 || f->channels > 8 || f->subframe < 1 || f->subframe > 4) {
            reason = "unsupported channel count or sample size";
            continue;
        }
        if (!f->ep_maxpkt || f->ep_maxpkt > UAC_PACKET_MAX) {
            reason = "unsupported endpoint packet size";
            continue;
        }
        if (!(r = uac_pick_rate(f))) {
            reason = "no usable sample rate";
            continue;
        }
        score = (uac_max_packet(f, r) <= UAC_ONE_SPLIT ? 16 : 0) +
                (f->subframe == 2 ? 8 : 0) + (f->channels == 2 ? 4 : f->channels == 1 ? 2 : 0) +
                (r == 48000 || r == 44100 ? 1 : 0);
        if (score > best_score) {
            best_score = score;
            best = (int) i;
            if (rate)
                *rate = r;
        }
    }
    if (why)
        *why = best >= 0 ? "ok" : reason;
    return best;
}

static const struct uac_unit *find_unit(const struct uac_info *u, uint8_t id)
{
    unsigned i;
    for (i = 0; i < u->nunits; i++)
        if (u->unit[i].id == id && u->unit[i].subtype != 0x0A)
            return &u->unit[i];
    return NULL;
}

const struct uac_unit *uac_feature_unit(const struct uac_info *u, const struct uac_fmt *f)
{
    unsigned i;
    for (i = 0; i < u->nunits; i++) {
        const struct uac_unit *n = &u->unit[i], *s = n;
        int depth;
        if (n->subtype != 6)
            continue;
        for (depth = 0; depth < 8 && s; depth++) {
            if (s->subtype == 2)
                break;
            s = find_unit(u, s->source);
        }
        if (s && s->subtype == 2 && s->id == f->terminal_link)
            return n;
    }
    return NULL;
}

const char *uac_sync_name(uint8_t sync)
{
    static const char *const n[4] = { "none", "asynchronous", "adaptive", "synchronous" };
    return n[sync & 3];
}

/* ---- text ----------------------------------------------------------------- */

static void hexline(uac_emit emit, void *ctx, const char *pfx, const uint8_t *d, unsigned l)
{
    char line[UAC_LINE];
    unsigned i, j, n;
    for (i = 0; i < l; i += 24) {
        n = (unsigned) snprintf(line, sizeof line, "%s", i ? "     " : pfx);
        for (j = i; j < l && j < i + 24; j++)
            n += (unsigned) snprintf(line + n, sizeof line - n, " %02x", d[j]);
        emit(ctx, line);
    }
}

#define OUT(...) do { snprintf(line, sizeof line, __VA_ARGS__); emit(ctx, line); } while (0)

static const char *term_name(uint16_t t)
{
    switch (t) {
    case 0x0101: return "USB streaming";
    case 0x0201: return "microphone";
    case 0x0204: return "headset microphone";
    case 0x0205: return "mic array";
    case 0x0301: return "speaker";
    case 0x0302: return "headphones";
    case 0x0304: return "desktop speaker";
    case 0x0402: return "headset";
    case 0x0603: return "line connector";
    case 0x0605: return "S/PDIF";
    default: return (t >> 8) == 3 ? "output terminal" : (t >> 8) == 2 ? "input terminal" : "terminal";
    }
}

void uac_dump_device(const uint8_t *d, unsigned len, uac_emit emit, void *ctx)
{
    char line[UAC_LINE];
    if (!d || len < 18 || d[0] < 18 || d[1] != DT_DEVICE) {
        OUT("device descriptor: not available or malformed");
        return;
    }
    hexline(emit, ctx, "dev  ", d, 18);
    OUT("  device: USB %x.%02x class %02x/%02x/%02x ep0 %u bytes, id %04x:%04x rel %x.%02x, %u configuration(s)",
        d[3], d[2], d[4], d[5], d[6], d[7], rd16(d + 8), rd16(d + 10), d[13], d[12], d[17]);
}

void uac_dump(const uint8_t *cfg, unsigned len, uac_emit emit, void *ctx)
{
    char line[UAC_LINE];
    unsigned off, i;
    uint8_t cls = 0, sub = 0, proto = 0;
    if (!cfg || len < 9 || cfg[1] != DT_CONFIG) {
        OUT("configuration descriptor: not available or malformed");
        return;
    }
    if (rd16(cfg + 2) < len)
        len = rd16(cfg + 2);
    for (off = 0; off + 2 <= len; off += cfg[off]) {
        const uint8_t *d = cfg + off;
        unsigned l = d[0];
        if (l < 2 || off + l > len) {
            OUT("  (descriptor at +%u has length %u: walk stops)", off, l);
            break;
        }
        hexline(emit, ctx, "desc ", d, l);
        switch (d[1]) {
        case DT_CONFIG:
            if (l >= 9)
                OUT("  configuration %u: total %u bytes, %u interface(s), attributes %02x (%s), max power %u mA",
                    d[5], rd16(d + 2), d[4], d[7], (d[7] & 0x40) ? "self powered" : "bus powered", d[8] * 2);
            break;
        case DT_IAD:
            if (l >= 8)
                OUT("  interface association: first %u, count %u, class %02x/%02x/%02x%s", d[2], d[3], d[4],
                    d[5], d[6], d[6] == 0x20 ? " (audio class 2)" : "");
            break;
        case DT_INTERFACE:
            if (l < 9)
                break;
            cls = d[5]; sub = d[6]; proto = d[7];
            OUT("  interface %u alt %u: %u endpoint(s), class %02x/%02x/%02x%s", d[2], d[3], d[4], cls, sub,
                proto, cls != 1 ? (cls == 3 ? " (HID)" : "") : sub == 1 ? (proto == 0x20 ? " (audio control, class 2)" : " (audio control)") :
                sub == 2 ? (proto == 0x20 ? " (audio streaming, class 2)" : " (audio streaming)") : " (audio)");
            break;
        case DT_ENDPOINT:
            if (l >= 7) {
                static const char *const tn[4] = { "control", "isochronous", "bulk", "interrupt" };
                static const char *const un[4] = { "data", "feedback", "implicit feedback data", "reserved" };
                OUT("  endpoint %02x %s %s, sync %s, usage %s, max packet %u, interval %u%s%s", d[2],
                    (d[2] & 0x80) ? "IN" : "OUT", tn[d[3] & 3], uac_sync_name((d[3] >> 2) & 3),
                    un[(d[3] >> 4) & 3], rd16(d + 4) & 0x7FF, d[6], l >= 9 ? ", refresh/synch " : "", "");
                if (l >= 9)
                    OUT("    bRefresh %u bSynchAddress %02x", d[7], d[8]);
            }
            break;
        case DT_CS_EP:
            if (l >= 4 && d[2] == 1)
                OUT("  audio endpoint: attributes %02x (%s%s%s)", d[3], (d[3] & 1) ? "sampling frequency control " : "",
                    (d[3] & 2) ? "pitch control " : "", (d[3] & 0x80) ? "max packets only" : "");
            break;
        case DT_CS_IFACE:
            if (cls != 1 || l < 3)
                break;
            if (sub == 1) {
                switch (d[2]) {
                case 1:
                    if (l >= 5) OUT("  AC header: ADC %x.%02x%s", d[4], d[3], rd16(d + 3) >= 0x200 ? " (class 2)" : "");
                    if (l >= 8 && rd16(d + 3) < 0x200)
                        for (i = 0; i < d[7] && 8 + i < l; i++)
                            OUT("    streaming interface %u", d[8 + i]);
                    break;
                case 2:
                    if (l >= 8) OUT("  input terminal %u: %04x (%s), %u channel(s)", d[3], rd16(d + 4),
                                   term_name(rd16(d + 4)), proto == 0x20 ? (l > 8 ? d[8] : 0) : d[7]);
                    break;
                case 3:
                    if (l >= 8) OUT("  output terminal %u: %04x (%s), source %u", d[3], rd16(d + 4),
                                   term_name(rd16(d + 4)), d[7]);
                    break;
                case 4: OUT("  mixer unit %u", d[3]); break;
                case 5: OUT("  selector unit %u", d[3]); break;
                case 6:
                    if (proto == 0x20) {
                        if (l >= 9) OUT("  feature unit %u (class 2): source %u, master controls %08lx", d[3], d[4],
                                       (unsigned long) rd32(d + 5));
                    } else if (l >= 7) {
                        unsigned cs = d[5], n = cs ? (l - 7) / cs : 0;
                        OUT("  feature unit %u: source %u, control size %u, %u control word(s)", d[3], d[4], cs, n);
                        for (i = 0; cs && i < n; i++)
                            OUT("    %s %u: %02x (%s%s)", i ? "channel" : "master", i, d[6 + i * cs],
                                (d[6 + i * cs] & 1) ? "mute " : "", (d[6 + i * cs] & 2) ? "volume" : "");
                    }
                    break;
                case 0x0A: OUT("  clock source %u (class 2)", d[3]); break;
                default: OUT("  audio control descriptor subtype %02x", d[2]); break;
                }
            } else if (sub == 2) {
                if (d[2] == 1 && proto == 0x20 && l >= 11)
                    OUT("  AS general (class 2): terminal %u, format type %u, formats %08lx, %u channel(s)", d[3],
                        d[5], (unsigned long) rd32(d + 6), d[10]);
                else if (d[2] == 1 && l >= 7)
                    OUT("  AS general: terminal %u, delay %u, format tag %04x (%s)", d[3], d[4], rd16(d + 5),
                        rd16(d + 5) == 1 ? "PCM" : rd16(d + 5) == 2 ? "PCM8" : rd16(d + 5) == 3 ? "IEEE float" : "other");
                else if (d[2] == 2 && proto == 0x20 && l >= 6)
                    OUT("  format type %u (class 2): subslot %u bytes, %u bits", d[3], d[4], d[5]);
                else if (d[2] == 2 && l >= 8 && d[3] == 1) {
                    OUT("  format type I: %u channel(s), %u-byte subframes, %u bits, %s", d[4], d[5], d[6],
                        d[7] ? "discrete rates" : "continuous rates");
                    if (d[7] == 0 && l >= 14)
                        OUT("    rates %lu..%lu Hz", (unsigned long) rd24(d + 8), (unsigned long) rd24(d + 11));
                    for (i = 0; d[7] && i < d[7] && 8 + 3 * i + 3 <= l; i++)
                        OUT("    rate %lu Hz", (unsigned long) rd24(d + 8 + 3 * i));
                } else
                    OUT("  streaming descriptor subtype %02x", d[2]);
            }
            break;
        case 0x21:
            OUT("  HID descriptor");
            break;
        default:
            OUT("  descriptor type %02x", d[1]);
            break;
        }
    }
}
