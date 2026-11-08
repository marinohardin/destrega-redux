/* Vulkan (SPIR-V) shaders of the SDL_GPU hi-res backend: the same logic as the MSL source in
 * runtime/hires_gpu.c (keep the two in step; the software rasterizer in hires.c is the
 * reference both must match bit for bit). Compiled by tools/gen_spirv.py into
 * runtime/shaders/hires_spirv.h, one entry point per -D define:
 *   VS_PRIM / FS_PRIM      primitives into the B5G5R5A1 (15-bit) or RGBA8 (full-precision) target
 *   VS_FULL / FS_PRESENT   target -> RGBA8 presentation texture (SSAA box filter)
 *
 * 64-bit integers: Vulkan only has them with the optional shaderInt64 feature (and SDL_GPU
 * does not enable it), so every 64-bit value is a uvec2 (x = low word, y = high word, two's
 * complement) with exact wrapping add/sub/multiply (uaddCarry, usubBorrow, umulExtended,
 * imulExtended: core SPIR-V, no capability needed). The MSL source divides 64-bit values in
 * three places; here each one is exact without a 64-bit divide:
 *   attr()     the 2^-16 fraction is split off (G = q*D + r), leaving a 0/1 carry test;
 *   lceil()    span edges: float estimate of N/D, corrected with integer multiplies (the
 *              quotient is a pixel column, |q| < 2^15);
 *   div24()    already estimate + correction in MSL;
 *   ldivs()    (U1 - U0) / n with n <= 8: 16-bit long division.
 * Signed 32-bit division and % are avoided too (GLSL leaves negative operands loosely
 * specified): floordiv()/tdiv() divide magnitudes.
 *
 * Resource sets (SDL_GPU SPIR-V convention): vertex uniforms set 1; fragment samplers
 * set 2 bindings 0..2, the primitive storage buffer set 2 binding 3, fragment uniforms set 3.
 */
#version 450

#if defined(VS_PRIM)
layout(location = 0) in vec2 in_pos;
layout(location = 1) in uint in_prim;
layout(location = 0) flat out uint v_prim;
layout(std140, set = 1, binding = 0) uniform VU { vec4 k; } vu;
void main()
{
    gl_Position = vec4(in_pos.x * vu.k.x - 1.0, 1.0 - in_pos.y * vu.k.y, 0.0, 1.0);
    v_prim = in_prim;
}
#endif

#if defined(VS_FULL)
void main()
{
    uint vid = uint(gl_VertexIndex);
    gl_Position = vec4(vid == 1u ? 3.0 : -1.0, vid == 2u ? 3.0 : -1.0, 0.0, 1.0);
}
#endif

#if defined(FS_PRESENT)
/* target -> RGBA8: n x n box average (SSAA, rounded; hires.c hires_view_to_rgba) of 8-bit channels */
layout(set = 2, binding = 0) uniform sampler2D t_target;
layout(std140, set = 3, binding = 0) uniform PU { int x, y, n, hp; } pu;
layout(location = 0) out vec4 o_color;
void main()
{
    uvec3 s = uvec3(0u);
    int bx = int(gl_FragCoord.x) * pu.n + pu.x, by = int(gl_FragCoord.y) * pu.n + pu.y;
    for (int j = 0; j < pu.n; j++)
        for (int i = 0; i < pu.n; i++) {
            vec4 v = texelFetch(t_target, ivec2(bx + i, by + j), 0);
            if (pu.hp != 0) s += uvec3(uint(roundEven(v.r * 255.0)), uint(roundEven(v.g * 255.0)), uint(roundEven(v.b * 255.0)));
            else s += uvec3(uint(roundEven(v.b * 31.0)), uint(roundEven(v.g * 31.0)), uint(roundEven(v.r * 31.0))) * 8u;
        }
    uint nn = uint(pu.n * pu.n);
    o_color = vec4(vec3((s + nn / 2u) / nn) / 255.0, 1.0);
}
#endif

#if defined(FS_PRIM)
/* ---- 64-bit integers as uvec2 (lo, hi) ------------------------------------------------- */
uvec2 L(int a) { return uvec2(uint(a), a < 0 ? 0xFFFFFFFFu : 0u); }
uvec2 ladd(uvec2 a, uvec2 b) { uint c; uint lo = uaddCarry(a.x, b.x, c); return uvec2(lo, a.y + b.y + c); }
uvec2 lsub(uvec2 a, uvec2 b) { uint c; uint lo = usubBorrow(a.x, b.x, c); return uvec2(lo, a.y - b.y - c); }
uvec2 lneg(uvec2 a) { return lsub(uvec2(0u), a); }
uvec2 lmul(uvec2 a, uvec2 b)   /* low 64 bits of the product (= C wrapping semantics) */
{
    uint hi, lo;
    umulExtended(a.x, b.x, hi, lo);
    return uvec2(lo, hi + a.x * b.y + a.y * b.x);
}
uvec2 lmul32(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return uvec2(uint(lo), uint(hi)); }
bool lisneg(uvec2 a) { return int(a.y) < 0; }
bool llt(uvec2 a, uvec2 b) { return int(a.y) < int(b.y) || (a.y == b.y && a.x < b.x); }
bool lle(uvec2 a, uvec2 b) { return !llt(b, a); }
uvec2 lsar(uvec2 a, int n) { return uvec2((a.x >> uint(n)) | (a.y << uint(32 - n)), uint(int(a.y) >> n)); }   /* 0 < n < 32 */
uvec2 lshl(uvec2 a, int n) { return uvec2(a.x << uint(n), (a.y << uint(n)) | (a.x >> uint(32 - n))); }       /* 0 < n < 32 */
int lint(uvec2 a) { return int(a.x); }                                 /* C (int) cast: low word */
float lfloat(uvec2 a) { return float(int(a.y)) * 4294967296.0 + float(a.x); }

/* floor(a / b) and trunc(a / b) for b > 0, on magnitudes only */
int floordiv(int a, int b) { return a >= 0 ? a / b : -int((uint(-(a + 1)) + uint(b)) / uint(b)); }
int tdiv(int a, int b) { return a >= 0 ? a / b : -int((uint(-(a + 1)) + 1u) / uint(b)); }
/* trunc(a / n) for 0 < n < 65536 (16-bit long division of the magnitude) */
uvec2 ldivs(uvec2 a, int n)
{
    bool neg = lisneg(a);
    uvec2 m = neg ? lneg(a) : a;
    uint d = uint(n);
    uint qh = m.y / d, r = m.y - qh * d;
    uint mid = (r << 16) | (m.x >> 16), qm = mid / d;
    r = mid - qm * d;
    uint low = (r << 16) | (m.x & 0xFFFFu), ql = low / d;
    uvec2 q = uvec2((qm << 16) | ql, qh);
    return neg ? lneg(q) : q;
}
/* ceil(N / D), D > 0, for quotients well inside 32 bits (pixel columns) */
int lceil(uvec2 N, uvec2 D)
{
    float e = ceil(lfloat(N) / lfloat(D));
    int t = int(clamp(e, -1073741824.0, 1073741824.0));
    uvec2 r = lsub(N, lmul(L(t), D));   /* want -D < r <= 0 */
    for (int k = 0; k < 4 && !lle(r, uvec2(0u)); k++) { r = lsub(r, D); t++; }
    for (int k = 0; k < 4 && lle(r, lneg(D)); k++) { r = ladd(r, D); t--; }
    return t;
}

/* ---- resources --------------------------------------------------------------------------- */
struct Prim {   /* = GPrim in hires_gpu.c (std430: natural C layout, 272 bytes) */
    uint kf, tp, clut, tw, clip0, clip1, uvr, cmin, cmax, color, aux0, aux1;
    int qx[5]; int qy[5]; uint rxy[5]; uint pad; uvec2 cint[5]; uint pad2[2]; int ev[6]; uint pad3[2];
    uvec2 pl[9]; int porg[2];
};
layout(set = 2, binding = 0) uniform sampler2D t_tmp;
layout(set = 2, binding = 1) uniform usampler2D t_put;
layout(set = 2, binding = 2) uniform usampler2D t_vram;
layout(std430, set = 2, binding = 3) readonly buffer Prims { Prim prims[]; };
layout(std140, set = 3, binding = 0) uniform FU { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; } u;
layout(location = 0) flat in uint v_prim;
layout(location = 0) out vec4 o_color;

const int DITH[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

/* floor of the 1x plane equation at hi-res pixel (n = 2X+1-Sx, m = 2Y+1-Sy), exactly:
 * E = cint + qx*n + qy*m + floor((cf*D + 65536*G) / (65536*D)), D = 4*Sx*Sy,
 * G = rx*n*2*Sy + ry*m*2*Sx; with G = q*D + r the last term is q + (cf*D + 65536*r >= 65536*D) */
uvec2 attr(uint pi, int i, int n, int m, int sx, int sy)
{
    uint w = prims[pi].rxy[i];
    int rx = int(w & 0xFFu), ry = int((w >> 8) & 0xFFu), cf = int(w >> 16);
    int D = 4 * sx * sy;
    int G = rx * n * 2 * sy + ry * m * 2 * sx;
    int q = floordiv(G, D), r = G - q * D;
    int c = (cf * D + 65536 * r >= 65536 * D) ? 1 : 0;
    return ladd(ladd(prims[pi].cint[i], lmul32(prims[pi].qx[i], n)), ladd(lmul32(prims[pi].qy[i], m), L(q + c)));
}
uint vram_at(uint x, uint y) { return texelFetch(t_vram, ivec2(int(x & 1023u), int(y & 511u)), 0).r; }
uint tex_fetch(uint pi, uint depth, uint uu, uint vv)
{
    uint tw = prims[pi].tw;
    uu = (uu & (tw & 0xFFu)) | ((tw >> 8) & 0xFFu);
    vv = (vv & ((tw >> 16) & 0xFFu)) | (tw >> 24);
    uint tpx = prims[pi].tp & 0xFFFFu, tpy = prims[pi].tp >> 16, cx = prims[pi].clut & 0xFFFFu, cy = prims[pi].clut >> 16;
    uint row = (tpy + vv) & 511u;
    if (depth == 0u) { uint w = vram_at(tpx + (uu >> 2), row); uint idx = (w >> ((uu & 3u) * 4u)) & 0xFu; return vram_at(cx + idx, cy); }
    if (depth == 1u) { uint w = vram_at(tpx + (uu >> 1), row); uint idx = (w >> ((uu & 1u) * 8u)) & 0xFFu; return vram_at(cx + idx, cy); }
    return vram_at(tpx + uu, row);
}
/* Colours are uvec4 (r, g, b, mask bit) in output units: 5-bit channels for the B5G5R5A1 target,
 * 8-bit for the full-precision RGBA8 one (hp). B5G5R5A1 stores .b in the low 5 bits, so PS1 red
 * goes to .b there. Blend pass: B/2+F/2 as F/2 - 1/4 (0.5 constant dst factor), B+F/4 as floor(F/4). */
vec4 pack4(uvec4 c, int pass, int mode, int hp)
{
    float r = float(c.x), g = float(c.y), b = float(c.z);
    if (pass == 1) {
        if (mode == 0) { r = r * 0.5 - 0.25; g = g * 0.5 - 0.25; b = b * 0.5 - 0.25; }
        else if (mode == 3) { r = floor(r * 0.25); g = floor(g * 0.25); b = floor(b * 0.25); }
    }
    if (hp != 0) return vec4(r / 255.0, g / 255.0, b / 255.0, float(c.w));
    return vec4(b / 31.0, g / 31.0, r / 31.0, float(c.w));
}
/* a 15-bit PS1 pixel in output units (full precision: c << 3, the PS1's 8-bit scale) */
uvec4 c15u(uint c, int hp)
{
    uint s = hp != 0 ? 3u : 0u;
    return uvec4((c & 31u) << s, ((c >> 5) & 31u) << s, ((c >> 10) & 31u) << s, (c >> 15) & 1u);
}
uvec4 unpack4(vec4 t, int hp)
{
    if (hp != 0)
        return uvec4(uint(roundEven(t.r * 255.0)), uint(roundEven(t.g * 255.0)), uint(roundEven(t.b * 255.0)), t.a > 0.5 ? 1u : 0u);
    return uvec4(uint(roundEven(t.b * 31.0)), uint(roundEven(t.g * 31.0)), uint(roundEven(t.r * 31.0)), t.a > 0.5 ? 1u : 0u);
}
int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* filtered texels (hires.c tex_combine / samp_bilinear / samp_xbr, bit for bit); result
 * (r, g, b, stp) with 5.8 fixed-point channels */
bool tcomb(uint t[5], uint w[5], int n, out ivec4 o)
{
    uint cov = 0u, sr = 0u, sg = 0u, sb = 0u, ss = 0u;
    for (int k = 0; k < n; k++) {
        if (t[k] == 0u || w[k] == 0u) continue;
        cov += w[k]; sr += w[k] * (t[k] & 31u); sg += w[k] * ((t[k] >> 5) & 31u); sb += w[k] * ((t[k] >> 10) & 31u);
        if ((t[k] & 0x8000u) != 0u) ss += w[k];
    }
    o = ivec4(0);
    if (cov < 32768u) return false;
    o = ivec4(int((sr * 256u + (cov >> 1)) / cov), int((sg * 256u + (cov >> 1)) / cov), int((sb * 256u + (cov >> 1)) / cov),
              ss * 2u >= cov ? 1 : 0);
    return true;
}
void clamp_axis(inout int i, inout int f, int lo, int hi) { if (i < lo) { i = lo; f = 0; } else if (i >= hi) { i = hi; f = 0; } }
bool samp_filt(uint pi, uint depth, bool xbr, int iu, int fu, int iv, int fv, int ulo, int uhi, int vlo, int vhi,
               int sx, int sy, out ivec4 o)
{
    uint t[5] = uint[5](0u, 0u, 0u, 0u, 0u), w[5] = uint[5](0u, 0u, 0u, 0u, 0u);
    int n;
    if (!xbr) {
        int iu1 = iu < uhi ? iu + 1 : uhi, iv1 = iv < vhi ? iv + 1 : vhi;
        t[0] = tex_fetch(pi, depth, uint(iu), uint(iv)); t[1] = tex_fetch(pi, depth, uint(iu1), uint(iv));
        t[2] = tex_fetch(pi, depth, uint(iu), uint(iv1)); t[3] = tex_fetch(pi, depth, uint(iu1), uint(iv1));
        w[0] = uint((256 - fu) * (256 - fv)); w[1] = uint(fu * (256 - fv)); w[2] = uint((256 - fu) * fv); w[3] = uint(fu * fv);
        n = 4;
    } else {
        int ie = iu, dx = fu, je = iv, dy = fv;
        if (fu >= 128) { ie = iu + 1; dx = fu - 256; }
        if (fv >= 128) { je = iv + 1; dy = fv - 256; }
        int hx = dx >= 0 ? 1 : -1, hy = dy >= 0 ? 1 : -1;
        t[0] = tex_fetch(pi, depth, uint(ie), uint(je));
        t[1] = tex_fetch(pi, depth, uint(min(max(ie + hx, ulo), uhi)), uint(je));
        t[2] = tex_fetch(pi, depth, uint(ie), uint(min(max(je + hy, vlo), vhi)));
        t[3] = tex_fetch(pi, depth, uint(min(max(ie - hx, ulo), uhi)), uint(je));
        t[4] = tex_fetch(pi, depth, uint(ie), uint(min(max(je - hy, vlo), vhi)));
        w[0] = 65536u; n = 2;
        if (t[1] == t[2] && t[1] != t[0] && t[1] != t[3] && t[2] != t[4]) {
            int d8 = abs(dx) + abs(dy) - 128, a = min(max(((d8 * (sx + sy)) >> 1) + 128, 0), 256);
            w[0] = uint(256 - a) * 256u; w[1] = uint(a) * 256u;
        }
    }
    return tcomb(t, w, n, o);
}
/* rect texel coordinate on one axis at hi-res pixel X (hires.c rect_axis): index and 1/256 fraction */
ivec2 rect_axis(int t0, int d, int k0, int X, int s)
{
    int D = 2 * s, num = D * t0 + d * (2 * X + 1 - s - D * k0);
    int q = floordiv(num, D);
    return ivec2(q, ((num - q * D) * 256) / D);
}
/* sampled texel (5.8) modulated by the vertex colour, in output units (hires.c tex8_out15/32) */
uvec4 tex_out(ivec4 t, int cr, int cg, int cb, bool raw, bool dith, int d, int hp)
{
    int r, g, b;
    if (raw) { int s = hp != 0 ? 5 : 8; r = t.x >> s; g = t.y >> s; b = t.z >> s; }
    else if (hp != 0) { r = min((t.x * cr) >> 12, 255); g = min((t.y * cg) >> 12, 255); b = min((t.z * cb) >> 12, 255); }
    else if (dith) { r = clamp255(((t.x * cr) >> 12) + d) >> 3; g = clamp255(((t.y * cg) >> 12) + d) >> 3; b = clamp255(((t.z * cb) >> 12) + d) >> 3; }
    else { r = min((t.x * cr) >> 15, 31); g = min((t.y * cg) >> 15, 31); b = min((t.z * cb) >> 15, 31); }
    return uvec4(uint(r), uint(g), uint(b), uint(t.w));
}

/* hires.c's coverage rule, exactly: rows [ceil(Ya), ceil(Yc)), columns [ceil(xl), ceil(xr)) with
 * xl/xr on the long edge a-c and the short edge a-b (rows above Yb) or b-c; 16.16 hi-res coords */
bool edge_right_of(uvec2 X16, uvec2 Y16, uvec2 px, uvec2 py, uvec2 qx, uvec2 qy)
{
    return lle(lmul(lsub(qx, px), lsub(Y16, py)), lmul(lsub(X16, px), lsub(qy, py)));
}
bool covered(uint pi, int X, int Y)
{
    uvec2 X16 = lmul32(X, 65536), Y16 = lmul32(Y, 65536);
    uvec2 ax = L(prims[pi].ev[0]), ay = L(prims[pi].ev[1]), bx = L(prims[pi].ev[2]), by = L(prims[pi].ev[3]);
    uvec2 cx = L(prims[pi].ev[4]), cy = L(prims[pi].ev[5]);
    if (llt(Y16, ay) || !llt(Y16, cy)) return false;
    uvec2 sx0, sy0, sx1, sy1;
    if (llt(Y16, by)) { sx0 = ax; sy0 = ay; sx1 = bx; sy1 = by; } else { sx0 = bx; sy0 = by; sx1 = cx; sy1 = cy; }
    bool on_long = edge_right_of(X16, Y16, ax, ay, cx, cy), on_short = edge_right_of(X16, Y16, sx0, sy0, sx1, sy1);
    return ((prims[pi].kf & (1u << 20)) != 0u) ? (on_long && !on_short) : (on_short && !on_long);
}
/* ceil of the x of the span edge on row Y: the long edge (left = true when the long edge is
 * on that side) or the short edge a-b / b-c */
int span_edge(uint pi, int Y, bool use_long)
{
    uvec2 Y16 = lmul32(Y, 65536);
    int ax = prims[pi].ev[0], ay = prims[pi].ev[1], bx = prims[pi].ev[2], by = prims[pi].ev[3];
    int cx = prims[pi].ev[4], cy = prims[pi].ev[5];
    int px, py, qx, qy;
    if (use_long) { px = ax; py = ay; qx = cx; qy = cy; }
    else if (llt(Y16, L(by))) { px = ax; py = ay; qx = bx; qy = by; }
    else { px = bx; py = by; qx = cx; qy = cy; }
    uvec2 dy = lsub(L(qy), L(py));
    uvec2 N = ladd(lmul(L(px), dy), lmul(lsub(L(qx), L(px)), lsub(Y16, L(py))));
    return lceil(N, lshl(dy, 16));
}
/* first pixel of the span hires.c draws on row Y: ceil(left edge x), at least the clip */
int span_start(uint pi, int Y, int cx1) { return max(span_edge(pi, Y, (prims[pi].kf & (1u << 20)) != 0u), cx1); }
/* one past the last pixel of that span: ceil(right edge x), at most the clip */
int span_end(uint pi, int Y, int cx2) { return min(span_edge(pi, Y, (prims[pi].kf & (1u << 20)) == 0u), cx2); }

/* floor(a * 2^24 / q) for q > 0: float estimates corrected exactly with integer multiplies;
 * r < q <= 2^38 keeps r << 24 in range */
uvec2 div24(uvec2 a, uvec2 q)
{
    float fq = lfloat(q);
    int t = int(clamp(floor(lfloat(a) / fq), -1073741824.0, 1073741824.0));
    uvec2 r = lsub(a, lmul(L(t), q));
    for (int k = 0; k < 4 && lisneg(r); k++) { r = ladd(r, q); t--; }
    for (int k = 0; k < 4 && !llt(r, q); k++) { r = lsub(r, q); t++; }
    uvec2 s = lshl(r, 24);
    int e = int(clamp(lfloat(s) / fq, -1073741824.0, 1073741824.0));
    uvec2 r2 = lsub(s, lmul(L(e), q));
    for (int k = 0; k < 8 && lisneg(r2); k++) { r2 = ladd(r2, q); e--; }
    for (int k = 0; k < 8 && !llt(r2, q); k++) { r2 = lsub(r2, q); e++; }
    return ladd(lshl(L(t), 24), L(e));
}
/* u (x) and v (y) in 8.24 at hi-res pixel X: q, u*q, v*q planes in 2^-38 */
void persp_at(uint pi, int X, int Y, out uvec2 U, out uvec2 V)
{
    uvec2 fx = L(X - prims[pi].porg[0]), fy = L(Y - prims[pi].porg[1]);
    uvec2 q = ladd(ladd(prims[pi].pl[0], lmul(prims[pi].pl[3], fx)), lmul(prims[pi].pl[6], fy));
    if (lle(q, uvec2(0u))) { U = L(8388608); V = L(8388608); return; }
    uvec2 a = ladd(ladd(prims[pi].pl[1], lmul(prims[pi].pl[4], fx)), lmul(prims[pi].pl[7], fy));
    uvec2 b = ladd(ladd(prims[pi].pl[2], lmul(prims[pi].pl[5], fx)), lmul(prims[pi].pl[8], fy));
    U = ladd(div24(a, q), L(8388608));
    V = ladd(div24(b, q), L(8388608));
}
/* hires.c's perspective u/v (PGXP): exact at the start of every 8-pixel run, stepped in between */
void persp_uv(uint pi, int xs, int xe, int X, int Y, out uvec2 U, out uvec2 V)
{
    int xr = xs + ((X - xs) & ~7), n = xe - xr < 8 ? xe - xr : 8;
    uvec2 U0, V0, U1, V1;
    persp_at(pi, xr, Y, U0, V0);
    persp_at(pi, xr + n, Y, U1, V1);
    U = ladd(U0, lmul(L(X - xr), ldivs(lsub(U1, U0), n)));
    V = ladd(V0, lmul(L(X - xr), ldivs(lsub(V1, V0), n)));
}
/* hires.c's attribute value at X: the plane equation at the span start, then stepped by the
 * per-pixel gradient truncated to an integer (gdx / Sx; gdx is the original 32-bit gradient) */
uvec2 attr_span(uint pi, int i, int xs, int X, int m, int sx, int sy)
{
    int gdx = prims[pi].qx[i] * 2 * sx + int(prims[pi].rxy[i] & 0xFFu);
    return ladd(attr(pi, i, 2 * xs + 1 - sx, m, sx, sy), lmul32(X - xs, tdiv(gdx, sx)));
}

void main()
{
    uint pi = v_prim;
    int X = int(gl_FragCoord.x) + u.ox, Y = int(gl_FragCoord.y) + u.oy;
    uint kf = prims[pi].kf, kind = kf & 15u;
    int hp = u.p0;
    int x1 = floordiv(X, u.sx), y1 = Y / u.sy;
    if (kind == 2u) { if (u.pass != 0) discard; o_color = pack4(c15u(prims[pi].color & 0xFFFFu, hp), 0, 0, hp); return; }
    if (kind == 6u) { o_color = pack4(c15u(vram_at(uint(x1), uint(y1)), hp), 0, 0, hp); return; }
    if (kind == 4u) {
        uint w = texelFetch(t_put, ivec2(x1 - u.rx, y1 - u.ry), 0).r;
        if ((w & 0x10000u) == 0u) discard;
        o_color = pack4(c15u((w & 0xFFFFu) | ((kf & (1u << 18)) != 0u ? 0x8000u : 0u), hp), 0, 0, hp);
        return;
    }
    if (kind == 3u) {
        int sb = X - x1 * u.sx, sa = Y - y1 * u.sy;
        uint a0 = prims[pi].aux0, a1 = prims[pi].aux1;
        int csx = int(a0 & 0xFFFFu), csy = int(a0 >> 16), cdx = int(a1 & 0xFFFFu), cdy = int(a1 >> 16);
        int scol = (csx + ((x1 - cdx) & 1023)) & 1023, srow = (csy + ((y1 - cdy) & 511)) & 511;
        uvec4 c;
        if (scol >= u.rx && scol < u.rx + u.rw && srow >= u.ry && srow < u.ry + u.rh)
            c = unpack4(texelFetch(t_tmp, ivec2(scol * u.sx + sb - u.ox, srow * u.sy + sa - u.oy), 0), hp);
        else c = c15u(vram_at(uint(scol), uint(srow)), hp);
        if ((kf & (1u << 18)) != 0u) c.w = 1u;
        o_color = pack4(c, 0, 0, hp);
        return;
    }
    int cx1 = int(prims[pi].clip0 << 16) >> 16, cy1 = int(prims[pi].clip0 >> 16);
    int cx2 = int(prims[pi].clip1 << 16) >> 16, cy2 = int(prims[pi].clip1 >> 16);
    if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard;
    bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u;
    bool gour = (kf & (1u << 7)) != 0u, dith = (kf & (1u << 8)) != 0u;
    bool filt = (kf & (3u << 22)) != 0u, xbr = (kf & (1u << 23)) != 0u;
    uint depth = (kf >> 9) & 3u;
    uint col = prims[pi].color;
    int cr = int(col & 255u), cg = int((col >> 8) & 255u), cb = int((col >> 16) & 255u);
    uint uu = 0u, vv = 0u;
    int iu = 0, fu = 0, iv = 0, fv = 0, ulo = 0, uhi = 255, vlo = 0, vhi = 255;
    if (kind == 0u) {
        if ((kf & (1u << 19)) != 0u && !covered(pi, X, Y)) discard;
        int m = 2 * Y + 1 - u.sy, xs = span_start(pi, Y, cx1);
        if (gour) {
            int c3[3];
            bool c1x = (kf & (1u << 14)) != 0u;
            for (int i = 0; i < 3; i++) {
                uvec2 val = attr_span(pi, i, xs, X, m, u.sx, u.sy);
                int lo = int((prims[pi].cmin >> (8 * i)) & 255u), hi = int((prims[pi].cmax >> (8 * i)) & 255u);
                if (c1x) {
                    int s = int(val.x >> 24);
                    c3[i] = s < lo ? lo : (s > hi ? hi : s);
                } else {
                    uvec2 s = lsar(val, 24);
                    c3[i] = llt(s, L(lo)) ? lo : (llt(L(hi), s) ? hi : lint(s));
                }
            }
            cr = c3[0]; cg = c3[1]; cb = c3[2];
        }
        if (tex) {
            uvec2 U, V;
            if ((kf & (1u << 21)) != 0u) {
                int xe = span_end(pi, Y, cx2);
                persp_uv(pi, xs, xe, X, Y, U, V);
            } else {
                U = attr_span(pi, 3, xs, X, m, u.sx, u.sy);
                V = attr_span(pi, 4, xs, X, m, u.sx, u.sy);
            }
            uint uvr = prims[pi].uvr;
            ulo = int(uvr & 255u); uhi = int((uvr >> 8) & 255u); vlo = int((uvr >> 16) & 255u); vhi = int(uvr >> 24);
            if (filt) {   /* sample point without the +0.5 rounding bias, footprint inside the 1x-sampled range */
                uvec2 su = lsub(U, L(8388608)), sv = lsub(V, L(8388608));
                iu = lint(lsar(su, 24)); fu = int((su.x >> 16) & 255u);
                iv = lint(lsar(sv, 24)); fv = int((sv.x >> 16) & 255u);
                clamp_axis(iu, fu, ulo, uhi); clamp_axis(iv, fv, vlo, vhi);
            } else {
                int ui = lint(lsar(U, 24)), vi = lint(lsar(V, 24));
                if ((kf & (1u << 15)) != 0u) {
                    ui = ui < ulo ? ulo : (ui > uhi ? uhi : ui);
                    vi = vi < vlo ? vlo : (vi > vhi ? vhi : vi);
                }
                uu = uint(ui); vv = uint(vi);
            }
        }
    } else if (kind == 1u) {
        uint a0 = prims[pi].aux0, a1 = prims[pi].aux1;
        int x0 = int(a0 << 16) >> 16, y0 = int(a0) >> 16;
        uint u0 = a1 & 255u, v0 = (a1 >> 8) & 255u;
        bool fx = (kf & (1u << 16)) != 0u, fy = (kf & (1u << 17)) != 0u;
        if (tex && fx) u0 |= 1u;
        if (filt) {   /* range of the drawn columns / rows, signed 16-bit in cmin / cmax */
            uint c0 = prims[pi].cmin, c1 = prims[pi].cmax;
            ulo = int(c0 << 16) >> 16; vlo = int(c0) >> 16; uhi = int(c1 << 16) >> 16; vhi = int(c1) >> 16;
            ivec2 a = rect_axis(int(u0), fx ? -1 : 1, x0, X, u.sx), b = rect_axis(int(v0), fy ? -1 : 1, y0, Y, u.sy);
            iu = a.x; fu = a.y; iv = b.x; fv = b.y;
            clamp_axis(iu, fu, ulo, uhi); clamp_axis(iv, fv, vlo, vhi);
        } else {
            uu = (u0 + uint(fx ? x0 - x1 : x1 - x0)) & 255u;
            vv = (v0 + uint(fy ? y0 - y1 : y1 - y0)) & 255u;
        }
    }
    int d = dith ? DITH[(y1 & 3) * 4 + (x1 & 3)] : 0;
    uvec4 outc;
    bool blend;
    if (tex) {
        ivec4 t8;
        if (filt) { if (!samp_filt(pi, depth, xbr, iu, fu, iv, fv, ulo, uhi, vlo, vhi, u.sx, u.sy, t8)) discard; }
        else {
            uint t = tex_fetch(pi, depth, uu, vv);
            if (t == 0u) discard;
            t8 = ivec4(int(t & 31u) << 8, int((t >> 5) & 31u) << 8, int((t >> 10) & 31u) << 8, int(t >> 15));
        }
        outc = tex_out(t8, cr, cg, cb, raw, dith, d, hp);
        blend = semi && t8.w != 0;
    } else {
        int r, g, b;
        if (hp != 0) { r = cr; g = cg; b = cb; }
        else if (dith) { r = clamp255(cr + d) >> 3; g = clamp255(cg + d) >> 3; b = clamp255(cb + d) >> 3; }
        else { r = cr >> 3; g = cg >> 3; b = cb >> 3; }
        outc = uvec4(uint(r), uint(g), uint(b), 0u);
        blend = semi;
    }
    if (blend != (u.pass == 1)) discard;
    if ((kf & (1u << 13)) != 0u) outc.w = 1u;
    o_color = pack4(outc, u.pass, u.mode, hp);
}
#endif
