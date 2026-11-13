/* Self-checking GTE tests. Build:
 *   clang -std=c11 -O2 -Wall -Wextra -Iruntime runtime/gte.c runtime/tests/test_gte.c -o build/tests/test_gte
 */
#include "psx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define EQ(a, b) do { u32 _a = (u32)(a), _b = (u32)(b); \
    CHECK(_a == _b, "%s = 0x%08x, expected 0x%08x", #a, _a, _b); } while (0)

/* Instruction words (cop2 imm25) */
#define SF (1u << 19)
#define LM (1u << 10)
#define OP_RTPS  0x01
#define OP_NCLIP 0x06
#define OP_DPCS  0x10
#define OP_MVMVA 0x12
#define OP_SQR   0x28
#define OP_AVSZ3 0x2D
#define OP_AVSZ4 0x2E
#define OP_RTPT  0x30
#define OP_GPF   0x3D
#define MVMVA(mx, vx, tx) (OP_MVMVA | ((u32)(mx) << 17) | ((u32)(vx) << 15) | ((u32)(tx) << 13))
#define COP2(op) (0x4A000000u | (op))

static u32 flag(void) { return gte_read_ctrl(31); }

static void set_identity_rt(void)
{
    gte_write_ctrl(0, 0x1000); /* RT11=1.0, RT12=0 */
    gte_write_ctrl(1, 0);      /* RT13, RT21 */
    gte_write_ctrl(2, 0x1000); /* RT22=1.0, RT23 */
    gte_write_ctrl(3, 0);      /* RT31, RT32 */
    gte_write_ctrl(4, 0x1000); /* RT33 */
    gte_write_ctrl(5, 0); gte_write_ctrl(6, 0); gte_write_ctrl(7, 0);
}

static void test_regs(void)
{
    gte_init();
    gte_write_data(1, 0xffff);         EQ(gte_read_data(1), 0xffffffff);
    gte_write_data(1, 0x12347fff);     EQ(gte_read_data(1), 0x7fff);
    gte_write_data(9, 0x8000);         EQ(gte_read_data(9), 0xffff8000);
    gte_write_data(7, 0xffff8001);     EQ(gte_read_data(7), 0x8001);
    gte_write_data(17, 0x12348765);    EQ(gte_read_data(17), 0x8765);
    gte_write_data(0, 0xdeadbeef);     EQ(gte_read_data(0), 0xdeadbeef);
    /* SXYP FIFO */
    gte_write_data(12, 1); gte_write_data(13, 2); gte_write_data(14, 3);
    gte_write_data(15, 4);
    EQ(gte_read_data(12), 2); EQ(gte_read_data(13), 3); EQ(gte_read_data(14), 4);
    EQ(gte_read_data(15), 4);
    /* control quirks */
    gte_write_ctrl(26, 0x8000);        EQ(gte_read_ctrl(26), 0xffff8000);
    gte_write_ctrl(26, 0x12340100);    EQ(gte_read_ctrl(26), 0x0100);
    gte_write_ctrl(4, 0x0001ffff);     EQ(gte_read_ctrl(4), 0xffffffff);
    gte_write_ctrl(12, 0x00008000);    EQ(gte_read_ctrl(12), 0xffff8000);
    gte_write_ctrl(20, 0x00001234);    EQ(gte_read_ctrl(20), 0x1234);
    gte_write_ctrl(27, 0xfffe);        EQ(gte_read_ctrl(27), 0xfffffffe);
    gte_write_ctrl(29, 0x8000);        EQ(gte_read_ctrl(29), 0xffff8000);
    gte_write_ctrl(28, 0x80000000);    EQ(gte_read_ctrl(28), 0x80000000);
    gte_write_ctrl(0, 0xffff0001);     EQ(gte_read_ctrl(0), 0xffff0001);
    /* FLAG write: low 12 bits / bit31 masked, bit31 recomputed */
    gte_write_ctrl(31, 0xffffffff);    EQ(flag(), 0xfffff000);
    gte_write_ctrl(31, 0x00001000);    EQ(flag(), 0x00001000); /* IR0 sat: not an error bit */
    gte_write_ctrl(31, 0x00040000);    EQ(flag(), 0x80040000); /* SZ sat: error bit */
    gte_write_ctrl(31, 0x00400000);    EQ(flag(), 0x00400000); /* IR3 sat: NOT an error bit */
    gte_write_ctrl(31, 0x00800000);    EQ(flag(), 0x80800000); /* IR2 sat: error bit */
    gte_write_ctrl(31, 0x00380000);    EQ(flag(), 0x00380000); /* color sat: not error */
    gte_command(COP2(OP_NCLIP));       EQ(flag(), 0);           /* cleared per command */
}

static void test_lzcr(void)
{
    gte_init();
    EQ(gte_read_data(31), 32);
    static const struct { u32 in, out; } t[] = {
        {0, 32}, {0xffffffff, 32}, {1, 31}, {0x00010000, 15}, {0x80000000, 1},
        {0x7fffffff, 1}, {0xfff00000, 12}, {0xfffffffe, 31}, {0x00ffffff, 8},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        gte_write_data(30, t[i].in);
        EQ(gte_read_data(30), t[i].in);
        CHECK(gte_read_data(31) == t[i].out, "LZCR(0x%08x)=%u expected %u",
              t[i].in, gte_read_data(31), t[i].out);
    }
    gte_write_data(31, 5); /* read-only */
    EQ(gte_read_data(31), 8);
}

static void test_irgb_orgb(void)
{
    gte_init();
    gte_write_data(28, 0xffff);
    EQ(gte_read_data(9), 0xf80); EQ(gte_read_data(10), 0xf80); EQ(gte_read_data(11), 0xf80);
    EQ(gte_read_data(29), 0x7fff);
    EQ(gte_read_data(28), 0x7fff);
    gte_write_data(28, (3u) | (17u << 5) | (30u << 10));
    EQ(gte_read_data(9), 3 << 7); EQ(gte_read_data(10), 17 << 7); EQ(gte_read_data(11), 30 << 7);
    EQ(gte_read_data(29), (3u) | (17u << 5) | (30u << 10));
    gte_write_data(9, (u32)-5); gte_write_data(10, 0x2000); gte_write_data(11, 0x100);
    EQ(gte_read_data(29), (0u) | (0x1fu << 5) | (2u << 10));
    gte_write_data(29, 0); /* read-only */
    EQ(gte_read_data(29), (0u) | (0x1fu << 5) | (2u << 10));
}

static void setup_proj(u32 h)
{
    gte_init();
    set_identity_rt();
    gte_write_ctrl(24, 160u << 16); /* OFX */
    gte_write_ctrl(25, 120u << 16); /* OFY */
    gte_write_ctrl(26, h);
    gte_write_ctrl(27, 1);          /* DQA = 1 -> MAC0 = H/SZ3 quotient */
    gte_write_ctrl(28, 0);          /* DQB */
}

static void test_rtps(void)
{
    setup_proj(1000);
    gte_write_data(0, (100u & 0xffff) | ((u32)(u16)-50 << 16));
    gte_write_data(1, 1000);
    gte_command(COP2(SF | OP_RTPS));
    EQ(gte_read_data(25), 100); EQ(gte_read_data(26), (u32)-50); EQ(gte_read_data(27), 1000);
    EQ(gte_read_data(9), 100); EQ(gte_read_data(10), (u32)-50); EQ(gte_read_data(11), 1000);
    EQ(gte_read_data(19), 1000);
    EQ(gte_read_data(24), 0x10000);           /* MAC0 = quotient*DQA + DQB */
    EQ(gte_read_data(8), 0x10);               /* IR0 = MAC0 >> 12 */
    EQ(gte_read_data(14), (260u) | (70u << 16));
    EQ(flag(), 0);

    /* Exact UNR quotients (reference from psx-spx algorithm) */
    static const struct { u32 h, sz, q; } dq[] = {
        {1000, 1000, 0x10000}, {200, 300, 0xaaab}, {1, 3, 0x5555}, {0x7fff, 0x4001, 0x1fff4},
    };
    for (size_t i = 0; i < sizeof dq / sizeof dq[0]; i++) {
        setup_proj(dq[i].h);
        gte_write_data(0, 0);
        gte_write_data(1, dq[i].sz);
        gte_command(COP2(SF | OP_RTPS));
        CHECK(gte_read_data(24) == dq[i].q, "UNR %u/%u = 0x%x expected 0x%x",
              dq[i].h, dq[i].sz, gte_read_data(24), dq[i].q);
        CHECK((flag() & (1u << 17)) == 0, "no div overflow");
    }

    /* Sweep: UNR within 3 of the exact rounded quotient */
    int bad = 0;
    for (u32 sz = 1; sz < 0x8000; sz += 13) {
        for (u32 h = 1; h < 0x10000; h += 977) {
            if (h >= sz * 2) continue;
            setup_proj(h);
            gte_write_data(0, 0);
            gte_write_data(1, sz);
            gte_command(COP2(SF | OP_RTPS));
            s64 exact = (((s64)h * 0x20000 / sz) + 1) / 2;
            s64 got = (s32)gte_read_data(24);
            if (exact > 0x1ffff) exact = 0x1ffff;
            if (llabs(got - exact) > 3) /* UNR approximation error is up to 3 */ { if (bad < 8) printf("  h=%u sz=%u got=%lld exact=%lld\n", h, sz, (long long)got, (long long)exact); bad++; }
        }
    }
    CHECK(bad == 0, "UNR sweep mismatches: %d", bad);

    /* Divide overflow: H >= SZ3*2 */
    setup_proj(1000);
    gte_write_data(0, 1);
    gte_write_data(1, 400);
    gte_command(COP2(SF | OP_RTPS));
    EQ(gte_read_data(24), 0x1ffff);
    CHECK((flag() & 0x80020000) == 0x80020000, "div overflow flag 0x%08x", flag());
    EQ(gte_read_data(14) & 0xffff, 161); /* (0x1ffff*1 + 160<<16) >> 16 */

    /* SZ3 = 0 -> overflow; SX/SY saturation */
    setup_proj(1000);
    gte_write_data(0, (0x7000u) | (0x9000u << 16));
    gte_write_data(1, 0);
    gte_command(COP2(SF | OP_RTPS));
    EQ(gte_read_data(14), 0x3ffu | (0xfc00u << 16));
    CHECK((flag() & ((1u << 17) | (1u << 14) | (1u << 13) | (1u << 31))) ==
          ((1u << 17) | (1u << 14) | (1u << 13) | (1u << 31)), "sat flags 0x%08x", flag());
    EQ(gte_read_data(8), 0x1f);               /* IR0 = 0x1ffff >> 12 */

    /* Negative Z -> SZ3 saturates to 0 with flag 18 */
    setup_proj(1000);
    gte_write_data(0, 0);
    gte_write_data(1, (u32)-10);
    gte_command(COP2(SF | OP_RTPS));
    EQ(gte_read_data(19), 0);
    CHECK(flag() & (1u << 18), "SZ3 sat flag 0x%08x", flag());

    /* RTPS sf=0 IR3 quirk: MAC3 = 0x10000 (saturates IR3) but MAC3>>12 fits -> no flag */
    setup_proj(1000);
    gte_write_data(0, 0);
    gte_write_data(1, 16);
    gte_command(COP2(OP_RTPS));
    EQ(gte_read_data(27), 0x10000);
    EQ(gte_read_data(11), 0x7fff);
    EQ(gte_read_data(19), 16);
    CHECK((flag() & (1u << 22)) == 0, "RTPS IR3 quirk flag 0x%08x", flag());

    /* RTPT: three vertices into FIFO */
    setup_proj(1000);
    gte_write_data(0, 10);  gte_write_data(1, 1000);
    gte_write_data(2, 20);  gte_write_data(3, 1000);
    gte_write_data(4, 30);  gte_write_data(5, 2000);
    gte_command(COP2(SF | OP_RTPT));
    EQ(gte_read_data(12) & 0xffff, 170);
    EQ(gte_read_data(13) & 0xffff, 180);
    EQ(gte_read_data(14) & 0xffff, 175);
    EQ(gte_read_data(17), 1000); EQ(gte_read_data(18), 1000); EQ(gte_read_data(19), 2000);
    EQ(gte_read_data(24), 0x8000);
}

static void test_nclip_avsz(void)
{
    gte_init();
    gte_write_data(12, 0);
    gte_write_data(13, 10);
    gte_write_data(14, 10u << 16);
    gte_command(COP2(OP_NCLIP));
    EQ(gte_read_data(24), 100);
    gte_write_data(13, 10u << 16);
    gte_write_data(14, 10);
    gte_command(COP2(OP_NCLIP));
    EQ(gte_read_data(24), (u32)-100);

    gte_write_ctrl(29, 0x555);
    gte_write_data(16, 0); gte_write_data(17, 300); gte_write_data(18, 600); gte_write_data(19, 900);
    gte_command(COP2(OP_AVSZ3));
    EQ(gte_read_data(24), 1365 * 1800);
    EQ(gte_read_data(7), (1365 * 1800) >> 12);
    EQ(flag(), 0);
    gte_write_ctrl(30, 0x400);
    gte_write_data(16, 200);
    gte_command(COP2(OP_AVSZ4));
    EQ(gte_read_data(7), (0x400 * 2000) >> 12);
    /* OTZ saturation */
    gte_write_ctrl(30, 0x7fff);
    gte_write_data(16, 0xffff); gte_write_data(17, 0xffff); gte_write_data(18, 0xffff); gte_write_data(19, 0xffff);
    gte_command(COP2(OP_AVSZ4));
    EQ(gte_read_data(7), 0xffff);
    EQ(flag(), 0x80050000); /* SZ/OTZ sat + MAC0 pos overflow */
    /* MAC0 overflow */
    gte_write_ctrl(30, (u32)-0x8000);
    gte_command(COP2(OP_AVSZ4));
    CHECK(flag() & (1u << 15), "MAC0 neg overflow flag 0x%08x", flag());
    EQ(gte_read_data(7), 0);
}

static void test_sqr_gpf(void)
{
    gte_init();
    gte_write_data(9, 3); gte_write_data(10, (u32)-4); gte_write_data(11, 200);
    gte_command(COP2(OP_SQR));
    EQ(gte_read_data(25), 9); EQ(gte_read_data(26), 16); EQ(gte_read_data(27), 40000);
    EQ(gte_read_data(9), 9); EQ(gte_read_data(10), 16); EQ(gte_read_data(11), 0x7fff);
    EQ(flag(), 0x00400000); /* IR3 sat is not an error bit */
    gte_write_data(9, 0x1000); gte_write_data(10, (u32)-0x1000); gte_write_data(11, 0x800);
    gte_command(COP2(SF | OP_SQR));
    EQ(gte_read_data(25), 0x1000); EQ(gte_read_data(26), 0x1000); EQ(gte_read_data(27), 0x400);
    EQ(flag(), 0);

    /* GPF: IR0 * IR -> color FIFO */
    gte_init();
    gte_write_data(6, 0xab000000);
    gte_write_data(8, 0x800);
    gte_write_data(9, 0x400); gte_write_data(10, 0x200); gte_write_data(11, (u32)-0x10);
    gte_command(COP2(SF | OP_GPF));
    EQ(gte_read_data(25), 0x200); EQ(gte_read_data(26), 0x100); EQ(gte_read_data(27), (u32)-8);
    EQ(gte_read_data(22), 0xab000000u | 0x20 | (0x10 << 8) | (0 << 16));
    CHECK(flag() & (1u << 19), "B color sat flag 0x%08x", flag());
    CHECK(!(flag() & 0x80000000u), "color sat not error 0x%08x", flag());
}

static void test_mvmva(void)
{
    gte_init();
    set_identity_rt();
    gte_write_ctrl(5, 1); gte_write_ctrl(6, 2); gte_write_ctrl(7, 3);
    gte_write_data(0, 0x00200010); gte_write_data(1, 0x30);
    gte_command(COP2(SF | MVMVA(0, 0, 0)));
    EQ(gte_read_data(25), 0x11); EQ(gte_read_data(26), 0x22); EQ(gte_read_data(27), 0x33);
    gte_command(COP2(SF | MVMVA(0, 0, 3)));
    EQ(gte_read_data(25), 0x10); EQ(gte_read_data(26), 0x20); EQ(gte_read_data(27), 0x30);
    /* vx=3 uses IR, lm clamps negatives */
    gte_write_data(9, (u32)-5); gte_write_data(10, 7); gte_write_data(11, 9);
    gte_command(COP2(SF | LM | MVMVA(0, 3, 3)));
    EQ(gte_read_data(25), (u32)-5); EQ(gte_read_data(9), 0); EQ(gte_read_data(10), 7);
    CHECK(flag() & (1u << 24), "IR1 lm sat 0x%08x", flag());
    /* FC translation bug: first column (FC + RT11*VX) dropped, only flags */
    gte_write_ctrl(21, 0x10000); gte_write_ctrl(22, 0); gte_write_ctrl(23, 0);
    gte_write_data(0, 0x00200010); gte_write_data(1, 0x30);
    gte_command(COP2(SF | MVMVA(0, 0, 2)));
    EQ(gte_read_data(25), 0); EQ(gte_read_data(26), 0x20); EQ(gte_read_data(27), 0x30);
    EQ(flag(), 0x81000000);
    /* MAC 44-bit overflow */
    gte_write_ctrl(5, 0x7ff00000);
    gte_write_ctrl(0, 0x7fff);
    gte_write_data(0, 0x7fff);
    gte_command(COP2(MVMVA(0, 0, 0)));
    EQ(flag() & 0x40000000u, 0); /* 0x7ff00000*4096 + 0x7fff^2 < 2^43 */
    gte_write_ctrl(5, 0x7fffffff);
    gte_write_ctrl(0, 0x7fff7fff);
    gte_write_ctrl(1, 0x00007fff);
    gte_write_data(0, 0x7fff7fff); gte_write_data(1, 0x7fff);
    gte_command(COP2(MVMVA(0, 0, 0)));
    CHECK(flag() & 0x40000000u, "MAC1 pos overflow 0x%08x", flag());
    CHECK(flag() & 0x80000000u, "error bit 0x%08x", flag());
}

static void test_dpcs(void)
{
    gte_init();
    gte_write_data(6, 0x55302010);
    gte_write_data(8, 0);
    gte_command(COP2(SF | OP_DPCS));
    EQ(gte_read_data(22), 0x55302010);
    gte_write_ctrl(21, 0x800); gte_write_ctrl(22, 0x400); gte_write_ctrl(23, 0x200);
    gte_write_data(8, 0x1000);
    gte_command(COP2(SF | OP_DPCS));
    EQ(gte_read_data(22), 0x55204080);
    EQ(gte_read_data(21), 0x55302010);
}

/* gte_exec_on: the guest GTE (registers, FLAG, PGXP FIFO tags, g_pgxp) is untouched and the
 * host-owned state ends exactly as if the guest GTE had run the same commands. */
#include "pgxp.h"
static u32 rnd_state = 12345;
static u32 rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 8) ^ (rnd_state << 13); }

static void random_gte(void)
{
    for (int r = 0; r < 32; r++) if (r != 15 && r != 28 && r != 29 && r != 31) gte_write_data(r, rnd());
    for (int r = 0; r < 31; r++) gte_write_ctrl(r, rnd() & (r == 26 ? 0x3ff : 0xffffffff));
    gte_write_ctrl(26, 0x300 + (rnd() & 0xff)); /* H */
}

static void test_exec_on(void)
{
    static const u32 ops[] = {
        0x4a180001, 0x4a280030, 0x4a080001, 0x4a400006, 0x4a780010, 0x4af8002a, 0x4ae80413,
        0x4af80416, 0x4a486012, 0x4a49e012, 0x4a58002d, 0x4a68002e, 0x4aa00428, 0x4b90003d,
        0x4b98003e, 0x4b28001b, 0x4b3f0011, 0x4a700029, 0x4b000013,
    };
    for (int round = 0; round < 300; round++) {
        gte_init();
        random_gte();
        GteState host0, host, guest, after, ref;
        gte_save(&host0);
        host = host0;
        random_gte();
        gte_save(&guest);
        g_pgxp = true;
        g_pgxp_gte[0] = 11; g_pgxp_gte[1] = 22; g_pgxp_gte[2] = 33;
        u32 seq[8], val[8];
        for (int i = 0; i < 8; i++) { seq[i] = ops[rnd() % (sizeof ops / sizeof ops[0])]; val[i] = rnd(); }
        u32 hv[8], hf[8];
        for (int i = 0; i < 8; i++) {
            gte_exec_on(&host, seq[i]);
            hf[i] = gte_read_ctrl_on(&host, 31);
            hv[i] = gte_read_data_on(&host, 22);
            gte_write_data_on(&host, i & 1 ? 15 : 8, val[i]);
            gte_write_ctrl_on(&host, (int)(val[i] % 31), val[i] ^ 0x5a5a5a5a);
        }
        gte_save(&after);
        CHECK(!memcmp(&guest, &after, sizeof guest), "round %d: guest GTE changed", round);
        CHECK(g_pgxp && g_pgxp_gte[0] == 11 && g_pgxp_gte[1] == 22 && g_pgxp_gte[2] == 33,
              "round %d: PGXP tags or flag changed", round);
        /* the same on the guest GTE (PGXP off, as gte_exec_on runs it) */
        g_pgxp = false;
        gte_restore(&host0);
        int same = 1;
        for (int i = 0; i < 8; i++) {
            gte_command(seq[i]);
            same &= gte_read_ctrl(31) == hf[i] && gte_read_data(22) == hv[i];
            gte_write_data(i & 1 ? 15 : 8, val[i]);
            gte_write_ctrl((int)(val[i] % 31), val[i] ^ 0x5a5a5a5a);
        }
        gte_save(&ref);
        CHECK(same && !memcmp(&ref, &host, sizeof ref), "round %d: host result differs from guest", round);
    }
}

int main(void)
{
    test_regs();
    test_lzcr();
    test_irgb_orgb();
    test_rtps();
    test_nclip_avsz();
    test_sqr_gpf();
    test_mvmva();
    test_dpcs();
    test_exec_on();
    printf("test_gte: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
