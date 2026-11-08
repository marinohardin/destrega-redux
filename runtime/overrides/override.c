#include "override.h"
#include "../savestate.h"
#include <stdio.h>
#include <string.h>

int g_verify_overrides;

#define MAX_OVERRIDES 32
static struct { const char *name; u64 calls, nonzero, bad; } s_stats[MAX_OVERRIDES];

u32 guest_call(RecompFn fn, u32 a0, u32 a1, u32 a2, u32 a3) {
    g_cpu.r[4] = a0;
    g_cpu.r[5] = a1;
    g_cpu.r[6] = a2;
    g_cpu.r[7] = a3;
    fn();
    return g_cpu.r[2];
}

static int stat_slot(const char *name) {
    for (int i = 0; i < MAX_OVERRIDES; i++) {
        if (s_stats[i].name == name) return i;
        if (!s_stats[i].name) { s_stats[i].name = name; return i; }
    }
    return MAX_OVERRIDES - 1;
}

/* Registers a MIPS callee must preserve (s0-s7, gp, sp, fp, ra). */
static bool callee_saved_equal(const Cpu *a, const Cpu *b) {
    for (int i = 16; i <= 23; i++) if (a->r[i] != b->r[i]) return false;
    for (int i = 28; i <= 31; i++) if (a->r[i] != b->r[i]) return false;
    return true;
}

/* Whole-memory snapshots (RAM + scratchpad). Verification is a dev tool, so it
 * trades speed for catching every side effect, including state a callee keeps
 * between calls. */
typedef struct { u8 ram[RAM_SIZE]; u8 scratch[sizeof g_scratch]; } MemImage;
static MemImage s_before, s_orig;

static void mem_save(MemImage *m) {
    memcpy(m->ram, g_ram, RAM_SIZE);
    memcpy(m->scratch, g_scratch, sizeof g_scratch);
}

static void mem_restore(const MemImage *m) {
    memcpy(g_ram, m->ram, RAM_SIZE);
    memcpy(g_scratch, m->scratch, sizeof g_scratch);
}

/* The dead stack below the entry sp holds each version's private frames and
 * legitimately differs; everything else must match. */
#define DEAD_STACK_BYTES 1024
static u32 s_first_diff;   /* guest address of the first differing byte (diagnostics) */

static bool mem_equal_live(const MemImage *saved, u32 entry_sp) {
    /* The game's stack lives in main RAM normally but in the 1 KB scratchpad
     * during fights; exclude the dead part of whichever one sp points into. */
    bool sp_in_scratch = (entry_sp & 0xfffffc00u) == 0x1f800000u;
    u32 sp_off = sp_in_scratch ? (entry_sp & 0x3ffu) : (entry_sp & 0x1fffffu);
    u32 dead_lo = sp_off > DEAD_STACK_BYTES ? sp_off - DEAD_STACK_BYTES : 0;
    for (u32 i = 0; i < RAM_SIZE; i++) {
        if (!sp_in_scratch && i >= dead_lo && i < sp_off) continue;
        if (saved->ram[i] != g_ram[i]) { s_first_diff = 0x80000000u + i; return false; }
    }
    for (u32 i = 0; i < sizeof g_scratch; i++) {
        if (sp_in_scratch && i >= dead_lo && i < sp_off) continue;
        if (saved->scratch[i] != g_scratch[i]) { s_first_diff = 0x1f800000u + i; return false; }
    }
    return true;
}

void override_call(const char *name, RecompFn orig, RecompFn fresh) {
    if (!g_verify_overrides) { fresh(); return; }
    g_ss_unsafe++;   /* the verification keeps guest state in this C frame */

    EVT_CHECK();                      /* deliver pending interrupts before snapshotting */
    Cpu saved = g_cpu;
    u32 depth = g_depth;
    u32 entry_sp = saved.r[29];

    mem_save(&s_before);
    orig();
    Cpu after_orig = g_cpu;
    mem_save(&s_orig);

    g_cpu = saved;
    g_depth = depth;
    mem_restore(&s_before);
    fresh();

    int slot = stat_slot(name);
    s_stats[slot].calls++;
    s_stats[slot].nonzero += after_orig.r[2] != 0;
    bool regs_ok = g_cpu.r[2] == after_orig.r[2] && callee_saved_equal(&g_cpu, &after_orig);
    bool timing_ok = g_cpu.cycles == after_orig.cycles;
    bool mem_ok = mem_equal_live(&s_orig, entry_sp);
    if (!regs_ok || !mem_ok || !timing_ok) {
        if (s_stats[slot].bad++ < 10)
            rt_log("override %s MISMATCH (%s %08x): args %08x %08x %08x %08x  result orig=%08x new=%08x sp=%08x",
                   name, !regs_ok ? "registers" : !mem_ok ? "memory" : "cycles", s_first_diff, saved.r[4], saved.r[5],
                   saved.r[6], saved.r[7], after_orig.r[2], g_cpu.r[2], entry_sp);
        /* Keep the original's behaviour so a bad override cannot change the game. */
        g_cpu = after_orig;
        mem_restore(&s_orig);
    }
    g_ss_unsafe--;
}

void override_report(void) {
    if (!g_verify_overrides) return;
    for (int i = 0; i < MAX_OVERRIDES && s_stats[i].name; i++)
        fprintf(stderr, "override %-24s %10llu calls (%llu non-zero results), %llu mismatches\n",
                s_stats[i].name, (unsigned long long)s_stats[i].calls,
                (unsigned long long)s_stats[i].nonzero, (unsigned long long)s_stats[i].bad);
}
