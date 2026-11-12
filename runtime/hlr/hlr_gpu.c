/* Native 3D stage drawing (docs/HLR_PLAN.md 2b; hlr_gpu.h). Presentation only.
 *
 * Each pass (HiresBackend.external: called at submit time with the backend's command buffer and
 * target, no render or copy pass open) draws part of a frame's stage polygons with real depth:
 *
 *   1. the pass rectangle of the target (the stage's draw area, guard band included) is brought
 *      into an offscreen colour texture: a texture copy without MSAA; with MSAA a full-rectangle
 *      draw reads the target into the multisampled texture (every sample = the target's pixel);
 *   2. the depth texture (D32 float, else D24) is cleared to 0 by the frame's first pass and
 *      loaded by the later ones (reverse Z: z_ndc = near / view z, test GREATER_OR_EQUAL, so far
 *      away is 0 and there is no far plane);
 *   3. the polygons: opaque ones (depth written), then the semi-transparent ones in the order
 *      given (depth tested, not written; the four PS1 blend modes as fixed-function blends);
 *   4. MSAA resolves into the single-sample texture; the rectangle is copied back into the target.
 *
 * Shaders: MSL below (Metal), shaders/hlr.glsl -> shaders/hlr_spirv.h (Vulkan; tools/gen_spirv.py)
 * and -> shaders/hlr_dxil.h (Direct3D 12; tools/gen_dxil.py: GLSL -> SPIR-V -> HLSL -> signed
 * DXIL, registers in SDL's set = space convention). */
#include "hlr_gpu.h"
#include "hlr.h"
#include "../hires_gpu.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "../shaders/hlr_spirv.h"
#include "../shaders/hlr_dxil.h"

static const char s_msl[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct HU { float4 rt0, rt1, rt2, tr, proj, misc, fog, fc, llm0, llm1, llm2, lcm0, lcm1, lcm2, bk; };\n"
"struct HVI { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float4 col [[attribute(2)]];\n"
"             float2 uv [[attribute(3)]]; uint uvr [[attribute(4)]]; uint info [[attribute(5)]]; };\n"
"struct HVO { float4 pos [[position]]; float3 nrm; float3 raw; float3 vcol; float2 uv; float vz;\n"
"             uint uvr [[flat]]; uint info [[flat]]; };\n"
"static float h_fog(constant HU &u, float vz) {\n"
"  if (u.fog.w == 0.0) return 0.0;\n"
"  float hd = vz > 0.0 ? min(u.fog.x / vz, 131071.0) : 131071.0;\n"
"  return clamp((hd * u.fog.y + u.fog.z) / 16777216.0, 0.0, 1.0); }\n"
"static float3 h_light(constant HU &u, float3 n) {\n"
"  float3 l = clamp(float3(dot(u.llm0.xyz, n), dot(u.llm1.xyz, n), dot(u.llm2.xyz, n)), 0.0, 8.0);\n"
"  float3 c = clamp(u.bk.xyz + float3(dot(u.lcm0.xyz, l), dot(u.lcm1.xyz, l), dot(u.lcm2.xyz, l)), 0.0, 8.0);\n"
"  return c * (128.0 / 255.0); }\n"
"static float3 h_shade(constant HU &u, uint shade, float3 raw, float3 n, float vz) {\n"
"  float3 c = shade == 1u ? h_light(u, n) : raw;\n"
"  if (shade != 0u) c = c + (u.fc.xyz - c) * h_fog(u, vz);\n"
"  return clamp(c, 0.0, 1.0); }\n"
"vertex HVO vs_hlr(HVI in [[stage_in]], constant HU &u [[buffer(0)]]) {\n"
"  HVO o; float3 p = in.pos;\n"
"  float vx = dot(u.rt0.xyz, p) + u.tr.x, vy = dot(u.rt1.xyz, p) + u.tr.y, vz = dot(u.rt2.xyz, p) + u.tr.z;\n"
"  if ((in.info & 0x1000u) != 0u) { o.pos = float4(p.xy, 0.0, 1.0); vz = 1.0; }\n"
"  else o.pos = float4(u.proj.x * vx + u.proj.y * vz, u.proj.z * vy + u.proj.w * vz, u.misc.x, vz);\n"
"  uint shade = (in.info >> 10) & 3u;\n"
"  o.nrm = in.nrm; o.raw = in.col.rgb; o.vz = vz; o.uv = in.uv; o.uvr = in.uvr; o.info = in.info;\n"
"  o.vcol = h_shade(u, shade, in.col.rgb, in.nrm, vz);\n"
"  return o; }\n"
"fragment float4 fs_hlr(HVO in [[stage_in]], constant HU &u [[buffer(0)]],\n"
"    texture2d_array<float> tc [[texture(0)]], sampler ts [[sampler(0)]],\n"
"    texture2d_array<float> tstp [[texture(1)]], sampler ss [[sampler(1)]]) {\n"
"  uint info = in.info, shade = (info >> 10) & 3u;\n"
"  float3 c = u.misc.y > 0.5 ? h_shade(u, shade, in.raw, shade == 1u ? normalize(in.nrm) : in.nrm, in.vz) : in.vcol;\n"
"  if ((info & 0x100u) != 0u) {\n"
"    float2 lo = float2(float(in.uvr & 255u), float((in.uvr >> 16) & 255u)) + 0.5;\n"
"    float2 hi = float2(float((in.uvr >> 8) & 255u), float(in.uvr >> 24)) + 0.5;\n"
"    float2 st = clamp(in.uv, lo, hi) / 256.0;\n"
"    float layer = float(info & 255u);\n"
"    float4 t = tc.sample(ts, st, uint(layer));\n"
"    if (t.a < 0.5) discard_fragment();\n"
"    uint sel = (info >> 13) & 3u;\n"
"    if (sel != 0u) { bool s = tstp.sample(ss, st, uint(layer)).r >= t.a * 0.5; if ((sel == 1u) == s) discard_fragment(); }\n"
"    float3 tx = t.rgb / t.a;\n"
"    c = (info & 0x200u) != 0u ? tx : min(tx * c * (255.0 / 128.0), 1.0);\n"
"  }\n"
"  return u.misc.z > 0.5 ? float4(c.zyx, 0.0) : float4(c, 0.0); }\n"
"struct BO { float4 pos [[position]]; };\n"
"vertex BO vs_hblit(uint vid [[vertex_id]]) {\n"
"  BO o; o.pos = float4(vid == 1u ? 3.0 : -1.0, vid == 2u ? -3.0 : 1.0, 0.0, 1.0); return o; }\n"
"struct BU { int4 off; };\n"
"fragment float4 fs_hblit(BO in [[stage_in]], constant BU &u [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
"  return t.read(uint2(int2(in.pos.xy) + u.off.xy)); }\n";

enum { NBLEND = 5 };                /* opaque, modes 0..3 */
typedef struct { int fmt, samples; SDL_GPUGraphicsPipeline *p[NBLEND], *blit; } PipeSet;

static SDL_GPUDevice *s_dev;        /* the device everything below was made on */
static bool s_failed;               /* init failed on this device */
static const char *s_why = "no GPU renderer";
static SDL_GPUShader *s_vs, *s_fs, *s_bvs, *s_bfs;
static PipeSet s_ps[4];
static int s_nps;
static SDL_GPUTextureFormat s_dfmt;
static SDL_GPUTexture *s_dummy;
static SDL_GPUSampler *s_psamp;    /* point sampling, level 0 only (--hlr-look ps1) */
static SDL_GPUBuffer *s_vbuf; static u32 s_vbuf_cap;
static SDL_GPUTransferBuffer *s_tbuf; static u32 s_tbuf_cap;
static SDL_GPUTexture *s_col, *s_msaa, *s_depth;     /* pass textures */
static int s_tex_w, s_tex_h, s_tex_fmt, s_tex_samples;
static int s_msaa_req = 4;
static HlrGpuStats S;
static u64 s_serial;

void hlr_gpu_get_stats(HlrGpuStats *st) { *st = S; }
void hlr_gpu_set_msaa(int n) { s_msaa_req = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }
int hlr_gpu_get_msaa(void) { return s_msaa_req; }

static void release_all(void)
{
    if (!s_dev) return;
    for (int i = 0; i < s_nps; i++) {
        for (int k = 0; k < NBLEND; k++) if (s_ps[i].p[k]) SDL_ReleaseGPUGraphicsPipeline(s_dev, s_ps[i].p[k]);
        if (s_ps[i].blit) SDL_ReleaseGPUGraphicsPipeline(s_dev, s_ps[i].blit);
    }
    s_nps = 0;
    SDL_GPUShader *sh[4] = { s_vs, s_fs, s_bvs, s_bfs };
    for (int i = 0; i < 4; i++) if (sh[i]) SDL_ReleaseGPUShader(s_dev, sh[i]);
    s_vs = s_fs = s_bvs = s_bfs = NULL;
    SDL_GPUTexture *tx[4] = { s_dummy, s_col, s_msaa, s_depth };
    for (int i = 0; i < 4; i++) if (tx[i]) SDL_ReleaseGPUTexture(s_dev, tx[i]);
    s_dummy = s_col = s_msaa = s_depth = NULL;
    s_tex_w = s_tex_h = 0;
    if (s_vbuf) SDL_ReleaseGPUBuffer(s_dev, s_vbuf);
    if (s_tbuf) SDL_ReleaseGPUTransferBuffer(s_dev, s_tbuf);
    if (s_psamp) SDL_ReleaseGPUSampler(s_dev, s_psamp);
    s_psamp = NULL;
    s_vbuf = NULL; s_tbuf = NULL; s_vbuf_cap = s_tbuf_cap = 0;
    s_dev = NULL;
    s_failed = false;
}
static void on_shutdown(void) { release_all(); }

static SDL_GPUShader *shader(const char *entry, SDL_GPUShaderStage stage, u32 samplers, u32 ubufs, SDL_GPUShaderFormat fmt)
{
    SDL_GPUShaderCreateInfo si = { .code = (const Uint8 *)s_msl, .code_size = sizeof s_msl - 1, .entrypoint = entry,
                                   .format = SDL_GPU_SHADERFORMAT_MSL, .stage = stage, .num_samplers = samplers,
                                   .num_uniform_buffers = ubufs };
    if (fmt == SDL_GPU_SHADERFORMAT_SPIRV || fmt == SDL_GPU_SHADERFORMAT_DXIL) {
        /* one module per entry point (from shaders/hlr.glsl), each with entry "main" */
        static const struct { const char *name; const unsigned char *spv, *dxil; size_t spv_n, dxil_n; } mods[] = {
            { "vs_hlr", s_spv_vs_hlr, s_dxil_vs_hlr, sizeof s_spv_vs_hlr, sizeof s_dxil_vs_hlr },
            { "fs_hlr", s_spv_fs_hlr, s_dxil_fs_hlr, sizeof s_spv_fs_hlr, sizeof s_dxil_fs_hlr },
            { "vs_hblit", s_spv_vs_hblit, s_dxil_vs_hblit, sizeof s_spv_vs_hblit, sizeof s_dxil_vs_hblit },
            { "fs_hblit", s_spv_fs_hblit, s_dxil_fs_hblit, sizeof s_spv_fs_hblit, sizeof s_dxil_fs_hblit },
        };
        const bool dx = fmt == SDL_GPU_SHADERFORMAT_DXIL;
        for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++)
            if (!strcmp(mods[i].name, entry)) {
                si.code = dx ? mods[i].dxil : mods[i].spv;
                si.code_size = dx ? mods[i].dxil_n : mods[i].spv_n;
            }
        si.entrypoint = "main";
        si.format = fmt;
    }
    SDL_GPUShader *s = SDL_CreateGPUShader(s_dev, &si);
    if (!s) rt_log("hlr-gpu: shader %s: %s", entry, SDL_GetError());
    return s;
}

static bool ensure_device(void)
{
    HiresGpuInfo gi;
    if (!hires_gpu_info(&gi)) { s_why = "no GPU renderer"; return false; }
    if (gi.dev != s_dev) {
        release_all();
        s_dev = gi.dev;
        hires_gpu_on_shutdown(on_shutdown);
        if (gi.shader_format != SDL_GPU_SHADERFORMAT_MSL && gi.shader_format != SDL_GPU_SHADERFORMAT_SPIRV &&
            gi.shader_format != SDL_GPU_SHADERFORMAT_DXIL) {
            s_why = "no native renderer shaders for this graphics API";
            s_failed = true;
            rt_log("hlr-gpu: %s; the stage is replayed instead", s_why);
            return false;
        }
        const SDL_GPUShaderFormat f = (SDL_GPUShaderFormat)gi.shader_format;
        s_vs = shader("vs_hlr", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, f);
        s_fs = shader("fs_hlr", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, f);
        s_bvs = shader("vs_hblit", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, f);
        s_bfs = shader("fs_hblit", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, f);
        const SDL_GPUTextureUsageFlags du = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        s_dfmt = SDL_GPUTextureSupportsFormat(s_dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, du)
                     ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 1, .height = 1,
                                        .layer_count_or_depth = 1, .num_levels = 1 };
        s_dummy = SDL_CreateGPUTexture(s_dev, &ti);
        SDL_GPUSamplerCreateInfo sci = { .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
                                         .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                         .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                         .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                         .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                         .min_lod = 0.0f, .max_lod = 0.0f };
        s_psamp = SDL_CreateGPUSampler(s_dev, &sci);
        if (!s_vs || !s_fs || !s_bvs || !s_bfs || !s_dummy || !s_psamp) {
            s_why = "shader or texture creation failed";
            s_failed = true;
            return false;
        }
        rt_log("hlr-gpu: native stage renderer ready (%s shaders, depth %s)",
               f == SDL_GPU_SHADERFORMAT_MSL ? "MSL" : f == SDL_GPU_SHADERFORMAT_DXIL ? "DXIL" : "SPIR-V", s_dfmt == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "D32" : "D24");
    }
    return !s_failed;
}

bool hlr_gpu_available(void) { return ensure_device(); }
const char *hlr_gpu_unavailable_reason(void) { return s_why; }

static SDL_GPUSampleCount sc_of(int n)
{
    return n >= 8 ? SDL_GPU_SAMPLECOUNT_8 : n >= 4 ? SDL_GPU_SAMPLECOUNT_4 : n >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

int hlr_gpu_msaa_for(int fmt)
{
    if (!ensure_device()) return 1;
    for (int n = s_msaa_req; n > 1; n >>= 1)
        if (SDL_GPUTextureSupportsSampleCount(s_dev, (SDL_GPUTextureFormat)fmt, sc_of(n)) &&
            SDL_GPUTextureSupportsSampleCount(s_dev, s_dfmt, sc_of(n)))
            return n;
    return 1;
}

static void blend_state(int k, SDL_GPUColorTargetBlendState *b)
{
    memset(b, 0, sizeof *b);
    if (k == 0) return;
    b->enable_blend = true;
    b->color_blend_op = SDL_GPU_BLENDOP_ADD;
    b->alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    b->src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;    /* keep the target's mask bit */
    b->dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    switch (k - 1) {
    case 0: b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;          /* B/2 + F/2 (constant 0.5) */
            b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR; break;
    case 1: b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;
    case 2: b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            b->color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT; break;             /* B - F */
    case 3: b->src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;          /* B + F/4 (constant 0.25) */
            b->dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; break;
    }
}

static PipeSet *pipes(int fmt, int samples)
{
    for (int i = 0; i < s_nps; i++) if (s_ps[i].fmt == fmt && s_ps[i].samples == samples) return &s_ps[i];
    if (s_nps == 4) return NULL;
    PipeSet *ps = &s_ps[s_nps];
    memset(ps, 0, sizeof *ps);
    ps->fmt = fmt; ps->samples = samples;
    SDL_GPUVertexBufferDescription vbd = { .slot = 0, .pitch = sizeof(HlrVtx), .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX };
    SDL_GPUVertexAttribute va[6] = {
        { .location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = 0 },
        { .location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = 12 },
        { .location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = 24 },
        { .location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 28 },
        { .location = 4, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 36 },
        { .location = 5, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 40 },
    };
    for (int k = 0; k <= NBLEND; k++) {
        const bool blit = k == NBLEND;
        SDL_GPUColorTargetDescription cd = { .format = (SDL_GPUTextureFormat)fmt };
        blend_state(blit ? 0 : k, &cd.blend_state);
        SDL_GPUGraphicsPipelineCreateInfo pi = {
            .vertex_shader = blit ? s_bvs : s_vs, .fragment_shader = blit ? s_bfs : s_fs,
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE },
            .multisample_state = { .sample_count = sc_of(samples) },
            .depth_stencil_state = { .compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, .enable_depth_test = !blit,
                                     .enable_depth_write = k == 0 },
            .target_info = { .color_target_descriptions = &cd, .num_color_targets = 1,
                             .depth_stencil_format = s_dfmt, .has_depth_stencil_target = true },
        };
        if (!blit) {
            pi.vertex_input_state = (SDL_GPUVertexInputState){ .vertex_buffer_descriptions = &vbd, .num_vertex_buffers = 1,
                                                               .vertex_attributes = va, .num_vertex_attributes = 6 };
        }
        SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(s_dev, &pi);
        if (!p) {
            rt_log("hlr-gpu: pipeline %d (format %d, %d samples): %s", k, fmt, samples, SDL_GetError());
            for (int j = 0; j < k; j++) SDL_ReleaseGPUGraphicsPipeline(s_dev, ps->p[j]);
            return NULL;
        }
        if (blit) ps->blit = p; else ps->p[k] = p;
    }
    s_nps++;
    return ps;
}

static bool pass_textures(int w, int h, int fmt, int samples)
{
    if (s_col && s_tex_w == w && s_tex_h == h && s_tex_fmt == fmt && s_tex_samples == samples) return true;
    SDL_GPUTexture *tx[3] = { s_col, s_msaa, s_depth };
    for (int i = 0; i < 3; i++) if (tx[i]) SDL_ReleaseGPUTexture(s_dev, tx[i]);
    s_col = s_msaa = s_depth = NULL;
    SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D, .format = (SDL_GPUTextureFormat)fmt,
                                    .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                    .width = (u32)w, .height = (u32)h, .layer_count_or_depth = 1, .num_levels = 1 };
    s_col = SDL_CreateGPUTexture(s_dev, &ti);
    if (samples > 1) {
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.sample_count = sc_of(samples);
        s_msaa = SDL_CreateGPUTexture(s_dev, &ti);
    }
    ti.format = s_dfmt;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ti.sample_count = sc_of(samples);
    s_depth = SDL_CreateGPUTexture(s_dev, &ti);
    if (!s_col || !s_depth || (samples > 1 && !s_msaa)) {
        rt_log("hlr-gpu: pass textures %dx%d (%d samples): %s", w, h, samples, SDL_GetError());
        SDL_GPUTexture *t2[3] = { s_col, s_msaa, s_depth };
        for (int i = 0; i < 3; i++) if (t2[i]) SDL_ReleaseGPUTexture(s_dev, t2[i]);
        s_col = s_msaa = s_depth = NULL;
        s_tex_w = 0;
        return false;
    }
    s_tex_w = w; s_tex_h = h; s_tex_fmt = fmt; s_tex_samples = samples;
    return true;
}

HlrGpuFrame *hlr_gpu_frame_new(void)
{
    HlrGpuFrame *f = calloc(1, sizeof *f);
    if (!f) rt_fatal("hlr-gpu: out of memory");
    f->refs = 1;
    f->serial = ++s_serial;
    return f;
}

void hlr_gpu_frame_unref(HlrGpuFrame *f)
{
    if (f && --f->refs == 0) { free(f->v); free(f); }
}

HlrVtx *hlr_gpu_frame_vtx(HlrGpuFrame *f, u32 n)
{
    if (f->nv + n > f->vcap) {
        while (f->nv + n > f->vcap) f->vcap = f->vcap ? f->vcap * 2 : 16384;
        f->v = realloc(f->v, (size_t)f->vcap * sizeof *f->v);
        if (!f->v) rt_fatal("hlr-gpu: out of memory");
    }
    HlrVtx *v = f->v + f->nv;
    memset(v, 0, (size_t)n * sizeof *v);
    f->nv += n;
    return v;
}

static bool upload(SDL_GPUCommandBuffer *cb, HlrGpuFrame *f)
{
    const u32 bytes = f->nv * (u32)sizeof(HlrVtx);
    if (!bytes) return true;
    if (bytes > s_vbuf_cap) {
        if (s_vbuf) SDL_ReleaseGPUBuffer(s_dev, s_vbuf);
        u32 cap = 1u << 20;
        while (cap < bytes) cap <<= 1;
        SDL_GPUBufferCreateInfo bi = { .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = cap };
        s_vbuf = SDL_CreateGPUBuffer(s_dev, &bi);
        s_vbuf_cap = s_vbuf ? cap : 0;
        if (!s_vbuf) return false;
    }
    if (bytes > s_tbuf_cap) {
        if (s_tbuf) SDL_ReleaseGPUTransferBuffer(s_dev, s_tbuf);
        SDL_GPUTransferBufferCreateInfo ti = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = s_vbuf_cap };
        s_tbuf = SDL_CreateGPUTransferBuffer(s_dev, &ti);
        s_tbuf_cap = s_tbuf ? s_vbuf_cap : 0;
        if (!s_tbuf) return false;
    }
    void *m = SDL_MapGPUTransferBuffer(s_dev, s_tbuf, true);
    if (!m) return false;
    memcpy(m, f->v, bytes);
    SDL_UnmapGPUTransferBuffer(s_dev, s_tbuf);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src = { .transfer_buffer = s_tbuf, .offset = 0 };
    SDL_GPUBufferRegion dst = { .buffer = s_vbuf, .offset = 0, .size = bytes };
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);
    S.uploads++;
    S.upload_bytes += bytes;
    return true;
}

void hlr_gpu_external(void *ctx, void *cmdbuf, void *colour_tex, int w, int h)
{
    (void)w; (void)h;
    HlrGpuPass *ps = ctx;
    HlrGpuFrame *f = ps->f;
    SDL_GPUCommandBuffer *cb = cmdbuf;
    SDL_GPUTexture *target = colour_tex;
    HiresGpuInfo gi;
    bool ok = ensure_device() && hires_gpu_info(&gi) && target && f->pw > 0 && f->ph > 0 &&
              f->px0 >= 0 && f->py0 >= 0 && f->px0 + f->pw <= gi.aw && f->py0 + f->ph <= gi.ah;
    const int samples = f->samples > 1 ? f->samples : 1;
    PipeSet *pp = ok ? pipes(gi.target_format, samples) : NULL;
    ok = ok && pp && pass_textures(f->pw, f->ph, gi.target_format, samples);
    if (ok && !f->uploaded) { ok = upload(cb, f); f->uploaded = ok; }
    if (!ok) { S.failed++; goto out; }
    {
        /* 1. target rectangle -> colour texture (without MSAA; with MSAA the blit below reads it) */
        if (samples == 1) {
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
            SDL_GPUTextureLocation src = { .texture = target, .x = (u32)f->px0, .y = (u32)f->py0 };
            SDL_GPUTextureLocation dst = { .texture = s_col };
            SDL_CopyGPUTextureToTexture(cp, &src, &dst, (u32)f->pw, (u32)f->ph, 1, false);
            SDL_EndGPUCopyPass(cp);
        }
        SDL_GPUColorTargetInfo ct = { .texture = samples > 1 ? s_msaa : s_col,
                                      .load_op = samples > 1 ? SDL_GPU_LOADOP_DONT_CARE : SDL_GPU_LOADOP_LOAD,
                                      .store_op = samples > 1 ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE,
                                      .resolve_texture = samples > 1 ? s_col : NULL };
        SDL_GPUDepthStencilTargetInfo dt = { .texture = s_depth, .clear_depth = 0.0f,
                                             .load_op = f->cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
                                             .store_op = SDL_GPU_STOREOP_STORE,
                                             .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
                                             .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE };
        f->cleared = true;
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cb, &ct, 1, &dt);
        if (!rp) { S.failed++; goto out; }
        if (samples > 1) {   /* 2. the target's pixels into every sample */
            SDL_BindGPUGraphicsPipeline(rp, pp->blit);
            SDL_GPUTextureSamplerBinding tb = { target, gi.nearest };
            SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
            const int off[4] = { f->px0, f->py0, 0, 0 };
            SDL_PushGPUFragmentUniformData(cb, 0, off, sizeof off);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            S.msaa_passes++;
        }
        SDL_GPUBufferBinding vb = { .buffer = s_vbuf, .offset = 0 };
        SDL_GPUTexture *tcc = gi.tc_colour ? gi.tc_colour : s_dummy, *tcs = gi.tc_stp ? gi.tc_stp : s_dummy;
        SDL_GPUSampler *smp = f->nearest_tex ? s_psamp : gi.tc_sampler;
        SDL_GPUTextureSamplerBinding tb[2] = { { tcc, smp }, { tcs, smp } };
        int bound = -2;
        for (int i = 0; i < ps->nd; i++) {
            const HlrDraw *d = &ps->d[i];
            if (!d->count) continue;
            const int k = d->blend < 0 ? 0 : 1 + (d->blend & 3);
            if (k != bound) {
                SDL_BindGPUGraphicsPipeline(rp, pp->p[k]);
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
                SDL_PushGPUVertexUniformData(cb, 0, &f->u, sizeof f->u);
                SDL_PushGPUFragmentUniformData(cb, 0, &f->u, sizeof f->u);
                if (k == 1) SDL_SetGPUBlendConstants(rp, (SDL_FColor){ 0.5f, 0.5f, 0.5f, 0.5f });
                if (k == 4) SDL_SetGPUBlendConstants(rp, (SDL_FColor){ 0.25f, 0.25f, 0.25f, 0.25f });
                bound = k;
            }
            SDL_DrawGPUPrimitives(rp, d->count, 1, d->first, 0);
            S.draws++;
            S.tris += d->count / 3;
        }
        SDL_EndGPURenderPass(rp);
        /* 4. back into the target */
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureLocation src = { .texture = s_col };
        SDL_GPUTextureLocation dst = { .texture = target, .x = (u32)f->px0, .y = (u32)f->py0 };
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, (u32)f->pw, (u32)f->ph, 1, false);
        SDL_EndGPUCopyPass(cp);
        S.passes++;
    }
out:
    hlr_gpu_frame_unref(f);
    free(ps->d);
    free(ps);
}
