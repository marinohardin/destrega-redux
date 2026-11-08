/* High-level emulation of the PS1 kernel (BIOS A0/B0/C0 calls used by Destrega). */
#include "recomp.h"
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

void bios_deliver_event(u32 cls, u32 spec) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        Event *e = &s_ev[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == 0x2000) {
            e->status = EV_READY;
        } else if (e->mode == 0x1000 && e->func) {
            Cpu saved = g_cpu;
            g_cpu.r[31] = 0;
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
#define TRAMP_BASE 0x3000   /* A0: 0x3000+fn*4, B0: 0x3400+, C0: 0x3800+ */

void bios_init(void) {
    for (u32 i = 0; i < 0xc0; i++) mem_w32(0x80000000 | (A0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + i * 4));
    for (u32 i = 0; i < 0x60; i++) mem_w32(0x80000000 | (B0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + 0x400 + i * 4));
    for (u32 i = 0; i < 0x20; i++) mem_w32(0x80000000 | (C0_TABLE + i * 4), 0x80000000 | (TRAMP_BASE + 0x800 + i * 4));
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

void bios_irq_prologue(void) {
    if (g_istat & 1) pad_fill();
    run_intrp_chains();
}

/* ---- memory card (no card inserted for now) --------------------------------- */

static void card_no_card(void) {
    bios_deliver_event(0xf4000001, 0x0100);  /* SwCARD: timeout / no card */
    bios_deliver_event(0xf0000011, 0x0100);  /* HwCARD */
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
    case 0x49: gpu_write_gp0_tag(r[4], g_pgxp_reg[4]); break;   /* GPU_cw */
    case 0x70: break;                        /* _bu_init */
    case 0x72: break;                        /* CdRemove */
    case 0xab: r[2] = 1; card_no_card(); break; /* _card_info */
    case 0xac: r[2] = 1; card_no_card(); break; /* _card_load */
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
    case 0x32: r[2] = 0xffffffffu; break;                /* open: no card files */
    case 0x33: r[2] = 0xffffffffu; break;                /* lseek */
    case 0x34: r[2] = 0xffffffffu; break;                /* read */
    case 0x35: /* write */
        if (r[4] == 1) { stdout_write(r[5], r[6]); r[2] = r[6]; }
        else r[2] = 0xffffffffu;
        break;
    case 0x36: r[2] = 0; break;                          /* close */
    case 0x41: r[2] = 0; break;                          /* format */
    case 0x42: r[2] = 0; break;                          /* firstfile */
    case 0x43: r[2] = 0; break;                          /* nextfile */
    case 0x45: r[2] = 0; break;                          /* erase */
    case 0x4a: break;                                    /* InitCARD */
    case 0x4b: break;                                    /* StartCARD */
    case 0x4c: break;                                    /* StopCARD */
    case 0x4e: r[2] = 1; card_no_card(); break;          /* _card_write */
    case 0x50: break;                                    /* _new_card */
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
    if (g_trace) rt_log("bios: %02X:%02x a0=%08x a1=%08x", vec, fn, g_cpu.r[4], g_cpu.r[5]);
    g_cpu.cycles += 200;
    if (vec == 0xa0) bios_a0(fn);
    else if (vec == 0xb0) bios_b0(fn);
    else bios_c0(fn);
}
