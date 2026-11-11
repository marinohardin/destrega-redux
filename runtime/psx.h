/* Shared runtime interface for the Destrega static recompilation.
 *
 * Generated code (gen/) and every hardware module include this header.
 * Hardware modules are emulated at register level (psx-spx semantics);
 * the CPU itself is replaced by recompiled C plus an interpreter fallback.
 */
#ifndef PSX_H
#define PSX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

/* ---- CPU state ---------------------------------------------------------- */

typedef struct {
    u32 r[32];
    u32 hi, lo;
    u32 pc;          /* only meaningful in the interpreter / for diagnostics */
    u32 cop0[32];    /* SR=12, CAUSE=13, EPC=14 */
    u64 cycles;      /* elapsed CPU cycles (approximate) */
    u64 next_event;  /* cycles value at which rt_event() must run */
} Cpu;

extern Cpu g_cpu;

#define COP0_SR 12
#define COP0_CAUSE 13
#define COP0_EPC 14

/* ---- Memory ------------------------------------------------------------- */

#define RAM_SIZE (2 * 1024 * 1024)
extern u8 g_ram[RAM_SIZE];
extern u8 g_scratch[1024];

u32 bus_read8(u32 addr);
u32 bus_read16(u32 addr);
u32 bus_read32(u32 addr);
void bus_write8(u32 addr, u32 v);
void bus_write16(u32 addr, u32 v);
void bus_write32(u32 addr, u32 v);

/* Write journal (widescreen shadow pass, overrides/widescreen.c). Stores take the RAM fast path
 * only below g_mem_wlimit: normally 0x800000 (all of RAM and its mirrors), 0 while journalling, so
 * every RAM store then reaches bus_write*, which saves the old contents of each 256-byte block on
 * its first write (mem_journal_note). The pass is undone without copying all of RAM, and the fast
 * path costs one load instead of an immediate. */
#define MEM_JOURNAL_SHIFT 8
#define MEM_JOURNAL_BLOCKS (RAM_SIZE >> MEM_JOURNAL_SHIFT)
extern u32 g_mem_wlimit;
bool mem_journal_begin(void);    /* bus.c: start journalling (false: out of memory) */
u32 mem_journal_undo(void);      /* restore every journalled block, stop; returns the block count */

static inline u32 mem_r32(u32 a) {
    u32 p = a & 0x1fffffff;
    if (p < 0x800000) return *(u32 *)(g_ram + (p & 0x1ffffc));
    if ((p & 0xfffffc00) == 0x1f800000) return *(u32 *)(g_scratch + (p & 0x3fc));
    return bus_read32(a);
}
static inline u32 mem_r16(u32 a) {
    u32 p = a & 0x1fffffff;
    if (p < 0x800000) return *(u16 *)(g_ram + (p & 0x1ffffe));
    if ((p & 0xfffffc00) == 0x1f800000) return *(u16 *)(g_scratch + (p & 0x3fe));
    return bus_read16(a);
}
static inline u32 mem_r8(u32 a) {
    u32 p = a & 0x1fffffff;
    if (p < 0x800000) return g_ram[p & 0x1fffff];
    if ((p & 0xfffffc00) == 0x1f800000) return g_scratch[p & 0x3ff];
    return bus_read8(a);
}
static inline void mem_w32(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    if (p < g_mem_wlimit) { *(u32 *)(g_ram + (p & 0x1ffffc)) = v; return; }
    if ((p & 0xfffffc00) == 0x1f800000) { *(u32 *)(g_scratch + (p & 0x3fc)) = v; return; }
    bus_write32(a, v);
}
static inline void mem_w16(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    if (p < g_mem_wlimit) { *(u16 *)(g_ram + (p & 0x1ffffe)) = (u16)v; return; }
    if ((p & 0xfffffc00) == 0x1f800000) { *(u16 *)(g_scratch + (p & 0x3fe)) = (u16)v; return; }
    bus_write16(a, v);
}
static inline void mem_w8(u32 a, u32 v) {
    u32 p = a & 0x1fffffff;
    if (p < g_mem_wlimit) { g_ram[p & 0x1fffff] = (u8)v; return; }
    if ((p & 0xfffffc00) == 0x1f800000) { g_scratch[p & 0x3ff] = (u8)v; return; }
    bus_write8(a, v);
}

/* Host pointer into RAM for a KSEG/KUSEG address (DMA helpers). */
static inline u8 *ram_ptr(u32 a) { return g_ram + (a & 0x1fffff); }

/* ---- Interrupts --------------------------------------------------------- */

enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3,
    IRQ_TIMER0 = 4, IRQ_TIMER1 = 5, IRQ_TIMER2 = 6, IRQ_SIO0 = 7,
    IRQ_SIO = 8, IRQ_SPU = 9, IRQ_PIO = 10,
};
void irq_raise(int bit);

/* ---- Scheduling ---------------------------------------------------------- */

#define PSX_CPU_HZ 33868800
/* Schedule `cb` to run once `delay` CPU cycles from now (one slot per id). */
enum { EVT_VBLANK, EVT_HBLANK, EVT_CDROM, EVT_CDROM_READ, EVT_DMA, EVT_SPU,
       EVT_TIMER, EVT_SIO, EVT_PROFILE, EVT_COUNT };
void sched_event(int id, u64 delay, void (*cb)(void));
void sched_cancel(int id);

/* ---- GTE (runtime/gte.c) ------------------------------------------------- */

void gte_init(void);
void gte_command(u32 instr);           /* COP2 imm25 operation */
u32 gte_read_data(int reg);           /* mfc2 / swc2 */
void gte_write_data(int reg, u32 v);  /* mtc2 / lwc2 */
u32 gte_read_ctrl(int reg);           /* cfc2 */
typedef struct { u32 d[32], c[32], flag; } GteState;
void gte_save(GteState *s);              /* snapshot of all GTE registers */
void gte_restore(const GteState *s);
void gte_write_ctrl(int reg, u32 v);  /* ctc2 */
/* The same operations on a caller-owned state (HLR audit; guest GTE and PGXP tags untouched,
 * PGXP production off). Not for generated code. */
void gte_exec_on(GteState *s, u32 cmd);
void gte_write_data_on(GteState *s, int reg, u32 v);
void gte_write_ctrl_on(GteState *s, int reg, u32 v);
u32 gte_read_data_on(GteState *s, int reg);
u32 gte_read_ctrl_on(GteState *s, int reg);

/* ---- GPU (runtime/gpu.c) ------------------------------------------------- */

void gpu_init(void);
void gpu_write_gp0(u32 v);
struct PgxpVert;
/* presentation-only packet (hi-res target only); pv: optional precise vertex per word (PGXP) */
void gpu_shadow_packet(const u32 *w, int n, const struct PgxpVert *const *pv);
/* The same for polygons that belong inside the 1x draw area (drawn into the hi-res target only,
 * clipped like the guest's own polygons): the character faces of overrides/char_mesh.c. */
void gpu_shadow_packet_in_area(const u32 *w, int n, const struct PgxpVert *const *pv);
/* GP0 word with its PGXP precision tag (runtime/pgxp.h; 0 = none). */
void gpu_write_gp0_tag(u32 v, u32 tag);
void gpu_write_gp1(u32 v);
u32 gpu_read_data(void);              /* GPUREAD 0x1f801810 */
u32 gpu_read_stat(void);              /* GPUSTAT 0x1f801814 */
void gpu_vblank(void);                /* called at the start of each vblank */
u16 *gpu_vram(void);                  /* 1024x512 16bpp: the emulated VRAM (hashed, saved) */
/* The 1x picture: the presentation-only display copy when enabled, else the emulated VRAM (which
 * lacks the pixels of primitives drawn inside the framebuffer region, see gpu.c). */
u16 *gpu_display_vram(void);
bool gpu_display_copy(void);
void gpu_enable_display_copy(void);   /* keep the full 1x picture from now on (never turned off) */
void gpu_set_skip_rule(bool on);      /* unit tests only: draw every primitive into the emulated VRAM */
/* Guest reads of skipped framebuffer pixels so far: kind 0 equal values, 1 differing, 2 unverified. */
u64 gpu_1x_observed(int kind);
/* Polygons, rects and lines processed so far (changes whenever anything is drawn). */
u64 gpu_prim_count(void);
void gpu_1x_report(void);
typedef struct {
    int x, y;          /* display area start in VRAM */
    int w, h;          /* resolution (e.g. 320x240) */
    bool rgb24;
    bool enabled;
} GpuDisplay;
void gpu_get_display(GpuDisplay *d);

/* ---- SPU (runtime/spu.c) ------------------------------------------------- */

void spu_init(void);
const u8 *spu_ram_ptr(size_t *n);    /* SPU RAM, for state hashing */
u16 spu_read16(u32 offset);            /* offset from 0x1f801c00 */
void spu_write16(u32 offset, u16 v);
void spu_dma_write(const u16 *src, u32 halfwords);
void spu_dma_read(u16 *dst, u32 halfwords);
/* Generate `frames` stereo samples at 44100Hz into out[frames*2]. */
void spu_render(s16 *out, int frames);
/* CD audio (XA / CD-DA) input at 44100Hz, already resampled, stereo. */
void spu_push_cd_audio(const s16 *stereo, int frames);

/* XA-ADPCM decoding (runtime/xa.c). One XaDecoder per stream; reset it when
 * the CD controller starts a new stream/seek. */
typedef struct {
    s32 prev[2][2];        /* ADPCM history per channel */
    s32 resample_hist[2][32];
    u32 resample_pos;
    double resample_frac;
} XaDecoder;
void xa_reset(XaDecoder *d);
/* Decode one raw 2352-byte Form-2 XA audio sector (sync+header+subheader
 * included). Output is interleaved stereo s16 at 44100Hz (mono is duplicated).
 * Returns number of stereo frames written (<= max_frames). */
int xa_decode_sector(XaDecoder *d, const u8 *sector, s16 *out, int max_frames);

/* ---- CD-ROM (runtime/cdrom.c) -------------------------------------------- */

u8 cdrom_read8(u32 offset);           /* offset 0..3 from 0x1f801800 */
void cdrom_write8(u32 offset, u8 v);
u32 cdrom_dma_read(u32 *dst, u32 words);

/* ---- Pads (BIOS HLE) ----------------------------------------------------- */

/* Active-low digital button mask in PSX order (bit0 = Select ... bit15 = Square). */
extern volatile u16 g_pad_buttons[2];

/* ---- Recompiled code dispatch -------------------------------------------- */

typedef void (*RecompFn)(void);
typedef struct {
    u32 addr;
    RecompFn fn;
    u32 overlay;   /* 0 = main executable, else overlay id (1-based) */
    u32 lo, hi;    /* original code range [lo, hi) covered by this function */
    u32 hash;      /* FNV-1a of the original bytes in [lo, hi) (overlay check) */
} RecompEntry;

#if defined(DESTREGA_GAME_MODULE) && !defined(DESTREGA_MODULE_SIDE)
/* module build (game_abi.h): the table lives in the game module, set by game_load.c */
extern const RecompEntry *g_recomp_table;
extern u32 g_recomp_count;
#else
extern const RecompEntry g_recomp_table[];
extern const u32 g_recomp_count;
#endif

typedef struct {
    const char *name;  /* disc path, e.g. "OVL/MDOIL.BIN" */
    u32 load_addr;     /* address the file image is copied to */
    u32 size;
} OverlayInfo;
#if defined(DESTREGA_GAME_MODULE) && !defined(DESTREGA_MODULE_SIDE)
extern const OverlayInfo *g_overlays;
extern u32 g_overlay_count;
#else
extern const OverlayInfo g_overlays[];
extern const u32 g_overlay_count;
#endif

/* Call the function at `addr` (sets nothing; caller sets $ra). */
void rt_call(u32 addr);
/* Indirect jump that isn't a call (tail jump); runs target then returns. */
void rt_jump(u32 addr);
void rt_syscall(u32 pc);
void rt_break(u32 pc, u32 code);
void rt_event(void);   /* hardware/IRQ service; invoked when cycles >= next_event */
void rt_mtc0(int reg, u32 v);
u32 rt_mfc0(int reg);
void rt_rfe(void);
void rt_invalid(u32 pc, u32 word);
void rt_unreached(u32 pc);

/* Interpreter fallback: execute from g_cpu.pc until it returns to `ret_addr`. */
void interp_run(u32 entry, u32 ret_addr);

/* Logging */
void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
extern int g_trace;
extern int g_no_interp;

#endif
