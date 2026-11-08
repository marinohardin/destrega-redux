/* SDL_GPU (Metal) high-resolution backend (see hires_gpu.c, AGENTS.md "GPU renderer"). */
#ifndef HIRES_GPU_H
#define HIRES_GPU_H

#include "hires.h"

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUTexture SDL_GPUTexture;

/* Create the backend's pipelines on `dev` (the windowed GPU renderer's device), or on a
 * device of its own when dev is NULL (headless). False if SDL_GPU / MSL is unavailable. */
bool hires_gpu_init(SDL_GPUDevice *dev);
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
