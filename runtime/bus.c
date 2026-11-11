/* Memory map, interrupt controller, DMA controller, root counters, scheduler. */
#include "psx.h"
#include "mdec.h"
#include "pgxp.h"
#include "savestate.h"
#include "hlr_hook.h"

u32 sio_read(u32 off, int size);
void sio_write(u32 off, u32 v, int size);
#include <string.h>
#include <stdlib.h>

u8 g_ram[RAM_SIZE];
u8 g_scratch[1024];

/* ---- scheduler ----------------------------------------------------------- */

static struct { u64 when; void (*cb)(void); bool active; } s_events[EVT_COUNT];

static void sched_recalc(void) {
    u64 n = ~0ull;
    for (int i = 0; i < EVT_COUNT; i++)
        if (s_events[i].active && s_events[i].when < n) n = s_events[i].when;
    g_cpu.next_event = n;
}

void sched_event(int id, u64 delay, void (*cb)(void)) {
    s_events[id].when = g_cpu.cycles + delay;
    s_events[id].cb = cb;
    s_events[id].active = true;
    sched_recalc();
}

void sched_cancel(int id) {
    s_events[id].active = false;
    sched_recalc();
}

/* Run all due events; returns after none are due. */
void sched_run(void) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < EVT_COUNT; i++)
            if (s_events[i].active && s_events[i].when <= g_cpu.cycles &&
                (best < 0 || s_events[i].when < s_events[best].when))
                best = i;
        if (best < 0) break;
        s_events[best].active = false;
        sched_recalc();
        s_events[best].cb();
    }
    sched_recalc();
}

/* ---- interrupt controller ------------------------------------------------ */

u32 g_istat, g_imask;
u32 g_flips;   /* GP1(05h) display-start writes: one per game frame */
u32 g_mdec_in_dmas, g_cd_dma_words;

void irq_raise(int bit) {
    g_istat |= 1u << bit;
    if (g_istat & g_imask) g_cpu.next_event = g_cpu.cycles; /* service soon */
}

/* ---- root counters ------------------------------------------------------- */

#define CYCLES_PER_LINE 2152   /* NTSC: 3413 video cycles * 11/7 */
#define LINES_PER_FRAME 263
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)

typedef struct {
    u32 mode, target;
    u64 base;      /* cycles timestamp where counter == base_val */
    u32 base_val;
} Timer;
static Timer s_tmr[3];
extern u64 g_frame_start;  /* cycle at start of current frame (vblank end) */

static u32 timer_div(int n) {
    u32 src = (s_tmr[n].mode >> 8) & 3;
    if (n == 1 && (src & 1)) return CYCLES_PER_LINE;   /* hblank */
    if (n == 2 && (src & 2)) return 8;                 /* sysclk/8 */
    if (n == 0 && (src & 1)) return 5;                 /* dotclock ~ 320px */
    return 1;
}

static u32 timer_value(int n) {
    Timer *t = &s_tmr[n];
    u64 ticks = (g_cpu.cycles - t->base) / timer_div(n) + t->base_val;
    u32 wrap = (t->mode & 8) ? (t->target ? t->target + 1 : 0x10000) : 0x10000;
    return (u32)(ticks % wrap) & 0xffff;
}

static void timer_irq_cb(void);
static u64 s_tmr_due[3][2];  /* absolute cycle of next target hit / overflow */

static void timer_schedule(void) {
    u64 best = ~0ull;
    for (int n = 0; n < 3; n++) {
        Timer *t = &s_tmr[n];
        s_tmr_due[n][0] = s_tmr_due[n][1] = ~0ull;
        if (!(t->mode & 0x30)) continue;
        u32 v = timer_value(n), div = timer_div(n);
        u32 period = ((t->mode & 8) && t->target) ? t->target + 1 : 0x10000;
        if (t->mode & 0x10) {
            u32 d = t->target > v ? t->target - v : period - v + t->target;
            if (d == 0) d = period;
            s_tmr_due[n][0] = g_cpu.cycles + (u64)d * div;
        }
        if ((t->mode & 0x20) && period == 0x10000)
            s_tmr_due[n][1] = g_cpu.cycles + (u64)(0x10000 - v) * div;
        for (int k = 0; k < 2; k++) if (s_tmr_due[n][k] < best) best = s_tmr_due[n][k];
    }
    if (best != ~0ull) sched_event(EVT_TIMER, best - g_cpu.cycles, timer_irq_cb);
    else sched_cancel(EVT_TIMER);
}

static void timer_irq_cb(void) {
    for (int n = 0; n < 3; n++) {
        bool hit = false;
        if (s_tmr_due[n][0] <= g_cpu.cycles) { s_tmr[n].mode |= 0x800; hit = true; }
        if (s_tmr_due[n][1] <= g_cpu.cycles) { s_tmr[n].mode |= 0x1000; hit = true; }
        if (hit) irq_raise(IRQ_TIMER0 + n);
    }
    timer_schedule();
}

static void timer_write(int n, int reg, u32 v) {
    Timer *t = &s_tmr[n];
    if (reg == 0) { t->base = g_cpu.cycles; t->base_val = v & 0xffff; }
    else if (reg == 1) { t->base = g_cpu.cycles; t->base_val = 0; t->mode = (v & 0x3ff) | 0x400; }
    else if (reg == 2) t->target = v & 0xffff;
    timer_schedule();
}

static u32 timer_read(int n, int reg) {
    Timer *t = &s_tmr[n];
    if (reg == 0) return timer_value(n);
    if (reg == 1) { u32 m = t->mode; t->mode &= ~0x1800u; return m; }
    return t->target;
}

/* ---- DMA controller ------------------------------------------------------ */

static u32 s_dma_madr[7], s_dma_bcr[7], s_dma_chcr[7];
static u32 s_dpcr = 0x07654321, s_dicr;
/* Per-channel completion: each channel finishes at its own time; a
 * completion that arrives while the same channel's flag is still unacked is
 * queued (not merged), so a callback chain (e.g. MDEC slice -> callback ->
 * next slice) can never lose an interrupt. */
static u64 s_dma_done_at[7] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
static u32 s_dma_queued[7];
static void dma_irq_cb(void);

static void dicr_update(void) {
    bool force = s_dicr & (1u << 15);
    bool master = s_dicr & (1u << 23);
    u32 flags = (s_dicr >> 24) & 0x7f, en = (s_dicr >> 16) & 0x7f;
    bool irq = force || (master && (flags & en));
    bool was = s_dicr & 0x80000000u;
    if (irq) s_dicr |= 0x80000000u; else s_dicr &= ~0x80000000u;
    if (irq && !was) irq_raise(IRQ_DMA);
}

static void dma_set_flag(int ch) {
    u32 bit = 1u << (24 + ch);
    if (!(s_dicr & (1u << (16 + ch)))) return;          /* IRQ disabled for channel */
    if (g_trace > 2) rt_log("@%llu dma%d flag (dicr=%08x, queued=%u)", (unsigned long long)g_cpu.cycles, ch, s_dicr, s_dma_queued[ch]);
    if (s_dicr & bit) { s_dma_queued[ch]++; return; }   /* previous one not acked yet */
    s_dicr |= bit;
    bool was = s_dicr & 0x80000000u;
    dicr_update();
    /* a new flag while bit31 was already set still needs servicing */
    if (was && (s_dicr & (1u << 23))) irq_raise(IRQ_DMA);
}

static void dma_schedule(void) {
    u64 best = ~0ull;
    for (int ch = 0; ch < 7; ch++) if (s_dma_done_at[ch] < best) best = s_dma_done_at[ch];
    if (best == ~0ull) sched_cancel(EVT_DMA);
    else sched_event(EVT_DMA, best > g_cpu.cycles ? best - g_cpu.cycles : 0, dma_irq_cb);
}

bool rt_in_exception(void);
static void dma_irq_cb(void) {
    /* Interrupt handlers (e.g. libcd's) read-modify-write DICR, which would
     * ack a flag that became set in between. On hardware that window is a
     * few instructions; here events land at recompiled safe points, so
     * completions are held back until the handler has returned. */
    if (rt_in_exception()) {
        for (int ch = 0; ch < 7; ch++)
            if (s_dma_done_at[ch] <= g_cpu.cycles) s_dma_done_at[ch] = g_cpu.cycles + 256;
        dma_schedule();
        return;
    }
    for (int ch = 0; ch < 7; ch++) {
        if (s_dma_done_at[ch] <= g_cpu.cycles) {
            s_dma_done_at[ch] = ~0ull;
            if (g_trace > 2) rt_log("@%llu dma%d done dicr=%08x", (unsigned long long)g_cpu.cycles, ch, s_dicr);
            dma_set_flag(ch);
        }
    }
    dma_schedule();
}

/* Approximate transfer/processing time in CPU cycles per 32-bit word. */
static const u32 s_dma_cycles_per_word[7] = {
    2,   /* MDEC in */
    10,  /* MDEC out: dominated by macroblock decode time */
    1,   /* GPU */
    6,   /* CD-ROM (sector buffer reads are slow) */
    4,   /* SPU */
    1,   /* PIO */
    1,   /* OTC */
};

/* Called for every node of a GPU linked-list DMA (RAM offset of its header) and once with
 * 0xffffffff at the end; used by the widescreen shadow pass to inject presentation-only
 * packets (runtime/overrides/widescreen.c). Must not change emulated state. */
void (*g_gpu_ll_hook)(u32 node);
void (*g_gpu_ll_hook2)(u32 node);   /* a second one (overrides/char_mesh.c), called after it */
HlrHooks g_hlr;                     /* hlr_hook.h: native high-level renderer, all NULL = off */

static void dma_run(int ch) {
    u32 chcr = s_dma_chcr[ch];
    u32 madr = s_dma_madr[ch] & 0x1ffffc;
    u32 sync = (chcr >> 9) & 3;
    bool to_ram = !(chcr & 1);
    int step = (chcr & 2) ? -4 : 4;
    u32 words;
    if (sync == 0) words = s_dma_bcr[ch] & 0xffff ? s_dma_bcr[ch] & 0xffff : 0x10000;
    else words = (s_dma_bcr[ch] & 0xffff) * (s_dma_bcr[ch] >> 16);
    u64 cost = words;

    if (ch == 0) g_mdec_in_dmas++;
    if (g_trace > 2 && ch != 2 && ch != 6)
        rt_log("@%llu dma%d start madr=%06x bcr=%08x chcr=%08x dicr=%08x words=%u", (unsigned long long)g_cpu.cycles, ch, madr, s_dma_bcr[ch], chcr, s_dicr, words);
    if (ch == 3) g_cd_dma_words += words;
    switch (ch) {
    case 0: /* MDEC in */
        for (u32 i = 0; i < words; i++, madr = (madr + step) & 0x1ffffc)
            mdec_write_cmd(*(u32 *)(g_ram + madr));
        break;
    case 1: /* MDEC out */
        for (u32 i = 0; i < words; i++, madr = (madr + step) & 0x1ffffc)
            *(u32 *)(g_ram + madr) = mdec_read_data();
        break;
    case 2: /* GPU */
        if (sync == 2) {
            pgxp_new_batch();   /* PGXP welding scope: one ordering-table DMA */
            u32 a = madr, guard = 0;
            cost = 0;
            for (;;) {
                if (g_gpu_ll_hook) g_gpu_ll_hook(a & 0x1ffffc);   /* presentation only */
                if (g_gpu_ll_hook2) g_gpu_ll_hook2(a & 0x1ffffc);
                /* the HLR drew its own version of this node into the hi-res target: the
                 * words still run on the 1x GPU, they are only not forwarded (presentation) */
                const bool no_hires = g_hlr.ll_filter && g_hlr.ll_filter(a & 0x1ffffc);
                if (no_hires) gpu_set_node_no_hires(true);
                u32 hdr = *(u32 *)(g_ram + (a & 0x1ffffc));
                u32 n = hdr >> 24;
                for (u32 i = 0; i < n; i++) {
                    u32 wa = (a + 4 + i * 4) & 0x1ffffc;
                    gpu_write_gp0_tag(*(u32 *)(g_ram + wa), g_pgxp_mem[wa >> 2]);
                }
                if (no_hires) gpu_set_node_no_hires(false);
                cost += n + 1;
                if ((hdr & 0x800000) || ++guard > 0x40000) break;
                a = hdr & 0x1ffffc;
            }
            if (g_gpu_ll_hook) g_gpu_ll_hook(0xffffffffu);
            if (g_gpu_ll_hook2) g_gpu_ll_hook2(0xffffffffu);
            if (g_hlr.ll_filter) g_hlr.ll_filter(0xffffffffu);
            madr = 0xffffff;
        } else if (to_ram) {
            for (u32 i = 0; i < words; i++, madr = (madr + step) & 0x1ffffc)
                *(u32 *)(g_ram + madr) = gpu_read_data();
        } else {
            for (u32 i = 0; i < words; i++, madr = (madr + step) & 0x1ffffc)
                gpu_write_gp0_tag(*(u32 *)(g_ram + madr), g_pgxp_mem[madr >> 2]);
        }
        break;
    case 3: /* CDROM */
        cdrom_dma_read((u32 *)(g_ram + madr), words);
        madr += words * 4;
        break;
    case 4: /* SPU */
        if (to_ram) spu_dma_read((u16 *)(g_ram + madr), words * 2);
        else spu_dma_write((const u16 *)(g_ram + madr), words * 2);
        madr += words * 4;
        break;
    case 6: /* OTC: reverse-clear ordering table */
        for (u32 i = 0; i < words; i++) {
            u32 v = (i == words - 1) ? 0xffffff : ((madr - 4) & 0x1fffff);
            *(u32 *)(g_ram + madr) = v;
            madr = (madr - 4) & 0x1ffffc;
        }
        break;
    default:
        break;
    }
    if (sync != 0) s_dma_madr[ch] = madr;
    s_dma_chcr[ch] &= ~0x11000000u;   /* clear busy + trigger */
    cost *= s_dma_cycles_per_word[ch];
    s_dma_done_at[ch] = g_cpu.cycles + (cost < 64 ? 64 : cost);
    dma_schedule();
}

static void dma_write(u32 off, u32 v) {
    if (off >= 0x70) {
        if (off == 0x70) s_dpcr = v;
        else if (off == 0x74) {
            if (g_trace > 2) rt_log("@%llu dicr write %08x (was %08x)", (unsigned long long)g_cpu.cycles, v, s_dicr);
            /* Bit 31 is read-only and set whenever an enabled flag is
             * pending, so a write with bit 31 set is a read-modify-write of
             * the whole register (libcd does this while toggling its channel
             * enable) - don't let it ack other channels' flags. libetc's
             * genuine acks write the flag bit with bit 31 clear. */
            u32 ack = (v & 0x80000000u) ? 0 : (v & 0x7f000000u);
            s_dicr = (s_dicr & 0x7f000000u & ~ack) | (v & 0x00ff803fu);
            dicr_update();
            for (int ch = 0; ch < 7; ch++)
                if ((ack >> (24 + ch)) & 1 && s_dma_queued[ch]) { s_dma_queued[ch]--; dma_set_flag(ch); }
        }
        return;
    }
    int ch = off >> 4;
    switch (off & 0xf) {
    case 0: s_dma_madr[ch] = v & 0xffffff; break;
    case 4: s_dma_bcr[ch] = v; break;
    case 8:
        s_dma_chcr[ch] = v;
        if (v & 0x01000000u) {
            u32 sync = (v >> 9) & 3;
            if (sync != 0 || (v & 0x10000000u) || ch == 6) dma_run(ch);
        }
        break;
    }
}

static u32 dma_read(u32 off) {
    if (off == 0x70) return s_dpcr;
    if (off == 0x74) return s_dicr;
    if (off >= 0x70) return 0;
    int ch = off >> 4;
    switch (off & 0xf) {
    case 0: return s_dma_madr[ch];
    case 4: return s_dma_bcr[ch];
    case 8: return s_dma_chcr[ch];
    }
    return 0;
}

/* ---- I/O dispatch ---------------------------------------------------------- */

static u32 s_io_misc[0x1000 / 4];
static u16 s_spu_last;

static u32 io_read(u32 p, int size) {
    u32 off = p - 0x1f801000;
    if (off >= 0x70 && off < 0x78) {
        u32 v = off < 0x74 ? g_istat : g_imask;
        return v;
    }
    if (off >= 0x80 && off < 0x100) return dma_read((off - 0x80) & ~3u) >> ((off & 3) * 8);
    if (off >= 0x100 && off < 0x130) return timer_read((off - 0x100) >> 4, ((off & 0xf) >> 2));
    if (off >= 0x800 && off < 0x804) {
        u32 v = cdrom_read8(off - 0x800);
        if (size >= 2) v |= (u32)cdrom_read8(off - 0x800) << 8;
        if (size == 4) { v |= (u32)cdrom_read8(off - 0x800) << 16; v |= (u32)cdrom_read8(off - 0x800) << 24; }
        return v;
    }
    if (off == 0x810) return gpu_read_data();
    if (off == 0x814) return gpu_read_stat();
    if (off == 0x820) return mdec_read_data();
    if (off == 0x824) return mdec_read_status();
    if (off >= 0xc00 && off < 0x1000) {
        if (size == 4) return spu_read16(off - 0xc00) | ((u32)spu_read16(off - 0xc00 + 2) << 16);
        u32 v = spu_read16((off - 0xc00) & ~1u);
        return (off & 1) ? v >> 8 : v;
    }
    if (off >= 0x40 && off < 0x50) return sio_read(off - 0x40, size);
    (void)s_spu_last;
    return s_io_misc[(off & 0xfff) >> 2] >> ((off & 3) * 8);
}

static void io_write(u32 p, u32 v, int size) {
    u32 off = p - 0x1f801000;
    if (off >= 0x70 && off < 0x78) {
        if (off < 0x74) g_istat &= v;
        else g_imask = v & 0x7ff;
        if (g_istat & g_imask) g_cpu.next_event = g_cpu.cycles;
        return;
    }
    if (off >= 0x80 && off < 0x100) {
        if (size < 4) {
            u32 sh = (off & 3) * 8, cur = dma_read((off - 0x80) & ~3u);
            u32 mask = (size == 1 ? 0xffu : 0xffffu) << sh;
            v = (cur & ~mask) | ((v << sh) & mask);
        }
        dma_write((off - 0x80) & ~3u, v);
        return;
    }
    if (off >= 0x40 && off < 0x50) { sio_write(off - 0x40, v, size); return; }
    if (off >= 0x100 && off < 0x130) { timer_write((off - 0x100) >> 4, (off & 0xf) >> 2, v); return; }
    if (off >= 0x800 && off < 0x804) {
        cdrom_write8(off - 0x800, (u8)v);
        if (size >= 2) cdrom_write8(off - 0x800 + 1, (u8)(v >> 8));
        return;
    }
    if (off == 0x810) { gpu_write_gp0_tag(v, size == 4 ? g_pgxp_mem[PGXP_IO_SLOT] : 0); return; }
    if (off == 0x814) {
        if ((v >> 24) == 0x05) g_flips++;
        gpu_write_gp1(v);
        return;
    }
    if (off == 0x820) { mdec_write_cmd(v); return; }
    if (off == 0x824) { mdec_write_ctrl(v); return; }
    if (off >= 0xc00 && off < 0x1000) {
        if (size == 4) { spu_write16(off - 0xc00, (u16)v); spu_write16(off - 0xc00 + 2, (u16)(v >> 16)); }
        else spu_write16((off - 0xc00) & ~1u, (u16)v);
        return;
    }
    if (size == 4) s_io_misc[(off & 0xfff) >> 2] = v;
}

/* RAM write journal (psx.h g_mem_wlimit; used by the widescreen shadow pass). While it is open,
 * RAM stores leave the inline fast path and come here; the first write to a block copies the
 * block's old contents into s_mj_save at the same offset and records the block, and
 * mem_journal_undo() copies the recorded blocks back. Only stores through mem_w8/16/32 are seen:
 * the caller must make sure nothing else writes RAM meanwhile (the bus sandbox blocks DMA and
 * the BIOS). */
u32 g_mem_wlimit = 0x800000;
static u8 s_mj_bits[MEM_JOURNAL_BLOCKS / 8];
static u8 *s_mj_save;
static u32 s_mj_list[MEM_JOURNAL_BLOCKS], s_mj_n;

static void mem_journal_note(u32 p) {
    const u32 b = (p & 0x1fffff) >> MEM_JOURNAL_SHIFT;
    if ((s_mj_bits[b >> 3] >> (b & 7)) & 1) return;
    s_mj_bits[b >> 3] |= (u8)(1u << (b & 7));
    s_mj_list[s_mj_n++] = b;
    const size_t off = (size_t)b << MEM_JOURNAL_SHIFT;
    memcpy(s_mj_save + off, g_ram + off, (size_t)1 << MEM_JOURNAL_SHIFT);
}

bool mem_journal_begin(void) {
    if (!s_mj_save && !(s_mj_save = malloc(RAM_SIZE))) return false;
    s_mj_n = 0;
    g_mem_wlimit = 0;
    return true;
}

u32 mem_journal_undo(void) {   /* returns the number of blocks restored */
    g_mem_wlimit = 0x800000;
    for (u32 i = 0; i < s_mj_n; i++) {
        const u32 b = s_mj_list[i];
        const size_t off = (size_t)b << MEM_JOURNAL_SHIFT;
        memcpy(g_ram + off, s_mj_save + off, (size_t)1 << MEM_JOURNAL_SHIFT);
        s_mj_bits[b >> 3] = 0;
    }
    u32 n = s_mj_n;
    s_mj_n = 0;
    return n;
}

/* a RAM store while the journal is open (never reached otherwise: the inline fast path takes it) */
#define JOURNALLED_RAM_STORE(p, stmt) \
    if (__builtin_expect((p) < 0x800000, 0) && g_mem_wlimit == 0) { mem_journal_note(p); stmt; return; }

/* Sandbox (widescreen shadow pass): guest code must not reach hardware; accesses are
 * dropped (reads return 0) and counted so the caller can reject the pass. */
int g_bus_sandbox;
u32 g_bus_sandbox_hits;

u32 bus_read32(u32 a) {
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return 0; }
    u32 p = a & 0x1fffffff;
    if (p >= 0x1f801000 && p < 0x1f802000) return io_read(p, 4);
    if (p >= 0x1fc00000 && p < 0x1fc80000) return 0;   /* BIOS ROM (HLE) */
    if (p >= 0x1f000000 && p < 0x1f800000) return 0xffffffff;  /* expansion 1 */
    if (a == 0xfffe0130) return 0;
    return 0;
}
u32 bus_read16(u32 a) {
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return 0; }
    u32 p = a & 0x1fffffff;
    if (p >= 0x1f801000 && p < 0x1f802000) return io_read(p, 2) & 0xffff;
    if (p >= 0x1f000000 && p < 0x1f800000) return 0xffff;
    return 0;
}
u32 bus_read8(u32 a) {
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return 0; }
    u32 p = a & 0x1fffffff;
    if (p >= 0x1f801000 && p < 0x1f802000) return io_read(p, 1) & 0xff;
    if (p >= 0x1f000000 && p < 0x1f800000) return 0xff;
    return 0;
}
void bus_write32(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    JOURNALLED_RAM_STORE(p, *(u32 *)(g_ram + (p & 0x1ffffc)) = v)
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return; }
    if (p >= 0x1f801000 && p < 0x1f802000) io_write(p, v, 4);
}
void bus_write16(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    JOURNALLED_RAM_STORE(p, *(u16 *)(g_ram + (p & 0x1ffffe)) = (u16)v)
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return; }
    if (p >= 0x1f801000 && p < 0x1f802000) io_write(p, v & 0xffff, 2);
}
void bus_write8(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    JOURNALLED_RAM_STORE(p, g_ram[p & 0x1fffff] = (u8)v)
    if (__builtin_expect(g_bus_sandbox, 0)) { g_bus_sandbox_hits++; return; }
    if (p >= 0x1f801000 && p < 0x1f802000) io_write(p, v & 0xff, 1);
}

/* ---- video timing ---------------------------------------------------------- */

u64 g_frame_start;
void frontend_vblank(void);   /* main.c: present + pace + input + audio */

static void vblank_cb(void) {
    g_frame_start = g_cpu.cycles;
    gpu_vblank();
    irq_raise(IRQ_VBLANK);
    ss_vblank_begin();    /* frame boundary: save states / rollback (savestate.c) */
    frontend_vblank();
    ss_vblank_end();      /* inputs for this frame are final: replay / netplay */
    sched_event(EVT_VBLANK, CYCLES_PER_FRAME, vblank_cb);
}

void bus_init(void) {
    memset(g_ram, 0, sizeof g_ram);
    memset(g_scratch, 0, sizeof g_scratch);
    memset(s_events, 0, sizeof s_events);
    for (int n = 0; n < 3; n++) s_tmr[n] = (Timer){0};
    g_cpu.next_event = ~0ull;
    sched_event(EVT_VBLANK, CYCLES_PER_FRAME, vblank_cb);
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
#include "hlr_hook.h"
void bus_state_io(StateIO *io) {
    ss_io(io, g_ram, sizeof g_ram);
    ss_io(io, g_scratch, sizeof g_scratch);
    for (int i = 0; i < EVT_COUNT; i++) {
        SS_VAR(io, s_events[i].when);
        SS_VAR(io, s_events[i].active);
        ss_io_ptr(io, &s_events[i].cb, sizeof s_events[i].cb);
    }
    SS_VAR(io, g_istat); SS_VAR(io, g_imask);
    for (int n = 0; n < 3; n++) {
        SS_VAR(io, s_tmr[n].mode); SS_VAR(io, s_tmr[n].target);
        SS_VAR(io, s_tmr[n].base); SS_VAR(io, s_tmr[n].base_val);
    }
    SS_VAR(io, s_tmr_due);
    SS_VAR(io, s_dma_madr); SS_VAR(io, s_dma_bcr); SS_VAR(io, s_dma_chcr);
    SS_VAR(io, s_dpcr); SS_VAR(io, s_dicr);
    SS_VAR(io, s_dma_done_at); SS_VAR(io, s_dma_queued);
    SS_VAR(io, s_io_misc); SS_VAR(io, s_spu_last);
    SS_VAR(io, g_frame_start);
}
