/* SDL_GPU (Metal) high-resolution backend (see hires.h, AGENTS.md "GPU renderer").
 *
 * Implements HiresBackend with the same rules as the software rasterizer in hires.c, which
 * stays the reference: same coverage (the scaled vertices are shifted by half a pixel so the
 * GPU's pixel-centre sampling with the top-left fill rule equals the software rasterizer's
 * integer-position sampling with its ceil() rule), the same 1x attribute plane equations
 * evaluated per hi-res pixel in exact 64-bit integer arithmetic, textures/CLUTs fetched from a
 * copy of the 1x VRAM, the UV clamp to the range the 1x draw sampled (has_uvr) and the gouraud
 * colour clamp to the range the 1x draw produced (has_cr).
 *
 * Target: B5G5R5A1_UNORM, so every stored value is an exact PS1 15-bit colour plus the mask
 * bit (alpha), and the download is the PS1 pixel format as is (the shader writes PS1 red into
 * the format's low 5 bits). Semi-transparency uses fixed-function blending on that format:
 *   B+F: ONE/ONE add; B-F: reverse subtract; B+F/4: the shader outputs floor(F/4), add;
 *   B/2+F/2: the shader outputs F/2-1/4, dst factor 0.5 (constant); the 5-bit store rounds to
 *   nearest, which gives floor((B+F)/2) for F > 0. A channel with F = 0 over an odd B is a
 *   rounding tie and rounds to even (off by one when B/2 rounds up); mode 0 is not used by
 *   Destrega (counted in a fight).
 * Mask check uses the destination alpha in the blend equation (ONE_MINUS_DST_ALPHA), exact
 * for opaque, B+F, B-F and B+F/4; with B/2+F/2 the masked pixel is halved (not used by the game,
 * which never sets or checks the mask bit).
 *
 * Batching: every primitive carries its parameters in a storage-buffer record (GPrim), so
 * primitives with the same blend pipeline go into one draw call. A semi-transparent textured
 * primitive has an opaque part (texels without bit 15) and a blended part, drawn by two
 * pipelines. A "group" collects primitives per pipeline key and is emitted as one draw per
 * key in a fixed key order; a primitive that would be reordered against an earlier,
 * overlapping primitive of a later key (32x32 1x tile masks) closes the group first. So
 * command order is kept wherever pixels can overlap, with no framebuffer reads or render pass
 * breaks for blending. Render passes break only for VRAM-texture uploads (texture data
 * changed), CPU->VRAM uploads into the target (puts) and VRAM->VRAM copies (the source is
 * snapshotted with a texture copy).
 *
 * Vertices are float target coordinates plus a primitive index; per-vertex extras (PGXP W)
 * and a wider target (widescreen) only touch GVtx, the projection in vs_prim and configure().
 */
#include "hires_gpu.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512
#define FRAC 24

enum { K_TRI = 0, K_RECT, K_FILL, K_COPY, K_PUT, K_LINEPX, K_SEED };
#define F_TEX (1u << 4)
#define F_RAW (1u << 5)
#define F_SEMI (1u << 6)
#define F_GOUR (1u << 7)
#define F_DITHER (1u << 8)
#define F_DEPTH_SHIFT 9
#define F_BLEND_SHIFT 11
#define F_MASKOR (1u << 13)
#define F_C1X (1u << 14)
#define F_CLAMP (1u << 15)
#define F_FLIPX (1u << 16)
#define F_FLIPY (1u << 17)
#define F_MSET (1u << 18)
#define F_EDGE (1u << 19)       /* triangle drawn as its bounding box; the shader tests coverage */
#define F_LONGLEFT (1u << 20)

/* pipeline keys: 0 opaque, 1..4 semi-transparency mode 0..3; +5 = with mask check */
#define NKEYS 10

typedef struct { float x, y; u32 prim; } GVtx;
typedef struct {   /* must match struct Prim in the MSL source */
    u32 kf, tp, clut, tw, clip0, clip1, uvr, cmin, cmax, color, aux0, aux1;
    s32 qx[5], qy[5];
    u32 rxy[5], pad;
    s64 cint[5];
    u32 pad2[2];
    s32 ev[6];                  /* F_EDGE: sorted vertices a, b, c in hi-res 16.16 (x, y) */
    u32 pad3[2];
} GPrim;
_Static_assert(sizeof(GPrim) == 192, "GPrim must match the MSL struct Prim");

typedef struct { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; } FragU;

static const char s_msl[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Prim { uint kf, tp, clut, tw, clip0, clip1, uvr, cmin, cmax, color, aux0, aux1;\n"
"  int qx[5]; int qy[5]; uint rxy[5]; uint pad; long cint[5]; uint pad2[2]; int ev[6]; uint pad3[2]; };\n"
"struct VI { float2 pos [[attribute(0)]]; uint prim [[attribute(1)]]; };\n"
"struct VO { float4 pos [[position]]; uint prim [[flat]]; };\n"
"struct VU { float4 k; };\n"
"vertex VO vs_prim(VI in [[stage_in]], constant VU &u [[buffer(0)]]) {\n"
"  VO o; o.pos = float4(in.pos.x * u.k.x - 1.0, 1.0 - in.pos.y * u.k.y, 0.0, 1.0); o.prim = in.prim; return o; }\n"
"struct FU { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; };\n"
"constant int DITH[16] = { -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2 };\n"
"static inline long fdiv(long a, long b) { long q = a / b; if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1; return q; }\n"
/* floor of the 1x plane equation at hi-res pixel (n = 2X+1-Sx, m = 2Y+1-Sy), exactly:
 * E = C + gdx*n/(2Sx) + gdy*m/(2Sy) with C = cint + cfrac/65536, gdx = qx*2Sx + rx */
"static inline long attr(device const Prim &p, int i, long n, long m, long sx, long sy) {\n"
"  uint w = p.rxy[i]; long rx = long(w & 0xFFu), ry = long((w >> 8) & 0xFFu), cf = long(w >> 16);\n"
"  long F = cf * 4 * sx * sy + rx * n * 2 * sy * 65536 + ry * m * 2 * sx * 65536;\n"
"  return p.cint[i] + long(p.qx[i]) * n + long(p.qy[i]) * m + fdiv(F, 65536 * 4 * sx * sy); }\n"
"static inline uint vram_at(texture2d<ushort> t, uint x, uint y) { return uint(t.read(uint2(x & 1023u, y & 511u)).r); }\n"
"static inline uint tex_fetch(texture2d<ushort> vr, device const Prim &p, uint depth, uint u, uint v) {\n"
"  uint tw = p.tw;\n"
"  u = (u & (tw & 0xFFu)) | ((tw >> 8) & 0xFFu);\n"
"  v = (v & ((tw >> 16) & 0xFFu)) | (tw >> 24);\n"
"  uint tpx = p.tp & 0xFFFFu, tpy = p.tp >> 16, cx = p.clut & 0xFFFFu, cy = p.clut >> 16;\n"
"  uint row = (tpy + v) & 511u;\n"
"  if (depth == 0u) { uint w = vram_at(vr, tpx + (u >> 2), row); uint idx = (w >> ((u & 3u) * 4u)) & 0xFu; return uint(vr.read(uint2((cx + idx) & 1023u, cy)).r); }\n"
"  if (depth == 1u) { uint w = vram_at(vr, tpx + (u >> 1), row); uint idx = (w >> ((u & 1u) * 8u)) & 0xFFu; return uint(vr.read(uint2((cx + idx) & 1023u, cy)).r); }\n"
"  return vram_at(vr, tpx + u, row); }\n"
/* PS1 pixel -> B5G5R5A1 output: the format stores .b in the low 5 bits, so PS1 red goes to .b */
"static inline float4 pack(uint c, int pass, int mode) {\n"
"  float r = float(c & 31u), g = float((c >> 5) & 31u), b = float((c >> 10) & 31u);\n"
"  if (pass == 1) {\n"
"    if (mode == 0) { r = r * 0.5 - 0.25; g = g * 0.5 - 0.25; b = b * 0.5 - 0.25; }\n"
"    else if (mode == 3) { r = floor(r * 0.25); g = floor(g * 0.25); b = floor(b * 0.25); }\n"
"  }\n"
"  return float4(b / 31.0, g / 31.0, r / 31.0, float(c >> 15)); }\n"
"static inline uint unpack(float4 t) {\n"
"  return uint(rint(t.b * 31.0)) | (uint(rint(t.g * 31.0)) << 5) | (uint(rint(t.r * 31.0)) << 10) | (t.a > 0.5 ? 0x8000u : 0u); }\n"
"static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }\n"
/* hires.c's coverage rule, exactly: rows [ceil(Ya), ceil(Yc)), columns [ceil(xl), ceil(xr)) with
 * xl/xr on the long edge a-c and the short edge a-b (rows above Yb) or b-c; 16.16 hi-res coords */
"static inline bool edge_right_of(long X16, long Y16, long px, long py, long qx, long qy) {\n"
"  return (X16 - px) * (qy - py) >= (qx - px) * (Y16 - py); }\n"
"static inline bool covered(device const Prim &p, int X, int Y) {\n"
"  long X16 = long(X) * 65536, Y16 = long(Y) * 65536;\n"
"  long ax = p.ev[0], ay = p.ev[1], bx = p.ev[2], by = p.ev[3], cx = p.ev[4], cy = p.ev[5];\n"
"  if (Y16 < ay || Y16 >= cy) return false;\n"
"  long sx0, sy0, sx1, sy1;\n"
"  if (Y16 < by) { sx0 = ax; sy0 = ay; sx1 = bx; sy1 = by; } else { sx0 = bx; sy0 = by; sx1 = cx; sy1 = cy; }\n"
"  bool on_long = edge_right_of(X16, Y16, ax, ay, cx, cy), on_short = edge_right_of(X16, Y16, sx0, sy0, sx1, sy1);\n"
"  return ((p.kf & (1u << 20)) != 0u) ? (on_long && !on_short) : (on_short && !on_long); }\n"
/* first pixel of the span hires.c draws on row Y: ceil(left edge x), at least the clip */
"static inline long span_start(device const Prim &p, int Y, int cx1) {\n"
"  long Y16 = long(Y) * 65536;\n"
"  long ax = p.ev[0], ay = p.ev[1], bx = p.ev[2], by = p.ev[3], cx = p.ev[4], cy = p.ev[5];\n"
"  long px, py, qx, qy;\n"
"  if ((p.kf & (1u << 20)) != 0u) { px = ax; py = ay; qx = cx; qy = cy; }\n"
"  else if (Y16 < by) { px = ax; py = ay; qx = bx; qy = by; }\n"
"  else { px = bx; py = by; qx = cx; qy = cy; }\n"
"  long N = px * (qy - py) + (qx - px) * (Y16 - py), D = (qy - py) * 65536;\n"
"  long xs = -fdiv(-N, D);\n"
"  return xs < long(cx1) ? long(cx1) : xs; }\n"
/* hires.c's attribute value at X: the plane equation at the span start, then stepped by the
 * per-pixel gradient truncated to an integer (gdx / Sx) */
"static inline long attr_span(device const Prim &p, int i, long xs, long X, long m, long sx, long sy) {\n"
"  long gdx = long(p.qx[i]) * 2 * sx + long(p.rxy[i] & 0xFFu);\n"
"  return attr(p, i, 2 * xs + 1 - sx, m, sx, sy) + (X - xs) * (gdx / sx); }\n"
"fragment float4 fs_prim(VO in [[stage_in]], constant FU &u [[buffer(0)]], device const Prim *prims [[buffer(1)]],\n"
"    texture2d<float> tmp [[texture(0)]], texture2d<uint> putt [[texture(1)]], texture2d<ushort> vram [[texture(2)]]) {\n"
"  device const Prim &p = prims[in.prim];\n"
"  int X = int(in.pos.x) + u.ox, Y = int(in.pos.y) + u.oy;\n"
"  uint kf = p.kf, kind = kf & 15u;\n"
"  int x1 = X / u.sx, y1 = Y / u.sy;\n"
"  if (kind == 2u) { if (u.pass != 0) discard_fragment(); return pack(p.color & 0xFFFFu, 0, 0); }\n"
"  if (kind == 6u) return pack(vram_at(vram, uint(x1), uint(y1)), 0, 0);\n"
"  if (kind == 4u) {\n"
"    uint w = putt.read(uint2(uint(x1 - u.rx), uint(y1 - u.ry))).r;\n"
"    if ((w & 0x10000u) == 0u) discard_fragment();\n"
"    return pack((w & 0xFFFFu) | ((kf & (1u << 18)) ? 0x8000u : 0u), 0, 0); }\n"
"  if (kind == 3u) {\n"
"    int sb = X - x1 * u.sx, sa = Y - y1 * u.sy;\n"
"    int sx = int(p.aux0 & 0xFFFFu), sy = int(p.aux0 >> 16), dx = int(p.aux1 & 0xFFFFu), dy = int(p.aux1 >> 16);\n"
"    int scol = (sx + ((x1 - dx) & 1023)) & 1023, srow = (sy + ((y1 - dy) & 511)) & 511;\n"
"    uint c;\n"
"    if (scol >= u.rx && scol < u.rx + u.rw && srow >= u.ry && srow < u.ry + u.rh)\n"
"      c = unpack(tmp.read(uint2(uint(scol * u.sx + sb - u.ox), uint(srow * u.sy + sa - u.oy))));\n"
"    else c = vram_at(vram, uint(scol), uint(srow));\n"
"    return pack(c | ((kf & (1u << 18)) ? 0x8000u : 0u), 0, 0); }\n"
"  int cx1 = int(p.clip0 & 0xFFFFu), cy1 = int(p.clip0 >> 16), cx2 = int(p.clip1 & 0xFFFFu), cy2 = int(p.clip1 >> 16);\n"
"  if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard_fragment();\n"
"  bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u;\n"
"  bool gour = (kf & (1u << 7)) != 0u, dith = (kf & (1u << 8)) != 0u;\n"
"  uint depth = (kf >> 9) & 3u;\n"
"  int cr = int(p.color & 255u), cg = int((p.color >> 8) & 255u), cb = int((p.color >> 16) & 255u);\n"
"  uint uu = 0u, vv = 0u;\n"
"  if (kind == 0u) {\n"
"    if ((kf & (1u << 19)) != 0u && !covered(p, X, Y)) discard_fragment();\n"
"    long m = long(2 * Y + 1 - u.sy), xs = span_start(p, Y, cx1), lX = long(X), lsx = long(u.sx), lsy = long(u.sy);\n"
"    if (gour) {\n"
"      int c3[3];\n"
"      bool c1x = (kf & (1u << 14)) != 0u;\n"
"      for (int i = 0; i < 3; i++) {\n"
"        long val = attr_span(p, i, xs, lX, m, lsx, lsy);\n"
"        int lo = int((p.cmin >> (8 * i)) & 255u), hi = int((p.cmax >> (8 * i)) & 255u);\n"
"        long s = c1x ? long(uint(val) >> 24) : (val >> 24);\n"
"        c3[i] = s < long(lo) ? lo : (s > long(hi) ? hi : int(s));\n"
"      }\n"
"      cr = c3[0]; cg = c3[1]; cb = c3[2];\n"
"    }\n"
"    if (tex) {\n"
"      int ui = int(attr_span(p, 3, xs, lX, m, lsx, lsy) >> 24), vi = int(attr_span(p, 4, xs, lX, m, lsx, lsy) >> 24);\n"
"      if ((kf & (1u << 15)) != 0u) {\n"
"        int umin = int(p.uvr & 255u), umax = int((p.uvr >> 8) & 255u), vmin = int((p.uvr >> 16) & 255u), vmax = int(p.uvr >> 24);\n"
"        ui = ui < umin ? umin : (ui > umax ? umax : ui);\n"
"        vi = vi < vmin ? vmin : (vi > vmax ? vmax : vi);\n"
"      }\n"
"      uu = uint(ui); vv = uint(vi);\n"
"    }\n"
"  } else if (kind == 1u) {\n"
"    int x0 = int(p.aux0 << 16) >> 16, y0 = int(p.aux0) >> 16;\n"
"    uint u0 = p.aux1 & 255u, v0 = (p.aux1 >> 8) & 255u;\n"
"    bool fx = (kf & (1u << 16)) != 0u, fy = (kf & (1u << 17)) != 0u;\n"
"    if (tex && fx) u0 |= 1u;\n"
"    uu = (u0 + uint(fx ? x0 - x1 : x1 - x0)) & 255u;\n"
"    vv = (v0 + uint(fy ? y0 - y1 : y1 - y0)) & 255u;\n"
"  }\n"
"  int d = dith ? DITH[(y1 & 3) * 4 + (x1 & 3)] : 0;\n"
"  uint outc; bool blend;\n"
"  if (tex) {\n"
"    uint t = tex_fetch(vram, p, depth, uu, vv);\n"
"    if (t == 0u) discard_fragment();\n"
"    if (raw) outc = t;\n"
"    else {\n"
"      int tr = int(t & 31u), tg = int((t >> 5) & 31u), tb = int((t >> 10) & 31u);\n"
"      if (dith) { tr = clamp255(((tr * cr) >> 4) + d) >> 3; tg = clamp255(((tg * cg) >> 4) + d) >> 3; tb = clamp255(((tb * cb) >> 4) + d) >> 3; }\n"
"      else { tr = min((tr * cr) >> 7, 31); tg = min((tg * cg) >> 7, 31); tb = min((tb * cb) >> 7, 31); }\n"
"      outc = uint(tr) | (uint(tg) << 5) | (uint(tb) << 10) | (t & 0x8000u);\n"
"    }\n"
"    blend = semi && (t & 0x8000u) != 0u;\n"
"  } else {\n"
"    int r, g, b;\n"
"    if (dith) { r = clamp255(cr + d) >> 3; g = clamp255(cg + d) >> 3; b = clamp255(cb + d) >> 3; }\n"
"    else { r = cr >> 3; g = cg >> 3; b = cb >> 3; }\n"
"    outc = uint(r) | (uint(g) << 5) | (uint(b) << 10);\n"
"    blend = semi;\n"
"  }\n"
"  if (blend != (u.pass == 1)) discard_fragment();\n"
"  if ((kf & (1u << 13)) != 0u) outc |= 0x8000u;\n"
"  return pack(outc, u.pass, u.mode); }\n"
"struct PO { float4 pos [[position]]; };\n"
"vertex PO vs_full(uint vid [[vertex_id]]) {\n"
"  PO o; o.pos = float4(vid == 1u ? 3.0 : -1.0, vid == 2u ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
"struct PU { int x, y, p0, p1; };\n"
"fragment float4 fs_present(PO in [[stage_in]], constant PU &u [[buffer(0)]], texture2d<float> t [[texture(0)]]) {\n"
"  float4 v = t.read(uint2(uint(int(in.pos.x) + u.x), uint(int(in.pos.y) + u.y)));\n"
"  return float4(rint(v.b * 31.0) * 8.0 / 255.0, rint(v.g * 31.0) * 8.0 / 255.0, rint(v.r * 31.0) * 8.0 / 255.0, 1.0); }\n";

/* ---- small dynamic arrays --------------------------------------------------------- */

typedef struct { void *p; size_t n, cap, esz; } Vec;
static void *vec_push(Vec *v, size_t k)
{
    if (v->n + k > v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 1024;
        while (nc < v->n + k) nc *= 2;
        void *np = realloc(v->p, nc * v->esz);
        if (!np) { rt_log("hires-gpu: out of memory"); abort(); }
        v->p = np;
        v->cap = nc;
    }
    void *r = (char *)v->p + v->n * v->esz;
    v->n += k;
    return r;
}

enum { ST_DRAW, ST_VRAM_UP, ST_PUT_UP, ST_TCOPY };
typedef struct { int type, key; u32 first, count; int x, y, w, h; size_t off; } Step;

/* ---- state -------------------------------------------------------------------------- */

static SDL_GPUDevice *s_dev;
static SDL_GPUGraphicsPipeline *s_pipe[NKEYS], *s_present_pipe;
static SDL_GPUSampler *s_samp;
static SDL_GPUTexture *s_vramtex, *s_target, *s_temp, *s_puttex, *s_present;
static int s_pw, s_ph;
static SDL_GPUBuffer *s_vbuf, *s_pbuf;
static u32 s_vbuf_cap, s_pbuf_cap;
static SDL_GPUTransferBuffer *s_up, *s_down;
static u32 s_up_cap, s_down_cap;
static SDL_GPUFence *s_fences[2];
static int s_fence_i;
static bool s_inited, s_ready;

static const u16 *s_vram;
static int s_Sx, s_Sy, s_rx, s_ry, s_rw, s_rh, s_tw, s_th, s_ox, s_oy;
/* Allocated target size: the next powers of two above s_tw x s_th, so the pixel <-> NDC
 * transform is exact. With 480*Sy rows it rounds; vertices then land ~1e-5 px off pixel
 * centres, which Apple GPUs resolve, breaking fill-rule ties on shared edges. */
static int s_aw, s_ah;

static Vec s_verts = { .esz = sizeof(GVtx) }, s_prims = { .esz = sizeof(GPrim) },
           s_steps = { .esz = sizeof(Step) }, s_arena = { .esz = 1 };
static Vec s_gv[NKEYS];
static u64 s_gm[NKEYS][8];
static u32 s_gused;

static u64 s_dirty[32];    /* 1x VRAM blocks (16x16) changed since uploaded to s_vramtex */

static u32 *s_put;         /* CPU->VRAM pixels pending for the target (region coords), bit 16 = set */
static int s_put_n, s_pbx0, s_pby0, s_pbx1, s_pby1;
static bool s_put_mc, s_put_ms;

static u16 *s_rb;          /* readback pixels */
static size_t s_rb_cap;
static bool s_dirty_present = true;

static HiresGpuStats s_st;
void hires_gpu_get_stats(HiresGpuStats *st) { *st = s_st; }

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline s64 floor_div64(s64 a, s64 b)
{
    s64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* ---- VRAM texture dirty tracking ---------------------------------------------------- */

static u64 block_cols(int x, int w)
{
    if (w >= VRAM_W) return ~0ull;
    int b0 = (x & 1023) >> 4, b1 = ((x & 1023) + w - 1) >> 4;
    if (b1 - b0 >= 63) return ~0ull;
    u64 m = 0;
    for (int b = b0; b <= b1; b++) m |= 1ull << (b & 63);
    return m;
}

static void mark_vram(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    u64 cm = block_cols(x, w);
    int by0 = (y & 511) >> 4, by1 = ((y & 511) + imin(h, VRAM_H) - 1) >> 4;
    if (by1 - by0 >= 31) { by0 = 0; by1 = 31; }
    for (int by = by0; by <= by1; by++) s_dirty[by & 31] |= cm;
}

static size_t arena_alloc(size_t n)
{
    size_t off = (s_arena.n + 15) & ~(size_t)15;
    vec_push(&s_arena, off - s_arena.n + n);
    return off;
}

static Step *add_step(int type)
{
    Step *s = vec_push(&s_steps, 1);
    memset(s, 0, sizeof *s);
    s->type = type;
    return s;
}

static void close_group(void)
{
    if (!s_gused) return;
    for (int k = 0; k < NKEYS; k++) {
        if (!s_gv[k].n) continue;
        Step *st = add_step(ST_DRAW);
        st->key = k;
        st->first = (u32)s_verts.n;
        st->count = (u32)s_gv[k].n;
        memcpy(vec_push(&s_verts, s_gv[k].n), s_gv[k].p, s_gv[k].n * sizeof(GVtx));
        s_gv[k].n = 0;
    }
    memset(s_gm, 0, sizeof s_gm);
    s_gused = 0;
}

/* Make sure the VRAM texture holds the current 1x VRAM in this rectangle (wrapping). */
static void ensure_vram(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    u64 cm = block_cols(x, w);
    int by0 = (y & 511) >> 4, by1 = ((y & 511) + imin(h, VRAM_H) - 1) >> 4;
    if (by1 - by0 >= 31) { by0 = 0; by1 = 31; }
    bool any = false;
    for (int by = by0; by <= by1 && !any; by++) any = (s_dirty[by & 31] & cm) != 0;
    if (!any) return;
    close_group();   /* draws so far sampled the old contents */
    for (int by = by0; by <= by1; by++) {
        u64 m = s_dirty[by & 31] & cm;
        s_dirty[by & 31] &= ~m;
        while (m) {
            int b0 = __builtin_ctzll(m);
            u64 rest = ~(m >> b0);
            int n = rest ? __builtin_ctzll(rest) : 64 - b0;
            if (b0 + n > 64) n = 64 - b0;
            m &= ~((n >= 64 ? ~0ull : ((1ull << n) - 1)) << b0);
            int rx = b0 * 16, ry = (by & 31) * 16, rw = n * 16, rh = 16;
            size_t off = arena_alloc((size_t)rw * rh * 2);
            u16 *dst = (u16 *)((u8 *)s_arena.p + off);
            for (int j = 0; j < rh; j++) memcpy(dst + (size_t)j * rw, s_vram + (size_t)(ry + j) * VRAM_W + rx, (size_t)rw * 2);
            Step *st = add_step(ST_VRAM_UP);
            st->x = rx; st->y = ry; st->w = rw; st->h = rh; st->off = off;
            s_st.vram_up_texels += (u64)rw * rh;
        }
    }
}

/* VRAM area a textured primitive can sample: u/v range (after the texture window) on its page,
 * plus the CLUT. */
static void tex_need(const HiresPrim *p, int u0, int u1, int v0, int v1)
{
    if (!(p->tw_and_u == 0xFF && p->tw_or_u == 0)) { u0 = (int)p->tw_or_u; u1 = (int)(p->tw_or_u | p->tw_and_u); }
    if (!(p->tw_and_v == 0xFF && p->tw_or_v == 0)) { v0 = (int)p->tw_or_v; v1 = (int)(p->tw_or_v | p->tw_and_v); }
    const int sh = p->depth == 0 ? 2 : (p->depth == 1 ? 1 : 0);
    int x0 = (int)p->tp_x + (u0 >> sh), x1 = (int)p->tp_x + (u1 >> sh);
    ensure_vram(x0, (int)p->tp_y + v0, x1 - x0 + 1, v1 - v0 + 1);
    if (p->depth < 2) ensure_vram((int)p->clut_x, (int)p->clut_y, p->depth == 0 ? 16 : 256, 1);
}

/* ---- groups --------------------------------------------------------------------------- */

static void tile_mask(int x0, int y0, int x1, int y1, u64 m[8])
{
    memset(m, 0, 8 * sizeof(u64));
    x0 = imax(x0, 0); y0 = imax(y0, 0); x1 = imin(x1, VRAM_W - 1); y1 = imin(y1, VRAM_H - 1);
    if (x0 > x1 || y0 > y1) return;
    int c0 = x0 >> 5, c1 = x1 >> 5;
    u64 row = (c1 - c0 >= 31) ? 0xFFFFFFFFull : (((1ull << (c1 - c0 + 1)) - 1) << c0);
    for (int r = y0 >> 5; r <= (y1 >> 5); r++) m[r >> 1] |= row << ((r & 1) * 32);
}

/* Add a primitive's parts (pipeline keys) to the group. A group is emitted key by key, so a
 * part may not overtake an earlier overlapping part of a later key: close the group then. */
static void group_add(const int *keys, int nk, const GVtx *v, int nv, const u64 m[8])
{
    bool conflict = false;
    for (int i = 0; i < nk && !conflict; i++)
        for (int j = keys[i] + 1; j < NKEYS && !conflict; j++)
            if (s_gused & (1u << j))
                for (int w = 0; w < 8; w++)
                    if (s_gm[j][w] & m[w]) { conflict = true; break; }
    if (conflict) close_group();
    for (int i = 0; i < nk; i++) {
        int k = keys[i];
        if (!s_gv[k].esz) s_gv[k].esz = sizeof(GVtx);
        memcpy(vec_push(&s_gv[k], (size_t)nv), v, (size_t)nv * sizeof(GVtx));
        s_gused |= 1u << k;
        for (int w = 0; w < 8; w++) s_gm[k][w] |= m[w];
    }
}

static GPrim *new_prim(u32 *idx)
{
    *idx = (u32)s_prims.n;
    GPrim *g = vec_push(&s_prims, 1);
    memset(g, 0, sizeof *g);
    s_st.prims++;
    s_dirty_present = true;
    return g;
}

static int prim_keys(const HiresPrim *p, bool semi, int *keys)
{
    const int mc = p->mask_check ? 5 : 0;
    int n = 0;
    if (!semi) keys[n++] = mc;
    else {
        if (p->tex) keys[n++] = mc;          /* texels without bit 15 are opaque */
        keys[n++] = mc + 1 + p->blend;
    }
    return n;
}

static void quad(GVtx *v, float x0, float y0, float x1, float y1, u32 prim)
{
    const GVtx q[6] = { { x0, y0, prim }, { x1, y0, prim }, { x0, y1, prim },
                        { x1, y0, prim }, { x1, y1, prim }, { x0, y1, prim } };
    memcpy(v, q, sizeof q);
}

/* quad over the 1x rectangle [x0, x1) x [y0, y1) (inside the region) */
static void quad1x(GVtx *v, int x0, int y0, int x1, int y1, u32 prim)
{
    quad(v, (float)(x0 * s_Sx - s_ox), (float)(y0 * s_Sy - s_oy), (float)(x1 * s_Sx - s_ox),
         (float)(y1 * s_Sy - s_oy), prim);
}

static u32 base_flags(const HiresPrim *p, u32 kind, bool tex)
{
    u32 f = kind;
    if (tex) f |= F_TEX | ((u32)p->depth << F_DEPTH_SHIFT);
    if (tex && p->raw) f |= F_RAW;
    if (p->semi) f |= F_SEMI;
    f |= (u32)(p->blend & 3) << F_BLEND_SHIFT;
    if (p->mask_or) f |= F_MASKOR;
    return f;
}

static void set_tex(GPrim *g, const HiresPrim *p)
{
    g->tp = p->tp_x | (p->tp_y << 16);
    g->clut = p->clut_x | (p->clut_y << 16);
    g->tw = (p->tw_and_u & 0xFF) | ((p->tw_or_u & 0xFF) << 8) | ((p->tw_and_v & 0xFF) << 16) | ((p->tw_or_v & 0xFF) << 24);
}

static void set_clip(GPrim *g, const HiresPrim *p)
{
    int cx1 = imax(p->clip_x1 * s_Sx, s_ox), cy1 = imax(p->clip_y1 * s_Sy, s_oy);
    int cx2 = imin((p->clip_x2 + 1) * s_Sx, s_ox + s_tw), cy2 = imin((p->clip_y2 + 1) * s_Sy, s_oy + s_th);
    g->clip0 = (u32)imax(cx1, 0) | ((u32)imax(cy1, 0) << 16);
    g->clip1 = (u32)imax(cx2, 0) | ((u32)imax(cy2, 0) << 16);
}

static inline bool dither_on(const HiresPrim *p) { return p->dither && ((s_Sx == 1 && s_Sy == 1) || hires_get_dither()); }

/* ---- CPU->VRAM pixels ------------------------------------------------------------------- */

static void flush_puts(void)
{
    if (!s_put_n) return;
    close_group();
    const int w = s_pbx1 - s_pbx0 + 1, h = s_pby1 - s_pby0 + 1;
    size_t off = arena_alloc((size_t)w * h * 4);
    u32 *dst = (u32 *)((u8 *)s_arena.p + off);
    for (int j = 0; j < h; j++) {
        u32 *src = s_put + (size_t)(s_pby0 - s_ry + j) * s_rw + (s_pbx0 - s_rx);
        memcpy(dst + (size_t)j * w, src, (size_t)w * 4);
        memset(src, 0, (size_t)w * 4);
    }
    Step *st = add_step(ST_PUT_UP);
    st->x = s_pbx0 - s_rx; st->y = s_pby0 - s_ry; st->w = w; st->h = h; st->off = off;
    u32 idx;
    GPrim *g = new_prim(&idx);
    g->kf = K_PUT | (s_put_ms ? F_MSET : 0);
    GVtx v[6];
    quad1x(v, s_pbx0, s_pby0, s_pbx1 + 1, s_pby1 + 1, idx);
    u64 m[8];
    tile_mask(s_pbx0, s_pby0, s_pbx1, s_pby1, m);
    int key = s_put_mc ? 5 : 0;
    group_add(&key, 1, v, 6, m);
    s_put_n = 0;
}

static void be_put(int x, int y, u16 v, bool mc, bool ms)
{
    x &= 1023; y &= 511;
    s_dirty[y >> 4] |= 1ull << (x >> 4);
    if (!s_ready) return;
    if (x < s_rx || y < s_ry || x >= s_rx + s_rw || y >= s_ry + s_rh) return;
    u32 *d = &s_put[(size_t)(y - s_ry) * s_rw + (x - s_rx)];
    if (s_put_n && (mc != s_put_mc || ms != s_put_ms || (mc && ms && (*d & 0x10000)))) flush_puts();
    if (!s_put_n) { s_pbx0 = s_pbx1 = x; s_pby0 = s_pby1 = y; }
    else {
        if (x < s_pbx0) s_pbx0 = x;
        if (x > s_pbx1) s_pbx1 = x;
        if (y < s_pby0) s_pby0 = y;
        if (y > s_pby1) s_pby1 = y;
    }
    s_put_mc = mc; s_put_ms = ms;
    *d = (u32)v | 0x10000u;
    s_put_n++;
}

/* ---- triangles -------------------------------------------------------------------------- */

static void be_tri(const HiresPrim *p, const HiresVtx vin[3])
{
    if (!s_ready) return;
    flush_puts();
    const int Sx = s_Sx, Sy = s_Sy;
    s32 miny = vin[0].y, maxy = vin[0].y, minx = vin[0].x, maxx = vin[0].x;
    for (int i = 1; i < 3; i++) {
        if (vin[i].y < miny) miny = vin[i].y;
        if (vin[i].y > maxy) maxy = vin[i].y;
        if (vin[i].x < minx) minx = vin[i].x;
        if (vin[i].x > maxx) maxx = vin[i].x;
    }
    /* the 1x GPU has already drawn this triangle into VRAM */
    {
        int x0 = imax((int)(minx >> 16), p->clip_x1), x1 = imin((int)((maxx + 0xFFFF) >> 16), p->clip_x2);
        int y0 = imax((int)(miny >> 16), p->clip_y1), y1 = imin((int)((maxy + 0xFFFF) >> 16), p->clip_y2);
        if (x0 <= x1 && y0 <= y1) mark_vram(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    }
    const HiresVtx *a = &vin[0], *b = &vin[1], *c = &vin[2], *t;
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y < b->y) { t = b; b = c; c = t; }
    if (b->y < a->y) { t = a; a = b; b = t; }
    if (c->y == a->y) return;
    const s64 denom = ((s64)b->x - a->x) * ((s64)c->y - a->y) - ((s64)c->x - a->x) * ((s64)b->y - a->y);
    if (denom == 0) return;
    int ylo = imax(imax((int)(((s64)miny * Sy) >> 16), p->clip_y1 * Sy), s_oy);
    int yhi = imin(imin((int)((((s64)maxy * Sy) >> 16) + 1), (p->clip_y2 + 1) * Sy), s_oy + s_th);
    int xlo = imax(imax((int)(((s64)minx * Sx) >> 16), p->clip_x1 * Sx), s_ox);
    int xhi = imin(imin((int)((((s64)maxx * Sx) >> 16) + 1), (p->clip_x2 + 1) * Sx), s_ox + s_tw);
    if (yhi <= ylo || xhi <= xlo) return;

    const bool tex = p->tex, gour = p->gour;
    const u32 av[5] = { a->r, a->g, a->b, a->u, a->v };
    const u32 bv[5] = { b->r, b->g, b->b, b->u, b->v };
    const u32 cv[5] = { c->r, c->g, c->b, c->u, c->v };

    int umin = 0, umax = 255, vmin = 0, vmax = 255;
    if (tex && p->has_uvr) {
        umin = p->uvr_umin; umax = p->uvr_umax; vmin = p->uvr_vmin; vmax = p->uvr_vmax;
    } else if (tex) {
        umin = umax = (int)av[3]; vmin = vmax = (int)av[4];
        const u32 *o[2] = { bv, cv };
        for (int k = 0; k < 2; k++) {
            umin = imin(umin, (int)o[k][3]); umax = imax(umax, (int)o[k][3]);
            vmin = imin(vmin, (int)o[k][4]); vmax = imax(vmax, (int)o[k][4]);
        }
    }
    if (tex) tex_need(p, umin, umax, vmin, vmax);

    u32 idx;
    GPrim *g = new_prim(&idx);
    const bool dith = dither_on(p);
    g->kf = base_flags(p, K_TRI, tex) | (gour ? F_GOUR : 0) | (dith ? F_DITHER : 0) |
            (gour && p->has_cr ? F_C1X : 0) | ((Sx > 1 || Sy > 1) ? F_CLAMP : 0);
    set_tex(g, p);
    set_clip(g, p);
    g->uvr = (u32)umin | ((u32)umax << 8) | ((u32)vmin << 16) | ((u32)vmax << 24);
    g->color = av[0] | (av[1] << 8) | (av[2] << 16);
    for (int i = 0; i < 3; i++) {
        int lo, hi;
        if (gour && p->has_cr) { lo = p->cr_min[i]; hi = p->cr_max[i]; }
        else {
            lo = imin(imin((int)av[i], (int)bv[i]), (int)cv[i]);
            hi = imax(imax((int)av[i], (int)bv[i]), (int)cv[i]);
        }
        g->cmin |= (u32)(lo & 255) << (8 * i);
        g->cmax |= (u32)(hi & 255) << (8 * i);
    }
    /* attribute planes: gradients exactly as hires.c / the 1x GPU (12-bit quotient << 12),
     * then E(P, Q) = base + gdx*(P - ax) + gdy*(Q - ay) split for exact evaluation */
    for (int i = 0; i < 5; i++) {
        if ((i < 3 && !gour) || (i >= 3 && !tex)) continue;
        s64 n_x = (s64)((s32)bv[i] - (s32)av[i]) * ((s64)c->y - a->y) -
                  (s64)((s32)cv[i] - (s32)av[i]) * ((s64)b->y - a->y);
        s64 n_y = (s64)((s32)cv[i] - (s32)av[i]) * ((s64)b->x - a->x) -
                  (s64)((s32)bv[i] - (s32)av[i]) * ((s64)c->x - a->x);
        s32 gdx = (s32)((u32)(s32)(s64)(((__int128)n_x << 28) / denom) << 12);
        s32 gdy = (s32)((u32)(s32)(s64)(((__int128)n_y << 28) / denom) << 12);
        u32 base = (av[i] << FRAC) + (1u << (FRAC - 1));
        s64 c16 = (s64)(((__int128)base << 16) - (__int128)gdx * a->x - (__int128)gdy * a->y);
        g->cint[i] = c16 >> 16;
        u32 cfrac = (u32)(c16 & 0xFFFF);
        s64 qx = floor_div64(gdx, 2 * Sx), qy = floor_div64(gdy, 2 * Sy);
        g->qx[i] = (s32)qx;
        g->qy[i] = (s32)qy;
        g->rxy[i] = (u32)(gdx - qx * 2 * Sx) | ((u32)(gdy - qy * 2 * Sy) << 8) | (cfrac << 16);
    }
    /* Geometry: the triangle itself (GPU coverage = hires.c's: vertices at pixel centres, top-left
     * rule), unless a vertex is outside the viewport. The GPU would clip it, and the clipped
     * vertices are inexact, which flips fill-rule ties on its edges (pixel centres lie exactly on
     * edges all the time). Then draw the clipped bounding box and test coverage in the shader. */
    GVtx v[6];
    int nv = 3;
    bool inside = true;
    for (int i = 0; i < 3; i++) {
        v[i].x = (float)((double)((s64)vin[i].x * Sx) / 65536.0 - s_ox + 0.5);
        v[i].y = (float)((double)((s64)vin[i].y * Sy) / 65536.0 - s_oy + 0.5);
        v[i].prim = idx;
        if (v[i].x < 0 || v[i].y < 0 || v[i].x > (float)s_aw || v[i].y > (float)s_ah) inside = false;
    }
    g->kf |= denom > 0 ? F_LONGLEFT : 0;
    {
        const HiresVtx *sv[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            g->ev[2 * i] = (s32)((s64)sv[i]->x * Sx);
            g->ev[2 * i + 1] = (s32)((s64)sv[i]->y * Sy);
        }
    }
    if (!inside) {
        g->kf |= F_EDGE;
        quad(v, (float)(xlo - s_ox), (float)(ylo - s_oy), (float)(xhi - s_ox), (float)(yhi - s_oy), idx);
        nv = 6;
        s_st.edge_tris++;
    }
    int keys[2];
    int nk = prim_keys(p, p->semi, keys);
    u64 m[8];
    tile_mask(xlo / Sx, ylo / Sy, (xhi - 1) / Sx, (yhi - 1) / Sy, m);
    group_add(keys, nk, v, nv, m);
}

/* ---- rectangles -------------------------------------------------------------------------- */

static void be_rect(const HiresPrim *p, int x, int y, int w, int h, u32 r, u32 g8, u32 b,
                    u32 u0, u32 v0, bool fx, bool fy)
{
    if (!s_ready) return;
    flush_puts();
    int xs = imax(x, p->clip_x1), xe = imin(x + w, p->clip_x2 + 1);
    int ys = imax(y, p->clip_y1), ye = imin(y + h, p->clip_y2 + 1);
    if (xs >= xe || ys >= ye) return;
    int rxs = imax(xs, s_rx), rxe = imin(xe, s_rx + s_rw), rys = imax(ys, s_ry), rye = imin(ye, s_ry + s_rh);
    if (rxs < rxe && rys < rye) {
        if (p->tex) {   /* u/v sampled over the drawn columns/rows, as the 1x rect samples them */
            int uu0 = (int)(u0 | (fx ? 1u : 0u)), du = fx ? -1 : 1, dv = fy ? -1 : 1;
            int ua = uu0 + du * (rxs - x), ub = uu0 + du * (rxe - 1 - x);
            int va = (int)v0 + dv * (rys - y), vb = (int)v0 + dv * (rye - 1 - y);
            int ulo = imin(ua, ub), uhi = imax(ua, ub), vlo = imin(va, vb), vhi = imax(va, vb);
            if (ulo < 0 || uhi > 255) { ulo = 0; uhi = 255; }
            if (vlo < 0 || vhi > 255) { vlo = 0; vhi = 255; }
            tex_need(p, ulo, uhi, vlo, vhi);
        }
        u32 idx;
        GPrim *gp = new_prim(&idx);
        gp->kf = base_flags(p, K_RECT, p->tex) | (fx ? F_FLIPX : 0) | (fy ? F_FLIPY : 0);
        set_tex(gp, p);
        set_clip(gp, p);
        gp->color = r | (g8 << 8) | (b << 16);
        gp->aux0 = ((u32)x & 0xFFFF) | ((u32)y << 16);
        gp->aux1 = (u0 & 255) | ((v0 & 255) << 8);
        GVtx v[6];
        quad1x(v, rxs, rys, rxe, rye, idx);
        int keys[2];
        int nk = prim_keys(p, p->semi, keys);
        u64 m[8];
        tile_mask(rxs, rys, rxe - 1, rye - 1, m);
        group_add(keys, nk, v, 6, m);
    }
    mark_vram(xs, ys, xe - xs, ye - ys);   /* the 1x rect is drawn after this call */
}

/* ---- lines --------------------------------------------------------------------------------- */

/* Same pixels as hires.c's be_line: one quad per step (a 1 x Sy or Sx x 1 run), each with its
 * own colour record. Lines are rare (Destrega draws none in a fight). */
static void line_px(const HiresPrim *p, u32 kf, int x, int y, int w, int h, u32 cr, u32 cg, u32 cb, const int *keys, int nk)
{
    u32 idx;
    GPrim *g = new_prim(&idx);
    g->kf = kf;
    set_clip(g, p);
    g->color = (cr & 255) | ((cg & 255) << 8) | ((cb & 255) << 16);
    GVtx v[6];
    quad(v, (float)(x - s_ox), (float)(y - s_oy), (float)(x + w - s_ox), (float)(y + h - s_oy), idx);
    u64 m[8];
    tile_mask(x / s_Sx, y / s_Sy, (x + w - 1) / s_Sx, (y + h - 1) / s_Sy, m);
    group_add(keys, nk, v, 6, m);
}

static void be_line(const HiresPrim *p, int x0, int y0, u32 c0, int x1, int y1, u32 c1)
{
    if (!s_ready) return;
    flush_puts();
    {
        int lx0 = imax(imin(x0, x1), p->clip_x1), lx1 = imin(x0 > x1 ? x0 : x1, p->clip_x2);
        int ly0 = imax(imin(y0, y1), p->clip_y1), ly1 = imin(y0 > y1 ? y0 : y1, p->clip_y2);
        if (lx0 <= lx1 && ly0 <= ly1) mark_vram(lx0, ly0, lx1 - lx0 + 1, ly1 - ly0 + 1);
    }
    const int Sx = s_Sx, Sy = s_Sy;
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (!p->gour) c1 = c0;
    const u32 kf = base_flags(p, K_LINEPX, false) | (dither_on(p) ? F_DITHER : 0);
    int keys[2];
    int nk = prim_keys(p, p->semi, keys);
    const int k = adx > ady ? adx : ady;
    const bool xmajor = adx > ady;
    if (k == 0) {
        line_px(p, kf, x0 * Sx, y0 * Sy, Sx, Sy, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF, keys, nk);
        return;
    }
    const int Sa = xmajor ? Sx : Sy;
    s64 xf = (((s64)x0 * Sx) << 32) + ((s64)Sx << 31);
    s64 yf = (((s64)y0 * Sy) << 32) + ((s64)Sy << 31);
    const int kh = k * Sa;
    s64 sx = (((s64)dx * Sx) << 32) / kh, sy = (((s64)dy * Sy) << 32) / kh;
    xf -= sx * (Sa - 1) / 2;
    yf -= sy * (Sa - 1) / 2;
    s32 r = (s32)((c0 & 0xFF) << 12), g = (s32)(((c0 >> 8) & 0xFF) << 12), b = (s32)(((c0 >> 16) & 0xFF) << 12);
    s32 dr = (s32)(((s32)(c1 & 0xFF) - (s32)(c0 & 0xFF)) * 4096 / kh);
    s32 dg = (s32)(((s32)((c1 >> 8) & 0xFF) - (s32)((c0 >> 8) & 0xFF)) * 4096 / kh);
    s32 db = (s32)(((s32)((c1 >> 16) & 0xFF) - (s32)((c0 >> 16) & 0xFF)) * 4096 / kh);
    if (dx < 0) xf -= 1;
    if (dy < 0) yf -= 1;
    r += 1 << 11; g += 1 << 11; b += 1 << 11;
    const int steps = kh + (Sa - 1);
    for (int i = 0; i <= steps; i++) {
        int x = (int)(xf >> 32), y = (int)(yf >> 32);
        u32 cr = (u32)(r >> 12), cg = (u32)(g >> 12), cb = (u32)(b >> 12);
        if (xmajor) line_px(p, kf, x, y - Sy / 2, 1, Sy, cr, cg, cb, keys, nk);
        else line_px(p, kf, x - Sx / 2, y, Sx, 1, cr, cg, cb, keys, nk);
        xf += sx; yf += sy;
        r += dr; g += dg; b += db;
    }
}

/* ---- fills and copies ---------------------------------------------------------------------------- */

/* split a wrapped 1x rectangle into up to 4 pieces inside the region; returns the count */
static int region_pieces(int x, int y, int w, int h, int out[4][4])
{
    int xs[2][2], ys[2][2], nx = 0, ny = 0;
    x &= 1023; y &= 511;
    if (w > VRAM_W) w = VRAM_W;
    if (h > VRAM_H) h = VRAM_H;
    xs[nx][0] = x; xs[nx][1] = imin(x + w, VRAM_W); nx++;
    if (x + w > VRAM_W) { xs[nx][0] = 0; xs[nx][1] = x + w - VRAM_W; nx++; }
    ys[ny][0] = y; ys[ny][1] = imin(y + h, VRAM_H); ny++;
    if (y + h > VRAM_H) { ys[ny][0] = 0; ys[ny][1] = y + h - VRAM_H; ny++; }
    int n = 0;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            int a = imax(xs[i][0], s_rx), b = imin(xs[i][1], s_rx + s_rw);
            int c = imax(ys[j][0], s_ry), d = imin(ys[j][1], s_ry + s_rh);
            if (a < b && c < d) { out[n][0] = a; out[n][1] = c; out[n][2] = b; out[n][3] = d; n++; }
        }
    return n;
}

static void be_fill(int x, int y, int w, int h, u16 c)
{
    if (s_ready) {
        flush_puts();
        int pc[4][4];
        int n = region_pieces(x, y, w, h, pc);
        if (n) {
            u32 idx;
            GPrim *g = new_prim(&idx);
            g->kf = K_FILL;
            g->color = c;
            for (int i = 0; i < n; i++) {
                GVtx v[6];
                quad1x(v, pc[i][0], pc[i][1], pc[i][2], pc[i][3], idx);
                u64 m[8];
                tile_mask(pc[i][0], pc[i][1], pc[i][2] - 1, pc[i][3] - 1, m);
                int key = 0;
                group_add(&key, 1, v, 6, m);
            }
        }
    }
    mark_vram(x, y, w, h);
}

static void be_copy(int sx, int sy, int dx, int dy, int wd, int ht, bool mc, bool ms)
{
    if (s_ready) {
        flush_puts();
        int pc[4][4];
        if (region_pieces(dx, dy, wd, ht, pc)) {
            /* source outside the region is read from the VRAM texture (as it is before this copy) */
            ensure_vram(sx, sy, wd, ht);
            /* rows a row-by-row copy reads after this copy wrote them: run it in chunks */
            int d = (dy - sy) & 511, chunk = (d > 0 && d < ht) ? d : ht;
            for (int j0 = 0; j0 < ht; j0 += chunk) {
                int hh = imin(chunk, ht - j0);
                int n = region_pieces(dx, dy + j0, wd, hh, pc);
                if (!n) continue;
                close_group();
                Step *st = add_step(ST_TCOPY);   /* snapshot the target for the in-region source */
                st->y = 0; st->h = s_th;
                s_st.tcopies++;
                u32 idx;
                GPrim *g = new_prim(&idx);
                g->kf = K_COPY | (ms ? F_MSET : 0);
                g->aux0 = (u32)(sx & 1023) | ((u32)((sy + j0) & 511) << 16);
                g->aux1 = (u32)(dx & 1023) | ((u32)((dy + j0) & 511) << 16);
                for (int i = 0; i < n; i++) {
                    GVtx v[6];
                    quad1x(v, pc[i][0], pc[i][1], pc[i][2], pc[i][3], idx);
                    u64 m[8];
                    tile_mask(pc[i][0], pc[i][1], pc[i][2] - 1, pc[i][3] - 1, m);
                    int key = mc ? 5 : 0;
                    group_add(&key, 1, v, 6, m);
                }
            }
        }
    }
    mark_vram(dx, dy, wd, ht);
}

/* ---- submission ------------------------------------------------------------------------------------ */

static SDL_GPUBuffer *ensure_buffer(SDL_GPUBuffer *b, u32 *cap, u32 need, SDL_GPUBufferUsageFlags usage)
{
    if (b && *cap >= need) return b;
    if (b) SDL_ReleaseGPUBuffer(s_dev, b);
    u32 c = 1 << 16;
    while (c < need) c *= 2;
    SDL_GPUBufferCreateInfo bi = { .usage = usage, .size = c };
    *cap = c;
    return SDL_CreateGPUBuffer(s_dev, &bi);
}

static SDL_GPUTransferBuffer *ensure_tbuf(SDL_GPUTransferBuffer *b, u32 *cap, u32 need, SDL_GPUTransferBufferUsage usage)
{
    if (b && *cap >= need) return b;
    if (b) SDL_ReleaseGPUTransferBuffer(s_dev, b);
    u32 c = 1 << 20;
    while (c < need) c *= 2;
    SDL_GPUTransferBufferCreateInfo ti = { .usage = usage, .size = c };
    *cap = c;
    return SDL_CreateGPUTransferBuffer(s_dev, &ti);
}

static void wait_fence(SDL_GPUFence **f)
{
    if (!*f) return;
    SDL_WaitForGPUFences(s_dev, true, f, 1);
    SDL_ReleaseGPUFence(s_dev, *f);
    *f = NULL;
}

static void reset_lists(void)
{
    s_verts.n = s_prims.n = s_steps.n = s_arena.n = 0;
}

/* Record and submit everything pending. readback: also download the target rectangle
 * (target-local pixels) into s_rb and wait for it. present: convert (px, py, pw, ph) into
 * s_present. */
static bool submit(bool readback, int rx, int ry, int rw, int rh, bool present, int px, int py)
{
    flush_puts();
    close_group();
    if (!s_steps.n && !readback && !present) return true;
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(s_dev);
    if (!cb) { rt_log("hires-gpu: no command buffer: %s", SDL_GetError()); reset_lists(); return false; }
    const size_t vb = s_verts.n * sizeof(GVtx), pb = s_prims.n * sizeof(GPrim);
    const size_t voff = 0, poff = (vb + 255) & ~(size_t)255, aoff = (poff + pb + 255) & ~(size_t)255;
    const size_t total = aoff + s_arena.n;
    SDL_GPUCopyPass *cp = NULL;
    SDL_GPURenderPass *rp = NULL;
    if (vb || pb || s_arena.n) {
        s_up = ensure_tbuf(s_up, &s_up_cap, (u32)total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        u8 *m = SDL_MapGPUTransferBuffer(s_dev, s_up, true);
        if (!m) { rt_log("hires-gpu: map: %s", SDL_GetError()); SDL_CancelGPUCommandBuffer(cb); reset_lists(); return false; }
        if (vb) memcpy(m + voff, s_verts.p, vb);
        if (pb) memcpy(m + poff, s_prims.p, pb);
        if (s_arena.n) memcpy(m + aoff, s_arena.p, s_arena.n);
        SDL_UnmapGPUTransferBuffer(s_dev, s_up);
        cp = SDL_BeginGPUCopyPass(cb);
        if (vb) {
            s_vbuf = ensure_buffer(s_vbuf, &s_vbuf_cap, (u32)vb, SDL_GPU_BUFFERUSAGE_VERTEX);
            SDL_GPUTransferBufferLocation src = { s_up, (u32)voff };
            SDL_GPUBufferRegion dst = { s_vbuf, 0, (u32)vb };
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        }
        if (pb) {
            s_pbuf = ensure_buffer(s_pbuf, &s_pbuf_cap, (u32)pb, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
            SDL_GPUTransferBufferLocation src = { s_up, (u32)poff };
            SDL_GPUBufferRegion dst = { s_pbuf, 0, (u32)pb };
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        }
    }
    const Step *steps = s_steps.p;
    int cur_key = -1;
    const float vk[4] = { 2.0f / (float)s_aw, 2.0f / (float)s_ah, 0, 0 };
    for (size_t i = 0; i < s_steps.n; i++) {
        const Step *st = &steps[i];
        if (st->type == ST_DRAW) {
            if (cp) { SDL_EndGPUCopyPass(cp); cp = NULL; }
            if (!rp) {
                SDL_GPUColorTargetInfo ct = { .texture = s_target, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE };
                rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
                s_st.passes++;
                cur_key = -1;
                SDL_SetGPUBlendConstants(rp, (SDL_FColor){ 0.5f, 0.5f, 0.5f, 0.5f });
                SDL_PushGPUVertexUniformData(cb, 0, vk, sizeof vk);
            }
            if (st->key != cur_key) {
                SDL_BindGPUGraphicsPipeline(rp, s_pipe[st->key]);
                SDL_GPUBufferBinding vbb = { s_vbuf, 0 };
                SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
                SDL_BindGPUFragmentStorageBuffers(rp, 0, &s_pbuf, 1);
                SDL_GPUTextureSamplerBinding tsb[3] = { { s_temp, s_samp }, { s_puttex, s_samp }, { s_vramtex, s_samp } };
                SDL_BindGPUFragmentSamplers(rp, 0, tsb, 3);
                cur_key = st->key;
            }
            int k = st->key % 5;
            FragU fu = { s_Sx, s_Sy, s_ox, s_oy, k ? 1 : 0, k ? k - 1 : 0, s_rx, s_ry, s_rw, s_rh, 0, 0 };
            SDL_PushGPUFragmentUniformData(cb, 0, &fu, sizeof fu);
            SDL_DrawGPUPrimitives(rp, st->count, 1, st->first, 0);
            s_st.draws++;
        } else {
            if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; }
            if (!cp) cp = SDL_BeginGPUCopyPass(cb);
            if (st->type == ST_TCOPY) {
                SDL_GPUTextureLocation src = { .texture = s_target, .x = 0, .y = (u32)st->y };
                SDL_GPUTextureLocation dst = { .texture = s_temp, .x = 0, .y = (u32)st->y };
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, (u32)s_tw, (u32)st->h, 1, false);
            } else {
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = s_up, .offset = (u32)(aoff + st->off),
                                                   .pixels_per_row = (u32)st->w, .rows_per_layer = (u32)st->h };
                SDL_GPUTextureRegion dst = { .texture = st->type == ST_VRAM_UP ? s_vramtex : s_puttex,
                                             .x = (u32)st->x, .y = (u32)st->y, .w = (u32)st->w, .h = (u32)st->h, .d = 1 };
                SDL_UploadToGPUTexture(cp, &src, &dst, false);
            }
        }
    }
    if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; }
    if (present) {
        if (cp) { SDL_EndGPUCopyPass(cp); cp = NULL; }
        SDL_GPUColorTargetInfo ct = { .texture = s_present, .load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE };
        rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
        SDL_BindGPUGraphicsPipeline(rp, s_present_pipe);
        SDL_GPUTextureSamplerBinding tsb = { s_target, s_samp };
        SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
        const int pu[4] = { px, py, 0, 0 };
        SDL_PushGPUFragmentUniformData(cb, 0, pu, sizeof pu);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        rp = NULL;
        s_st.passes++;
    }
    if (readback) {
        const u32 bytes = (u32)rw * (u32)rh * 2;
        s_down = ensure_tbuf(s_down, &s_down_cap, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
        if (!cp) cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureRegion src = { .texture = s_target, .x = (u32)rx, .y = (u32)ry, .w = (u32)rw, .h = (u32)rh, .d = 1 };
        SDL_GPUTextureTransferInfo dst = { .transfer_buffer = s_down, .offset = 0 };
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        s_st.readbacks++;
    }
    if (cp) SDL_EndGPUCopyPass(cp);
    reset_lists();
    s_st.submits++;
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (!f) { rt_log("hires-gpu: submit: %s", SDL_GetError()); return false; }
    if (readback) {
        wait_fence(&f);
        size_t need = (size_t)rw * rh;
        if (need > s_rb_cap) { free(s_rb); s_rb = malloc(need * 2); s_rb_cap = s_rb ? need : 0; }
        const u16 *m = SDL_MapGPUTransferBuffer(s_dev, s_down, false);
        if (!m || !s_rb) return false;
        memcpy(s_rb, m, need * 2);
        SDL_UnmapGPUTransferBuffer(s_dev, s_down);
        return true;
    }
    /* at most two frames in flight */
    wait_fence(&s_fences[s_fence_i]);
    s_fences[s_fence_i] = f;
    s_fence_i ^= 1;
    return true;
}

/* ---- backend: target management ---------------------------------------------------------------- */

static SDL_GPUTexture *make_tex(SDL_GPUTextureFormat fmt, int w, int h, SDL_GPUTextureUsageFlags usage)
{
    SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage,
                                    .width = (u32)w, .height = (u32)h, .layer_count_or_depth = 1, .num_levels = 1 };
    return SDL_CreateGPUTexture(s_dev, &ti);
}

static void release_target(void)
{
    for (int i = 0; i < 2; i++) wait_fence(&s_fences[i]);
    if (s_target) SDL_ReleaseGPUTexture(s_dev, s_target);
    if (s_temp) SDL_ReleaseGPUTexture(s_dev, s_temp);
    if (s_puttex) SDL_ReleaseGPUTexture(s_dev, s_puttex);
    s_target = s_temp = s_puttex = NULL;
    free(s_put);
    s_put = NULL;
    s_put_n = 0;
}

static void be_destroy(void)
{
    if (!s_inited) return;
    reset_lists();
    for (int k = 0; k < NKEYS; k++) s_gv[k].n = 0;
    memset(s_gm, 0, sizeof s_gm);
    s_gused = 0;
    s_put_n = 0;
    release_target();
    s_ready = false;
    s_Sx = s_Sy = 0;
}

static bool be_configure(const u16 *vram1x, int sx, int sy, int rx, int ry, int rw, int rh)
{
    if (!s_inited) return false;
    if (sx < 1 || sx > HIRES_MAX_SCALE || sy < 1 || sy > HIRES_MAX_SCALE || rw <= 0 || rh <= 0 ||
        rx < 0 || ry < 0 || rx + rw > VRAM_W || ry + rh > VRAM_H)
        return false;
    if (s_ready) submit(false, 0, 0, 0, 0, false, 0, 0);
    be_destroy();
    s_vram = vram1x;
    s_Sx = sx; s_Sy = sy;
    s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
    s_tw = rw * sx; s_th = rh * sy;
    s_ox = rx * sx; s_oy = ry * sy;
    const SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM;
    for (s_aw = 64; s_aw < s_tw; s_aw *= 2) {}
    for (s_ah = 64; s_ah < s_th; s_ah *= 2) {}
    s_target = make_tex(fmt, s_aw, s_ah, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    s_temp = make_tex(fmt, s_aw, s_ah, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    s_puttex = make_tex(SDL_GPU_TEXTUREFORMAT_R32_UINT, rw, rh, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    s_put = calloc((size_t)rw * rh, sizeof(u32));
    if (!s_target || !s_temp || !s_puttex || !s_put) {
        rt_log("hires-gpu: cannot allocate a %dx%d target: %s", s_tw, s_th, SDL_GetError());
        release_target();
        return false;
    }
    /* full VRAM upload, then seed the target from it (nearest-neighbour upscale) */
    memset(s_dirty, 0xFF, sizeof s_dirty);
    s_ready = true;
    ensure_vram(0, 0, VRAM_W, VRAM_H);
    u32 idx;
    GPrim *g = new_prim(&idx);
    g->kf = K_SEED;
    GVtx v[6];
    quad1x(v, rx, ry, rx + rw, ry + rh, idx);
    u64 m[8];
    tile_mask(rx, ry, rx + rw - 1, ry + rh - 1, m);
    int key = 0;
    group_add(&key, 1, v, 6, m);
    return true;
}

static bool view_rect(int x, int y, int w, int h, HiresView *out)
{
    if (!s_ready || w <= 0 || h <= 0) return false;
    if (x < s_rx || y < s_ry || x + w > s_rx + s_rw || y + h > s_ry + s_rh) return false;
    out->pixels = NULL;
    out->stride = 0;
    out->w = w * s_Sx;
    out->h = h * s_Sy;
    out->scale_x = s_Sx;
    out->scale_y = s_Sy;
    out->gpu = s_target;
    out->gx = x * s_Sx - s_ox;
    out->gy = y * s_Sy - s_oy;
    return true;
}

static bool be_view(int x, int y, int w, int h, HiresView *out)
{
    if (!view_rect(x, y, w, h, out)) return false;
    submit(false, 0, 0, 0, 0, false, 0, 0);   /* one command buffer per presented frame */
    return true;
}

static bool be_read(int x, int y, int w, int h, HiresView *out)
{
    if (!view_rect(x, y, w, h, out)) return false;
    if (!submit(true, out->gx, out->gy, out->w, out->h, false, 0, 0)) return false;
    out->pixels = s_rb;
    out->stride = out->w;
    out->gpu = NULL;
    return true;
}

SDL_GPUTexture *hires_gpu_present(const HiresView *v)
{
    if (!s_ready || !v->gpu) return NULL;
    if (!s_present || s_pw != v->w || s_ph != v->h) {
        if (s_present) SDL_ReleaseGPUTexture(s_dev, s_present);
        s_present = make_tex(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, v->w, v->h,
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        s_pw = v->w; s_ph = v->h;
        s_dirty_present = true;
        if (!s_present) return NULL;
    }
    static int last_x = -1, last_y = -1;
    if (s_dirty_present || last_x != v->gx || last_y != v->gy) {
        submit(false, 0, 0, 0, 0, true, v->gx, v->gy);
        last_x = v->gx; last_y = v->gy;
        s_dirty_present = false;
    }
    return s_present;
}

static const HiresBackend s_gpu = {
    "gpu", be_configure, be_tri, be_rect, be_line, be_fill, be_copy, be_put, be_view, be_read, be_destroy,
};

const HiresBackend *hires_gpu_backend(void) { return &s_gpu; }

/* ---- init ------------------------------------------------------------------------------------------ */

static SDL_GPUShader *make_shader(const char *entry, SDL_GPUShaderStage stage, u32 samplers, u32 sbufs, u32 ubufs)
{
    SDL_GPUShaderCreateInfo si = { .code = (const Uint8 *)s_msl, .code_size = sizeof s_msl - 1, .entrypoint = entry,
                                   .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage, .num_samplers = samplers,
                                   .num_storage_buffers = sbufs, .num_uniform_buffers = ubufs };
    SDL_GPUShader *s = SDL_CreateGPUShader(s_dev, &si);
    if (!s) rt_log("hires-gpu: shader %s: %s", entry, SDL_GetError());
    return s;
}

static void blend_for_key(int key, SDL_GPUColorTargetBlendState *b)
{
    memset(b, 0, sizeof *b);
    const bool mc = key >= 5;
    const int k = key % 5;
    if (!mc && k == 0) return;   /* opaque: no blending */
    b->enable_blend = true;
    b->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    b->src_alpha_blendfactor = mc ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    b->dst_alpha_blendfactor = mc ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_ZERO;
    b->src_color_blendfactor = mc ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
    b->color_blend_op = SDL_GPU_BLENDOP_ADD;
    switch (k) {
    case 0: b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA; break;   /* opaque + mask check */
    case 1: b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR; break;   /* B/2 + F/2 */
    case 2: case 4: b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;   /* B + F, B + F/4 */
    case 3: b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;                 /* B - F */
            b->color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT; break;
    }
}

bool hires_gpu_init(SDL_GPUDevice *dev)
{
    if (s_inited) return true;
    if (!dev) {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) {   /* SDL_GPU needs it (the offscreen driver has no GPU backend) */
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { rt_log("hires-gpu: SDL video: %s", SDL_GetError()); return false; }
        }
        dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, NULL);
        if (!dev) { rt_log("hires-gpu: no GPU device: %s", SDL_GetError()); return false; }
    }
    if (!(SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL)) {
        rt_log("hires-gpu: device %s does not take MSL shaders (only Metal is supported)", SDL_GetGPUDeviceDriver(dev));
        return false;
    }
    s_dev = dev;
    const SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM;
    if (!SDL_GPUTextureSupportsFormat(dev, fmt, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        rt_log("hires-gpu: B5G5R5A1 render targets unsupported on this GPU");
        return false;
    }
    SDL_GPUShader *vs = make_shader("vs_prim", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
    SDL_GPUShader *fs = make_shader("fs_prim", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1, 1);
    SDL_GPUShader *pvs = make_shader("vs_full", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    SDL_GPUShader *pfs = make_shader("fs_present", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);
    if (!vs || !fs || !pvs || !pfs) return false;
    SDL_GPUVertexBufferDescription vbd = { .slot = 0, .pitch = sizeof(GVtx), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute va[2] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 8 },
    };
    for (int k = 0; k < NKEYS; k++) {
        SDL_GPUColorTargetDescription cd = { .format = fmt };
        blend_for_key(k, &cd.blend_state);
        SDL_GPUGraphicsPipelineCreateInfo pi = {
            .vertex_shader = vs, .fragment_shader = fs,
            .vertex_input_state = { .vertex_buffer_descriptions = &vbd, .num_vertex_buffers = 1,
                                    .vertex_attributes = va, .num_vertex_attributes = 2 },
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
            .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1 },
        };
        s_pipe[k] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!s_pipe[k]) { rt_log("hires-gpu: pipeline %d: %s", k, SDL_GetError()); return false; }
    }
    {
        SDL_GPUColorTargetDescription cd = { .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM };
        SDL_GPUGraphicsPipelineCreateInfo pi = {
            .vertex_shader = pvs, .fragment_shader = pfs,
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
            .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1 },
        };
        s_present_pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!s_present_pipe) { rt_log("hires-gpu: present pipeline: %s", SDL_GetError()); return false; }
    }
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    SDL_ReleaseGPUShader(dev, pvs);
    SDL_ReleaseGPUShader(dev, pfs);
    SDL_GPUSamplerCreateInfo sci = { .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
                                     .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                     .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
    s_samp = SDL_CreateGPUSampler(dev, &sci);
    s_vramtex = make_tex(SDL_GPU_TEXTUREFORMAT_R16_UINT, VRAM_W, VRAM_H, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!s_samp || !s_vramtex) { rt_log("hires-gpu: %s", SDL_GetError()); return false; }
    s_inited = true;
    rt_log("hires-gpu: SDL_GPU device ready (driver %s)", SDL_GetGPUDeviceDriver(dev));
    return true;
}
