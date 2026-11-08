/* Recompiled-code runtime: dispatch, COP0, syscalls, interrupt delivery. */
#include "recomp.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Cpu g_cpu;
int g_trace;
u32 g_callstack[1024];
u32 g_depth;

int g_widescreen_cull;   /* --widescreen: widened stage culling bounds (WS_IMM, recomp.h) */
u32 g_ws_cull_hits;

void rt_dump_stack(void) {
    fprintf(stderr, "guest call stack (depth %u):\n", g_depth);
    for (u32 i = g_depth > 40 ? g_depth - 40 : 0; i < g_depth; i++)
        fprintf(stderr, "  #%u %08x\n", i, g_callstack[i & 1023]);
}

void sched_run(void);              /* bus.c */
extern u32 g_istat, g_imask;
void bios_call(u32 vec);           /* bios.c */
void bios_irq_prologue(void);      /* bios.c: BIOS-level work at interrupt entry */
bool bios_hook_get(u32 buf[12]);   /* bios.c: HookEntryInt buffer, if set */

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%10llu] ", (unsigned long long)(g_cpu.cycles / 564480));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fprintf(stderr, "  ra=%08x sp=%08x a0=%08x v0=%08x\n", g_cpu.r[31], g_cpu.r[29], g_cpu.r[4], g_cpu.r[2]);
    rt_dump_stack();
    exit(1);
}

/* ---- dispatch -------------------------------------------------------------- */

static u32 fnv1a(const u8 *p, u32 n) {
    u32 h = 0x811c9dc5u;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* Small cache of verified overlay entries keyed by address. Overlay code can
 * be replaced at any time (file copied into the slot), so a cached hit is only
 * trusted while the slot's first words are unchanged. */
typedef struct { u32 addr; const RecompEntry *e; u32 sig; } CacheEnt;
static CacheEnt s_cache[4096];

static u32 slot_sig(u32 addr) {
    /* Overlay slots: header word + first code words identify the file. */
    u32 base = addr >= 0x8001a268 ? 0x8001a268 : 0x80010000;
    const u32 *p = (const u32 *)ram_ptr(base);
    return p[0] ^ (p[1] * 31) ^ (p[2] * 131) ^ (p[7] * 1031) ^ (p[64] * 7);
}

static const RecompEntry *lookup_entry(u32 addr) {
    addr &= 0x1fffffff;
    addr |= 0x80000000;
    u32 lo = 0, hi = g_recomp_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_recomp_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    if (lo >= g_recomp_count || g_recomp_table[lo].addr != addr) return NULL;
    if (g_recomp_table[lo].overlay == 0) return &g_recomp_table[lo];
    CacheEnt *c = &s_cache[(addr >> 2) & 4095];
    u32 sig = slot_sig(addr);
    if (c->addr == addr && c->sig == sig) return c->e;
    for (u32 i = lo; i < g_recomp_count && g_recomp_table[i].addr == addr; i++) {
        const RecompEntry *e = &g_recomp_table[i];
        if (fnv1a(ram_ptr(e->lo), e->hi - e->lo) == e->hash) {
            c->addr = addr; c->e = e; c->sig = sig;
            return e;
        }
    }
    return NULL;
}

RecompFn rt_lookup(u32 addr) {
    const RecompEntry *e = lookup_entry(addr);
    return e ? e->fn : NULL;
}

bool bios_tramp(u32 addr);
void bios_call_tramp(u32 addr);

bool rt_is_hle(u32 addr) {
    u32 p = addr & 0x1fffffff;
    return p == 0xa0 || p == 0xb0 || p == 0xc0 || bios_tramp(addr);
}

static u32 s_missing_logged[256];
void rt_call(u32 addr) {
    u32 p = addr & 0x1fffffff;
    if (p == 0xa0 || p == 0xb0 || p == 0xc0) { bios_call(p); return; }
    if (bios_tramp(addr)) { bios_call_tramp(addr); return; }
    const RecompEntry *e = lookup_entry(addr);
    if (e) { e->fn(); return; }
    u32 h = (addr >> 2) & 255;
    if (s_missing_logged[h] != addr) {
        s_missing_logged[h] = addr;
        rt_log("dispatch: no recompiled function at %08x (ra=%08x), interpreting", addr, g_cpu.r[31]);
    }
    interp_run(addr, g_cpu.r[31]);
}

void rt_jump(u32 addr) { rt_call(addr); }

/* ---- COP0 / exceptions ------------------------------------------------------- */

u32 rt_mfc0(int reg) { return g_cpu.cop0[reg]; }

void rt_mtc0(int reg, u32 v) {
    g_cpu.cop0[reg] = v;
    if (reg == COP0_SR) g_cpu.next_event = g_cpu.cycles;  /* may unmask IRQs */
}

void rt_rfe(void) {
    u32 sr = g_cpu.cop0[COP0_SR];
    g_cpu.cop0[COP0_SR] = (sr & ~0xfu) | ((sr >> 2) & 0xf);
}

void rt_syscall(u32 pc) {
    (void)pc;
    u32 *sr = &g_cpu.cop0[COP0_SR];
    switch (g_cpu.r[4]) {
    case 1: /* EnterCriticalSection */
        g_cpu.r[2] = (*sr & 0x401) == 0x401;
        *sr &= ~0x401u;
        break;
    case 2: /* ExitCriticalSection */
        *sr |= 0x401;
        g_cpu.next_event = g_cpu.cycles;
        break;
    default:
        break;
    }
}

void rt_break(u32 pc, u32 code) {
    if (code == 7) return;   /* divide by zero check: result already defined */
    rt_log("break %x at %08x", code, pc);
}

void rt_invalid(u32 pc, u32 word) { rt_fatal("invalid instruction %08x at %08x", word, pc); }

void rt_unreached(u32 pc) { rt_fatal("fell through to unanalysed code at %08x", pc); }

/* ---- interrupt delivery ------------------------------------------------------- */

static jmp_buf *s_exc_env;
static int s_in_exception;
bool rt_in_exception(void) { return s_in_exception != 0; }

void rt_return_from_exception(void) {
    if (s_exc_env) longjmp(*s_exc_env, 1);
    rt_log("ReturnFromException outside of exception");
}

u64 g_irq_cycles;   /* emulated cycles spent inside interrupt handlers */
static void take_interrupt(void) {
    u64 irq_t0 = g_cpu.cycles;
    if (g_trace > 2) rt_log("@%llu irq: istat=%03x imask=%03x", (unsigned long long)g_cpu.cycles, g_istat, g_imask);
    Cpu saved = g_cpu;
    u32 depth = g_depth;
    s_in_exception++;
    /* push interrupt-enable stack like the hardware */
    u32 sr = g_cpu.cop0[COP0_SR];
    g_cpu.cop0[COP0_SR] = (sr & ~0x3fu) | ((sr << 2) & 0x3c);
    g_cpu.cop0[COP0_CAUSE] = (g_istat & g_imask) ? 0x400 : 0;
    bios_irq_prologue();
    u32 hook[12];
    if (bios_hook_get(hook)) {
        jmp_buf env, *prev = s_exc_env;
        s_exc_env = &env;
        if (!setjmp(env)) {
            g_cpu.r[31] = hook[0];
            g_cpu.r[29] = hook[1];
            g_cpu.r[30] = hook[2];
            for (int i = 0; i < 8; i++) g_cpu.r[16 + i] = hook[3 + i];
            g_cpu.r[28] = hook[11];
            g_cpu.r[2] = 1;
            /* resume after the setjmp() call that filled the hook buffer */
            rt_call(hook[0]);   /* 0x8009b924 is a recompiled entry (extra_seeds.txt) */
            rt_log("interrupt hook returned without ReturnFromException");
        }
        s_exc_env = prev;
    } else {
        /* No hook: BIOS default handler just returns; ack VBlank so we don't spin. */
        g_istat &= ~1u;
    }
    u64 cycles = g_cpu.cycles, next = g_cpu.next_event;
    g_cpu = saved;
    g_cpu.cycles = cycles;
    g_cpu.next_event = next;
    g_depth = depth;
    s_in_exception--;
    g_irq_cycles += g_cpu.cycles - irq_t0;
}

/* Sampling profiler over the shadow call stack (enabled with --profile). */
int g_profile;
static struct { u32 addr, n; } s_prof[4096];
static u32 s_prof_total;
void rt_profile_sample(void) {
    if (!g_profile || !g_depth) return;
    u32 a = g_callstack[(g_depth - 1) & 1023];
    /* attribute time spent in VSync/DrawSync to the waiting function itself */
    u32 h = (a >> 2) & 4095;
    while (s_prof[h].n && s_prof[h].addr != a) h = (h + 1) & 4095;
    s_prof[h].addr = a; s_prof[h].n++; s_prof_total++;
}
void rt_profile_dump(void) {
    if (!g_profile || !s_prof_total) return;
    fprintf(stderr, "profile (%u samples):\n", s_prof_total);
    for (int k = 0; k < 15; k++) {
        int best = -1;
        for (int i = 0; i < 4096; i++) if (s_prof[i].n && (best < 0 || s_prof[i].n > s_prof[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "  %08x %5.1f%%\n", s_prof[best].addr, 100.0 * s_prof[best].n / s_prof_total);
        s_prof[best].n = 0;
    }
}

static void profile_tick(void) {
    rt_profile_sample();
    sched_event(EVT_PROFILE, 4000, profile_tick);
}
void rt_profile_start(void) { sched_event(EVT_PROFILE, 4000, profile_tick); }

void rt_event(void) {
    for (int guard = 0; guard < 16; guard++) {
        sched_run();
        u32 sr = g_cpu.cop0[COP0_SR];
        if (!s_in_exception && (g_istat & g_imask) && (sr & 0x401) == 0x401) {
            take_interrupt();
            continue;
        }
        break;
    }
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
void cpu_state_io(StateIO *io) {
    SS_VAR(io, g_cpu.r); SS_VAR(io, g_cpu.hi); SS_VAR(io, g_cpu.lo); SS_VAR(io, g_cpu.pc);
    SS_VAR(io, g_cpu.cop0); SS_VAR(io, g_cpu.cycles); SS_VAR(io, g_cpu.next_event);
}
