/* CD-ROM controller (register level, psx-spx) backed by a MODE2/2352 .bin. */
#include "psx.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2352
#define READ_CYCLES_1X (PSX_CPU_HZ / 75)


static u8 s_index;
static u8 s_params[16]; static int s_nparams;
static u8 s_resp[16]; static int s_resp_len, s_resp_pos;
static u8 s_int_enable = 0x1f, s_int_flag;  /* BIOS leaves CD IRQs enabled */
static u8 s_stat = 0x02;     /* motor on */
static u8 s_mode;
static u8 s_filter_file, s_filter_chan;
static u8 s_request;
static bool s_muted, s_busy;

static u32 s_setloc, s_curloc;
static bool s_setloc_pending;
static bool s_reading;

static u8 s_sector[SECTOR];         /* last sector read */
static u8 s_data[SECTOR]; static u32 s_data_len, s_data_pos;
static u8 s_last_header[8];

static XaDecoder s_xa;
static u8 s_atv[4] = {0x80, 0, 0x80, 0}, s_atv_pending[4];

/* Pending interrupt queue: delivered one at a time once the game acks. */
/* Data sectors waiting to be announced by INT1. The drive buffers a few
 * sectors; each INT1 makes its own sector current for the data FIFO. */
#define SECBUF_SLOTS 8
static u8 s_secbuf[SECBUF_SLOTS][SECTOR];
static int s_secbuf_next;
static u32 s_int1_dropped;

typedef struct { u8 type; u8 resp[16]; int len; u64 delay; int slot; } Pending;
static Pending s_q[8]; static int s_qn;

static void cd_deliver(void);
static void push_int_slot(u8 type, const u8 *resp, int len, u64 delay, int slot);
static void push_int(u8 type, const u8 *resp, int len, u64 delay) { push_int_slot(type, resp, len, delay, -1); }

static void push_int_slot(u8 type, const u8 *resp, int len, u64 delay, int slot) {
    if (s_qn >= 8) {
        /* drop the oldest queued data-ready interrupt (buffer overrun) */
        int k = 0;
        while (k < s_qn && s_q[k].type != 1) k++;
        if (k == s_qn) k = 0;
        memmove(s_q + k, s_q + k + 1, sizeof(Pending) * (s_qn - k - 1));
        s_qn--;
        s_int1_dropped++;
    }
    Pending *p = &s_q[s_qn++];
    p->type = type; memcpy(p->resp, resp, len); p->len = len; p->delay = delay; p->slot = slot;
    if (s_qn == 1 && s_int_flag == 0) sched_event(EVT_CDROM, delay, cd_deliver);
}

u32 g_cd_int1_pushed, g_cd_int1_delivered;
u32 cdrom_int1_dropped(void) { return s_int1_dropped; }
u32 cdrom_stream_lba(void) { return s_reading ? s_curloc : 0; }
static void cd_deliver(void) {
    if (s_qn == 0) return;
    if (s_int_flag) return;   /* wait for ack */
    Pending p = s_q[0];
    memmove(s_q, s_q + 1, sizeof(Pending) * (--s_qn));
    s_int_flag = p.type;
    if (p.type == 1) g_cd_int1_delivered++;
    if (p.slot >= 0) {
        memcpy(s_sector, s_secbuf[p.slot], SECTOR);
        s_data_len = s_data_pos = 0;   /* FIFO now refers to the new sector */
    }
    memcpy(s_resp, p.resp, p.len);
    s_resp_len = p.len; s_resp_pos = 0;
    s_busy = false;
    if (s_int_flag & s_int_enable & 0x1f) irq_raise(IRQ_CDROM);
    if (g_trace) rt_log("cd: INT%d resp %02x len %d", p.type, p.resp[0], p.len);
}

static u8 bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

/* The disc itself lives in disc.c (raw image or data pack). */
#define cdrom_read_sector disc_read_sector

void cdrom_reset_audio(void) { xa_reset(&s_xa); }

/* ---- sector reading ---------------------------------------------------------- */

static void read_tick(void);

/* DESTREGA_XA_TRACE=1: log every sector the drive reads (LBA, mode, subheader, filter) and every
 * GetlocL answer. Diagnostics only (host side, never touches emulated state). */
static bool xa_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("DESTREGA_XA_TRACE") != NULL;
    return on;
}

static u64 read_period(void) { return (s_mode & 0x80) ? READ_CYCLES_1X / 2 : READ_CYCLES_1X; }

static void start_reading(void) {
    if (s_setloc_pending) { s_curloc = s_setloc; s_setloc_pending = false; }
    s_reading = true;
    s_stat = (s_stat & ~0xc0) | 0x20 | 0x02;
    xa_reset(&s_xa);
    /* first sector arrives after a seek */
    sched_event(EVT_CDROM_READ, read_period() + 20000, read_tick);
}

static void stop_reading(void) {
    s_reading = false;
    s_stat &= ~0xe0;
    sched_cancel(EVT_CDROM_READ);
}

static void read_tick(void) {
    if (!s_reading) return;
    u32 lba = s_curloc++;
    int slot = s_secbuf_next;
    s_secbuf_next = (s_secbuf_next + 1) % SECBUF_SLOTS;
    u8 *sec = s_secbuf[slot];
    cdrom_read_sector(lba, sec);
    /* GetlocL reports the header + subheader of the sector the drive read last, whatever its
     * type: XA-ADPCM sectors that go to the SPU count too. The game polls GetlocL every 2
     * vblanks while its BGM streams and loops the track once the position passes the track's
     * end. Updating this only for data sectors (at INT1) left it stale for the whole song, then
     * at the padding data sectors after the end; after the game's re-seek it still said "past
     * the end", so the game re-seeked every 9 vblanks forever (the stuck BGM loop, NOTES.md). */
    memcpy(s_last_header, sec + 12, 8);
    u8 submode = sec[18];
    u8 file = sec[16], chan = sec[17];
    bool audio = (sec[15] == 2) && (submode & 0x04);
    sched_event(EVT_CDROM_READ, read_period(), read_tick);
    if (xa_trace()) rt_log("xa: lba %u mode %02x sm %02x f %u c %u ci %02x filt %u/%u", lba, s_mode,
                           submode, file, chan, sec[19], s_filter_file, s_filter_chan);

    if ((s_mode & 0x40) && audio) {
        /* XA-ADPCM: goes to the SPU, never to the data path */
        if (!(s_mode & 0x08) || (file == s_filter_file && chan == s_filter_chan)) {
            static s16 pcm[9408 * 2];
            int n = xa_decode_sector(&s_xa, sec, pcm, 9408);
            if (!s_muted && n > 0) {
                /* apply ATV volume matrix */
                for (int i = 0; i < n; i++) {
                    int l = pcm[i * 2], r = pcm[i * 2 + 1];
                    int ol = (l * s_atv[0] + r * s_atv[3]) >> 7;
                    int orr = (r * s_atv[2] + l * s_atv[1]) >> 7;
                    pcm[i * 2] = (s16)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
                    pcm[i * 2 + 1] = (s16)(orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr);
                }
                spu_push_cd_audio(pcm, n);
            }
            if (submode & 0x80) { /* EOF */ }
        }
        return;
    }
    if (audio) return;    /* audio sectors without XA mode are skipped by ReadS? keep simple */
    /* data sector -> INT1 (Setfilter also applies to data in XA mode 0x08) */
    u8 st = s_stat;
    g_cd_int1_pushed++;
    push_int_slot(1, &st, 1, 0, slot);
}

/* ---- commands ------------------------------------------------------------- */

#define ACK_DELAY 25000
#define LONG_DELAY 80000

static void cmd_stat_int2(u64 delay) { u8 st = s_stat; push_int(2, &st, 1, delay); }

static void execute(u8 cmd) {
    u8 st;
    u8 r[16];
    if (g_trace) rt_log("cd: cmd %02x params %d [%02x %02x %02x]", cmd, s_nparams, s_params[0], s_params[1], s_params[2]);
    s_busy = true;
    switch (cmd) {
    case 0x01: /* Getstat */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        s_stat &= ~0x10;
        break;
    case 0x02: /* Setloc */
        s_setloc = (unbcd(s_params[0]) * 60 + unbcd(s_params[1])) * 75 + unbcd(s_params[2]);
        s_setloc = s_setloc >= 150 ? s_setloc - 150 : 0;
        s_setloc_pending = true;
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        break;
    case 0x03: /* Play (CD-DA): no audio tracks on this disc */
        st = s_stat | 0x80; push_int(3, &st, 1, ACK_DELAY);
        break;
    case 0x06: case 0x1b: /* ReadN / ReadS */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        start_reading();
        break;
    case 0x07: /* MotorOn */
        s_stat |= 0x02; st = s_stat; push_int(3, &st, 1, ACK_DELAY); cmd_stat_int2(LONG_DELAY);
        break;
    case 0x08: /* Stop */
        stop_reading(); st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        s_stat &= ~0x02; cmd_stat_int2(LONG_DELAY);
        break;
    case 0x09: /* Pause */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        stop_reading(); cmd_stat_int2(s_reading ? LONG_DELAY * 4 : LONG_DELAY);
        break;
    case 0x0a: /* Init */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        stop_reading(); s_mode = 0; s_stat = 0x02; cmd_stat_int2(LONG_DELAY);
        break;
    case 0x0b: s_muted = true; st = s_stat; push_int(3, &st, 1, ACK_DELAY); break;
    case 0x0c: s_muted = false; st = s_stat; push_int(3, &st, 1, ACK_DELAY); break;
    case 0x0d: /* Setfilter */
        s_filter_file = s_params[0]; s_filter_chan = s_params[1];
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        break;
    case 0x0e: /* Setmode */
        s_mode = s_params[0]; st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        break;
    case 0x0f: /* Getparam */
        r[0] = s_stat; r[1] = s_mode; r[2] = 0; r[3] = s_filter_file; r[4] = s_filter_chan;
        push_int(3, r, 5, ACK_DELAY);
        break;
    case 0x10: /* GetlocL */
        memcpy(r, s_last_header, 8); push_int(3, r, 8, ACK_DELAY);
        if (xa_trace()) rt_log("xa: getlocl %02x:%02x:%02x m%u f %u c %u sm %02x ci %02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        break;
    case 0x11: { /* GetlocP */
        u32 l = s_curloc + 150, rel = s_curloc;
        r[0] = 1; r[1] = 1;
        r[2] = bcd(rel / 75 / 60); r[3] = bcd(rel / 75 % 60); r[4] = bcd(rel % 75);
        r[5] = bcd(l / 75 / 60); r[6] = bcd(l / 75 % 60); r[7] = bcd(l % 75);
        push_int(3, r, 8, ACK_DELAY);
        break;
    }
    case 0x13: /* GetTN */
        r[0] = s_stat; r[1] = 1; r[2] = 1; push_int(3, r, 3, ACK_DELAY);
        break;
    case 0x14: { /* GetTD */
        u32 l = (s_params[0] == 0) ? disc_sector_count() + 150 : 150;
        r[0] = s_stat; r[1] = bcd(l / 75 / 60); r[2] = bcd(l / 75 % 60);
        push_int(3, r, 3, ACK_DELAY);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        stop_reading();
        if (s_setloc_pending) { s_curloc = s_setloc; s_setloc_pending = false; }
        st = s_stat | 0x40; push_int(3, &st, 1, ACK_DELAY);
        cmd_stat_int2(LONG_DELAY);
        break;
    case 0x19: /* Test */
        if (s_params[0] == 0x20) { r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xc0; push_int(3, r, 4, ACK_DELAY); }
        else { st = s_stat; push_int(3, &st, 1, ACK_DELAY); }
        break;
    case 0x1a: /* GetID */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY);
        r[0] = 0x02; r[1] = 0x00; r[2] = 0x20; r[3] = 0x00; memcpy(r + 4, "SCEA", 4);
        push_int(2, r, 8, LONG_DELAY);
        break;
    case 0x1e: /* ReadTOC */
        st = s_stat; push_int(3, &st, 1, ACK_DELAY); cmd_stat_int2(LONG_DELAY * 4);
        break;
    default:
        rt_log("cd: unhandled command %02x", cmd);
        r[0] = s_stat | 1; r[1] = 0x40; push_int(5, r, 2, ACK_DELAY);
        break;
    }
    s_nparams = 0;
}

static u8 cdrom_read8_(u32 off);
u8 cdrom_read8(u32 off) {
    u8 v = cdrom_read8_(off);
    if (g_trace > 1) rt_log("cd: read %u.%u -> %02x", off, s_index, v);
    return v;
}
static u8 cdrom_read8_(u32 off) {
    switch (off) {
    case 0: {
        u8 v = s_index;
        if (s_nparams == 0) v |= 0x08;
        if (s_nparams < 16) v |= 0x10;
        if (s_resp_pos < s_resp_len) v |= 0x20;
        if (s_data_pos < s_data_len) v |= 0x40;
        if (s_busy) v |= 0x80;
        return v;
    }
    case 1:
        if (s_resp_pos < s_resp_len) return s_resp[s_resp_pos++];
        return 0;
    case 2:
        if (s_data_pos < s_data_len) return s_data[s_data_pos++];
        return 0;
    case 3:
        if (s_index & 1) return s_int_flag | 0xe0;
        return s_int_enable | 0xe0;
    }
    return 0;
}

void cdrom_write8(u32 off, u8 v) {
    if (g_trace > 1) rt_log("cd: write %u.%u = %02x", off, s_index, v);
    if (off == 0) { s_index = v & 3; return; }
    switch (off * 4 + s_index) {
    case 1 * 4 + 0: execute(v); break;
    case 1 * 4 + 3: s_atv_pending[2] = v; break;   /* right->right */
    case 2 * 4 + 0: if (s_nparams < 16) s_params[s_nparams++] = v; break;
    case 2 * 4 + 1: s_int_enable = v & 0x1f; break;
    case 2 * 4 + 2: s_atv_pending[0] = v; break;   /* left->left */
    case 2 * 4 + 3: s_atv_pending[3] = v; break;   /* right->left */
    case 3 * 4 + 0:
        s_request = v;
        if (v & 0x80) {
            if (s_data_pos >= s_data_len) {
                if (s_mode & 0x20) { memcpy(s_data, s_sector + 12, 2340); s_data_len = 2340; }
                else { memcpy(s_data, s_sector + 24, 2048); s_data_len = 2048; }
                s_data_pos = 0;
            }
        } else {
            s_data_len = s_data_pos = 0;
        }
        break;
    case 3 * 4 + 1:
        s_int_flag &= ~(v & 0x1f);
        if (v & 0x40) s_nparams = 0;
        if (s_int_flag == 0) {
            s_resp_len = s_resp_pos = 0;
            if (s_qn) sched_event(EVT_CDROM, s_q[0].delay < 2000 ? 2000 : 2000, cd_deliver);
        }
        break;
    case 3 * 4 + 2: s_atv_pending[1] = v; break;   /* left->right */
    case 3 * 4 + 3:
        if (v & 0x20) memcpy(s_atv, s_atv_pending, 4);
        break;
    }
}

u32 cdrom_dma_read(u32 *dst, u32 words) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < words * 4; i++)
        d[i] = s_data_pos < s_data_len ? s_data[s_data_pos++] : 0;
    return words;
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
/* In-flight reads are the EVT_CDROM / EVT_CDROM_READ scheduler slots (bus.c) plus the
 * position, sector buffers and interrupt queue here. */
void cdrom_state_io(StateIO *io) {
    SS_VAR(io, s_index); SS_VAR(io, s_params); SS_VAR(io, s_nparams);
    SS_VAR(io, s_resp); SS_VAR(io, s_resp_len); SS_VAR(io, s_resp_pos);
    SS_VAR(io, s_int_enable); SS_VAR(io, s_int_flag); SS_VAR(io, s_stat); SS_VAR(io, s_mode);
    SS_VAR(io, s_filter_file); SS_VAR(io, s_filter_chan); SS_VAR(io, s_request);
    SS_VAR(io, s_muted); SS_VAR(io, s_busy);
    SS_VAR(io, s_setloc); SS_VAR(io, s_curloc); SS_VAR(io, s_setloc_pending); SS_VAR(io, s_reading);
    SS_VAR(io, s_sector); SS_VAR(io, s_data); SS_VAR(io, s_data_len); SS_VAR(io, s_data_pos);
    SS_VAR(io, s_last_header);
    SS_VAR(io, s_xa.prev); SS_VAR(io, s_xa.resample_hist); SS_VAR(io, s_xa.resample_pos);
    SS_VAR(io, s_xa.resample_frac);
    SS_VAR(io, s_atv); SS_VAR(io, s_atv_pending);
    SS_VAR(io, s_secbuf); SS_VAR(io, s_secbuf_next); SS_VAR(io, s_int1_dropped);
    for (int i = 0; i < 8; i++) {
        SS_VAR(io, s_q[i].type); SS_VAR(io, s_q[i].resp); SS_VAR(io, s_q[i].len);
        SS_VAR(io, s_q[i].delay); SS_VAR(io, s_q[i].slot);
    }
    SS_VAR(io, s_qn);
}
