/* PS1 MDEC (Macroblock Decoder), register-level emulation per psx-spx.
 *
 * Input words (0x1f801820 writes / DMA0) are collected per command. Once a
 * command's parameter words have all arrived it is executed immediately; for
 * MDEC(1) the whole input is decoded eagerly into an output FIFO that
 * 0x1f801820 reads / DMA1 drain in any chunk size.
 *
 * Busy (status bit29) is only reported while parameter words are still
 * expected, so polling loops never deadlock regardless of whether the game
 * drains output before or after waiting for input completion.
 */
#include "mdec.h"
#include <string.h>

/* ---- tables ---------------------------------------------------------- */

static const u8 s_zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};
static u8 s_zagzig[64]; /* sequence index -> block position */

/* Standard PSX IDCT scale table (as uploaded by the BIOS/libpress). */
static const s16 s_default_scale[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, -0x18F8, -0x471C, -0x6A6D, -0x7D8A,
    0x7641, 0x30FB, -0x30FB, -0x7641, -0x7641, -0x30FB, 0x30FB, 0x7641,
    0x6A6D, -0x18F8, -0x7D8A, -0x471C, 0x471C, 0x7D8A, 0x18F8, -0x6A6D,
    0x5A82, -0x5A82, -0x5A82, 0x5A82, 0x5A82, -0x5A82, -0x5A82, 0x5A82,
    0x471C, -0x7D8A, 0x18F8, 0x6A6D, -0x6A6D, -0x18F8, 0x7D8A, -0x471C,
    0x30FB, -0x7641, 0x7641, -0x30FB, -0x30FB, 0x7641, -0x7641, 0x30FB,
    0x18F8, -0x471C, 0x6A6D, -0x7D8A, 0x7D8A, -0x6A6D, 0x471C, -0x18F8,
};

/* ---- state ----------------------------------------------------------- */

#define IN_MAX_WORDS  0x10000u
#define OUT_MAX_WORDS (1u << 20)

static u8  s_qt_y[64], s_qt_c[64];
static s16 s_scale[64];

static u32 s_cmd;            /* current/last command word */
static u32 s_in[IN_MAX_WORDS];
static u32 s_in_count;       /* parameter words received */
static u32 s_in_need;        /* parameter words expected (0 = idle) */
static int s_cur_block;      /* status bits 18-16 */
static u32 s_status_low;     /* status bits 15-0 when idle */

static u32 s_out[OUT_MAX_WORDS];
static u32 s_out_rd, s_out_wr;
static u8  s_out_bytes[4];   /* byte packer for 24bpp / 4bpp / 8bpp */
static int s_out_nbytes;

static bool s_en_in, s_en_out;

/* ---- output FIFO ----------------------------------------------------- */

static void out_word(u32 w)
{
    if (s_out_wr >= OUT_MAX_WORDS) {
        /* compact; if still full, drop oldest unread data */
        u32 n = s_out_wr - s_out_rd;
        if (n >= OUT_MAX_WORDS) { s_out_rd = s_out_wr = 0; n = 0; }
        else memmove(s_out, s_out + s_out_rd, n * sizeof(u32));
        s_out_rd = 0;
        s_out_wr = n;
    }
    s_out[s_out_wr++] = w;
}

static void out_byte(u8 b)
{
    s_out_bytes[s_out_nbytes++] = b;
    if (s_out_nbytes == 4) {
        out_word((u32)s_out_bytes[0] | ((u32)s_out_bytes[1] << 8) |
                 ((u32)s_out_bytes[2] << 16) | ((u32)s_out_bytes[3] << 24));
        s_out_nbytes = 0;
    }
}

/* ---- decode helpers --------------------------------------------------- */

static int sx10(u32 v) { v &= 0x3FF; return (v & 0x200) ? (int)v - 0x400 : (int)v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

typedef struct {
    const u32 *words;
    u32 nhalf;   /* total halfwords */
    u32 pos;     /* next halfword */
} HalfSrc;

static bool src_get(HalfSrc *s, u16 *out)
{
    if (s->pos >= s->nhalf) return false;
    u32 w = s->words[s->pos >> 1];
    *out = (u16)((s->pos & 1) ? (w >> 16) : (w & 0xFFFF));
    s->pos++;
    return true;
}

static s32 floor_shift13(s64 v) { return (s32)((v + 0x1000) >> 13); }

static void idct(s16 blk[64])
{
    s32 tmp[64];
    s32 src[64];
    for (int i = 0; i < 64; i++) src[i] = blk[i];
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                s64 sum = 0;
                for (int z = 0; z < 8; z++)
                    sum += (s64)src[y + z * 8] * (s64)(s_scale[x + z * 8] / 8);
                tmp[x + y * 8] = floor_shift13(sum);
            }
        }
        memcpy(src, tmp, sizeof(src));
    }
    for (int i = 0; i < 64; i++) blk[i] = (s16)clampi(src[i], -32768, 32767);
}

/* Returns false if input ran out before the block finished. */
static bool rl_decode_block(HalfSrc *s, s16 blk[64], const u8 *qt)
{
    u16 n;
    memset(blk, 0, 64 * sizeof(s16));
    do {
        if (!src_get(s, &n)) return false;
    } while (n == 0xFE00);

    int k = 0;
    int q_scale = (n >> 10) & 0x3F;
    int val = sx10(n) * qt[0];
    for (;;) {
        if (q_scale == 0) val = sx10(n) * 2;
        val = clampi(val, -0x400, 0x3FF);
        if (q_scale > 0) blk[s_zagzig[k]] = (s16)val;
        else blk[k] = (s16)val;
        if (!src_get(s, &n)) return false;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (sx10(n) * qt[k] * q_scale + 4) / 8;
    }
    idct(blk);
    return true;
}

/* depth: 2 = 24bpp, 3 = 15bpp */
static void emit_color_mb(const s16 *cr, const s16 *cb, s16 y[4][64], int depth,
                          bool is_signed, bool bit15)
{
    u32 rgb[256];
    for (int b = 0; b < 4; b++) {
        int xx = (b & 1) * 8, yy = (b >> 1) * 8;
        for (int py = 0; py < 8; py++) {
            for (int px = 0; px < 8; px++) {
                int ci = ((px + xx) >> 1) + ((py + yy) >> 1) * 8;
                int R = cr[ci], B = cb[ci];
                int G = (-3437 * B - 7143 * R) / 10000;
                R = (14020 * R) / 10000;
                B = (17720 * B) / 10000;
                int Y = y[b][px + py * 8];
                int r = clampi(Y + R, -128, 127);
                int g = clampi(Y + G, -128, 127);
                int bb = clampi(Y + B, -128, 127);
                u32 c = ((u32)(r & 0xFF)) | ((u32)(g & 0xFF) << 8) | ((u32)(bb & 0xFF) << 16);
                if (!is_signed) c ^= 0x808080;
                rgb[(px + xx) + (py + yy) * 16] = c;
            }
        }
    }
    if (depth == 2) {
        for (int i = 0; i < 256; i++) {
            out_byte((u8)rgb[i]);
            out_byte((u8)(rgb[i] >> 8));
            out_byte((u8)(rgb[i] >> 16));
        }
    } else {
        for (int i = 0; i < 256; i += 2) {
            u32 h[2];
            for (int j = 0; j < 2; j++) {
                u32 c = rgb[i + j];
                u32 r = (c & 0xFF) >> 3, g = ((c >> 8) & 0xFF) >> 3, b = ((c >> 16) & 0xFF) >> 3;
                h[j] = r | (g << 5) | (b << 10) | (bit15 ? 0x8000u : 0u);
            }
            out_word(h[0] | (h[1] << 16));
        }
    }
}

/* depth: 0 = 4bpp, 1 = 8bpp */
static void emit_mono_block(const s16 *yb, int depth, bool is_signed)
{
    u8 pix[64];
    for (int i = 0; i < 64; i++) {
        int v = yb[i] & 0x1FF;
        if (v & 0x100) v -= 0x200;
        v = clampi(v, -128, 127);
        u8 p = (u8)(v & 0xFF);
        if (!is_signed) p ^= 0x80;
        pix[i] = p;
    }
    if (depth == 1) {
        for (int i = 0; i < 64; i++) out_byte(pix[i]);
    } else {
        for (int i = 0; i < 64; i += 2)
            out_byte((u8)((pix[i] >> 4) | (pix[i + 1] & 0xF0)));
    }
}

static void decode_macroblocks(void)
{
    HalfSrc s = { s_in, s_in_count * 2, 0 };
    int depth = (int)((s_cmd >> 27) & 3);
    bool is_signed = (s_cmd >> 26) & 1;
    bool bit15 = (s_cmd >> 25) & 1;
    s16 cr[64], cb[64], y[4][64];

    s_out_nbytes = 0;
    if (depth >= 2) {
        for (;;) {
            s_cur_block = 4;
            if (!rl_decode_block(&s, cr, s_qt_c)) break;
            s_cur_block = 5;
            if (!rl_decode_block(&s, cb, s_qt_c)) break;
            bool ok = true;
            for (int b = 0; b < 4 && ok; b++) {
                s_cur_block = b;
                ok = rl_decode_block(&s, y[b], s_qt_y);
            }
            if (!ok) break;
            emit_color_mb(cr, cb, y, depth, is_signed, bit15);
        }
    } else {
        for (;;) {
            s_cur_block = 4;
            if (!rl_decode_block(&s, y[0], s_qt_y)) break;
            emit_mono_block(y[0], depth, is_signed);
        }
    }
    /* flush any partial word (only with odd-sized data) */
    while (s_out_nbytes) out_byte(0);
    s_cur_block = 4;
}

static void execute_command(void)
{
    u32 op = s_cmd >> 29;
    if (op == 1) {
        decode_macroblocks();
    } else if (op == 2) {
        u8 tmp[128] = {0};
        for (u32 i = 0; i < s_in_count && i < 32; i++) {
            tmp[i * 4 + 0] = (u8)s_in[i];
            tmp[i * 4 + 1] = (u8)(s_in[i] >> 8);
            tmp[i * 4 + 2] = (u8)(s_in[i] >> 16);
            tmp[i * 4 + 3] = (u8)(s_in[i] >> 24);
        }
        memcpy(s_qt_y, tmp, 64);
        if (s_cmd & 1) memcpy(s_qt_c, tmp + 64, 64);
    } else if (op == 3) {
        for (u32 i = 0; i < 32; i++) {
            s_scale[i * 2 + 0] = (s16)(s_in[i] & 0xFFFF);
            s_scale[i * 2 + 1] = (s16)(s_in[i] >> 16);
        }
    }
    s_in_need = 0;
    s_in_count = 0;
    s_status_low = 0xFFFF;
}

/* ---- public interface ------------------------------------------------ */

static void mdec_reset(void)
{
    s_cmd = 0;
    s_in_count = s_in_need = 0;
    s_out_rd = s_out_wr = 0;
    s_out_nbytes = 0;
    s_cur_block = 4;
    s_status_low = 0xFFFF;
}

void mdec_init(void)
{
    for (int i = 0; i < 64; i++) s_zagzig[s_zigzag[i]] = (u8)i;
    for (int i = 0; i < 64; i++) { s_qt_y[i] = 1; s_qt_c[i] = 1; }
    memcpy(s_scale, s_default_scale, sizeof(s_scale));
    s_en_in = s_en_out = false;
    mdec_reset();
}

void mdec_write_cmd(u32 v)
{
    if (s_in_need) {
        if (s_in_count < IN_MAX_WORDS) s_in[s_in_count] = v;
        s_in_count++;
        if (s_in_count >= s_in_need) execute_command();
        return;
    }
    s_cmd = v;
    s_in_count = 0;
    switch (v >> 29) {
    case 1: s_in_need = v & 0xFFFF; break;
    case 2: s_in_need = (v & 1) ? 32 : 16; break;
    case 3: s_in_need = 32; break;
    default:
        s_in_need = 0;
        s_status_low = v & 0xFFFF; /* psx-spx: reflected without "minus 1" */
        return;
    }
    if (s_in_need == 0) execute_command();
    else s_status_low = 0xFFFF;
}

void mdec_write_ctrl(u32 v)
{
    if (v & 0x80000000u) mdec_reset();
    s_en_in = (v >> 30) & 1;
    s_en_out = (v >> 29) & 1;
}

u32 mdec_read_data(void)
{
    if (s_out_rd < s_out_wr) {
        u32 w = s_out[s_out_rd++];
        if (s_out_rd == s_out_wr) s_out_rd = s_out_wr = 0;
        return w;
    }
    return 0;
}

u32 mdec_read_status(void)
{
    bool out_avail = s_out_rd < s_out_wr;
    u32 st = 0;
    if (!out_avail) st |= 1u << 31;
    /* bit30 data-in FIFO full: never, input is consumed immediately */
    if (s_in_need) st |= 1u << 29;
    if (s_en_in) st |= 1u << 28; /* always ready to accept input */
    if (s_en_out && out_avail) st |= 1u << 27;
    st |= ((s_cmd >> 25) & 0xF) << 23;
    st |= (u32)(s_cur_block & 7) << 16;
    if (s_in_need) st |= (s_in_need - s_in_count - 1) & 0xFFFF;
    else st |= s_status_low & 0xFFFF;
    return st;
}

/* ---- save states (savestate.h) ---------------------------------------------- */
#include "savestate.h"
void mdec_state_io(StateIO *io) {
    SS_VAR(io, s_qt_y); SS_VAR(io, s_qt_c); SS_VAR(io, s_scale);
    SS_VAR(io, s_cmd); SS_VAR(io, s_in_count); SS_VAR(io, s_in_need);
    SS_VAR(io, s_cur_block); SS_VAR(io, s_status_low);
    u32 nin = s_in_count < IN_MAX_WORDS ? s_in_count : IN_MAX_WORDS;   /* received parameter words */
    ss_io(io, s_in, (size_t)nin * 4);
    SS_VAR(io, s_out_rd); SS_VAR(io, s_out_wr);
    if (s_out_rd <= s_out_wr && s_out_wr <= OUT_MAX_WORDS)            /* unread output words */
        ss_io(io, s_out + s_out_rd, (size_t)(s_out_wr - s_out_rd) * 4);
    SS_VAR(io, s_out_bytes); SS_VAR(io, s_out_nbytes);
    SS_VAR(io, s_en_in); SS_VAR(io, s_en_out);
}
