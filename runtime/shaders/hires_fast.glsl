/* Vulkan (SPIR-V) and Direct3D 12 (DXIL) shaders of the SDL_GPU hi-res backend's *fast* precision
 * (runtime/hires_gpu.c "Fast precision"; --gpu-precision fast): the same logic as vs_fast / fs_fast
 * in the MSL source of hires_gpu.c (keep the two in step). Unlike hires.glsl nothing here is
 * bit-exact: the hardware rasterizes float vertices and interpolates u/v (perspective-correct from
 * the clip w, which is the PGXP depth or 1) and colour (screen-linear); the fragment shader decodes
 * the 4/8/15-bit texel from the 1x VRAM texture with the texture window and CLUT, and then uses the
 * exact backend's integer texel / modulation / blend-input formulas. 32-bit integers only.
 * Compiled by tools/gen_spirv.py into runtime/shaders/hires_fast_spirv.h and by tools/gen_dxil.py
 * into runtime/shaders/hires_fast_dxil.h: regenerate both after an edit. Entry points:
 *   VS_FAST / FS_FAST      triangles and rects into the 15-bit or RGBA8 target
 *
 * Resource sets (SDL_GPU SPIR-V convention): vertex uniforms set 1; the VRAM texture set 2
 * binding 0, the primitive storage buffer set 2 binding 1, fragment uniforms set 3.
 */
#version 450

#if defined(VS_FAST)
layout(location = 0) in vec2 in_pos;
layout(location = 1) in float in_w;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_col;
layout(location = 4) in uint in_prim;
layout(location = 0) out vec2 v_uv;
layout(location = 1) noperspective out vec3 v_col;
layout(location = 2) flat out uint v_prim;
layout(std140, set = 1, binding = 0) uniform VU { vec4 k; } vu;
void main()
{
    float w = in_w;
    gl_Position = vec4((in_pos.x * vu.k.x - 1.0) * w, (1.0 - in_pos.y * vu.k.y) * w, 0.0, w);
    v_uv = in_uv;
    v_col = in_col.rgb * 255.0;
    v_prim = in_prim;
}
#endif

#if defined(FS_FAST)
struct FPrim { uint kf, tp, clut, tw, clip0, clip1, ur, vr; };   /* = FPrim in hires_gpu.c */
layout(set = 2, binding = 0) uniform usampler2D t_vram;
layout(std430, set = 2, binding = 1) readonly buffer FPrims { FPrim prims[]; };
layout(std140, set = 3, binding = 0) uniform FU { int sx, sy, ox, oy, pass, mode, rx, ry, rw, rh, p0, p1; } u;
layout(location = 0) in vec2 v_uv;
layout(location = 1) noperspective in vec3 v_col;
layout(location = 2) flat in uint v_prim;
layout(location = 0) out vec4 o_color;

const int DITH[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

int floordiv(int a, int b) { return a >= 0 ? a / b : -int((uint(-(a + 1)) + uint(b)) / uint(b)); }
uint vram_at(uint x, uint y) { return texelFetch(t_vram, ivec2(int(x & 1023u), int(y & 511u)), 0).r; }
uint f_fetch(uint tp, uint clut, uint tw, uint depth, uint uu, uint vv)
{
    uu = (uu & (tw & 0xFFu)) | ((tw >> 8) & 0xFFu);
    vv = (vv & ((tw >> 16) & 0xFFu)) | (tw >> 24);
    uint tpx = tp & 0xFFFFu, row = ((tp >> 16) + vv) & 511u, cx = clut & 0xFFFFu, cy = clut >> 16;
    if (depth == 0u) { uint w = vram_at(tpx + (uu >> 2), row); return vram_at(cx + ((w >> ((uu & 3u) * 4u)) & 0xFu), cy); }
    if (depth == 1u) { uint w = vram_at(tpx + (uu >> 1), row); return vram_at(cx + ((w >> ((uu & 1u) * 8u)) & 0xFFu), cy); }
    return vram_at(tpx + uu, row);
}
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
int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
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
    if (cov == 65536u) {
        o = ivec4(int((sr + 128u) >> 8), int((sg + 128u) >> 8), int((sb + 128u) >> 8), ss * 2u >= cov ? 1 : 0);
        return true;
    }
    o = ivec4(int((sr * 256u + (cov >> 1)) / cov), int((sg * 256u + (cov >> 1)) / cov), int((sb * 256u + (cov >> 1)) / cov),
              ss * 2u >= cov ? 1 : 0);
    return true;
}
void clamp_axis(inout int i, inout int f, int lo, int hi) { if (i < lo) { i = lo; f = 0; } else if (i >= hi) { i = hi; f = 0; } }
bool f_samp(uint tp, uint clut, uint tw, uint depth, bool xbr, int iu, int fu, int iv, int fv, int ulo, int uhi, int vlo, int vhi,
            int sx, int sy, out ivec4 o)
{
    uint t[5] = uint[5](0u, 0u, 0u, 0u, 0u), w[5] = uint[5](0u, 0u, 0u, 0u, 0u);
    int n;
    if (!xbr) {
        int iu1 = iu < uhi ? iu + 1 : uhi, iv1 = iv < vhi ? iv + 1 : vhi;
        t[0] = f_fetch(tp, clut, tw, depth, uint(iu), uint(iv));
        if (fu != 0) t[1] = f_fetch(tp, clut, tw, depth, uint(iu1), uint(iv));
        if (fv != 0) t[2] = f_fetch(tp, clut, tw, depth, uint(iu), uint(iv1));
        if (fu != 0 && fv != 0) t[3] = f_fetch(tp, clut, tw, depth, uint(iu1), uint(iv1));
        w[0] = uint((256 - fu) * (256 - fv)); w[1] = uint(fu * (256 - fv)); w[2] = uint((256 - fu) * fv); w[3] = uint(fu * fv);
        n = 4;
    } else {
        int ie = iu, dx = fu, je = iv, dy = fv;
        if (fu >= 128) { ie = iu + 1; dx = fu - 256; }
        if (fv >= 128) { je = iv + 1; dy = fv - 256; }
        int hx = dx >= 0 ? 1 : -1, hy = dy >= 0 ? 1 : -1;
        t[0] = f_fetch(tp, clut, tw, depth, uint(ie), uint(je));
        t[1] = f_fetch(tp, clut, tw, depth, uint(min(max(ie + hx, ulo), uhi)), uint(je));
        t[2] = f_fetch(tp, clut, tw, depth, uint(ie), uint(min(max(je + hy, vlo), vhi)));
        t[3] = f_fetch(tp, clut, tw, depth, uint(min(max(ie - hx, ulo), uhi)), uint(je));
        t[4] = f_fetch(tp, clut, tw, depth, uint(ie), uint(min(max(je - hy, vlo), vhi)));
        w[0] = 65536u; n = 2;
        if (t[1] == t[2] && t[1] != t[0] && t[1] != t[3] && t[2] != t[4]) {
            int d8 = abs(dx) + abs(dy) - 128, a = min(max(((d8 * (sx + sy)) >> 1) + 128, 0), 256);
            w[0] = uint(256 - a) * 256u; w[1] = uint(a) * 256u;
        }
    }
    return tcomb(t, w, n, o);
}
uvec4 tex_out(ivec4 t, int cr, int cg, int cb, bool raw, bool dith, int d, int hp)
{
    int r, g, b;
    if (raw) { int s = hp != 0 ? 5 : 8; r = t.x >> s; g = t.y >> s; b = t.z >> s; }
    else if (hp != 0) { r = min((t.x * cr) >> 12, 255); g = min((t.y * cg) >> 12, 255); b = min((t.z * cb) >> 12, 255); }
    else if (dith) { r = clamp255(((t.x * cr) >> 12) + d) >> 3; g = clamp255(((t.y * cg) >> 12) + d) >> 3; b = clamp255(((t.z * cb) >> 12) + d) >> 3; }
    else { r = min((t.x * cr) >> 15, 31); g = min((t.y * cg) >> 15, 31); b = min((t.z * cb) >> 15, 31); }
    return uvec4(uint(r), uint(g), uint(b), uint(t.w));
}

void main()
{
    FPrim p = prims[v_prim];
    int X = int(gl_FragCoord.x) + u.ox, Y = int(gl_FragCoord.y) + u.oy;
    int cx1 = int(p.clip0 << 16) >> 16, cy1 = int(p.clip0 >> 16), cx2 = int(p.clip1 << 16) >> 16, cy2 = int(p.clip1 >> 16);
    if (X < cx1 || X >= cx2 || Y < cy1 || Y >= cy2) discard;
    uint kf = p.kf;
    int hp = u.p0;
    bool tex = (kf & (1u << 4)) != 0u, raw = (kf & (1u << 5)) != 0u, semi = (kf & (1u << 6)) != 0u, dith = (kf & (1u << 8)) != 0u;
    ivec3 c = ivec3(clamp(floor(v_col + 0.5), 0.0, 255.0));
    int d = 0;
    if (dith) { int x1 = floordiv(X, u.sx), y1 = floordiv(Y, u.sy); d = DITH[(y1 & 3) * 4 + (x1 & 3)]; }
    uvec4 outc;
    bool blend;
    if (tex) {
        uint depth = (kf >> 9) & 3u;
        ivec4 t8;
        int ulo = int(p.ur << 16) >> 16, uhi = int(p.ur) >> 16, vlo = int(p.vr << 16) >> 16, vhi = int(p.vr) >> 16;
        if ((kf & (3u << 22)) != 0u) {   /* filter sample point: without the rounding bias */
            vec2 s = v_uv - 0.5, f0 = floor(s);
            int iu = int(f0.x), iv = int(f0.y), fu = min(int((s.x - f0.x) * 256.0), 255), fv = min(int((s.y - f0.y) * 256.0), 255);
            clamp_axis(iu, fu, ulo, uhi);
            clamp_axis(iv, fv, vlo, vhi);
            if (!f_samp(p.tp, p.clut, p.tw, depth, (kf & (1u << 23)) != 0u, iu, fu, iv, fv, ulo, uhi, vlo, vhi, u.sx, u.sy, t8)) discard;
        } else {
            int iu = int(floor(v_uv.x)), iv = int(floor(v_uv.y));
            if ((kf & (1u << 15)) != 0u) { iu = clamp(iu, ulo, uhi); iv = clamp(iv, vlo, vhi); }
            uint t = f_fetch(p.tp, p.clut, p.tw, depth, uint(iu), uint(iv));
            if (t == 0u) discard;
            t8 = ivec4(int(t & 31u) << 8, int((t >> 5) & 31u) << 8, int((t >> 10) & 31u) << 8, int(t >> 15));
        }
        outc = tex_out(t8, c.x, c.y, c.z, raw, dith, d, hp);
        blend = semi && t8.w != 0;
    } else {
        ivec3 o3;
        if (hp != 0) o3 = c;
        else if (dith) o3 = ivec3(clamp255(c.x + d) >> 3, clamp255(c.y + d) >> 3, clamp255(c.z + d) >> 3);
        else o3 = c >> 3;
        outc = uvec4(uint(o3.x), uint(o3.y), uint(o3.z), 0u);
        blend = semi;
    }
    if (blend != (u.pass == 1)) discard;
    if ((kf & (1u << 13)) != 0u) outc.w = 1u;
    o_color = pack4(outc, u.pass, u.mode, hp);
}
#endif
