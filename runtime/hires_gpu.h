/* SDL_GPU high-resolution backend: Metal (MSL), Vulkan (SPIR-V) or Direct3D 12 (DXIL); see
 * hires_gpu.c, AGENTS.md "GPU renderer". */
#ifndef HIRES_GPU_H
#define HIRES_GPU_H

#include "hires.h"

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;

/* Graphics APIs (SDL_GPU drivers) the backend has shaders for. */
enum { HGPU_METAL = 0, HGPU_VULKAN, HGPU_D3D12, HGPU_NAPI };
const char *hires_gpu_api_name(int api);    /* "metal", "vulkan", "d3d12" (settings, menu actions) */
const char *hires_gpu_api_title(int api);   /* "Metal", "Vulkan", "Direct3D 12" (UI) */
int hires_gpu_api_from_name(const char *s); /* the names above or SDL's ("direct3d12"); -1 */
/* This platform's APIs in order of preference (the default is the first that works):
 * macOS Metal, Vulkan; Windows Direct3D 12, Vulkan; Linux Vulkan. Returns the count. */
int hires_gpu_api_order(int out[HGPU_NAPI]);
bool hires_gpu_api_offered(int api);        /* in this platform's list */
/* Whether SDL can prepare the driver here (library present, a device with the shader format);
 * cached, the reason logged once. Does not create a device. */
bool hires_gpu_api_probe(int api);
/* Remember that a device of this API could not be used (probe answers false from now on). */
void hires_gpu_api_mark_failed(int api);

/* --gpu-driver: NULL/"auto" (the order above) or one API's name. False for unknown names or
 * APIs this platform does not offer. Call before hires_gpu_init(NULL). */
bool hires_gpu_set_driver(const char *name);
int hires_gpu_driver(void);                 /* -1 = auto */
/* A device of this API that takes its shader format, for the windowed GPU renderer
 * (SDL_CreateGPURenderer). NULL if there is none. */
SDL_GPUDevice *hires_gpu_create_device(int api);
/* Create the backend's pipelines on `dev` (the windowed GPU renderer's device), or on a
 * device of its own when dev is NULL (headless: the --gpu-driver API, else the first of the
 * platform's that works). False if no device takes our shaders or a B5G5R5A1 / RGBA8 render
 * target is unavailable; nothing stays allocated then. */
bool hires_gpu_init(SDL_GPUDevice *dev);
/* Release everything the backend created on its device (and the device, if it is its own),
 * between frames and while the hi-res layer is not drawing with this backend. hires_gpu_init
 * may then be called again, on another device / API (View > Renderer). */
void hires_gpu_shutdown(void);
int hires_gpu_api(void);                    /* API of the initialised device, -1 if none */
const char *hires_gpu_device_name(void);    /* its adapter's name ("" if unknown / none) */
/* The device is a software rasterizer (WARP / Basic Render Driver, llvmpipe, SwiftShader):
 * the windowed default skips it, since the Software renderer is faster there. */
bool hires_gpu_software_adapter(void);
const HiresBackend *hires_gpu_backend(void);
/* Convert the view (from hires_get_view on this backend) into an RGBA8888 texture of
 * v->w x v->h (R in the low byte, = SDL_PIXELFORMAT_ABGR8888) for presentation, submitted
 * on the device's queue ahead of the renderer's work. NULL on failure. */
SDL_GPUTexture *hires_gpu_present(const HiresView *v);

/* Counters since start: submitted command buffers, render passes, draw calls,
 * primitives, VRAM-texture uploads (texels), target copies (VRAM->VRAM copy ops). */
typedef struct {
    unsigned long long submits, passes, draws, prims, edge_tris, vram_up_texels, tcopies, readbacks, tris, persp_tris, fast_prims;
    unsigned long long externals;   /* HiresBackend.external passes run */
    /* texture cache (fast precision): lookups, found, first decodes, re-decodes after a VRAM
     * change of the page / CLUT, VRAM writes that left the content unchanged, evictions, misses
     * because every layer was in use this frame, triangles drawn from the cache, most entries
     * live, CPU decode time; verify mode: entries checked, stale entries, GPU layers read back,
     * read-back layers differing from a fresh decode */
    unsigned long long tc_lookups, tc_hits, tc_decodes, tc_redecodes, tc_same, tc_evictions, tc_full, tc_prims,
        tc_max_live, tc_decode_ns, tc_verified, tc_stale, tc_readbacks, tc_readback_bad;
} HiresGpuStats;
void hires_gpu_get_stats(HiresGpuStats *st);
/* Precision of triangles and rects (presentation only): exact (default, bit-identical to the
 * software backend) or fast (hardware rasterization / interpolation, approximate; see "Fast
 * precision" in hires_gpu.c). Takes effect for the next primitive: switch between frames and
 * re-seed the target (hires_reconfigure in main.c). */
void hires_gpu_set_fast(bool on);
bool hires_gpu_get_fast(void);
/* Texture cache of the fast precision (hires_gpu.c "Texture cache"): textured 3D triangles sample
 * a decoded copy of their (page, CLUT, depth, texture window) with mipmaps and anisotropic
 * filtering instead of the raw VRAM. Verify: also re-check every entry against VRAM and read one
 * layer back from the GPU every `every` frames, logging stale ones. No effect in exact precision. */
enum { HGPU_TC_OFF = 0, HGPU_TC_ON, HGPU_TC_VERIFY };
void hires_gpu_set_tex_cache(int mode, int every);
int hires_gpu_get_tex_cache(void);
void hires_gpu_tc_diag_log(void);   /* DESTREGA_TC_DIAG=1 histograms (exit) */
void hires_gpu_tc_check(void);      /* the verify-mode check now (tests; submits pending work first) */

/* ---- for an external pass (HiresBackend.external: the native high-level renderer, runtime/hlr/) ----
 * Everything an external pass needs to draw into the hi-res target itself. Valid while the backend
 * is configured (between frames it can change: read it again at record time). */
typedef struct SDL_GPUSampler SDL_GPUSampler;
typedef struct {
    SDL_GPUDevice *dev;
    int api;                        /* HGPU_METAL / VULKAN / D3D12 */
    unsigned shader_format;         /* SDL_GPUShaderFormat the device takes (MSL, SPIR-V or DXIL) */
    int target_format;              /* SDL_GPUTextureFormat of the target (B5G5R5A1 or R8G8B8A8) */
    bool full_colour;               /* target is RGBA8 (HIRES_FMT_RGBA8) */
    int Sx, Sy;                     /* render scale */
    int rx, ry, rw, rh;             /* covered 1x VRAM region */
    int gx;                         /* guard band columns (1x) each side */
    int ox, oy;                     /* target pixel = vram x * Sx - ox, vram y * Sy - oy */
    int tw, th, aw, ah;             /* used / allocated target size (pixels) */
    SDL_GPUTexture *tc_colour, *tc_stp;   /* texture cache arrays (NULL until the first lookup) */
    SDL_GPUSampler *tc_sampler;     /* trilinear + anisotropic, clamp to edge */
    SDL_GPUSampler *nearest;        /* nearest, clamp to edge */
} HiresGpuInfo;
bool hires_gpu_info(HiresGpuInfo *out);     /* false: no configured target */
/* Texture-cache layer of (texture page, CLUT, depth 0/1/2, texture window as FPrim.tw = and_u |
 * or_u << 8 | and_v << 16 | or_v << 24), decoded and current, at record time (a decode queues its
 * upload before the steps that follow); -1: not cached. *content gets a fingerprint of the decoded
 * texels (colours, transparency and STP bits; independent of where in VRAM they are), the key a
 * texture replacement pack would use. */
int hires_gpu_tc_layer(int depth, unsigned tp_x, unsigned tp_y, unsigned clut_x, unsigned clut_y, unsigned tw,
                       unsigned long long *content);
/* Called by hires_gpu_shutdown before the device goes away (release what was made on it). */
void hires_gpu_on_shutdown(void (*fn)(void));

#endif
