/* C port of recomp/ (*.py) (see recompiler.h). Structure follows the Python files:
 *   mips.py     -> decode(), disasm(), reads_writes()
 *   analyze.py  -> Analyzer: trace(), const_of(), find_jtable()
 *   codegen.py  -> Emitter: simple(), emit()
 *   recomp.py   -> recomp_run(): seeds, overlays, output files
 * Python's sets and dicts become U32Set; where Python iterates a set in sorted order, so does
 * this file, and where it relies on insertion order (lists), a vector is used. */
#include "recompiler.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

static char s_err[512];
static bool s_failed;

static void fail(const char *fmt, ...)
{
    if (s_failed) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof s_err, fmt, ap);
    va_end(ap);
    s_failed = true;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "recompiler: out of memory\n"); abort(); }
    return p;
}
static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "recompiler: out of memory\n"); abort(); }
    return p;
}

/* ---- containers ---------------------------------------------------------------------------- */

typedef struct { u32 *v; size_t n, cap; } Vec;
static void vec_push(Vec *a, u32 x)
{
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 16; a->v = xrealloc(a->v, a->cap * sizeof *a->v); }
    a->v[a->n++] = x;
}
static void vec_free(Vec *a) { free(a->v); memset(a, 0, sizeof *a); }
static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

typedef struct { u32 *k; u8 *used; size_t n, cap; } Set;   /* open addressing, cap power of 2 */
static size_t set_hash(u32 x, size_t cap) { return (size_t)((x * 2654435761u) ^ (x >> 15)) & (cap - 1); }
static bool set_has(const Set *s, u32 x)
{
    if (!s->cap) return false;
    for (size_t i = set_hash(x, s->cap);; i = (i + 1) & (s->cap - 1)) {
        if (!s->used[i]) return false;
        if (s->k[i] == x) return true;
    }
}
static bool set_add(Set *s, u32 x);
static void set_grow(Set *s)
{
    Set t = {0};
    t.cap = s->cap ? s->cap * 2 : 64;
    t.k = xmalloc(t.cap * sizeof *t.k);
    t.used = calloc(t.cap, 1);
    if (!t.used) abort();
    for (size_t i = 0; i < s->cap; i++) if (s->used[i]) set_add(&t, s->k[i]);
    free(s->k); free(s->used);
    *s = t;
}
static bool set_add(Set *s, u32 x)   /* true if newly added */
{
    if ((s->n + 1) * 2 > s->cap) set_grow(s);
    for (size_t i = set_hash(x, s->cap);; i = (i + 1) & (s->cap - 1)) {
        if (!s->used[i]) { s->used[i] = 1; s->k[i] = x; s->n++; return true; }
        if (s->k[i] == x) return false;
    }
}
static void set_free(Set *s) { free(s->k); free(s->used); memset(s, 0, sizeof *s); }
static Vec set_sorted(const Set *s)
{
    Vec v = {0};
    for (size_t i = 0; i < s->cap; i++) if (s->used[i]) vec_push(&v, s->k[i]);
    if (v.n) qsort(v.v, v.n, sizeof *v.v, cmp_u32);
    return v;
}

typedef struct { char *s; size_t n, cap; } Buf;
static void buf_putn(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 4096;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof tmp) { buf_putn(b, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_putn(b, big, (size_t)n);
    free(big);
}

/* ---- mips.py ------------------------------------------------------------------------------- */

enum {
    OP_INVALID, OP_SLL, OP_SRL, OP_SRA, OP_SLLV, OP_SRLV, OP_SRAV, OP_JR, OP_JALR, OP_SYSCALL, OP_BREAK,
    OP_MFHI, OP_MTHI, OP_MFLO, OP_MTLO, OP_MULT, OP_MULTU, OP_DIV, OP_DIVU, OP_ADD, OP_ADDU, OP_SUB,
    OP_SUBU, OP_AND, OP_OR, OP_XOR, OP_NOR, OP_SLT, OP_SLTU,
    OP_ADDI, OP_ADDIU, OP_SLTI, OP_SLTIU, OP_ANDI, OP_ORI, OP_XORI, OP_LUI,
    OP_LB, OP_LH, OP_LWL, OP_LW, OP_LBU, OP_LHU, OP_LWR, OP_SB, OP_SH, OP_SWL, OP_SW, OP_SWR,
    OP_LWC2, OP_SWC2,
    OP_BLTZ, OP_BGEZ, OP_BLTZAL, OP_BGEZAL, OP_J, OP_JAL, OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ,
    OP_MFC0, OP_MTC0, OP_RFE, OP_COP2, OP_MFC2, OP_CFC2, OP_MTC2, OP_CTC2,
    OP_COUNT
};
static const char *const OPNAME[OP_COUNT] = {
    "invalid", "sll", "srl", "sra", "sllv", "srlv", "srav", "jr", "jalr", "syscall", "break",
    "mfhi", "mthi", "mflo", "mtlo", "mult", "multu", "div", "divu", "add", "addu", "sub",
    "subu", "and", "or", "xor", "nor", "slt", "sltu",
    "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
    "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "sb", "sh", "swl", "sw", "swr",
    "lwc2", "swc2",
    "bltz", "bgez", "bltzal", "bgezal", "j", "jal", "beq", "bne", "blez", "bgtz",
    "mfc0", "mtc0", "rfe", "cop2", "mfc2", "cfc2", "mtc2", "ctc2",
};
static const char *const REG[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

typedef struct {
    u32 addr, word;
    int op;
    int rs, rt, rd, sa;
    s32 imm;       /* sign- or zero-extended per op */
    u32 target;
} Ins;

static bool is_branch(int o)
{
    return o == OP_BEQ || o == OP_BNE || o == OP_BLEZ || o == OP_BGTZ || o == OP_BLTZ || o == OP_BGEZ ||
           o == OP_BLTZAL || o == OP_BGEZAL;
}
static bool has_delay(int o) { return is_branch(o) || o == OP_J || o == OP_JAL || o == OP_JR || o == OP_JALR; }
static s32 sx16(u32 v) { return (v & 0x8000) ? (s32)v - 0x10000 : (s32)v; }

static Ins decode(u32 word, u32 addr)
{
    Ins i = {addr, word, OP_INVALID, 0, 0, 0, 0, 0, 0};
    u32 op = word >> 26;
    int rs = (word >> 21) & 31, rt = (word >> 16) & 31, rd = (word >> 11) & 31, sa = (word >> 6) & 31;
    u32 imm16 = word & 0xffff;
    if (op == 0) {
        static const signed char special[64] = {
            [0x00] = OP_SLL, [0x02] = OP_SRL, [0x03] = OP_SRA, [0x04] = OP_SLLV, [0x06] = OP_SRLV,
            [0x07] = OP_SRAV, [0x08] = OP_JR, [0x09] = OP_JALR, [0x0c] = OP_SYSCALL, [0x0d] = OP_BREAK,
            [0x10] = OP_MFHI, [0x11] = OP_MTHI, [0x12] = OP_MFLO, [0x13] = OP_MTLO,
            [0x18] = OP_MULT, [0x19] = OP_MULTU, [0x1a] = OP_DIV, [0x1b] = OP_DIVU,
            [0x20] = OP_ADD, [0x21] = OP_ADDU, [0x22] = OP_SUB, [0x23] = OP_SUBU,
            [0x24] = OP_AND, [0x25] = OP_OR, [0x26] = OP_XOR, [0x27] = OP_NOR, [0x2a] = OP_SLT, [0x2b] = OP_SLTU,
        };
        int m = special[word & 63];
        if (!m) return i;
        i.op = m; i.rs = rs; i.rt = rt; i.rd = rd; i.sa = sa;
        return i;
    }
    if (op == 1) {
        int m = rt == 0 ? OP_BLTZ : rt == 1 ? OP_BGEZ : rt == 0x10 ? OP_BLTZAL : rt == 0x11 ? OP_BGEZAL : 0;
        if (!m) return i;
        i.op = m; i.rs = rs; i.rt = rt; i.imm = sx16(imm16);
        i.target = addr + 4 + ((u32)sx16(imm16) << 2);
        return i;
    }
    if (op == 2 || op == 3) {
        i.op = op == 2 ? OP_J : OP_JAL;
        i.target = ((addr + 4) & 0xf0000000u) | ((word & 0x3ffffff) << 2);
        return i;
    }
    if (op >= 4 && op <= 7) {
        static const int m[4] = {OP_BEQ, OP_BNE, OP_BLEZ, OP_BGTZ};
        i.op = m[op - 4]; i.rs = rs; i.rt = rt; i.imm = sx16(imm16);
        i.target = addr + 4 + ((u32)sx16(imm16) << 2);
        return i;
    }
    if (op == 0x10) {
        if (rs == 0) { i.op = OP_MFC0; i.rs = rs; i.rt = rt; i.rd = rd; }
        else if (rs == 4) { i.op = OP_MTC0; i.rs = rs; i.rt = rt; i.rd = rd; }
        else if (rs == 0x10 && (word & 63) == 0x10) i.op = OP_RFE;
        return i;
    }
    if (op == 0x12) {
        if (rs & 0x10) { i.op = OP_COP2; i.imm = (s32)(word & 0x1ffffff); return i; }
        int m = rs == 0 ? OP_MFC2 : rs == 2 ? OP_CFC2 : rs == 4 ? OP_MTC2 : rs == 6 ? OP_CTC2 : 0;
        if (!m) return i;
        i.op = m; i.rs = rs; i.rt = rt; i.rd = rd;
        return i;
    }
    static const signed char primary[64] = {
        [0x08] = OP_ADDI, [0x09] = OP_ADDIU, [0x0a] = OP_SLTI, [0x0b] = OP_SLTIU,
        [0x0c] = OP_ANDI, [0x0d] = OP_ORI, [0x0e] = OP_XORI, [0x0f] = OP_LUI,
        [0x20] = OP_LB, [0x21] = OP_LH, [0x22] = OP_LWL, [0x23] = OP_LW, [0x24] = OP_LBU, [0x25] = OP_LHU,
        [0x26] = OP_LWR, [0x28] = OP_SB, [0x29] = OP_SH, [0x2a] = OP_SWL, [0x2b] = OP_SW, [0x2e] = OP_SWR,
        [0x32] = OP_LWC2, [0x3a] = OP_SWC2,
    };
    int m = primary[op];
    if (!m) return i;
    i.op = m; i.rs = rs; i.rt = rt;
    bool zext = m == OP_ANDI || m == OP_ORI || m == OP_XORI || m == OP_LUI;
    i.imm = zext ? (s32)imm16 : sx16(imm16);
    return i;
}

static bool is_special_op(int o) { return o >= OP_SLL && o <= OP_SLTU; }
static bool is_primary_op(int o) { return o >= OP_ADDI && o <= OP_SWC2; }

/* mips.py disasm(): the text of the comments in the generated code */
static void disasm(const Ins *i, char *out, size_t n)
{
    int o = i->op;
    const char *on = OPNAME[o];
    if (o == OP_SLL || o == OP_SRL || o == OP_SRA) {
        if (i->word == 0) snprintf(out, n, "nop");
        else snprintf(out, n, "%s %s, %s, %d", on, REG[i->rd], REG[i->rt], i->sa);
    } else if (o == OP_SLLV || o == OP_SRLV || o == OP_SRAV)
        snprintf(out, n, "%s %s, %s, %s", on, REG[i->rd], REG[i->rt], REG[i->rs]);
    else if (o == OP_JR) snprintf(out, n, "jr %s", REG[i->rs]);
    else if (o == OP_JALR) snprintf(out, n, "jalr %s, %s", REG[i->rd], REG[i->rs]);
    else if (o == OP_MFHI || o == OP_MFLO) snprintf(out, n, "%s %s", on, REG[i->rd]);
    else if (o == OP_MTHI || o == OP_MTLO) snprintf(out, n, "%s %s", on, REG[i->rs]);
    else if (o == OP_MULT || o == OP_MULTU || o == OP_DIV || o == OP_DIVU)
        snprintf(out, n, "%s %s, %s", on, REG[i->rs], REG[i->rt]);
    else if (is_special_op(o)) {
        if (o == OP_SYSCALL || o == OP_BREAK) snprintf(out, n, "%s", on);
        else snprintf(out, n, "%s %s, %s, %s", on, REG[i->rd], REG[i->rs], REG[i->rt]);
    } else if (o == OP_BEQ || o == OP_BNE)
        snprintf(out, n, "%s %s, %s, 0x%08x", on, REG[i->rs], REG[i->rt], i->target);
    else if (o == OP_BLEZ || o == OP_BGTZ || o == OP_BLTZ || o == OP_BGEZ || o == OP_BLTZAL || o == OP_BGEZAL)
        snprintf(out, n, "%s %s, 0x%08x", on, REG[i->rs], i->target);
    else if (o == OP_J || o == OP_JAL) snprintf(out, n, "%s 0x%08x", on, i->target);
    else if (o == OP_LUI) snprintf(out, n, "lui %s, 0x%04x", REG[i->rt], (u32)i->imm);
    else if (o == OP_ANDI || o == OP_ORI || o == OP_XORI)
        snprintf(out, n, "%s %s, %s, 0x%x", on, REG[i->rt], REG[i->rs], (u32)i->imm);
    else if (o == OP_ADDI || o == OP_ADDIU || o == OP_SLTI || o == OP_SLTIU)
        snprintf(out, n, "%s %s, %s, %d", on, REG[i->rt], REG[i->rs], i->imm);
    else if (o == OP_LWC2 || o == OP_SWC2) snprintf(out, n, "%s $%d, %d(%s)", on, i->rt, i->imm, REG[i->rs]);
    else if (is_primary_op(o)) snprintf(out, n, "%s %s, %d(%s)", on, REG[i->rt], i->imm, REG[i->rs]);
    else if (o == OP_MFC0 || o == OP_MTC0 || o == OP_MFC2 || o == OP_MTC2 || o == OP_CFC2 || o == OP_CTC2)
        snprintf(out, n, "%s %s, $%d", on, REG[i->rt], i->rd);
    else if (o == OP_COP2) snprintf(out, n, "cop2 0x%07x", (u32)i->imm);
    else if (o == OP_RFE) snprintf(out, n, "rfe");
    else snprintf(out, n, ".word 0x%08x", i->word);
}

/* mips.py reads_writes(): GPR bitmasks */
static void reads_writes(const Ins *i, u32 *rd_, u32 *wr_)
{
    int o = i->op;
    u32 r = 0, w = 0;
#define B(x) (1u << (x))
    switch (o) {
    case OP_SLL: case OP_SRL: case OP_SRA: r = B(i->rt); w = B(i->rd); break;
    case OP_SLLV: case OP_SRLV: case OP_SRAV: case OP_ADD: case OP_ADDU: case OP_SUB: case OP_SUBU:
    case OP_AND: case OP_OR: case OP_XOR: case OP_NOR: case OP_SLT: case OP_SLTU:
        r = B(i->rs) | B(i->rt); w = B(i->rd); break;
    case OP_JR: r = B(i->rs); break;
    case OP_JALR: r = B(i->rs); w = B(i->rd); break;
    case OP_MFHI: case OP_MFLO: w = B(i->rd); break;
    case OP_MTHI: case OP_MTLO: r = B(i->rs); break;
    case OP_MULT: case OP_MULTU: case OP_DIV: case OP_DIVU: case OP_BEQ: case OP_BNE:
        r = B(i->rs) | B(i->rt); break;
    case OP_BLEZ: case OP_BGTZ: case OP_BLTZ: case OP_BGEZ: r = B(i->rs); break;
    case OP_BLTZAL: case OP_BGEZAL: r = B(i->rs); w = B(31); break;
    case OP_JAL: w = B(31); break;
    case OP_ADDI: case OP_ADDIU: case OP_SLTI: case OP_SLTIU: case OP_ANDI: case OP_ORI: case OP_XORI:
        r = B(i->rs); w = B(i->rt); break;
    case OP_LUI: w = B(i->rt); break;
    case OP_LB: case OP_LH: case OP_LW: case OP_LBU: case OP_LHU: r = B(i->rs); w = B(i->rt); break;
    case OP_LWL: case OP_LWR: r = B(i->rs) | B(i->rt); w = B(i->rt); break;
    case OP_SB: case OP_SH: case OP_SW: case OP_SWL: case OP_SWR: r = B(i->rs) | B(i->rt); break;
    case OP_LWC2: case OP_SWC2: r = B(i->rs); break;
    case OP_MFC0: case OP_MFC2: case OP_CFC2: w = B(i->rt); break;
    case OP_MTC0: case OP_MTC2: case OP_CTC2: r = B(i->rt); break;
    default: break;
    }
#undef B
    *rd_ = r;
    *wr_ = w;
}
static bool writes_reg(const Ins *i, int reg) { u32 r, w; reads_writes(i, &r, &w); return (w >> reg) & 1; }

/* ---- image.py ------------------------------------------------------------------------------ */

typedef struct {
    char name[32];
    u32 base, end;
    const u8 *data;
    u32 len;
    u32 entry;
} Image;

/* struct.unpack_from('<I', data, a - base): a negative offset counts from the end, as in Python */
static u32 img_word(const Image *m, u32 a)
{
    s64 off = (s64)a - (s64)m->base;
    if (off < 0) off += m->len;
    if (off < 0 || off + 4 > (s64)m->len) {
        fail("%s: read outside the image at %08x", m->name, a);
        return 0;
    }
    const u8 *p = m->data + off;
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static Ins img_ins(const Image *m, u32 a) { return decode(img_word(m, a), a); }
static bool img_contains(const Image *m, u64 a) { return m->base <= a && a < m->end; }

/* ---- analyze.py ---------------------------------------------------------------------------- */

typedef struct { u32 jr; Vec targets; } JTable;
typedef struct { u32 addr; char msg[96]; } Bad;

typedef struct {
    u32 entry;
    Set insns;         /* addresses incl. delay slots (decoded again from the image when needed) */
    Set labels;
    JTable *jt; size_t njt;
    Set calls, tailcalls;
    Bad *bad; size_t nbad;
} Func;

static void func_bad(Func *f, u32 a, const char *msg)
{
    f->bad = xrealloc(f->bad, (f->nbad + 1) * sizeof *f->bad);
    f->bad[f->nbad].addr = a;
    snprintf(f->bad[f->nbad].msg, sizeof f->bad[f->nbad].msg, "%s", msg);
    f->nbad++;
}
static const JTable *func_jt(const Func *f, u32 jr)
{
    for (size_t k = 0; k < f->njt; k++) if (f->jt[k].jr == jr) return &f->jt[k];
    return NULL;
}

typedef struct {
    const Image *img;
    u32 code_lo, code_hi;
    Set seeds;
    const Set *external;
    /* funcs: entry -> Func, kept as a vector plus an index set */
    Func **funcs; size_t nfuncs;
} Analyzer;

static bool an_in_code(const Analyzer *an, u32 a) { return an->code_lo <= a && a < an->code_hi && (a & 3) == 0; }

static bool const_of(Analyzer *an, Func *f, int reg, u32 before, u32 *out)
{
    u32 a = before - 4;
    u32 add = 0;
    for (int k = 0; k < 32; k++) {
        if (a < f->entry) break;
        Ins i = img_ins(an->img, a);
        if (writes_reg(&i, reg)) {
            if ((i.op == OP_ADDIU || i.op == OP_ORI) && i.rs == reg) add += (u32)i.imm;
            else if (i.op == OP_ADDIU && i.rs != reg) {
                u32 c;
                if (!const_of(an, f, i.rs, a, &c)) return false;
                *out = c + (u32)i.imm + add;
                return true;
            } else if (i.op == OP_LUI) {
                *out = ((u32)i.imm << 16) + add;
                return true;
            } else if (i.op == OP_ADDU && i.rt == 0) {
                u32 c;
                if (!const_of(an, f, i.rs, a, &c)) return false;
                *out = c + add;
                return true;
            } else return false;
        }
        a -= 4;
    }
    /* whole-function fallback */
    Set vals = {0};
    for (a = f->entry; a < before; a += 4) {
        Ins i = img_ins(an->img, a);
        if (!writes_reg(&i, reg)) continue;
        if (i.op == OP_ADDIU && i.rs != reg) {
            Ins j = img_ins(an->img, a - 4);
            if (j.op == OP_LUI && j.rt == i.rs) set_add(&vals, ((u32)j.imm << 16) + (u32)i.imm);
            else { set_free(&vals); return false; }
        } else if (i.op == OP_LUI) {
            Ins j = img_ins(an->img, a + 4);
            if (!(j.op == OP_ADDIU && j.rs == reg && j.rt == reg)) set_add(&vals, (u32)i.imm << 16);
        } else { set_free(&vals); return false; }
    }
    bool ok = vals.n == 1;
    if (ok) for (size_t k = 0; k < vals.cap; k++) if (vals.used[k]) *out = vals.k[k];
    set_free(&vals);
    return ok;
}

static bool find_jtable(Analyzer *an, Func *f, u32 jr_addr, Vec *targets)
{
    const Image *img = an->img;
    int reg = img_ins(img, jr_addr).rs;
    bool have_lw = false;
    u32 lw_at = 0, a = jr_addr - 4;
    for (int k = 0; k < 8; k++) {
        Ins i = img_ins(img, a);
        if (i.op == OP_LW && i.rt == reg) { lw_at = a; have_lw = true; break; }
        if (writes_reg(&i, reg)) return false;
        a -= 4;
    }
    if (!have_lw) return false;
    Ins lw = img_ins(img, lw_at);
    bool have_base = false;
    u32 base = 0;
    a = lw_at - 4;
    for (int k = 0; k < 8; k++) {
        Ins i = img_ins(img, a);
        if (writes_reg(&i, lw.rs)) {
            if (i.op == OP_ADDU) {
                int regs[2] = {i.rs, i.rt};
                for (int b = 0; b < 2; b++) {
                    u32 c;
                    if (const_of(an, f, regs[b], a, &c)) { base = c; have_base = true; break; }
                }
            } else if (i.op == OP_ADDIU || i.op == OP_LUI) {
                have_base = const_of(an, f, lw.rs, lw_at, &base);
            }
            break;
        }
        a -= 4;
    }
    if (!have_base) return false;
    base += (u32)lw.imm;
    bool have_count = false;
    s32 count = 0;
    a = lw_at - 4;
    for (int k = 0; k < 16; k++) {
        Ins i = img_ins(img, a);
        if (i.op == OP_SLTIU) { count = i.imm; have_count = true; break; }
        a -= 4;
    }
    s64 n = have_count ? count : 512;
    for (s64 k = 0; k < n; k++) {
        u64 ta = (u64)base + 4 * (u64)k;
        if (!img_contains(img, ta)) break;
        u32 t = img_word(img, (u32)ta);
        if (!an_in_code(an, t) || (!have_count && !((u64)f->entry <= t && (u64)t < (u64)f->entry + 0x10000))) {
            if (have_count) {
                char msg[96];
                snprintf(msg, sizeof msg, "jtable entry %lld -> 0x%x outside code", (long long)k, t);
                func_bad(f, jr_addr, msg);
            }
            break;
        }
        vec_push(targets, t);
    }
    return targets->n > 0;
}

static Func *trace(Analyzer *an, u32 entry, const Set *known)
{
    const Image *img = an->img;
    Func *f = calloc(1, sizeof *f);
    if (!f) abort();
    f->entry = entry;
    Vec work = {0};
    vec_push(&work, entry);
    Set seen = {0};
    while (work.n && !s_failed) {
        u32 a = work.v[--work.n];
        for (;;) {
            if (set_has(&seen, a)) break;
            if (!an_in_code(an, a)) { func_bad(f, a, "flow leaves code region"); break; }
            set_add(&seen, a);
            Ins i = img_ins(img, a);
            set_add(&f->insns, a);
            if (i.op == OP_INVALID) { func_bad(f, a, "invalid instruction"); break; }
            if (has_delay(i.op)) {
                Ins d = img_ins(img, a + 4);
                set_add(&f->insns, a + 4);
                if (has_delay(d.op)) func_bad(f, a, "branch in delay slot");
                int o = i.op;
                if (o == OP_JAL || o == OP_BLTZAL || o == OP_BGEZAL) { set_add(&f->calls, i.target); a += 8; continue; }
                if (o == OP_JALR) { a += 8; continue; }
                if (o == OP_J) {
                    u32 t = i.target;
                    if (t != entry && (set_has(known, t) || set_has(an->external, t) || !an_in_code(an, t))) {
                        set_add(&f->tailcalls, t);
                        break;
                    }
                    set_add(&f->labels, t);
                    a = t;
                    continue;
                }
                if (o == OP_JR) {
                    if (i.rs == 31) break;
                    Vec tbl = {0};
                    if (find_jtable(an, f, a, &tbl)) {
                        f->jt = xrealloc(f->jt, (f->njt + 1) * sizeof *f->jt);
                        f->jt[f->njt].jr = a;
                        f->jt[f->njt].targets = tbl;
                        f->njt++;
                        for (size_t k = 0; k < tbl.n; k++) { set_add(&f->labels, tbl.v[k]); vec_push(&work, tbl.v[k]); }
                    } else vec_free(&tbl);
                    break;
                }
                /* conditional branch */
                set_add(&f->labels, i.target);
                vec_push(&work, i.target);
                if (o == OP_BEQ && i.rs == 0 && i.rt == 0) break;   /* unconditional `b` */
                if (o == OP_BGEZ && i.rs == 0) break;
                a += 8;
                continue;
            }
            a += 4;
        }
    }
    vec_free(&work);
    set_free(&seen);
    return f;
}

static void an_run(Analyzer *an)
{
    Vec work = set_sorted(&an->seeds);
    Set known = {0};
    for (size_t k = 0; k < work.n; k++) set_add(&known, work.v[k]);
    while (work.n && !s_failed) {
        u32 e = work.v[--work.n];
        Func *f = trace(an, e, &known);
        an->funcs = xrealloc(an->funcs, (an->nfuncs + 1) * sizeof *an->funcs);
        an->funcs[an->nfuncs++] = f;
        Vec calls = set_sorted(&f->calls);
        for (size_t k = 0; k < calls.n; k++) {
            u32 t = calls.v[k];
            if (an_in_code(an, t) && !set_has(&known, t)) { set_add(&known, t); vec_push(&work, t); }
        }
        vec_free(&calls);
    }
    vec_free(&work);
    set_free(&known);
}

static int cmp_func(const void *a, const void *b)
{
    u32 x = (*(Func *const *)a)->entry, y = (*(Func *const *)b)->entry;
    return x < y ? -1 : x > y;
}

static void func_free(Func *f)
{
    set_free(&f->insns); set_free(&f->labels); set_free(&f->calls); set_free(&f->tailcalls);
    for (size_t k = 0; k < f->njt; k++) vec_free(&f->jt[k].targets);
    free(f->jt); free(f->bad); free(f);
}

/* ---- codegen.py ---------------------------------------------------------------------------- */

typedef struct { u32 addr; u32 wide; } WsPatch;
typedef struct { u32 br; char helper[64]; } IdleLoop;

typedef struct {
    WsPatch *ws; size_t nws; Set ws_emitted;
    IdleLoop *idle; size_t nidle; Set idle_emitted;
} CodegenGlobals;
static CodegenGlobals G;

static const WsPatch *ws_find(u32 a) { for (size_t k = 0; k < G.nws; k++) if (G.ws[k].addr == a) return &G.ws[k]; return NULL; }
static const IdleLoop *idle_find(u32 a) { for (size_t k = 0; k < G.nidle; k++) if (G.idle[k].br == a) return &G.idle[k]; return NULL; }

typedef struct Emitter Emitter;
typedef const char *(*NameOf)(Emitter *em, u32 addr);
struct Emitter {
    Func *f;
    const Image *img;
    const Set *image_funcs;
    NameOf name_of;
    void *name_ud;
    const char *ovl_tag;   /* "" for the main executable, else "_<NAME>" */
    Buf out;               /* lines joined with '\n' */
    bool first_line;
    int pending;
};

static void em_w(Emitter *em, const char *s)
{
    if (!em->first_line) buf_putn(&em->out, "\n", 1);
    em->first_line = false;
    buf_puts(&em->out, s);
}
static void em_wf(Emitter *em, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    em_w(em, tmp);
}
static void flush_cycles(Emitter *em)
{
    if (em->pending) { em_wf(em, "    CYC(%d);", em->pending); em->pending = 0; }
}

static const char *Rn(int n, char *buf) { if (n == 0) return "0"; sprintf(buf, "R%d", n); return buf; }

/* setr(): assignment to a GPR; writes to $zero become a comment (or a discarded load) */
static void setr(char *out, size_t n, const Ins *i, int dst, const char *expr)
{
    if (dst == 0) {
        if (!strncmp(expr, "mem_", 4)) snprintf(out, n, "(void)%s;", expr);
        else { char d[96]; disasm(i, d, sizeof d); snprintf(out, n, "/* %s */", d); }
        return;
    }
    snprintf(out, n, "R%d = %s;", dst, expr);
}

/* codegen.py FuncEmitter.simple(): one non-control instruction; "" for nothing */
static void simple(Emitter *em, const Ins *i, char *out, size_t n)
{
    char b1[8], b2[8], e[256], ea[64], t2[512];
    const char *rs = Rn(i->rs, b1), *rt = Rn(i->rt, b2);
    int o = i->op, sa = i->sa;
    s32 imm = i->imm;
    out[0] = 0;
    if (imm) snprintf(ea, sizeof ea, "%s + %d", rs, imm); else snprintf(ea, sizeof ea, "%s", rs);
    switch (o) {
    case OP_SLL:
        if (i->word == 0) return;
        snprintf(e, sizeof e, "%s << %d", rt, sa); setr(out, n, i, i->rd, e); return;
    case OP_SRL: snprintf(e, sizeof e, "%s >> %d", rt, sa); setr(out, n, i, i->rd, e); return;
    case OP_SRA: snprintf(e, sizeof e, "(u32)((s32)%s >> %d)", rt, sa); setr(out, n, i, i->rd, e); return;
    case OP_SLLV: snprintf(e, sizeof e, "%s << (%s & 31)", rt, rs); setr(out, n, i, i->rd, e); return;
    case OP_SRLV: snprintf(e, sizeof e, "%s >> (%s & 31)", rt, rs); setr(out, n, i, i->rd, e); return;
    case OP_SRAV: snprintf(e, sizeof e, "(u32)((s32)%s >> (%s & 31))", rt, rs); setr(out, n, i, i->rd, e); return;
    case OP_MFHI: setr(out, n, i, i->rd, "HI"); return;
    case OP_MFLO: setr(out, n, i, i->rd, "LO"); return;
    case OP_MTHI: snprintf(out, n, "HI = %s;", rs); return;
    case OP_MTLO: snprintf(out, n, "LO = %s;", rs); return;
    case OP_MULT:
        snprintf(out, n, "{ s64 p = (s64)(s32)%s * (s32)%s; LO = (u32)p; HI = (u32)(p >> 32); }", rs, rt); return;
    case OP_MULTU:
        snprintf(out, n, "{ u64 p = (u64)%s * %s; LO = (u32)p; HI = (u32)(p >> 32); }", rs, rt); return;
    case OP_DIV: snprintf(out, n, "op_div(%s, %s);", rs, rt); return;
    case OP_DIVU: snprintf(out, n, "op_divu(%s, %s);", rs, rt); return;
    default: break;
    }
    if ((o == OP_ADD || o == OP_ADDU || o == OP_OR) && i->rd && (i->rs == 0 || i->rt == 0)) {
        int src = i->rs == 0 ? i->rt : i->rs;
        snprintf(e, sizeof e, o != OP_OR ? "%s + %s" : "%s | %s", rs, rt);
        setr(t2, sizeof t2, i, i->rd, e);
        snprintf(out, n, "%s pgxp_mov(%d, %d);", t2, i->rd, src);
        return;
    }
    switch (o) {
    case OP_ADD: case OP_ADDU: snprintf(e, sizeof e, "%s + %s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_SUB: case OP_SUBU: snprintf(e, sizeof e, "%s - %s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_AND: snprintf(e, sizeof e, "%s & %s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_OR: snprintf(e, sizeof e, "%s | %s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_XOR: snprintf(e, sizeof e, "%s ^ %s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_NOR: snprintf(e, sizeof e, "~(%s | %s)", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_SLT: snprintf(e, sizeof e, "(s32)%s < (s32)%s", rs, rt); setr(out, n, i, i->rd, e); return;
    case OP_SLTU: snprintf(e, sizeof e, "%s < %s", rs, rt); setr(out, n, i, i->rd, e); return;
    default: break;
    }
    if ((o == OP_ADDI || o == OP_ADDIU) && imm == 0 && i->rt) {
        snprintf(e, sizeof e, "%s + %d", rs, imm);
        setr(t2, sizeof t2, i, i->rt, e);
        snprintf(out, n, "%s pgxp_mov(%d, %d);", t2, i->rt, i->rs);
        return;
    }
    switch (o) {
    case OP_ADDI: case OP_ADDIU: snprintf(e, sizeof e, "%s + %d", rs, imm); setr(out, n, i, i->rt, e); return;
    case OP_SLTI: {
        const WsPatch *p = em->ovl_tag[0] == 0 ? ws_find(i->addr) : NULL;
        if (p) {
            set_add(&G.ws_emitted, i->addr);
            snprintf(e, sizeof e, "(s32)%s < WS_IMM(%d, %d)", rs, imm, (s32)p->wide);
        } else snprintf(e, sizeof e, "(s32)%s < %d", rs, imm);
        setr(out, n, i, i->rt, e);
        return;
    }
    case OP_SLTIU: snprintf(e, sizeof e, "%s < (u32)%d", rs, imm); setr(out, n, i, i->rt, e); return;
    case OP_ANDI: snprintf(e, sizeof e, "%s & 0x%x", rs, (u32)imm); setr(out, n, i, i->rt, e); return;
    case OP_ORI: snprintf(e, sizeof e, "%s | 0x%x", rs, (u32)imm); setr(out, n, i, i->rt, e); return;
    case OP_XORI: snprintf(e, sizeof e, "%s ^ 0x%x", rs, (u32)imm); setr(out, n, i, i->rt, e); return;
    case OP_LUI: snprintf(e, sizeof e, "0x%08x", (u32)imm << 16); setr(out, n, i, i->rt, e); return;
    case OP_LB: snprintf(e, sizeof e, "(u32)(s8)mem_r8(%s)", ea); setr(out, n, i, i->rt, e); return;
    case OP_LBU: snprintf(e, sizeof e, "mem_r8(%s)", ea); setr(out, n, i, i->rt, e); return;
    case OP_LH: snprintf(e, sizeof e, "(u32)(s16)mem_r16(%s)", ea); setr(out, n, i, i->rt, e); return;
    case OP_LHU: snprintf(e, sizeof e, "mem_r16(%s)", ea); setr(out, n, i, i->rt, e); return;
    case OP_LW:
        if (i->rt == 0) { snprintf(e, sizeof e, "mem_r32(%s)", ea); setr(out, n, i, i->rt, e); return; }
        snprintf(out, n, "{ u32 a_ = %s; R%d = mem_r32(a_); pgxp_lw(%d, a_); }", ea, i->rt, i->rt);
        return;
    case OP_LWL:
        if (!i->rt) return;
        snprintf(e, sizeof e, "op_lwl(%s, %s)", rt, ea); setr(out, n, i, i->rt, e); return;
    case OP_LWR:
        if (!i->rt) return;
        snprintf(e, sizeof e, "op_lwr(%s, %s)", rt, ea); setr(out, n, i, i->rt, e); return;
    case OP_SB: snprintf(out, n, "mem_w8(%s, %s);", ea, rt); return;
    case OP_SH: snprintf(out, n, "mem_w16(%s, %s);", ea, rt); return;
    case OP_SW: snprintf(out, n, "{ u32 a_ = %s; pgxp_sw(a_, %d); mem_w32(a_, %s); }", ea, i->rt, rt); return;
    case OP_SWL: snprintf(out, n, "op_swl(%s, %s);", rt, ea); return;
    case OP_SWR: snprintf(out, n, "op_swr(%s, %s);", rt, ea); return;
    case OP_LWC2:
        snprintf(out, n, "{ u32 a_ = %s; gte_write_data(%d, mem_r32(a_)); pgxp_lwc2(%d, a_); }", ea, i->rt, i->rt);
        return;
    case OP_SWC2:
        snprintf(out, n, "{ u32 a_ = %s; pgxp_swc2(a_, %d); mem_w32(a_, gte_read_data(%d)); }", ea, i->rt, i->rt);
        return;
    case OP_MFC2:
        snprintf(e, sizeof e, "gte_read_data(%d)", i->rd);
        if (i->rt == 0) { setr(out, n, i, i->rt, e); return; }
        setr(t2, sizeof t2, i, i->rt, e);
        snprintf(out, n, "%s pgxp_mfc2(%d, %d);", t2, i->rt, i->rd);
        return;
    case OP_CFC2: snprintf(e, sizeof e, "gte_read_ctrl(%d)", i->rd); setr(out, n, i, i->rt, e); return;
    case OP_MTC2: snprintf(out, n, "gte_write_data(%d, %s); pgxp_mtc2(%d, %d);", i->rd, rt, i->rd, i->rt); return;
    case OP_CTC2: snprintf(out, n, "gte_write_ctrl(%d, %s);", i->rd, rt); return;
    case OP_COP2: snprintf(out, n, "gte_command(0x%08x);", i->word); return;
    case OP_MFC0: snprintf(e, sizeof e, "rt_mfc0(%d)", i->rd); setr(out, n, i, i->rt, e); return;
    case OP_MTC0: snprintf(out, n, "rt_mtc0(%d, %s);", i->rd, rt); return;
    case OP_RFE: snprintf(out, n, "rt_rfe();"); return;
    case OP_SYSCALL: snprintf(out, n, "CYC_FLUSH(); rt_syscall(0x%08x);", i->addr); return;
    case OP_BREAK: snprintf(out, n, "rt_break(0x%08x, 0x%x);", i->addr, (i->word >> 6) & 0xfffff); return;
    case OP_INVALID: snprintf(out, n, "rt_invalid(0x%08x, 0x%08x); FN_RET();", i->addr, i->word); return;
    default:
        fail("unhandled %s", OPNAME[o]);
        return;
    }
}

static void call_expr(Emitter *em, u32 target, char *out, size_t n)
{
    if (set_has(em->image_funcs, target)) snprintf(out, n, "%s();", em->name_of(em, target));
    else snprintf(out, n, "rt_call(0x%08x);", target);
}

static void cond_expr(const Ins *i, char *out, size_t n)
{
    char b1[8], b2[8];
    const char *rs = Rn(i->rs, b1), *rt = Rn(i->rt, b2);
    switch (i->op) {
    case OP_BEQ: snprintf(out, n, "%s == %s", rs, rt); break;
    case OP_BNE: snprintf(out, n, "%s != %s", rs, rt); break;
    case OP_BLEZ: snprintf(out, n, "(s32)%s <= 0", rs); break;
    case OP_BGTZ: snprintf(out, n, "(s32)%s > 0", rs); break;
    case OP_BLTZ: case OP_BLTZAL: snprintf(out, n, "(s32)%s < 0", rs); break;
    default: snprintf(out, n, "(s32)%s >= 0", rs); break;   /* bgez, bgezal */
    }
}

static void goto_expr(Emitter *em, u32 src, u32 t, char *out, size_t n)
{
    if (t <= src) {
        const IdleLoop *il = em->ovl_tag[0] == 0 ? idle_find(src) : NULL;
        if (il) {
            set_add(&G.idle_emitted, src);
            snprintf(out, n, "{ %s(); EVT_CHECK(); goto L_%08x; }", il->helper, t);
            return;
        }
        snprintf(out, n, "{ EVT_CHECK(); goto L_%08x; }", t);
        return;
    }
    snprintf(out, n, "goto L_%08x;", t);
}

static void emit_func(Emitter *em)
{
    Func *f = em->f;
    Vec addrs = set_sorted(&f->insns);
    const char *name = em->name_of(em, f->entry);
    char nm[96];
    snprintf(nm, sizeof nm, "%s", name);
    em_wf(em, "void %s(void) {", nm);
    em_wf(em, "    FN_ENTER(0x%08x);", f->entry);
    em_wf(em, "    COV_HIT(%s);", nm);
    em_w(em, "    EVT_CHECK();");
    Set skip = {0};
    bool have_prev_end = false;
    char s[1024], ds[1024], tmp[1200], d1[96];
    for (size_t idx = 0; idx < addrs.n && !s_failed; idx++) {
        u32 a = addrs.v[idx];
        if (set_has(&skip, a)) continue;
        Ins i = img_ins(em->img, a);
        if (set_has(&f->labels, a)) {
            flush_cycles(em);
            em_wf(em, "L_%08x:;", a);
        }
        em->pending++;
        if (!has_delay(i.op)) {
            simple(em, &i, s, sizeof s);
            if (s[0]) em_wf(em, "    %s", s);
            if (i.op == OP_INVALID) { have_prev_end = false; continue; }
            have_prev_end = true;
            bool has_nxt = idx + 1 < addrs.n;
            u32 nxt = has_nxt ? addrs.v[idx + 1] : 0;
            if (!has_nxt || nxt != a + 4) {
                if (set_has(&f->insns, a + 4)) {
                    flush_cycles(em);
                    em_wf(em, "    goto L_%08x;", a + 4);
                    set_add(&f->labels, a + 4);
                } else {
                    flush_cycles(em);
                    em_wf(em, "    rt_unreached(0x%08x); FN_RET();", a + 4);
                }
                have_prev_end = false;
            }
            continue;
        }
        /* control transfer with delay slot */
        ds[0] = 0;
        if (set_has(&f->insns, a + 4)) {
            Ins d = img_ins(em->img, a + 4);
            if (has_delay(d.op)) { disasm(&d, d1, sizeof d1); snprintf(ds, sizeof ds, "/* branch in delay slot: %s */", d1); }
            else simple(em, &d, ds, sizeof ds);
        }
        em->pending++;
        int o = i.op;
        disasm(&i, d1, sizeof d1);
        em_wf(em, "    /* %08x: %s */", a, d1);
        bool have_after = false;
        u32 after = 0;
        if (o == OP_JAL) {
            flush_cycles(em);
            em_wf(em, "    R31 = 0x%08x;", a + 8);
            if (ds[0]) em_wf(em, "    %s", ds);
            call_expr(em, i.target, tmp, sizeof tmp);
            em_wf(em, "    %s", tmp);
            after = a + 8; have_after = true;
        } else if (o == OP_JALR) {
            char b1[8];
            flush_cycles(em);
            em_wf(em, "    { u32 t = %s;", Rn(i.rs, b1));
            if (i.rd) em_wf(em, "      R%d = 0x%08x;", i.rd, a + 8);
            if (ds[0]) em_wf(em, "      %s", ds);
            em_w(em, "      rt_call(t); }");
            after = a + 8; have_after = true;
        } else if (o == OP_BLTZAL || o == OP_BGEZAL) {
            char c[96];
            flush_cycles(em);
            cond_expr(&i, c, sizeof c);
            em_wf(em, "    { int c = %s; R31 = 0x%08x;", c, a + 8);
            if (ds[0]) em_wf(em, "      %s", ds);
            call_expr(em, i.target, tmp, sizeof tmp);
            em_wf(em, "      if (c) %s }", tmp);
            after = a + 8; have_after = true;
        } else if (o == OP_J) {
            flush_cycles(em);
            if (ds[0]) em_wf(em, "    %s", ds);
            if (set_has(&f->labels, i.target)) {
                goto_expr(em, a, i.target, tmp, sizeof tmp);
                em_wf(em, "    %s", tmp);
            } else {
                call_expr(em, i.target, tmp, sizeof tmp);
                em_wf(em, "    %s FN_RET();", tmp);
            }
        } else if (o == OP_JR) {
            char b1[8];
            flush_cycles(em);
            const JTable *jt = func_jt(f, a);
            if (i.rs == 31) {
                if (ds[0]) em_wf(em, "    %s", ds);
                em_w(em, "    FN_RET();");
            } else if (jt) {
                em_wf(em, "    { u32 t = %s;", Rn(i.rs, b1));
                if (ds[0]) em_wf(em, "      %s", ds);
                em_w(em, "      switch (t) {");
                Set uniq = {0};
                for (size_t k = 0; k < jt->targets.n; k++) set_add(&uniq, jt->targets.v[k]);
                Vec ts = set_sorted(&uniq);
                for (size_t k = 0; k < ts.n; k++) {
                    goto_expr(em, a, ts.v[k], tmp, sizeof tmp);
                    em_wf(em, "      case 0x%08x: %s", ts.v[k], tmp);
                }
                vec_free(&ts);
                set_free(&uniq);
                em_w(em, "      }");
                em_w(em, "      rt_jump(t); FN_RET(); }");
            } else {
                em_wf(em, "    { u32 t = %s;", Rn(i.rs, b1));
                if (ds[0]) em_wf(em, "      %s", ds);
                em_w(em, "      rt_jump(t); FN_RET(); }");
            }
        } else {
            flush_cycles(em);
            bool uncond = (o == OP_BEQ && i.rs == 0 && i.rt == 0) || (o == OP_BGEZ && i.rs == 0);
            if (uncond) {
                if (ds[0]) em_wf(em, "    %s", ds);
                goto_expr(em, a, i.target, tmp, sizeof tmp);
                em_wf(em, "    %s", tmp);
            } else {
                char c[96];
                cond_expr(&i, c, sizeof c);
                em_wf(em, "    { int c = %s;", c);
                if (ds[0]) em_wf(em, "      %s", ds);
                goto_expr(em, a, i.target, tmp, sizeof tmp);
                em_wf(em, "      if (c) %s }", tmp);
                after = a + 8; have_after = true;
            }
        }
        /* delay slot consumed; skip it unless something jumps into it */
        bool has_nxt;
        u32 nxt;
        if (!set_has(&f->labels, a + 4)) {
            set_add(&skip, a + 4);
            has_nxt = false; nxt = 0;
            for (size_t k = idx + 1; k < addrs.n; k++)
                if (!set_has(&skip, addrs.v[k])) { has_nxt = true; nxt = addrs.v[k]; break; }
        } else { has_nxt = true; nxt = a + 4; }
        if (have_after) {
            bool same = has_nxt && nxt == after;
            if (!same) {
                if (set_has(&f->insns, after)) {
                    em_wf(em, "    goto L_%08x;", after);
                    set_add(&f->labels, after);
                } else em_wf(em, "    rt_unreached(0x%08x); FN_RET();", after);
            }
            have_prev_end = same;
        } else have_prev_end = false;
    }
    flush_cycles(em);
    em_w(em, have_prev_end ? "    rt_unreached(0); FN_RET();" : "");
    em_w(em, "}");
    vec_free(&addrs);
    set_free(&skip);
}

/* ---- minimal JSON reader (rea_seeds) ------------------------------------------------------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    JType t;
    char *str;           /* J_STR (decoded) */
    JVal **items;        /* J_ARR values, J_OBJ values */
    char **keys;         /* J_OBJ keys */
    size_t n;
};
typedef struct { const char *p, *end; bool err; } JP;

static void jskip(JP *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static JVal *jnew(JType t) { JVal *v = calloc(1, sizeof *v); if (!v) abort(); v->t = t; return v; }
static void jfree(JVal *v)
{
    if (!v) return;
    for (size_t k = 0; k < v->n; k++) { jfree(v->items[k]); if (v->keys) free(v->keys[k]); }
    free(v->items); free(v->keys); free(v->str); free(v);
}
static void jput_utf8(Buf *b, u32 c)
{
    char t[4];
    if (c < 0x80) { t[0] = (char)c; buf_putn(b, t, 1); }
    else if (c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 63)); buf_putn(b, t, 2); }
    else if (c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | ((c >> 6) & 63)); t[2] = (char)(0x80 | (c & 63)); buf_putn(b, t, 3); }
    else { t[0] = (char)(0xf0 | c >> 18); t[1] = (char)(0x80 | ((c >> 12) & 63)); t[2] = (char)(0x80 | ((c >> 6) & 63)); t[3] = (char)(0x80 | (c & 63)); buf_putn(b, t, 4); }
}
static char *jstring(JP *j)
{
    Buf b = {0};
    buf_puts(&b, "");
    j->p++;   /* opening quote */
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { buf_putn(&b, &c, 1); continue; }
        if (j->p >= j->end) break;
        c = *j->p++;
        switch (c) {
        case 'n': buf_puts(&b, "\n"); break;
        case 't': buf_puts(&b, "\t"); break;
        case 'r': buf_puts(&b, "\r"); break;
        case 'b': buf_puts(&b, "\b"); break;
        case 'f': buf_puts(&b, "\f"); break;
        case 'u': {
            if (j->end - j->p < 4) { j->err = true; break; }
            char h[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
            j->p += 4;
            jput_utf8(&b, (u32)strtoul(h, NULL, 16));
            break;
        }
        default: buf_putn(&b, &c, 1); break;
        }
    }
    if (j->p >= j->end) j->err = true; else j->p++;
    return b.s;
}
static JVal *jvalue(JP *j, int depth)
{
    jskip(j);
    if (j->p >= j->end || depth > 200) { j->err = true; return NULL; }
    char c = *j->p;
    if (c == '{' || c == '[') {
        JVal *v = jnew(c == '{' ? J_OBJ : J_ARR);
        j->p++;
        jskip(j);
        if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) { j->p++; return v; }
        for (;;) {
            char *key = NULL;
            if (c == '{') {
                jskip(j);
                if (j->p >= j->end || *j->p != '"') { j->err = true; return v; }
                key = jstring(j);
                jskip(j);
                if (j->p >= j->end || *j->p != ':') { free(key); j->err = true; return v; }
                j->p++;
            }
            JVal *item = jvalue(j, depth + 1);
            v->items = xrealloc(v->items, (v->n + 1) * sizeof *v->items);
            if (c == '{') v->keys = xrealloc(v->keys, (v->n + 1) * sizeof *v->keys), v->keys[v->n] = key;
            v->items[v->n++] = item;
            if (j->err) return v;
            jskip(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) { j->p++; return v; }
            j->err = true;
            return v;
        }
    }
    if (c == '"') { JVal *v = jnew(J_STR); v->str = jstring(j); return v; }
    if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "null", 4)) { j->p += 4; return jnew(c == 't' ? J_BOOL : J_NULL); }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return jnew(J_BOOL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        while (j->p < j->end && strchr("+-0123456789.eE", *j->p)) j->p++;
        return jnew(J_NUM);
    }
    j->err = true;
    return NULL;
}
/* Python dict.get: the LAST value of a duplicated key wins in json.load */
static JVal *jget(const JVal *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    JVal *r = NULL;
    for (size_t k = 0; k < o->n; k++) if (!strcmp(o->keys[k], key)) r = o->items[k];
    return r;
}

/* recomp.py rea_seeds(): every "address" "0x..." of the procedure list, -1 = file missing */
static long rea_seeds(const RecompInput *in, const char *rel, Vec *out)
{
    size_t len;
    char *text = in->analysis_file(in->ud, rel, &len);
    if (!text) return 0;
    JP j = {text, text + len, false};
    JVal *d = jvalue(&j, 0);
    if (j.err || !d) { fail("analysis/%s: not valid JSON", rel); jfree(d); free(text); return 0; }
    JVal *items = jget(d, "normalized_result");
    if (!items) items = jget(d, "result");
    if (!items) items = d;
    if (items->t == J_OBJ) {
        static const char *const keys[] = {"procedures", "items", "matches", "results"};
        for (int k = 0; k < 4; k++) { JVal *x = jget(items, keys[k]); if (x) { items = x; break; } }
    }
    long count = 0;
    if (items->t == J_ARR || items->t == J_OBJ) {
        /* iterating a dict in Python yields its keys (strings): p.get fails there; the inputs
         * never have that shape */
        for (size_t k = 0; k < items->n; k++) {
            JVal *p = items->items[k];
            JVal *a = p && p->t == J_OBJ ? jget(p, "address") : NULL;
            if (a && a->t == J_STR && !strncmp(a->str, "0x", 2)) {
                vec_push(out, (u32)strtoull(a->str + 2, NULL, 16));
                count++;
            }
        }
    }
    jfree(d);
    free(text);
    return count;
}

/* `line.split('#')[0].split()` */
static int split_line(char *line, char **tok, int max)
{
    char *h = strchr(line, '#');
    if (h) *h = 0;
    int n = 0;
    for (char *p = line; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n < max) tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}
/* int(x, 16) / int(x, 0) */
static bool parse_int(const char *s, int base, s64 *out)
{
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (base == 0) base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!*s) return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, base);
    if (*end || errno) return false;
    *out = neg ? -(s64)v : (s64)v;
    return true;
}

typedef void (*LineFn)(void *ctx, int lineno, char **tok, int n);
static void each_line(const RecompInput *in, const char *rel, LineFn fn, void *ctx)
{
    size_t len;
    char *text = in->analysis_file(in->ud, rel, &len);
    if (!text) return;
    int lineno = 0;
    for (char *p = text; *p && !s_failed;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        lineno++;
        char *tok[8];
        int n = split_line(p, tok, 8);
        if (n) fn(ctx, lineno, tok, n);
        if (!nl) break;
        p = nl + 1;
    }
    free(text);
}

/* extra_seeds.txt: "<hex> [overlay]" */
typedef struct { char name[32]; Vec v; } NamedSeeds;
typedef struct { NamedSeeds *s; size_t n; } ExtraSeeds;
static Vec *extra_get(ExtraSeeds *x, const char *name, bool create)
{
    for (size_t k = 0; k < x->n; k++) if (!strcmp(x->s[k].name, name)) return &x->s[k].v;
    if (!create) return NULL;
    x->s = xrealloc(x->s, (x->n + 1) * sizeof *x->s);
    memset(&x->s[x->n], 0, sizeof x->s[x->n]);
    snprintf(x->s[x->n].name, sizeof x->s[x->n].name, "%s", name);
    return &x->s[x->n++].v;
}
static void extra_line(void *ctx, int lineno, char **tok, int n)
{
    s64 v;
    if (!parse_int(tok[0], 16, &v)) { fail("analysis/extra_seeds.txt:%d: bad address %s", lineno, tok[0]); return; }
    vec_push(extra_get(ctx, n > 1 ? tok[1] : "main", true), (u32)v);
}
static void overrides_line(void *ctx, int lineno, char **tok, int n)
{
    (void)n;
    s64 v;
    if (!parse_int(tok[0], 16, &v)) { fail("analysis/overrides.txt:%d: bad address %s", lineno, tok[0]); return; }
    set_add(ctx, (u32)v);
}
static u32 fnv1a(const u8 *p, size_t n)
{
    u32 h = 0x811c9dc5u;
    for (size_t k = 0; k < n; k++) h = (h ^ p[k]) * 0x01000193u;
    return h;
}
static const Image *s_main_img;
static void idle_line(void *ctx, int lineno, char **tok, int n)
{
    (void)ctx;
    s64 br, start, want;
    if (n < 4 || !parse_int(tok[0], 16, &br) || !parse_int(tok[2], 16, &start) || !parse_int(tok[3], 16, &want)) {
        fail("analysis/idle_loops.txt:%d: malformed line", lineno);
        return;
    }
    Ins i = img_ins(s_main_img, (u32)br);
    if (!(i.op == OP_BEQ || i.op == OP_BNE || i.op == OP_BLEZ || i.op == OP_BGTZ || i.op == OP_BLTZ || i.op == OP_BGEZ) ||
        i.target != (u32)start) {
        fail("analysis/idle_loops.txt:%d: %08x is not a branch back to %08x", lineno, (u32)br, (u32)start);
        return;
    }
    Buf b = {0};
    for (u32 a = (u32)start; a < (u32)br + 8; a += 4) {
        u32 w = img_word(s_main_img, a);
        u8 le[4] = {(u8)w, (u8)(w >> 8), (u8)(w >> 16), (u8)(w >> 24)};
        buf_putn(&b, (const char *)le, 4);
    }
    u32 h = fnv1a((const u8 *)b.s, b.n);
    free(b.s);
    if (h != (u32)want) {
        fail("analysis/idle_loops.txt:%d: loop %08x..%08x hash %08x, expected %08x", lineno, (u32)start, (u32)br, h, (u32)want);
        return;
    }
    for (size_t k = 0; k < G.nidle; k++)   /* dict: a later line replaces an earlier one */
        if (G.idle[k].br == (u32)br) { snprintf(G.idle[k].helper, sizeof G.idle[k].helper, "%s", tok[1]); return; }
    G.idle = xrealloc(G.idle, (G.nidle + 1) * sizeof *G.idle);
    G.idle[G.nidle].br = (u32)br;
    snprintf(G.idle[G.nidle].helper, sizeof G.idle[G.nidle].helper, "%s", tok[1]);
    G.nidle++;
}
static void ws_line(void *ctx, int lineno, char **tok, int n)
{
    (void)ctx;
    s64 a, orig, wide;
    if (n < 3 || !parse_int(tok[0], 16, &a) || !parse_int(tok[1], 0, &orig) || !parse_int(tok[2], 0, &wide)) {
        fail("analysis/widescreen_cull.txt:%d: malformed line", lineno);
        return;
    }
    Ins i = img_ins(s_main_img, (u32)a);
    if (i.op != OP_SLTI || i.imm != orig) {
        fail("analysis/widescreen_cull.txt:%d: %08x is %s imm %d, expected slti %lld", lineno, (u32)a, OPNAME[i.op], i.imm,
             (long long)orig);
        return;
    }
    for (size_t k = 0; k < G.nws; k++) if (G.ws[k].addr == (u32)a) { G.ws[k].wide = (u32)wide; return; }
    G.ws = xrealloc(G.ws, (G.nws + 1) * sizeof *G.ws);
    G.ws[G.nws].addr = (u32)a;
    G.ws[G.nws].wide = (u32)wide;
    G.nws++;
}

/* ---- recomp.py ----------------------------------------------------------------------------- */

#define MAIN_LO 0x80026000u
#define MAIN_HI 0x800aa000u
#define SLOT_LO 0x80010000u
#define SLOT_HI 0x80026000u
#define FUNCS_PER_FILE 80
#define P1 0x80010000u
#define P2 0x8001a268u
static const char *const CHARS[14] = {"MGRAD", "MSEREA", "MANZEAL", "MMILENA", "MDOIL", "MREUS", "MTEEM", "MKUGA",
                                      "MROSEN", "MLAON", "MFALMA", "MZAUBER", "MSOLD", "MGENE"};

typedef struct { char path[48]; u32 load; } OvlDef;
static int overlay_list(OvlDef out[32])
{
    int n = 0;
    for (int c = 0; c < 14; c++) { snprintf(out[n].path, sizeof out[n].path, "OVL/%s.BIN", CHARS[c]); out[n++].load = P1; }
    for (int c = 0; c < 14; c++) { snprintf(out[n].path, sizeof out[n].path, "OVL/%s2.BIN", CHARS[c]); out[n++].load = P2; }
    snprintf(out[n].path, sizeof out[n].path, "OVL/EVENT_SP.BIN"); out[n++].load = P1;
    snprintf(out[n].path, sizeof out[n].path, "OVL/EVENTOVL.BIN"); out[n++].load = P2;
    snprintf(out[n].path, sizeof out[n].path, "OVL/OPTOVL.BIN"); out[n++].load = P1;
    return n;
}

static bool jr_ra(const Ins *p) { return p->op == OP_JR && p->rs == 31; }
static bool sp_prologue(const Ins *i) { return i->op == OP_ADDIU && i->rs == 29 && i->rt == 29 && i->imm < 0; }

static void prologue_seeds(const Image *img, u32 lo, u32 hi, Vec *out)
{
    for (u32 a = lo; a < hi; a += 4) {
        Ins i = img_ins(img, a);
        if (sp_prologue(&i)) {
            bool have_p = a - 8 >= lo;
            Ins p = have_p ? img_ins(img, a - 8) : (Ins){0};
            if (a == lo || (have_p && jr_ra(&p))) vec_push(out, a);
        }
    }
}

static bool plausible(const Image *img, u32 lo, u32 hi, const Set *known, u32 t)
{
    if (!(lo <= t && t < hi) || (t & 3)) return false;
    if (set_has(known, t)) return true;
    Ins p = img_ins(img, t - 8), i = img_ins(img, t);
    if (i.op == OP_INVALID) return false;
    return jr_ra(&p) || sp_prologue(&i);
}

static void pointer_seeds(const Image *img, u32 lo, u32 hi, const Set *known, Set *out)
{
    for (u32 a = img->base; (s64)a < (s64)img->end - 3; a += 4) {
        u32 w = img_word(img, a);
        if (plausible(img, lo, hi, known, w)) set_add(out, w);
    }
    for (u32 a = lo; a < hi - 4; a += 4) {
        Ins i = img_ins(img, a);
        if (i.op != OP_LUI) continue;
        u32 lim = a + 40 < hi ? a + 40 : hi;
        for (u32 b = a + 4; b < lim; b += 4) {
            Ins j = img_ins(img, b);
            if (j.op == OP_ADDIU && j.rs == i.rt) {
                u32 t = ((u32)i.imm << 16) + (u32)j.imm;
                if (plausible(img, lo, hi, known, t)) set_add(out, t);
                break;
            }
        }
    }
}

/* name maps */
typedef struct { u32 *addr; char (*name)[48]; size_t n; } Names;   /* sorted by addr */
static const char *names_find(const Names *m, u32 a)
{
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->addr[mid] < a) lo = mid + 1; else hi = mid;
    }
    return lo < m->n && m->addr[lo] == a ? m->name[lo] : NULL;
}
static Names names_build(Func **funcs, size_t n, const char *tag)
{
    Names m = {xmalloc(n * sizeof *m.addr), xmalloc(n * sizeof *m.name), n};
    for (size_t k = 0; k < n; k++) {   /* funcs are sorted by entry */
        m.addr[k] = funcs[k]->entry;
        snprintf(m.name[k], sizeof m.name[k], "f_%08x%s", funcs[k]->entry, tag);
    }
    return m;
}
static void names_free(Names *m) { free(m->addr); free(m->name); }

typedef struct { const Names *own, *main; } NameCtx;
static const char *name_of_cb(Emitter *em, u32 a)
{
    const NameCtx *c = em->name_ud;
    const char *s = c->own ? names_find(c->own, a) : NULL;
    if (!s) s = names_find(c->main, a);
    if (!s) { fail("no name for %08x", a); return "f_unknown"; }
    return s;
}

typedef struct { u32 addr; char name[48]; u32 ov, lo, hi, hash; } TableRow;
static int cmp_row(const void *a, const void *b)
{
    const TableRow *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return x->ov < y->ov ? -1 : x->ov > y->ov;
}

typedef struct {
    const RecompInput *in;
    Buf report;           /* report lines, '\n' after each */
    char (*files)[64]; size_t nfiles;
    Buf protos;           /* each proto + '\n' except the last, joined later */
    size_t nprotos;
} Out;

static void report(Out *o, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_puts(&o->report, tmp);
    buf_puts(&o->report, "\n");
    if (o->in->log && tmp[0] != ' ') o->in->log(o->in->ud, tmp);
}
static void proto(Out *o, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (o->nprotos++) buf_puts(&o->protos, "\n");
    buf_puts(&o->protos, tmp);
}

static bool write_text(const char *dir, const char *name, const char *data, size_t n)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fail("cannot write %s: %s", path, strerror(errno)); return false; }
    bool ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (!ok) fail("cannot write %s", path);
    return ok;
}

static void write_c_file(Out *o, const char *name, Buf *chunks, size_t n)
{
    Buf b = {0};
    buf_puts(&b, "/* Generated by recomp/recomp.py - do not edit. */\n");
    buf_puts(&b, "#include \"recomp.h\"\n#include \"gen_protos.h\"\n\n");
    for (size_t k = 0; k < n; k++) { buf_putn(&b, chunks[k].s, chunks[k].n); buf_puts(&b, "\n\n"); }
    write_text(o->in->out_dir, name, b.s ? b.s : "", b.n);
    free(b.s);
    o->files = xrealloc(o->files, (o->nfiles + 1) * sizeof *o->files);
    snprintf(o->files[o->nfiles++], sizeof o->files[0], "%s", name);
}

static Buf *emit_funcs(Func **funcs, size_t n, const Image *img, const Set *callable, const NameCtx *nc, const char *tag)
{
    Buf *chunks = calloc(n ? n : 1, sizeof *chunks);
    if (!chunks) abort();
    for (size_t k = 0; k < n && !s_failed; k++) {
        Emitter em = {funcs[k], img, callable, name_of_cb, (void *)nc, tag, {0}, true, 0};
        emit_func(&em);
        chunks[k] = em.out;
    }
    return chunks;
}

static void func_range(const Func *f, u32 *lo, u32 *hi)
{
    u32 mn = 0xffffffffu, mx = 0;
    for (size_t k = 0; k < f->insns.cap; k++)
        if (f->insns.used[k]) { if (f->insns.k[k] < mn) mn = f->insns.k[k]; if (f->insns.k[k] > mx) mx = f->insns.k[k]; }
    *lo = mn;
    *hi = mx + 4;
}

static void free_globals(void)
{
    free(G.ws); free(G.idle);
    set_free(&G.ws_emitted); set_free(&G.idle_emitted);
    memset(&G, 0, sizeof G);
}

const char *recomp_run(const RecompInput *in, int *files_out)
{
    s_failed = false;
    s_err[0] = 0;
    free_globals();
    Out o = {in, {0}, NULL, 0, {0}, 0};
    ExtraSeeds xs = {0};
    each_line(in, "extra_seeds.txt", extra_line, &xs);

    u32 exe_size = 0;
    u8 *exe = in->disc_file(in->ud, "SLUS_007.32", &exe_size);
    if (!exe || exe_size < 0x800 || memcmp(exe, "PS-X EXE", 8)) { free(exe); return "SLUS_007.32 is missing or not a PS-X EXE"; }
    u32 pc0 = exe[0x10] | exe[0x11] << 8 | exe[0x12] << 16 | (u32)exe[0x13] << 24;
    u32 taddr = exe[0x18] | exe[0x19] << 8 | exe[0x1a] << 16 | (u32)exe[0x1b] << 24;
    u32 tsize = exe[0x1c] | exe[0x1d] << 8 | exe[0x1e] << 16 | (u32)exe[0x1f] << 24;
    if (tsize > exe_size - 0x800) tsize = exe_size - 0x800;   /* d[0x800:0x800 + tsize] */
    Image main_img = {"main", taddr, taddr + tsize, exe + 0x800, tsize, pc0};
    s_main_img = &main_img;

    Vec seeds = {0};
    rea_seeds(in, "rea_list_procedures_main.json", &seeds);
    vec_push(&seeds, main_img.entry);
    Vec *xm = extra_get(&xs, "main", false);
    if (xm) for (size_t k = 0; k < xm->n; k++) vec_push(&seeds, xm->v[k]);
    Set seed_set = {0};
    for (size_t k = 0; k < seeds.n; k++) set_add(&seed_set, seeds.v[k]);
    Set ps = {0};
    pointer_seeds(&main_img, MAIN_LO, MAIN_HI, &seed_set, &ps);

    OvlDef ovl[32];
    int novl = overlay_list(ovl);
    u8 *ovl_data[32] = {0};
    u32 ovl_size[32] = {0};
    for (int k = 0; k < novl; k++) {
        ovl_data[k] = in->disc_file(in->ud, ovl[k].path, &ovl_size[k]);
        if (!ovl_data[k]) { fail("disc file %s is missing", ovl[k].path); break; }
    }

    Set ovl_refs = {0};
    for (u32 a = main_img.base; !s_failed && (s64)a < (s64)main_img.end - 3; a += 4) {
        u32 t = img_word(&main_img, a);
        if (SLOT_LO <= t && t < SLOT_HI && (t & 3) == 0) set_add(&ovl_refs, t);
    }
    for (int k = 0; k < novl && !s_failed; k++) {
        Image oimg = {"o", ovl[k].load, ovl[k].load + ovl_size[k], ovl_data[k], ovl_size[k], 0};
        for (u32 a = oimg.base; (s64)a < (s64)oimg.end - 3; a += 4) {
            Ins i = img_ins(&oimg, a);
            u32 t = i.op == OP_JAL ? i.target : img_word(&oimg, a);
            if (SLOT_LO <= t && t < SLOT_HI && (t & 3) == 0) set_add(&ovl_refs, t);
            if (MAIN_LO <= t && t < MAIN_HI && (t & 3) == 0) {
                Ins p = img_ins(&main_img, t - 8), j = img_ins(&main_img, t);
                if (set_has(&seed_set, t) || jr_ra(&p) || sp_prologue(&j)) set_add(&ps, t);
            }
        }
    }
    size_t extra = 0;
    for (size_t k = 0; k < ps.cap; k++) if (ps.used[k] && !set_has(&seed_set, ps.k[k])) extra++;
    report(&o, "main: %zu extra seeds from code pointers", extra);

    Analyzer an = {&main_img, MAIN_LO, MAIN_HI, {0}, NULL, NULL, 0};
    Set no_ext = {0};
    an.external = &no_ext;
    for (size_t k = 0; k < seeds.n; k++) if (MAIN_LO <= seeds.v[k] && seeds.v[k] < MAIN_HI) set_add(&an.seeds, seeds.v[k]);
    for (size_t k = 0; k < ps.cap; k++) if (ps.used[k] && MAIN_LO <= ps.k[k] && ps.k[k] < MAIN_HI) set_add(&an.seeds, ps.k[k]);
    if (!s_failed) an_run(&an);
    qsort(an.funcs, an.nfuncs, sizeof *an.funcs, cmp_func);
    Func **main_funcs = an.funcs;
    size_t nmain = an.nfuncs;
    each_line(in, "widescreen_cull.txt", ws_line, NULL);
    each_line(in, "idle_loops.txt", idle_line, NULL);
    Names main_names = names_build(main_funcs, nmain, "");
    size_t ninsns = 0;
    Set main_set = {0};
    for (size_t k = 0; k < nmain; k++) { ninsns += main_funcs[k]->insns.n; set_add(&main_set, main_funcs[k]->entry); }
    report(&o, "main: %zu functions, %zu instructions", nmain, ninsns);

    Set main_calls = {0};
    for (size_t k = 0; k < nmain; k++) {
        const Func *f = main_funcs[k];
        for (size_t q = 0; q < f->calls.cap; q++) if (f->calls.used[q]) set_add(&main_calls, f->calls.k[q]);
        for (size_t q = 0; q < f->tailcalls.cap; q++) if (f->tailcalls.used[q]) set_add(&main_calls, f->tailcalls.k[q]);
    }

    TableRow *table = NULL;
    size_t ntable = 0;

    /* ---- main executable ---- */
    NameCtx mnc = {NULL, &main_names};
    Buf *chunks = s_failed ? NULL : emit_funcs(main_funcs, nmain, &main_img, &main_set, &mnc, "");
    Set ovr = {0};
    each_line(in, "overrides.txt", overrides_line, &ovr);
    {
        Vec ov = set_sorted(&ovr);
        for (size_t k = 0; k < ov.n && !s_failed; k++) {
            u32 a = ov.v[k];
            const char *nm = names_find(&main_names, a);
            if (!nm) { fail("analysis/overrides.txt: %08x is not a recompiled main function", a); break; }
            char head[96];
            snprintf(head, sizeof head, "void %s(void) {", nm);
            size_t hl = strlen(head), hit = 0, at = 0;
            for (size_t c = 0; c < nmain; c++)
                if (chunks[c].n >= hl && !memcmp(chunks[c].s, head, hl)) { hit++; at = c; }
            if (hit != 1) { fail("override %08x: cannot locate generated body", a); break; }
            Buf nb = {0};
            buf_printf(&nb, "void %s_orig(void) {", nm);
            buf_putn(&nb, chunks[at].s + hl, chunks[at].n - hl);
            free(chunks[at].s);
            chunks[at] = nb;
            proto(&o, "void %s_orig(void);", nm);
        }
        vec_free(&ov);
    }
    if (!s_failed) {
        Buf miss = {0};
        Vec ws = {0};
        for (size_t k = 0; k < G.nws; k++) vec_push(&ws, G.ws[k].addr);
        if (ws.n) qsort(ws.v, ws.n, sizeof *ws.v, cmp_u32);
        for (size_t k = 0; k < ws.n; k++) if (!set_has(&G.ws_emitted, ws.v[k])) buf_printf(&miss, " %08x", ws.v[k]);
        vec_free(&ws);
        if (miss.n) fail("analysis/widescreen_cull.txt: not in recompiled code:%s", miss.s);
        free(miss.s);
    }
    if (!s_failed) report(&o, "main: %zu widescreen culling patch points", G.ws_emitted.n);
    if (!s_failed) {
        Buf miss = {0};
        Vec il = {0};
        for (size_t k = 0; k < G.nidle; k++) vec_push(&il, G.idle[k].br);
        if (il.n) qsort(il.v, il.n, sizeof *il.v, cmp_u32);
        for (size_t k = 0; k < il.n; k++) if (!set_has(&G.idle_emitted, il.v[k])) buf_printf(&miss, " %08x", il.v[k]);
        vec_free(&il);
        if (miss.n) fail("analysis/idle_loops.txt: not in recompiled code:%s", miss.s);
        free(miss.s);
    }
    if (!s_failed) report(&o, "main: %zu idle-loop fast-forward points", G.idle_emitted.n);
    for (size_t n0 = 0; n0 < nmain && !s_failed; n0 += FUNCS_PER_FILE) {
        char nm[64];
        snprintf(nm, sizeof nm, "main_%02zu.c", n0 / FUNCS_PER_FILE);
        write_c_file(&o, nm, chunks + n0, nmain - n0 < FUNCS_PER_FILE ? nmain - n0 : FUNCS_PER_FILE);
    }
    for (size_t k = 0; k < nmain && !s_failed; k++) {
        const Func *f = main_funcs[k];
        TableRow r = {f->entry, "", 0, 0, 0, 0};
        snprintf(r.name, sizeof r.name, "%s", main_names.name[k]);
        func_range(f, &r.lo, &r.hi);
        table = xrealloc(table, (ntable + 1) * sizeof *table);
        table[ntable++] = r;
        proto(&o, "void %s(void);", main_names.name[k]);
        for (size_t b = 0; b < f->nbad; b++) report(&o, "  main %08x: %08x %s", f->entry, f->bad[b].addr, f->bad[b].msg);
    }
    if (chunks) for (size_t k = 0; k < nmain; k++) free(chunks[k].s);
    free(chunks);

    /* ---- overlays ---- */
    typedef struct { const char *path; u32 load, size; } OvlInfo;
    OvlInfo infos[32];
    int ninfos = 0;
    for (int k = 0; k < novl && !s_failed; k++) {
        u32 load = ovl[k].load, size = ovl_size[k];
        char base[32];
        const char *bn = strrchr(ovl[k].path, '/');
        snprintf(base, sizeof base, "%s", bn ? bn + 1 : ovl[k].path);
        char *dot = strstr(base, ".BIN");
        if (dot) memmove(dot, dot + 4, strlen(dot + 4) + 1);
        Image img = {"", load, load + size, ovl_data[k], size, 0};
        snprintf(img.name, sizeof img.name, "%s", base);
        u32 lo = load + 4, hi = load + size;
        char rel[96];
        snprintf(rel, sizeof rel, "overlays/%s.procs.json", base);
        Vec s = {0};
        long rea_n = rea_seeds(in, rel, &s);
        /* looks_like_entry(t) for main-call and overlay-reference seeds */
        Set *srcs[2] = {&main_calls, &ovl_refs};
        for (int q = 0; q < 2; q++) {
            Vec srt = set_sorted(srcs[q]);
            for (size_t c = 0; c < srt.n; c++) {
                u32 t = srt.v[c];
                if (!(lo <= t && t < hi)) continue;
                Ins i = img_ins(&img, t);
                bool have_p = t - 8 >= lo;
                Ins p = have_p ? img_ins(&img, t - 8) : (Ins){0};
                if (t == lo || (have_p && jr_ra(&p)) || sp_prologue(&i)) vec_push(&s, t);
            }
            vec_free(&srt);
        }
        prologue_seeds(&img, lo, hi, &s);
        Vec *xo = extra_get(&xs, base, false);
        if (xo) for (size_t c = 0; c < xo->n; c++) vec_push(&s, xo->v[c]);
        Analyzer oan = {&img, lo, hi, {0}, &main_set, NULL, 0};
        for (size_t c = 0; c < s.n; c++) {
            u32 x = s.v[c];
            if (!(lo <= x && x < hi && (x & 3) == 0)) continue;
            if (img_ins(&img, x).op == OP_INVALID) continue;
            set_add(&oan.seeds, x);
        }
        vec_free(&s);
        if (!s_failed) an_run(&oan);
        qsort(oan.funcs, oan.nfuncs, sizeof *oan.funcs, cmp_func);
        char tag[40];
        snprintf(tag, sizeof tag, "_%s", base);
        Names onames = names_build(oan.funcs, oan.nfuncs, tag);
        Set callable = {0};
        for (size_t c = 0; c < oan.nfuncs; c++) set_add(&callable, oan.funcs[c]->entry);
        for (size_t c = 0; c < nmain; c++) set_add(&callable, main_funcs[c]->entry);
        NameCtx onc = {&onames, &main_names};
        Buf *oc = s_failed ? NULL : emit_funcs(oan.funcs, oan.nfuncs, &img, &callable, &onc, tag);
        char fn[64];
        snprintf(fn, sizeof fn, "ovl_%s.c", base);
        if (!s_failed) write_c_file(&o, fn, oc, oan.nfuncs);
        for (size_t c = 0; c < oan.nfuncs && !s_failed; c++) {
            const Func *f = oan.funcs[c];
            TableRow r = {f->entry, "", (u32)k + 1, 0, 0, 0};
            snprintf(r.name, sizeof r.name, "%s", onames.name[c]);
            func_range(f, &r.lo, &r.hi);
            r.hash = fnv1a(ovl_data[k] + (r.lo - load), r.hi - r.lo);
            table = xrealloc(table, (ntable + 1) * sizeof *table);
            table[ntable++] = r;
            proto(&o, "void %s(void);", onames.name[c]);
            for (size_t b = 0; b < f->nbad; b++) report(&o, "  %s %08x: %08x %s", base, f->entry, f->bad[b].addr, f->bad[b].msg);
        }
        infos[ninfos].path = ovl[k].path; infos[ninfos].load = load; infos[ninfos].size = size; ninfos++;
        if (!s_failed) report(&o, "%s: %zu functions (REA seeds %ld)", base, oan.nfuncs, rea_n);
        if (oc) for (size_t c = 0; c < oan.nfuncs; c++) free(oc[c].s);
        free(oc);
        for (size_t c = 0; c < oan.nfuncs; c++) func_free(oan.funcs[c]);
        free(oan.funcs);
        set_free(&oan.seeds);
        set_free(&callable);
        names_free(&onames);
    }

    if (!s_failed) {
        Buf b = {0};
        buf_puts(&b, "/* Generated - do not edit. */\n#pragma once\n");
        if (o.protos.s) buf_putn(&b, o.protos.s, o.protos.n);
        buf_puts(&b, "\n");
        write_text(in->out_dir, "gen_protos.h", b.s, b.n);
        free(b.s);
    }
    if (!s_failed) {
        qsort(table, ntable, sizeof *table, cmp_row);
        Buf b = {0};
        buf_puts(&b, "/* Generated - do not edit. */\n#include \"psx.h\"\n#include \"gen_protos.h\"\n\n");
        buf_puts(&b, "const RecompEntry g_recomp_table[] = {\n");
        for (size_t k = 0; k < ntable; k++)
            buf_printf(&b, "    {0x%08x, %s, %u, 0x%08x, 0x%08x, 0x%08x},\n", table[k].addr, table[k].name, table[k].ov,
                       table[k].lo, table[k].hi, table[k].hash);
        buf_puts(&b, "};\n");
        buf_printf(&b, "const u32 g_recomp_count = %zu;\n\n", ntable);
        buf_puts(&b, "const OverlayInfo g_overlays[] = {\n");
        for (int k = 0; k < ninfos; k++) buf_printf(&b, "    {\"%s\", 0x%08x, %u},\n", infos[k].path, infos[k].load, infos[k].size);
        buf_puts(&b, "};\n");
        buf_printf(&b, "const u32 g_overlay_count = %d;\n", ninfos);
        write_text(in->out_dir, "table.c", b.s, b.n);
        free(b.s);
        o.files = xrealloc(o.files, (o.nfiles + 1) * sizeof *o.files);
        snprintf(o.files[o.nfiles++], sizeof o.files[0], "table.c");
    }
    if (!s_failed) {
        Buf b = {0};
        buf_puts(&b, "set(GEN_SOURCES\n");
        for (size_t k = 0; k < o.nfiles; k++) buf_printf(&b, "%s  ${GEN_DIR}/%s", k ? "\n" : "", o.files[k]);
        buf_puts(&b, "\n)\n");
        write_text(in->out_dir, "files.cmake", b.s, b.n);
        free(b.s);
        write_text(in->out_dir, "report.txt", o.report.s ? o.report.s : "\n", o.report.s ? o.report.n : 1);
        if (in->log) {
            char tmp[128];
            snprintf(tmp, sizeof tmp, "total table entries: %zu; files: %zu", ntable, o.nfiles);
            in->log(in->ud, tmp);
        }
    }
    if (files_out) *files_out = (int)o.nfiles;

    /* cleanup */
    for (size_t k = 0; k < nmain; k++) func_free(main_funcs[k]);
    free(main_funcs);
    set_free(&an.seeds); set_free(&no_ext);
    names_free(&main_names);
    set_free(&main_set); set_free(&main_calls); set_free(&ovr); set_free(&ovl_refs); set_free(&ps); set_free(&seed_set);
    vec_free(&seeds);
    for (size_t k = 0; k < xs.n; k++) vec_free(&xs.s[k].v);
    free(xs.s);
    for (int k = 0; k < novl; k++) free(ovl_data[k]);
    free(exe);
    free(table);
    free(o.files); free(o.protos.s); free(o.report.s);
    free_globals();
    s_main_img = NULL;
    return s_failed ? s_err : NULL;
}
