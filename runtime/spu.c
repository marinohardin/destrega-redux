/* PS1 SPU emulation (register level, psx-spx / nocash semantics).
 *
 * - 512KB sound RAM, 24 ADPCM voices with 4-point Gaussian interpolation
 *   (official 512-entry table), pitch modulation, noise, ADSR and volume
 *   sweep envelopes, ENDX/KON/KOFF, SPU IRQ (voice reads, transfers,
 *   capture buffer writes, reverb writes).
 * - Reverb runs at 22.05kHz on the work area in SPU RAM; input is
 *   decimated and output interpolated with the 39-tap FIR from psx-spx.
 * - CD audio arrives via spu_push_cd_audio() (44.1kHz stereo) into a ring
 *   buffer that spu_render() drains, one frame per output sample.
 *
 * Everything runs synchronously from spu_render(): one call to tick() is one
 * 44.1kHz SPU cycle. Register writes take effect immediately (real hardware
 * applies them on the next 44.1kHz cycle; the difference is inaudible).
 */
#include "psx.h"
#include <string.h>

#define SPU_RAM_SIZE 0x80000u
#define SPU_RAM_MASK 0x7ffffu
#define NUM_VOICES 24
#define CD_BUF_FRAMES 65536u /* ~1.49s at 44.1kHz, power of two */

/* ---- Tables ------------------------------------------------------------- */

static const s16 gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7,
    8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24,
    25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56,
    58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111,
    116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199,
    205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328,
    338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512,
    525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761,
    779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089,
    1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
    1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
    2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
    2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
    3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
    4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
    5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
    6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
    7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
    9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
    10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
    12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
    13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
    15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
    16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
    18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
    19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
    21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
    22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
    22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
    22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

static const s32 adpcm_pos[5] = {0, 60, 115, 98, 122};
static const s32 adpcm_neg[5] = {0, 0, -52, -55, -60};

/* 39-tap reverb resampling FIR (psx-spx "Reverb Buffer Resampling"). */
static const s32 rv_fir[39] = {
    -0x0001, 0x0000, 0x0002, 0x0000, -0x000A, 0x0000, 0x0023, 0x0000,
    -0x0067, 0x0000, 0x010A, 0x0000, -0x0268, 0x0000, 0x0534, 0x0000,
    -0x0B90, 0x0000, 0x2806, 0x4000, 0x2806, 0x0000, -0x0B90, 0x0000,
    0x0534,  0x0000, -0x0268, 0x0000, 0x010A, 0x0000, -0x0067, 0x0000,
    0x0023,  0x0000, -0x000A, 0x0000, 0x0002, 0x0000, -0x0001,
};

/* ---- State -------------------------------------------------------------- */

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    s16 level;
    s32 counter;
    bool active; /* sweep mode (bit15 set in the volume register) */
    u16 reg;
} Sweep;

typedef struct {
    u32 addr;          /* byte address of the current ADPCM block */
    u16 repeat;        /* repeat address (8-byte units) */
    bool ignore_loop;  /* repeat address written by software since KON */
    bool has_samples;  /* current block decoded */
    u8 flags;          /* flags byte of the current block */
    u32 counter;       /* pitch counter: bits 12+ sample index, 4..11 interp */
    s16 buf[3 + 28];   /* 3 samples of history + current block */
    s32 old, older;    /* ADPCM decoder history */
    int phase;
    s16 envx;
    s32 env_counter;
    Sweep vol[2];
    s16 out;           /* last output after ADSR (pitch modulation source) */
} Voice;

static u8 ram[SPU_RAM_SIZE];
static u16 regs[0x200]; /* raw last-written values, indexed by offset/2 */
static Voice voices[NUM_VOICES];
static Sweep main_vol[2];
static u16 spucnt;
static u16 stat_irq;    /* bit 6 */
static u32 endx;
static u32 xfer_addr;   /* internal current transfer address (bytes) */
static u16 noise_level;
static s32 noise_timer;
static u32 capture_pos; /* 0..0x1ff */

static u32 rv_base;     /* halfword index */
static u32 rv_cur;      /* halfword index */
static s16 rv_in_hist[2][64];
static s16 rv_out_hist[2][64];
static u32 rv_pos;

static s16 cd_buf[CD_BUF_FRAMES * 2];
static u32 cd_rd, cd_wr; /* free-running frame indices */

#define REG(off) regs[((off) & 0x3fe) >> 1]

/* ---- Helpers ------------------------------------------------------------ */

static inline s32 clamp16(s32 v) {
    return v < -0x8000 ? -0x8000 : (v > 0x7fff ? 0x7fff : v);
}

static inline u16 ram_r16(u32 a) {
    a &= SPU_RAM_MASK & ~1u;
    return (u16)(ram[a] | (ram[a + 1] << 8));
}

static inline void ram_w16(u32 a, u16 v) {
    a &= SPU_RAM_MASK & ~1u;
    ram[a] = (u8)v;
    ram[a + 1] = (u8)(v >> 8);
}

static inline bool irq_enabled(void) {
    return (spucnt & 0x8040) == 0x8040;
}

static void irq_hit(void) {
    if (!(stat_irq & 0x40)) {
        stat_irq |= 0x40;
        irq_raise(IRQ_SPU);
    }
}

/* Access to [a, a+len) touches the IRQ address? */
static inline void irq_check_range(u32 a, u32 len) {
    if (!irq_enabled()) return;
    u32 ia = (u32)REG(0x1a4) << 3;
    if (((ia - a) & SPU_RAM_MASK) < len) irq_hit();
}

/* ---- Envelopes (ADSR and volume sweep share the psx-spx algorithm) ------- */

static void env_tick(s16 *level, s32 *counter, int rate, int rate_mask,
                     bool dec, bool expo, bool neg) {
    int shift = rate >> 2;
    int stepv = rate & 3;
    s32 step = 7 - stepv;
    if (dec != neg) step = ~step;               /* +7..+4 -> -8..-5 */
    if (shift < 11) step *= 1 << (11 - shift);
    s32 inc = 0x8000 >> (shift > 11 ? shift - 11 : 0);
    s32 lv = *level;

    if (expo && !dec && lv > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) inc >>= 2;
        else { step >>= 1; inc >>= 1; }
    } else if (expo && dec) {
        step = (step * lv) >> 15;
    }
    if (rate != rate_mask && inc < 1) inc = 1;

    *counter += inc;
    if (!(*counter & 0x8000)) return;
    *counter = 0;

    lv += step;
    if (!dec) lv = clamp16(lv);
    else if (neg) lv = lv < -0x8000 ? -0x8000 : (lv > 0 ? 0 : lv);
    else lv = lv < 0 ? 0 : (lv > 0x7fff ? 0x7fff : lv);
    *level = (s16)lv;
}

static void sweep_set(Sweep *s, u16 v) {
    s->reg = v;
    if (v & 0x8000) {
        s->active = true;
        s->counter = 0;
    } else {
        s->active = false;
        s->level = (s16)(u16)(v << 1);
    }
}

static void sweep_tick(Sweep *s) {
    if (!s->active) return;
    u16 v = s->reg;
    env_tick(&s->level, &s->counter, v & 0x7f, 0x7f,
             (v >> 13) & 1, (v >> 14) & 1, (v >> 12) & 1);
}

static void adsr_tick(Voice *v, int vi) {
    u16 a1 = REG(vi * 16 + 8), a2 = REG(vi * 16 + 10);
    int rate, mask;
    bool dec, expo;
    s32 target = 0;
    switch (v->phase) {
    case PH_ATTACK:
        rate = (a1 >> 8) & 0x7f; mask = 0x7f; dec = false; expo = a1 >> 15;
        target = 0x7fff;
        break;
    case PH_DECAY:
        rate = ((a1 >> 4) & 0xf) << 2; mask = 0x7c; dec = true; expo = true;
        target = ((a1 & 0xf) + 1) * 0x800;
        if (target > 0x7fff) target = 0x7fff;
        break;
    case PH_SUSTAIN:
        rate = (a2 >> 6) & 0x7f; mask = 0x7f; dec = (a2 >> 14) & 1;
        expo = a2 >> 15;
        break;
    case PH_RELEASE:
        rate = (a2 & 0x1f) << 2; mask = 0x7c; dec = true; expo = (a2 >> 5) & 1;
        target = 0;
        break;
    default:
        return;
    }
    env_tick(&v->envx, &v->env_counter, rate, mask, dec, expo, false);
    if (v->phase == PH_SUSTAIN) return;
    bool reached = dec ? (v->envx <= target) : (v->envx >= target);
    if (reached) {
        v->env_counter = 0;
        switch (v->phase) {
        case PH_ATTACK: v->phase = PH_DECAY; break;
        case PH_DECAY: v->phase = PH_SUSTAIN; break;
        case PH_RELEASE: v->phase = PH_OFF; v->envx = 0; break;
        }
    }
}

/* ---- Voices ------------------------------------------------------------- */

static void voice_decode_block(Voice *v) {
    u32 a = v->addr & 0x7fff0u;
    v->addr = a;
    irq_check_range(a, 16);
    u8 hdr = ram[a], flags = ram[a + 1];
    int shift = hdr & 0xf;
    if (shift > 12) shift = 9;
    int filt = (hdr >> 4) & 7;
    if (filt > 4) filt = 4;
    s32 f0 = adpcm_pos[filt], f1 = adpcm_neg[filt];

    v->buf[0] = v->buf[28];
    v->buf[1] = v->buf[29];
    v->buf[2] = v->buf[30];
    s32 old = v->old, older = v->older;
    for (int i = 0; i < 28; i++) {
        u8 b = ram[a + 2 + (i >> 1)];
        int nib = (i & 1) ? (b >> 4) : (b & 0xf);
        s32 t = (s32)(s16)(u16)(nib << 12) >> shift;
        s32 s = clamp16(t + ((old * f0 + older * f1 + 32) >> 6));
        v->buf[3 + i] = (s16)s;
        older = old;
        old = s;
    }
    v->old = old;
    v->older = older;
    v->flags = flags;
    if ((flags & 4) && !v->ignore_loop) v->repeat = (u16)(a >> 3);
    v->has_samples = true;
}

static void key_on(int vi) {
    Voice *v = &voices[vi];
    v->addr = ((u32)REG(vi * 16 + 6) << 3) & 0x7fff0u;
    v->counter = 0;
    v->envx = 0;
    v->env_counter = 0;
    v->phase = PH_ATTACK;
    memset(v->buf, 0, sizeof v->buf);
    v->old = v->older = 0;
    v->has_samples = false;
    v->ignore_loop = false;
    endx &= ~(1u << vi);
}

static void key_off(int vi) {
    Voice *v = &voices[vi];
    if (v->phase == PH_OFF || v->phase == PH_RELEASE) return;
    v->phase = PH_RELEASE;
    v->env_counter = 0;
}

static inline u32 voice_bits(u32 lo_off) {
    return REG(lo_off) | ((u32)(REG(lo_off + 2) & 0xff) << 16);
}

static void voice_tick(int vi, u32 pmon, u32 non, s32 *ol, s32 *orr) {
    Voice *v = &voices[vi];
    if (v->phase == PH_OFF && !irq_enabled()) {
        v->out = 0;
        *ol = *orr = 0;
        sweep_tick(&v->vol[0]);
        sweep_tick(&v->vol[1]);
        return;
    }
    if (!v->has_samples) voice_decode_block(v);

    bool noise = (non >> vi) & 1;
    s32 s;
    if (noise) {
        s = (s16)noise_level;
    } else {
        u32 idx = v->counter >> 12;
        u32 i = (v->counter >> 4) & 0xff;
        const s16 *p = &v->buf[idx];
        s = (gauss[0x0ff - i] * p[0]) >> 15;
        s += (gauss[0x1ff - i] * p[1]) >> 15;
        s += (gauss[0x100 + i] * p[2]) >> 15;
        s += (gauss[0x000 + i] * p[3]) >> 15;
    }
    s32 out = (s * v->envx) >> 15;
    v->out = (s16)clamp16(out);

    if (v->phase != PH_OFF) adsr_tick(v, vi);

    u32 step = REG(vi * 16 + 4);
    if (vi > 0 && ((pmon >> vi) & 1)) {
        s32 factor = (s32)voices[vi - 1].out + 0x8000;
        s32 st = (s32)(s16)(u16)step;
        step = (u32)((st * factor) >> 15) & 0xffff;
    }
    if (step > 0x3fff) step = 0x4000;
    v->counter += step;

    if ((v->counter >> 12) >= 28) {
        v->counter -= 28u << 12;
        v->has_samples = false;
        if (v->flags & 1) {
            endx |= 1u << vi;
            v->addr = ((u32)v->repeat << 3) & 0x7fff0u;
            if (!(v->flags & 2) && !noise) {
                v->phase = PH_OFF;
                v->envx = 0;
            }
        } else {
            v->addr = (v->addr + 16) & SPU_RAM_MASK;
        }
    }

    *ol = (v->out * v->vol[0].level) >> 15;
    *orr = (v->out * v->vol[1].level) >> 15;
    sweep_tick(&v->vol[0]);
    sweep_tick(&v->vol[1]);
}

static void noise_tick(void) {
    int shift = (spucnt >> 10) & 0xf;
    int step = ((spucnt >> 8) & 3) + 4;
    noise_timer -= step;
    u16 n = noise_level;
    int parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1) & 1;
    if (noise_timer < 0) {
        noise_level = (u16)((n << 1) | parity);
        noise_timer += 0x20000 >> shift;
        if (noise_timer < 0) noise_timer += 0x20000 >> shift;
    }
}

/* ---- Reverb ------------------------------------------------------------- */

static u32 rv_addr(s32 rel) {
    s32 size = (s32)(0x40000u - rv_base);
    if (size <= 0) size = 1;
    s32 off = ((s32)rv_cur - (s32)rv_base + rel) % size;
    if (off < 0) off += size;
    return rv_base + (u32)off;
}

static inline s32 rv_rd(s32 rel) {
    return (s16)ram_r16(rv_addr(rel) << 1);
}

static inline void rv_wr(s32 rel, s32 v) {
    u32 a = rv_addr(rel) << 1;
    irq_check_range(a, 2);
    ram_w16(a, (u16)clamp16(v));
}

#define RVREG(off) ((s32)(s16)REG(off))
#define RVADR(off) ((s32)REG(off) * 4)

static void reverb_core(s32 inl, s32 inr, s32 *outl, s32 *outr) {
    const s32 dAPF1 = RVADR(0x1c0), dAPF2 = RVADR(0x1c2);
    const s32 vIIR = RVREG(0x1c4);
    const s32 vCOMB1 = RVREG(0x1c6), vCOMB2 = RVREG(0x1c8);
    const s32 vCOMB3 = RVREG(0x1ca), vCOMB4 = RVREG(0x1cc);
    const s32 vWALL = RVREG(0x1ce), vAPF1 = RVREG(0x1d0), vAPF2 = RVREG(0x1d2);
    const s32 mLSAME = RVADR(0x1d4), mRSAME = RVADR(0x1d6);
    const s32 mLCOMB1 = RVADR(0x1d8), mRCOMB1 = RVADR(0x1da);
    const s32 mLCOMB2 = RVADR(0x1dc), mRCOMB2 = RVADR(0x1de);
    const s32 dLSAME = RVADR(0x1e0), dRSAME = RVADR(0x1e2);
    const s32 mLDIFF = RVADR(0x1e4), mRDIFF = RVADR(0x1e6);
    const s32 mLCOMB3 = RVADR(0x1e8), mRCOMB3 = RVADR(0x1ea);
    const s32 mLCOMB4 = RVADR(0x1ec), mRCOMB4 = RVADR(0x1ee);
    const s32 dLDIFF = RVADR(0x1f0), dRDIFF = RVADR(0x1f2);
    const s32 mLAPF1 = RVADR(0x1f4), mRAPF1 = RVADR(0x1f6);
    const s32 mLAPF2 = RVADR(0x1f8), mRAPF2 = RVADR(0x1fa);
    const s32 vLIN = RVREG(0x1fc), vRIN = RVREG(0x1fe);
    const bool wr = (spucnt & 0x80) != 0;

    if (wr) {
        s32 lin = (vLIN * inl) >> 15;
        s32 rin = (vRIN * inr) >> 15;
        s32 p, v;
        /* same side reflection */
        p = rv_rd(mLSAME - 1);
        v = ((((lin + ((rv_rd(dLSAME) * vWALL) >> 15)) - p) * vIIR) >> 15) + p;
        rv_wr(mLSAME, v);
        p = rv_rd(mRSAME - 1);
        v = ((((rin + ((rv_rd(dRSAME) * vWALL) >> 15)) - p) * vIIR) >> 15) + p;
        rv_wr(mRSAME, v);
        /* different side reflection */
        p = rv_rd(mLDIFF - 1);
        v = ((((lin + ((rv_rd(dRDIFF) * vWALL) >> 15)) - p) * vIIR) >> 15) + p;
        rv_wr(mLDIFF, v);
        p = rv_rd(mRDIFF - 1);
        v = ((((rin + ((rv_rd(dLDIFF) * vWALL) >> 15)) - p) * vIIR) >> 15) + p;
        rv_wr(mRDIFF, v);
    }

    /* early echo (comb) */
    s32 lout = (vCOMB1 * rv_rd(mLCOMB1) + vCOMB2 * rv_rd(mLCOMB2) +
                vCOMB3 * rv_rd(mLCOMB3) + vCOMB4 * rv_rd(mLCOMB4)) >> 15;
    s32 rout = (vCOMB1 * rv_rd(mRCOMB1) + vCOMB2 * rv_rd(mRCOMB2) +
                vCOMB3 * rv_rd(mRCOMB3) + vCOMB4 * rv_rd(mRCOMB4)) >> 15;
    lout = clamp16(lout);
    rout = clamp16(rout);

    /* all-pass 1 */
    s32 t;
    t = rv_rd(mLAPF1 - dAPF1);
    lout = clamp16(lout - ((vAPF1 * t) >> 15));
    if (wr) rv_wr(mLAPF1, lout);
    lout = clamp16(((lout * vAPF1) >> 15) + t);
    t = rv_rd(mRAPF1 - dAPF1);
    rout = clamp16(rout - ((vAPF1 * t) >> 15));
    if (wr) rv_wr(mRAPF1, rout);
    rout = clamp16(((rout * vAPF1) >> 15) + t);

    /* all-pass 2 */
    t = rv_rd(mLAPF2 - dAPF2);
    lout = clamp16(lout - ((vAPF2 * t) >> 15));
    if (wr) rv_wr(mLAPF2, lout);
    lout = clamp16(((lout * vAPF2) >> 15) + t);
    t = rv_rd(mRAPF2 - dAPF2);
    rout = clamp16(rout - ((vAPF2 * t) >> 15));
    if (wr) rv_wr(mRAPF2, rout);
    rout = clamp16(((rout * vAPF2) >> 15) + t);

    *outl = (lout * RVREG(0x184)) >> 15;
    *outr = (rout * RVREG(0x186)) >> 15;

    rv_cur++;
    if (rv_cur > 0x3ffff) rv_cur = rv_base;
}

static s32 fir39(const s16 *h, u32 pos) {
    s32 sum = 0;
    for (int k = 0; k < 39; k++) sum += rv_fir[k] * h[(pos - (u32)k) & 63];
    return sum;
}

/* One 44.1kHz reverb step: decimate input, run core at 22.05kHz,
 * zero-stuff + FIR interpolate the output. */
static void reverb_tick(s32 inl, s32 inr, s32 *outl, s32 *outr) {
    u32 p = rv_pos & 63;
    rv_in_hist[0][p] = (s16)clamp16(inl);
    rv_in_hist[1][p] = (s16)clamp16(inr);
    s32 zl = 0, zr = 0;
    if (rv_pos & 1) {
        s32 dl = clamp16(fir39(rv_in_hist[0], rv_pos) >> 15);
        s32 dr = clamp16(fir39(rv_in_hist[1], rv_pos) >> 15);
        reverb_core(dl, dr, &zl, &zr);
    }
    rv_out_hist[0][p] = (s16)clamp16(zl);
    rv_out_hist[1][p] = (s16)clamp16(zr);
    *outl = clamp16(fir39(rv_out_hist[0], rv_pos) >> 14);
    *outr = clamp16(fir39(rv_out_hist[1], rv_pos) >> 14);
    rv_pos++;
}

/* ---- Mixer -------------------------------------------------------------- */

static void capture_write(u32 base, s32 v) {
    u32 a = base + capture_pos * 2;
    ram_w16(a, (u16)(s16)clamp16(v));
    if (REG(0x1ac) & 0xc) irq_check_range(a, 2);
}

static void tick(s16 *out) {
    s32 l = 0, r = 0, rl = 0, rr = 0;
    noise_tick();

    if (spucnt & 0x8000) {
        u32 pmon = voice_bits(0x190), non = voice_bits(0x194);
        u32 eon = voice_bits(0x198);
        for (int vi = 0; vi < NUM_VOICES; vi++) {
            s32 vl, vr;
            voice_tick(vi, pmon, non, &vl, &vr);
            l += vl;
            r += vr;
            if ((eon >> vi) & 1) { rl += vl; rr += vr; }
        }
    } else {
        for (int vi = 0; vi < NUM_VOICES; vi++) voices[vi].out = 0;
    }
    if (!(spucnt & 0x4000)) l = r = 0; /* mute (CD audio unaffected) */

    s32 cl = 0, cr = 0;
    if (cd_wr != cd_rd) {
        u32 i = (cd_rd & (CD_BUF_FRAMES - 1)) * 2;
        cl = cd_buf[i];
        cr = cd_buf[i + 1];
        cd_rd++;
    }
    if (spucnt & 1) {
        s32 vl = (cl * (s16)REG(0x1b0)) >> 15;
        s32 vr = (cr * (s16)REG(0x1b2)) >> 15;
        l += vl;
        r += vr;
        if (spucnt & 4) { rl += vl; rr += vr; }
    }

    s32 ol, orr;
    reverb_tick(rl, rr, &ol, &orr);
    l += ol;
    r += orr;

    out[0] = (s16)clamp16((clamp16(l) * main_vol[0].level) >> 15);
    out[1] = (s16)clamp16((clamp16(r) * main_vol[1].level) >> 15);

    capture_write(0x000, cl);
    capture_write(0x400, cr);
    capture_write(0x800, voices[1].out);
    capture_write(0xc00, voices[3].out);
    capture_pos = (capture_pos + 1) & 0x1ff;

    sweep_tick(&main_vol[0]);
    sweep_tick(&main_vol[1]);
}

/* ---- Public API --------------------------------------------------------- */

const u8 *spu_ram_ptr(size_t *n) { *n = sizeof ram; return ram; }

void spu_init(void) {
    memset(ram, 0, sizeof ram);
    memset(regs, 0, sizeof regs);
    memset(voices, 0, sizeof voices);
    memset(main_vol, 0, sizeof main_vol);
    spucnt = 0;
    stat_irq = 0;
    endx = 0;
    xfer_addr = 0;
    noise_level = 0;
    noise_timer = 0;
    capture_pos = 0;
    rv_base = rv_cur = 0;
    memset(rv_in_hist, 0, sizeof rv_in_hist);
    memset(rv_out_hist, 0, sizeof rv_out_hist);
    rv_pos = 0;
    cd_rd = cd_wr = 0;
}

static u16 spu_stat(void) {
    u16 s = (u16)(spucnt & 0x3f) | stat_irq;
    u32 mode = (spucnt >> 4) & 3;
    if (spucnt & 0x20) s |= 0x80;
    if (mode == 2) s |= 0x100;
    if (mode == 3) s |= 0x200;
    if (capture_pos & 0x100) s |= 0x800;
    return s;
}

u16 spu_read16(u32 offset) {
    offset &= 0x3fe;
    if (offset < 0x180) {
        Voice *v = &voices[offset >> 4];
        switch (offset & 0xf) {
        case 0xc: return (u16)v->envx;
        case 0xe: return v->repeat;
        default: return REG(offset);
        }
    }
    if (offset >= 0x200 && offset < 0x260) {
        Voice *v = &voices[(offset - 0x200) >> 2];
        return (u16)v->vol[(offset >> 1) & 1].level;
    }
    switch (offset) {
    case 0x19c: return (u16)endx;
    case 0x19e: return (u16)(endx >> 16);
    case 0x1aa: return spucnt;
    case 0x1ae: return spu_stat();
    case 0x1b8: return (u16)main_vol[0].level;
    case 0x1ba: return (u16)main_vol[1].level;
    default: return REG(offset);
    }
}

void spu_write16(u32 offset, u16 v) {
    offset &= 0x3fe;
    REG(offset) = v;
    if (offset < 0x180) {
        int vi = (int)(offset >> 4);
        Voice *vo = &voices[vi];
        switch (offset & 0xf) {
        case 0x0: sweep_set(&vo->vol[0], v); break;
        case 0x2: sweep_set(&vo->vol[1], v); break;
        case 0xc: vo->envx = (s16)v; break;
        case 0xe: vo->repeat = v; vo->ignore_loop = true; break;
        default: break;
        }
        return;
    }
    if (offset >= 0x200 && offset < 0x260) {
        Voice *vo = &voices[(offset - 0x200) >> 2];
        vo->vol[(offset >> 1) & 1].level = (s16)v;
        return;
    }
    switch (offset) {
    case 0x180: sweep_set(&main_vol[0], v); break;
    case 0x182: sweep_set(&main_vol[1], v); break;
    case 0x188:
    case 0x18a: {
        u32 bits = (offset == 0x188) ? v : ((u32)(v & 0xff) << 16);
        for (int i = 0; i < NUM_VOICES; i++)
            if ((bits >> i) & 1) key_on(i);
        break;
    }
    case 0x18c:
    case 0x18e: {
        u32 bits = (offset == 0x18c) ? v : ((u32)(v & 0xff) << 16);
        for (int i = 0; i < NUM_VOICES; i++)
            if ((bits >> i) & 1) key_off(i);
        break;
    }
    case 0x1a2:
        rv_base = (u32)v << 2;
        rv_cur = rv_base;
        break;
    case 0x1a6:
        xfer_addr = ((u32)v << 3) & SPU_RAM_MASK;
        break;
    case 0x1a8:
        irq_check_range(xfer_addr, 2);
        ram_w16(xfer_addr, v);
        xfer_addr = (xfer_addr + 2) & SPU_RAM_MASK;
        break;
    case 0x1aa:
        spucnt = v;
        if (!(v & 0x40)) stat_irq &= ~0x40; /* acknowledge */
        break;
    default:
        break;
    }
}

void spu_dma_write(const u16 *src, u32 halfwords) {
    for (u32 i = 0; i < halfwords; i++) {
        irq_check_range(xfer_addr, 2);
        ram_w16(xfer_addr, src[i]);
        xfer_addr = (xfer_addr + 2) & SPU_RAM_MASK;
    }
}

void spu_dma_read(u16 *dst, u32 halfwords) {
    for (u32 i = 0; i < halfwords; i++) {
        irq_check_range(xfer_addr, 2);
        dst[i] = ram_r16(xfer_addr);
        xfer_addr = (xfer_addr + 2) & SPU_RAM_MASK;
    }
}

void spu_render(s16 *out, int frames) {
    for (int i = 0; i < frames; i++) tick(out + i * 2);
}

void spu_push_cd_audio(const s16 *stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        if (cd_wr - cd_rd >= CD_BUF_FRAMES) break; /* full: drop the rest */
        u32 j = (cd_wr & (CD_BUF_FRAMES - 1)) * 2;
        cd_buf[j] = stereo[i * 2];
        cd_buf[j + 1] = stereo[i * 2 + 1];
        cd_wr++;
    }
}
