/* Vulkan (SPIR-V) shaders of the SDL_GPU hi-res backend: the same logic as the MSL source in
 * runtime/hires_gpu.c (keep the two in step; the software rasterizer in hires.c is the
 * reference both must match bit for bit). Compiled by tools/gen_spirv.py into
 * runtime/shaders/hires_spirv.h, one entry point per -D define:
 *   VS_PRIM / FS_PRIM      primitives into the B5G5R5A1 target
 *   VS_FULL / FS_PRESENT   target -> RGBA8 presentation texture
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
layout(set = 2, binding = 0) uniform sampler2D t_target;
layout(std140, set = 3, binding = 0) uniform PU { int x, y, p0, p1; } pu;
layout(location = 0) out vec4 o_color;
void main()
{
    vec4 v = texelFetch(t_target, ivec2(int(gl_FragCoord.x) + pu.x, int(gl_FragCoord.y) + pu.y), 0);
    o_color = vec4(roundEven(v.b * 31.0) * 8.0 / 255.0, roundEven(v.g * 31.0) * 8.0 / 255.0,
                   roundEven(v.r * 31.0) * 8.0 / 255.0, 1.0);
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
/* PS1 pixel -> B5G5R5A1 output: the format stores .b in the low 5 bits, so PS1 red goes to .b */
vec4 pack(uint c, int pass, int mode)
{
    float r = float(c & 31u), g = float((c >> 5) & 31u), b = float((c >> 10) & 31u);
    if (pass == 1) {
        if (mode == 0) { r = r * 0.5 - 0.25; g = g * 0.5 - 0.25; b = b * 0.5 - 0.25; }
        else if (mode == 3) { r = floor(r * 0.25); g = floor(g * 0.25); b = floor(b * 0.25); }
    }
    return vec4(b / 31.0, g / 31.0, r / 31.0, float(c >> 15));
}
uint unpack(vec4 t)
{
    return uint(roundEven(t.b * 31.0)) | (uint(roundEven(t.g * 31.0)) << 5) | (uint(roundEven(t.r * 31.0)) << 10) |
           (t.a > 0.5 ? 0x8000u : 0u);
}
int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

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
    int x1 = floordiv(X, u.sx), y1 = Y / u.sy;
    if (kind == 2u) { if (u.pass != 0) discard; o_color = pack(prims[pi].color & 0xFFFFu, 0, 0); return; }
    if (kind == 6u) { o_color = pack(vram_at(uint(x1), uint(y1)), 0, 0); return; }
    if (kind == 4u) {
        uint w = texelFetch(t_put, ivec2(x1 - u.rx, y1 - u.ry), 0).r;
        if ((w & 0x10000u) == 0u) discard;
        o_color = pack((w & 0xFFFFu) | ((kf & (1u << 18)) != 0u ? 0x8000u : 0u), 0, 0);
        return;
    }
    if (kind == 3u) {
        int sb = X - x1 * u.sx, sa = Y - y1 * u.sy;
        uint a0 = prims[pi].aux0, a1 = prims[pi].aux1;
        int csx = int(a0 & 0xFFFFu), csy = int(a0 >> 16), cdx = int(a1 & 0xFFFFu), cdy = int(a1 >> 16);
        int scol = (csx + ((x1 - cdx) & 1023)) & 1023, srow = (csy + ((y1 - cdy) & 511)) & 511;
        uint c;
        if (scol >= u.rx && scol < u.rx + u.rw && srow >= u.ry && srow < u.ry + u.rh)
            c = unpack(texelFetch(t_tmp, ivec2(scol * u.sx + sb - u.ox, srow * u.sy + sa - u.oy), 0));
        else c = vram_at(uint(scol), uint(srow));
        o_color = pack(c | ((kf & (1u << 18)) != 0u ? 0x8000u : 0u), 0, 0);
        return;
    }
    int cx1 = int(prims[pi].clip0 << 16) >> 16, cy1 = int(prims[pi].clip0 >> 16);
    int cx2 = int(prims[pi].clip1 << 16) >> 16, cy2 = int(prims[pi].clip1 >> 16);
    if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard;
    bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u;
    bool gour = (kf & (1u << 7)) != 0u, dith = (kf & (1u << 8)) != 0u;
    uint depth = (kf >> 9) & 3u;
    uint col = prims[pi].color;
    int cr = int(col & 255u), cg = int((col >> 8) & 255u), cb = int((col >> 16) & 255u);
    uint uu = 0u, vv = 0u;
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
            int ui, vi;
            if ((kf & (1u << 21)) != 0u) {
                int xe = span_end(pi, Y, cx2);
                uvec2 U, V;
                persp_uv(pi, xs, xe, X, Y, U, V);
                ui = lint(lsar(U, 24)); vi = lint(lsar(V, 24));
            } else {
                ui = lint(lsar(attr_span(pi, 3, xs, X, m, u.sx, u.sy), 24));
                vi = lint(lsar(attr_span(pi, 4, xs, X, m, u.sx, u.sy), 24));
            }
            if ((kf & (1u << 15)) != 0u) {
                uint uvr = prims[pi].uvr;
                int umin = int(uvr & 255u), umax = int((uvr >> 8) & 255u), vmin = int((uvr >> 16) & 255u), vmax = int(uvr >> 24);
                ui = ui < umin ? umin : (ui > umax ? umax : ui);
                vi = vi < vmin ? vmin : (vi > vmax ? vmax : vi);
            }
            uu = uint(ui); vv = uint(vi);
        }
    } else if (kind == 1u) {
        uint a0 = prims[pi].aux0, a1 = prims[pi].aux1;
        int x0 = int(a0 << 16) >> 16, y0 = int(a0) >> 16;
        uint u0 = a1 & 255u, v0 = (a1 >> 8) & 255u;
        bool fx = (kf & (1u << 16)) != 0u, fy = (kf & (1u << 17)) != 0u;
        if (tex && fx) u0 |= 1u;
        uu = (u0 + uint(fx ? x0 - x1 : x1 - x0)) & 255u;
        vv = (v0 + uint(fy ? y0 - y1 : y1 - y0)) & 255u;
    }
    int d = dith ? DITH[(y1 & 3) * 4 + (x1 & 3)] : 0;
    uint outc;
    bool blend;
    if (tex) {
        uint t = tex_fetch(pi, depth, uu, vv);
        if (t == 0u) discard;
        if (raw) outc = t;
        else {
            int tr = int(t & 31u), tg = int((t >> 5) & 31u), tb = int((t >> 10) & 31u);
            if (dith) { tr = clamp255(((tr * cr) >> 4) + d) >> 3; tg = clamp255(((tg * cg) >> 4) + d) >> 3; tb = clamp255(((tb * cb) >> 4) + d) >> 3; }
            else { tr = min((tr * cr) >> 7, 31); tg = min((tg * cg) >> 7, 31); tb = min((tb * cb) >> 7, 31); }
            outc = uint(tr) | (uint(tg) << 5) | (uint(tb) << 10) | (t & 0x8000u);
        }
        blend = semi && (t & 0x8000u) != 0u;
    } else {
        int r, g, b;
        if (dith) { r = clamp255(cr + d) >> 3; g = clamp255(cg + d) >> 3; b = clamp255(cb + d) >> 3; }
        else { r = cr >> 3; g = cg >> 3; b = cb >> 3; }
        outc = uint(r) | (uint(g) << 5) | (uint(b) << 10);
        blend = semi;
    }
    if (blend != (u.pass == 1)) discard;
    if ((kf & (1u << 13)) != 0u) outc |= 0x8000u;
    o_color = pack(outc, u.pass, u.mode);
}
#endif
