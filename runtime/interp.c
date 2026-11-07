/* MIPS-I interpreter fallback.
 *
 * Used for code the recompiler did not cover (unknown indirect targets,
 * resuming at a BIOS-hook address in the middle of a function). Calls whose
 * target has a recompiled function are executed natively, so the interpreter
 * normally only runs a handful of instructions before handing back.
 */
#include "recomp.h"
#include <stdio.h>

RecompFn rt_lookup(u32 addr);   /* rt.c */
bool rt_is_hle(u32 addr);       /* rt.c: BIOS vector addresses */

static inline u32 ld(u32 a, int sz, bool sgn) {
    switch (sz) {
    case 1: return sgn ? (u32)(s8)mem_r8(a) : mem_r8(a);
    case 2: return sgn ? (u32)(s16)mem_r16(a) : mem_r16(a);
    default: return mem_r32(a);
    }
}

/* Execute one non-branch instruction. Returns false if `w` is a control
 * transfer (handled by the caller). */
static bool exec_simple(u32 pc, u32 w) {
    u32 *r = g_cpu.r;
    u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    u32 sa = (w >> 6) & 31;
    s32 simm = (s16)(w & 0xffff);
    u32 uimm = w & 0xffff;
    u32 v = 0;
    int dst = -1;
    switch (op) {
    case 0x00:
        switch (w & 63) {
        case 0x00: v = r[rt] << sa; dst = rd; break;
        case 0x02: v = r[rt] >> sa; dst = rd; break;
        case 0x03: v = (u32)((s32)r[rt] >> sa); dst = rd; break;
        case 0x04: v = r[rt] << (r[rs] & 31); dst = rd; break;
        case 0x06: v = r[rt] >> (r[rs] & 31); dst = rd; break;
        case 0x07: v = (u32)((s32)r[rt] >> (r[rs] & 31)); dst = rd; break;
        case 0x08: case 0x09: return false;
        case 0x0c: rt_syscall(pc); return true;
        case 0x0d: rt_break(pc, (w >> 6) & 0xfffff); return true;
        case 0x10: v = g_cpu.hi; dst = rd; break;
        case 0x11: g_cpu.hi = r[rs]; return true;
        case 0x12: v = g_cpu.lo; dst = rd; break;
        case 0x13: g_cpu.lo = r[rs]; return true;
        case 0x18: { s64 p = (s64)(s32)r[rs] * (s32)r[rt]; g_cpu.lo = (u32)p; g_cpu.hi = (u32)(p >> 32); return true; }
        case 0x19: { u64 p = (u64)r[rs] * r[rt]; g_cpu.lo = (u32)p; g_cpu.hi = (u32)(p >> 32); return true; }
        case 0x1a: op_div(r[rs], r[rt]); return true;
        case 0x1b: op_divu(r[rs], r[rt]); return true;
        case 0x20: case 0x21: v = r[rs] + r[rt]; dst = rd; break;
        case 0x22: case 0x23: v = r[rs] - r[rt]; dst = rd; break;
        case 0x24: v = r[rs] & r[rt]; dst = rd; break;
        case 0x25: v = r[rs] | r[rt]; dst = rd; break;
        case 0x26: v = r[rs] ^ r[rt]; dst = rd; break;
        case 0x27: v = ~(r[rs] | r[rt]); dst = rd; break;
        case 0x2a: v = (s32)r[rs] < (s32)r[rt]; dst = rd; break;
        case 0x2b: v = r[rs] < r[rt]; dst = rd; break;
        default: rt_invalid(pc, w); return true;
        }
        break;
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        return false;
    case 0x08: case 0x09: v = r[rs] + simm; dst = rt; break;
    case 0x0a: v = (s32)r[rs] < simm; dst = rt; break;
    case 0x0b: v = r[rs] < (u32)simm; dst = rt; break;
    case 0x0c: v = r[rs] & uimm; dst = rt; break;
    case 0x0d: v = r[rs] | uimm; dst = rt; break;
    case 0x0e: v = r[rs] ^ uimm; dst = rt; break;
    case 0x0f: v = uimm << 16; dst = rt; break;
    case 0x10:
        if (rs == 0) { v = rt_mfc0(rd); dst = rt; }
        else if (rs == 4) rt_mtc0(rd, r[rt]);
        else if ((w & 63) == 0x10) rt_rfe();
        break;
    case 0x12:
        if (rs & 0x10) gte_command(w);
        else if (rs == 0) { v = gte_read_data(rd); dst = rt; }
        else if (rs == 2) { v = gte_read_ctrl(rd); dst = rt; }
        else if (rs == 4) gte_write_data(rd, r[rt]);
        else if (rs == 6) gte_write_ctrl(rd, r[rt]);
        break;
    case 0x20: v = ld(r[rs] + simm, 1, true); dst = rt; break;
    case 0x21: v = ld(r[rs] + simm, 2, true); dst = rt; break;
    case 0x22: v = op_lwl(r[rt], r[rs] + simm); dst = rt; break;
    case 0x23: v = ld(r[rs] + simm, 4, false); dst = rt; break;
    case 0x24: v = ld(r[rs] + simm, 1, false); dst = rt; break;
    case 0x25: v = ld(r[rs] + simm, 2, false); dst = rt; break;
    case 0x26: v = op_lwr(r[rt], r[rs] + simm); dst = rt; break;
    case 0x28: mem_w8(r[rs] + simm, r[rt]); break;
    case 0x29: mem_w16(r[rs] + simm, r[rt]); break;
    case 0x2a: op_swl(r[rt], r[rs] + simm); break;
    case 0x2b: mem_w32(r[rs] + simm, r[rt]); break;
    case 0x2e: op_swr(r[rt], r[rs] + simm); break;
    case 0x32: gte_write_data(rt, mem_r32(r[rs] + simm)); break;
    case 0x3a: mem_w32(r[rs] + simm, gte_read_data(rt)); break;
    default: rt_invalid(pc, w); break;
    }
    if (dst > 0) r[dst] = v;
    return true;
}

/* Call `t` as a function from interpreted code: native if available. */
static void interp_call(u32 t, u32 ret) {
    if (rt_lookup(t) || rt_is_hle(t)) rt_call(t);
    else interp_run(t, ret);
}

int g_no_interp;   /* --no-interp: entering the interpreter is fatal */

void interp_run(u32 pc, u32 ret_addr) {
    u32 *r = g_cpu.r;
    if (g_no_interp)
        rt_fatal("interpreter entered at %08x (ra=%08x): missing recompiled entry", pc, g_cpu.r[31]);
    u32 entry_sp = r[29];
    if (g_trace) rt_log("interp: enter %08x (ret %08x)", pc, ret_addr);
    for (;;) {
        if (pc == ret_addr && r[29] == entry_sp) return;
        /* Hand off to native code if we land on a recompiled entry. */
        if (pc != ret_addr) {
            RecompFn fn = rt_lookup(pc);
            if (fn) {
                fn();
                pc = r[31];
                if (pc == ret_addr) return;
                continue;
            }
            if (rt_is_hle(pc)) { rt_call(pc); pc = r[31]; continue; }
        }
        u32 w = mem_r32(pc);
        g_cpu.pc = pc;
        g_cpu.cycles += CYCLES_PER_INSN;
        if (exec_simple(pc, w)) { pc += 4; continue; }

        /* control transfer */
        u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
        s32 off = (s16)(w & 0xffff) * 4;
        u32 target = 0;
        bool taken = false, link = false, call = false, jreg = false;
        int link_reg = 31;
        switch (op) {
        case 0x00:
            target = r[rs];
            jreg = true;
            if ((w & 63) == 0x09) { link = true; link_reg = rd; call = true; }
            taken = true;
            break;
        case 0x01: {
            bool ge = (s32)r[rs] >= 0;
            taken = (rt & 1) ? ge : !ge;
            if ((rt & 0x1e) == 0x10) { link = true; call = taken; }
            target = pc + 4 + off;
            break;
        }
        case 0x02: case 0x03:
            target = ((pc + 4) & 0xf0000000u) | ((w & 0x3ffffff) << 2);
            taken = true;
            if (op == 3) { link = true; call = true; }
            break;
        case 0x04: taken = r[rs] == r[rt]; target = pc + 4 + off; break;
        case 0x05: taken = r[rs] != r[rt]; target = pc + 4 + off; break;
        case 0x06: taken = (s32)r[rs] <= 0; target = pc + 4 + off; break;
        case 0x07: taken = (s32)r[rs] > 0; target = pc + 4 + off; break;
        }
        if (link && link_reg) r[link_reg] = pc + 8;
        /* delay slot */
        u32 dw = mem_r32(pc + 4);
        g_cpu.cycles += CYCLES_PER_INSN;
        if (!exec_simple(pc + 4, dw)) rt_log("interp: branch in delay slot at %08x", pc + 4);
        if (call) {
            interp_call(target, pc + 8);
            pc = pc + 8;
        } else if (taken) {
            if (target <= pc) EVT_CHECK();
            if (jreg && target == ret_addr && r[29] == entry_sp) return;
            pc = target;
        } else {
            pc += 8;
        }
    }
}
