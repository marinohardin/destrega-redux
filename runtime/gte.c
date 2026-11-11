/* PS1 Geometry Transformation Engine (COP2), register-level emulation.
 *
 * Semantics follow psx-spx (nocash) "Geometry Transformation Engine (GTE)":
 * 44-bit MAC1..3 accumulation with per-step overflow flags, 32-bit MAC0
 * overflow flags, IR/color/SZ/SX/SY/IR0 saturation, UNR division, and the
 * documented hardware quirks (H read sign extension, RTP IR3 flag quirk,
 * MVMVA far-color translation bug, mx=3 garbage matrix).
 *
 * Storage: data regs d[] and control regs c[] hold raw 32-bit register
 * values, normalised on write (16-bit signed regs stored sign-extended,
 * unsigned 16-bit regs stored zero-extended) so reads are mostly direct.
 */
#include <math.h>
#include "psx.h"
#include <string.h>
#include "pgxp.h"

static u32 d[32]; /* cop2 data registers */
static u32 c[32]; /* cop2 control registers (c[31] unused; see g_flag) */
static u32 g_flag; /* FLAG bits 30..12 (bit 31 computed on read) */

#define FLAG_ERROR_MASK 0x7F87E000u

/* Data register indices */
enum {
    D_VXY0 = 0, D_VZ0 = 1, D_RGBC = 6, D_OTZ = 7, D_IR0 = 8, D_IR1 = 9,
    D_SXY0 = 12, D_SXY1 = 13, D_SXY2 = 14, D_SXYP = 15,
    D_SZ0 = 16, D_SZ1 = 17, D_SZ2 = 18, D_SZ3 = 19,
    D_RGB0 = 20, D_RGB1 = 21, D_RGB2 = 22, D_RES1 = 23,
    D_MAC0 = 24, D_MAC1 = 25, D_IRGB = 28, D_ORGB = 29, D_LZCS = 30, D_LZCR = 31,
};
/* Control register indices */
enum {
    C_RT = 0, C_TR = 5, C_LLM = 8, C_BK = 13, C_LCM = 16, C_FC = 21,
    C_OFX = 24, C_OFY = 25, C_H = 26, C_DQA = 27, C_DQB = 28,
    C_ZSF3 = 29, C_ZSF4 = 30, C_FLAG = 31,
};

static const u8 unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B,
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F,
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00,
};

/* ---- small helpers ------------------------------------------------------ */

static inline s32 sx16(u32 v) { return (s32)(s16)(u16)v; }

static inline s32 ir(int i) { return (s32)d[D_IR0 + i]; } /* stored sign-extended */
static inline void set_ir(int i, s32 v) { d[D_IR0 + i] = (u32)v; }
static inline s32 mac(int i) { return (s32)d[D_MAC0 + i]; }
static inline void set_mac(int i, s32 v) { d[D_MAC0 + i] = (u32)v; }

/* Element (row r, col k) of a packed 3x3 matrix starting at control reg base. */
static inline s32 mat_el(int base, int r, int k)
{
    int idx = r * 3 + k;
    u32 w = c[base + (idx >> 1)];
    return (idx & 1) ? (s32)(s16)(u16)(w >> 16) : (s32)(s16)(u16)w;
}

static inline s64 sar64(s64 v, int s) { return v >> s; } /* arithmetic on clang/gcc */

/* 44-bit MAC1..3 overflow check (i = 1..3) and sign-extension to 44 bits. */
static s64 mac_chk(int i, s64 v)
{
    if (v > 0x7FFFFFFFFFFLL)
        g_flag |= 1u << (31 - i);          /* 30,29,28 */
    else if (v < -0x80000000000LL)
        g_flag |= 1u << (28 - i);          /* 27,26,25 */
    return (s64)((u64)v << 20) >> 20;
}

/* MAC0 32-bit overflow check. */
static s64 mac0_chk(s64 v)
{
    if (v > 0x7FFFFFFFLL)
        g_flag |= 1u << 16;
    else if (v < -0x80000000LL)
        g_flag |= 1u << 15;
    return v;
}

/* IR1..3 saturation (i = 1..3). */
static s32 sat_ir(int i, s32 v, int lm)
{
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { g_flag |= 1u << (25 - i); return lo; }    /* 24,23,22 */
    if (v > 0x7FFF) { g_flag |= 1u << (25 - i); return 0x7FFF; }
    return v;
}

static void set_mac_ir(int i, s64 v, int shift, int lm)
{
    v = mac_chk(i, v);
    s32 m = (s32)sar64(v, shift);
    set_mac(i, m);
    set_ir(i, sat_ir(i, m, lm));
}

static void set_mac_only(int i, s64 v, int shift)
{
    v = mac_chk(i, v);
    set_mac(i, (s32)sar64(v, shift));
}

static void set_ir0(s64 v)
{
    if (v < 0) { g_flag |= 1u << 12; v = 0; }
    else if (v > 0x1000) { g_flag |= 1u << 12; v = 0x1000; }
    set_ir(0, (s32)v);
}

static u32 sat_col(int idx, s32 v)
{
    if (v < 0) { g_flag |= 1u << (21 - idx); return 0; }
    if (v > 0xFF) { g_flag |= 1u << (21 - idx); return 0xFF; }
    return (u32)v;
}

static void push_rgb_from_mac(void)
{
    u32 r = sat_col(0, mac(1) >> 4);
    u32 g = sat_col(1, mac(2) >> 4);
    u32 b = sat_col(2, mac(3) >> 4);
    d[D_RGB0] = d[D_RGB1];
    d[D_RGB1] = d[D_RGB2];
    d[D_RGB2] = r | (g << 8) | (b << 16) | (d[D_RGBC] & 0xFF000000u);
}

static void push_sz(s64 z)
{
    if (z < 0) { g_flag |= 1u << 18; z = 0; }
    else if (z > 0xFFFF) { g_flag |= 1u << 18; z = 0xFFFF; }
    d[D_SZ0] = d[D_SZ1];
    d[D_SZ1] = d[D_SZ2];
    d[D_SZ2] = d[D_SZ3];
    d[D_SZ3] = (u32)z;
}

/* PGXP: the precision tag FIFO moves with SXY0..2; a new entry has no association until the
 * caller attaches one. */
static inline void pgxp_shift(void)
{
    g_pgxp_gte[0] = g_pgxp_gte[1];
    g_pgxp_gte[1] = g_pgxp_gte[2];
    g_pgxp_gte[2] = 0;
}

static void push_sxy(s64 x, s64 y)
{
    if (x < -0x400) { g_flag |= 1u << 14; x = -0x400; }
    else if (x > 0x3FF) { g_flag |= 1u << 14; x = 0x3FF; }
    if (y < -0x400) { g_flag |= 1u << 13; y = -0x400; }
    else if (y > 0x3FF) { g_flag |= 1u << 13; y = 0x3FF; }
    d[D_SXY0] = d[D_SXY1];
    d[D_SXY1] = d[D_SXY2];
    d[D_SXY2] = ((u32)x & 0xFFFF) | ((u32)y << 16);
    pgxp_shift();
}

static u32 lzc(u32 v)
{
    if (v & 0x80000000u) v = ~v;
    return v ? (u32)__builtin_clz(v) : 32u;
}

/* UNR (Newton-Raphson) unsigned division H / SZ3, result 0..0x1FFFF. */
static u32 gte_divide(void)
{
    u32 h = c[C_H] & 0xFFFF;
    u32 sz3 = d[D_SZ3] & 0xFFFF;
    if (h < sz3 * 2) {
        int z = __builtin_clz(sz3) - 16;
        u32 n = h << z;
        u32 dv = sz3 << z;
        u32 u = (u32)unr_table[(dv - 0x7FC0) >> 7] + 0x101;
        dv = (0x2000080u - dv * u) >> 8;
        dv = (0x0000080u + dv * u) >> 8;
        u64 r = ((u64)n * dv + 0x8000) >> 16;
        return r > 0x1FFFF ? 0x1FFFF : (u32)r;
    }
    g_flag |= 1u << 17;
    return 0x1FFFF;
}

/* ---- matrix * vector core ----------------------------------------------- */

enum { MX_RT = 0, MX_LLM = 1, MX_LCM = 2, MX_BAD = 3 };
enum { TX_TR = 0, TX_BK = 1, TX_FC = 2, TX_NONE = 3 };

/* Reference form (every matrix / translation, including the garbage matrix and the FC bug).
 * Computes MAC1..3 = (T*1000h + M*V) SAR shift with per-step 44-bit checks.
 * Returns the final 44-bit accumulators in acc[] (pre-shift). Does not set IR. */
__attribute__((noinline)) static void mvmva_generic(int mx, const s32 v[3], int tx, int shift, s64 acc[3])
{
    for (int i = 0; i < 3; i++) {
        s64 m0, m1, m2;
        if (mx == MX_BAD) {
            /* Garbage matrix: row 1 = -R<<4, R<<4, IR0; row 2 = RT13 x3; row 3 = RT22 x3 */
            if (i == 0) {
                s32 r = (s32)((d[D_RGBC] & 0xFF) << 4);
                m0 = -r; m1 = r; m2 = ir(0);
            } else {
                s32 e = (i == 1) ? mat_el(C_RT, 0, 2) : mat_el(C_RT, 1, 1);
                m0 = m1 = m2 = e;
            }
        } else {
            int base = (mx == MX_RT) ? C_RT : (mx == MX_LLM) ? C_LLM : C_LCM;
            m0 = mat_el(base, i, 0);
            m1 = mat_el(base, i, 1);
            m2 = mat_el(base, i, 2);
        }
        s64 t = 0;
        if (tx != TX_NONE) {
            int tb = (tx == TX_TR) ? C_TR : (tx == TX_BK) ? C_BK : C_FC;
            t = (s64)(s32)c[tb + i] * 4096;
        }
        s64 a = mac_chk(i + 1, t + m0 * v[0]);
        if (tx == TX_FC) {
            /* Hardware bug: first column result only affects flags, then is dropped. */
            sat_ir(i + 1, (s32)sar64(a, shift), 0);
            a = 0;
        }
        a = mac_chk(i + 1, a + m1 * v[1]);
        a = mac_chk(i + 1, a + m2 * v[2]);
        set_mac(i + 1, (s32)sar64(a, shift));
        acc[i] = a;
    }
}

/* Same results, specialised: always inlined, so callers with a constant matrix / translation
 * get straight-line code. Matrix elements and vector components are 16-bit signed (the
 * registers are stored sign-extended / saturated, see gte_write_data, sat_ir), so each product
 * is below 2^30 in magnitude; when |T*1000h| <= 2^42 no partial sum can leave the 44-bit range,
 * the three checks set no flag and the sign extension changes nothing, so the plain sum is the
 * same value. The garbage matrix and the FC bug take the reference form. */
static inline __attribute__((always_inline)) void mvmva_core(int mx, const s32 v[3], int tx, int shift, s64 acc[3])
{
    if (mx == MX_BAD || tx == TX_FC) { mvmva_generic(mx, v, tx, shift, acc); return; }
    const int base = (mx == MX_RT) ? C_RT : (mx == MX_LLM) ? C_LLM : C_LCM;
    for (int i = 0; i < 3; i++) {
        const s64 m0 = mat_el(base, i, 0), m1 = mat_el(base, i, 1), m2 = mat_el(base, i, 2);
        s64 t = 0;
        if (tx != TX_NONE) {
            const int tb = (tx == TX_TR) ? C_TR : C_BK;
            t = (s64)(s32)c[tb + i] * 4096;
        }
        s64 a;
        if (__builtin_expect(t >= -(1LL << 42) && t <= (1LL << 42), 1)) {
            a = t + m0 * v[0] + m1 * v[1] + m2 * v[2];
        } else {
            a = mac_chk(i + 1, t + m0 * v[0]);
            a = mac_chk(i + 1, a + m1 * v[1]);
            a = mac_chk(i + 1, a + m2 * v[2]);
        }
        set_mac(i + 1, (s32)sar64(a, shift));
        acc[i] = a;
    }
}

static inline __attribute__((always_inline)) void mat_vec(int mx, const s32 v[3], int tx, int shift, int lm)
{
    s64 acc[3];
    mvmva_core(mx, v, tx, shift, acc);
    for (int i = 1; i <= 3; i++) set_ir(i, sat_ir(i, mac(i), lm));
}

static void get_v(int n, s32 v[3])
{
    if (n == 3) {
        v[0] = ir(1); v[1] = ir(2); v[2] = ir(3);
    } else {
        v[0] = sx16(d[D_VXY0 + n * 2]);
        v[1] = sx16(d[D_VXY0 + n * 2] >> 16);
        v[2] = (s32)d[D_VZ0 + n * 2];
    }
}

static void get_ir_vec(s32 v[3]) { v[0] = ir(1); v[1] = ir(2); v[2] = ir(3); }

/* ---- commands ------------------------------------------------------------ */

/* PGXP: the same projection without the integer steps (UNR reciprocal of the integer SZ3,
 * IR saturation, SX/SY truncation), from the 44-bit accumulators of the RT*V+TR product.
 * Only read by the hi-res renderer; the guest-visible results above are untouched. A vertex
 * gets a precise value only where the hardware result is not clamped or saturated (otherwise
 * the integer one is what the game meant) and the two agree to within a few pixels. */
static void pgxp_project(const s64 acc[3], int shift, int lm)
{
    double z = (double)acc[2] / 4096.0;            /* SZ3 before rounding / clamping */
    double h = (double)(c[C_H] & 0xFFFF);
    double scale = 1.0 / (double)(1 << shift);
    double ix = (double)acc[0] * scale, iy = (double)acc[1] * scale;
    double lo = lm ? 0 : -32768.0;
    if (!(z > 0 && z <= 65535.0 && h < 2.0 * z)) return;          /* divide overflow / clamped SZ */
    if (ix < lo || ix > 32767.0 || iy < lo || iy > 32767.0) return;   /* IR1/IR2 saturated */
    double fx = ix * (h / z) + (double)(s32)c[C_OFX] / 65536.0;
    double fy = iy * (h / z) + (double)(s32)c[C_OFY] / 65536.0;
    u32 w = d[D_SXY2];
    double isx = (double)sx16(w), isy = (double)sx16(w >> 16);
    double ddx = fx - isx, ddy = fy - isy;
    if (isx <= -0x400 || isx >= 0x3FF || isy <= -0x400 || isy >= 0x3FF) return;   /* SX/SY clamped */
    if (fabs(ddx) >= 4.0 || fabs(ddy) >= 4.0) return;            /* way off: keep the integer */
    g_pgxp_gte[2] = pgxp_new(w, (float)ddx, (float)ddy, (float)z);
}

static void rtp(int n, int shift, int lm, int last)
{
    s32 v[3];
    s64 acc[3];
    get_v(n, v);
    mvmva_core(MX_RT, v, TX_TR, shift, acc);
    set_ir(1, sat_ir(1, mac(1), lm));
    set_ir(2, sat_ir(2, mac(2), lm));
    /* IR3 quirk: saturated per MAC3/lm, but flag set from MAC3_unshifted SAR 12. */
    {
        s64 z12 = sar64(acc[2], 12);
        s32 m3 = mac(3);
        s32 lo = lm ? 0 : -0x8000;
        if (z12 < -0x8000 || z12 > 0x7FFF) g_flag |= 1u << 22;
        if (m3 < lo) m3 = lo;
        if (m3 > 0x7FFF) m3 = 0x7FFF;
        set_ir(3, m3);
        push_sz(z12);
    }
    s64 hd = (s64)gte_divide();
    s64 sxv = mac0_chk(hd * ir(1) + (s64)(s32)c[C_OFX]);
    s64 syv = mac0_chk(hd * ir(2) + (s64)(s32)c[C_OFY]);
    push_sxy(sar64(sxv, 16), sar64(syv, 16));
    if (g_pgxp) pgxp_project(acc, shift, lm);
    if (last) {
        s64 dq = mac0_chk(hd * sx16(c[C_DQA]) + (s64)(s32)c[C_DQB]);
        set_mac(0, (s32)dq);
        set_ir0(sar64(dq, 12));
    }
}

static void op_nclip(void)
{
    s64 x0 = sx16(d[D_SXY0]), y0 = sx16(d[D_SXY0] >> 16);
    s64 x1 = sx16(d[D_SXY1]), y1 = sx16(d[D_SXY1] >> 16);
    s64 x2 = sx16(d[D_SXY2]), y2 = sx16(d[D_SXY2] >> 16);
    s64 v = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
    set_mac(0, (s32)mac0_chk(v));
}

static void op_op(int shift, int lm)
{
    s64 d1 = mat_el(C_RT, 0, 0), d2 = mat_el(C_RT, 1, 1), d3 = mat_el(C_RT, 2, 2);
    s64 i1 = ir(1), i2 = ir(2), i3 = ir(3);
    set_mac_ir(1, i3 * d2 - i2 * d3, shift, lm);
    set_mac_ir(2, i1 * d3 - i3 * d1, shift, lm);
    set_mac_ir(3, i2 * d1 - i1 * d2, shift, lm);
}

/* [MAC] = in + (FC*1000h - in) * IR0, with IR set; then caller pushes color. */
static void interpolate_color(s64 m1, s64 m2, s64 m3, int shift, int lm)
{
    s64 in[3] = { m1, m2, m3 };
    for (int i = 0; i < 3; i++)
        set_mac_ir(i + 1, (s64)(s32)c[C_FC + i] * 4096 - in[i], shift, 0);
    for (int i = 0; i < 3; i++)
        set_mac_ir(i + 1, (s64)ir(i + 1) * ir(0) + in[i], shift, lm);
}

static void op_dpcs(u32 color, int shift, int lm)
{
    for (int i = 0; i < 3; i++)
        set_mac_only(i + 1, (s64)((color >> (8 * i)) & 0xFF) << 16, 0);
    interpolate_color(mac(1), mac(2), mac(3), shift, lm);
    push_rgb_from_mac();
}

static void op_intpl(int shift, int lm)
{
    for (int i = 0; i < 3; i++)
        set_mac_only(i + 1, (s64)ir(i + 1) * 4096, 0);
    interpolate_color(mac(1), mac(2), mac(3), shift, lm);
    push_rgb_from_mac();
}

static void op_dcpl(int shift, int lm)
{
    for (int i = 0; i < 3; i++) {
        s64 col = (d[D_RGBC] >> (8 * i)) & 0xFF;
        set_mac_only(i + 1, (col * ir(i + 1)) * 16, 0);
    }
    interpolate_color(mac(1), mac(2), mac(3), shift, lm);
    push_rgb_from_mac();
}

/* Light: IR = LLM*V, then IR = BK + LCM*IR */
static void light(int n, int shift, int lm)
{
    s32 v[3];
    get_v(n, v);
    mat_vec(MX_LLM, v, TX_NONE, shift, lm);
    get_ir_vec(v);
    mat_vec(MX_LCM, v, TX_BK, shift, lm);
}

static void color_mul_mac(void)
{
    /* [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4 */
    for (int i = 0; i < 3; i++) {
        s64 col = (d[D_RGBC] >> (8 * i)) & 0xFF;
        set_mac_only(i + 1, (col * ir(i + 1)) * 16, 0);
    }
}

static void op_ncs(int n, int shift, int lm)
{
    light(n, shift, lm);
    push_rgb_from_mac();
}

static void op_nccs(int n, int shift, int lm)
{
    light(n, shift, lm);
    color_mul_mac();
    for (int i = 1; i <= 3; i++) set_mac_ir(i, mac(i), shift, lm);
    push_rgb_from_mac();
}

static void op_ncds(int n, int shift, int lm)
{
    light(n, shift, lm);
    color_mul_mac();
    interpolate_color(mac(1), mac(2), mac(3), shift, lm);
    push_rgb_from_mac();
}

static void op_cc(int shift, int lm)
{
    s32 v[3];
    get_ir_vec(v);
    mat_vec(MX_LCM, v, TX_BK, shift, lm);
    color_mul_mac();
    for (int i = 1; i <= 3; i++) set_mac_ir(i, mac(i), shift, lm);
    push_rgb_from_mac();
}

static void op_cdp(int shift, int lm)
{
    s32 v[3];
    get_ir_vec(v);
    mat_vec(MX_LCM, v, TX_BK, shift, lm);
    color_mul_mac();
    interpolate_color(mac(1), mac(2), mac(3), shift, lm);
    push_rgb_from_mac();
}

static void op_sqr(int shift, int lm)
{
    for (int i = 1; i <= 3; i++) {
        s64 v = ir(i);
        set_mac_ir(i, v * v, shift, lm);
    }
}

static void op_avsz(int four)
{
    s64 sum = (s64)d[D_SZ1] + d[D_SZ2] + d[D_SZ3];
    s64 zsf;
    if (four) { sum += d[D_SZ0]; zsf = (s32)c[C_ZSF4]; }
    else zsf = (s32)c[C_ZSF3];
    s64 v = mac0_chk(zsf * sum);
    set_mac(0, (s32)v);
    s64 o = sar64(v, 12);
    if (o < 0) { g_flag |= 1u << 18; o = 0; }
    else if (o > 0xFFFF) { g_flag |= 1u << 18; o = 0xFFFF; }
    d[D_OTZ] = (u32)o;
}

static void op_gpf(int shift, int lm)
{
    for (int i = 1; i <= 3; i++)
        set_mac_ir(i, (s64)ir(i) * ir(0), shift, lm);
    push_rgb_from_mac();
}

static void op_gpl(int shift, int lm)
{
    for (int i = 1; i <= 3; i++)
        set_mac_ir(i, (s64)mac(i) * ((s64)1 << shift) + (s64)ir(i) * ir(0), shift, lm);
    push_rgb_from_mac();
}

/* ---- public API ----------------------------------------------------------- */

void gte_init(void)
{
    for (int i = 0; i < 32; i++) { d[i] = 0; c[i] = 0; }
    g_flag = 0;
    d[D_LZCR] = 32; /* LZCS=0 -> 32 leading zeros */
    g_pgxp_gte[0] = g_pgxp_gte[1] = g_pgxp_gte[2] = 0;
}

void gte_command(u32 instr)
{
    int shift = (instr & (1u << 19)) ? 12 : 0;
    int lm = (instr >> 10) & 1;
    int mx = (instr >> 17) & 3;
    int vx = (instr >> 15) & 3;
    int tx = (instr >> 13) & 3;

    g_flag = 0;

    switch (instr & 0x3F) {
    case 0x01: rtp(0, shift, lm, 1); break;                     /* RTPS */
    case 0x06: op_nclip(); break;                               /* NCLIP */
    case 0x0C: op_op(shift, lm); break;                         /* OP */
    case 0x10: op_dpcs(d[D_RGBC], shift, lm); break;            /* DPCS */
    case 0x11: op_intpl(shift, lm); break;                      /* INTPL */
    case 0x12: {                                                /* MVMVA */
        s32 v[3];
        get_v(vx, v);
        switch (mx * 4 + tx) {   /* constant matrix / translation: specialised mvmva_core */
        case MX_RT * 4 + TX_TR:    mat_vec(MX_RT, v, TX_TR, shift, lm); break;
        case MX_RT * 4 + TX_BK:    mat_vec(MX_RT, v, TX_BK, shift, lm); break;
        case MX_RT * 4 + TX_NONE:  mat_vec(MX_RT, v, TX_NONE, shift, lm); break;
        case MX_LLM * 4 + TX_TR:   mat_vec(MX_LLM, v, TX_TR, shift, lm); break;
        case MX_LLM * 4 + TX_BK:   mat_vec(MX_LLM, v, TX_BK, shift, lm); break;
        case MX_LLM * 4 + TX_NONE: mat_vec(MX_LLM, v, TX_NONE, shift, lm); break;
        case MX_LCM * 4 + TX_TR:   mat_vec(MX_LCM, v, TX_TR, shift, lm); break;
        case MX_LCM * 4 + TX_BK:   mat_vec(MX_LCM, v, TX_BK, shift, lm); break;
        case MX_LCM * 4 + TX_NONE: mat_vec(MX_LCM, v, TX_NONE, shift, lm); break;
        default:                   mat_vec(mx, v, tx, shift, lm); break;
        }
        break;
    }
    case 0x13: op_ncds(0, shift, lm); break;                    /* NCDS */
    case 0x14: op_cdp(shift, lm); break;                        /* CDP */
    case 0x16:                                                  /* NCDT */
        for (int n = 0; n < 3; n++) op_ncds(n, shift, lm);
        break;
    case 0x1B: op_nccs(0, shift, lm); break;                    /* NCCS */
    case 0x1C: op_cc(shift, lm); break;                         /* CC */
    case 0x1E: op_ncs(0, shift, lm); break;                     /* NCS */
    case 0x20:                                                  /* NCT */
        for (int n = 0; n < 3; n++) op_ncs(n, shift, lm);
        break;
    case 0x28: op_sqr(shift, lm); break;                        /* SQR */
    case 0x29: op_dcpl(shift, lm); break;                       /* DCPL */
    case 0x2A:                                                  /* DPCT */
        for (int n = 0; n < 3; n++) op_dpcs(d[D_RGB0], shift, lm);
        break;
    case 0x2D: op_avsz(0); break;                               /* AVSZ3 */
    case 0x2E: op_avsz(1); break;                               /* AVSZ4 */
    case 0x30:                                                  /* RTPT */
        rtp(0, shift, lm, 0);
        rtp(1, shift, lm, 0);
        rtp(2, shift, lm, 1);
        break;
    case 0x3D: op_gpf(shift, lm); break;                        /* GPF */
    case 0x3E: op_gpl(shift, lm); break;                        /* GPL */
    case 0x3F:                                                  /* NCCT */
        for (int n = 0; n < 3; n++) op_nccs(n, shift, lm);
        break;
    default:
        /* Unassigned opcodes: no documented effect (FLAG still cleared). */
        break;
    }
}

u32 gte_read_data(int reg)
{
    reg &= 31;
    switch (reg) {
    case D_SXYP:
        return d[D_SXY2];
    case D_IRGB:
    case D_ORGB: {
        u32 out = 0;
        for (int i = 0; i < 3; i++) {
            s32 v = ir(i + 1) >> 7;
            if (v < 0) v = 0;
            if (v > 0x1F) v = 0x1F;
            out |= (u32)v << (5 * i);
        }
        return out;
    }
    default:
        return d[reg];
    }
}

void gte_write_data(int reg, u32 v)
{
    reg &= 31;
    switch (reg) {
    case 1: case 3: case 5:              /* VZ0..2 */
    case 8: case 9: case 10: case 11:    /* IR0..3 */
        d[reg] = (u32)sx16(v);
        break;
    case D_OTZ:
    case D_SZ0: case D_SZ1: case D_SZ2: case D_SZ3:
        d[reg] = v & 0xFFFF;
        break;
    case D_SXYP:
        d[D_SXY0] = d[D_SXY1];
        d[D_SXY1] = d[D_SXY2];
        d[D_SXY2] = v;
        pgxp_shift();
        break;
    case D_SXY0: case D_SXY1: case D_SXY2:
        d[reg] = v;
        g_pgxp_gte[reg - D_SXY0] = 0;   /* the mtc2/lwc2 hook attaches the source's tag */
        break;
    case D_IRGB:
        d[D_IRGB] = v & 0x7FFF;
        set_ir(1, (s32)((v & 0x1F) << 7));
        set_ir(2, (s32)(((v >> 5) & 0x1F) << 7));
        set_ir(3, (s32)(((v >> 10) & 0x1F) << 7));
        break;
    case D_ORGB:
    case D_LZCR:
        break; /* read-only */
    case D_LZCS:
        d[D_LZCS] = v;
        d[D_LZCR] = lzc(v);
        break;
    default:
        d[reg] = v;
        break;
    }
}

u32 gte_read_ctrl(int reg)
{
    reg &= 31;
    switch (reg) {
    case C_H:
        return (u32)sx16(c[C_H]); /* hardware bug: H reads sign-extended */
    case C_FLAG: {
        u32 f = g_flag & 0x7FFFF000u;
        if (f & FLAG_ERROR_MASK) f |= 0x80000000u;
        return f;
    }
    default:
        return c[reg];
    }
}

void gte_write_ctrl(int reg, u32 v)
{
    reg &= 31;
    switch (reg) {
    case 4: case 12: case 20:            /* RT33, L33, LB3 */
    case C_DQA: case C_ZSF3: case C_ZSF4:
        c[reg] = (u32)sx16(v);
        break;
    case C_H:
        c[reg] = v & 0xFFFF;
        break;
    case C_FLAG:
        g_flag = v & 0x7FFFF000u;
        break;
    default:
        c[reg] = v;
        break;
    }
}

/* Whole-GTE snapshot (widescreen shadow pass, runtime/overrides/widescreen.c). */
void gte_save(GteState *s)
{
    memcpy(s->d, d, sizeof d);
    memcpy(s->c, c, sizeof c);
    s->flag = g_flag;
}

void gte_restore(const GteState *s)
{
    memcpy(d, s->d, sizeof d);
    memcpy(c, s->c, sizeof c);
    g_flag = s->flag;
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
void gte_state_io(StateIO *io) { SS_VAR(io, d); SS_VAR(io, c); SS_VAR(io, g_flag); }
