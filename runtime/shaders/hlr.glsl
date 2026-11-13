/* Vulkan (SPIR-V) and Direct3D 12 (DXIL) shaders of the native high-level renderer's 3D stage (runtime/hlr/hlr_gpu.c):
 * the same logic as vs_hlr / fs_hlr / vs_hblit / fs_hblit in the MSL source of hlr_gpu.c (keep
 * the two in step). Float camera, reverse-Z depth, per-pixel GTE fog and lighting
 * (runtime/hlr/hlr_cam.h has the formulas), textures from the texture cache's arrays.
 * Compiled by tools/gen_spirv.py into runtime/shaders/hlr_spirv.h (Vulkan) and by tools/gen_dxil.py
 * (-DPORTABLE_MUL, SPIRV-Cross HLSL, DXC) into runtime/shaders/hlr_dxil.h (Direct3D 12).
 * Entry points:
 *   VS_HLR / FS_HLR        stage polygons and background strips
 *   VS_HBLIT / FS_HBLIT    the target rectangle into every sample of the MSAA colour texture (a
 *                          later pass of a frame: only pixels that changed since the previous one)
 *
 * Resource sets (SDL_GPU SPIR-V convention): vertex uniforms set 1; fragment samplers set 2
 * (the texture cache's colour and STP arrays); fragment uniforms set 3.
 */
#version 450

#if defined(VS_HLR) || defined(FS_HLR)
#define HU_BLOCK { vec4 rt0, rt1, rt2, tr, proj, misc, fog, fc, llm0, llm1, llm2, lcm0, lcm1, lcm2, bk; }
float h_fog(vec4 fog, float vz)
{
    if (fog.w == 0.0) return 0.0;
    float hd = vz > 0.0 ? min(fog.x / vz, 131071.0) : 131071.0;
    return clamp((hd * fog.y + fog.z) / 16777216.0, 0.0, 1.0);
}
#define H_LIGHT(u, n) (clamp(u.bk.xyz + vec3(dot(u.lcm0.xyz, clamp(vec3(dot(u.llm0.xyz, n), dot(u.llm1.xyz, n), dot(u.llm2.xyz, n)), 0.0, 8.0)), \
    dot(u.lcm1.xyz, clamp(vec3(dot(u.llm0.xyz, n), dot(u.llm1.xyz, n), dot(u.llm2.xyz, n)), 0.0, 8.0)), \
    dot(u.lcm2.xyz, clamp(vec3(dot(u.llm0.xyz, n), dot(u.llm1.xyz, n), dot(u.llm2.xyz, n)), 0.0, 8.0))), 0.0, 8.0) * (128.0 / 255.0))
#endif

#if defined(VS_HLR)
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec4 in_col;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in uint in_uvr;
layout(location = 5) in uint in_info;
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_raw;
layout(location = 2) out vec3 v_vcol;
layout(location = 3) out vec2 v_uv;
layout(location = 4) out float v_vz;
layout(location = 5) flat out uint v_uvr;
layout(location = 6) flat out uint v_info;
layout(std140, set = 1, binding = 0) uniform HU HU_BLOCK u;
void main()
{
    vec3 p = in_pos;
    float vx = dot(u.rt0.xyz, p) + u.tr.x, vy = dot(u.rt1.xyz, p) + u.tr.y, vz = dot(u.rt2.xyz, p) + u.tr.z;
    if ((in_info & 0x1000u) != 0u) { gl_Position = vec4(p.xy, 0.0, 1.0); vz = 1.0; }
    else gl_Position = vec4(u.proj.x * vx + u.proj.y * vz, u.proj.z * vy + u.proj.w * vz,
                            (in_info & 0x8000u) != 0u ? u.misc.x * vz / max(vz - u.misc.w, 0.5 * u.misc.x) : u.misc.x, vz);
    uint shade = (in_info >> 10) & 3u;
    v_nrm = in_nrm; v_raw = in_col.rgb; v_vz = vz; v_uv = in_uv; v_uvr = in_uvr; v_info = in_info;
    vec3 c = shade == 1u ? H_LIGHT(u, in_nrm) : in_col.rgb;
    if (shade != 0u) c = c + (u.fc.xyz - c) * h_fog(u.fog, vz);
    v_vcol = clamp(c, 0.0, 1.0);
}
#endif

#if defined(FS_HLR)
layout(set = 2, binding = 0) uniform sampler2DArray t_tcc;
layout(set = 2, binding = 1) uniform sampler2DArray t_tcs;
layout(std140, set = 3, binding = 0) uniform HU HU_BLOCK u;
layout(location = 0) in vec3 v_nrm;
layout(location = 1) in vec3 v_raw;
layout(location = 2) in vec3 v_vcol;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in float v_vz;
layout(location = 5) flat in uint v_uvr;
layout(location = 6) flat in uint v_info;
layout(location = 0) out vec4 o_color;
void main()
{
    uint info = v_info, shade = (info >> 10) & 3u;
    vec3 c = v_vcol;
    if (u.misc.y > 0.5) {
        vec3 n = shade == 1u ? normalize(v_nrm) : v_nrm;
        c = shade == 1u ? H_LIGHT(u, n) : v_raw;
        if (shade != 0u) c = c + (u.fc.xyz - c) * h_fog(u.fog, v_vz);
        c = clamp(c, 0.0, 1.0);
    }
    if ((info & 0x100u) != 0u) {
        vec2 lo = vec2(float(v_uvr & 255u), float((v_uvr >> 16) & 255u)) + 0.5;
        vec2 hi = vec2(float((v_uvr >> 8) & 255u), float(v_uvr >> 24)) + 0.5;
        vec2 st = clamp(v_uv, lo, hi) / 256.0;
        float layer = float(info & 255u);
        vec4 t = texture(t_tcc, vec3(st, layer));
        if (t.a < 0.5) discard;
        uint sel = (info >> 13) & 3u;
        if (sel != 0u) { bool s = texture(t_tcs, vec3(st, layer)).r >= t.a * 0.5; if ((sel == 1u) == s) discard; }
        vec3 tx = t.rgb / t.a;
        c = (info & 0x200u) != 0u ? tx : min(tx * c * (255.0 / 128.0), vec3(1.0));
    }
    o_color = u.misc.z > 0.5 ? vec4(c.bgr, 0.0) : vec4(c, 0.0);   /* B5G5R5A1 target: PS1 red in .b */
}
#endif

#if defined(VS_HBLIT)
void main()
{
    uint vid = uint(gl_VertexIndex);
    gl_Position = vec4(vid == 1u ? 3.0 : -1.0, vid == 2u ? -3.0 : 1.0, 0.0, 1.0);
}
#endif

#if defined(FS_HBLIT)
layout(set = 2, binding = 0) uniform sampler2D t_target;
layout(set = 2, binding = 1) uniform sampler2D t_prev;     /* the previous pass's resolve */
layout(std140, set = 3, binding = 0) uniform BU { ivec4 off; } bu;   /* xy: rectangle origin, z: keep */
layout(location = 0) out vec4 o_color;
void main()
{
    vec4 c = texelFetch(t_target, ivec2(gl_FragCoord.xy) + bu.off.xy, 0);
    /* unchanged since the previous pass of this frame: keep the multisampled pixel's samples */
    if (bu.off.z != 0 && all(equal(c, texelFetch(t_prev, ivec2(gl_FragCoord.xy), 0)))) discard;
    o_color = c;
}
#endif
