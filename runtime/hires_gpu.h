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
typedef struct { unsigned long long submits, passes, draws, prims, edge_tris, vram_up_texels, tcopies, readbacks, tris, persp_tris; } HiresGpuStats;
void hires_gpu_get_stats(HiresGpuStats *st);

#endif
