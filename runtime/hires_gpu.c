/* SDL_GPU high-resolution backend (see hires.h, AGENTS.md "GPU renderer").
 *
 * Shaders: MSL source below for Metal; SPIR-V for Vulkan and DXIL for Direct3D 12, both from
 * runtime/shaders/hires.glsl (tools/gen_spirv.py -> shaders/hires_spirv.h; tools/gen_dxil.py:
 * GLSL -> SPIR-V -> HLSL (SPIRV-Cross) -> DXIL (DXC) -> shaders/hires_dxil.h). MSL and GLSL
 * implement the same rules; the GLSL emulates 64-bit integers with 32-bit pairs (see its
 * header). On Vulkan every triangle takes the exact edge test (F_EDGE, below): Vulkan does not
 * fix the rasterizer's tie-break rule for pixel centres on an edge; Metal and D3D12 specify
 * top-left. The graphics API is chosen per device (s_apis, s_order; hires_gpu_shutdown +
 * hires_gpu_init switch it at runtime).
 *
 * Speed: the fragment shader is the cost (it scales with hi-res pixels; submission is ~1 submit
 * and ~150 draws per frame). Apple GPUs have no integer divider: 64-bit division is a long
 * emulated sequence and even 32-bit division is slow. The MSL therefore divides nothing on the
 * per-pixel path: the plane-equation fraction is a 0/1 carry after a 32-bit floor, span edges and
 * 1x pixel coordinates are float quotients corrected exactly with integer multiplies, gdx / Sx
 * comes from the stored split gdx = qx*2Sx + rx, (U1-U0)/n is a 32-bit (or 16-bit long) division
 * (perspective runs only). Removing the 64-bit divisions made a 4x6 widescreen fight 3x faster
 * (18 -> 6 ms/frame headless, M1 Pro), the 32-bit ones another 8%. hires.glsl has the same
 * divide-free forms except the gdx / Sx split and the float floordiv (it still uses 32-bit
 * division there; porting them needs the DXIL to be regenerated, which needs DXC).
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
 * Full-precision colour (hires_set_colour, AGENTS.md "Render quality"): R8G8B8A8_UNORM target
 * with the same blends in 8-bit units (a second pipeline set, s_pipe[1]; FragU.p0 = hp), PS1
 * red in .r. Texture filters (F_BILIN, F_XBR) and the SSAA box filter of the present pass are
 * the integer formulas of hires.c line for line (tex_combine, samp_bilinear, samp_xbr,
 * rect_axis, hires_view_to_rgba), so the two backends stay bit-identical in every mode.
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
 * Vertices are float target coordinates plus a primitive index; a wider target (widescreen)
 * only touches GVtx, the projection in vs_prim and configure().
 *
 * Widescreen guard band (HiresBackend.set_guard, same rules as hires.c): the target is
 * gx 1x columns wider on each side (s_ox = (rx - gx) * Sx, so absolute hi-res x can be
 * negative with rx = 0: the clip rectangle is packed as signed 16-bit and the shader's 1x
 * column uses floor division). Primitives whose draw area touches the region edge are clipped
 * gx columns further out (guard_clip), untextured full-width rects and full-width fill rows
 * extend over it, uploads/copies never touch it, and configure() starts it black.
 *
 * PGXP (runtime/pgxp.h; the vertices arrive already validated and welded by gpu.c):
 * - Fractional 16.16 positions keep the exact coverage. A triangle with a fractional vertex is
 *   drawn as itself grown by 1 px per edge (or its bounding box for slivers), and the shader
 *   applies hires.c's edge rule (F_EDGE). Float vertices fed to the GPU rasterizer would be
 *   snapped to its sub-pixel grid and break fill-rule ties. Attribute planes already took
 *   16.16 positions, so gouraud and affine u/v stay exact: `--pgxp affine` compares at
 *   0 differing pixels.
 * - Perspective u/v (F_PERSP: textured, w on all three vertices, hires_get_perspective()):
 *   q, u*q and v*q planes as hires.c builds them, re-based to the bbox corner and stored as
 *   64-bit fixed point (2^-38). The shader evaluates them exactly at the start and end of
 *   hires.c's 8-pixel run and divides exactly (div24: a float estimate corrected with integer
 *   multiplies, since 64-bit division is slow on the GPU). It then steps linearly in between,
 *   the same way hires.c does. hires.c uses double, so u/v differ in the last bits, and a
 *   handful of samples land on the neighbouring texel (29 pixels over 331 fight frames at 4x6;
 *   a float version gave 12,494). Colours stay affine, the UV clamp also applies at 1x1 when a
 *   vertex is precise, and clamped GTE vertices never have precise values (pgxp.c).
 *
 * Fast precision (hires_gpu_set_fast, --gpu-precision fast; AGENTS.md "Fast GPU precision"):
 * triangles and rects only; fills, uploads, copies, lines and the seed stay on the exact
 * pipelines, so the target is the exact one wherever no polygon is drawn. Approximate, not
 * bit-exact, and presentation only like everything here.
 * - Geometry: the exact path's vertex positions (16.16 scaled, + 0.5) straight to the
 *   hardware rasterizer: its own fill rule and sub-pixel snapping, no edge test, no bbox
 *   fallback (the GPU clips). Rects are quads on their 1x pixel edges.
 * - Attributes are interpolated by the hardware. u/v: each vertex gets the value hires.c's
 *   planes have at the vertex + (0.5 / S - 0.5) 1x pixels (hires.c samples at (X + 0.5) / S -
 *   0.5, the hardware at the pixel centre, X / S here) plus the +0.5 rounding bias. With PGXP
 *   depth on all three vertices, clip w = 1 / q at that shifted point, so the hardware's
 *   perspective division reproduces hires.c's u/v (which hires.c itself only computes exactly
 *   every 8 pixels). Colour: screen-linear (noperspective), unshifted, rounded like the PS1
 *   (floor(c + 0.5)); no clamp to the 1x colour range.
 * - Texture decode in fs_fast / hires_fast.glsl FS_FAST: integer u/v (floor), texture window,
 *   4/8-bit through the CLUT or 15-bit direct from the R16_UINT VRAM texture (the same
 *   dirty-block upload as exact); texel 0 is transparent; then the exact backend's integer
 *   modulation, raw texture, dither (only with F_DITHER: off above 1x1 as in hires.c) and
 *   texture filters (tcomb / samp_filt logic, sample point = interpolated u/v - 0.5, clamped to
 *   the UV range of the record). Rects without a filter take no clamp.
 * - Blending, mask bit and pass split: the same 10 pipeline blend states as exact, the same
 *   two-pass split for semi-transparent textured primitives (opaque texels, then STP texels),
 *   and the mask bit in the target's alpha: set-mask writes alpha 1, check-mask uses
 *   ONE_MINUS_DST_ALPHA in the blend. So semi-transparency is fixed-function and exact given
 *   the same input colour.
 * - Records: FVtx (28 bytes: position, w, u/v, RGBA8 colour, record index) and FPrim (32
 *   bytes: flags, page, CLUT, window, clip rectangle, UV clamp range) instead of the 272-byte
 *   GPrim with its exact plane equations, so the CPU builds a triangle with a few float
 *   operations and the fragment shader has no 64-bit or exact rational math. Group keys
 *   NKEYS..2*NKEYS-1 are the fast pipelines; they share the group / tile-mask ordering, so
 *   exact and fast primitives (e.g. a fill between them) keep command order.
 *
 * Texture cache (fast precision only; --tex-cache off|on|verify[=N], hires_gpu_set_tex_cache;
 * AGENTS.md "Texture cache"). Textured 3D triangles (not hires_tri_is_2d: those, and every rect,
 * keep the VRAM decode above so the xbr 2D path is unchanged) sample a decoded copy of their
 * texture instead of the raw VRAM, with real GPU filtering: trilinear mipmaps + anisotropy.
 * - Key (= one material's texture for the high-level renderer, docs/HIGH_LEVEL_RENDERER.md):
 *   page origin, depth (4 / 8 / 15-bit), CLUT origin (4/8-bit only) and the texture window word.
 *   One entry = the whole 256x256 page as the PS1 samples it under that window and CLUT: u/v
 *   0..255 map to the layer's texels, so the vertex u/v need no change.
 * - Storage: layer FPrim.tp (replaces the page origin in the record when F_TC is set) of two 2D
 *   arrays, s_tc_layers (128) x 256 x 256, s_tc_levels (4) mip levels: RGBA8 premultiplied
 *   (texel 0 = (0,0,0,0), else the 5-bit channels as c << 3 and alpha 255) and R8 = coverage of
 *   texels with bit 15 (STP). 128 layers = 44 MB + 11 MB; created on first use.
 * - Mips: 2x2 averages of the premultiplied values on the CPU (tc_decode), so transparent texels
 *   add no colour (no dark fringe) and alpha is the opaque coverage. The shader divides by alpha,
 *   discards below half coverage (the rule of the exact filters) and takes the STP bit as the
 *   majority of the opaque coverage (st * 2 >= a); then the usual integer modulation / blend
 *   inputs. Not SDL_GenerateMipmapsForGPUTexture: it rebuilds every layer of the array (all 128
 *   on each decode) and its filter is the API's (not alpha-aware, different per API).
 * - Page edges / bleed: a polygon's UV rectangle rarely aligns with mip blocks (DESTREGA_TC_DIAG,
 *   Hills match: 66 % of the cached triangles have an odd edge, 87 % one not on a multiple of 4), so a block may mix the polygon's texels with
 *   the neighbouring tile's. Level count is the bound: 4 levels = blocks of at most 8x8 texels,
 *   and the sample is clamped to the record's UV range (texel centres) as in the exact filters.
 *   At 4x6 more than 99 % of the 3D-textured pixels are magnified and levels >= 3 (8x8 blocks,
 *   above 8 texels per render pixel) cover < 0.001 % of them (every VS stage), so 4 vs 9 levels
 *   compare equal to the 4th digit against software. Per-tile sub-rectangles would need a layer
 *   (or an atlas slot) per UV rectangle instead of per page.
 * - Validity: every VRAM write stamps the 16x16 blocks it touches (mark_vram, be_put:
 *   s_bst / s_tst per 64x256 page tile); an entry compares the stamps of its page tiles and CLUT
 *   blocks with the clock it was last checked at, and only then hashes the page / CLUT area: a
 *   changed hash re-decodes the layer (uploads go after the draws recorded so far, which saw the
 *   old texels), an equal one only advances the clock. So uploads, fills, VRAM copies and CLUT
 *   rewrites are exact; a new texture window is a new key. Cleared on reconfigure.
 * - Eviction: least recently used by frame (be_view); an entry used in the current frame is never
 *   evicted (its layer is referenced by recorded draws); if none is free the primitive takes the
 *   uncached path (tc_full).
 * - Verify (--tex-cache verify=N): every N frames every entry whose stamps say "unchanged" is
 *   re-hashed (a mismatch = a write the tracking missed: STALE), and one layer per round is read
 *   back from the GPU and compared with a fresh decode.
 */
#include "hires_gpu.h"
#include "shaders/hires_spirv.h"
#include "shaders/hires_dxil.h"
#include "shaders/hires_fast_spirv.h"
#include "shaders/hires_fast_dxil.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
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
#define F_PERSP (1u << 21)      /* PGXP: perspective-correct u/v from the pf planes */
#define F_BILIN (1u << 22)      /* texture filter: bilinear (hires.c samp_bilinear) */
#define F_XBR (1u << 23)        /* texture filter: 2D edge-aware (hires.c samp_xbr) */
/* fast precision, texture cache ("Texture cache" below): */
#define F_TC (1u << 24)         /* texels from cache layer FPrim.tp, sampled with mipmaps + anisotropy */
#define F_TCNEAR (1u << 25)     /* magnified: the nearest texel (texture filter off) */
#define F_TCSTP (1u << 26)      /* the entry has texels with bit 15: sample the STP array too */

/* pipeline keys: 0 opaque, 1..4 semi-transparency mode 0..3; +5 = with mask check */
#define NKEYS 10

typedef struct { float x, y; u32 prim; } GVtx;
typedef struct {   /* must match struct Prim in the MSL source */
    u32 kf, tp, clut, tw, clip0, clip1, uvr, cmin, cmax, color, aux0, aux1;
    s32 qx[5], qy[5];
    u32 rxy[5], pad;
    s64 cint[5];
    u32 pad2[2];
    s32 ev[6];                  /* sorted vertices a, b, c in hi-res 16.16 (x, y) */
    u32 pad3[2];
    s64 pl[9];                  /* F_PERSP: q, u*q, v*q at pixel porg; then d/dX; then d/dY (2^-38) */
    s32 porg[2];                /* hi-res pixel the pl planes are relative to */
} GPrim;
_Static_assert(sizeof(GPrim) == 272, "GPrim must match the MSL struct Prim");

typedef struct { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; } FragU;

/* fast precision: vertex (float target position, clip w, u/v in texels, RGBA8 colour, primitive)
 * and per-primitive record (= FVI / FPrim in the MSL and hires_fast.glsl) */
typedef struct { float x, y, w, u, v; u32 col, prim; } FVtx;
typedef struct { u32 kf, tp, clut, tw, clip0, clip1, ur, vr; } FPrim;
_Static_assert(sizeof(FVtx) == 28 && sizeof(FPrim) == 32, "FVtx / FPrim must match the shaders");
/* group keys: 0..NKEYS-1 exact pipelines (GVtx), NKEYS..2*NKEYS-1 the same blend states with the
 * fast shaders (FVtx) */
#define NGKEYS (2 * NKEYS)

static const char s_msl[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Prim { uint kf, tp, clut, tw, clip0, clip1, uvr, cmin, cmax, color, aux0, aux1;\n"
"  int qx[5]; int qy[5]; uint rxy[5]; uint pad; long cint[5]; uint pad2[2]; int ev[6]; uint pad3[2];\n"
"  long pl[9]; int porg[2]; };\n"
"struct VI { float2 pos [[attribute(0)]]; uint prim [[attribute(1)]]; };\n"
"struct VO { float4 pos [[position]]; uint prim [[flat]]; };\n"
"struct VU { float4 k; };\n"
"vertex VO vs_prim(VI in [[stage_in]], constant VU &u [[buffer(0)]]) {\n"
"  VO o; o.pos = float4(in.pos.x * u.k.x - 1.0, 1.0 - in.pos.y * u.k.y, 0.0, 1.0); o.prim = in.prim; return o; }\n"
"struct FU { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; };\n"
"constant int DITH[16] = { -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2 };\n"
/* 64-bit integer division is emulated on Apple GPUs (and costs about as much as the rest of a
 * pixel), so, as in hires.glsl, none is left on the per-pixel path: floor/trunc of 32-bit values
 * by small positive divisors, and float estimates corrected exactly with integer multiplies */
/* floor(a / b) for b > 0 and |a / b| < 2^21: the float quotient is within 1 of it, then exact */
"static inline int floordiv(int a, int b) {\n"
"  int q = int(floor(float(a) / float(b))), r = a - q * b;\n"
"  return r < 0 ? q - 1 : (r >= b ? q + 1 : q); }\n"
/* floor of the 1x plane equation at hi-res pixel (n = 2X+1-Sx, m = 2Y+1-Sy), exactly:
 * E = cint + qx*n + qy*m + floor((cf*D + 65536*G) / (65536*D)), D = 4*Sx*Sy,
 * G = rx*n*2*Sy + ry*m*2*Sx; with G = q*D + r the last term is q + (cf*D + 65536*r >= 65536*D) */
"static inline long attr(device const Prim &p, int i, int n, int m, int sx, int sy) {\n"
"  uint w = p.rxy[i]; int rx = int(w & 0xFFu), ry = int((w >> 8) & 0xFFu), cf = int(w >> 16);\n"
"  int D = 4 * sx * sy, G = rx * n * 2 * sy + ry * m * 2 * sx;\n"
"  int q = floordiv(G, D), r = G - q * D;\n"
"  int c = (cf * D + 65536 * r >= 65536 * D) ? 1 : 0;\n"
"  return p.cint[i] + long(p.qx[i]) * long(n) + long(p.qy[i]) * long(m) + long(q + c); }\n"
/* ceil(N / D), D > 0, for quotients well inside 32 bits (pixel columns) */
"static inline int lceil(long N, long D) {\n"
"  int t = int(clamp(ceil(float(N) / float(D)), -1073741824.0f, 1073741824.0f));\n"
"  long r = N - long(t) * D;\n"   /* want -D < r <= 0 */
"  for (int k = 0; k < 4 && r > 0; k++) { r -= D; t++; }\n"
"  for (int k = 0; k < 4 && r <= -D; k++) { r += D; t--; }\n"
"  return t; }\n"
/* trunc(a / n) for 0 < n <= 8 */
"static inline long ldivs(long a, int n) {\n"
"  if (a >= -2147483647L && a <= 2147483647L) return long(int(a) / n);\n"
"  ulong m = ulong(a < 0 ? -a : a); uint d = uint(n);\n"
"  uint hi = uint(m >> 32), lo = uint(m), qh = hi / d, r = hi - qh * d;\n"
"  uint mid = (r << 16) | (lo >> 16), qm = mid / d; r = mid - qm * d;\n"
"  uint low = (r << 16) | (lo & 0xFFFFu), ql = low / d;\n"
"  long q = long((ulong(qh) << 32) | ulong((qm << 16) | ql));\n"
"  return a < 0 ? -q : q; }\n"
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
/* Colours are uint4 (r, g, b, mask bit) in output units: 5-bit channels for the B5G5R5A1 target,
 * 8-bit for the full-precision RGBA8 one (hp). B5G5R5A1 stores .b in the low 5 bits, so PS1 red
 * goes to .b there. Blend pass (fixed-function semi-transparency): B/2+F/2 as F/2 - 1/4 with a 0.5
 * constant dst factor, B+F/4 as floor(F/4), in the units of the target. */
"static inline float4 pack4(uint4 c, int pass, int mode, int hp) {\n"
"  float r = float(c.x), g = float(c.y), b = float(c.z);\n"
"  if (pass == 1) {\n"
"    if (mode == 0) { r = r * 0.5 - 0.25; g = g * 0.5 - 0.25; b = b * 0.5 - 0.25; }\n"
"    else if (mode == 3) { r = floor(r * 0.25); g = floor(g * 0.25); b = floor(b * 0.25); }\n"
"  }\n"
"  if (hp != 0) return float4(r / 255.0, g / 255.0, b / 255.0, float(c.w));\n"
"  return float4(b / 31.0, g / 31.0, r / 31.0, float(c.w)); }\n"
/* a 15-bit PS1 pixel in output units (full precision: c << 3, the PS1's 8-bit scale) */
"static inline uint4 c15u(uint c, int hp) {\n"
"  uint s = hp != 0 ? 3u : 0u;\n"
"  return uint4((c & 31u) << s, ((c >> 5) & 31u) << s, ((c >> 10) & 31u) << s, (c >> 15) & 1u); }\n"
"static inline uint4 unpack4(float4 t, int hp) {\n"
"  if (hp != 0) return uint4(uint(rint(t.r * 255.0)), uint(rint(t.g * 255.0)), uint(rint(t.b * 255.0)), t.a > 0.5 ? 1u : 0u);\n"
"  return uint4(uint(rint(t.b * 31.0)), uint(rint(t.g * 31.0)), uint(rint(t.r * 31.0)), t.a > 0.5 ? 1u : 0u); }\n"
"static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }\n"
/* filtered texels (hires.c tex_combine / samp_bilinear / samp_xbr, bit for bit): CLUT-looked-up
 * texels combined with integer weights summing to 65536; 0000 = transparent (no colour, the rest
 * renormalised; under half opaque weight = transparent); semi-transparency bit by weighted
 * majority. Result (r, g, b, stp) with 5.8 fixed-point channels. */
"static inline bool tcomb(thread const uint *t, thread const uint *w, int n, thread int4 &o) {\n"
"  uint cov = 0u, sr = 0u, sg = 0u, sb = 0u, ss = 0u;\n"
"  for (int k = 0; k < n; k++) {\n"
"    if (t[k] == 0u || w[k] == 0u) continue;\n"
"    cov += w[k]; sr += w[k] * (t[k] & 31u); sg += w[k] * ((t[k] >> 5) & 31u); sb += w[k] * ((t[k] >> 10) & 31u);\n"
"    if ((t[k] & 0x8000u) != 0u) ss += w[k]; }\n"
"  if (cov < 32768u) return false;\n"
"  if (cov == 65536u) o = int4(int((sr + 128u) >> 8), int((sg + 128u) >> 8), int((sb + 128u) >> 8), ss * 2u >= cov ? 1 : 0);\n"
"  else o = int4(int((sr * 256u + (cov >> 1)) / cov), int((sg * 256u + (cov >> 1)) / cov), int((sb * 256u + (cov >> 1)) / cov), ss * 2u >= cov ? 1 : 0);\n"
"  return true; }\n"
"static inline void clamp_axis(thread int &i, thread int &f, int lo, int hi) { if (i < lo) { i = lo; f = 0; } else if (i >= hi) { i = hi; f = 0; } }\n"
"static inline bool samp_filt(texture2d<ushort> vr, device const Prim &p, uint depth, bool xbr, int iu, int fu, int iv, int fv,\n"
"    int ulo, int uhi, int vlo, int vhi, int sx, int sy, thread int4 &o) {\n"
"  uint t[5] = { 0u, 0u, 0u, 0u, 0u }; uint w[5] = { 0u, 0u, 0u, 0u, 0u }; int n;\n"
"  if (!xbr) {\n"
"    int iu1 = iu < uhi ? iu + 1 : uhi, iv1 = iv < vhi ? iv + 1 : vhi;\n"
"    t[0] = tex_fetch(vr, p, depth, uint(iu), uint(iv));\n"   /* taps with weight 0 are not fetched */
"    if (fu != 0) t[1] = tex_fetch(vr, p, depth, uint(iu1), uint(iv));\n"
"    if (fv != 0) t[2] = tex_fetch(vr, p, depth, uint(iu), uint(iv1));\n"
"    if (fu != 0 && fv != 0) t[3] = tex_fetch(vr, p, depth, uint(iu1), uint(iv1));\n"
"    w[0] = uint((256 - fu) * (256 - fv)); w[1] = uint(fu * (256 - fv)); w[2] = uint((256 - fu) * fv); w[3] = uint(fu * fv);\n"
"    n = 4;\n"
"  } else {\n"
"    int ie = iu, dx = fu, je = iv, dy = fv;\n"
"    if (fu >= 128) { ie = iu + 1; dx = fu - 256; }\n"
"    if (fv >= 128) { je = iv + 1; dy = fv - 256; }\n"
"    int hx = dx >= 0 ? 1 : -1, hy = dy >= 0 ? 1 : -1;\n"
"    t[0] = tex_fetch(vr, p, depth, uint(ie), uint(je));\n"
"    t[1] = tex_fetch(vr, p, depth, uint(min(max(ie + hx, ulo), uhi)), uint(je));\n"
"    t[2] = tex_fetch(vr, p, depth, uint(ie), uint(min(max(je + hy, vlo), vhi)));\n"
"    t[3] = tex_fetch(vr, p, depth, uint(min(max(ie - hx, ulo), uhi)), uint(je));\n"
"    t[4] = tex_fetch(vr, p, depth, uint(ie), uint(min(max(je - hy, vlo), vhi)));\n"
"    w[0] = 65536u; n = 2;\n"
"    if (t[1] == t[2] && t[1] != t[0] && t[1] != t[3] && t[2] != t[4]) {\n"
"      int d8 = abs(dx) + abs(dy) - 128, a = min(max(((d8 * (sx + sy)) >> 1) + 128, 0), 256);\n"
"      w[0] = uint(256 - a) * 256u; w[1] = uint(a) * 256u; }\n"
"  }\n"
"  return tcomb(t, w, n, o); }\n"
/* rect texel coordinate on one axis at hi-res pixel X (hires.c rect_axis): index and 1/256 fraction */
"static inline int2 rect_axis(int t0, int d, int k0, int X, int s) {\n"
"  int D = 2 * s, num = D * t0 + d * (2 * X + 1 - s - D * k0);\n"
"  int q = num / D;\n"
"  if (num % D != 0 && num < 0) q -= 1;\n"
"  return int2(q, ((num - q * D) * 256) / D); }\n"
/* sampled texel (5.8) modulated by the vertex colour, in output units (hires.c tex8_out15/32) */
"static inline uint4 tex_out(int4 t, int cr, int cg, int cb, bool raw, bool dith, int d, int hp) {\n"
"  int r, g, b;\n"
"  if (raw) { int s = hp != 0 ? 5 : 8; r = t.x >> s; g = t.y >> s; b = t.z >> s; }\n"
"  else if (hp != 0) { r = min((t.x * cr) >> 12, 255); g = min((t.y * cg) >> 12, 255); b = min((t.z * cb) >> 12, 255); }\n"
"  else if (dith) { r = clamp255(((t.x * cr) >> 12) + d) >> 3; g = clamp255(((t.y * cg) >> 12) + d) >> 3; b = clamp255(((t.z * cb) >> 12) + d) >> 3; }\n"
"  else { r = min((t.x * cr) >> 15, 31); g = min((t.y * cg) >> 15, 31); b = min((t.z * cb) >> 15, 31); }\n"
"  return uint4(uint(r), uint(g), uint(b), uint(t.w)); }\n"
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
"static inline int span_start(device const Prim &p, int Y, int cx1) {\n"
"  long Y16 = long(Y) * 65536;\n"
"  long ax = p.ev[0], ay = p.ev[1], bx = p.ev[2], by = p.ev[3], cx = p.ev[4], cy = p.ev[5];\n"
"  long px, py, qx, qy;\n"
"  if ((p.kf & (1u << 20)) != 0u) { px = ax; py = ay; qx = cx; qy = cy; }\n"
"  else if (Y16 < by) { px = ax; py = ay; qx = bx; qy = by; }\n"
"  else { px = bx; py = by; qx = cx; qy = cy; }\n"
"  long N = px * (qy - py) + (qx - px) * (Y16 - py), D = (qy - py) * 65536;\n"
"  return max(lceil(N, D), cx1); }\n"
/* one past the last pixel of that span: ceil(right edge x), at most the clip */
"static inline int span_end(device const Prim &p, int Y, int cx2) {\n"
"  long Y16 = long(Y) * 65536;\n"
"  long ax = p.ev[0], ay = p.ev[1], bx = p.ev[2], by = p.ev[3], cx = p.ev[4], cy = p.ev[5];\n"
"  long px, py, qx, qy;\n"
"  if ((p.kf & (1u << 20)) == 0u) { px = ax; py = ay; qx = cx; qy = cy; }\n"
"  else if (Y16 < by) { px = ax; py = ay; qx = bx; qy = by; }\n"
"  else { px = bx; py = by; qx = cx; qy = cy; }\n"
"  long N = px * (qy - py) + (qx - px) * (Y16 - py), D = (qy - py) * 65536;\n"
"  return min(lceil(N, D), cx2); }\n"
/* hires.c's perspective u/v (PGXP): exact (u*q)/q at the start of every 8-pixel run of the
 * span, integer-stepped in between; float here, double there */
/* floor(a * 2^24 / q) for q > 0 without 64-bit division (slow on the GPU): float estimates,
 * corrected exactly with integer multiplies; r < q <= 2^38 keeps r << 24 in range */
"static inline long div24(long a, long q) {\n"
"  float fq = float(q);\n"
"  long t = long(floor(float(a) / fq)), r = a - t * q;\n"
"  for (int k = 0; k < 4 && r < 0; k++) { r += q; t--; }\n"
"  for (int k = 0; k < 4 && r >= q; k++) { r -= q; t++; }\n"
"  long s = r << 24, e = long(float(s) / fq), r2 = s - e * q;\n"
"  for (int k = 0; k < 8 && r2 < 0; k++) { r2 += q; e--; }\n"
"  for (int k = 0; k < 8 && r2 >= q; k++) { r2 -= q; e++; }\n"
"  return t * 16777216 + e; }\n"
/* u (k = 0) and v (k = 1) in 8.24 at hi-res pixel X: q, u*q, v*q planes in 2^-38 */
"static inline long div24r(long a, long q, float rq) {\n"   /* div24 with 1/q computed once for u and v */
"  long t = long(floor(float(a) * rq)), r = a - t * q;\n"
"  for (int k = 0; k < 8 && r < 0; k++) { r += q; t--; }\n"
"  for (int k = 0; k < 8 && r >= q; k++) { r -= q; t++; }\n"
"  long s = r << 24, e = long(float(s) * rq), r2 = s - e * q;\n"
"  for (int k = 0; k < 16 && r2 < 0; k++) { r2 += q; e--; }\n"
"  for (int k = 0; k < 16 && r2 >= q; k++) { r2 -= q; e++; }\n"
"  return t * 16777216 + e; }\n"
"static inline long2 persp_at(device const Prim &p, long X, int Y) {\n"
"  long fx = X - long(p.porg[0]), fy = long(Y - p.porg[1]);\n"
"  long q = p.pl[0] + p.pl[3] * fx + p.pl[6] * fy;\n"
"  if (q <= 0) return long2(8388608, 8388608);\n"
"  long a = p.pl[1] + p.pl[4] * fx + p.pl[7] * fy, b = p.pl[2] + p.pl[5] * fx + p.pl[8] * fy;\n"
"  float rq = 1.0f / float(q);\n"
"  return long2(div24r(a, q, rq) + 8388608, div24r(b, q, rq) + 8388608); }\n"
/* Same values as `n = min(span_end - xr, 8)`, U0 + (X - xr) * trunc((U1 - U0) / n), but cheaper
 * (it was ~2 ms of a 4x6 fight frame): the run is a full 8 pixels unless it is the span's last,
 * which an exact edge comparison tells without the division in span_end; X == xr needs only U0;
 * trunc(a / 8) is a shift; div24r shares one float reciprocal of q between u and v instead of
 * four float divisions (the integer corrections keep it exact). Together 5.7 -> 4.85 ms per 4x6
 * fight frame on an M1 Pro, dumped frames byte-identical. shaders/hires.glsl (Vulkan, D3D12)
 * has the same form (div24r, span_end_beyond, ltdiv8 on uvec2 pairs). */
"static inline bool span_end_beyond(device const Prim &p, int Y, long k) {\n"   /* ceil(right edge) > k */
"  long Y16 = long(Y) * 65536;\n"
"  long ax = p.ev[0], ay = p.ev[1], bx = p.ev[2], by = p.ev[3], cx = p.ev[4], cy = p.ev[5];\n"
"  long px, py, qx, qy;\n"
"  if ((p.kf & (1u << 20)) == 0u) { px = ax; py = ay; qx = cx; qy = cy; }\n"
"  else if (Y16 < by) { px = ax; py = ay; qx = bx; qy = by; }\n"
"  else { px = bx; py = by; qx = cx; qy = cy; }\n"
"  long N = px * (qy - py) + (qx - px) * (Y16 - py), D = (qy - py) * 65536;\n"
"  return N > k * D; }\n"
"static inline long tdiv8(long a) { return a >= 0 ? (a >> 3) : -((-a) >> 3); }\n"
"static inline long2 persp_uv(device const Prim &p, long xs, int cx2, long X, int Y) {\n"
"  long xr = xs + ((X - xs) & ~7L);\n"
"  long2 U0 = persp_at(p, xr, Y);\n"
"  if (X == xr) return U0;\n"
"  long n = 8;\n"
"  if (xr + 8 > long(cx2) || !span_end_beyond(p, Y, xr + 7)) { long xe = long(span_end(p, Y, cx2)); n = xe - xr < 8 ? xe - xr : 8; }\n"
"  long2 U1 = persp_at(p, xr + n, Y);\n"
"  if (n == 8) return long2(U0.x + (X - xr) * tdiv8(U1.x - U0.x), U0.y + (X - xr) * tdiv8(U1.y - U0.y));\n"
"  return long2(U0.x + (X - xr) * ldivs(U1.x - U0.x, int(n)), U0.y + (X - xr) * ldivs(U1.y - U0.y, int(n))); }\n"
/* hires.c's attribute value at X: the plane equation at the span start, then stepped by the
 * per-pixel gradient truncated to an integer (gdx / Sx) */
"static inline long attr_span(device const Prim &p, int i, int xs, int X, int m, int sx, int sy) {\n"
"  int qx = p.qx[i], rx = int(p.rxy[i] & 0xFFu);\n"
/* gdx / Sx truncated, without dividing: gdx = qx*2Sx + rx with 0 <= rx < 2Sx (gdx is negative
 * exactly when qx is, and inexact unless rx is 0 or Sx) */
"  int step = 2 * qx + (rx >= sx ? 1 : 0) + ((qx < 0 && rx != 0 && rx != sx) ? 1 : 0);\n"
"  return attr(p, i, 2 * xs + 1 - sx, m, sx, sy) + long(X - xs) * long(step); }\n"
"fragment float4 fs_prim(VO in [[stage_in]], constant FU &u [[buffer(0)]], device const Prim *prims [[buffer(1)]],\n"
"    texture2d<float> tmp [[texture(0)]], texture2d<uint> putt [[texture(1)]], texture2d<ushort> vram [[texture(2)]]) {\n"
"  device const Prim &p = prims[in.prim];\n"
"  int X = int(in.pos.x) + u.ox, Y = int(in.pos.y) + u.oy;\n"
"  uint kf = p.kf, kind = kf & 15u;\n"
"  int hp = u.p0;\n"
"  int x1 = floordiv(X, u.sx), y1 = Y >= 0 ? floordiv(Y, u.sy) : Y / u.sy;\n"
"  if (kind == 2u) { if (u.pass != 0) discard_fragment(); return pack4(c15u(p.color & 0xFFFFu, hp), 0, 0, hp); }\n"
"  if (kind == 6u) return pack4(c15u(vram_at(vram, uint(x1), uint(y1)), hp), 0, 0, hp);\n"
"  if (kind == 4u) {\n"
"    uint w = putt.read(uint2(uint(x1 - u.rx), uint(y1 - u.ry))).r;\n"
"    if ((w & 0x10000u) == 0u) discard_fragment();\n"
"    return pack4(c15u((w & 0xFFFFu) | ((kf & (1u << 18)) ? 0x8000u : 0u), hp), 0, 0, hp); }\n"
"  if (kind == 3u) {\n"
"    int sb = X - x1 * u.sx, sa = Y - y1 * u.sy;\n"
"    int sx = int(p.aux0 & 0xFFFFu), sy = int(p.aux0 >> 16), dx = int(p.aux1 & 0xFFFFu), dy = int(p.aux1 >> 16);\n"
"    int scol = (sx + ((x1 - dx) & 1023)) & 1023, srow = (sy + ((y1 - dy) & 511)) & 511;\n"
"    uint4 c;\n"
"    if (scol >= u.rx && scol < u.rx + u.rw && srow >= u.ry && srow < u.ry + u.rh)\n"
"      c = unpack4(tmp.read(uint2(uint(scol * u.sx + sb - u.ox), uint(srow * u.sy + sa - u.oy))), hp);\n"
"    else c = c15u(vram_at(vram, uint(scol), uint(srow)), hp);\n"
"    if ((kf & (1u << 18)) != 0u) c.w = 1u;\n"
"    return pack4(c, 0, 0, hp); }\n"
"  int cx1 = int(p.clip0 << 16) >> 16, cy1 = int(p.clip0 >> 16), cx2 = int(p.clip1 << 16) >> 16, cy2 = int(p.clip1 >> 16);\n"
"  if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard_fragment();\n"
"  bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u;\n"
"  bool gour = (kf & (1u << 7)) != 0u, dith = (kf & (1u << 8)) != 0u;\n"
"  bool filt = (kf & (3u << 22)) != 0u, xbr = (kf & (1u << 23)) != 0u;\n"
"  uint depth = (kf >> 9) & 3u;\n"
"  int cr = int(p.color & 255u), cg = int((p.color >> 8) & 255u), cb = int((p.color >> 16) & 255u);\n"
"  uint uu = 0u, vv = 0u;\n"
"  int iu = 0, fu = 0, iv = 0, fv = 0, ulo = 0, uhi = 255, vlo = 0, vhi = 255;\n"
"  if (kind == 0u) {\n"
"    if ((kf & (1u << 19)) != 0u && !covered(p, X, Y)) discard_fragment();\n"
"    int m = 2 * Y + 1 - u.sy, xs = span_start(p, Y, cx1);\n"
"    if (gour) {\n"
"      int c3[3];\n"
"      bool c1x = (kf & (1u << 14)) != 0u;\n"
"      for (int i = 0; i < 3; i++) {\n"
"        long val = attr_span(p, i, xs, X, m, u.sx, u.sy);\n"
"        int lo = int((p.cmin >> (8 * i)) & 255u), hi = int((p.cmax >> (8 * i)) & 255u);\n"
"        long s = c1x ? long(uint(val) >> 24) : (val >> 24);\n"
"        c3[i] = s < long(lo) ? lo : (s > long(hi) ? hi : int(s));\n"
"      }\n"
"      cr = c3[0]; cg = c3[1]; cb = c3[2];\n"
"    }\n"
"    if (tex) {\n"
"      long U, V;\n"
"      if ((kf & (1u << 21)) != 0u) {\n"
"        long2 uv = persp_uv(p, long(xs), cx2, long(X), Y);\n"
"        U = uv.x; V = uv.y;\n"
"      } else { U = attr_span(p, 3, xs, X, m, u.sx, u.sy); V = attr_span(p, 4, xs, X, m, u.sx, u.sy); }\n"
"      ulo = int(p.uvr & 255u); uhi = int((p.uvr >> 8) & 255u); vlo = int((p.uvr >> 16) & 255u); vhi = int(p.uvr >> 24);\n"
"      if (filt) {\n"   /* sample point without the +0.5 rounding bias, footprint inside the 1x-sampled range */
"        long su = U - 8388608, sv = V - 8388608;\n"
"        iu = int(su >> 24); fu = int((su >> 16) & 255); iv = int(sv >> 24); fv = int((sv >> 16) & 255);\n"
"        clamp_axis(iu, fu, ulo, uhi); clamp_axis(iv, fv, vlo, vhi);\n"
"      } else {\n"
"        int ui = int(U >> 24), vi = int(V >> 24);\n"
"        if ((kf & (1u << 15)) != 0u) {\n"
"          ui = ui < ulo ? ulo : (ui > uhi ? uhi : ui);\n"
"          vi = vi < vlo ? vlo : (vi > vhi ? vhi : vi);\n"
"        }\n"
"        uu = uint(ui); vv = uint(vi);\n"
"      }\n"
"    }\n"
"  } else if (kind == 1u) {\n"
"    int x0 = int(p.aux0 << 16) >> 16, y0 = int(p.aux0) >> 16;\n"
"    uint u0 = p.aux1 & 255u, v0 = (p.aux1 >> 8) & 255u;\n"
"    bool fx = (kf & (1u << 16)) != 0u, fy = (kf & (1u << 17)) != 0u;\n"
"    if (tex && fx) u0 |= 1u;\n"
"    if (filt) {\n"   /* range of the drawn columns / rows, signed 16-bit in cmin / cmax */
"      ulo = int(p.cmin << 16) >> 16; vlo = int(p.cmin) >> 16; uhi = int(p.cmax << 16) >> 16; vhi = int(p.cmax) >> 16;\n"
"      int2 a = rect_axis(int(u0), fx ? -1 : 1, x0, X, u.sx), b = rect_axis(int(v0), fy ? -1 : 1, y0, Y, u.sy);\n"
"      iu = a.x; fu = a.y; iv = b.x; fv = b.y;\n"
"      clamp_axis(iu, fu, ulo, uhi); clamp_axis(iv, fv, vlo, vhi);\n"
"    } else {\n"
"      uu = (u0 + uint(fx ? x0 - x1 : x1 - x0)) & 255u;\n"
"      vv = (v0 + uint(fy ? y0 - y1 : y1 - y0)) & 255u;\n"
"    }\n"
"  }\n"
"  int d = dith ? DITH[(y1 & 3) * 4 + (x1 & 3)] : 0;\n"
"  uint4 outc; bool blend;\n"
"  if (tex) {\n"
"    int4 t8;\n"
"    if (filt) { if (!samp_filt(vram, p, depth, xbr, iu, fu, iv, fv, ulo, uhi, vlo, vhi, u.sx, u.sy, t8)) discard_fragment(); }\n"
"    else {\n"
"      uint t = tex_fetch(vram, p, depth, uu, vv);\n"
"      if (t == 0u) discard_fragment();\n"
"      t8 = int4(int(t & 31u) << 8, int((t >> 5) & 31u) << 8, int((t >> 10) & 31u) << 8, int(t >> 15));\n"
"    }\n"
"    outc = tex_out(t8, cr, cg, cb, raw, dith, d, hp);\n"
"    blend = semi && t8.w != 0;\n"
"  } else {\n"
"    int r, g, b;\n"
"    if (hp != 0) { r = cr; g = cg; b = cb; }\n"
"    else if (dith) { r = clamp255(cr + d) >> 3; g = clamp255(cg + d) >> 3; b = clamp255(cb + d) >> 3; }\n"
"    else { r = cr >> 3; g = cg >> 3; b = cb >> 3; }\n"
"    outc = uint4(uint(r), uint(g), uint(b), 0u);\n"
"    blend = semi;\n"
"  }\n"
"  if (blend != (u.pass == 1)) discard_fragment();\n"
"  if ((kf & (1u << 13)) != 0u) outc.w = 1u;\n"
"  return pack4(outc, u.pass, u.mode, hp); }\n"
"struct PO { float4 pos [[position]]; };\n"
"vertex PO vs_full(uint vid [[vertex_id]]) {\n"
"  PO o; o.pos = float4(vid == 1u ? 3.0 : -1.0, vid == 2u ? 3.0 : -1.0, 0.0, 1.0); return o; }\n"
/* target -> RGBA8: n x n box average (SSAA, rounded; hires.c hires_view_to_rgba) of 8-bit channels */
"struct PU { int x, y, n, hp; };\n"
"fragment float4 fs_present(PO in [[stage_in]], constant PU &u [[buffer(0)]], texture2d<float> t [[texture(0)]]) {\n"
"  uint3 s = uint3(0u);\n"
"  int bx = int(in.pos.x) * u.n + u.x, by = int(in.pos.y) * u.n + u.y;\n"
"  for (int j = 0; j < u.n; j++)\n"
"    for (int i = 0; i < u.n; i++) {\n"
"      float4 v = t.read(uint2(uint(bx + i), uint(by + j)));\n"
"      if (u.hp != 0) s += uint3(uint(rint(v.r * 255.0)), uint(rint(v.g * 255.0)), uint(rint(v.b * 255.0)));\n"
"      else s += uint3(uint(rint(v.b * 31.0)), uint(rint(v.g * 31.0)), uint(rint(v.r * 31.0))) * 8u;\n"
"    }\n"
"  uint nn = uint(u.n * u.n);\n"
"  return float4(float3((s + nn / 2u) / nn) / 255.0, 1.0); }\n"
/* ---- fast (approximate) precision: hardware rasterization and interpolation (see "Fast
 * precision" in the header comment; shaders/hires_fast.glsl is the same for Vulkan / D3D12).
 * Vertices carry float target positions, clip w (PGXP depth for perspective-correct u/v, else 1),
 * u/v in texels (with the PS1's +0.5 rounding bias) and an 8-bit colour, interpolated without
 * perspective (the PS1's colours are affine); a 32-byte record per primitive holds the texture
 * page, CLUT, texture window, draw area and UV clamp range. */
"struct FPrim { uint kf, tp, clut, tw, clip0, clip1, ur, vr; };\n"
"struct FVI { float2 pos [[attribute(0)]]; float w [[attribute(1)]]; float2 uv [[attribute(2)]]; float4 col [[attribute(3)]]; uint prim [[attribute(4)]]; };\n"
"struct FVO { float4 pos [[position]]; float2 uv; float3 col [[center_no_perspective]]; uint prim [[flat]]; };\n"
"vertex FVO vs_fast(FVI in [[stage_in]], constant VU &u [[buffer(0)]]) {\n"
"  FVO o; float w = in.w;\n"
"  o.pos = float4((in.pos.x * u.k.x - 1.0) * w, (1.0 - in.pos.y * u.k.y) * w, 0.0, w);\n"
"  o.uv = in.uv; o.col = in.col.rgb * 255.0; o.prim = in.prim; return o; }\n"
"static inline uint f_fetch(texture2d<ushort> vr, uint tp, uint clut, uint tw, uint depth, uint u, uint v) {\n"
"  u = (u & (tw & 0xFFu)) | ((tw >> 8) & 0xFFu);\n"
"  v = (v & ((tw >> 16) & 0xFFu)) | (tw >> 24);\n"
"  uint tpx = tp & 0xFFFFu, row = ((tp >> 16) + v) & 511u, cx = clut & 0xFFFFu, cy = clut >> 16;\n"
"  if (depth == 0u) { uint w = vram_at(vr, tpx + (u >> 2), row); return vram_at(vr, cx + ((w >> ((u & 3u) * 4u)) & 0xFu), cy); }\n"
"  if (depth == 1u) { uint w = vram_at(vr, tpx + (u >> 1), row); return vram_at(vr, cx + ((w >> ((u & 1u) * 8u)) & 0xFFu), cy); }\n"
"  return vram_at(vr, tpx + u, row); }\n"
"static inline bool f_samp(texture2d<ushort> vr, uint tp, uint clut, uint tw, uint depth, bool xbr, int iu, int fu, int iv, int fv,\n"
"    int ulo, int uhi, int vlo, int vhi, int sx, int sy, thread int4 &o) {\n"
"  uint t[5] = { 0u, 0u, 0u, 0u, 0u }; uint w[5] = { 0u, 0u, 0u, 0u, 0u }; int n;\n"
"  if (!xbr) {\n"
"    int iu1 = iu < uhi ? iu + 1 : uhi, iv1 = iv < vhi ? iv + 1 : vhi;\n"
"    t[0] = f_fetch(vr, tp, clut, tw, depth, uint(iu), uint(iv));\n"
"    if (fu != 0) t[1] = f_fetch(vr, tp, clut, tw, depth, uint(iu1), uint(iv));\n"
"    if (fv != 0) t[2] = f_fetch(vr, tp, clut, tw, depth, uint(iu), uint(iv1));\n"
"    if (fu != 0 && fv != 0) t[3] = f_fetch(vr, tp, clut, tw, depth, uint(iu1), uint(iv1));\n"
"    w[0] = uint((256 - fu) * (256 - fv)); w[1] = uint(fu * (256 - fv)); w[2] = uint((256 - fu) * fv); w[3] = uint(fu * fv);\n"
"    n = 4;\n"
"  } else {\n"
"    int ie = iu, dx = fu, je = iv, dy = fv;\n"
"    if (fu >= 128) { ie = iu + 1; dx = fu - 256; }\n"
"    if (fv >= 128) { je = iv + 1; dy = fv - 256; }\n"
"    int hx = dx >= 0 ? 1 : -1, hy = dy >= 0 ? 1 : -1;\n"
"    t[0] = f_fetch(vr, tp, clut, tw, depth, uint(ie), uint(je));\n"
"    t[1] = f_fetch(vr, tp, clut, tw, depth, uint(min(max(ie + hx, ulo), uhi)), uint(je));\n"
"    t[2] = f_fetch(vr, tp, clut, tw, depth, uint(ie), uint(min(max(je + hy, vlo), vhi)));\n"
"    t[3] = f_fetch(vr, tp, clut, tw, depth, uint(min(max(ie - hx, ulo), uhi)), uint(je));\n"
"    t[4] = f_fetch(vr, tp, clut, tw, depth, uint(ie), uint(min(max(je - hy, vlo), vhi)));\n"
"    w[0] = 65536u; n = 2;\n"
"    if (t[1] == t[2] && t[1] != t[0] && t[1] != t[3] && t[2] != t[4]) {\n"
"      int d8 = abs(dx) + abs(dy) - 128, a = min(max(((d8 * (sx + sy)) >> 1) + 128, 0), 256);\n"
"      w[0] = uint(256 - a) * 256u; w[1] = uint(a) * 256u; }\n"
"  }\n"
"  return tcomb(t, w, n, o); }\n"
"fragment float4 fs_fast(FVO in [[stage_in]], constant FU &u [[buffer(0)]], device const FPrim *prims [[buffer(1)]],\n"
"    texture2d<ushort> vram [[texture(0)]], texture2d_array<float> tcc [[texture(1)]], texture2d_array<float> tcs [[texture(2)]],\n"
"    sampler tsm [[sampler(1)]]) {\n"
"  float2 gx = dfdx(in.uv), gy = dfdy(in.uv);\n"   /* texture cache LOD: before any discard */
"  device const FPrim &p = prims[in.prim];\n"
"  int X = int(in.pos.x) + u.ox, Y = int(in.pos.y) + u.oy;\n"
"  int cx1 = int(p.clip0 << 16) >> 16, cy1 = int(p.clip0 >> 16), cx2 = int(p.clip1 << 16) >> 16, cy2 = int(p.clip1 >> 16);\n"
"  if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard_fragment();\n"
"  uint kf = p.kf; int hp = u.p0;\n"
"  bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u, dith = (kf & (1u << 8)) != 0u;\n"
"  int3 c = int3(clamp(floor(in.col + 0.5), 0.0, 255.0));\n"
"  int d = 0;\n"
"  if (dith) { int x1 = floordiv(X, u.sx), y1 = floordiv(Y, u.sy); d = DITH[(y1 & 3) * 4 + (x1 & 3)]; }\n"
"  uint4 outc; bool blend;\n"
"  if (tex) {\n"
"    uint depth = (kf >> 9) & 3u; int4 t8;\n"
"    int ulo = int(p.ur << 16) >> 16, uhi = int(p.ur) >> 16, vlo = int(p.vr << 16) >> 16, vhi = int(p.vr) >> 16;\n"
"    if ((kf & (1u << 24)) != 0u) {\n"   /* texture cache: layer p.tp, premultiplied, mipmapped (header comment) */
"      float2 cuv = clamp(in.uv, float2(float(ulo) + 0.5, float(vlo) + 0.5), float2(float(uhi) + 0.5, float(vhi) + 0.5));\n"
"      float4 tc; float st = 0.0; bool stp = (kf & (1u << 26)) != 0u;\n"
"      if ((kf & (1u << 25)) != 0u && max(length(gx), length(gy)) <= 1.0) {\n"   /* filter off, magnified: nearest texel */
"        uint2 ip = uint2(clamp(floor(cuv), 0.0, 255.0));\n"
"        tc = tcc.read(ip, p.tp, 0); if (stp) st = tcs.read(ip, p.tp, 0).r;\n"
"      } else {\n"
"        gradient2d g = gradient2d(gx * (1.0 / 256.0), gy * (1.0 / 256.0));\n"
"        tc = tcc.sample(tsm, cuv * (1.0 / 256.0), p.tp, g); if (stp) st = tcs.sample(tsm, cuv * (1.0 / 256.0), p.tp, g).r;\n"
"      }\n"
"      if (tc.a < 0.5) discard_fragment();\n"   /* transparent when opaque texels cover less than half */
"      int3 c8 = min(int3(tc.rgb / tc.a * 8160.0 + 0.5), int3(7936));\n"   /* 5.8 fixed point: c5 << 3 / 255 * 8160 = c5 << 8 */
"      t8 = int4(c8, st * 2.0 >= tc.a ? 1 : 0);\n"   /* STP: majority of the opaque coverage */
"    } else if ((kf & (3u << 22)) != 0u) {\n"   /* filter sample point: without the rounding bias */
"      float2 s = in.uv - 0.5, f0 = floor(s);\n"
"      int iu = int(f0.x), iv = int(f0.y), fu = min(int((s.x - f0.x) * 256.0), 255), fv = min(int((s.y - f0.y) * 256.0), 255);\n"
"      clamp_axis(iu, fu, ulo, uhi); clamp_axis(iv, fv, vlo, vhi);\n"
"      if (!f_samp(vram, p.tp, p.clut, p.tw, depth, (kf & (1u << 23)) != 0u, iu, fu, iv, fv, ulo, uhi, vlo, vhi, u.sx, u.sy, t8)) discard_fragment();\n"
"    } else {\n"
"      int iu = int(floor(in.uv.x)), iv = int(floor(in.uv.y));\n"
"      if ((kf & (1u << 15)) != 0u) { iu = clamp(iu, ulo, uhi); iv = clamp(iv, vlo, vhi); }\n"
"      uint t = f_fetch(vram, p.tp, p.clut, p.tw, depth, uint(iu), uint(iv));\n"
"      if (t == 0u) discard_fragment();\n"
"      t8 = int4(int(t & 31u) << 8, int((t >> 5) & 31u) << 8, int((t >> 10) & 31u) << 8, int(t >> 15));\n"
"    }\n"
"    outc = tex_out(t8, c.x, c.y, c.z, raw, dith, d, hp);\n"
"    blend = semi && t8.w != 0;\n"
"  } else {\n"
"    int3 o3;\n"
"    if (hp != 0) o3 = c;\n"
"    else if (dith) o3 = int3(clamp255(c.x + d) >> 3, clamp255(c.y + d) >> 3, clamp255(c.z + d) >> 3);\n"
"    else o3 = c >> 3;\n"
"    outc = uint4(uint(o3.x), uint(o3.y), uint(o3.z), 0u);\n"
"    blend = semi;\n"
"  }\n"
"  if (blend != (u.pass == 1)) discard_fragment();\n"
"  if ((kf & (1u << 13)) != 0u) outc.w = 1u;\n"
"  return pack4(outc, u.pass, u.mode, hp); }\n";

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

enum { ST_DRAW, ST_VRAM_UP, ST_PUT_UP, ST_TCOPY, ST_TC_UP, ST_EXTERNAL };
/* ST_TC_UP: one mip level of a texture-cache layer (key = layer, x = level, y = 1 for the STP
 * array, w = h = level size) */
typedef void (*ExtFn)(void *ctx, void *cmdbuf, void *colour_tex, int w, int h);
/* ST_EXTERNAL: HiresBackend.external (fn, ctx), run between passes */
typedef struct { int type, key; u32 first, count; int x, y, w, h; size_t off; ExtFn fn; void *ctx; } Step;

/* ---- state -------------------------------------------------------------------------- */

static SDL_GPUDevice *s_dev;
static SDL_GPUGraphicsPipeline *s_pipe[2][NKEYS], *s_present_pipe;   /* [hp][key] */
static SDL_GPUGraphicsPipeline *s_fpipe[2][NKEYS];                    /* fast precision: [hp][key] */
static bool s_fast;            /* fast precision: tris and rects with the fast shaders (hires_gpu_set_fast) */
static SDL_GPUSampler *s_samp;
static SDL_GPUTexture *s_vramtex, *s_target, *s_temp, *s_puttex, *s_present;
static int s_pw, s_ph;
static SDL_GPUBuffer *s_vbuf, *s_pbuf, *s_fvbuf, *s_fpbuf;
static u32 s_vbuf_cap, s_pbuf_cap, s_fvbuf_cap, s_fpbuf_cap;
static SDL_GPUTransferBuffer *s_up, *s_down;
static u32 s_up_cap, s_down_cap;
static SDL_GPUFence *s_fences[2];
static int s_fence_i;
static bool s_inited, s_ready;
static SDL_GPUShaderFormat s_shfmt;   /* MSL (Metal), SPIR-V (Vulkan) or DXIL (Direct3D 12) */
static bool s_force_edge;   /* exact edge test for every triangle (Vulkan; DESTREGA_GPU_EDGE=0/1) */
static int s_present_x = -1, s_present_y = -1;   /* view origin of the last present pass */

static const u16 *s_vram;
static int s_Sx, s_Sy, s_rx, s_ry, s_rw, s_rh, s_tw, s_th, s_ox, s_oy;
static int s_gx, s_pend_gx;   /* guard band in use / for the next configure() */
static int s_hp;              /* target format: 0 = B5G5R5A1 (15-bit), 1 = R8G8B8A8 (full precision) */
static int s_ssaa_present = 1; /* box filter of the present pass (the view's ssaa) */
/* Allocated target size: the next powers of two above s_tw x s_th, so the pixel <-> NDC
 * transform is exact. With 480*Sy rows it rounds; vertices then land ~1e-5 px off pixel
 * centres, which Apple GPUs resolve, breaking fill-rule ties on shared edges. */
static int s_aw, s_ah;

static Vec s_verts = { .esz = sizeof(GVtx) }, s_prims = { .esz = sizeof(GPrim) },
           s_steps = { .esz = sizeof(Step) }, s_arena = { .esz = 1 },
           s_fverts = { .esz = sizeof(FVtx) }, s_fprims = { .esz = sizeof(FPrim) };
static Vec s_gv[NGKEYS];     /* group vertices per key: GVtx (exact keys) or FVtx (fast keys) */
static u64 s_gm[NGKEYS][8];
static u32 s_gused;

static u64 s_dirty[32];    /* 1x VRAM blocks (16x16) changed since uploaded to s_vramtex */

static u32 *s_put;         /* CPU->VRAM pixels pending for the target (region coords), bit 16 = set */
static int s_put_n, s_pbx0, s_pby0, s_pbx1, s_pby1;
static bool s_put_mc, s_put_ms;

static void *s_rb;         /* readback pixels (u16 or u32 per pixel, by s_hp) */
static size_t s_rb_cap;
static bool s_dirty_present = true;

static HiresGpuStats s_st;
void hires_gpu_get_stats(HiresGpuStats *st) { *st = s_st; }
void hires_gpu_set_fast(bool on) { s_fast = on; }
bool hires_gpu_get_fast(void) { return s_fast; }

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

/* Texture-cache write stamps ("Texture cache" below): a clock bumped on every VRAM write, and per
 * 16x16 block / per 64x256 page tile the clock of its last write. An entry checked at clock t is
 * known current while none of its blocks has a newer stamp. */
static u64 s_wclock = 1;
static u64 s_bst[32][64];   /* [block row][block column] */
static u64 s_tst[2][16];    /* [y / 256][x / 64]: the newest stamp of the tile's blocks */

static void tc_stamp(u64 cm, int by0, int by1)
{
    const u64 c = ++s_wclock;
    for (int by = by0; by <= by1; by++) {
        u64 m = cm;
        while (m) {
            const int b = __builtin_ctzll(m);
            m &= m - 1;
            s_bst[by & 31][b] = c;
            s_tst[(by & 31) >> 4][b >> 2] = c;
        }
    }
}

static void mark_vram(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || g_hires_shadow) return;   /* guard-band packets never touch VRAM */
    u64 cm = block_cols(x, w);
    int by0 = (y & 511) >> 4, by1 = ((y & 511) + imin(h, VRAM_H) - 1) >> 4;
    if (by1 - by0 >= 31) { by0 = 0; by1 = 31; }
    for (int by = by0; by <= by1; by++) s_dirty[by & 31] |= cm;
    tc_stamp(cm, by0, by1);
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
    for (int k = 0; k < NGKEYS; k++) {
        if (!s_gv[k].n) continue;
        Vec *dst = k < NKEYS ? &s_verts : &s_fverts;
        Step *st = add_step(ST_DRAW);
        st->key = k;
        st->first = (u32)dst->n;
        st->count = (u32)s_gv[k].n;
        memcpy(vec_push(dst, s_gv[k].n), s_gv[k].p, s_gv[k].n * dst->esz);
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

/* ---- texture cache (fast precision; header comment "Texture cache") ----------------------------- */

#define TC_DIM 256
#define TC_MAX_LEVELS 9
#define TC_MAX_LAYERS 256
static int s_tc_mode = HGPU_TC_ON;          /* HGPU_TC_OFF / ON / VERIFY */
static int s_tc_verify_every = 60;          /* verify: frames between checks */
static int s_tc_layers = 128;               /* entries = array layers (DESTREGA_TC_LAYERS) */
static int s_tc_levels = 4;                 /* mip levels: blocks up to 8x8 texels (DESTREGA_TC_LEVELS) */
static int s_tc_aniso = 16;                 /* sampler max anisotropy, 1 = off (DESTREGA_TC_ANISO) */
static bool s_tc_diag;                      /* DESTREGA_TC_DIAG=1: re-decode log, UV alignment / LOD histograms */
static SDL_GPUTexture *s_tccol, *s_tcstp;   /* RGBA8 and R8 2D arrays, s_tc_layers x 256 x 256, s_tc_levels */
static SDL_GPUTexture *s_tcdummy;           /* 1x1x1 array bound while the cache has no arrays */
static SDL_GPUSampler *s_tcsamp;
static bool s_tc_failed;                    /* the arrays could not be created: cache off */

typedef struct {
    u32 tp, clut, tw;     /* key: page origin x | y << 16, CLUT origin (0 for 15-bit), texture window (FPrim.tw) */
    int depth;            /* key: 0 = 4-bit, 1 = 8-bit, 2 = 15-bit */
    bool live, stp;       /* stp: some texel has bit 15 */
    int next;             /* hash chain */
    u64 checked;          /* s_wclock when the content was last known to equal VRAM */
    u64 page_h, clut_h;   /* hashes of the VRAM it was decoded from */
    u64 content_h;        /* fingerprint of the decoded level 0 (hires_gpu_tc_layer) */
    u64 last_frame;       /* LRU (s_tc_frame) */
} TcEntry;
static TcEntry s_tc[TC_MAX_LAYERS];
static int s_tc_bucket[512];
static u64 s_tc_frame = 1;                  /* be_view count */
static u64 s_tc_alignh[9], s_tc_lodh[6], s_tc_lodpx[6];   /* DESTREGA_TC_DIAG */
static u64 s_tc_newdepth[3], s_tc_newwin;                  /* DESTREGA_TC_DIAG: new keys by depth, with a window */
static int s_tc_verify_next;                /* verify: next layer read back */

static inline int tc_page_cols(int depth) { return depth == 0 ? 64 : depth == 1 ? 128 : 256; }

static u64 tc_hash_rows(int x, int y, int w, int h)
{
    u64 hv = 0x9E3779B97F4A7C15ull ^ ((u64)x << 32) ^ ((u64)y << 16) ^ (u64)w;
    for (int j = 0; j < h; j++) {
        const u16 *r = s_vram + (size_t)((y + j) & 511) * VRAM_W;
        for (int i = 0; i < w; i += 4) {   /* w is a multiple of 4 (16, 64, 128, 256) */
            u64 v;
            const int xx = (x + i) & 1023;
            if (xx + 4 <= VRAM_W) memcpy(&v, r + xx, 8);
            else v = (u64)r[xx] | (u64)r[(xx + 1) & 1023] << 16 | (u64)r[(xx + 2) & 1023] << 32 | (u64)r[(xx + 3) & 1023] << 48;
            hv = (hv ^ v) * 0x100000001B3ull;
            hv ^= hv >> 29;
        }
    }
    return hv;
}
static u64 tc_page_hash(const TcEntry *e) { return tc_hash_rows((int)(e->tp & 0xFFFF), (int)(e->tp >> 16), tc_page_cols(e->depth), TC_DIM); }
static u64 tc_clut_hash(const TcEntry *e)
{
    return e->depth < 2 ? tc_hash_rows((int)(e->clut & 0xFFFF), (int)(e->clut >> 16), e->depth == 0 ? 16 : 256, 1) : 0;
}

/* newest write stamp of the entry's page tiles / CLUT blocks */
static u64 tc_page_stamp(const TcEntry *e)
{
    const int tx = (int)(e->tp & 0xFFFF) >> 6, ty = ((int)(e->tp >> 16) & 511) >> 8, n = tc_page_cols(e->depth) >> 6;
    u64 s = 0;
    for (int i = 0; i < n; i++) if (s_tst[ty][(tx + i) & 15] > s) s = s_tst[ty][(tx + i) & 15];
    return s;
}
static u64 tc_clut_stamp(const TcEntry *e)
{
    if (e->depth >= 2) return 0;
    const int bx = (int)(e->clut & 0xFFFF) >> 4, by = ((int)(e->clut >> 16) & 511) >> 4, n = e->depth == 0 ? 1 : 16;
    u64 s = 0;
    for (int i = 0; i < n; i++) if (s_bst[by][(bx + i) & 63] > s) s = s_bst[by][(bx + i) & 63];
    return s;
}

/* The 15-bit texel the PS1 samples at (u, v) for this key (texture window, CLUT): what hires.c /
 * the fast shader's f_fetch read from VRAM. */
static inline u16 tc_texel(const TcEntry *e, u32 u, u32 v)
{
    u = (u & (e->tw & 0xFF)) | ((e->tw >> 8) & 0xFF);
    v = (v & ((e->tw >> 16) & 0xFF)) | (e->tw >> 24);
    const u32 tpx = e->tp & 0xFFFF, row = ((e->tp >> 16) + v) & 511, cx = e->clut & 0xFFFF, cy = e->clut >> 16;
    const u16 *r = s_vram + (size_t)row * VRAM_W;
    if (e->depth == 0) return s_vram[(size_t)cy * VRAM_W + ((cx + ((r[(tpx + (u >> 2)) & 1023] >> ((u & 3) * 4)) & 0xF)) & 1023)];
    if (e->depth == 1) return s_vram[(size_t)cy * VRAM_W + ((cx + ((r[(tpx + (u >> 1)) & 1023] >> ((u & 1) * 8)) & 0xFF)) & 1023)];
    return r[(tpx + u) & 1023];
}

/* Level 0 as stored: premultiplied RGBA8 with the 5-bit channels as c << 3 (so the shader gets
 * c << 8 back exactly from an unfiltered texel), alpha 0 for texel 0 (transparent) and 255 else;
 * STP coverage 255 for opaque texels with bit 15. Returns whether any texel has bit 15. */
static bool tc_decode0(const TcEntry *e, u32 *col, u8 *stp)
{
    bool any = false;
    for (u32 v = 0; v < TC_DIM; v++)
        for (u32 u = 0; u < TC_DIM; u++) {
            const u16 t = tc_texel(e, u, v);
            const size_t i = (size_t)v * TC_DIM + u;
            if (!t) { col[i] = 0; stp[i] = 0; continue; }
            col[i] = (u32)(t & 31) << 3 | (u32)((t >> 5) & 31) << 11 | (u32)((t >> 10) & 31) << 19 | 0xFF000000u;
            stp[i] = (t & 0x8000) ? 255 : 0;
            any |= (t & 0x8000) != 0;
        }
    return any;
}

/* Decode the entry from the current VRAM and queue the upload of all its levels to its layer
 * (after the draws recorded so far, which sampled the old content). Mip levels are 2x2 averages of
 * the premultiplied values: transparent texels add no colour (no dark fringe), alpha is the
 * coverage and the STP array the coverage of bit-15 texels. Computed here, not with
 * SDL_GenerateMipmapsForGPUTexture: that regenerates every layer of the array, and doing it on
 * the CPU gives the same levels on Metal, Vulkan and Direct3D 12. */
static void tc_decode(TcEntry *e, int layer)
{
    const u64 t0 = SDL_GetTicksNS();
    close_group();
    size_t ocol[TC_MAX_LEVELS], ostp[TC_MAX_LEVELS];
    for (int l = 0, d = TC_DIM; l < s_tc_levels; l++, d >>= 1) {
        ocol[l] = arena_alloc((size_t)d * d * 4);
        ostp[l] = arena_alloc((size_t)d * d);
    }
    u8 *base = s_arena.p;   /* arena_alloc may move it: take the pointer after all allocations */
    e->stp = tc_decode0(e, (u32 *)(base + ocol[0]), base + ostp[0]);
    {   /* content fingerprint (FNV-1a over the decoded colour + STP texels): position independent */
        const u32 *c0 = (const u32 *)(base + ocol[0]);
        const u8 *s0 = base + ostp[0];
        u64 h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < (size_t)TC_DIM * TC_DIM; i++) {
            h = (h ^ (c0[i] | (u32)(s0[i] != 0) << 2)) * 0x100000001B3ull;
        }
        e->content_h = h;
    }
    for (int l = 1, d = TC_DIM / 2; l < s_tc_levels; l++, d >>= 1) {
        const u8 *pc = base + ocol[l - 1], *ps = base + ostp[l - 1];
        u8 *nc = base + ocol[l], *ns = base + ostp[l];
        const int pd = d * 2;
        for (int y = 0; y < d; y++)
            for (int x = 0; x < d; x++) {
                const size_t a = ((size_t)(2 * y) * pd + 2 * x), b = a + pd;
                for (int c = 0; c < 4; c++)
                    nc[((size_t)y * d + x) * 4 + c] = (u8)((pc[a * 4 + c] + pc[a * 4 + 4 + c] + pc[b * 4 + c] + pc[b * 4 + 4 + c] + 2) >> 2);
                ns[(size_t)y * d + x] = (u8)((ps[a] + ps[a + 1] + ps[b] + ps[b + 1] + 2) >> 2);
            }
    }
    for (int l = 0, d = TC_DIM; l < s_tc_levels; l++, d >>= 1)
        for (int k = 0; k < 2; k++) {
            Step *st = add_step(ST_TC_UP);
            st->key = layer; st->x = l; st->y = k; st->w = st->h = d; st->off = k ? ostp[l] : ocol[l];
        }
    e->page_h = tc_page_hash(e);
    e->clut_h = tc_clut_hash(e);
    e->checked = s_wclock;
    s_st.tc_decode_ns += SDL_GetTicksNS() - t0;
}

static inline u32 tc_bucket(u32 tp, u32 clut, u32 tw, int depth)
{
    u32 h = tp * 0x9E3779B1u ^ clut * 0x85EBCA77u ^ tw * 0xC2B2AE3Du ^ (u32)depth * 0x27D4EB2Fu;
    return (h ^ (h >> 15)) & 511;
}
static void tc_unlink(int i)
{
    TcEntry *e = &s_tc[i];
    int *pp = &s_tc_bucket[tc_bucket(e->tp, e->clut, e->tw, e->depth)];
    while (*pp >= 0 && *pp != i) pp = &s_tc[*pp].next;
    if (*pp == i) *pp = e->next;
    e->live = false;
}
static void tc_clear(void)
{
    memset(s_tc, 0, sizeof s_tc);
    for (int i = 0; i < 512; i++) s_tc_bucket[i] = -1;
}

static bool tc_alloc_arrays(void)
{
    if (s_tccol) return true;
    if (s_tc_failed) return false;
    SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = TC_DIM, .height = TC_DIM,
                                    .layer_count_or_depth = (u32)s_tc_layers, .num_levels = (u32)s_tc_levels };
    s_tccol = SDL_CreateGPUTexture(s_dev, &ti);
    ti.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    s_tcstp = s_tccol ? SDL_CreateGPUTexture(s_dev, &ti) : NULL;
    if (!s_tcstp) {
        rt_log("hires-gpu: texture cache: cannot allocate %d layers: %s; texture cache off", s_tc_layers, SDL_GetError());
        if (s_tccol) SDL_ReleaseGPUTexture(s_dev, s_tccol);
        s_tccol = NULL;
        s_tc_failed = true;
        return false;
    }
    tc_clear();
    return true;
}

/* Layer holding (page, CLUT, depth, window) of this textured primitive, decoded and current; -1:
 * not cached (cache off or full: every layer is in use this frame), the caller samples VRAM. */
static int tc_lookup(const HiresPrim *p)
{
    if (!tc_alloc_arrays()) return -1;
    const int depth = p->depth >= 2 ? 2 : p->depth;
    const u32 tp = (p->tp_x & 1023) | (p->tp_y & 511) << 16;
    const u32 clut = depth < 2 ? ((p->clut_x & 1023) | (p->clut_y & 511) << 16) : 0;
    const u32 tw = (p->tw_and_u & 0xFF) | ((p->tw_or_u & 0xFF) << 8) | ((p->tw_and_v & 0xFF) << 16) | ((p->tw_or_v & 0xFF) << 24);
    const u32 bk = tc_bucket(tp, clut, tw, depth);
    s_st.tc_lookups++;
    for (int i = s_tc_bucket[bk]; i >= 0; i = s_tc[i].next) {
        TcEntry *e = &s_tc[i];
        if (e->tp != tp || e->clut != clut || e->tw != tw || e->depth != depth) continue;
        if (e->checked != s_wclock) {   /* VRAM was written since: did it touch this entry's texels? */
            const bool pn = tc_page_stamp(e) > e->checked, cn = tc_clut_stamp(e) > e->checked;
            if (pn || cn) {
                if ((pn && tc_page_hash(e) != e->page_h) || (cn && tc_clut_hash(e) != e->clut_h)) {
                    if (s_tc_diag && s_st.tc_redecodes < 200)
                        rt_log("tex-cache: re-decode page %u,%u depth %d CLUT %u,%u (%s changed) frame %llu",
                               tp & 0xFFFF, tp >> 16, depth, clut & 0xFFFF, clut >> 16, pn ? "page" : "CLUT",
                               (unsigned long long)s_tc_frame);
                    tc_decode(e, i);
                    s_st.tc_redecodes++;
                } else s_st.tc_same++;
            }
            e->checked = s_wclock;
        }
        e->last_frame = s_tc_frame;
        s_st.tc_hits++;
        return i;
    }
    int pick = -1;   /* a free layer, else the least recently used one not used this frame */
    for (int i = 0; i < s_tc_layers; i++) {
        if (!s_tc[i].live) { pick = i; break; }
        if (s_tc[i].last_frame < s_tc_frame && (pick < 0 || s_tc[i].last_frame < s_tc[pick].last_frame)) pick = i;
    }
    if (pick < 0) { s_st.tc_full++; return -1; }
    TcEntry *e = &s_tc[pick];
    if (e->live) { tc_unlink(pick); s_st.tc_evictions++; }
    e->tp = tp; e->clut = clut; e->tw = tw; e->depth = depth;
    e->live = true;
    e->next = s_tc_bucket[bk];
    s_tc_bucket[bk] = pick;
    e->last_frame = s_tc_frame;
    tc_decode(e, pick);
    s_st.tc_decodes++;
    if (s_tc_diag) {
        s_tc_newdepth[depth]++;
        if (tw != 0x00FF00FFu) {
            s_tc_newwin++;
            rt_log("tex-cache: texture window and %02x/%02x or %02x/%02x on page %u,%u depth %d (frame %llu)", tw & 0xFF,
                   (tw >> 16) & 0xFF, (tw >> 8) & 0xFF, tw >> 24, tp & 0xFFFF, tp >> 16, depth, (unsigned long long)s_tc_frame);
        }
    }
    int live = 0;
    for (int i = 0; i < s_tc_layers; i++) live += s_tc[i].live;
    if ((u64)live > s_st.tc_max_live) s_st.tc_max_live = (u64)live;
    return pick;
}

static SDL_GPUTransferBuffer *s_tcdown;
static u32 s_tcdown_cap;
static void wait_fence(SDL_GPUFence **f);
static bool submit(bool readback, int rx, int ry, int rw, int rh, bool present, int px, int py);

/* Sampler and the placeholder array; the cache arrays themselves are created on first use. */
static bool tc_init(SDL_GPUDevice *dev)
{
    const char *e;
    if ((e = SDL_getenv("DESTREGA_TC_LAYERS")) && *e) s_tc_layers = SDL_clamp(atoi(e), 8, TC_MAX_LAYERS);
    if ((e = SDL_getenv("DESTREGA_TC_LEVELS")) && *e) s_tc_levels = SDL_clamp(atoi(e), 1, TC_MAX_LEVELS);
    if ((e = SDL_getenv("DESTREGA_TC_ANISO")) && *e) s_tc_aniso = SDL_clamp(atoi(e), 1, 16);
    s_tc_diag = (e = SDL_getenv("DESTREGA_TC_DIAG")) && *e && *e != '0';
    SDL_GPUSamplerCreateInfo sci = { .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
                                     .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
                                     .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .min_lod = 0, .max_lod = (float)(s_tc_levels - 1),
                                     .max_anisotropy = (float)s_tc_aniso, .enable_anisotropy = s_tc_aniso > 1 };
    s_tcsamp = SDL_CreateGPUSampler(dev, &sci);
    SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 1, .height = 1,
                                    .layer_count_or_depth = 1, .num_levels = 1 };
    s_tcdummy = SDL_CreateGPUTexture(dev, &ti);
    tc_clear();
    return s_tcsamp && s_tcdummy;
}

/* --tex-cache verify: every entry whose page and CLUT were not written since it was last known
 * current must still hash to what it was decoded from (else the write tracking missed a write:
 * STALE); and one layer per round is read back from the GPU and compared with a fresh decode of
 * the VRAM (upload path, layer bookkeeping). Called after the frame's command buffer was submitted. */
static void tc_verify(void)
{
    for (int i = 0; i < s_tc_layers; i++) {
        TcEntry *e = &s_tc[i];
        if (!e->live || tc_page_stamp(e) > e->checked || tc_clut_stamp(e) > e->checked) continue;
        s_st.tc_verified++;
        const bool pb = tc_page_hash(e) != e->page_h, cb = tc_clut_hash(e) != e->clut_h;
        if (pb || cb) {
            s_st.tc_stale++;
            rt_log("tex-cache: STALE entry layer %d page %u,%u depth %d CLUT %u,%u: %s changed without a tracked write "
                   "(frame %llu)", i, e->tp & 0xFFFF, e->tp >> 16, e->depth, e->clut & 0xFFFF, e->clut >> 16,
                   pb ? "page" : "CLUT", (unsigned long long)s_tc_frame);
        }
    }
    int layer = -1;
    for (int k = 0; k < s_tc_layers && layer < 0; k++) {
        const int i = (s_tc_verify_next + k) % s_tc_layers;
        const TcEntry *e = &s_tc[i];
        if (e->live && tc_page_stamp(e) <= e->checked && tc_clut_stamp(e) <= e->checked &&
            tc_page_hash(e) == e->page_h && tc_clut_hash(e) == e->clut_h) layer = i;
    }
    if (layer < 0) return;
    s_tc_verify_next = layer + 1;
    const u32 nc = TC_DIM * TC_DIM * 4, ns = TC_DIM * TC_DIM;
    if (!s_tcdown || s_tcdown_cap < nc + ns) {
        if (s_tcdown) SDL_ReleaseGPUTransferBuffer(s_dev, s_tcdown);
        SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = nc + ns };
        s_tcdown = SDL_CreateGPUTransferBuffer(s_dev, &ti);
        s_tcdown_cap = s_tcdown ? nc + ns : 0;
        if (!s_tcdown) return;
    }
    SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(s_dev);
    if (!cb) return;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    for (int k = 0; k < 2; k++) {
        SDL_GPUTextureRegion src = { .texture = k ? s_tcstp : s_tccol, .layer = (u32)layer, .w = TC_DIM, .h = TC_DIM, .d = 1 };
        SDL_GPUTextureTransferInfo dst = { .transfer_buffer = s_tcdown, .offset = k ? nc : 0 };
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
    }
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    if (!f) return;
    wait_fence(&f);
    const u8 *m = SDL_MapGPUTransferBuffer(s_dev, s_tcdown, false);
    if (!m) return;
    u32 *col = malloc(nc);
    u8 *stp = malloc(ns);
    if (col && stp) {
        tc_decode0(&s_tc[layer], col, stp);
        s_st.tc_readbacks++;
        size_t bad = 0;
        for (size_t i = 0; i < TC_DIM * TC_DIM; i++) {
            u32 g;
            memcpy(&g, m + i * 4, 4);
            bad += g != col[i] || m[nc + i] != stp[i];
        }
        if (bad) {
            s_st.tc_readback_bad++;
            rt_log("tex-cache: layer %d on the GPU differs from VRAM in %zu texels (frame %llu)", layer, bad,
                   (unsigned long long)s_tc_frame);
        }
    }
    free(col);
    free(stp);
    SDL_UnmapGPUTransferBuffer(s_dev, s_tcdown);
}

void hires_gpu_set_tex_cache(int mode, int every)
{
    s_tc_mode = mode < HGPU_TC_OFF || mode > HGPU_TC_VERIFY ? HGPU_TC_ON : mode;
    if (every > 0) s_tc_verify_every = every;
}
int hires_gpu_get_tex_cache(void) { return s_tc_mode; }
void hires_gpu_tc_check(void)
{
    if (!s_ready || !s_tccol) return;
    submit(false, 0, 0, 0, 0, false, 0, 0);
    tc_verify();
}

/* DESTREGA_TC_DIAG: how the cached triangles' vertex UV rectangles align to power-of-two blocks
 * (mip bleed) and their texel-per-pixel ratio at the render scale (which mip levels they use). */
static void tc_diag_tri(const HiresVtx *v, int Sx, int Sy)
{
    int u0 = (int)v[0].u, u1 = u0, w0 = (int)v[0].v, w1 = w0;
    for (int k = 1; k < 3; k++) {
        u0 = imin(u0, (int)v[k].u); u1 = imax(u1, (int)v[k].u);
        w0 = imin(w0, (int)v[k].v); w1 = imax(w1, (int)v[k].v);
    }
    /* end alignment: the polygon's last u is the tile's last texel (u1 + 1) or its edge (u1) */
    const int a0 = __builtin_ctz((u32)u0 | 256), a1 = imax(__builtin_ctz((u32)(u1 + 1) | 256), __builtin_ctz((u32)u1 | 256));
    const int a2 = __builtin_ctz((u32)w0 | 256), a3 = imax(__builtin_ctz((u32)(w1 + 1) | 256), __builtin_ctz((u32)w1 | 256));
    s_tc_alignh[imin(imin(a0, a1), imin(a2, a3))]++;
    const double sa = fabs((double)(v[1].x - v[0].x) * (v[2].y - v[0].y) - (double)(v[2].x - v[0].x) * (v[1].y - v[0].y)) /
                      (65536.0 * 65536.0) * Sx * Sy;
    const double ta = fabs((double)((int)v[1].u - (int)v[0].u) * ((int)v[2].v - (int)v[0].v) -
                           (double)((int)v[2].u - (int)v[0].u) * ((int)v[1].v - (int)v[0].v));
    int b = 0;
    if (sa > 0 && ta > 0) {
        const double lod = 0.5 * log2(ta / sa);
        b = lod < 0 ? 0 : lod < 1 ? 1 : lod < 2 ? 2 : lod < 3 ? 3 : lod < 4 ? 4 : 5;
    }
    s_tc_lodh[b]++;
    s_tc_lodpx[b] += (u64)(sa / 2);
}

void hires_gpu_tc_diag_log(void)
{
    if (!s_tc_diag) return;
    rt_log("tex-cache-diag: new keys: %llu 4-bit, %llu 8-bit, %llu 15-bit; %llu with a texture window",
           (unsigned long long)s_tc_newdepth[0], (unsigned long long)s_tc_newdepth[1], (unsigned long long)s_tc_newdepth[2],
           (unsigned long long)s_tc_newwin);
    rt_log("tex-cache-diag: vertex UV rectangle alignment (largest 2^k dividing all edges), cached triangles: "
           "1:%llu 2:%llu 4:%llu 8:%llu 16:%llu 32:%llu 64:%llu 128:%llu 256:%llu",
           (unsigned long long)s_tc_alignh[0], (unsigned long long)s_tc_alignh[1], (unsigned long long)s_tc_alignh[2],
           (unsigned long long)s_tc_alignh[3], (unsigned long long)s_tc_alignh[4], (unsigned long long)s_tc_alignh[5],
           (unsigned long long)s_tc_alignh[6], (unsigned long long)s_tc_alignh[7], (unsigned long long)s_tc_alignh[8]);
    rt_log("tex-cache-diag: texels per render pixel (area estimate), triangles / pixels: "
           "<1 (magnified) %llu / %llu, 1-2 %llu / %llu, 2-4 %llu / %llu, 4-8 %llu / %llu, 8-16 %llu / %llu, >=16 %llu / %llu",
           (unsigned long long)s_tc_lodh[0], (unsigned long long)s_tc_lodpx[0], (unsigned long long)s_tc_lodh[1],
           (unsigned long long)s_tc_lodpx[1], (unsigned long long)s_tc_lodh[2], (unsigned long long)s_tc_lodpx[2],
           (unsigned long long)s_tc_lodh[3], (unsigned long long)s_tc_lodpx[3], (unsigned long long)s_tc_lodh[4],
           (unsigned long long)s_tc_lodpx[4], (unsigned long long)s_tc_lodh[5], (unsigned long long)s_tc_lodpx[5]);
}

/* ---- groups --------------------------------------------------------------------------- */

static void tile_mask(int x0, int y0, int x1, int y1, u64 m[8])
{
    memset(m, 0, 8 * sizeof(u64));
    if (s_gx) { x0 += s_gx - 1; x1 += s_gx + 1; }   /* guard columns (and truncated divisions) */
    x0 = imax(x0, 0); y0 = imax(y0, 0); x1 = imin(x1, VRAM_W - 1); y1 = imin(y1, VRAM_H - 1);
    if (x0 > x1 || y0 > y1) return;
    int c0 = x0 >> 5, c1 = x1 >> 5;
    u64 row = (c1 - c0 >= 31) ? 0xFFFFFFFFull : (((1ull << (c1 - c0 + 1)) - 1) << c0);
    for (int r = y0 >> 5; r <= (y1 >> 5); r++) m[r >> 1] |= row << ((r & 1) * 32);
}

/* Add a primitive's parts (pipeline keys) to the group. A group is emitted key by key, so a
 * part may not overtake an earlier overlapping part of a later key: close the group then. */
static void group_add(const int *keys, int nk, const void *v, int nv, const u64 m[8])
{
    bool conflict = false;
    for (int i = 0; i < nk && !conflict; i++)
        for (int j = keys[i] + 1; j < NGKEYS && !conflict; j++)
            if (s_gused & (1u << j))
                for (int w = 0; w < 8; w++)
                    if (s_gm[j][w] & m[w]) { conflict = true; break; }
    if (conflict) close_group();
    for (int i = 0; i < nk; i++) {
        int k = keys[i];
        if (!s_gv[k].esz) s_gv[k].esz = k < NKEYS ? sizeof(GVtx) : sizeof(FVtx);
        memcpy(vec_push(&s_gv[k], (size_t)nv), v, (size_t)nv * s_gv[k].esz);
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

static void clip_words(const HiresPrim *p, u32 *c0, u32 *c1)
{
    int cx1 = imax(p->clip_x1 * s_Sx, s_ox), cy1 = imax(p->clip_y1 * s_Sy, s_oy);
    int cx2 = imin((p->clip_x2 + 1) * s_Sx, s_ox + s_tw), cy2 = imin((p->clip_y2 + 1) * s_Sy, s_oy + s_th);
    *c0 = ((u32)cx1 & 0xFFFFu) | ((u32)imax(cy1, 0) << 16);   /* x signed (guard band) */
    *c1 = ((u32)cx2 & 0xFFFFu) | ((u32)imax(cy2, 0) << 16);
}
static void set_clip(GPrim *g, const HiresPrim *p) { clip_words(p, &g->clip0, &g->clip1); }

/* ---- fast precision ------------------------------------------------------------------------ */

static FPrim *new_fprim(u32 *idx, const HiresPrim *p, u32 kf)
{
    *idx = (u32)s_fprims.n;
    FPrim *f = vec_push(&s_fprims, 1);
    memset(f, 0, sizeof *f);
    f->kf = kf;
    f->tp = p->tp_x | (p->tp_y << 16);
    f->clut = p->clut_x | (p->clut_y << 16);
    f->tw = (p->tw_and_u & 0xFF) | ((p->tw_or_u & 0xFF) << 8) | ((p->tw_and_v & 0xFF) << 16) | ((p->tw_or_v & 0xFF) << 24);
    clip_words(p, &f->clip0, &f->clip1);
    s_st.prims++;
    s_st.fast_prims++;
    s_dirty_present = true;
    return f;
}
static inline u32 srange(int lo, int hi) { return ((u32)lo & 0xFFFFu) | ((u32)hi << 16); }   /* signed 16-bit pair */

/* Triangle with hardware rasterization and interpolation. Coverage: the same vertex positions as
 * the exact path (pixel centres on the 1x grid scaled), but the GPU's own fill rule and sub-pixel
 * snapping. Attributes: hires.c samples a hi-res pixel's u/v at the 1x point P = (X + 0.5) / S - 0.5
 * while the hardware interpolates at the pixel centre, i.e. at 1x X / S with these positions; so
 * each vertex gets the triangle's u/v at its own position + (0.5 / S - 0.5), plus the PS1's +0.5
 * rounding bias. With perspective, q = wmin / w and u*q are linear in screen space, so the vertex
 * gets both planes at the shifted point: clip w = 1 / q', u = (u*q)' / q'; the hardware's
 * perspective interpolation then gives exactly hires.c's u/v at P (up to float rounding).
 * Colours are not shifted (sub-pixel). */
static void fast_tri(const HiresPrim *p, const HiresVtx *sv[3], bool persp, u32 kf, int umin, int umax, int vmin, int vmax,
                     int xlo, int ylo, int xhi, int yhi, int layer)
{
    const int Sx = s_Sx, Sy = s_Sy;
    u32 idx;
    FPrim *f = new_fprim(&idx, p, kf);
    if (layer >= 0) f->tp = (u32)layer;   /* F_TC: the cache layer instead of the page origin */
    f->ur = srange(umin, umax);
    f->vr = srange(vmin, vmax);
    double X[3], Y[3], q[3] = { 1, 1, 1 };
    double wmin = 0;
    if (persp) {
        wmin = sv[0]->w;
        for (int k = 1; k < 3; k++) if (sv[k]->w < wmin) wmin = sv[k]->w;
    }
    for (int k = 0; k < 3; k++) {
        X[k] = (double)sv[k]->x / 65536.0;
        Y[k] = (double)sv[k]->y / 65536.0;
        if (persp) q[k] = wmin / (double)sv[k]->w;
    }
    const double den = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    const double dx = 0.5 / Sx - 0.5, dy = 0.5 / Sy - 0.5;
    const bool tex = (kf & F_TEX) != 0, gour = (kf & F_GOUR) != 0;
    FVtx v[3];
    for (int i = 0; i < 3; i++) {
        const HiresVtx *s = sv[i];
        double uu = s->u, vv = s->v, wq = q[i];
        if (tex && fabs(den) > 1e-9) {
            const double px = X[i] + dx, py = Y[i] + dy;
            double l1 = ((px - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (py - Y[0])) / den;
            double l2 = ((X[1] - X[0]) * (py - Y[0]) - (px - X[0]) * (Y[1] - Y[0])) / den;
            double l0 = 1 - l1 - l2;
            const double qs = l0 * q[0] + l1 * q[1] + l2 * q[2];
            if (qs > 1e-9) {   /* q and u*q are screen-linear: their planes at the shifted point */
                uu = (l0 * q[0] * sv[0]->u + l1 * q[1] * sv[1]->u + l2 * q[2] * sv[2]->u) / qs;
                vv = (l0 * q[0] * sv[0]->v + l1 * q[1] * sv[1]->v + l2 * q[2] * sv[2]->v) / qs;
                wq = qs;
            }
        }
        const HiresVtx *cs = gour ? s : sv[0];
        v[i].x = (float)((double)((s64)s->x * Sx) / 65536.0 - s_ox + 0.5);
        v[i].y = (float)((double)((s64)s->y * Sy) / 65536.0 - s_oy + 0.5);
        v[i].w = persp ? (float)(1.0 / wq) : 1.0f;   /* clip w = 1 / q: the hardware's perspective division */
        v[i].u = (float)(uu + 0.5);
        v[i].v = (float)(vv + 0.5);
        v[i].col = (cs->r & 255) | ((cs->g & 255) << 8) | ((cs->b & 255) << 16);
        v[i].prim = idx;
    }
    int keys[2];
    int nk = prim_keys(p, p->semi, keys);
    for (int i = 0; i < nk; i++) keys[i] += NKEYS;
    u64 m[8];
    tile_mask(xlo / Sx, ylo / Sy, (xhi - 1) / Sx, (yhi - 1) / Sy, m);
    group_add(keys, nk, v, 3, m);
}

/* guard band: a draw area that ends at the region's left/right edge extends gx columns further */
static inline void guard_clip(HiresPrim *p)
{
    if (!s_gx || p->clip_x2 < p->clip_x1) return;
    if (p->clip_x1 == s_rx) p->clip_x1 = s_rx - s_gx;
    if (p->clip_x2 == s_rx + s_rw - 1) p->clip_x2 = s_rx + s_rw - 1 + s_gx;
}

/* never in full precision (same as hires.c) */
static inline bool dither_on(const HiresPrim *p) { return p->dither && !s_hp && ((s_Sx == 1 && s_Sy == 1) || hires_get_dither()); }

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
    s_bst[y >> 4][x >> 4] = s_tst[y >> 8][x >> 6] = ++s_wclock;
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

/* Grow the triangle (target coordinates) by 1 px on every edge, so that the GPU's own
 * rasterization surely covers every pixel centre hires.c's rule covers; the shader then tests
 * coverage exactly (F_EDGE). Used for fractional (PGXP) vertices instead of the bounding box.
 * False for slivers whose mitred corners would go far (the caller draws the bounding box). */
static bool conservative_tri(GVtx v[3])
{
    double px[3], py[3], nx[3], ny[3];
    for (int i = 0; i < 3; i++) { px[i] = v[i].x; py[i] = v[i].y; }
    const double cx = (px[0] + px[1] + px[2]) / 3, cy = (py[0] + py[1] + py[2]) / 3;
    for (int k = 0; k < 3; k++) {   /* outward unit normal of edge k -> k+1 */
        int j = (k + 1) % 3;
        double ex = px[j] - px[k], ey = py[j] - py[k], len = sqrt(ex * ex + ey * ey);
        if (len < 1e-6) return false;
        nx[k] = ey / len; ny[k] = -ex / len;
        if (nx[k] * (cx - px[k]) + ny[k] * (cy - py[k]) > 0) { nx[k] = -nx[k]; ny[k] = -ny[k]; }
    }
    GVtx o[3];
    for (int i = 0; i < 3; i++) {   /* vertex i is on edges i-1 and i: offset both lines by 1 */
        int a = (i + 2) % 3;
        double den = 1 + nx[a] * nx[i] + ny[a] * ny[i];
        if (den < 0.02) return false;   /* miter > 10 px */
        o[i].x = (float)(px[i] + (nx[a] + nx[i]) / den);
        o[i].y = (float)(py[i] + (ny[a] + ny[i]) / den);
        o[i].prim = v[i].prim;
    }
    memcpy(v, o, sizeof o);
    return true;
}

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
    HiresPrim gpr;
    if (s_gx) { gpr = *p; guard_clip(&gpr); p = &gpr; }
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
    /* PGXP (see hires.c): any precise vertex moves samples off the 1x grid (UV clamp at 1x1
     * too); depth on all three = perspective-correct u/v; fractional positions are drawn with
     * the exact edge test (the GPU rasterizer's sub-pixel snapping would not match hires.c) */
    const bool precise = vin[0].w > 0 || vin[1].w > 0 || vin[2].w > 0;
    const bool persp = tex && hires_get_perspective() && vin[0].w > 0 && vin[1].w > 0 && vin[2].w > 0 &&
                       !(vin[0].w == vin[1].w && vin[1].w == vin[2].w);   /* = affine (hires.c) */
    const bool frac = ((vin[0].x | vin[0].y | vin[1].x | vin[1].y | vin[2].x | vin[2].y) & 0xFFFF) != 0;
    s_st.tris++;
    s_st.persp_tris += persp;
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
    const bool dith = dither_on(p);
    const int filt = tex ? hires_get_tex_filter() : HIRES_TEX_NEAREST;
    if (s_fast) {
        u32 kf = base_flags(p, K_TRI, tex) | (gour ? F_GOUR : 0) | (dith ? F_DITHER : 0) |
                 ((Sx > 1 || Sy > 1 || precise) ? F_CLAMP : 0);
        const bool is2d = tex && hires_tri_is_2d(vin);
        int layer = -1;
        if (tex && s_tc_mode != HGPU_TC_OFF && !is2d && (layer = tc_lookup(p)) >= 0) {
            /* texture cache: mipmapped + anisotropic sampling of the decoded page; no VRAM texels */
            kf |= F_TC | F_CLAMP | (filt == HIRES_TEX_NEAREST ? F_TCNEAR : 0) | (s_tc[layer].stp ? F_TCSTP : 0);
            s_st.tc_prims++;
            if (s_tc_diag) tc_diag_tri(vin, Sx, Sy);
        } else {
            if (tex) tex_need(p, umin, umax, vmin, vmax);
            if (filt != HIRES_TEX_NEAREST) kf |= F_CLAMP | ((filt == HIRES_TEX_XBR && is2d) ? F_XBR : F_BILIN);
        }
        const HiresVtx *sv[3] = { a, b, c };
        fast_tri(p, sv, persp, kf, umin, umax, vmin, vmax, xlo, ylo, xhi, yhi, layer);
        return;
    }
    if (tex) tex_need(p, umin, umax, vmin, vmax);
    (void)frac;
    u32 idx;
    GPrim *g = new_prim(&idx);
    g->kf = base_flags(p, K_TRI, tex) | (gour ? F_GOUR : 0) | (dith ? F_DITHER : 0) |
            (gour && p->has_cr ? F_C1X : 0) | ((Sx > 1 || Sy > 1 || precise) ? F_CLAMP : 0) |
            (persp ? F_PERSP : 0);
    if (filt != HIRES_TEX_NEAREST)   /* the footprint always stays in the 1x-sampled range */
        g->kf |= F_CLAMP | ((filt == HIRES_TEX_XBR && hires_tri_is_2d(vin)) ? F_XBR : F_BILIN);
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
    bool cons = false;
    if (frac || s_force_edge) {   /* grown triangle, else the bounding box; both edge-tested */
        cons = inside && conservative_tri(v);
        inside = false;
    }
    if (persp) {
        /* the same planes as hires.c tri_impl (vertices sorted the same way, relative to a,
         * normalised by the smallest w), re-based to hi-res pixel (xlo, ylo):
         * P(X) - ax = (X - xlo) / Sx + ((xlo + 0.5) / Sx - 0.5 - ax) */
        const HiresVtx *vv[3] = { a, b, c };
        double wmin = a->w;
        if (b->w < wmin) wmin = b->w;
        if (c->w < wmin) wmin = c->w;
        double f[3][3], X[3], Y[3];
        for (int k = 0; k < 3; k++) {
            double q = wmin / (double)vv[k]->w;
            f[k][0] = q; f[k][1] = (double)vv[k]->u * q; f[k][2] = (double)vv[k]->v * q;
            X[k] = (double)vv[k]->x / 65536.0; Y[k] = (double)vv[k]->y / 65536.0;
        }
        double den = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        double ox = ((double)xlo + 0.5) / Sx - 0.5 - X[0], oy = ((double)ylo + 0.5) / Sy - 0.5 - Y[0];
        for (int i = 0; i < 3; i++) {
            double dx = ((f[1][i] - f[0][i]) * (Y[2] - Y[0]) - (f[2][i] - f[0][i]) * (Y[1] - Y[0])) / den;
            double dy = ((f[2][i] - f[0][i]) * (X[1] - X[0]) - (f[1][i] - f[0][i]) * (X[2] - X[0])) / den;
            /* fixed point 2^-38: q <= 1 (normalised), u*q, v*q <= 255 */
            g->pl[i] = llround((f[0][i] + dx * ox + dy * oy) * 274877906944.0);
            g->pl[3 + i] = llround(dx / Sx * 274877906944.0);
            g->pl[6 + i] = llround(dy / Sy * 274877906944.0);
        }
        g->porg[0] = xlo;
        g->porg[1] = ylo;
    }
    g->kf |= denom > 0 ? F_LONGLEFT : 0;
    {
        const HiresVtx *sv[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            g->ev[2 * i] = (s32)((s64)sv[i]->x * Sx);
            g->ev[2 * i + 1] = (s32)((s64)sv[i]->y * Sy);
        }
    }
    if (cons) {
        g->kf |= F_EDGE;   /* geometry is only a bound: the shader tests coverage */
        s_st.edge_tris++;
    } else if (!inside) {
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
    const int mxs = xs, mxe = xe;   /* what the 1x rect draws */
    HiresPrim gpr;
    if (s_gx) {
        /* an untextured rect over the whole draw-area width (fade, flash) spans the guard band */
        bool full = !p->tex && x <= p->clip_x1 && x + w >= p->clip_x2 + 1;
        gpr = *p;
        guard_clip(&gpr);
        p = &gpr;
        if (full) { x = p->clip_x1; w = p->clip_x2 + 1 - x; }
        xs = imax(x, p->clip_x1); xe = imin(x + w, p->clip_x2 + 1);
    }
    if (xs >= xe || ys >= ye) return;
    int rxs = imax(xs, s_rx - s_gx), rxe = imin(xe, s_rx + s_rw + s_gx), rys = imax(ys, s_ry), rye = imin(ye, s_ry + s_rh);
    if (rxs < rxe && rys < rye) {
        const bool filt = p->tex && hires_get_tex_filter() != HIRES_TEX_NEAREST;
        int fr[4] = { 0, 0, 0, 0 };   /* filtered: footprint range (hires.c) */
        if (filt) hires_rect_uv_range(p, x, y, w, h, u0, v0, fx, fy, &fr[0], &fr[1], &fr[2], &fr[3]);
        if (p->tex) {   /* u/v sampled over the drawn columns/rows, as the 1x rect samples them */
            int uu0 = (int)(u0 | (fx ? 1u : 0u)), du = fx ? -1 : 1, dv = fy ? -1 : 1;
            int ua = uu0 + du * (rxs - x), ub = uu0 + du * (rxe - 1 - x);
            int va = (int)v0 + dv * (rys - y), vb = (int)v0 + dv * (rye - 1 - y);
            int ulo = imin(ua, ub), uhi = imax(ua, ub), vlo = imin(va, vb), vhi = imax(va, vb);
            if (filt) { ulo = imin(ulo, fr[0]); uhi = imax(uhi, fr[1]); vlo = imin(vlo, fr[2]); vhi = imax(vhi, fr[3]); }
            if (ulo < 0 || uhi > 255) { ulo = 0; uhi = 255; }
            if (vlo < 0 || vhi > 255) { vlo = 0; vhi = 255; }
            tex_need(p, ulo, uhi, vlo, vhi);
        }
        if (s_fast) {
            /* quad on the 1x pixel edges; U at 1x x-coordinate e is t0 + d * (e - 0.5 - x) + 0.5, so
             * floor(U) at a hi-res pixel centre is the 1x rect's texel for that column (and U - 0.5
             * the filter's sample point) */
            u32 kf = base_flags(p, K_RECT, p->tex);
            if (filt) kf |= F_CLAMP | (hires_get_tex_filter() == HIRES_TEX_XBR ? F_XBR : F_BILIN);
            u32 idx;
            FPrim *f = new_fprim(&idx, p, kf);
            f->ur = srange(fr[0], fr[1]);
            f->vr = srange(fr[2], fr[3]);
            const double t0 = (double)(u0 | (fx ? 1u : 0u)), s0 = (double)v0;
            const double du = fx ? -1 : 1, dv = fy ? -1 : 1;
            const int ex[2] = { rxs, rxe }, ey[2] = { rys, rye };
            FVtx c4[4];
            for (int j = 0; j < 4; j++) {
                const int ix = ex[j & 1], iy = ey[j >> 1];
                c4[j].x = (float)(ix * s_Sx - s_ox);
                c4[j].y = (float)(iy * s_Sy - s_oy);
                c4[j].w = 1.0f;
                c4[j].u = (float)(t0 + du * (ix - 0.5 - x) + 0.5);
                c4[j].v = (float)(s0 + dv * (iy - 0.5 - y) + 0.5);
                c4[j].col = (r & 255) | ((g8 & 255) << 8) | ((b & 255) << 16);
                c4[j].prim = idx;
            }
            const FVtx v[6] = { c4[0], c4[1], c4[2], c4[1], c4[3], c4[2] };
            int keys[2];
            int nk = prim_keys(p, p->semi, keys);
            for (int i = 0; i < nk; i++) keys[i] += NKEYS;
            u64 m[8];
            tile_mask(rxs, rys, rxe - 1, rye - 1, m);
            group_add(keys, nk, v, 6, m);
            if (mxs < mxe) mark_vram(mxs, ys, mxe - mxs, ye - ys);
            return;
        }
        u32 idx;
        GPrim *gp = new_prim(&idx);
        gp->kf = base_flags(p, K_RECT, p->tex) | (fx ? F_FLIPX : 0) | (fy ? F_FLIPY : 0);
        if (filt) {
            gp->kf |= hires_get_tex_filter() == HIRES_TEX_XBR ? F_XBR : F_BILIN;
            gp->cmin = ((u32)fr[0] & 0xFFFFu) | ((u32)fr[2] << 16);   /* signed 16-bit pairs */
            gp->cmax = ((u32)fr[1] & 0xFFFFu) | ((u32)fr[3] << 16);
        }
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
    if (mxs < mxe) mark_vram(mxs, ys, mxe - mxs, ye - ys);   /* the 1x rect is drawn after this call */
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

static inline u32 lclamp(s32 v, u32 c0, u32 c1, int sh)
{
    const s32 a = (s32)((c0 >> sh) & 0xFF), b = (s32)((c1 >> sh) & 0xFF);
    const s32 lo = a < b ? a : b, hi = a < b ? b : a;
    return (u32)(v < lo ? lo : (v > hi ? hi : v));
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
    HiresPrim gpr;
    if (s_gx) { gpr = *p; guard_clip(&gpr); p = &gpr; }
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
        /* the Sa - 1 steps past the end extrapolate the gradient: keep it between the end
         * colours (no-op at 1x1, where the line stops at the end; it used to wrap below 0) */
        u32 cr = lclamp(r >> 12, c0, c1, 0), cg = lclamp(g >> 12, c0, c1, 8), cb = lclamp(b >> 12, c0, c1, 16);
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
        const int fx = x & 1023;
        const bool wide = s_gx && fx + imin(w, VRAM_W) <= VRAM_W && fx <= s_rx && fx + w >= s_rx + s_rw;
        if (n) {
            u32 idx;
            GPrim *g = new_prim(&idx);
            g->kf = K_FILL;
            g->color = c;
            for (int i = 0; i < n; i++) {
                if (wide && pc[i][0] == s_rx && pc[i][2] == s_rx + s_rw) { pc[i][0] -= s_gx; pc[i][2] += s_gx; }
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

/* HiresBackend.external: a pass of the caller's own between this point's commands */
static void be_external(ExtFn fn, void *ctx)
{
    if (!s_ready || !fn) return;
    flush_puts();
    close_group();
    Step *st = add_step(ST_EXTERNAL);
    st->fn = fn;
    st->ctx = ctx;
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
    s_fverts.n = s_fprims.n = 0;
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
    const size_t fvb = s_fverts.n * sizeof(FVtx), fpb = s_fprims.n * sizeof(FPrim);
    const size_t voff = 0, poff = (vb + 255) & ~(size_t)255, fvoff = (poff + pb + 255) & ~(size_t)255,
                 fpoff = (fvoff + fvb + 255) & ~(size_t)255, aoff = (fpoff + fpb + 255) & ~(size_t)255;
    const size_t total = aoff + s_arena.n;
    SDL_GPUCopyPass *cp = NULL;
    SDL_GPURenderPass *rp = NULL;
    if (vb || pb || fvb || fpb || s_arena.n) {
        s_up = ensure_tbuf(s_up, &s_up_cap, (u32)total, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        u8 *m = SDL_MapGPUTransferBuffer(s_dev, s_up, true);
        if (!m) { rt_log("hires-gpu: map: %s", SDL_GetError()); SDL_CancelGPUCommandBuffer(cb); reset_lists(); return false; }
        if (vb) memcpy(m + voff, s_verts.p, vb);
        if (pb) memcpy(m + poff, s_prims.p, pb);
        if (fvb) memcpy(m + fvoff, s_fverts.p, fvb);
        if (fpb) memcpy(m + fpoff, s_fprims.p, fpb);
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
        if (fvb) {
            s_fvbuf = ensure_buffer(s_fvbuf, &s_fvbuf_cap, (u32)fvb, SDL_GPU_BUFFERUSAGE_VERTEX);
            SDL_GPUTransferBufferLocation src = { s_up, (u32)fvoff };
            SDL_GPUBufferRegion dst = { s_fvbuf, 0, (u32)fvb };
            SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        }
        if (fpb) {
            s_fpbuf = ensure_buffer(s_fpbuf, &s_fpbuf_cap, (u32)fpb, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
            SDL_GPUTransferBufferLocation src = { s_up, (u32)fpoff };
            SDL_GPUBufferRegion dst = { s_fpbuf, 0, (u32)fpb };
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
                if (st->key < NKEYS) {
                    SDL_BindGPUGraphicsPipeline(rp, s_pipe[s_hp][st->key]);
                    SDL_GPUBufferBinding vbb = { s_vbuf, 0 };
                    SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
                    SDL_BindGPUFragmentStorageBuffers(rp, 0, &s_pbuf, 1);
                    SDL_GPUTextureSamplerBinding tsb[3] = { { s_temp, s_samp }, { s_puttex, s_samp }, { s_vramtex, s_samp } };
                    SDL_BindGPUFragmentSamplers(rp, 0, tsb, 3);
                } else {
                    SDL_BindGPUGraphicsPipeline(rp, s_fpipe[s_hp][st->key - NKEYS]);
                    SDL_GPUBufferBinding vbb = { s_fvbuf, 0 };
                    SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
                    SDL_BindGPUFragmentStorageBuffers(rp, 0, &s_fpbuf, 1);
                    /* 0: raw VRAM (uncached textures), 1 / 2: texture cache colour / STP arrays */
                    SDL_GPUTextureSamplerBinding tsb[3] = { { s_vramtex, s_samp },
                                                            { s_tccol ? s_tccol : s_tcdummy, s_tcsamp },
                                                            { s_tcstp ? s_tcstp : s_tcdummy, s_tcsamp } };
                    SDL_BindGPUFragmentSamplers(rp, 0, tsb, 3);
                }
                cur_key = st->key;
            }
            int k = (st->key % NKEYS) % 5;
            FragU fu = { s_Sx, s_Sy, s_ox, s_oy, k ? 1 : 0, k ? k - 1 : 0, s_rx, s_ry, s_rw, s_rh, s_hp, 0 };
            SDL_PushGPUFragmentUniformData(cb, 0, &fu, sizeof fu);
            SDL_DrawGPUPrimitives(rp, st->count, 1, st->first, 0);
            s_st.draws++;
        } else if (st->type == ST_EXTERNAL) {
            if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; }
            if (cp) { SDL_EndGPUCopyPass(cp); cp = NULL; }
            st->fn(st->ctx, cb, s_target, s_aw, s_ah);
            s_st.externals++;
        } else {
            if (rp) { SDL_EndGPURenderPass(rp); rp = NULL; }
            if (!cp) cp = SDL_BeginGPUCopyPass(cb);
            if (st->type == ST_TCOPY) {
                SDL_GPUTextureLocation src = { .texture = s_target, .x = 0, .y = (u32)st->y };
                SDL_GPUTextureLocation dst = { .texture = s_temp, .x = 0, .y = (u32)st->y };
                SDL_CopyGPUTextureToTexture(cp, &src, &dst, (u32)s_tw, (u32)st->h, 1, false);
            } else if (st->type == ST_TC_UP) {
                SDL_GPUTextureTransferInfo src = { .transfer_buffer = s_up, .offset = (u32)(aoff + st->off),
                                                   .pixels_per_row = (u32)st->w, .rows_per_layer = (u32)st->h };
                SDL_GPUTextureRegion dst = { .texture = st->y ? s_tcstp : s_tccol, .mip_level = (u32)st->x,
                                             .layer = (u32)st->key, .w = (u32)st->w, .h = (u32)st->h, .d = 1 };
                SDL_UploadToGPUTexture(cp, &src, &dst, false);
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
        const int pu[4] = { px, py, s_ssaa_present, s_hp };
        SDL_PushGPUFragmentUniformData(cb, 0, pu, sizeof pu);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        rp = NULL;
        s_st.passes++;
    }
    if (readback) {
        const u32 bytes = (u32)rw * (u32)rh * (s_hp ? 4 : 2);
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
        const size_t need = (size_t)rw * rh * (s_hp ? 4 : 2);
        if (need > s_rb_cap) { free(s_rb); s_rb = malloc(need); s_rb_cap = s_rb ? need : 0; }
        const void *m = SDL_MapGPUTransferBuffer(s_dev, s_down, false);
        if (!m || !s_rb) return false;
        memcpy(s_rb, m, need);
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

static void be_destroy_force(void)
{
    reset_lists();
    for (int k = 0; k < NGKEYS; k++) s_gv[k].n = 0;
    memset(s_gm, 0, sizeof s_gm);
    s_gused = 0;
    s_put_n = 0;
    release_target();
    s_ready = false;
    s_Sx = s_Sy = 0;
    s_gx = 0;
}

static void be_destroy(void)
{
    if (s_inited) be_destroy_force();
}

static bool be_configure(const u16 *vram1x, int sx, int sy, int rx, int ry, int rw, int rh)
{
    if (!s_inited) return false;
    if (sx < 1 || sx > HIRES_MAX_RENDER_SCALE || sy < 1 || sy > HIRES_MAX_RENDER_SCALE || rw <= 0 || rh <= 0 ||
        rx < 0 || ry < 0 || rx + rw > VRAM_W || ry + rh > VRAM_H)
        return false;
    if (s_ready) submit(false, 0, 0, 0, 0, false, 0, 0);
    be_destroy();
    s_vram = vram1x;
    s_Sx = sx; s_Sy = sy;
    s_rx = rx; s_ry = ry; s_rw = rw; s_rh = rh;
    s_gx = s_pend_gx;
    s_tw = (rw + 2 * s_gx) * sx; s_th = rh * sy;
    s_ox = (rx - s_gx) * sx; s_oy = ry * sy;
    s_hp = hires_target_fmt() == HIRES_FMT_RGBA8;
    const SDL_GPUTextureFormat fmt = s_hp ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM;
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
    /* The seed pass samples the VRAM texture, so upload the seed picture first. Its framebuffer
     * region may hold drawn pixels the emulated VRAM lacks (gpu.c skip rule); no primitive samples
     * the framebuffer region, and later writes there re-upload from the emulated VRAM as usual. */
    s_vram = hires_seed_vram(vram1x);
    ensure_vram(0, 0, VRAM_W, VRAM_H);
    s_vram = vram1x;   /* the texture source from now on; the seed picture may be freed after this call */
    tc_clear();        /* every entry is decoded again from this VRAM */
    u32 idx;
    GPrim *g = new_prim(&idx);
    g->kf = K_SEED;
    GVtx v[6];
    quad1x(v, rx, ry, rx + rw, ry + rh, idx);
    u64 m[8];
    tile_mask(rx, ry, rx + rw - 1, ry + rh - 1, m);
    int key = 0;
    group_add(&key, 1, v, 6, m);
    if (s_gx) {   /* the guard band starts black */
        GPrim *f = new_prim(&idx);
        f->kf = K_FILL;
        f->color = 0;
        for (int side = 0; side < 2; side++) {
            int a = side ? rx + rw : rx - s_gx;
            quad1x(v, a, ry, a + s_gx, ry + rh, idx);
            tile_mask(a, ry, a + s_gx - 1, ry + rh - 1, m);
            group_add(&key, 1, v, 6, m);
        }
    }
    return true;
}

static void be_set_guard(int gx) { s_pend_gx = gx < 0 ? 0 : (gx > HIRES_MAX_GUARD ? HIRES_MAX_GUARD : gx); }

static bool view_rect(int x, int y, int w, int h, HiresView *out)
{
    if (!s_ready || w <= 0 || h <= 0) return false;
    if (x < s_rx - s_gx || y < s_ry || x + w > s_rx + s_rw + s_gx || y + h > s_ry + s_rh) return false;
    out->pixels = NULL;
    out->px32 = NULL;
    out->fmt = s_hp ? HIRES_FMT_RGBA8 : HIRES_FMT_15;
    out->ssaa = 1;
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
    s_tc_frame++;
    if (s_tc_mode == HGPU_TC_VERIFY && s_tccol && s_tc_frame % (u64)s_tc_verify_every == 0) tc_verify();
    return true;
}

static bool be_read(int x, int y, int w, int h, HiresView *out)
{
    if (!view_rect(x, y, w, h, out)) return false;
    if (!submit(true, out->gx, out->gy, out->w, out->h, false, 0, 0)) return false;
    out->pixels = s_hp ? NULL : (const u16 *)s_rb;
    out->px32 = s_hp ? (const u32 *)s_rb : NULL;
    out->stride = out->w;
    out->gpu = NULL;
    return true;
}

SDL_GPUTexture *hires_gpu_present(const HiresView *v)
{
    if (!s_ready || !v->gpu) return NULL;
    const int n = v->ssaa > 1 ? v->ssaa : 1, ow = v->w / n, oh = v->h / n;   /* SSAA box filter */
    if (n != s_ssaa_present) { s_ssaa_present = n; s_dirty_present = true; }
    if (!s_present || s_pw != ow || s_ph != oh) {
        if (s_present) SDL_ReleaseGPUTexture(s_dev, s_present);
        s_present = make_tex(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, ow, oh,
                             SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        s_pw = ow; s_ph = oh;
        s_dirty_present = true;
        if (!s_present) return NULL;
    }
    if (s_dirty_present || s_present_x != v->gx || s_present_y != v->gy) {
        submit(false, 0, 0, 0, 0, true, v->gx, v->gy);
        s_present_x = v->gx; s_present_y = v->gy;
        s_dirty_present = false;
    }
    return s_present;
}

static const HiresBackend s_gpu = {
    "gpu", be_configure, be_tri, be_rect, be_line, be_fill, be_copy, be_put, be_view, be_read, be_destroy,
    be_set_guard, be_external,
};

const HiresBackend *hires_gpu_backend(void) { return &s_gpu; }

static int s_api = -1;              /* API of s_dev while initialised */

/* ---- for external passes (hires_gpu.h "for an external pass") ------------------------------------- */
#define MAX_SHUTDOWN_HOOKS 4
static void (*s_shutdown_hooks[MAX_SHUTDOWN_HOOKS])(void);
void hires_gpu_on_shutdown(void (*fn)(void))
{
    for (int i = 0; i < MAX_SHUTDOWN_HOOKS; i++) {
        if (s_shutdown_hooks[i] == fn) return;
        if (!s_shutdown_hooks[i]) { s_shutdown_hooks[i] = fn; return; }
    }
}

bool hires_gpu_info(HiresGpuInfo *o)
{
    if (!s_ready || !s_dev || !s_target) return false;
    memset(o, 0, sizeof *o);
    o->dev = s_dev; o->api = s_api; o->shader_format = (unsigned)s_shfmt;
    o->target_format = s_hp ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM;
    o->full_colour = s_hp != 0;
    o->Sx = s_Sx; o->Sy = s_Sy; o->rx = s_rx; o->ry = s_ry; o->rw = s_rw; o->rh = s_rh; o->gx = s_gx;
    o->ox = s_ox; o->oy = s_oy; o->tw = s_tw; o->th = s_th; o->aw = s_aw; o->ah = s_ah;
    o->tc_colour = s_tccol; o->tc_stp = s_tcstp; o->tc_sampler = s_tcsamp; o->nearest = s_samp;
    return true;
}

int hires_gpu_tc_layer(int depth, unsigned tp_x, unsigned tp_y, unsigned clut_x, unsigned clut_y, unsigned tw,
                       unsigned long long *content)
{
    if (!s_ready) return -1;
    HiresPrim p;
    memset(&p, 0, sizeof p);
    p.tex = true; p.depth = depth; p.tp_x = tp_x; p.tp_y = tp_y; p.clut_x = clut_x; p.clut_y = clut_y;
    p.tw_and_u = tw & 0xFF; p.tw_or_u = (tw >> 8) & 0xFF; p.tw_and_v = (tw >> 16) & 0xFF; p.tw_or_v = tw >> 24;
    const int l = tc_lookup(&p);
    if (l >= 0 && content) *content = s_tc[l].content_h;
    return l;
}

/* ---- init ------------------------------------------------------------------------------------------ */

static SDL_GPUShader *make_shader(const char *entry, SDL_GPUShaderStage stage, u32 samplers, u32 sbufs, u32 ubufs)
{
    SDL_GPUShaderCreateInfo si = { .code = (const Uint8 *)s_msl, .code_size = sizeof s_msl - 1, .entrypoint = entry,
                                   .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage, .num_samplers = samplers,
                                   .num_storage_buffers = sbufs, .num_uniform_buffers = ubufs };
    /* SPIR-V and DXIL: one module per entry point (from hires.glsl), each with entry "main" */
    static const struct { const char *name; const unsigned char *spv, *dxil; size_t spv_n, dxil_n; } mods[] = {
        { "vs_prim", s_spv_vs_prim, s_dxil_vs_prim, sizeof s_spv_vs_prim, sizeof s_dxil_vs_prim },
        { "fs_prim", s_spv_fs_prim, s_dxil_fs_prim, sizeof s_spv_fs_prim, sizeof s_dxil_fs_prim },
        { "vs_full", s_spv_vs_full, s_dxil_vs_full, sizeof s_spv_vs_full, sizeof s_dxil_vs_full },
        { "fs_present", s_spv_fs_present, s_dxil_fs_present, sizeof s_spv_fs_present, sizeof s_dxil_fs_present },
        { "vs_fast", s_spv_vs_fast, s_dxil_vs_fast, sizeof s_spv_vs_fast, sizeof s_dxil_vs_fast },
        { "fs_fast", s_spv_fs_fast, s_dxil_fs_fast, sizeof s_spv_fs_fast, sizeof s_dxil_fs_fast },
    };
    if (s_shfmt != SDL_GPU_SHADERFORMAT_MSL) {
        for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++)
            if (!strcmp(mods[i].name, entry)) {
                const bool dx = s_shfmt == SDL_GPU_SHADERFORMAT_DXIL;
                si.code = dx ? mods[i].dxil : mods[i].spv;
                si.code_size = dx ? mods[i].dxil_n : mods[i].spv_n;
            }
        si.entrypoint = "main";
        si.format = s_shfmt;
    }
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

/* ---- graphics APIs (SDL_GPU drivers) ----------------------------------------------------------------
 * One backend, three shader formats: Metal takes the MSL above, Vulkan the SPIR-V and Direct3D 12
 * the DXIL, both compiled from runtime/shaders/hires.glsl. s_order is the platform's preference
 * (the first that works is the default): macOS Metal, Vulkan (MoltenVK); Windows Direct3D 12,
 * Vulkan; elsewhere Vulkan. */
static const struct { const char *name, *title, *sdl; SDL_GPUShaderFormat fmt; } s_apis[HGPU_NAPI] = {
    [HGPU_METAL] = { "metal", "Metal", "metal", SDL_GPU_SHADERFORMAT_MSL },
    [HGPU_VULKAN] = { "vulkan", "Vulkan", "vulkan", SDL_GPU_SHADERFORMAT_SPIRV },
    [HGPU_D3D12] = { "d3d12", "Direct3D 12", "direct3d12", SDL_GPU_SHADERFORMAT_DXIL },
};
#if defined(__APPLE__)
static const int s_order[] = { HGPU_METAL, HGPU_VULKAN };
#elif defined(_WIN32)
static const int s_order[] = { HGPU_D3D12, HGPU_VULKAN };
#else
static const int s_order[] = { HGPU_VULKAN };
#endif
static int s_req_api = -1;               /* --gpu-driver: -1 = auto (s_order) */
static signed char s_probe[HGPU_NAPI];   /* 0 = not probed yet, 1 = available, -1 = not */

const char *hires_gpu_api_name(int api) { return api >= 0 && api < HGPU_NAPI ? s_apis[api].name : "none"; }
const char *hires_gpu_api_title(int api) { return api >= 0 && api < HGPU_NAPI ? s_apis[api].title : "none"; }
int hires_gpu_api_from_name(const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < HGPU_NAPI; i++)
        if (!strcmp(s, s_apis[i].name) || !strcmp(s, s_apis[i].sdl)) return i;
    return -1;
}
int hires_gpu_api_order(int out[HGPU_NAPI])
{
    int n = 0;
    for (size_t i = 0; i < sizeof s_order / sizeof s_order[0]; i++) out[n++] = s_order[i];
    return n;
}
bool hires_gpu_api_offered(int api)
{
    for (size_t i = 0; i < sizeof s_order / sizeof s_order[0]; i++)
        if (s_order[i] == api) return true;
    return false;
}

bool hires_gpu_set_driver(const char *name)
{
    if (!name || !*name || !strcmp(name, "auto")) { s_req_api = -1; return true; }
    const int a = hires_gpu_api_from_name(name);
    if (a < 0 || !hires_gpu_api_offered(a)) return false;
    s_req_api = a;
    return true;
}
int hires_gpu_driver(void) { return s_req_api; }

static bool video_up(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {   /* SDL_GPU needs it */
        rt_log("hires-gpu: SDL video: %s", SDL_GetError());
        return false;
    }
#ifdef __APPLE__
    /* Vulkan on macOS = MoltenVK through the Vulkan loader, which SDL only finds in the default
     * library paths; point it at Homebrew's loader unless SDL_VULKAN_LIBRARY says otherwise. */
    if (!SDL_GetHint(SDL_HINT_VULKAN_LIBRARY)) {
        static const char *const libs[] = { "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib" };
        for (size_t i = 0; i < sizeof libs / sizeof libs[0]; i++) {
            SDL_PathInfo pi;
            if (SDL_GetPathInfo(libs[i], &pi)) { SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, libs[i]); break; }
        }
    }
#endif
    return true;
}

bool hires_gpu_api_probe(int api)
{
    if (api < 0 || api >= HGPU_NAPI || !hires_gpu_api_offered(api)) return false;
    if (!s_probe[api]) {
        /* SDL prepares the driver (loads its library, looks for a device that takes this shader
         * format) without creating a device; render target formats are checked by init_on */
        SDL_ClearError();
        const bool ok = video_up() && SDL_GPUSupportsShaderFormats(s_apis[api].fmt, s_apis[api].sdl);
        s_probe[api] = ok ? 1 : -1;
        if (!ok) rt_log("hires-gpu: %s is not available on this computer%s%s", s_apis[api].title,
                        *SDL_GetError() ? ": " : "", SDL_GetError());
    }
    return s_probe[api] > 0;
}
void hires_gpu_api_mark_failed(int api) { if (api >= 0 && api < HGPU_NAPI) s_probe[api] = -1; }

SDL_GPUDevice *hires_gpu_create_device(int api)
{
    if (api < 0 || api >= HGPU_NAPI || !video_up()) return NULL;
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(s_apis[api].fmt, false, s_apis[api].sdl);
    if (!dev) rt_log("hires-gpu: no %s device: %s", s_apis[api].title, SDL_GetError());
    return dev;
}

static bool s_own_dev;              /* s_dev was created by hires_gpu_init(NULL) (headless) */
int hires_gpu_api(void) { return s_inited ? s_api : -1; }

static bool init_on(SDL_GPUDevice *dev);

bool hires_gpu_init(SDL_GPUDevice *dev)
{
    if (s_inited) return true;
    if (dev) {
        if (init_on(dev)) return true;
        hires_gpu_shutdown();
        return false;
    }
    /* a device of its own: the requested API, else the first of the platform's that works */
    int order[HGPU_NAPI], n = 0;
    if (s_req_api >= 0) order[n++] = s_req_api;
    else n = hires_gpu_api_order(order);
    for (int i = 0; i < n; i++) {
        if (!hires_gpu_api_probe(order[i])) continue;
        dev = hires_gpu_create_device(order[i]);
        if (!dev) { hires_gpu_api_mark_failed(order[i]); continue; }
        if (init_on(dev)) { s_own_dev = true; return true; }
        hires_gpu_shutdown();
        SDL_DestroyGPUDevice(dev);
        hires_gpu_api_mark_failed(order[i]);
    }
    return false;
}

/* Release everything created on the device (between frames; the hi-res layer must not be using
 * this backend: hires_set_scale(0, 0) or another backend first). The device itself is destroyed
 * only if it is our own (headless); a windowed renderer's device belongs to the caller. */
void hires_gpu_shutdown(void)
{
    if (!s_dev) { s_inited = false; return; }
    if (s_ready) submit(false, 0, 0, 0, 0, false, 0, 0);
    for (int i = 0; i < 2; i++) wait_fence(&s_fences[i]);
    SDL_WaitForGPUIdle(s_dev);
    for (int i = 0; i < MAX_SHUTDOWN_HOOKS; i++) if (s_shutdown_hooks[i]) s_shutdown_hooks[i]();
    be_destroy_force();
    for (int h = 0; h < 2; h++)
        for (int k = 0; k < NKEYS; k++)
            for (int f = 0; f < 2; f++) {
                SDL_GPUGraphicsPipeline **pp = f ? &s_fpipe[h][k] : &s_pipe[h][k];
                if (*pp) { SDL_ReleaseGPUGraphicsPipeline(s_dev, *pp); *pp = NULL; }
            }
    if (s_present_pipe) SDL_ReleaseGPUGraphicsPipeline(s_dev, s_present_pipe);
    if (s_samp) SDL_ReleaseGPUSampler(s_dev, s_samp);
    if (s_tcsamp) SDL_ReleaseGPUSampler(s_dev, s_tcsamp);
    if (s_tccol) SDL_ReleaseGPUTexture(s_dev, s_tccol);
    if (s_tcstp) SDL_ReleaseGPUTexture(s_dev, s_tcstp);
    if (s_tcdummy) SDL_ReleaseGPUTexture(s_dev, s_tcdummy);
    if (s_tcdown) SDL_ReleaseGPUTransferBuffer(s_dev, s_tcdown);
    s_tcsamp = NULL; s_tccol = s_tcstp = s_tcdummy = NULL; s_tcdown = NULL;
    s_tc_failed = false;
    tc_clear();
    if (s_vramtex) SDL_ReleaseGPUTexture(s_dev, s_vramtex);
    if (s_present) SDL_ReleaseGPUTexture(s_dev, s_present);
    if (s_vbuf) SDL_ReleaseGPUBuffer(s_dev, s_vbuf);
    if (s_pbuf) SDL_ReleaseGPUBuffer(s_dev, s_pbuf);
    if (s_fvbuf) SDL_ReleaseGPUBuffer(s_dev, s_fvbuf);
    if (s_fpbuf) SDL_ReleaseGPUBuffer(s_dev, s_fpbuf);
    if (s_up) SDL_ReleaseGPUTransferBuffer(s_dev, s_up);
    if (s_down) SDL_ReleaseGPUTransferBuffer(s_dev, s_down);
    s_present_pipe = NULL; s_samp = NULL; s_vramtex = s_present = NULL;
    s_vbuf = s_pbuf = s_fvbuf = s_fpbuf = NULL; s_up = s_down = NULL;
    s_vbuf_cap = s_pbuf_cap = s_fvbuf_cap = s_fpbuf_cap = s_up_cap = s_down_cap = 0;
    s_pw = s_ph = 0;
    s_dirty_present = true;
    s_present_x = s_present_y = -1;
    if (s_own_dev) SDL_DestroyGPUDevice(s_dev);
    s_own_dev = false;
    s_dev = NULL;
    s_inited = false;
    s_api = -1;
}

static char s_devname[128];

const char *hires_gpu_device_name(void) { return s_inited ? s_devname : ""; }

bool hires_gpu_software_adapter(void)
{
    if (!s_inited) return false;
    static const char *const soft[] = { "Basic Render", "WARP", "llvmpipe", "lavapipe", "softpipe", "SwiftShader" };
    for (size_t i = 0; i < sizeof soft / sizeof soft[0]; i++)
        if (SDL_strcasestr(s_devname, soft[i])) return true;
    return false;
}

static bool create_pipelines(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, SDL_GPUShader *vs, SDL_GPUShader *fs,
                             SDL_GPUShader *pvs, SDL_GPUShader *pfs, SDL_GPUShader *fvs, SDL_GPUShader *ffs)
{
    {   /* fast precision: the same blend states, FVtx vertices */
        SDL_GPUVertexBufferDescription vbd = { .slot = 0, .pitch = sizeof(FVtx), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
        SDL_GPUVertexAttribute va[5] = {
            { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0 },
            { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, .offset = 8 },
            { .location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 12 },
            { .location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = 20 },
            { .location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 24 },
        };
        for (int k = 0; k < 2 * NKEYS; k++) {
            SDL_GPUColorTargetDescription cd = { .format = k >= NKEYS ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : fmt };
            blend_for_key(k % NKEYS, &cd.blend_state);
            SDL_GPUGraphicsPipelineCreateInfo pi = {
                .vertex_shader = fvs, .fragment_shader = ffs,
                .vertex_input_state = { .vertex_buffer_descriptions = &vbd, .num_vertex_buffers = 1,
                                        .vertex_attributes = va, .num_vertex_attributes = 5 },
                .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
                .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
                .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1 },
            };
            s_fpipe[k / NKEYS][k % NKEYS] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (!s_fpipe[k / NKEYS][k % NKEYS]) { rt_log("hires-gpu: fast pipeline %d: %s", k, SDL_GetError()); return false; }
        }
    }
    SDL_GPUVertexBufferDescription vbd = { .slot = 0, .pitch = sizeof(GVtx), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute va[2] = {
        { .location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0 },
        { .location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 8 },
    };
    for (int k = 0; k < 2 * NKEYS; k++) {
        SDL_GPUColorTargetDescription cd = { .format = k >= NKEYS ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : fmt };
        blend_for_key(k % NKEYS, &cd.blend_state);
        SDL_GPUGraphicsPipelineCreateInfo pi = {
            .vertex_shader = vs, .fragment_shader = fs,
            .vertex_input_state = { .vertex_buffer_descriptions = &vbd, .num_vertex_buffers = 1,
                                    .vertex_attributes = va, .num_vertex_attributes = 2 },
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
            .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1 },
        };
        s_pipe[k / NKEYS][k % NKEYS] = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!s_pipe[k / NKEYS][k % NKEYS]) { rt_log("hires-gpu: pipeline %d: %s", k, SDL_GetError()); return false; }
    }
    SDL_GPUColorTargetDescription cd = { .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM };
    SDL_GPUGraphicsPipelineCreateInfo pi = {
        .vertex_shader = pvs, .fragment_shader = pfs,
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
        .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1 },
    };
    s_present_pipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!s_present_pipe) { rt_log("hires-gpu: present pipeline: %s", SDL_GetError()); return false; }
    return true;
}

/* Everything on `dev`; on failure the caller runs hires_gpu_shutdown() (s_dev is set early so it
 * releases what was made). */
static bool init_on(SDL_GPUDevice *dev)
{
    const char *drv = SDL_GetGPUDeviceDriver(dev);
    s_api = hires_gpu_api_from_name(drv);
    if (s_api < 0 || !(SDL_GetGPUShaderFormats(dev) & s_apis[s_api].fmt)) {
        rt_log("hires-gpu: device %s takes none of our shader formats (MSL, SPIR-V, DXIL)", drv ? drv : "?");
        return false;
    }
    s_shfmt = s_apis[s_api].fmt;
    /* Vulkan leaves the rasterizer's tie-break for pixel centres on an edge open; Metal and D3D12
     * specify the top-left rule the integer path relies on */
    s_force_edge = s_api == HGPU_VULKAN;
    {
        const char *e = SDL_getenv("DESTREGA_GPU_EDGE");   /* experiments: 1 = always, 0 = only when needed */
        if (e && *e) s_force_edge = *e != '0';
    }
    s_dev = dev;
    {
        const SDL_PropertiesID pr = SDL_GetGPUDeviceProperties(dev);
        const char *nm = pr ? SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_NAME_STRING, "") : "";
        const char *di = pr ? SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, "") : "";
        snprintf(s_devname, sizeof s_devname, "%s%s%s", nm, *di ? ", " : "", di);
    }
    const SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_B5G5R5A1_UNORM;
    const SDL_GPUTextureUsageFlags use = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (!SDL_GPUTextureSupportsFormat(dev, fmt, SDL_GPU_TEXTURETYPE_2D, use)) {
        rt_log("hires-gpu: %s: B5G5R5A1 render targets unsupported on this GPU (%s)", s_apis[s_api].title, s_devname);
        return false;
    }
    if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, use)) {
        rt_log("hires-gpu: %s: RGBA8 render targets unsupported on this GPU (%s)", s_apis[s_api].title, s_devname);
        return false;
    }
    SDL_GPUShader *vs = make_shader("vs_prim", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
    SDL_GPUShader *fs = make_shader("fs_prim", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1, 1);
    SDL_GPUShader *pvs = make_shader("vs_full", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    SDL_GPUShader *pfs = make_shader("fs_present", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);
    SDL_GPUShader *fvs = make_shader("vs_fast", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
    SDL_GPUShader *ffs = make_shader("fs_fast", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1, 1);
    const bool ok = vs && fs && pvs && pfs && fvs && ffs && create_pipelines(dev, fmt, vs, fs, pvs, pfs, fvs, ffs);
    SDL_GPUShader *sh[6] = { vs, fs, pvs, pfs, fvs, ffs };
    for (int i = 0; i < 6; i++)
        if (sh[i]) SDL_ReleaseGPUShader(dev, sh[i]);
    if (!ok) return false;
    SDL_GPUSamplerCreateInfo sci = { .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
                                     .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                     .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                     .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
    s_samp = SDL_CreateGPUSampler(dev, &sci);
    s_vramtex = make_tex(SDL_GPU_TEXTUREFORMAT_R16_UINT, VRAM_W, VRAM_H, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!s_samp || !s_vramtex) { rt_log("hires-gpu: %s", SDL_GetError()); return false; }
    if (!tc_init(dev)) { rt_log("hires-gpu: texture cache: %s", SDL_GetError()); return false; }
    s_inited = true;
    rt_log("hires-gpu: SDL_GPU device ready (%s, driver %s, %s shaders%s; %s; %s precision)", s_apis[s_api].title, drv,
           s_shfmt == SDL_GPU_SHADERFORMAT_MSL ? "MSL" : s_shfmt == SDL_GPU_SHADERFORMAT_DXIL ? "DXIL" : "SPIR-V",
           s_force_edge ? ", exact edge test on every triangle" : "", *s_devname ? s_devname : "unnamed adapter",
           s_fast ? "fast" : "exact");
    return true;
}
