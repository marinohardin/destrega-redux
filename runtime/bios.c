/* High-level emulation of the PS1 kernel (BIOS A0/B0/C0 calls used by Destrega). */
#include "recomp.h"
#include "memcard.h"
#include <stdio.h>
#include <string.h>

void rt_return_from_exception(void);
void gpu_write_gp0(u32 v);
extern u32 g_istat;

volatile u16 g_pad_buttons[2] = {0xffff, 0xffff};

/* ---- events ---------------------------------------------------------------- */

#define MAX_EVENTS 32
typedef struct { u32 cls, spec, mode, func, status; } Event;
static Event s_ev[MAX_EVENTS];

enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000 };

static u32 open_event(u32 cls, u32 spec, u32 mode, u32 func) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (s_ev[i].status == EV_FREE) {
            s_ev[i] = (Event){cls, spec, mode, func, EV_DISABLED};
            rt_log("bios: OpenEvent(%08x, %04x, %04x, %08x) -> %d", cls, spec, mode, func, i);
            return 0xf1000000u | i;
        }
    }
    return 0xffffffffu;
}

static Event *ev(u32 h) {
    u32 i = h & 0xffff;
    return i < MAX_EVENTS ? &s_ev[i] : NULL;
}

static bool s_deliver_on_bios_stack;   /* delivering from interrupt context (card completions) */

void bios_deliver_event(u32 cls, u32 spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        Event *e = &s_ev[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x2000) {
            e->status = EV_READY;
        } else if (e->mode == 0x1000 && e->func) {
            Cpu saved = g_cpu;
            g_cpu.r[31] = 0;
            if (s_deliver_on_bios_stack) g_cpu.r[29] = 0x8000fe00;
            rt_call(e->func);
            u64 c = g_cpu.cycles, n = g_cpu.next_event;
            g_cpu = saved; g_cpu.cycles = c; g_cpu.next_event = n;
        }
    }
}

/* ---- pads (InitPAD/StartPAD style buffers filled at vblank) ----------------- */

static u32 s_pad_buf[2], s_pad_len[2];
static bool s_pad_started;
static u32 s_old_pad_dest;   /* B(15h) OutdatedPadInitAndStart */

static void pad_fill(void) {
    if (s_pad_started) {
        for (int p = 0; p < 2; p++) {
            if (!s_pad_buf[p] || s_pad_len[p] < 2) continue;
            u8 *b = ram_ptr(s_pad_buf[p]);
            if (p == 0) {
                b[0] = 0x00; b[1] = 0x41;
                b[2] = g_pad_buttons[0] & 0xff; b[3] = g_pad_buttons[0] >> 8;
            } else if (g_pad_buttons[1] != 0xffff || 1) {
                /* second controller present (for 2P); idle when untouched */
                b[0] = 0x00; b[1] = 0x41;
                b[2] = g_pad_buttons[1] & 0xff; b[3] = g_pad_buttons[1] >> 8;
            }
        }
    }
    if (s_old_pad_dest) {
        u32 v = (u32)g_pad_buttons[0] | ((u32)g_pad_buttons[1] << 16);
        mem_w32(s_old_pad_dest, ~v);
    }
}

/* ---- kernel tables and HLE trampolines ------------------------------------------ */

/* Kernel RAM holds the A0/B0/C0 function tables (games read/patch them via
 * GetB0Table/GetC0Table). Entries point at trampoline addresses which the
 * dispatcher maps back to HLE calls. */
#define A0_TABLE 0x200
#define B0_TABLE 0x874
#define C0_TABLE 0x674
#define DCB_TABLE 0x6000
#define DCB_NAMES 0x6100
#define TRAMP_BASE 0x3000  /* A0: 0x3000+fn*4, B0: 0x3400+, C0: 0x3800+ */

void bios_init(void) {
    for (u32 i = 0; i < 0xc0; i++) mem_w32(0x80000000 | (A0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + i * 4));
    for (u32 i = 0; i < 0x60; i++) mem_w32(0x80000000 | (B0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + 0x400 + i * 4));
    for (u32 i = 0; i < 0x20; i++) mem_w32(0x80000000 | (C0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + 0x800 + i * 4));
    /* Device control block table (pointer at 0x150, byte size at 0x154, 80 bytes per DCB, name
     * pointer first). The game's libapi firstfile() wrapper (0x800a60d8) searches it for the
     * device name (the letters before the unit digits: "bu") and only calls the BIOS firstfile if it finds one; it also
     * patches the DCB word at +52, which our HLE never calls through. */
    {
        for (u32 k = 0; k < 0x50; k += 4) mem_w32(0x80000000 | (DCB_TABLE + k), 0);
        mem_w8(0x80000000 | DCB_NAMES, 'b');
        mem_w8(0x80000000 | (DCB_NAMES + 1), 'u');
        mem_w8(0x80000000 | (DCB_NAMES + 2), 0);
        mem_w32(0x80000000 | DCB_TABLE, 0x80000000 | DCB_NAMES);
        mem_w32(0x80000000 | (DCB_TABLE + 52), 0x80000000 | TRAMP_BASE);
        mem_w32(0x80000150, 0x80000000 | DCB_TABLE);
        mem_w32(0x80000154, 0x50);
    }
    /* each trampoline is `jr ra; nop` in case something executes it directly */
    for (u32 a = TRAMP_BASE; a < TRAMP_BASE + 0xc00; a += 8) {
        mem_w32(0x80000000 | a, 0x03e00008);
        mem_w32(0x80000000 | (a + 4), 0);
    }
}

bool bios_tramp(u32 addr) {
    u32 p = addr & 0x1fffffff;
    return p >= TRAMP_BASE && p < TRAMP_BASE + 0xc00;
}

void bios_call(u32 vec);
void bios_call_tramp(u32 addr) {
    u32 p = (addr & 0x1fffffff) - TRAMP_BASE;
    g_cpu.r[9] = (p & 0x3ff) / 4;
    bios_call(0xa0 + (p >> 10) * 0x10);
}

/* ---- SysEnqIntRP chains --------------------------------------------------------- */

static u32 s_intrp[4][8];
static int s_intrp_n[4];

static u32 call_guest(u32 fn, u32 a0) {
    Cpu saved = g_cpu;
    g_cpu.r[4] = a0;
    g_cpu.r[29] = 0x8000fe00;   /* BIOS exception stack (kernel RAM) */
    g_cpu.r[31] = 0;
    rt_call(fn);
    u32 v = g_cpu.r[2];
    u64 c = g_cpu.cycles, n = g_cpu.next_event;
    g_cpu = saved; g_cpu.cycles = c; g_cpu.next_event = n;
    return v;
}

static void run_intrp_chains(void) {
    for (int pr = 0; pr < 4; pr++) {
        for (int i = 0; i < s_intrp_n[pr]; i++) {
            u32 st = s_intrp[pr][i];
            u32 f2 = mem_r32(st + 4), f1 = mem_r32(st + 8);
            u32 v = f1 ? call_guest(f1, 0) : 0;
            if (v && f2) call_guest(f2, v);
        }
    }
}

/* ---- interrupt hook ------------------------------------------------------------ */

static u32 s_hook_addr;

bool bios_hook_get(u32 buf[12]) {
    if (!s_hook_addr) return false;
    for (int i = 0; i < 12; i++) buf[i] = mem_r32(s_hook_addr + i * 4);
    return true;
}

static void card_deliver_due(void);

void bios_irq_prologue(void) {
    if (g_istat & 1) pad_fill();
    run_intrp_chains();
    card_deliver_due();
}

/* ---- memory card ---------------------------------------------------------------------
 * Destrega's libmcrd sits on the BIOS card API: _card_info/_card_load/_card_write for the
 * card state and the "bu00:" file functions (open/read/write/firstfile/erase/format) for
 * data. Transfers are asynchronous on hardware: the call returns at once and the BIOS later
 * posts an event pair, HwCARD (0xf0000011) then SwCARD (0xf4000001), with spec IOE (ok),
 * ERROR, TIMOUT (no card) or NEW (card changed). libmcrd polls the callbacks it registered
 * on those events, so each op queues exactly one pair and bios_irq_prologue delivers it once
 * the (modelled) transfer time has passed. The data itself moves at call time. */

enum { CEV_IOE = 0x0004, CEV_ERROR = 0x8000, CEV_TIMOUT = 0x0100, CEV_NEW = 0x2000 };
#define CARD_SECTOR_CYCLES 60000   /* ~1.8 ms per 128-byte sector */

typedef struct { u64 due; u32 hw, sw; } CardEv;
static CardEv s_cev[32];
static int s_cev_n;
static bool s_allow_new;           /* _new_card: let the next op through despite the changed flag */

static void card_post(u32 hw, u32 sw, u32 sectors) {
    if (s_cev_n == 32) return;
    u64 due = g_cpu.cycles + (u64)(sectors ? sectors : 1) * CARD_SECTOR_CYCLES;
    if (s_cev_n && s_cev[s_cev_n - 1].due > due) due = s_cev[s_cev_n - 1].due;
    s_cev[s_cev_n++] = (CardEv){due, hw, sw};
}

static void card_deliver_due(void) {
    while (s_cev_n && s_cev[0].due <= g_cpu.cycles) {
        CardEv e = s_cev[0];
        memmove(&s_cev[0], &s_cev[1], (size_t)(--s_cev_n) * sizeof s_cev[0]);
        bool saved = s_deliver_on_bios_stack;
        s_deliver_on_bios_stack = true;
        if (g_trace) rt_log("card: event hw=%04x sw=%04x", e.hw, e.sw);
        bios_deliver_event(0xf0000011, e.hw);
        bios_deliver_event(0xf4000001, e.sw);
        s_deliver_on_bios_stack = saved;
        memcard_flush();
    }
}

/* Result of the card-presence / changed-flag check that every transfer starts with:
 * 0 = go ahead, otherwise the event spec to report instead. */
static u32 card_gate(int port) {
    if (!memcard_present(port)) return CEV_TIMOUT;
    bool allow = s_allow_new;
    s_allow_new = false;
    if (memcard_new_flag(port) && !allow) return CEV_NEW;
    return 0;
}

static int chan_port(u32 chan) { return (chan >> 4) & 1; }

/* ---- "buNN:" files ---- */

static void guest_str(u32 a, char *dst, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) { char c = (char)mem_r8(a + i); if (!c) break; dst[i] = c; }
    dst[i] = 0;
}

/* "bu00:NAME" -> port 0 / "NAME"; returns false for other devices. */
static bool bu_path(u32 a, int *port, char *name, size_t n) {
    char p[96];
    guest_str(a, p, sizeof p);
    if (p[0] != 'b' || p[1] != 'u' || p[2] < '0' || p[2] > '1' || p[3] < '0' || p[3] > '9' || p[4] != ':') return false;
    *port = p[2] - '0';
    snprintf(name, n, "%s", p + 5);
    return true;
}

static bool s_dir_valid;
static char s_dir_pat[24];
static int s_dir_port, s_dir_cursor;

static u32 dir_fill(u32 de, McDirEntry *e) {
    for (int i = 0; i < 20; i++) mem_w8(de + i, (u8)e->name[i]);
    mem_w32(de + 20, 0);                 /* attr */
    mem_w32(de + 24, e->size);
    mem_w32(de + 28, 0);                 /* next */
    mem_w32(de + 32, (u32)e->head);
    mem_w32(de + 36, 0);
    return de;
}

/* One directory step. A hit returns the entry; running off the end (or a card that cannot be
 * listed) returns 0 and, like the BIOS, posts a completion event that libmcrd waits for. */
static void dir_step(u32 *r, u32 de) {
    McDirEntry e;
    u32 gate = card_gate(s_dir_port);
    if (gate) { card_post(gate, gate, 1); return; }
    if (!memcard_formatted(s_dir_port)) { card_post(CEV_IOE, CEV_ERROR, 1); return; }
    if (memcard_dir_next(s_dir_port, s_dir_pat, &s_dir_cursor, &e)) r[2] = dir_fill(de, &e);
    else card_post(CEV_IOE, CEV_IOE, 1);
}

/* FASYNC (0x8000) in the open mode makes read/write asynchronous: they return 0 once the
 * transfer is queued (libmcrd loops until it sees 0) and the BIOS posts the event pair later. */
static bool s_fd_async[MC_MAXFD];

static void bu_open(u32 *r) {
    int port;
    char name[64];
    r[2] = 0xffffffffu;
    if (!bu_path(r[4], &port, name, sizeof name)) return;
    u32 mode = r[5];
    bool create = mode & 0x200;
    /* libmcrd opens with 0x8001 and then writes, so the access bits are not enforced */
    int h = memcard_open(port, name, create, (int)(mode >> 16), true);
    if (g_trace) rt_log("card: open(bu%d0:%s, %08x) -> %d", port, name, mode, h);
    if (h >= 0) { s_fd_async[h] = mode & 0x8000; r[2] = (u32)h + 2; }   /* fds 0/1 are the tty */
}

static void bu_xfer(u32 *r, bool write) {
    int h = (int)r[4] - 2;
    u32 buf = r[5], n = r[6];
    r[2] = 0xffffffffu;
    if (h < 0 || h >= MC_MAXFD) return;
    static u8 tmp[MC_SIZE];
    if (n > sizeof tmp) return;
    int port = memcard_handle_port(h);
    u32 gate = port < 0 ? 0 : card_gate(port);
    if (gate) { r[2] = s_fd_async[h] ? 0 : 0xffffffffu; card_post(gate, gate, 1); return; }   /* nothing transferred */
    long k;
    if (write) {
        for (u32 i = 0; i < n; i++) tmp[i] = (u8)mem_r8(buf + i);
        k = memcard_write(h, tmp, n);
    } else {
        k = memcard_read(h, tmp, n);
        for (long i = 0; i < k; i++) mem_w8(buf + (u32)i, tmp[i]);
    }
    if (g_trace) rt_log("card: %s(fd %d, %08x, %u) -> %ld", write ? "write" : "read", h + 2, buf, n, k);
    if (k < 0) { if (s_fd_async[h]) card_post(CEV_ERROR, CEV_ERROR, 1); return; }
    if (s_fd_async[h]) {
        r[2] = 0;
        card_post(CEV_IOE, CEV_IOE, (u32)(k + MC_SECTOR - 1) / MC_SECTOR);
    } else {
        r[2] = (u32)k;
    }
}

static void card_file_call(u32 fn) {
    u32 *r = g_cpu.r;
    int port;
    char name[64];
    switch (fn) {
    case 0x32: bu_open(r); break;                                  /* open */
    case 0x33: r[2] = (u32)memcard_seek((int)r[4] - 2, (s32)r[5], (int)r[6]); break;   /* lseek */
    case 0x34: bu_xfer(r, false); break;                           /* read */
    case 0x35: bu_xfer(r, true); break;                            /* write */
    case 0x36: { int h = memcard_close((int)r[4] - 2); r[2] = h < 0 ? 0xffffffffu : r[4]; break; }   /* close */
    case 0x41:                                                     /* format("bu00:") */
        r[2] = 0;
        if (bu_path(r[4], &port, name, sizeof name) && memcard_present(port)) {
            memcard_format(port);
            memcard_flush();
            r[2] = 1;
        }
        if (g_trace) rt_log("card: format -> %d", r[2]);
        break;
    case 0x42: {                                                   /* firstfile(pattern, direntry) */
        r[2] = 0;
        if (!bu_path(r[4], &port, name, sizeof name)) break;
        snprintf(s_dir_pat, sizeof s_dir_pat, "%s", name);
        s_dir_port = port;
        s_dir_cursor = 0;
        s_dir_valid = true;
        dir_step(r, r[5]);
        if (g_trace) rt_log("card: firstfile(%s) -> %08x", name, r[2]);
        break;
    }
    case 0x43:                                                     /* nextfile(direntry) */
        r[2] = 0;
        if (s_dir_valid) dir_step(r, r[4]);
        if (g_trace) rt_log("card: nextfile -> %08x", r[2]);
        break;
    case 0x45:                                                     /* erase(name) */
        r[2] = 0;
        if (bu_path(r[4], &port, name, sizeof name) && memcard_erase(port, name)) { memcard_flush(); r[2] = 1; }
        if (g_trace) rt_log("card: erase(%s) -> %d", name, r[2]);
        break;
    }
}

/* ---- sector level ---- */

static void card_info_call(u32 fn) {
    u32 *r = g_cpu.r;
    int port = chan_port(r[4]);
    r[2] = 1;
    u32 gate = card_gate(port);
    switch (fn) {
    case 0xab:   /* A0:ab _card_info(chan) */
        if (g_trace) rt_log("card: info(port %d) -> %s", port, gate == CEV_TIMOUT ? "no card" : gate ? "new card" : "ok");
        gate ? card_post(gate, gate, 1) : card_post(CEV_IOE, CEV_IOE, 1);
        break;
    case 0xac:   /* A0:ac _card_load(chan): read the directory */
        if (g_trace) rt_log("card: load(port %d) -> %s", port, gate == CEV_TIMOUT ? "no card" : gate ? "new card" : memcard_formatted(port) ? "ok" : "unformatted");
        if (gate) card_post(gate, gate, 1);
        else if (!memcard_formatted(port)) card_post(CEV_IOE, CEV_ERROR, 16);
        else card_post(CEV_IOE, CEV_IOE, 16);
        break;
    case 0x4e: { /* B0:4e _card_write(chan, sector, buf) */
        u8 b[MC_SECTOR];
        if (g_trace) rt_log("card: write sector %u (port %d)%s", r[5], port, gate ? " gated" : "");
        if (gate) { card_post(gate, gate, 1); break; }
        for (int i = 0; i < MC_SECTOR; i++) b[i] = (u8)mem_r8(r[6] + (u32)i);
        if (memcard_write_sector(port, r[5], b)) card_post(CEV_IOE, CEV_IOE, 1);
        else card_post(CEV_ERROR, CEV_ERROR, 1);
        break;
    }
    case 0x4f: { /* B0:4f _card_read(chan, sector, buf) */
        u8 b[MC_SECTOR];
        if (g_trace) rt_log("card: read sector %u (port %d)%s", r[5], port, gate ? " gated" : "");
        if (gate) { card_post(gate, gate, 1); break; }
        if (memcard_read_sector(port, r[5], b)) {
            for (int i = 0; i < MC_SECTOR; i++) mem_w8(r[6] + (u32)i, b[i]);
            card_post(CEV_IOE, CEV_IOE, 1);
        } else card_post(CEV_ERROR, CEV_ERROR, 1);
        break;
    }
    }
}

/* ---- dispatch ---------------------------------------------------------------- */

static void stdout_write(u32 buf, u32 len) {
    char tmp[512];
    u32 n = len < sizeof tmp - 1 ? len : sizeof tmp - 1;
    for (u32 i = 0; i < n; i++) tmp[i] = (char)mem_r8(buf + i);
    tmp[n] = 0;
    fprintf(stderr, "%s", tmp);
}

static void bios_a0(u32 fn) {
    u32 *r = g_cpu.r;
    switch (fn) {
    case 0x39: break;                        /* InitHeap */
    case 0x44: break;                        /* FlushCache */
    case 0x49: gpu_write_gp0(r[4]); break;   /* GPU_cw */
    case 0x70: break;                        /* _bu_init */
    case 0x72: break;                        /* CdRemove */
    case 0xab: case 0xac: card_info_call(fn); break;   /* _card_info, _card_load */
    default: rt_log("bios: unhandled A0:%02x", fn); break;
    }
}

static void bios_b0(u32 fn) {
    u32 *r = g_cpu.r;
    Event *e;
    switch (fn) {
    case 0x07: bios_deliver_event(r[4], r[5]); break;
    case 0x08: r[2] = open_event(r[4], r[5], r[6], r[7]); break;
    case 0x09: if ((e = ev(r[4]))) e->status = EV_FREE; r[2] = 1; break;
    case 0x0a: /* WaitEvent */
        e = ev(r[4]);
        r[2] = 0;
        if (e) {
            for (int guard = 0; e->status != EV_READY && guard < 1000000; guard++) {
                g_cpu.cycles += 64;
                EVT_CHECK();
            }
            if (e->status == EV_READY) { e->status = EV_ENABLED; r[2] = 1; }
        }
        break;
    case 0x0b: /* TestEvent */
        e = ev(r[4]);
        r[2] = 0;
        if (e && e->status == EV_READY) { e->status = EV_ENABLED; r[2] = 1; }
        break;
    case 0x0c: if ((e = ev(r[4]))) e->status = EV_ENABLED; r[2] = 1; break;
    case 0x0d: if ((e = ev(r[4]))) e->status = EV_DISABLED; r[2] = 1; break;
    case 0x12: /* InitPAD(buf1, siz1, buf2, siz2) */
        s_pad_buf[0] = r[4]; s_pad_len[0] = r[5];
        s_pad_buf[1] = r[6]; s_pad_len[1] = r[7];
        r[2] = 2;
        break;
    case 0x13: s_pad_started = true; r[2] = 1; break;   /* StartPAD */
    case 0x14: s_pad_started = false; r[2] = 1; break;  /* StopPAD */
    case 0x15: s_old_pad_dest = r[5]; r[2] = 1; break;  /* OutdatedPadInitAndStart */
    case 0x17: rt_return_from_exception(); break;
    case 0x18: s_hook_addr = 0; break;                   /* ResetEntryInt */
    case 0x19: s_hook_addr = r[4]; rt_log("bios: HookEntryInt(%08x)", r[4]); break;
    case 0x32: case 0x33: case 0x34: case 0x36:          /* open, lseek, read, close (bu00: files) */
    case 0x41: case 0x42: case 0x43: case 0x45:          /* format, firstfile, nextfile, erase */
        card_file_call(fn);
        break;
    case 0x35: /* write */
        if (r[4] == 1) { stdout_write(r[5], r[6]); r[2] = r[6]; }
        else card_file_call(fn);
        break;
    case 0x4a: break;                                    /* InitCARD */
    case 0x4b: break;                                    /* StartCARD */
    case 0x4c: break;                                    /* StopCARD */
    case 0x4e: case 0x4f: card_info_call(fn); break;     /* _card_write, _card_read */
    case 0x50: s_allow_new = true; break;                /* _new_card */
    case 0x56: r[2] = 0x80000000 | C0_TABLE; break;      /* GetC0Table */
    case 0x57: r[2] = 0x80000000 | B0_TABLE; break;      /* GetB0Table */
    case 0x5b: break;                                    /* ChangeClearPad */
    default: rt_log("bios: unhandled B0:%02x", fn); break;
    }
}

static void bios_c0(u32 fn) {
    u32 *r = g_cpu.r;
    switch (fn) {
    case 0x02: /* SysEnqIntRP */
        rt_log("bios: SysEnqIntRP(%d, %08x) f1=%08x f2=%08x", r[4], r[5], mem_r32(r[5] + 8), mem_r32(r[5] + 4));
        if (r[4] < 4 && s_intrp_n[r[4]] < 8) {
            for (int i = 0; i < s_intrp_n[r[4]]; i++) if (s_intrp[r[4]][i] == r[5]) goto done;
            s_intrp[r[4]][s_intrp_n[r[4]]++] = r[5];
        }
    done:
        r[2] = 0;
        break;
    case 0x03: /* SysDeqIntRP */
        if (r[4] < 4)
            for (int i = 0; i < s_intrp_n[r[4]]; i++)
                if (s_intrp[r[4]][i] == r[5]) {
                    s_intrp[r[4]][i] = s_intrp[r[4]][--s_intrp_n[r[4]]];
                    break;
                }
        r[2] = 0;
        break;
    case 0x0a: r[2] = 0; break;   /* ChangeClearRCnt */
    default: rt_log("bios: unhandled C0:%02x", fn); break;
    }
}

void bios_call(u32 vec) {
    u32 fn = g_cpu.r[9] & 0xff;
    if (g_trace) rt_log("bios: %02X:%02x a0=%08x a1=%08x a2=%08x ra=%08x", vec, fn, g_cpu.r[4], g_cpu.r[5], g_cpu.r[6], g_cpu.r[31]);
    g_cpu.cycles += 200;
    if (vec == 0xa0) bios_a0(fn);
    else if (vec == 0xb0) bios_b0(fn);
    else bios_c0(fn);
}
