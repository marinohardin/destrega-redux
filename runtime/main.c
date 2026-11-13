/* Frontend: boot, SDL3 window/audio/input, headless test mode. */
#include "hlr/hlr.h"
#include "hlr/hlr_gpu.h"
#include "recomp.h"
#include <math.h>
#include "mdec.h"
#include "memcard.h"
#include "menu.h"
#include "menu_overlay.h"
#include "splash.h"
#include "hires.h"
#include "widescreen.h"
#include "hires_gpu.h"
#include "overrides/override.h"
#include "savestate.h"
#include "smooth.h"
#include "input.h"
#include "settings.h"
#include "portmap.h"
#include "netplay_ui.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "platform.h"
#include <zlib.h>

void bus_init(void);
#include "disc.h"
#ifdef DESTREGA_GAME_MODULE
#include "launcher/launcher.h"   /* game module: found or built before boot */
#endif

static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static SDL_AudioStream *s_audio;
static bool s_headless, s_fast;
static u64 s_frame, s_max_frames;
static const char *s_dump_dir;
static int s_dump_every;
static u64 s_dump_from;           /* --dump-from F: first frame written by --dump */
static u64 s_next_tick_ns;
static u32 s_rgba[640 * 512];
static FILE *s_wav;
static u32 s_watch[8]; static int s_watch_n;   /* --watch ADDR: log 32-bit value changes per frame */
static u64 s_profile_from = 1;
static int s_profile_on;
static u32 s_wav_bytes;

/* hi-res options (see hires.h / AGENTS.md) */
/* Render resolution (View > Resolution, display.resolution, --scale): s_opt_sx/sy -1 = Auto,
 * 0 = hi-res off, > 0 = fixed. Auto aims for AUTO_SX x AUTO_SY (4x6) whatever the window size,
 * more only if the window needs it and --max-scale allows, never more than --max-scale; the scale
 * governor (smooth.h) lowers and raises it when this computer cannot keep up. s_res_fit is the
 * older "fit to the window" choice (the smallest scale that covers the window's pixels): the
 * headless default, so the --resize tests keep their window-driven scales. */
#define AUTO_SX 4
#define AUTO_SY 6
static int s_opt_sx = -1, s_opt_sy = -1;   /* --scale N|NxM|auto: -1 = auto, 0 = off */
static bool s_res_fit;                     /* Auto = fit to the window (headless default) */
static bool s_res_chosen;                  /* resolution set by --scale auto, settings or the menu */
static int s_max_sx = 4, s_max_sy = 6;     /* auto-scale caps per axis (see AGENTS.md) */
static int s_gov_sx, s_gov_sy;             /* scale governor caps (Auto), 0 = none (smooth.h) */
static int s_win_w = 960, s_win_h = 720;   /* --window WxH (logical points) */
static bool s_win_set;                     /* --window given */
/* --widescreen (see AGENTS.md "Widescreen"): the hi-res target gets a guard band of WS_GUARD
 * 1x columns per side and the stage renderer's horizontal culling bounds are widened by the
 * same amount (analysis/widescreen_cull.txt). Presented 16:9 (or the window aspect, if
 * narrower) while the stage is being drawn, 4:3 otherwise. */
#define WS_GUARD 88
static bool s_widescreen;                  /* active (shadow pass + wide presentation) */
static bool s_ws_req;                      /* requested: --widescreen, frontend_set_widescreen() */
static bool s_ws_used;                     /* was ever on (for the run summary) */
typedef struct { u64 frame; bool on; } WsStep;
static WsStep s_ws_steps[64];              /* --widescreen-at F:0|1,... (headless toggle schedule) */
static int s_ws_step_n;
static u64 s_ws_stage_frame;               /* last frame a widened culling bound was evaluated */
static bool s_ws_stage;                    /* stage drawn within the last few frames */
static int s_pres_w, s_pres_h;             /* window (physical pixels) presentation is laid out for */
static u64 s_fight_from = 3700;            /* frames >= this count as "fight" in the hi-res stats */
static int s_hires_min_fight = -1;         /* --hires-min-fight PCT: fail unless this % of fight frames used hi-res */
static u64 s_hr_used[2], s_hr_fallback[2]; /* [0] all frames, [1] frames >= s_fight_from */
static u64 s_verify_fight;
static double s_f_emu, s_f_present;        /* windowed: host time over fight frames */
static u64 s_f_n;
static int s_hires_threads = -1;           /* --hires-threads N: -1 = auto */
/* per-segment host timing (--segments F,F,...: segment k = frames [bound[k-1], bound[k])) */
static u64 s_seg_bound[16] = { 1200, 1800, 2400, 3000 };
static int s_seg_n = 4;
static struct { u64 frames, wall_ns, emu_ns, present_ns; } s_seg[17];
static u64 s_last_vb_ns;
static int seg_of(u64 frame) {
    int k = 0;
    while (k < s_seg_n && frame >= s_seg_bound[k]) k++;
    return k;
}
static bool s_hires_dither;
/* render quality (AGENTS.md "Render quality"); -1 = default: windowed play gets texture smoothing,
 * full-precision colour and (GPU renderer) 2x SSAA, headless runs keep the PS1-exact output */
static int s_opt_filter = -1, s_opt_full = -1, s_opt_ssaa = -1;
static bool s_ssaa_chosen;                 /* --ssaa or View > Anti-aliasing: no renderer-based default */
static int s_pgxp = 2;              /* --pgxp: 0 off, 1 precise positions only (affine), 2 on */
static bool s_verify;              /* --hires-verify: at scale 1 the target must equal the 1x VRAM */
static u64 s_verify_frames, s_verify_bad_frames;
static const char *s_memcard_path[2];  /* --memcard1/2 */
static bool s_no_memcard;
/* menu state (see "Application menus" below) */
static int s_argc; static char **s_argv;
static char s_pak_path[2048];          /* data pack in use / to create */
static char s_card_dir[2048];          /* folder for default card images, "" or ends with '/' */
static const char *s_disc_path;        /* positional image (dev use), else NULL */
static bool s_data_dir_given;
typedef struct { u64 frame; char name[24]; char arg[2048]; } MenuStep;
static MenuStep s_menu_script[64];     /* --menu-action FRAME:NAME[=ARG] test hook */
static int s_menu_script_n;
static void menu_script_and_poll(void);
static const char *s_ram_dump;     /* --ram-dump FILE: main RAM at the end of a headless run */
static FILE *s_state_hash;         /* --state-hash FILE */
static FILE *s_gameplay;           /* --gameplay-trace FILE */
static u64 s_ws_wide[2];           /* widescreen: frames presented wider than 4:3 (all, fight window) */
static const char *s_ram_snap_dir; /* --ram-snap DIR: raw RAM + scratchpad every --ram-snap-every frames */
static u64 s_ram_snap_every = 100;
static u64 s_ram_snap_from;     /* --ram-snap-from F: no snapshots before frame F */
typedef struct { u64 frame; int w, h; } ResizeStep;
static ResizeStep s_resizes[256];
static int s_resize_n;
static SDL_Texture *s_hitex;
static int s_hitex_w, s_hitex_h;
/* --renderer software|gpu|metal|vulkan|d3d12|compare (see AGENTS.md "GPU renderer"). The GPU
 * backend's graphics API is hires_gpu_api(); --gpu-driver / --renderer API / settings choose it. */
enum { R_SOFTWARE, R_GPU, R_COMPARE };
static int s_renderer = R_SOFTWARE;
static bool s_renderer_given;              /* --renderer on the command line */
static int s_api_pref = -1;                /* settings display.renderer = metal|vulkan|d3d12 (windowed) */
static SDL_GPUDevice *s_win_dev;           /* device of the window's GPU renderer (NULL: plain SDL renderer) */
static bool s_gpu_used;                    /* the GPU backend ran at some point (stats) */
static SDL_GPUTexture *s_gputex_src;       /* GPU texture wrapped by s_gputex */
static SDL_Texture *s_gputex;
static int s_compare_every = 30;           /* --compare-every N */
static double s_compare_max = -1;          /* --compare-max PCT: fail if a fight frame differs more */
static double s_compare_tol = -1;          /* --compare-tol PCT[:STEPS]: fail if a fight frame has more than */
static int s_compare_tol_steps = 4;        /*   PCT% of its pixels off by more than STEPS 15-bit steps */
static const char *s_compare_dump;         /* --compare-dump DIR: software / GPU / difference PNGs */
static int s_compare_dump_max = 4;         /* --compare-dump-max N: fight frames dumped */
static int s_compare_dumped;
static struct { u64 frames, exact, px, diff, big, big2, big4, mask, sum_abs, sum_sq; double worst, worst_tol;
                u64 worst_frame, worst_tol_frame; } s_cmp[2];
/* --gpu-precision exact|fast (settings display.gpu_precision, View > Renderer > Exact / Fast):
 * the GPU backend's triangles and rects bit-exact or with hardware interpolation (hires_gpu.c) */
static bool s_precision_given;
/* --tex-cache off|on|verify[=N] (settings display.tex_cache, View > Texture Filtering > Smooth
 * Distant Textures): the fast precision's texture cache (hires_gpu.c "Texture cache") */
static bool s_tex_cache_given;
/* --hlr off|on|exact|audit, --hlr-parts LIST (settings display.hlr / hlr_parts, View > 3D Scene):
 * the native high-level renderer (runtime/hlr/, docs/HLR_PLAN.md); presentation only */
static bool s_hlr_given, s_hlr_parts_given;
/* --hlr-msaa N, --hlr-near clip|ps1, --hlr-look enhanced|ps1 (settings display.hlr_msaa / hlr_near /
 * hlr_look, View > 3D Scene): how the native stage is drawn (hlr_native.c, hlr_gpu.c) */
static bool s_hlr_msaa_given, s_hlr_near_given, s_hlr_look_given;
/* --hlr-bones game|smooth (settings display.hlr_bones): characters' bone matrices as the game made
 * them or smoothed + predicted (hlr_char.c); presentation only */
static bool s_hlr_bones_given;
#define HLR_WINDOW_DEFAULT HLR_ON    /* windowed default: draws only with a GPU API (HiresBackend.external) */

static void wav_header(FILE *f, u32 bytes) {
    u32 hdr[11] = {0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 44100,
                   44100 * 4, 0x00100004, 0x61746164, bytes};
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 4, 11, f);
    fseek(f, 0, SEEK_END);
}

/* scripted input for headless runs: "frame:mask,frame:mask" (mask = pressed bits) */
typedef struct { u64 frame; u16 mask; } InputStep;
static InputStep s_script[16384];
static int s_script_n;
static InputStep s_script2[16384];   /* --press2: same format, for pad 2 */
static int s_script2_n;

/* ---- display conversion -------------------------------------------------------- */

static void convert_display(int *ow, int *oh) {
    GpuDisplay d;
    gpu_get_display(&d);
    /* the 1x picture of a 15-bit display needs the display copy (gpu.c skip rule): a window falls
     * back to 1x only when the hi-res target cannot show the display, so start it then */
    if (d.enabled && !d.rgb24 && !gpu_display_copy()) {
        rt_log("gpu-1x: presenting a 15-bit display from 1x VRAM: starting the display copy");
        gpu_enable_display_copy();
    }
    u16 *vram = gpu_display_vram();
    int w = d.w > 640 ? 640 : d.w, h = d.h > 512 ? 512 : d.h;
    if (w <= 0 || h <= 0) { w = 320; h = 240; }
    for (int y = 0; y < h; y++) {
        u32 *dst = s_rgba + y * w;
        int vy = (d.y + y) & 511;
        if (!d.enabled) { for (int x = 0; x < w; x++) dst[x] = 0xff000000u; continue; }
        if (d.rgb24) {
            const u8 *row = (const u8 *)(vram + vy * 1024);
            for (int x = 0; x < w; x++) {
                int bx = d.x * 2 + x * 3;
                u8 r = row[bx % 2048], g = row[(bx + 1) % 2048], b = row[(bx + 2) % 2048];
                dst[x] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
        } else {
            for (int x = 0; x < w; x++) {
                u16 p = vram[vy * 1024 + ((d.x + x) & 1023)];
                u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                dst[x] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
        }
    }
    *ow = w; *oh = h;
}

/* ---- PNG writer (for headless screenshots) --------------------------------------- */

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 len) {
    u8 hdr[8] = {len >> 24, len >> 16, len >> 8, len, type[0], type[1], type[2], type[3]};
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    u8 c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(c, 1, 4, f);
}

static void write_png(const char *path, const u32 *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    u8 ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    uLong raw_len = (uLong)(w * 4 + 1) * h;
    u8 *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[y * (w * 4 + 1)] = 0;
        memcpy(raw + y * (w * 4 + 1) + 1, rgba + y * w, w * 4);
    }
    uLong zlen = compressBound(raw_len);
    u8 *z = malloc(zlen);
    compress2(z, &zlen, raw, raw_len, 6);
    png_chunk(f, "IDAT", z, (u32)zlen);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

/* --compare-dump DIR: the software and GPU targets of a compared fight frame and their
 * difference (|a - b| x 4 per channel, on black), as frame_F_software.png / _gpu.png / _diff.png;
 * tools/contact_sheet.py tiles them */
static void compare_dump(void) {
    u32 *a, *b;
    int w, h;
    if (!hires_compare_images(&a, &b, &w, &h)) return;
    char p[600];
    snprintf(p, sizeof p, "%s/frame_%06llu_software.png", s_compare_dump, (unsigned long long)s_frame);
    write_png(p, a, w, h);
    snprintf(p, sizeof p, "%s/frame_%06llu_gpu.png", s_compare_dump, (unsigned long long)s_frame);
    write_png(p, b, w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        u32 o = 0xFF000000u;
        for (int sh = 0; sh < 24; sh += 8) {
            int d = abs((int)((a[i] >> sh) & 255) - (int)((b[i] >> sh) & 255)) * 4;
            o |= (u32)(d > 255 ? 255 : d) << sh;
        }
        a[i] = o;
    }
    snprintf(p, sizeof p, "%s/frame_%06llu_diff.png", s_compare_dump, (unsigned long long)s_frame);
    write_png(p, a, w, h);
    free(a);
    free(b);
    s_compare_dumped++;
}

/* Convert a hi-res view to RGBA8888 (R in the low byte, as ABGR8888 on little endian), SSAA
 * blocks box-averaged: view_w(v) x view_h(v) pixels. */
typedef struct { const HiresView *v; u32 *dst; size_t pitch; } ConvJob;
static void convert_rows(void *c, int y0, int y1) {
    const ConvJob *j = c;
    hires_view_to_rgba(j->v, j->dst, j->pitch, y0, y1);
}
static int view_w(const HiresView *v) { return v->w / (v->ssaa > 1 ? v->ssaa : 1); }
static int view_h(const HiresView *v) { return v->h / (v->ssaa > 1 ? v->ssaa : 1); }
static void convert_view(const HiresView *v, u32 *dst, size_t dst_pitch_px) {
    ConvJob j = { v, dst, dst_pitch_px };
    hires_parallel_rows(convert_rows, &j, view_h(v));
}

static int ws_present_extra(const GpuDisplay *d);
void frontend_screenshot(const char *path) {
    GpuDisplay d;
    HiresView hv;
    gpu_get_display(&d);
    if (hires_read_view_wide(&d, ws_present_extra(&d), &hv)) {
        u32 *buf = malloc((size_t)view_w(&hv) * view_h(&hv) * 4);
        if (buf) {
            convert_view(&hv, buf, (size_t)view_w(&hv));
            write_png(path, buf, view_w(&hv), view_h(&hv));
            free(buf);
            return;
        }
    }
    int w, h;
    convert_display(&w, &h);
    write_png(path, s_rgba, w, h);
}

static void dump_vram(const char *path) {
    static u32 buf[1024 * 512];
    u16 *v = gpu_display_vram();
    for (int i = 0; i < 1024 * 512; i++) {
        u16 p = v[i];
        buf[i] = 0xff000000u | (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) | ((p & 31) << 3);
    }
    write_png(path, buf, 1024, 512);
}

/* ---- input ---------------------------------------------------------------------------------
 * Host input -> pad masks lives in input.c (keyboard, controllers, bindings, press-to-bind). Here:
 * the SDL event loop, the window's own keys (which never reach a pad) and the --press scripts. */

static int s_gpu1x;          /* --gpu-1x: 0 auto, 1 copy, 2 skip (display copy of the 1x picture), 3 draw (no skip rule) */
static bool s_host_input;   /* headless: --input-event given, so host input feeds the pads */
#if defined(__APPLE__) || defined(_WIN32)
static int s_overlay_menu = 0;   /* --overlay-menu: the in-window menu bar (native menus here) */
#else
static int s_overlay_menu = 1;   /* the in-window menu bar is Linux's menus */
#endif
static u64 s_paused_ns;          /* time the game waited in an open in-window menu */
static void overlay_pause(void);
static void overlay_shot(const char *path);
static void window_settings_event(const SDL_Event *e);

/* --press / --press2 masks for the current frame (taps held 6 frames). */
static u16 script_mask(const InputStep *sc, int n) {
    u16 m = 0;
    for (int i = 0; i < n; i++)
        if (sc[i].frame <= s_frame && s_frame < sc[i].frame + 6) m |= sc[i].mask;
    return m;
}

/* Full screen: the mouse pointer hides after 2 s without movement and comes back when the mouse
 * moves (e.g. to the top of the screen for the menu bar on macOS). Windowed it always shows. */
#define CURSOR_IDLE_NS 2000000000ull
static u64 s_mouse_moved_ns;
static void cursor_idle_update(void) {
    if (!s_win) return;
    bool fs = (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) != 0;
    bool hide = fs && SDL_GetTicksNS() - s_mouse_moved_ns > CURSOR_IDLE_NS;
    if (hide == !SDL_CursorVisible()) return;
    if (hide) SDL_HideCursor();
    else SDL_ShowCursor();
}

/* One host event: press-to-bind first, then the in-window menu (menu_overlay.c), then the
 * window's own keys and the pads. Also the route of the overlay's synthetic test events. */
static void handle_event(SDL_Event *ev) {
    SDL_Event e = *ev;
    {
        if (e.type == SDL_EVENT_QUIT) exit(0);
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
            s_mouse_moved_ns = SDL_GetTicksNS();
        if (input_capture_event(&e)) return;   /* press-to-bind (Controls window) */
        if (overlay_menu_event(&e)) return;    /* the in-window menu bar (paused while open) */
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.key == SDLK_ESCAPE && down) {   /* full screen: leave it (macOS habit); windowed: quit */
                if (!frontend_fullscreen()) exit(0);
                if (!e.key.repeat) frontend_menu_action("fullscreen", "off");
                return;
            }
            if (e.key.key == SDLK_TAB) s_fast = down;
            if (e.key.key == SDLK_F9 && down && !e.key.repeat)   /* = View > Widescreen (16:9) */
                frontend_menu_action("widescreen", NULL);
            /* full screen: Alt+Enter, F11, and Ctrl+Cmd+F on macOS (= View > Enter Full Screen).
             * Handled here so it works on every platform; the Enter of Alt+Enter is not START. */
            SDL_Keymod mod = e.key.mod;
            bool fs_key = e.key.key == SDLK_F11 ||
                          (e.key.key == SDLK_RETURN && (mod & SDL_KMOD_ALT)) ||
                          (e.key.key == SDLK_F && (mod & SDL_KMOD_GUI) && (mod & SDL_KMOD_CTRL));
            if (fs_key) {
                if (down && !e.key.repeat) frontend_menu_action("fullscreen", NULL);
                return;
            }
            if (e.key.key == SDLK_F12 && down) {
                char p[64]; snprintf(p, sizeof p, "shot_%06llu.png", (unsigned long long)s_frame);
                frontend_screenshot(p);
            }
        }
        if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s_win, &pw, &ph);   /* the auto scale follows in present_frame */
            rt_log("fullscreen: %s, window %dx%d px", e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                   ? "entered" : "left", pw, ph);
        }
        window_settings_event(&e);
        input_handle_event(&e);
    }
}

static void poll_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);
    cursor_idle_update();
}

/* This frame's pads: host input (input.c) plus the --press scripts. */
static void host_pads(void) {
    u16 m[2];
    input_frame(m);
    m[0] |= script_mask(s_script, s_script_n);
    m[1] |= script_mask(s_script2, s_script2_n);
    g_pad_buttons[0] = (u16)~m[0];
    g_pad_buttons[1] = (u16)~m[1];
    input_log_frame(s_frame, m);
}

static void poll_input(void) {
    poll_events();
    host_pads();
}

/* ---- per-frame hook (called from the VBlank event) --------------------------------- */

/* ---- determinism hash: everything the emulated machine (gameplay) can see ------------ */

static u64 hash_mix(u64 h, const void *p, size_t n) {
    const u64 *w = p;
    for (size_t i = 0; i < n / 8; i++) { h = (h ^ w[i]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}

static void write_state_hash(void) {
    u64 h = 0xcbf29ce484222325ull;
    h = hash_mix(h, g_ram, RAM_SIZE);
    h = hash_mix(h, g_scratch, sizeof g_scratch);
    h = hash_mix(h, gpu_vram(), 1024 * 512 * 2);
    size_t spu_n;
    const u8 *spu = spu_ram_ptr(&spu_n);   /* separate statement: argument order is unspecified */
    h = hash_mix(h, spu, spu_n);
    h = hash_mix(h, g_cpu.r, sizeof g_cpu.r);
    u64 misc[4 + 32 + 2] = { g_cpu.hi, g_cpu.lo, g_cpu.pc, g_cpu.cycles, 0 };
    for (int i = 0; i < 32; i++) misc[4 + i] = g_cpu.cop0[i];
    misc[36] = g_cpu.next_event;
    h = hash_mix(h, misc, sizeof misc);
    h = memcard_hash(h);
    fprintf(s_state_hash, "%llu %016llx\n", (unsigned long long)s_frame, (unsigned long long)h);
}

static void present_frame(void);
extern u32 g_ws_cull_hits;

/* ---- gameplay trace: the fight state a player can observe (see analysis/symbols.txt) ---- */

#define FIGHTER_P1   0x800c9938u   /* fighter structs, 0x1dc bytes each */
#define FIGHTER_P2   0x800c9b14u
#define FIGHTER_SIZE 0x1dcu
static void write_gameplay_trace(void) {
    u32 fh = 0x811c9dc5u;           /* FNV-1a over both fighter structs */
    for (u32 a = FIGHTER_P1; a < FIGHTER_P1 + 2 * FIGHTER_SIZE; a++) fh = (fh ^ mem_r8(a)) * 0x01000193u;
    fprintf(s_gameplay, "%llu", (unsigned long long)s_frame);
    const u32 f[2] = { FIGHTER_P1, FIGHTER_P2 };
    for (int i = 0; i < 2; i++)
        fprintf(s_gameplay, " p%d hp=%d gauge=%d pos=%d,%d,%d", i + 1, (s16)mem_r16(f[i] + 0xc),
                (s16)mem_r16(f[i] + 0xe), (s32)mem_r32(f[i] + 0xa0), (s32)mem_r32(f[i] + 0xa4),
                (s32)mem_r32(f[i] + 0xa8));
    fprintf(s_gameplay, " time=%u wins=%u,%u rng=%08x fighters=%08x\n", mem_r32(0x800e379c),
            mem_r32(0x800e3788), mem_r32(0x800e378c), mem_r32(0x800e7560), fh);
    static int raw = -1;            /* DESTREGA_STRUCT_TRACE=1: also dump both structs (move lab) */
    if (raw < 0) raw = getenv("DESTREGA_STRUCT_TRACE") != NULL;
    if (raw) {
        for (int i = 0; i < 2; i++) {
            fprintf(s_gameplay, "S%d %llu", i + 1, (unsigned long long)s_frame);
            for (u32 o = 0; o < FIGHTER_SIZE; o += 4) fprintf(s_gameplay, " %x", mem_r32(f[i] + o));
            fputc('\n', s_gameplay);
        }
    }
}

/* Widescreen: guard-band columns per side to present for a pw x ph window showing a dw-wide
 * display: the window aspect clamped to [4:3, 4:3 * (dw + 2 * guard) / dw]. 0 when off. */
static int ws_extra_for(int pw, int ph, int dw, int guard) {
    if (!s_widescreen || guard <= 0 || pw <= 0 || ph <= 0 || dw <= 0) return 0;
    double ar = (double)pw / ph;
    double cols = ar * 0.75 * dw;              /* 1x columns that fill the window at 4:3 pixels */
    int e = (int)((cols - dw) / 2 + 0.5);
    return e < 0 ? 0 : (e > guard ? guard : e);
}

/* Columns per side the current frame is presented with (0 = plain 4:3). */
static int ws_present_extra(const GpuDisplay *d) {
    if (!s_widescreen || !s_ws_stage) return 0;
    return ws_extra_for(s_pres_w, s_pres_h, d->w, hires_guard());
}

static bool res_auto(void) { return s_opt_sx < 0 && !s_res_fit; }
/* the resolution choice as settings / menu actions name it: auto, off or SxxSy */
static const char *resolution_name(void) {
    static char b[16];
    if (s_opt_sx < 0) return "auto";
    if (s_opt_sx == 0) return "off";
    snprintf(b, sizeof b, "%dx%d", s_opt_sx, s_opt_sy);
    return b;
}
static bool gov_allowed(void) { return res_auto() && (!s_headless || smooth_fake_load_given()); }
static int ssaa_cap(void);
static int ssaa_default(int r);

/* Pick the render scales for a window of pw x ph physical pixels, from the display mode
 * currently shown (24-bit / disabled displays keep the 512x240 reference). */
static void apply_window_size(int pw, int ph, bool simulated) {
    if (!simulated && s_opt_sx >= 0) { smooth_governor_allow(false, 0, 0); return; }
    GpuDisplay d;
    gpu_get_display(&d);
    int dw = 512, dh = 240;
    if (d.enabled && !d.rgb24 && d.w > 0 && d.h > 0) { dw = d.w; dh = d.h; }
    int sx, sy, cx, cy;
    s_pres_w = pw; s_pres_h = ph;
    if (s_widescreen) {
        /* sized for the widest presentation this window allows, so switching between 4:3
         * screens and 16:9 fights never reallocates the target */
        int e = ws_extra_for(pw, ph, dw, WS_GUARD);
        hires_pick_scale_ar(pw, ph, dw + 2 * e, dh, 4.0 / 3.0 * (dw + 2 * e) / dw,
                            s_max_sx, s_max_sy, &sx, &sy);
    } else
        hires_pick_scale(pw, ph, dw, dh, s_max_sx, s_max_sy, &sx, &sy);
    if (res_auto()) {   /* at least 4x6 (within --max-scale), more if the window needs it */
        const int ax = AUTO_SX < s_max_sx ? AUTO_SX : s_max_sx, ay = AUTO_SY < s_max_sy ? AUTO_SY : s_max_sy;
        if (sx < ax) sx = ax;
        if (sy < ay) sy = ay;
    }
    smooth_governor_allow(gov_allowed(), sx, sy);
    if (s_gov_sx && s_gov_sx < sx) sx = s_gov_sx;
    if (s_gov_sy && s_gov_sy < sy) sy = s_gov_sy;
    /* while the governor holds the scale down, SSAA may not render more than the cap either */
    hires_set_ssaa_cap(ssaa_cap());
    hires_get_scale(&cx, &cy);
    if (sx != cx || sy != cy) {
        rt_log("hires: window %dx%d, display %dx%d -> render scale %dx%d", pw, ph, dw, dh, sx, sy);
        hires_set_scale(sx, sy);
    }
}

/* ---- runtime renderer / graphics API switch (View > Renderer, --menu-action renderer=...) ----
 * Between frames. Backend only (Software <-> GPU on the same API): select the backend and
 * reallocate the hi-res target at the current scale, seeded from the 1x VRAM like a resize
 * (hires_set_scale destroys the old backend's target). Another graphics API: the hi-res target
 * is dropped, the GPU backend shut down, and the window's SDL renderer and its SDL_GPU device are
 * rebuilt on the new API (headless: the backend's own device), then the target is reallocated
 * and seeded the same way. If the new API cannot be started, the previous one is restored (or
 * the Software renderer on whatever SDL renderer works). Nothing emulated is touched. */
static const char *renderer_name(int r) {
    if (r == R_COMPARE) return "compare";
    if (r == R_GPU) return hires_gpu_api_name(hires_gpu_api());
    return "software";
}
const char *frontend_renderer(void) { return renderer_name(s_renderer); }

/* The platform's default API: the first of its list that is available here (-1: none). */
static int default_api(void) {
    int order[HGPU_NAPI];
    const int n = hires_gpu_api_order(order);
    for (int i = 0; i < n; i++)
        if (order[i] == hires_gpu_api() || hires_gpu_api_probe(order[i])) return order[i];
    return -1;
}

int frontend_graphics_apis(const char **names, const char **titles, int max) {
    int order[HGPU_NAPI];
    const int n = hires_gpu_api_order(order);
    int k = 0;
    for (int i = 0; i < n && k < max; i++, k++) {
        names[k] = hires_gpu_api_name(order[i]);
        titles[k] = hires_gpu_api_title(order[i]);
    }
    return k;
}

bool frontend_renderer_available(const char *name) {
    if (!strcmp(name, "software")) return true;
    if (!strcmp(name, "gpu") || !strcmp(name, "compare")) return hires_gpu_api() >= 0 || default_api() >= 0;
    const int a = hires_gpu_api_from_name(name);
    return a >= 0 && hires_gpu_api_offered(a) && (a == hires_gpu_api() || hires_gpu_api_probe(a));
}

static void window_textures_drop(void) {
    if (s_gputex) SDL_DestroyTexture(s_gputex);
    if (s_hitex) SDL_DestroyTexture(s_hitex);
    if (s_tex) SDL_DestroyTexture(s_tex);
    s_gputex = s_hitex = s_tex = NULL;
    s_gputex_src = NULL;
    s_hitex_w = s_hitex_h = 0;
}

static void window_textures_make(void) {
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
}

/* Destroy the window's renderer, the GPU backend's resources and the renderer's device. */
static void window_renderer_drop(void) {
    window_textures_drop();
    splash_release();
    hires_gpu_shutdown();
    if (s_ren) SDL_DestroyRenderer(s_ren);
    s_ren = NULL;
    if (s_win_dev) SDL_DestroyGPUDevice(s_win_dev);   /* an external device: the renderer leaves it */
    s_win_dev = NULL;
}

/* The window's renderer on a new SDL_GPU device of `api`, with the GPU backend initialised on
 * it. auto_pick: refuse a software rasterizer (WARP, llvmpipe, ...): the Software renderer is
 * faster than those, and the API stays selectable by hand. */
static bool window_renderer_on(int api, bool auto_pick) {
    if (!hires_gpu_api_probe(api)) return false;
    SDL_GPUDevice *dev = hires_gpu_create_device(api);
    if (!dev) { hires_gpu_api_mark_failed(api); return false; }
    SDL_Renderer *ren = SDL_CreateGPURenderer(dev, s_win);
    if (!ren) {
        rt_log("hires-gpu: %s: no window renderer: %s", hires_gpu_api_title(api), SDL_GetError());
        SDL_DestroyGPUDevice(dev);
        hires_gpu_api_mark_failed(api);
        return false;
    }
    if (!hires_gpu_init(dev)) {
        SDL_DestroyRenderer(ren);
        SDL_DestroyGPUDevice(dev);
        hires_gpu_api_mark_failed(api);
        return false;
    }
    if (auto_pick && hires_gpu_software_adapter()) {
        rt_log("hires-gpu: %s runs on a software rasterizer here (%s); the Software renderer is used instead "
               "(View > Renderer still offers it)", hires_gpu_api_title(api), hires_gpu_device_name());
        hires_gpu_shutdown();
        SDL_DestroyRenderer(ren);
        SDL_DestroyGPUDevice(dev);
        return false;
    }
    s_ren = ren;
    s_win_dev = dev;
    return true;
}

/* Startup: `first` if >= 0 (strict: nothing else when the user forced it with --gpu-driver /
 * --renderer API), else / then the platform's order. */
static bool window_renderer_any(int first, bool strict) {
    if (first >= 0) {
        if (window_renderer_on(first, false)) return true;
        if (strict) return false;
        rt_log("hires-gpu: %s (from the settings) is not available; trying the default", hires_gpu_api_title(first));
    }
    int order[HGPU_NAPI];
    const int n = hires_gpu_api_order(order);
    for (int i = 0; i < n; i++)
        if (order[i] != first && window_renderer_on(order[i], true)) return true;
    return false;
}

/* Rebuild the GPU backend on `api` (the hi-res layer must not be using it). */
static bool gpu_device_switch(int api) {
    if (s_headless) {
        hires_gpu_shutdown();   /* destroys its own device */
        hires_gpu_set_driver(hires_gpu_api_name(api));
        return hires_gpu_init(NULL);
    }
    window_renderer_drop();
    return window_renderer_on(api, false);
}

static void backend_for(int r) {
    if (r == R_GPU) hires_set_backend(hires_gpu_backend(), NULL);
    else if (r == R_COMPARE) hires_set_backend(NULL, hires_gpu_backend());
    else hires_set_backend(NULL, NULL);
}

/* api: the graphics API for R_GPU / R_COMPARE; -1 = the one in use, else the default */
static void renderer_switch(int r, int api) {
    const int cur = hires_gpu_api();
    if (r != R_SOFTWARE && api < 0) api = cur >= 0 ? cur : default_api();
    if (r != R_SOFTWARE && api < 0) { rt_log("hires: no graphics API available; staying on %s", renderer_name(s_renderer)); return; }
    if (r == s_renderer && (r == R_SOFTWARE || api == cur)) return;
    int sx, sy;
    const bool on = hires_get_scale(&sx, &sy);
    if (r != R_SOFTWARE && api != cur) {
        if (!hires_gpu_api_offered(api) || !hires_gpu_api_probe(api)) {
            rt_log("hires: %s is not %s; staying on %s", hires_gpu_api_title(api),
                   hires_gpu_api_offered(api) ? "available on this computer" : "offered on this platform", renderer_name(s_renderer));
            return;
        }
        const int old_r = s_renderer;
        /* stop drawing with the GPU backend, then rebuild it on the new device */
        hires_set_backend(NULL, NULL);
        if (on) hires_set_scale(0, 0);
        const u64 t0 = SDL_GetTicksNS();
        if (gpu_device_switch(api))
            rt_log("hires: graphics %s -> %s in %.1f ms", cur >= 0 ? hires_gpu_api_title(cur) : "none", hires_gpu_api_title(api),
                   (double)(SDL_GetTicksNS() - t0) / 1e6);
        else {
            rt_log("hires: %s could not be started; back to %s", hires_gpu_api_title(api),
                   cur >= 0 ? hires_gpu_api_title(cur) : "software");
            if (cur >= 0 && gpu_device_switch(cur)) r = old_r;
            else r = R_SOFTWARE;
        }
        if (!s_headless) {
            if (!s_ren) s_ren = SDL_CreateRenderer(s_win, NULL);   /* the window always needs one */
            if (!s_ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); exit(1); }
            window_textures_make();
        }
        if (r != R_SOFTWARE && hires_gpu_api() < 0) r = R_SOFTWARE;
    }
    backend_for(r);
    if (!s_ssaa_chosen && !s_headless) hires_set_ssaa(ssaa_default(r));   /* default follows the renderer */
    if (on && !hires_set_scale(sx, sy)) {
        rt_log("hires: renderer %s cannot allocate %dx%d; back to software", renderer_name(r), sx, sy);
        r = R_SOFTWARE;
        backend_for(r);
        hires_set_scale(sx, sy);
    }
    rt_log("hires: renderer %s at frame %llu", renderer_name(r), (unsigned long long)s_frame);
    s_renderer = r;
    s_gpu_used |= r != R_SOFTWARE;
}

/* ---- runtime 4:3 / 16:9 switch ---- */

void frontend_set_widescreen(bool on) { s_ws_req = on; }
bool frontend_widescreen(void) { return s_ws_req; }

/* ---- full screen (Alt+Enter / F11 / Ctrl+Cmd+F, View > Enter Full Screen,
 * --menu-action fullscreen[=on|off|toggle]) ----
 * SDL_SetWindowFullscreen with the desktop mode (no mode change); on macOS that is a native
 * full-screen Space. SDL keeps the windowed size and position and restores them on exit. The
 * hi-res auto scale follows the new pixel size in present_frame like any resize. Headless runs
 * have no window and only log. */
static bool s_fs_headless;
bool frontend_fullscreen(void) {
    if (!s_win) return s_fs_headless;
    return (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) != 0;
}
static void fullscreen_set(bool on) {
    if (!s_win) {
        s_fs_headless = on;
        rt_log("fullscreen: %s at frame %llu (headless: no window)", on ? "on" : "off",
               (unsigned long long)s_frame);
        return;
    }
    if (on == frontend_fullscreen()) return;
    if (!SDL_SetWindowFullscreen(s_win, on)) rt_log("fullscreen: %s", SDL_GetError());
    else rt_log("fullscreen: %s at frame %llu", on ? "on" : "off", (unsigned long long)s_frame);
}

/* Apply a requested switch; runs at the start of a vblank, i.e. between frames. */
static void ws_apply(void) {
    if (s_ws_req == s_widescreen) return;
    s_widescreen = s_ws_req;
    s_ws_used |= s_widescreen;
    widescreen_enable(s_widescreen);
    hlr_set_guard(s_widescreen ? WS_GUARD : 0);
    s_ws_stage = false;
    s_ws_stage_frame = 0;
    rt_log("widescreen: %s at frame %llu", s_widescreen ? "on" : "off", (unsigned long long)s_frame);
    int cx, cy;
    if (!s_widescreen || !hires_get_scale(&cx, &cy)) return;
    /* first switch-on: allocate the guard band (kept from then on), re-pick an auto scale */
    hires_set_guard(WS_GUARD);
    if (s_opt_sx < 0 && s_pres_w > 0) apply_window_size(s_pres_w, s_pres_h, true);
    if (hires_get_scale(&cx, &cy) && hires_guard() != WS_GUARD) hires_set_scale(cx, cy);
}

static u32 g_audio_underruns;   /* host-side underrun pads (statistics only, never read by emulation) */
extern u64 g_hires_stat_inline_px, g_hires_stat_par_px, g_hires_stat_par_jobs, g_hires_stat_inline_jobs;
static void finish(void) {
    ss_finish();   /* savestate / replay / netplay reports (savestate.c) */
    void widescreen_report(void); widescreen_report();
    hlr_report();
    gpu_1x_report();
    void char_mesh_report(void); char_mesh_report();
    if (g_trace) rt_log("hires-pool: triangles inline %llu (bbox px %llu), parallel %llu (bbox px %llu)", (unsigned long long)g_hires_stat_inline_jobs, (unsigned long long)g_hires_stat_inline_px, (unsigned long long)g_hires_stat_par_jobs, (unsigned long long)g_hires_stat_par_px);
    if (!s_headless) rt_log("audio: %u host-side underrun pads in %llu frames", g_audio_underruns, (unsigned long long)s_frame);
    if (s_state_hash) fclose(s_state_hash);
    if (s_gameplay) fclose(s_gameplay);
    int rc = 0;
    if (ss_failed()) rc = 7;
    if (s_ws_used)
        rt_log("widescreen-frames: %llu presented wider than 4:3 (%llu in fight window of %llu)",
               (unsigned long long)s_ws_wide[0], (unsigned long long)s_ws_wide[1],
               (unsigned long long)(s_hr_used[1] + s_hr_fallback[1]));
    int sx, sy;
    if (hires_get_scale(&sx, &sy) || s_hr_used[0] + s_hr_fallback[0]) {
        u64 ft = s_hr_used[1] + s_hr_fallback[1];
        double pct = ft ? 100.0 * (double)s_hr_used[1] / (double)ft : 0;
        rt_log("hires-frames: all %llu hi-res / %llu 1x-fallback; fight window (frame >= %llu): "
               "%llu hi-res / %llu 1x-fallback (%.1f%% hi-res)",
               (unsigned long long)s_hr_used[0], (unsigned long long)s_hr_fallback[0],
               (unsigned long long)s_fight_from, (unsigned long long)s_hr_used[1],
               (unsigned long long)s_hr_fallback[1], pct);
        if (s_hires_min_fight >= 0 && pct < s_hires_min_fight) {
            rt_log("hires-frames: FAIL: %.1f%% < required %d%%", pct, s_hires_min_fight);
            rc = 4;
        }
    }
    if (s_verify) {
        rt_log("hires-verify: %llu frames checked (%llu in fight window), %llu mismatching",
               (unsigned long long)s_verify_frames, (unsigned long long)s_verify_fight,
               (unsigned long long)s_verify_bad_frames);
        if (s_verify_bad_frames || !s_verify_fight) rc = 3;
    }
    rt_log("idle-skip: %s, %llu loop iterations fast-forwarded", g_idle_skip ? "on" : "off",
           (unsigned long long)g_idle_skipped);
    if (g_pgxp_stat_tris) {
        rt_log("pgxp-check: %llu triangles with precise vertices; %llu with an on-screen vertex > 1.5 px from its integer, "
               "%llu with depth ratio > 2, %llu using a value older than 2 frames, %llu split vertices",
               (unsigned long long)g_pgxp_stat_tris, (unsigned long long)g_pgxp_stat_bad_d,
               (unsigned long long)g_pgxp_stat_bad_w, (unsigned long long)g_pgxp_stat_old,
               (unsigned long long)g_pgxp_stat_split);
        rt_log("pgxp-weld: %llu vertex uses snapped to another precise value of the same word, %llu to the "
               "integer, %llu table-full fallbacks; %llu back-facing character triangles left out",
               (unsigned long long)g_pgxp_stat_welded, (unsigned long long)g_pgxp_stat_weld_int,
               (unsigned long long)g_pgxp_stat_weld_full, (unsigned long long)g_pgxp_stat_backface);
    }
    {
        u64 ft, f2, fr;
        hires_get_filter_stats(&ft, &f2, &fr);
        if (ft || fr)
            rt_log("hires-filter: %s: %llu textured triangles filtered, %llu of them as 2D (xbr rule), %llu textured rects",
                   hires_tex_filter_name(hires_get_tex_filter()), (unsigned long long)ft, (unsigned long long)f2,
                   (unsigned long long)fr);
    }
    if (g_pgxp_stat_vtx) {
        u64 tris, persp;
        hires_get_persp_stats(&tris, &persp);
        if (s_renderer == R_GPU) {
            HiresGpuStats st;
            hires_gpu_get_stats(&st);
            tris = st.tris; persp = st.persp_tris;
        }
        rt_log("pgxp: %llu of %llu polygon vertices precise (%.1f%%), %llu GTE vertices tracked; "
               "%llu of %llu hi-res triangles perspective-textured; misses: %llu stale tag, "
               "%llu equal to a recent GTE result (association lost)",
               (unsigned long long)g_pgxp_stat_hit, (unsigned long long)g_pgxp_stat_vtx,
               100.0 * (double)g_pgxp_stat_hit / (double)g_pgxp_stat_vtx,
               (unsigned long long)g_pgxp_stat_new, (unsigned long long)persp,
               (unsigned long long)tris, (unsigned long long)g_pgxp_stat_miss_stale,
               (unsigned long long)g_pgxp_stat_miss_gte);
    }
    {
        HiresUvStats uv;
        hires_get_uv_stats(&uv);
        if (uv.samples || hires_get_scale(&sx, &sy))
            rt_log("hires-uv: %llu textured samples, %llu clamped to the 1x-sampled UV range, %llu out of range when fetched "
                   "(1x-sampled range used for %llu of %llu textured triangles)",
                   (unsigned long long)uv.samples, (unsigned long long)uv.clamped,
                   (unsigned long long)uv.escaped, (unsigned long long)uv.tris_1x_range,
                   (unsigned long long)uv.tris);
        if (uv.escaped) rc = 5;
    }
    if (s_renderer == R_COMPARE) {
        for (int k = 0; k < 2; k++) {
            if (!s_cmp[k].frames) continue;
            const double px = (double)s_cmp[k].px;
            const double mse = (double)s_cmp[k].sum_sq / (3.0 * px);
            rt_log("hires-compare: %s: %llu frames compared, %llu identical; %llu of %llu pixels differ (%.5f%%), "
                   "%llu by more than 1 step in a channel, %llu in the mask bit only; worst frame %llu: %.4f%%",
                   k ? "fight" : "all", (unsigned long long)s_cmp[k].frames, (unsigned long long)s_cmp[k].exact,
                   (unsigned long long)s_cmp[k].diff, (unsigned long long)s_cmp[k].px,
                   100.0 * (double)s_cmp[k].diff / px, (unsigned long long)s_cmp[k].big,
                   (unsigned long long)s_cmp[k].mask, (unsigned long long)s_cmp[k].worst_frame, s_cmp[k].worst);
            rt_log("hires-compare-steps: %s: (1 step = 8 of 255): > 1 %.4f%%, > 2 %.4f%%, > 4 %.4f%% of pixels; "
                   "mean abs error %.4f of 255 per channel, PSNR %.2f dB; worst frame > %d steps %llu: %.4f%%",
                   k ? "fight" : "all", 100.0 * (double)s_cmp[k].big / px, 100.0 * (double)s_cmp[k].big2 / px,
                   100.0 * (double)s_cmp[k].big4 / px, (double)s_cmp[k].sum_abs / (3.0 * px),
                   mse > 0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0, s_compare_tol_steps,
                   (unsigned long long)s_cmp[k].worst_tol_frame, s_cmp[k].worst_tol);
        }
        if (s_compare_max >= 0 && (!s_cmp[1].frames || s_cmp[1].worst > s_compare_max)) {
            rt_log("hires-compare: FAIL: worst fight frame %.4f%% > %.4f%% (or no fight frames compared)",
                   s_cmp[1].worst, s_compare_max);
            rc = 6;
        }
        if (s_compare_tol >= 0 && (!s_cmp[1].frames || s_cmp[1].worst_tol > s_compare_tol)) {
            rt_log("hires-compare: FAIL: worst fight frame has %.4f%% of its pixels off by more than %d steps > %.4f%% "
                   "(or no fight frames compared)", s_cmp[1].worst_tol, s_compare_tol_steps, s_compare_tol);
            rc = 6;
        }
    }
    if (s_gpu_used) {
        HiresGpuStats st;
        hires_gpu_get_stats(&st);
        double n = st.submits ? (double)st.submits : 1;
        rt_log("hires-gpu: %llu submits, %.1f render passes / %.1f draws / %.0f primitives per submit "
               "(%llu triangles coverage-tested in the shader), "
               "%llu VRAM texels uploaded, %llu VRAM copies, %llu readbacks; %llu primitives in fast precision",
               (unsigned long long)st.submits, st.passes / n, st.draws / n, st.prims / n,
               (unsigned long long)st.edge_tris,
               (unsigned long long)st.vram_up_texels, (unsigned long long)st.tcopies, (unsigned long long)st.readbacks,
               (unsigned long long)st.fast_prims);
        if (st.tc_lookups) {
            rt_log("hires-texcache: %llu lookups, %llu hits, %llu decodes (%.1f ms), %llu re-decodes after a VRAM change, "
                   "%llu writes that left the texels unchanged, %llu evictions, %llu not cached (full), %llu triangles from "
                   "the cache, at most %llu entries",
                   (unsigned long long)st.tc_lookups, (unsigned long long)st.tc_hits, (unsigned long long)st.tc_decodes,
                   st.tc_decode_ns / 1e6, (unsigned long long)st.tc_redecodes, (unsigned long long)st.tc_same,
                   (unsigned long long)st.tc_evictions, (unsigned long long)st.tc_full, (unsigned long long)st.tc_prims,
                   (unsigned long long)st.tc_max_live);
            if (hires_gpu_get_tex_cache() == HGPU_TC_VERIFY)
                rt_log("hires-texcache-verify: %llu entry checks, %llu STALE; %llu layers read back, %llu differing",
                       (unsigned long long)st.tc_verified, (unsigned long long)st.tc_stale,
                       (unsigned long long)st.tc_readbacks, (unsigned long long)st.tc_readback_bad);
            hires_gpu_tc_diag_log();
        }
    }
    for (int k = 0; k <= s_seg_n; k++) {
        if (!s_seg[k].frames) continue;
        u64 lo = k ? s_seg_bound[k - 1] : 0;
        char hi[32];
        if (k < s_seg_n) snprintf(hi, sizeof hi, "%llu", (unsigned long long)s_seg_bound[k]);
        else snprintf(hi, sizeof hi, "end");
        double n = (double)s_seg[k].frames;
        if (s_headless)
            rt_log("segment %llu-%s: %llu frames, %.2f ms/frame (emulation + hi-res)",
                   (unsigned long long)lo, hi, (unsigned long long)s_seg[k].frames, s_seg[k].wall_ns / n / 1e6);
        else
            rt_log("segment %llu-%s: %llu frames, emu %.2f ms + present %.2f ms = %.2f ms/frame (budget 16.7)",
                   (unsigned long long)lo, hi, (unsigned long long)s_seg[k].frames, s_seg[k].emu_ns / n / 1e6,
                   s_seg[k].present_ns / n / 1e6, (s_seg[k].emu_ns + s_seg[k].present_ns) / n / 1e6);
    }
    if (s_f_n)
        rt_log("windowed fight frames (>= %llu): %llu frames, emu %.2f ms, present+convert %.2f ms per frame",
               (unsigned long long)s_fight_from, (unsigned long long)s_f_n,
               s_f_emu / (double)s_f_n / 1e6, s_f_present / (double)s_f_n / 1e6);
    exit(rc);
}

/* Status text in the window title (netplay connection quality); headless: ignored. */
void frontend_set_status(const char *st) {
    if (!s_win) return;
    char t[320];
    snprintf(t, sizeof t, "Destrega (recompiled)%s%s", st && *st ? " - " : "", st ? st : "");
    SDL_SetWindowTitle(s_win, t);
}

/* Exit after frame n (headless and windowed), e.g. at the end of a --replay. */
void frontend_set_max_frames(u64 n) { if (!s_max_frames || n < s_max_frames) s_max_frames = n; }

/* Headless pads: the --press / --press2 scripts only (pad 2 untouched without --press2), or, with
 * --input-event, injected host events through the same path as a window's. */
static void script_pads(void) {
    if (s_host_input && !ss_resimulating()) {
        input_inject(s_frame);
        poll_events();
        host_pads();
        return;
    }
    u16 m[2] = { script_mask(s_script, s_script_n), script_mask(s_script2, s_script2_n) };
    g_pad_buttons[0] = (u16)~m[0];
    if (s_script2_n) g_pad_buttons[1] = (u16)~m[1];
    input_log_frame(s_frame, m);
}

void frontend_vblank(void) {
    s_frame++;
    if (ss_resimulating()) {   /* rollback / save-state re-run: emulation only (savestate.h) */
        static s16 resim_audio[735 * 2];
        spu_render(resim_audio, 735);
        if (s_headless) script_pads();
        return;
    }
    for (int i = 0; i < s_ws_step_n; i++)
        if (s_ws_steps[i].frame == s_frame) s_ws_req = s_ws_steps[i].on;
    ws_apply();
    if (s_widescreen) {
        static u32 last_hits;
        if (g_ws_cull_hits != last_hits) s_ws_stage_frame = s_frame;
        last_hits = g_ws_cull_hits;
        /* fights draw every second vblank; allow a few frames of slack before going 4:3 */
        bool stage = s_ws_stage_frame && s_frame - s_ws_stage_frame <= 6;
        if (stage != s_ws_stage && g_trace) rt_log("widescreen: %s", stage ? "16:9 (stage drawn)" : "4:3");
        s_ws_stage = stage;
    }
    if (s_headless) {
        u64 now = SDL_GetTicksNS();
        if (s_last_vb_ns) {
            int k = seg_of(s_frame);
            s_seg[k].frames++;
            s_seg[k].wall_ns += now - s_last_vb_ns;
        }
        s_last_vb_ns = now;
    }
    static s16 audio[2048 * 2];
    menu_script_and_poll();
    overlay_pause();                       /* the in-window menu is open: the game waits */
    settings_autosave(SDL_GetTicksNS());
    ws_apply();                            /* a View-menu switch takes effect this vblank */
    for (int i = 0; i < s_resize_n; i++)
        if (s_resizes[i].frame == s_frame) {
            rt_log("resize %dx%d", s_resizes[i].w, s_resizes[i].h);
            apply_window_size(s_resizes[i].w, s_resizes[i].h, true);
        }
    if (s_headless) {   /* scale governor test hook (--fake-load); windowed: smooth_present */
        smooth_headless_frame(s_frame);
        if (smooth_scale_cap(&s_gov_sx, &s_gov_sy)) apply_window_size(s_pres_w, s_pres_h, true);
    }
    {   /* which path would present this frame: hi-res target or the 1x fallback */
        int cx, cy;
        if (hires_get_scale(&cx, &cy)) {
            GpuDisplay d;
            HiresView hv;
            gpu_get_display(&d);
            bool used = hires_get_view(&d, &hv);
            int f = s_frame >= s_fight_from;
            (used ? s_hr_used : s_hr_fallback)[0]++;
            if (f) (used ? s_hr_used : s_hr_fallback)[1]++;
            bool wide = used && ws_present_extra(&d) > 0;
            if (wide) { s_ws_wide[0]++; if (f) s_ws_wide[1]++; }
            static bool was_wide;
            if (wide != was_wide && (s_ws_step_n || s_menu_script_n || g_trace))   /* checked by test_widescreen.sh */
                rt_log("widescreen-present: frame %llu %s", (unsigned long long)s_frame, wide ? "wide" : "4:3");
            was_wide = wide;
        }
    }
    {
        int cx, cy;
        if (s_verify && hires_get_scale(&cx, &cy) && hires_verify_possible()) {
        int fx = 0, fy = 0;
        u32 bad = hires_verify_1x(&fx, &fy);
        s_verify_frames++;
        if (s_frame >= s_fight_from) s_verify_fight++;
        if (bad) {
            if (!s_verify_bad_frames)
                rt_log("hires-verify: MISMATCH at frame %llu: %u pixels differ, first at (%d,%d)",
                       (unsigned long long)s_frame, bad, fx, fy);
            s_verify_bad_frames++;
        }
        }
    }
    if (s_renderer == R_COMPARE && s_compare_every > 0 && s_frame % (u64)s_compare_every == 0) {
        HiresCompare c;
        if (hires_compare(&c) && c.pixels) {
            double pct = 100.0 * (double)c.diff / (double)c.pixels;
            const u64 over = s_compare_tol_steps <= 1 ? c.diff_big : s_compare_tol_steps <= 2 ? c.diff_big2 : c.diff_big4;
            double tol = 100.0 * (double)over / (double)c.pixels;
            for (int k = 0; k < 2; k++) {
                if (k && s_frame < s_fight_from) continue;
                s_cmp[k].frames++;
                if (!c.diff) s_cmp[k].exact++;
                s_cmp[k].px += c.pixels; s_cmp[k].diff += c.diff; s_cmp[k].big += c.diff_big; s_cmp[k].mask += c.mask_diff;
                s_cmp[k].big2 += c.diff_big2; s_cmp[k].big4 += c.diff_big4;
                s_cmp[k].sum_abs += c.sum_abs; s_cmp[k].sum_sq += c.sum_sq;
                if (pct > s_cmp[k].worst || !s_cmp[k].worst_frame) { s_cmp[k].worst = pct; s_cmp[k].worst_frame = s_frame; }
                if (tol > s_cmp[k].worst_tol || !s_cmp[k].worst_tol_frame) { s_cmp[k].worst_tol = tol; s_cmp[k].worst_tol_frame = s_frame; }
            }
            if (s_compare_dump && s_frame >= s_fight_from && s_compare_dumped < s_compare_dump_max) compare_dump();
            if (c.diff && g_trace)
                rt_log("hires-compare: frame %llu: %llu pixels differ (%.4f%%), %llu by > 1, first at target (%d,%d)",
                       (unsigned long long)s_frame, (unsigned long long)c.diff, pct,
                       (unsigned long long)c.diff_big, c.first_x, c.first_y);
        }
    }
    if (s_state_hash) write_state_hash();
    if (s_gameplay) write_gameplay_trace();
    if (s_ram_snap_dir && s_frame >= s_ram_snap_from && s_frame % s_ram_snap_every == 0) {
        char p[512];
        snprintf(p, sizeof p, "%s/ram_%06llu.bin", s_ram_snap_dir, (unsigned long long)s_frame);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fwrite(g_scratch, 1, sizeof g_scratch, f); fclose(f); }
    }
    if (g_trace && s_frame % 60 == 0) {
        extern u32 g_mdec_in_dmas, g_cd_dma_words;
        static u32 lm, lc;
        extern u32 g_cd_int1_pushed, g_cd_int1_delivered; u32 cdrom_int1_dropped(void);
        static u32 lp, ld, ldr;
        rt_log("movie: %u MDEC-in DMAs, %u CD sectors via DMA; INT1 read %u delivered %u dropped %u",
               g_mdec_in_dmas - lm, (g_cd_dma_words - lc) / 512, g_cd_int1_pushed - lp,
               g_cd_int1_delivered - ld, cdrom_int1_dropped() - ldr);
        lp = g_cd_int1_pushed; ld = g_cd_int1_delivered; ldr = cdrom_int1_dropped();
        lm = g_mdec_in_dmas; lc = g_cd_dma_words;
    }
    for (int i = 0; i < s_watch_n; i++) {
        static u32 last[8]; static bool init[8];
        u32 v = mem_r32(s_watch[i]);
        if (!init[i] || v != last[i]) rt_log("watch %08x = %08x", s_watch[i], v);
        last[i] = v; init[i] = true;
    }
    if (s_frame % 600 == 0) {
        static u64 t0;
        extern u32 g_flips;
        static u32 last_flips;
        u64 now = SDL_GetTicksNS();
        if (t0 && !s_headless) {
            static u32 last_ur;
            rt_log("frontend: %.1f fps, %u audio underrun pads", 600.0 * 1e9 / (double)(now - t0), g_audio_underruns - last_ur);
            last_ur = g_audio_underruns;
        }
        extern u64 g_irq_cycles;
        static u64 last_irq;
        if (g_trace) rt_log("game: %.1f updates per 60 vblanks, %.1f%% of CPU time in IRQ handlers",
               (g_flips - last_flips) / 10.0, 100.0 * (g_irq_cycles - last_irq) / (600.0 * 564480));
        last_irq = g_irq_cycles;
        last_flips = g_flips;
        t0 = now;
    }
    if (s_frame == s_profile_from && s_profile_on) {
        extern int g_profile; void rt_profile_start(void);
        g_profile = 1; rt_profile_start();
    }
    if (s_headless) {
        spu_render(audio, 735);
        if (s_wav) { fwrite(audio, 4, 735, s_wav); s_wav_bytes += 735 * 4; }
        script_pads();
        if (s_dump_dir && s_dump_every && s_frame >= s_dump_from && s_frame % s_dump_every == 0) {
            char p[512];
            snprintf(p, sizeof p, "%s/frame_%06llu.png", s_dump_dir, (unsigned long long)s_frame);
            frontend_screenshot(p);
        }
        if (s_max_frames && s_frame >= s_max_frames) {
            if (s_dump_dir) {
                char p[512];
                snprintf(p, sizeof p, "%s/vram_final.png", s_dump_dir);
                dump_vram(p);
                snprintf(p, sizeof p, "%s/frame_final.png", s_dump_dir);
                frontend_screenshot(p);
            }
            if (s_ram_dump) { FILE *rf = fopen(s_ram_dump, "wb"); if (rf) { fwrite(g_ram, 1, RAM_SIZE, rf); fclose(rf); } }
            if (s_wav) { wav_header(s_wav, s_wav_bytes); fclose(s_wav); }
            rt_log("headless: reached %llu frames", (unsigned long long)s_frame);
            void rt_dump_stack(void); rt_dump_stack();
            void rt_profile_dump(void); rt_profile_dump();
            finish();
        }
        return;
    }
    static u64 t_emu, t_present, t_sleep, t_prev_end;
    if (t_prev_end) t_prev_end += s_paused_ns;   /* a menu pause is not host work (scale governor) */
    s_paused_ns = 0;
    u64 t_start = SDL_GetTicksNS();
    if (t_prev_end) t_emu += t_start - t_prev_end;
    poll_input();
    present_frame();
    u64 t_after_present = SDL_GetTicksNS();
    t_present += t_after_present - t_start;
    smooth_present(t_after_present, t_prev_end ? t_after_present - t_prev_end : 0, s_frame);
    if (smooth_scale_cap(&s_gov_sx, &s_gov_sy) && s_pres_w > 0) apply_window_size(s_pres_w, s_pres_h, false);
    if (t_prev_end) {
        int k = seg_of(s_frame);
        s_seg[k].frames++;
        s_seg[k].emu_ns += t_start - t_prev_end;
        s_seg[k].present_ns += t_after_present - t_start;
    }
    if (s_frame >= s_fight_from && t_prev_end) {
        s_f_emu += (double)(t_start - t_prev_end);
        s_f_present += (double)(t_after_present - t_start);
        s_f_n++;
    }
    if (s_max_frames && s_frame >= s_max_frames) {
        rt_log("windowed: reached %llu frames", (unsigned long long)s_frame);
        finish();
    }

    /* Audio: the SPU advances exactly 735 samples per vblank, whatever the host does, so emulated
     * state never depends on host timing. A block is dropped when the queue is already full, and
     * an underrun (queue below two blocks) is covered on the HOST side by queueing a short
     * cross-faded repeat of the tail of this block (no SPU render, no emulated state touched). */
    spu_render(audio, 735);
    int queued = 0;
    bool repeat = false;
    if (!s_fast) {
        static bool primed;
        if (!primed) {   /* two blocks of silence of head start: the first frames may be slow */
            static s16 silence[735 * 2];
            SDL_PutAudioStreamData(s_audio, silence, 735 * 4);
            SDL_PutAudioStreamData(s_audio, silence, 735 * 4);
            primed = true;
        }
        queued = SDL_GetAudioStreamQueued(s_audio);
        if (queued < 735 * 4 * 6) SDL_PutAudioStreamData(s_audio, audio, 735 * 4);
        if (queued < 735 * 4 * 2) {
            static s16 pad[367 * 2];
            for (int i = 0; i < 367; i++) {            /* last 367 frames, fading out */
                int k = 735 - 367 + i, w = 367 - i;
                pad[i * 2] = (s16)((audio[k * 2] * w) / 367);
                pad[i * 2 + 1] = (s16)((audio[k * 2 + 1] * w) / 367);
            }
            SDL_PutAudioStreamData(s_audio, pad, 367 * 4);
            g_audio_underruns++;
            repeat = true;
        }
    }
    if (!s_fast) smooth_audio(queued > 0 ? (u32)queued : 0, 735 * 4, repeat);

    /* pacing to 60Hz */
    u64 now = SDL_GetTicksNS();
    const u64 frame_ns = 1000000000ull / 60;
    if (s_frame % 600 == 0) {
        rt_log("host per frame: emu %.2f ms, present+convert %.2f ms, sleep %.2f ms",
               t_emu / 600.0 / 1e6, t_present / 600.0 / 1e6, t_sleep / 600.0 / 1e6);
        t_emu = t_present = t_sleep = 0;
    }
    if (s_fast) { s_next_tick_ns = now; t_prev_end = SDL_GetTicksNS(); return; }
    /* s_next_tick_ns = when this frame should end (+ gradual netplay time sync). More than 4
     * frames behind: start over from now, without sleeping (resetting to now and then adding a
     * frame slept a whole frame on every slow frame, e.g. 16 ms after a 50 ms present). */
    const u64 extra = netplay_pace_extra_ns();
    if (s_next_tick_ns == 0 || now > s_next_tick_ns + frame_ns * 4) s_next_tick_ns = now + extra;
    else s_next_tick_ns += frame_ns + extra;
    if (s_next_tick_ns > now && !menu_wait_until(s_next_tick_ns)) SDL_DelayNS(s_next_tick_ns - now);
    t_prev_end = SDL_GetTicksNS();
    t_sleep += t_prev_end - now;
}

/* ---- presentation ------------------------------------------------------------------- */

/* Letterbox viewport of aspect ar inside a pw x ph window (4:3 = the original path). */
static SDL_FRect letterbox_ar(int pw, int ph, double ar) {
    float w, h;
    if (ar == 4.0 / 3.0) {
        if ((s64)pw * 3 > (s64)ph * 4) { h = (float)ph; w = h * 4.0f / 3.0f; }
        else { w = (float)pw; h = w * 3.0f / 4.0f; }
    } else if ((double)pw > (double)ph * ar) { h = (float)ph; w = (float)(h * ar); }
    else { w = (float)pw; h = (float)(w / ar); }
    return (SDL_FRect){ ((float)pw - w) / 2, ((float)ph - h) / 2, w, h };
}

static void present_frame(void) {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(s_win, &pw, &ph);
    s_pres_w = pw; s_pres_h = ph;
    apply_window_size(pw, ph, false);
    GpuDisplay d;
    HiresView hv;
    gpu_get_display(&d);
    int extra = ws_present_extra(&d);
    SDL_FRect dst = letterbox_ar(pw, ph, 4.0 / 3.0);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);   /* bars black, whatever drew last (import bar) */
    SDL_RenderClear(s_ren);
    bool have = hires_get_view_wide(&d, extra, &hv);
    if (have && extra) dst = letterbox_ar(pw, ph, 4.0 / 3.0 * (d.w + 2 * extra) / d.w);
    if (have && hv.gpu) {
        /* GPU backend: present straight from the GPU (target -> RGBA8 conversion pass), no readback */
        SDL_GPUTexture *t = hires_gpu_present(&hv);
        if (t && (t != s_gputex_src || !s_gputex || s_gputex->w != view_w(&hv) || s_gputex->h != view_h(&hv))) {
            if (s_gputex) SDL_DestroyTexture(s_gputex);
            SDL_PropertiesID pr = SDL_CreateProperties();
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, view_w(&hv));
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, view_h(&hv));
            SDL_SetPointerProperty(pr, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, t);
            s_gputex = SDL_CreateTextureWithProperties(s_ren, pr);
            SDL_DestroyProperties(pr);
            if (!s_gputex) rt_log("hires-gpu: cannot wrap the GPU texture: %s", SDL_GetError());
            else SDL_SetTextureScaleMode(s_gputex, SDL_SCALEMODE_LINEAR);
            s_gputex_src = t;
        }
        if (t && s_gputex) SDL_RenderTexture(s_ren, s_gputex, NULL, &dst);
    } else if (have) {
        /* true hi-res target: downsampled/filtered into the viewport, never an upsampled 1x image */
        const int ow = view_w(&hv), oh = view_h(&hv);   /* after the SSAA box filter */
        /* Upload only if the displayed rows were drawn since the last upload (fights draw at
         * 30 fps but present at 60), or the view/target/texture changed. */
        static const void *up_pixels; static int up_stride, up_w, up_h; static u64 up_epoch; static bool up_ok;
        if (!s_hitex || s_hitex_w != ow || s_hitex_h != oh) {
            if (s_hitex) SDL_DestroyTexture(s_hitex);
            s_hitex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
            if (s_hitex) SDL_SetTextureScaleMode(s_hitex, SDL_SCALEMODE_LINEAR);
            s_hitex_w = ow; s_hitex_h = oh;
            up_ok = false;   /* a new texture (resize, or a new renderer after a graphics API switch) */
        }
        const void *hpx = hv.pixels ? (const void *)hv.pixels : (const void *)hv.px32;
        bool same = up_ok && up_pixels == hpx && up_stride == hv.stride && up_w == hv.w &&
                    up_h == hv.h && up_epoch == hires_epoch();
        if (!same || hires_dirty_rows(d.y, d.y + d.h - 1)) {
            void *px; int pitch;
            up_ok = false;
            if (s_hitex && SDL_LockTexture(s_hitex, NULL, &px, &pitch)) {
                convert_view(&hv, px, (size_t)pitch / 4);
                SDL_UnlockTexture(s_hitex);
                up_pixels = hpx; up_stride = hv.stride; up_w = hv.w; up_h = hv.h;
                up_epoch = hires_epoch(); up_ok = true;
                hires_clear_dirty();
            }
        }
        if (s_hitex) SDL_RenderTexture(s_ren, s_hitex, NULL, &dst);
    } else {
        int w, h;
        convert_display(&w, &h);
        SDL_UpdateTexture(s_tex, &(SDL_Rect){0, 0, w, h}, s_rgba, w * 4);
        SDL_FRect src = {0, 0, (float)w, (float)h};
        SDL_RenderTexture(s_ren, s_tex, &src, &dst);
    }
    SDL_FRect pic43 = letterbox_ar(pw, ph, 4.0 / 3.0);
    splash_draw(s_ren, &pic43, true);
    if (overlay_menu_enabled()) overlay_menu_draw(s_ren, pw, ph);
    SDL_RenderPresent(s_ren);
}

/* ---- the in-window menu (menu_overlay.c): pause, screenshots ------------------------------- */

/* While a menu or form is open the game waits here, between frames (like the Mac's menu
 * tracking): the last picture is presented with the overlay, every input event goes to the
 * overlay, audio is paused. Headless runs never wait: a menu still open after a test step is
 * closed. */
static void overlay_pause(void) {
    if (!overlay_menu_modal()) return;
    if (s_headless || !s_win) {
        rt_log("menu: overlay still open after the test step: closed (headless runs never pause)");
        overlay_menu_close();
        return;
    }
    const u64 t0 = SDL_GetTicksNS();
    if (s_audio) SDL_PauseAudioStreamDevice(s_audio);
    while (overlay_menu_modal()) {
        poll_events();
        if (overlay_menu_hold_expired() || !overlay_menu_modal()) break;
        present_frame();
        SDL_DelayNS(15000000);
    }
    if (s_audio) SDL_ResumeAudioStreamDevice(s_audio);
    s_paused_ns += SDL_GetTicksNS() - t0;
    s_next_tick_ns = 0;   /* pacing starts over */
    rt_log("menu: game resumed after %.1f s", (double)(SDL_GetTicksNS() - t0) / 1e9);
}

/* menu-test shot=PATH: the current picture letterboxed into the window (headless: --window) with
 * the overlay drawn on top by a software renderer. Presentation only. */
static void overlay_shot(const char *path) {
    GpuDisplay d;
    HiresView hv;
    gpu_get_display(&d);
    int extra = ws_present_extra(&d), w = 0, h = 0;
    u32 *px = NULL;
    bool own = false;
    if (hires_read_view_wide(&d, extra, &hv) && (px = malloc((size_t)view_w(&hv) * view_h(&hv) * 4))) {
        w = view_w(&hv); h = view_h(&hv);
        convert_view(&hv, px, (size_t)w);
        own = true;
    } else {
        convert_display(&w, &h);
        px = s_rgba;
        extra = 0;
    }
    int cw = s_pres_w > 0 ? s_pres_w : s_win_w, ch = s_pres_h > 0 ? s_pres_h : s_win_h;
    SDL_Surface *sf = SDL_CreateSurface(cw, ch, SDL_PIXELFORMAT_ABGR8888);
    SDL_Renderer *r = sf ? SDL_CreateSoftwareRenderer(sf) : NULL;
    SDL_Texture *t = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h) : NULL;
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        SDL_UpdateTexture(t, NULL, px, w * 4);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_FRect dst = letterbox_ar(cw, ch, extra ? 4.0 / 3.0 * (d.w + 2 * extra) / d.w : 4.0 / 3.0);
        SDL_RenderTexture(r, t, NULL, &dst);
        SDL_FRect pic43 = letterbox_ar(cw, ch, 4.0 / 3.0);
        splash_draw(r, &pic43, false);
        overlay_menu_draw(r, cw, ch);
        SDL_FlushRenderer(r);
        u32 *out = malloc((size_t)cw * ch * 4);
        if (out) {
            for (int y = 0; y < ch; y++) memcpy(out + (size_t)y * cw, (u8 *)sf->pixels + (size_t)y * sf->pitch, (size_t)cw * 4);
            for (size_t i = 0; i < (size_t)cw * ch; i++) out[i] |= 0xFF000000u;
            write_png(path, out, cw, ch);
            free(out);
        }
    } else rt_log("menu: test: shot %s failed: %s", path, SDL_GetError());
    if (t) SDL_DestroyTexture(t);
    if (r) SDL_DestroyRenderer(r);
    if (sf) SDL_DestroySurface(sf);
    if (own) free(px);
}

/* ---- saved settings (settings.c; keys documented in AGENTS.md "Settings file") ---------------
 * Display options and the window come from settings.ini unless given on the command line (a flag
 * locks its key: used for this run, never written). View-menu changes, full screen and window
 * resizes are stored as they happen; the file is written when something changed. */
static const char *s_settings_arg;   /* --settings FILE */
static bool s_no_settings;           /* --no-settings */
static bool s_scale_given, s_max_scale_given, s_ws_given;
static bool s_fs_start;              /* display.fullscreen = on */
static bool s_renderer_pref_sw;      /* display.renderer = software (GPU device still made: View > Renderer) */

static bool parse_wxh(const char *v, int *w, int *h, int lo, int hi) {
    char *e;
    long a = strtol(v, &e, 10), b = a;
    if (*e == 'x' || *e == 'X') b = strtol(e + 1, &e, 10);
    if (*e || a < lo || a > hi || b < lo || b > hi) return false;
    *w = (int)a; *h = (int)b;
    return true;
}

static void settings_save_at_exit(void) { settings_save(); }

static void settings_apply(const char *dir) {
    char path[2200];
    if (s_settings_arg) snprintf(path, sizeof path, "%s", s_settings_arg);
    else if (!s_headless && !s_no_settings) snprintf(path, sizeof path, "%ssettings.ini", dir);
    else path[0] = 0;   /* headless: hermetic unless --settings */
    settings_set_path(path[0] ? path : NULL);
    if (s_scale_given) { settings_lock("display", "scale"); settings_lock("display", "resolution"); }
    if (s_max_scale_given) settings_lock("display", "max_scale");
    if (s_ws_given) settings_lock("display", "widescreen");
    if (s_renderer_given) settings_lock("display", "renderer");
    if (s_precision_given) settings_lock("display", "gpu_precision");
    if (s_tex_cache_given) settings_lock("display", "tex_cache");
    if (s_hlr_given) settings_lock("display", "hlr");
    if (s_hlr_parts_given) settings_lock("display", "hlr_parts");
    if (s_hlr_msaa_given) settings_lock("display", "hlr_msaa");
    if (s_hlr_near_given) settings_lock("display", "hlr_near");
    if (s_hlr_look_given) settings_lock("display", "hlr_look");
    if (s_hlr_bones_given) settings_lock("display", "hlr_bones");
    if (s_win_set) settings_lock("display", "window");
    if (s_opt_filter >= 0) settings_lock("display", "tex_filter");
    if (s_opt_full >= 0) settings_lock("display", "smooth_colours");
    if (s_ssaa_chosen) settings_lock("display", "ssaa");
    if (path[0] && settings_load() >= 0) rt_log("settings: %s", path);
    const char *v;
    int a, b;
    if (!s_scale_given && (v = settings_get("display", "resolution"))) {   /* View > Resolution */
        if (!strcmp(v, "auto")) { s_opt_sx = s_opt_sy = -1; s_res_chosen = true; }
        else if (parse_wxh(v, &a, &b, 1, 8)) { s_opt_sx = a; s_opt_sy = b; s_res_chosen = true; }
        else rt_log("settings: display.resolution: auto, 4x6, 2x3 or 1x1");
    }
    else if (!s_scale_given && (v = settings_get("display", "scale"))) {   /* older files, hand edits */
        if (!strcmp(v, "auto")) { s_opt_sx = s_opt_sy = -1; s_res_chosen = true; }
        else if (!strcmp(v, "off") || !strcmp(v, "0")) s_opt_sx = s_opt_sy = 0;
        else if (parse_wxh(v, &a, &b, 1, 8)) { s_opt_sx = a; s_opt_sy = b; }
        else rt_log("settings: display.scale: auto, off or N / NxM (1..8)");
    }
    if (!s_max_scale_given && (v = settings_get("display", "max_scale"))) {
        if (parse_wxh(v, &a, &b, 1, 8)) { s_max_sx = a; s_max_sy = b; }
        else rt_log("settings: display.max_scale: N or NxM (1..8)");
    }
    /* windowed default: 16:9 fights (default window 1280x720); headless stays 4:3 (tests) */
    if (!s_ws_given) s_widescreen = settings_get_bool("display", "widescreen", !s_headless);
    if (!s_renderer_given && (v = settings_get("display", "renderer"))) {
        /* software | gpu (the platform default; older files) | metal | vulkan | d3d12 */
        s_renderer_pref_sw = !strcmp(v, "software");
        const int a = hires_gpu_api_from_name(v);
        if (a >= 0 && hires_gpu_api_offered(a)) s_api_pref = a;
        else if (a >= 0) rt_log("settings: display.renderer: %s is not offered on this platform; using the default", v);
        else if (strcmp(v, "software") && strcmp(v, "gpu"))
            rt_log("settings: display.renderer: software, gpu or a graphics API (metal, vulkan, d3d12)");
    }
    if (!s_precision_given && (v = settings_get("display", "gpu_precision"))) {
        if (!strcmp(v, "fast") || !strcmp(v, "exact")) hires_gpu_set_fast(!strcmp(v, "fast"));
        else rt_log("settings: display.gpu_precision: exact or fast");
    }
    if (!s_tex_cache_given && settings_get("display", "tex_cache"))
        hires_gpu_set_tex_cache(settings_get_bool("display", "tex_cache", true) ? HGPU_TC_ON : HGPU_TC_OFF, 0);
    if (!s_hlr_given) {   /* View > 3D Scene: on (Native) or off (Original); headless stays off */
        int m = s_headless ? HLR_OFF : HLR_WINDOW_DEFAULT;
        if ((v = settings_get("display", "hlr"))) {
            if (!strcmp(v, "on") || !strcmp(v, "off")) m = !strcmp(v, "on") ? HLR_ON : HLR_OFF;
            else rt_log("settings: display.hlr: on or off");
        }
        hlr_set_mode(m);
    }
    if (!s_hlr_parts_given && (v = settings_get("display", "hlr_parts"))) {
        unsigned parts;
        if (hlr_parse_parts(v, &parts)) hlr_set_parts(parts);
        else rt_log("settings: display.hlr_parts: all, none or a list of stage,chars,shadows,fx,story");
    }
    if (!s_hlr_msaa_given && (v = settings_get("display", "hlr_msaa"))) {
        const int n = atoi(v);
        if (n == 1 || n == 2 || n == 4 || n == 8) hlr_gpu_set_msaa(n);
        else rt_log("settings: display.hlr_msaa: 1 (off), 2, 4 or 8");
    }
    if (!s_hlr_near_given && (v = settings_get("display", "hlr_near"))) {
        if (!strcmp(v, "clip") || !strcmp(v, "ps1")) hlr_set_near(!strcmp(v, "ps1") ? HLR_NEAR_PS1 : HLR_NEAR_CLIP);
        else rt_log("settings: display.hlr_near: clip or ps1");
    }
    if (!s_hlr_look_given && (v = settings_get("display", "hlr_look"))) {
        if (!strcmp(v, "enhanced") || !strcmp(v, "ps1")) hlr_set_look(!strcmp(v, "ps1") ? HLR_LOOK_PS1 : HLR_LOOK_ENHANCED);
        else rt_log("settings: display.hlr_look: enhanced or ps1");
    }
    if (!s_hlr_bones_given && (v = settings_get("display", "hlr_bones"))) {
        if (!strcmp(v, "game") || !strcmp(v, "smooth")) hlr_set_bones(!strcmp(v, "game") ? HLR_BONES_GAME : HLR_BONES_SMOOTH);
        else rt_log("settings: display.hlr_bones: game or smooth");
    }
    if (!s_win_set && (v = settings_get("display", "window"))) {
        if (parse_wxh(v, &a, &b, 320, 16384)) { s_win_w = a; s_win_h = b; s_win_set = true; }
    }
    if (s_opt_filter < 0 && (v = settings_get("display", "tex_filter"))) {
        s_opt_filter = hires_tex_filter_from_name(v);
        if (s_opt_filter < 0) rt_log("settings: display.tex_filter: nearest, bilinear or xbr");
    }
    if (s_opt_full < 0 && settings_get("display", "smooth_colours"))
        s_opt_full = settings_get_bool("display", "smooth_colours", true) ? 1 : 0;
    if (!s_ssaa_chosen && (v = settings_get("display", "ssaa"))) {   /* only written by an explicit choice */
        int n = atoi(v);
        if (n >= 1 && n <= HIRES_MAX_SSAA) { s_opt_ssaa = n; s_ssaa_chosen = true; }
        else rt_log("settings: display.ssaa: 1 (off) to %d", HIRES_MAX_SSAA);
    }
    s_fs_start = !s_headless && settings_get_bool("display", "fullscreen", false);
    input_set_virtual_only(s_headless);
    input_init();
    atexit(settings_save_at_exit);
}

/* View-menu state into the settings (after a menu action ran). */
static void settings_note_view(const char *name) {
    if (!strcmp(name, "widescreen")) settings_set_bool("display", "widescreen", s_ws_req);
    else if (!strcmp(name, "renderer") && s_renderer != R_COMPARE) settings_set("display", "renderer", renderer_name(s_renderer));
    else if (!strcmp(name, "resolution")) settings_set("display", "resolution", resolution_name());
    else if (!strcmp(name, "gpu-precision")) settings_set("display", "gpu_precision", hires_gpu_get_fast() ? "fast" : "exact");
    else if (!strcmp(name, "tex-cache")) settings_set_bool("display", "tex_cache", hires_gpu_get_tex_cache() != HGPU_TC_OFF);
    else if (!strcmp(name, "hlr") && (hlr_get_mode() == HLR_OFF || hlr_get_mode() == HLR_ON))
        settings_set("display", "hlr", hlr_mode_name(hlr_get_mode()));
    else if (!strcmp(name, "hlr-part")) settings_set("display", "hlr_parts", hlr_parts_name(hlr_get_parts()));
    else if (!strcmp(name, "hlr-msaa")) {
        char n[8];
        snprintf(n, sizeof n, "%d", hlr_gpu_get_msaa());
        settings_set("display", "hlr_msaa", n);
    }
    else if (!strcmp(name, "hlr-near")) settings_set("display", "hlr_near", hlr_get_near() == HLR_NEAR_PS1 ? "ps1" : "clip");
    else if (!strcmp(name, "hlr-look")) settings_set("display", "hlr_look", hlr_get_look() == HLR_LOOK_PS1 ? "ps1" : "enhanced");
    else if (!strcmp(name, "hlr-bones")) settings_set("display", "hlr_bones", hlr_get_bones() == HLR_BONES_GAME ? "game" : "smooth");
    else if (!strcmp(name, "tex-filter")) settings_set("display", "tex_filter", hires_tex_filter_name(hires_get_tex_filter()));
    else if (!strcmp(name, "smooth-colours")) settings_set_bool("display", "smooth_colours", hires_get_colour());
    else if (!strcmp(name, "ssaa") && s_ssaa_chosen) {
        char n[8];
        snprintf(n, sizeof n, "%d", hires_get_ssaa());
        settings_set("display", "ssaa", n);
    }
    else return;
    settings_save();
}

static void window_settings_event(const SDL_Event *e) {
    if (!s_win) return;
    if (e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
        settings_set_bool("display", "fullscreen", e->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
    else if (e->type == SDL_EVENT_WINDOW_RESIZED && !(SDL_GetWindowFlags(s_win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        int w = 0, h = 0;
        SDL_GetWindowSize(s_win, &w, &h);
        char v[32];
        snprintf(v, sizeof v, "%dx%d", w, h);
        if (w >= 320 && h >= 240) settings_set("display", "window", v);
    }
}

/* ---- boot ---------------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: destrega [options] [disc.cue|disc.bin|destrega.pak]\n"
        "  (no path)             run from the imported data pack; first launch asks for your disc\n"
        "  --import FILE         convert your disc image (.cue/.bin) into the data pack\n"
        "  --data-dir DIR        data pack location (default: per-user app data folder)\n"
        "  --headless            no window/audio; run as fast as possible\n"
        "  --frames N            exit after N frames (headless)\n"
        "  --dump DIR            write screenshots to DIR (headless)\n"
        "  --dump-every N        screenshot every N frames\n"
        "  --dump-from F         first frame --dump writes (default 0)\n"
        "  --press F:MASK,...    headless input script (MASK = hex PSX button bits, active high)\n"
        "  --press2 F:MASK,...   same, for pad 2 (VS / team modes)\n"
        "  --wav FILE            record audio output (headless)\n"
        "  --scale N|NxM|auto    internal render scale (horizontal x vertical): 0 = hi-res off, 1..8\n"
        "                        fixed; auto = View > Resolution > Auto: 4x6 (more if the window needs it\n"
        "                        and --max-scale allows), lowered / raised by the scale governor (default\n"
        "                        in a window; headless default: off, --resize fits the simulated window)\n"
        "  --max-scale N|NxM     per-axis cap for the automatic scale (default 4x6)\n"
        "  --no-scale-governor   Auto keeps its scale even when this computer cannot keep up\n"
        "  --fake-load F-G:PCT,..  (testing) the scale governor sees PCT%% of the frame budget as extra\n"
        "                        load on frames F..G (headless: the governor runs only with this)\n"
        "  --window WxH          initial window size in points (default 960x720)\n"
        "  --hires-threads N     hi-res rasterizer worker threads: -1 auto (performance cores - 1), 0 serial\n"
        "  --segments F,F,...    frame boundaries for the per-segment timing report (default 1200,1800,2400,3000)\n"
        "  --fight-from F        first frame counted as 'fight' in the hi-res frame stats (default 3700)\n"
        "  --hires-min-fight P   exit non-zero unless P%% of fight frames were shown from the hi-res target\n"
        "  --hires-dither        keep PS1 dithering in the hi-res target at scale > 1\n"
        "  --tex-filter F        hi-res texture filter: nearest, bilinear, xbr (2D edge-aware, 3D bilinear)\n"
        "                        (default: xbr in a window, nearest headless)\n"
        "  --hires-colour 15|full  hi-res target colour: 15-bit or full precision (8-bit channels)\n"
        "                        (default: full in a window, 15 headless)\n"
        "  --ssaa N              supersampling 1 (off) to 4: render at N x the scale, box-filter at present\n"
        "                        (default: 2 with the GPU renderer in a window, else 1)\n"
        "  --pgxp on|off|affine  sub-pixel GTE vertices + perspective-correct textures in the hi-res\n"
        "                        target (default on; affine = precise positions, PS1 texture mapping)\n"
        "  --resize F:WxH,...    headless: simulate window resizes (physical pixels) on given frames\n"
        "  --ram-dump FILE       headless: write main RAM to FILE at the end\n"
        "  --memcard1 PATH       memory card image for slot 1 (raw 128 KiB .mcd; created if missing)\n"
        "  --memcard2 PATH       memory card image for slot 2 (default: empty slot)\n"
        "  --no-memcard          no memory cards at all (the game cannot save)\n"
        "                        (default: <data dir>/memcard1.mcd; headless runs: no card unless given)\n"
        "  --no-splash           no caption over the KOEI logo movie\n"
        "  --state-hash FILE     write a per-frame hash of all emulated state (determinism testing)\n"
        "  --gameplay-trace FILE per frame: fighters' health/gauge/position, round time, wins, RNG seed\n"
        "  --ram-snap DIR        write raw guest RAM + scratchpad to DIR every --ram-snap-every N frames (100)\n"
        "  --ram-snap-from F     no --ram-snap snapshots before frame F\n"
        "  --hires-verify        at scale 1, check the hi-res target equals the 1x VRAM every frame\n"
        "  --gpu-1x MODE         auto|copy|skip|draw: 1x primitives inside the framebuffer region are not\n"
        "                        drawn into the emulated VRAM (gpu.c); copy keeps the full 1x picture for\n"
        "                        presentation, skip never does, draw turns the rule off (old state hashes)\n"
        "  --renderer R          hi-res renderer: software, gpu (SDL_GPU: Metal or Vulkan), or compare\n"
        "                        (default: gpu in a window when a device exists, else software; headless:\n"
        "                        software; macOS View > Renderer switches at runtime)\n"
        "                        (software shown, gpu fed the same commands and compared to it)\n"
        "  --gpu-driver D        SDL_GPU driver for gpu/compare: auto (Metal on macOS, else Vulkan),\n"
        "                        metal or vulkan (macOS: MoltenVK via the Vulkan loader)\n"
        "  --compare-every N     compare mode: compare the targets every N frames (default 30)\n"
        "  --compare-max PCT     compare mode: exit non-zero if a fight frame differs in more than PCT%% of pixels\n"
        "  --compare-tol PCT[:STEPS]  compare mode: exit non-zero if a fight frame has more than PCT%% of its\n"
        "                        pixels off by more than STEPS (1, 2 or 4; default 4) 15-bit steps (fast precision)\n"
        "  --compare-dump DIR    compare mode: software / GPU / difference PNGs of compared fight frames\n"
        "  --compare-dump-max N  how many fight frames --compare-dump writes (default 4)\n"
        "  --gpu-precision P     GPU renderer: exact (default; bit-identical to software) or fast\n"
        "                        (hardware interpolation, approximate; View > Renderer > Exact / Fast)\n"
        "  --tex-cache M         fast precision: off, on (default) or verify[=N]: 3D textures from a cache of\n"
        "                        decoded pages sampled with mipmaps + anisotropic filtering (View > Texture\n"
        "                        Filtering > Smooth Distant Textures); verify re-checks every entry against\n"
        "                        VRAM every N frames (default 60) and logs stale ones\n"
        "  --widescreen          16:9 fights: wider stage culling + hi-res guard band (menus/FMV stay 4:3;\n"
        "                        needs hi-res; the window aspect picks 4:3..16:9; default window 1280x720;\n"
        "                        default on in a window, off headless;\n"
        "                        View > Widescreen (16:9) or F9 switches at runtime)\n"
        "  --widescreen-at F:0|1,...  headless: switch widescreen off/on at the given frames\n"
        "  --overlay-menu on|off  the in-window menu bar (F10, Alt, right-click; default on Linux,\n"
        "                        off on macOS / Windows, which have native menus)\n"
        "  --menu-action F:NAME[=ARG]  (testing) run a menu action on frame F: load=IMAGE, reveal-game,\n"
        "                        reveal-saves, save-import=FILE, save-export=FILE, save-delete,\n"
        "                        widescreen[=on|off], renderer[=software|gpu|compare],\n"
        "                        resolution[=auto|4x6|2x3|1x1],\n"
        "                        tex-filter[=nearest|bilinear|xbr], smooth-colours[=on|off], ssaa[=1..4],\n"
        "                        gpu-precision[=exact|fast], tex-cache[=on|off]\n"
        "                        (View menu; no argument = toggle / next), netplay-host=PORT[,DELAY],\n"
        "                        netplay-join=HOST:PORT[,DELAY], netplay-disconnect, netplay-copy-address,\n"
        "                        netplay-copy-report (Netplay menu),\n"
        "                        controls, controls-bind=P1:cross=key:Z|pad:east, controls-device=P2:keyboard,\n"
        "                        controls-stick=P1:on|off, controls-deadzone=P1:40, controls-reset=P1|P2|all,\n"
        "                        controls-dump (Controls window)\n"
        "                        (save actions use card slot 1; headless needs --data-dir or --memcard1)\n"
        "  --settings FILE       settings file to read and write (default: settings.ini next to the data\n"
        "                        pack in a window; headless runs use none unless given this)\n"
        "  --no-settings         windowed: do not read or write the settings file\n"
        "  --no-widescreen       4:3 even if the settings file says widescreen\n"
        "  --input-event F:EV,.. (testing) inject host input on frame F: key-down:NAME, key-up:NAME,\n"
        "                        pad-add:N[:ps4|xbox], pad-remove:N, pad-down:N:BUTTON, pad-up:N:BUTTON,\n"
        "                        pad-axis:N:AXIS:VALUE (virtual SDL controllers; see input.h)\n"
        "  --input-log FILE      write 'frame pad1 pad2' (hex, active high) whenever a pad mask changes\n"
        "  --trace               verbose runtime logging (repeat for more)\n"
        "  --no-interp           make any MIPS-interpreter fallback fatal (testing)\n"
        "  --no-idle-skip        run every iteration of idle loops (analysis/idle_loops.txt; A/B checks)\n"
        "  --verify-overrides    run hand-written overrides against the recompiled originals\n");
    ss_usage();
#ifdef DESTREGA_GAME_MODULE
    launcher_usage();
#endif
}

/* ---- first-launch import (bring your own disc) ----------------------------------- */

static char s_pick[2048];
static volatile int s_pick_state;   /* 0 waiting, 1 chosen, -1 cancelled */

static void SDLCALL pick_cb(void *ud, const char *const *files, int filter) {
    (void)ud; (void)filter;
    if (files && files[0]) { snprintf(s_pick, sizeof s_pick, "%s", files[0]); s_pick_state = 1; }
    else s_pick_state = -1;
}

static void draw_progress(void *ud, float frac) {
    (void)ud;
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) exit(0);
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderClear(s_ren);
    SDL_SetRenderDrawColor(s_ren, 90, 20, 20, 255);
    SDL_RenderRect(s_ren, &(SDL_FRect){120, 228, 400, 24});
    SDL_SetRenderDrawColor(s_ren, 200, 40, 40, 255);
    SDL_RenderFillRect(s_ren, &(SDL_FRect){122, 230, 396 * frac, 20});
    SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
    SDL_RenderPresent(s_ren);
}

static void cli_progress(void *ud, float frac) {
    (void)ud;
    fprintf(stderr, "\rimporting: %3d%%", (int)(frac * 100));
    if (frac >= 1) fputc('\n', stderr);
}

/* The one place a disc image becomes a data pack, for first launch and File > Load Game Image:
 * checks the file type, then disc_import() (which validates the disc itself). */
static const char *import_image(const char *path, const char *pak, DiscProgressFn progress) {
    const char *dot = strrchr(path, '.');
    static const char *ok[] = {".cue", ".bin", ".iso"};
    bool good = false;
    for (int i = 0; dot && i < 3; i++) {
        const char *a = dot, *b = ok[i];
        while (*a && *b && (*a | 32) == *b) a++, b++;
        if (!*a && !*b) good = true;
    }
    if (!good) return "Unsupported file type. Choose the disc image of the North American release: a .cue, a .bin, or a raw 2352-byte-sector .iso.";
    return disc_import(path, pak, progress, NULL);
}

/* Windowed first launch: ask for the user's own disc image and import it.
 * Returns true once `pak` exists. */
static bool gui_import(const char *pak) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega",
        "Destrega needs your own copy of the game (North American release, SLUS-00732).\n\n"
        "Select your disc image (.cue or .bin). It is converted once into a private data pack; "
        "afterwards the original image is no longer needed.", s_win);
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image", "cue;bin;iso"}};
    for (;;) {
        s_pick_state = 0;
        SDL_ShowOpenFileDialog(pick_cb, NULL, s_win, filters, 1, NULL, false);
        while (s_pick_state == 0) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) return false;
        }
        if (s_pick_state < 0) return false;
        const char *err = import_image(s_pick, pak, draw_progress);
        if (!err) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega",
                "Import complete. You can now delete or move the original disc image.", s_win);
            return true;
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Destrega", err, s_win);
    }
}

/* ---- Application menus (File menu; native on macOS, see menu.h) ------------------------------
 * Menu items and the --menu-action test hook both go through frontend_menu_action(), which only
 * queues; menu_script_and_poll() runs the queue at the top of each VBlank, i.e. between frames on
 * the main thread. Headless runs never create menus, but run the same handlers (no dialogs: file
 * arguments are required, confirmations are auto-accepted and logged). */

typedef struct { char name[24]; char arg[2048]; } MenuAction;
static MenuAction s_menu_q[8];
static int s_menu_q_n;

void frontend_menu_action(const char *name, const char *arg) {
    if (s_menu_q_n >= 8) return;
    /* one full-screen toggle per frame: on macOS Ctrl+Cmd+F can arrive both as the menu key
     * equivalent and as an SDL key event */
    if (!strcmp(name, "fullscreen") && !arg)
        for (int i = 0; i < s_menu_q_n; i++)
            if (!strcmp(s_menu_q[i].name, "fullscreen") && !s_menu_q[i].arg[0]) return;
    MenuAction *a = &s_menu_q[s_menu_q_n++];
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->arg, sizeof a->arg, "%s", arg ? arg : "");
}

static void menu_alert(const char *msg) {
    rt_log("menu: ERROR: %s", msg);
    if (!s_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Destrega", msg, s_win);
}

static bool menu_confirm(const char *msg, const char *yes) {
    if (s_headless) { rt_log("menu: confirm \"%s\" -> %s (headless: automatic)", msg, yes); return true; }
    const SDL_MessageBoxButtonData b[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes}};
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_WARNING, s_win, "Destrega", msg, 2, b, NULL};
    int hit = 0;
    return SDL_ShowMessageBox(&d, &hit) && hit == 1;
}

/* Blocking native open/save dialog (windowed only). */
static bool menu_pick(bool save, const SDL_DialogFileFilter *f, int nf, const char *def, char *out, size_t n) {
    s_pick_state = 0;
    if (save) SDL_ShowSaveFileDialog(pick_cb, NULL, s_win, f, nf, def);
    else SDL_ShowOpenFileDialog(pick_cb, NULL, s_win, f, nf, def, false);
    while (s_pick_state == 0) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) exit(0);
    }
    if (s_pick_state < 0) return false;
    snprintf(out, n, "%s", s_pick);
    return true;
}

static bool file_exists(const char *p) { SDL_PathInfo i; return SDL_GetPathInfo(p, &i); }

static bool same_file(const char *a, const char *b) { return plat_same_file(a, b); }

static bool copy_file(const char *src, const char *dst) {
    char tmp[2100];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *i = fopen(src, "rb"), *o = i ? fopen(tmp, "wb") : NULL;
    bool ok = i && o;
    char buf[65536];
    size_t k;
    while (ok && (k = fread(buf, 1, sizeof buf, i)) > 0) ok = fwrite(buf, 1, k, o) == k;
    if (i) fclose(i);
    if (o) ok = fclose(o) == 0 && ok;
    if (ok) ok = plat_replace_file(tmp, dst);
    if (!ok) remove(tmp);
    return ok;
}

static void dir_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *sl = (char *)plat_last_sep(out);
    if (sl && sl != out) *sl = 0;
    else if (!sl) snprintf(out, n, ".");
}

static void import_progress(void *ud, float frac) {
    (void)ud;
    if (!s_headless) draw_progress(NULL, frac);
}

/* Re-exec the process: a clean machine, never a half-reset one.
 *   load:    after a new data pack is in place (drops the positional image and every --menu-action)
 *   netplay: a synced fresh boot for a menu-started session (frontend_restart_with): drops earlier
 *            netplay options, appends `extra`, and keeps the --menu-action steps that are still in
 *            the future, re-based to the new boot's frame numbers (so tests can script past it). */
static void restart_exec(bool netplay, const char *const *extra, int nextra) {
    char exe[4096];
    if (!menu_executable_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "%s", s_argv[0]);
    char **av = calloc((size_t)s_argc + (size_t)nextra + 2 * (sizeof s_menu_script / sizeof s_menu_script[0]) + 2, sizeof *av);
    int n = 0;
    av[n++] = exe;
    for (int i = 1; i < s_argc; i++) {
        if (!strcmp(s_argv[i], "--menu-action") && i + 1 < s_argc) { i++; continue; }
        if (netplay) {
            if ((!strcmp(s_argv[i], "--host") || !strcmp(s_argv[i], "--join") || !strcmp(s_argv[i], "--net-delay"))
                && i + 1 < s_argc) { i++; continue; }
            if (!strcmp(s_argv[i], "--net-from-menu")) continue;
        } else if (s_disc_path && s_argv[i] == s_disc_path) continue;   /* run from the new pack instead */
        av[n++] = s_argv[i];
    }
    if (netplay) {
        for (int i = 0; i < s_menu_script_n; i++) {
            const MenuStep *m = &s_menu_script[i];
            if (m->frame <= s_frame) continue;
            char *a = malloc(2100);
            snprintf(a, 2100, "%llu:%s%s%s", (unsigned long long)(m->frame - s_frame), m->name, *m->arg ? "=" : "", m->arg);
            av[n++] = "--menu-action";
            av[n++] = a;
        }
        for (int i = 0; i < nextra; i++) av[n++] = (char *)extra[i];
    }
    rt_log(netplay ? "netplay: restarting for the session (fresh boot on both sides)" : "menu: restarting on the new data pack");
    if (!netplay) portmap_finish();   /* a new game image: hosting ends, delete the router's port mapping */
    memcard_flush();
    if (s_state_hash) fflush(s_state_hash);
    fflush(NULL);
    plat_setenv("DESTREGA_RESTARTED", "1");
    if (!s_headless) { if (s_audio) SDL_DestroyAudioStream(s_audio); SDL_Quit(); }
    plat_exec(exe, av);
    fprintf(stderr, "menu: ERROR: cannot restart (%s)\n", exe);
    exit(1);
}
static void restart_process(void) { restart_exec(false, NULL, 0); }
void frontend_restart_with(const char *const *args, int n) { restart_exec(true, args, n); }

/* Netplay (netplay.c): alerts and keeping the window alive while it waits. */
void frontend_alert(const char *msg) {
    rt_log("netplay: alert: %s", msg);
    /* DESTREGA_TEST_NO_ALERTS=1: windowed test runs log the alert instead of waiting for a click */
    const char *na = getenv("DESTREGA_TEST_NO_ALERTS");
    if (!s_headless && !(na && *na == '1')) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega Netplay", msg, s_win);
}
void frontend_pump_events(void) { if (!s_headless) SDL_PumpEvents(); }
/* Netplay > Copy My Address etc. (netplay_ui.c): the system clipboard; headless runs leave it alone
 * (the caller logs the text). */
bool frontend_copy_text(const char *text) {
    if (s_headless) return false;
    if (!SDL_SetClipboardText(text)) { rt_log("netplay: cannot copy to the clipboard: %s", SDL_GetError()); return false; }
    return true;
}

static void action_load(const char *arg) {
    char path[2048];
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image", "cue;bin;iso"}};
    if (*arg) snprintf(path, sizeof path, "%s", arg);
    else if (s_headless || !menu_pick(false, filters, 1, NULL, path, sizeof path)) {
        if (s_headless) menu_alert("load needs an image path in headless mode (load=FILE)");
        return;
    }
    rt_log("menu: load game image %s", path);
    memcard_flush();
    if (!s_headless) SDL_SetRenderLogicalPresentation(s_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    const char *err = import_image(path, s_pak_path, import_progress);
    if (!s_headless) SDL_SetRenderLogicalPresentation(s_ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    if (err) { rt_log("menu: load REJECTED"); menu_alert(err); return; }
    rt_log("menu: load OK (%s)", s_pak_path);
    restart_process();
}

static void action_reveal(bool cards) {
    char dir[2048];
    if (cards) {
        const char *p = memcard_path(0);
        if (p) dir_of(p, dir, sizeof dir);
        else if (s_card_dir[0]) snprintf(dir, sizeof dir, "%s", s_card_dir);
        else { menu_alert("No save location is known (try --memcard1 or --data-dir)."); return; }
        size_t l = strlen(dir);
        if (l > 1 && plat_is_sep(dir[l - 1])) dir[l - 1] = 0;
    } else dir_of(s_disc_path ? s_disc_path : s_pak_path, dir, sizeof dir);
    SDL_CreateDirectory(dir);
    rt_log("menu: show %s in file manager: %s", cards ? "save games" : "game files", dir);
    if (!s_headless) menu_reveal(dir);
}

/* ---- memory card actions ------------------------------------------------------------------- */

/* The menus expose a single save slot (card slot 1). ARG is an optional FILE. */
static bool card_args(const char *arg, int *slot, const char **file) {
    if (s_no_memcard) { menu_alert("Saving is disabled (--no-memcard)."); return false; }
    *slot = 0;
    *file = arg;
    return true;
}

/* Where slot `slot`'s card image lives: the attached one, --memcardN, or the default beside the pack.
 * Headless runs never fall back to the user's real data folder. */
static bool card_slot_path(int slot, char *out, size_t n) {
    const char *p = memcard_path(slot);
    if (!p) p = s_memcard_path[slot];
    if (p) { snprintf(out, n, "%s", p); return true; }
    if (s_headless && !s_data_dir_given) return false;
    snprintf(out, n, "%smemcard%d.mcd", s_card_dir, slot + 1);
    return true;
}

static void card_stamp(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    plat_localtime(&t, &tm);
    strftime(out, n, "%Y%m%d-%H%M%S", &tm);
}

static void action_card_import(const char *arg) {
    int slot; const char *file;
    if (!card_args(arg, &slot, &file)) return;
    char src[2048], dest[2048];
    static const SDL_DialogFileFilter filters[] = {{"PlayStation memory card image (.mcd)", "mcd;mcr;mc;bin;ps1;srm;vmp"}, {"All files", "*"}};
    if (*file) snprintf(src, sizeof src, "%s", file);
    else if (s_headless || !menu_pick(false, filters, 2, NULL, src, sizeof src)) {
        if (s_headless) menu_alert("save-import needs FILE in headless mode");
        return;
    }
    const char *err = memcard_validate_file(src);
    if (err) { rt_log("menu: save-import REJECTED"); menu_alert(err); return; }
    if (!card_slot_path(slot, dest, sizeof dest)) { menu_alert("No save location (headless needs --data-dir or --memcard1)."); return; }
    if (same_file(src, dest)) { menu_alert("Those are already your current save games."); return; }
    memcard_flush();                                  /* the on-disk card is now current */
    char backup[2200] = "";
    if (file_exists(dest)) {
        char stamp[32], base[2100];
        card_stamp(stamp, sizeof stamp);
        snprintf(base, sizeof base, "%s", dest);
        size_t l = strlen(base);
        if (l > 4 && !strcmp(base + l - 4, ".mcd")) base[l - 4] = 0;
        snprintf(backup, sizeof backup, "%s-backup-%s.mcd", base, stamp);
        char msg[4600];
        snprintf(msg, sizeof msg, "Replace your save games with the ones in this PlayStation memory card image (.mcd)?\n\nYour current save games are kept as a backup:\n%s", backup);
        if (!menu_confirm(msg, "Replace")) return;
        if (!copy_file(dest, backup)) { menu_alert("Could not write the backup of your current save games; nothing was changed."); return; }
    }
    if (!copy_file(src, dest)) { menu_alert("Could not write the save games (is the folder writable?)."); return; }
    err = memcard_attach(slot, dest);                 /* reloads the file and raises the card-changed flag */
    if (err) { menu_alert(err); return; }
    rt_log("menu: save-import OK slot %d <- %s (backup: %s)", slot + 1, src, *backup ? backup : "none");
}

static void action_card_export(const char *arg) {
    int slot; const char *file;
    if (!card_args(arg, &slot, &file)) return;
    if (!memcard_present(slot)) { menu_alert("There are no save games to export."); return; }
    char dst[2048];
    char def[2200];
    snprintf(def, sizeof def, "%sDestrega-saves.mcd", s_card_dir);
    static const SDL_DialogFileFilter filters[] = {{"PlayStation memory card image (.mcd)", "mcd"}};
    if (*file) snprintf(dst, sizeof dst, "%s", file);
    else if (s_headless || !menu_pick(true, filters, 1, def, dst, sizeof dst)) {
        if (s_headless) menu_alert("save-export needs FILE in headless mode");
        return;
    }
    const char *err = memcard_export(slot, dst);
    if (err) { menu_alert(err); return; }
    rt_log("menu: save-export OK slot %d -> %s", slot + 1, dst);
}

static void action_card_delete(const char *arg) {
    int slot; const char *file;
    if (!card_args(arg, &slot, &file)) return;
    if (!memcard_present(slot)) { menu_alert("There are no save games to export."); return; }
    McDirEntry e;
    int cur = 0;
    if (!memcard_dir_next(slot, "BASLUS-00732*", &cur, &e)) { menu_alert("There is no save game to delete."); return; }
    char msg[256];
    snprintf(msg, sizeof msg, "Delete your saved game?\n\nThis cannot be undone. Use Export Save Games first if you want a copy.");
    if (!menu_confirm(msg, "Delete")) return;
    memcard_flush();
    if (!memcard_erase(slot, e.name)) { menu_alert("Could not delete the save."); return; }
    memcard_flush();
    memcard_mark_new(slot);                           /* as if the card were pulled and re-inserted */
    rt_log("menu: save-delete OK slot %d (%s)", slot + 1, e.name);
}

/* During a netplay session both peers must emulate the same machine: actions that change emulated
 * state (a new game image, the memory card) are refused; presentation-only ones stay allowed.
 * Export only reads the card but flushes it first, so it is blocked too (netplay detaches cards). */
bool netplay_active(void);   /* netplay.c */
bool netplay_busy(void);
bool frontend_menu_action_allowed(const char *name) {
    static const char *const blocked[] = { "load", "save-import", "save-export", "save-delete" };
    if (!strncmp(name, "netplay-copy-", 13)) return netplay_ui_allowed(name);
    if (!strcmp(name, "netplay-host") || !strcmp(name, "netplay-join")) return !netplay_busy();
    if (!strcmp(name, "netplay-disconnect")) return netplay_busy();
    if (!netplay_active()) return true;
    for (size_t i = 0; i < sizeof blocked / sizeof *blocked; i++) if (!strcmp(name, blocked[i])) return false;
    return true;
}
bool frontend_netplay_busy(void) { return netplay_busy(); }

/* View-menu render-quality items: reallocate the target (seeded from the 1x VRAM, like a resize).
 * Runs between frames; presentation only. */
/* Anti-aliasing never takes the render target beyond 24 hi-res pixels per 1x pixel (4x6) in a
 * window, whether it is the default or chosen in the View menu / settings: at 4x6, 2x SSAA is
 * 8x12, four times the pixels (measured 49-87 ms per frame on an M-series GPU, a slideshow), and
 * a window big enough for 4x6 gains little from it. Small windows still get it. Headless runs
 * (tests, dumps) take an explicit --ssaa as given. */
static int ssaa_cap(void) {
    /* Auto lowered by the scale governor: the render target may not exceed the lowered scale's
     * pixels either (else 2x SSAA on a lowered 2x3 would render 4x6 again and save nothing) */
    if (res_auto() && s_gov_sx && s_gov_sy) return s_gov_sx * s_gov_sy;
    return s_headless && s_ssaa_chosen ? 0 : 24;
}
/* SSAA when none was chosen: 2x on the GPU renderer in a window while the resolution is Auto (it
 * only applies when the scale is small enough, see ssaa_cap); a fixed resolution renders exactly
 * what it says unless anti-aliasing is chosen in the menu. */
static int ssaa_default(int r) { return !s_headless && r == R_GPU && s_opt_sx < 0 ? 2 : 1; }

static void log_render_quality(const char *when) {
    int rx = 0, ry = 0;
    hires_get_render_scale(&rx, &ry);
    rt_log("hires: texture filter %s, %s colour, ssaa %d (in use %d, render scale %dx%d)%s",
           hires_tex_filter_name(hires_get_tex_filter()), hires_get_colour() ? "full-precision" : "15-bit",
           hires_get_ssaa(), hires_ssaa(), rx, ry, when);
}
static void view_changed(void) {
    if (!hires_reconfigure()) {
        rt_log("hires: cannot allocate the target with these settings; anti-aliasing off");
        hires_set_ssaa(1);
        hires_reconfigure();
    }
    char when[48];
    snprintf(when, sizeof when, " at frame %llu", (unsigned long long)s_frame);
    log_render_quality(when);
}
bool frontend_view_checked(const char *action, bool *checked) {
    const char *eq = strchr(action, '=');
    const size_t n = eq ? (size_t)(eq - action) : strlen(action);
    const char *arg = eq ? eq + 1 : "";
    if (n == 10 && !strncmp(action, "tex-filter", n) && *arg) { *checked = hires_tex_filter_from_name(arg) == hires_get_tex_filter(); return true; }
    if (n == 10 && !strncmp(action, "resolution", n) && *arg) { *checked = !strcmp(arg, resolution_name()); return true; }
    if (n == 14 && !strncmp(action, "smooth-colours", n)) { *checked = hires_get_colour(); return true; }
    if (n == 4 && !strncmp(action, "ssaa", n) && *arg) { *checked = atoi(arg) == hires_get_ssaa(); return true; }
    if (n == 13 && !strncmp(action, "gpu-precision", n) && *arg) { *checked = !strcmp(arg, "fast") == hires_gpu_get_fast(); return true; }
    if (n == 9 && !strncmp(action, "tex-cache", n)) { *checked = hires_gpu_get_tex_cache() != HGPU_TC_OFF; return true; }
    if (n == 3 && !strncmp(action, "hlr", n) && *arg) { *checked = !strcmp(arg, "on") == (hlr_get_mode() != HLR_OFF); return true; }
    if (n == 8 && !strncmp(action, "hlr-msaa", n) && *arg) { *checked = atoi(arg) == hlr_gpu_get_msaa(); return true; }
    if (n == 8 && !strncmp(action, "hlr-near", n) && *arg) { *checked = !strcmp(arg, "ps1") == (hlr_get_near() == HLR_NEAR_PS1); return true; }
    if (n == 8 && !strncmp(action, "hlr-look", n) && *arg) { *checked = !strcmp(arg, "ps1") == (hlr_get_look() == HLR_LOOK_PS1); return true; }
    if (n == 9 && !strncmp(action, "hlr-bones", n)) {   /* none: View > 3D Scene > Smooth Character Motion */
        *checked = *arg ? !strcmp(arg, "game") == (hlr_get_bones() == HLR_BONES_GAME) : hlr_get_bones() == HLR_BONES_SMOOTH;
        return true;
    }
    if (n == 8 && !strncmp(action, "hlr-part", n) && *arg) {   /* hlr-part=stage[:on] */
        char part[16];
        snprintf(part, sizeof part, "%.*s", (int)strcspn(arg, ":"), arg);
        unsigned bit;
        *checked = hlr_parse_parts(part, &bit) && (hlr_get_parts() & bit) == bit && bit;
        return true;
    }
    return false;
}

/* View > 3D Scene is enabled only while a GPU API is the renderer (the native pass needs one) */
bool frontend_hlr_available(void) { return s_renderer == R_GPU; }

/* View > Resolution (--menu-action resolution=auto|4x6|2x3|1x1, none = next): between frames. Auto
 * hands the scale back to apply_window_size and the governor; a fixed choice drops the governor's
 * caps and is never changed by it. A scale change reallocates the target and seeds it from the 1x
 * VRAM, like a resize. Presentation only. */
static const char *const k_resolutions[] = { "auto", "4x6", "2x3", "1x1" };
static void resolution_set(const char *arg) {
    const int n = (int)(sizeof k_resolutions / sizeof *k_resolutions);
    int cur = 0, pick = -1;
    for (int i = 0; i < n; i++) if (!strcmp(resolution_name(), k_resolutions[i])) cur = i;
    if (!*arg) pick = (cur + 1) % n;
    for (int i = 0; i < n && pick < 0; i++) if (!strcmp(arg, k_resolutions[i])) pick = i;
    if (pick < 0) { rt_log("menu: resolution: auto, 4x6, 2x3 or 1x1"); return; }
    int a = -1, b = -1;
    if (pick > 0) parse_wxh(k_resolutions[pick], &a, &b, 1, 8);
    s_opt_sx = a; s_opt_sy = b;
    s_res_fit = false;
    s_res_chosen = true;
    if (!s_ssaa_chosen) hires_set_ssaa(ssaa_default(s_renderer));
    if (a > 0) {
        s_gov_sx = s_gov_sy = 0;
        smooth_governor_allow(false, 0, 0);
        hires_set_ssaa_cap(ssaa_cap());
        if (!hires_set_scale(a, b)) rt_log("hires: cannot allocate scale %dx%d", a, b);
    } else {
        hires_set_ssaa_cap(ssaa_cap());
        apply_window_size(s_pres_w, s_pres_h, true);
    }
    int rx = 0, ry = 0;
    hires_get_render_scale(&rx, &ry);
    rt_log("hires: resolution %s at frame %llu (render scale %dx%d)", resolution_name(),
           (unsigned long long)s_frame, rx, ry);
}

static void menu_run(const char *name, const char *arg) {
    rt_log("menu: action %s%s%s", name, *arg ? " " : "", arg);
    if (!frontend_menu_action_allowed(name)) {
        if (!strncmp(name, "netplay-copy-", 13)) rt_log("menu: REFUSED %s: %s", name, netplay_ui_refusal(name));
        else if (!strncmp(name, "netplay-", 8))
            rt_log("menu: REFUSED %s: %s", name, netplay_busy() ? "a netplay session is already running" : "no netplay session");
        else rt_log("menu: REFUSED %s: not available during a netplay session (it would desync the peers)", name);
        return;
    }
    if (!strcmp(name, "load")) action_load(arg);
    else if (!strcmp(name, "reveal-game")) action_reveal(false);
    else if (!strcmp(name, "reveal-saves")) action_reveal(true);
    else if (!strcmp(name, "save-import")) action_card_import(arg);
    else if (!strcmp(name, "save-export")) action_card_export(arg);
    else if (!strcmp(name, "save-delete")) action_card_delete(arg);
    else if (!strcmp(name, "netplay-host")) netplay_ui_host_action(arg);   /* PORT[,DELAY] (netplay_ui.c) */
    else if (!strcmp(name, "netplay-join")) netplay_ui_join_action(arg);   /* HOST:PORT[,DELAY] */
    else if (!strcmp(name, "netplay-disconnect")) netplay_menu_disconnect();
    else if (!strncmp(name, "netplay-copy-", 13)) netplay_ui_copy(name);   /* -address, -report */
    else if (!strcmp(name, "renderer")) {
        /* View > Renderer; arg software | gpu (the platform default) | metal | vulkan | d3d12 |
         * compare (keeps the API in use); none = toggle software <-> GPU */
        const int a = hires_gpu_api_from_name(arg);
        if (!strcmp(arg, "gpu")) renderer_switch(R_GPU, default_api());
        else if (!strcmp(arg, "software")) renderer_switch(R_SOFTWARE, -1);
        else if (!strcmp(arg, "compare")) renderer_switch(R_COMPARE, -1);
        else if (a >= 0) renderer_switch(R_GPU, a);
        else if (!*arg) renderer_switch(s_renderer == R_SOFTWARE ? R_GPU : R_SOFTWARE, -1);
        else rt_log("menu: renderer: software, gpu, metal, vulkan, d3d12 or compare");
    }
    else if (!strcmp(name, "gpu-precision")) {   /* View > Renderer > Exact / Fast; none = toggle */
        if (*arg && strcmp(arg, "exact") && strcmp(arg, "fast")) rt_log("menu: gpu-precision: exact or fast");
        else {
            hires_gpu_set_fast(*arg ? !strcmp(arg, "fast") : !hires_gpu_get_fast());
            hires_reseed();   /* presentation only: the next frame starts from the 1x VRAM */
            rt_log("hires-gpu: %s precision at frame %llu%s", hires_gpu_get_fast() ? "fast" : "exact",
                   (unsigned long long)s_frame, s_renderer == R_SOFTWARE ? " (used when the GPU renderer runs)" : "");
        }
    }
    else if (!strcmp(name, "hlr")) {   /* View > 3D Scene > Original (off) / Native (on); none = toggle */
        int m = !strcmp(arg, "on") || !strcmp(arg, "1") ? HLR_ON
                : !strcmp(arg, "off") || !strcmp(arg, "0") ? HLR_OFF
                : hlr_parse_mode(arg) >= 0 ? hlr_parse_mode(arg)
                : hlr_get_mode() == HLR_OFF ? HLR_ON : HLR_OFF;
        hlr_set_mode(m);
        rt_log("hlr: %s at frame %llu%s", hlr_mode_name(m), (unsigned long long)s_frame,
               m == HLR_ON && !hlr_draws() ? " (drawn when a GPU renderer runs)" : "");
    }
    else if (!strcmp(name, "hlr-msaa")) {   /* View > 3D Scene > Anti-aliasing: 1|2|4|8, none = off <-> 4x */
        const int n = *arg ? atoi(arg) : hlr_gpu_get_msaa() > 1 ? 1 : 4;
        hlr_gpu_set_msaa(n);
        rt_log("hlr: MSAA %dx requested at frame %llu", hlr_gpu_get_msaa(), (unsigned long long)s_frame);
    }
    else if (!strcmp(name, "hlr-near")) {   /* View > 3D Scene: near polygons clipped / dropped (PS1); none = toggle */
        const int m = !strcmp(arg, "ps1") ? HLR_NEAR_PS1 : !strcmp(arg, "clip") ? HLR_NEAR_CLIP
                      : hlr_get_near() == HLR_NEAR_PS1 ? HLR_NEAR_CLIP : HLR_NEAR_PS1;
        hlr_set_near(m);
        rt_log("hlr: near polygons %s at frame %llu", m == HLR_NEAR_PS1 ? "dropped (ps1)" : "clipped", (unsigned long long)s_frame);
    }
    else if (!strcmp(name, "hlr-look")) {   /* View > 3D Scene: Enhanced / PS1 look; none = toggle */
        const int m = !strcmp(arg, "ps1") ? HLR_LOOK_PS1 : !strcmp(arg, "enhanced") ? HLR_LOOK_ENHANCED
                      : hlr_get_look() == HLR_LOOK_PS1 ? HLR_LOOK_ENHANCED : HLR_LOOK_PS1;
        hlr_set_look(m);
        rt_log("hlr: %s look at frame %llu", m == HLR_LOOK_PS1 ? "ps1" : "enhanced", (unsigned long long)s_frame);
    }
    else if (!strcmp(name, "hlr-bones")) {   /* characters' bones: game | smooth; none = toggle */
        const int m = !strcmp(arg, "game") ? HLR_BONES_GAME : !strcmp(arg, "smooth") ? HLR_BONES_SMOOTH
                      : hlr_get_bones() == HLR_BONES_GAME ? HLR_BONES_SMOOTH : HLR_BONES_GAME;
        hlr_set_bones(m);
        rt_log("hlr: bones %s at frame %llu", m == HLR_BONES_GAME ? "game" : "smooth", (unsigned long long)s_frame);
    }
    else if (!strcmp(name, "hlr-part")) {   /* View > 3D Scene > Stage: PART:on|off, none = toggle */
        char part[16];
        snprintf(part, sizeof part, "%.*s", (int)strcspn(arg, ":"), arg);
        const char *st = strchr(arg, ':');
        unsigned bit;
        if (!hlr_parse_parts(part, &bit) || !bit) rt_log("menu: hlr-part: stage|chars|shadows|fx|story[:on|off]");
        else {
            const bool on = st ? !strcmp(st + 1, "on") || !strcmp(st + 1, "1") : !(hlr_get_parts() & bit);
            hlr_set_parts(on ? hlr_get_parts() | bit : hlr_get_parts() & ~bit);
            rt_log("hlr: parts %s at frame %llu", hlr_parts_name(hlr_get_parts()), (unsigned long long)s_frame);
        }
    }
    else if (!strcmp(name, "tex-cache")) {   /* View > Texture Filtering > Smooth Distant Textures; on/off, none = toggle */
        const bool on = !strcmp(arg, "on") || !strcmp(arg, "1") ? true
                        : !strcmp(arg, "off") || !strcmp(arg, "0") ? false : hires_gpu_get_tex_cache() == HGPU_TC_OFF;
        hires_gpu_set_tex_cache(on ? HGPU_TC_ON : HGPU_TC_OFF, 0);
        rt_log("hires-gpu: texture cache %s at frame %llu%s", on ? "on" : "off", (unsigned long long)s_frame,
               hires_gpu_get_fast() ? "" : " (used in fast precision)");
    }
    else if (!strcmp(name, "fullscreen"))   /* View > Enter/Exit Full Screen; arg on/off, none/toggle */
        fullscreen_set(!strcmp(arg, "on") || !strcmp(arg, "1") ? true
                       : !strcmp(arg, "off") || !strcmp(arg, "0") ? false
                       : !frontend_fullscreen());
    else if (!strcmp(name, "widescreen"))   /* View > Widescreen (16:9); arg on/off, none = toggle */
        frontend_set_widescreen(!strcmp(arg, "on") || !strcmp(arg, "1") ? true
                                : !strcmp(arg, "off") || !strcmp(arg, "0") ? false
                                : !frontend_widescreen());
    else if (!strcmp(name, "resolution")) resolution_set(arg);   /* View > Resolution */
    else if (!strcmp(name, "tex-filter")) {   /* View > Texture Filtering; nearest|bilinear|xbr, none = next */
        int m = *arg ? hires_tex_filter_from_name(arg) : (hires_get_tex_filter() + 1) % 3;
        if (m < 0) rt_log("menu: tex-filter: nearest, bilinear or xbr");
        else { hires_set_tex_filter(m); view_changed(); }
    }
    else if (!strcmp(name, "smooth-colours")) {   /* View > Smooth Colours; on/off, none = toggle */
        hires_set_colour(!strcmp(arg, "on") || !strcmp(arg, "1") ? true
                         : !strcmp(arg, "off") || !strcmp(arg, "0") ? false : !hires_get_colour());
        view_changed();
    }
    else if (!strcmp(name, "ssaa")) {   /* View > Anti-aliasing; 1 (off) .. 4, none = toggle off / 2x */
        int n = *arg ? atoi(arg) : (hires_get_ssaa() > 1 ? 1 : 2);
        if (n < 1 || n > HIRES_MAX_SSAA) rt_log("menu: ssaa: 1 (off) to %d", HIRES_MAX_SSAA);
        else { hires_set_ssaa(n); hires_set_ssaa_cap(ssaa_cap()); s_ssaa_chosen = true; view_changed(); }
    }
    else if (!strcmp(name, "menu-test")) {  /* menu test hook: the in-window menu's (menu_overlay.c),
                                              * headless on every platform; else the native one */
        if (overlay_menu_enabled() || s_headless) overlay_menu_test(arg);
        else menu_test(arg);
    }
    else if (!strcmp(name, "controls") || !strcmp(name, "settings")) {   /* Settings… / View > Controls… */
        if (s_headless) rt_log("menu: controls: no window in headless mode");
        else if (overlay_menu_enabled()) overlay_menu_controls();
        else controls_open();
    }
    else if (!strncmp(name, "controls-", 9)) input_menu_action(name, arg);   /* input.c */
    else rt_log("menu: unknown action %s", name);
    settings_note_view(name);
}

static void menu_script_and_poll(void) {
    /* Start-up full screen waits until the app is up: entered at frame 0 (before the app had
     * finished launching and its window had focus), macOS's full-screen Space showed no menu bar at
     * all, while the same switch a second later keeps it reachable at the top of the screen.
     * At least 10 vblanks and focus, or frame 180 whatever happens. */
    if (s_fs_start && s_win && s_frame >= 10 &&
        ((SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS) || s_frame >= 180)) {
        s_fs_start = false;
        fullscreen_set(true);
    }
    for (int i = 0; i < s_menu_script_n; i++)
        if (s_menu_script[i].frame == s_frame) frontend_menu_action(s_menu_script[i].name, s_menu_script[i].arg);
    while (s_menu_q_n) {
        MenuAction a = s_menu_q[0];
        memmove(s_menu_q, s_menu_q + 1, (size_t)(--s_menu_q_n) * sizeof a);
        menu_run(a.name, a.arg);
    }
}

static void create_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    /* macOS: keep the menu bar reachable (mouse to the top of the screen) in full screen. SDL's
     * default hides it completely when full screen is entered by SDL_SetWindowFullscreen, which
     * is every path here (View menu, Ctrl+Cmd+F, Alt+Enter, F11), leaving only the shortcut. */
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "1");
    s_win = SDL_CreateWindow("Destrega (recompiled)", s_win_w, s_win_h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    s_ren = NULL;
    if (!s_renderer_given) s_renderer = R_GPU;   /* windowed default (falls back below) */
    /* SDL's GPU renderer on a device the hi-res backend draws with. Created whenever the GPU
     * backend may be used; --renderer software forces the plain SDL renderer (View > Renderer
     * can still switch to an API: that rebuilds the renderer). The API: --gpu-driver /
     * --renderer API (only that one), else the settings' choice, else the platform's order
     * (macOS Metal, Vulkan; Windows Direct3D 12, Vulkan; Linux Vulkan), first that works. */
    if (s_renderer != R_SOFTWARE || !s_renderer_given) {
        const int forced = hires_gpu_driver();
        if (!window_renderer_any(forced >= 0 ? forced : s_api_pref, forced >= 0)) {
            rt_log("hires-gpu: GPU renderer unavailable; using the software renderer");
            s_renderer = R_SOFTWARE;
        }
    }
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, NULL);
    if (!s_ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); exit(1); }
    SDL_SetRenderLogicalPresentation(s_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    window_textures_make();
}

int main(int argc, char **argv) {
    const char *disc_path = NULL, *import_path = NULL, *data_dir = NULL;
    s_argc = argc; s_argv = argv;
    int ss_k;
    ss_set_frame_counter(&s_frame);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) s_headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) s_max_frames = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) s_dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) s_dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) s_dump_from = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--trace")) g_trace++;
        else if (!strcmp(argv[i], "--no-interp")) g_no_interp = 1;
        else if (!strcmp(argv[i], "--no-idle-skip")) g_idle_skip = 0;
        else if (!strcmp(argv[i], "--hlr") && i + 1 < argc) {
            const int m = hlr_parse_mode(argv[++i]);
            if (m < 0) { fprintf(stderr, "--hlr: off, on, exact or audit\n"); return 2; }
            hlr_set_mode(m);
            s_hlr_given = true;
        }
        else if (!strcmp(argv[i], "--hlr-msaa") && i + 1 < argc) {
            const int n = atoi(argv[++i]);
            if (n != 1 && n != 2 && n != 4 && n != 8) { fprintf(stderr, "--hlr-msaa: 1 (off), 2, 4 or 8\n"); return 2; }
            hlr_gpu_set_msaa(n);
            s_hlr_msaa_given = true;
        }
        else if (!strcmp(argv[i], "--hlr-near") && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "clip") && strcmp(m, "ps1")) { fprintf(stderr, "--hlr-near: clip or ps1\n"); return 2; }
            hlr_set_near(!strcmp(m, "ps1") ? HLR_NEAR_PS1 : HLR_NEAR_CLIP);
            s_hlr_near_given = true;
        }
        else if (!strcmp(argv[i], "--hlr-bones") && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "game") && strcmp(m, "smooth")) { fprintf(stderr, "--hlr-bones: game or smooth\n"); return 2; }
            hlr_set_bones(!strcmp(m, "game") ? HLR_BONES_GAME : HLR_BONES_SMOOTH);
            s_hlr_bones_given = true;
        }
        else if (!strcmp(argv[i], "--hlr-look") && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "enhanced") && strcmp(m, "ps1")) { fprintf(stderr, "--hlr-look: enhanced or ps1\n"); return 2; }
            hlr_set_look(!strcmp(m, "ps1") ? HLR_LOOK_PS1 : HLR_LOOK_ENHANCED);
            s_hlr_look_given = true;
        }
        else if (!strcmp(argv[i], "--hlr-parts") && i + 1 < argc) {
            unsigned parts;
            if (!hlr_parse_parts(argv[++i], &parts)) { fprintf(stderr, "--hlr-parts: all, none or a list of stage,chars,shadows,fx,story\n"); return 2; }
            hlr_set_parts(parts);
            s_hlr_parts_given = true;
        }
        else if (!strcmp(argv[i], "--verify-overrides")) { g_verify_overrides = 1; atexit(override_report); }
        else if (!strcmp(argv[i], "--profile")) s_profile_on = 1;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && s_watch_n < 8) s_watch[s_watch_n++] = (u32)strtoul(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "--profile-from") && i + 1 < argc) { s_profile_on = 1; s_profile_from = strtoull(argv[++i], 0, 0); }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { s_wav = fopen(argv[++i], "wb"); if (s_wav) wav_header(s_wav, 0); }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && s_script_n < 16384) {
                s_script[s_script_n].frame = strtoull(s, &s, 0);
                if (*s == ':') s++;
                s_script[s_script_n++].mask = (u16)strtoul(s, &s, 16);
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "--press2") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && s_script2_n < 16384) {
                s_script2[s_script2_n].frame = strtoull(s, &s, 0);
                if (*s == ':') s++;
                s_script2[s_script2_n++].mask = (u16)strtoul(s, &s, 16);
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            char *e; s_scale_given = true;
            if (!strcmp(argv[i + 1], "auto")) { s_opt_sx = s_opt_sy = -1; s_res_chosen = true; i++; }
            else {
                s_opt_sx = (int)strtol(argv[++i], &e, 10);
                s_opt_sy = (*e == 'x' || *e == 'X') ? (int)strtol(e + 1, NULL, 10) : s_opt_sx;
            }
        }
        else if (!strcmp(argv[i], "--max-scale") && i + 1 < argc) {
            char *e; s_max_sx = (int)strtol(argv[++i], &e, 10); s_max_scale_given = true;
            s_max_sy = (*e == 'x' || *e == 'X') ? (int)strtol(e + 1, NULL, 10) : s_max_sx;
        }
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            char *e; s_win_w = (int)strtol(argv[++i], &e, 10); s_win_set = true;
            if (*e == 'x' || *e == 'X') s_win_h = (int)strtol(e + 1, NULL, 10);
        }
        else if (!strcmp(argv[i], "--hires-threads") && i + 1 < argc) s_hires_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hires-par-min") && i + 1 < argc) hires_set_parallel_threshold(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--segments") && i + 1 < argc) {
            char *e = argv[++i];
            s_seg_n = 0;
            while (*e && s_seg_n < 16) { s_seg_bound[s_seg_n++] = strtoull(e, &e, 0); if (*e == ',') e++; }
        }
        else if (!strcmp(argv[i], "--fight-from") && i + 1 < argc) s_fight_from = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--hires-min-fight") && i + 1 < argc) s_hires_min_fight = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hires-dither")) s_hires_dither = true;
        else if (!strcmp(argv[i], "--tex-filter") && i + 1 < argc) {
            s_opt_filter = hires_tex_filter_from_name(argv[++i]);
            if (s_opt_filter < 0) { fprintf(stderr, "--tex-filter: nearest, bilinear or xbr\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--hires-colour") && i + 1 < argc) {
            const char *c = argv[++i];
            if (!strcmp(c, "15")) s_opt_full = 0;
            else if (!strcmp(c, "full")) s_opt_full = 1;
            else { fprintf(stderr, "--hires-colour: 15 or full\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--ssaa") && i + 1 < argc) {
            s_opt_ssaa = atoi(argv[++i]);
            if (s_opt_ssaa < 1 || s_opt_ssaa > HIRES_MAX_SSAA) { fprintf(stderr, "--ssaa: 1 (off) to %d\n", HIRES_MAX_SSAA); return 1; }
            s_ssaa_chosen = true;
        }
        else if (!strcmp(argv[i], "--pgxp-diag") && i + 1 < argc) g_pgxp_diag = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pgxp") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "on")) s_pgxp = 2;
            else if (!strcmp(m, "off")) s_pgxp = 0;
            else if (!strcmp(m, "affine")) s_pgxp = 1;
            else { fprintf(stderr, "--pgxp: expected on, off or affine\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--hires-verify")) s_verify = true;
        else if (!strcmp(argv[i], "--widescreen")) s_widescreen = s_ws_given = true;
        else if (!strcmp(argv[i], "--no-widescreen")) { s_widescreen = false; s_ws_given = true; }
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc) s_settings_arg = argv[++i];
        else if (!strcmp(argv[i], "--no-settings")) s_no_settings = true;
        else if (!strcmp(argv[i], "--overlay-menu") && i + 1 < argc) s_overlay_menu = strcmp(argv[++i], "off") ? 1 : 0;
        else if (!strcmp(argv[i], "--input-event") && i + 1 < argc) {
            if (!input_parse_events(argv[++i])) return 1;
        }
        else if (!strcmp(argv[i], "--input-log") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "wb");
            if (!f) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
            input_set_log(f);
        }
        else if (!strcmp(argv[i], "--ram-snap") && i + 1 < argc) s_ram_snap_dir = argv[++i];
        else if (!strcmp(argv[i], "--gameplay-trace") && i + 1 < argc) {
            s_gameplay = fopen(argv[++i], "wb");
            if (!s_gameplay) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--ram-snap-every") && i + 1 < argc) s_ram_snap_every = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--ram-snap-from") && i + 1 < argc) s_ram_snap_from = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            const char *r = argv[++i];
            s_renderer_given = true;
            if (!strcmp(r, "software")) s_renderer = R_SOFTWARE;
            else if (!strcmp(r, "gpu")) s_renderer = R_GPU;
            else if (!strcmp(r, "compare")) s_renderer = R_COMPARE;
            else if (hires_gpu_api_from_name(r) >= 0) {   /* = gpu + --gpu-driver API */
                if (!hires_gpu_set_driver(r)) { fprintf(stderr, "--renderer: %s is not offered on this platform\n", r); return 1; }
                s_renderer = R_GPU;
            }
            else { fprintf(stderr, "--renderer: software, gpu, metal, vulkan, d3d12 or compare\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--gpu-driver") && i + 1 < argc) {
            if (!hires_gpu_set_driver(argv[++i])) {
                int o[HGPU_NAPI], n = hires_gpu_api_order(o);
                fprintf(stderr, "--gpu-driver: auto");
                for (int k = 0; k < n; k++) fprintf(stderr, ", %s", hires_gpu_api_name(o[k]));
                fprintf(stderr, " (on this platform)\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--compare-every") && i + 1 < argc) s_compare_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compare-max") && i + 1 < argc) s_compare_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--compare-tol") && i + 1 < argc) {
            const char *t = argv[++i], *c = strchr(t, ':');
            s_compare_tol = atof(t);
            if (c) s_compare_tol_steps = atoi(c + 1);
            if (s_compare_tol_steps != 1 && s_compare_tol_steps != 2 && s_compare_tol_steps != 4) {
                fprintf(stderr, "--compare-tol: PCT[:STEPS], STEPS 1, 2 or 4\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--compare-dump") && i + 1 < argc) s_compare_dump = argv[++i];
        else if (!strcmp(argv[i], "--compare-dump-max") && i + 1 < argc) s_compare_dump_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-precision") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "exact") && strcmp(v, "fast")) { fprintf(stderr, "--gpu-precision: exact or fast\n"); return 1; }
            hires_gpu_set_fast(!strcmp(v, "fast"));
            s_precision_given = true;
        }
        else if (!strcmp(argv[i], "--tex-cache") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "off")) hires_gpu_set_tex_cache(HGPU_TC_OFF, 0);
            else if (!strcmp(v, "on")) hires_gpu_set_tex_cache(HGPU_TC_ON, 0);
            else if (!strncmp(v, "verify", 6) && (!v[6] || (v[6] == '=' && atoi(v + 7) > 0)))
                hires_gpu_set_tex_cache(HGPU_TC_VERIFY, v[6] ? atoi(v + 7) : 0);
            else { fprintf(stderr, "--tex-cache: off, on or verify[=N]\n"); return 1; }
            s_tex_cache_given = true;
        }
        else if (!strcmp(argv[i], "--state-hash") && i + 1 < argc) {
            s_state_hash = fopen(argv[++i], "wb");
            if (!s_state_hash) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--gpu-1x") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "auto")) s_gpu1x = 0;
            else if (!strcmp(m, "copy")) s_gpu1x = 1;
            else if (!strcmp(m, "skip")) s_gpu1x = 2;
            else if (!strcmp(m, "draw")) s_gpu1x = 3;
            else { fprintf(stderr, "--gpu-1x: expected auto, copy, skip or draw\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--resize") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && s_resize_n < 256) {
                s_resizes[s_resize_n].frame = strtoull(s, &s, 0);
                if (*s == ':') s++;
                s_resizes[s_resize_n].w = (int)strtol(s, &s, 10);
                if (*s == 'x' || *s == 'X') s++;
                s_resizes[s_resize_n++].h = (int)strtol(s, &s, 10);
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "--widescreen-at") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && s_ws_step_n < 64) {
                s_ws_steps[s_ws_step_n].frame = strtoull(s, &s, 0);
                if (*s == ':') s++;
                s_ws_steps[s_ws_step_n++].on = strtol(s, &s, 10) != 0;
                if (*s == ',') s++;
            }
        }
        else if (!strcmp(argv[i], "--ram-dump") && i + 1 < argc) s_ram_dump = argv[++i];
        else if (!strcmp(argv[i], "--memcard1") && i + 1 < argc) s_memcard_path[0] = argv[++i];
        else if (!strcmp(argv[i], "--memcard2") && i + 1 < argc) s_memcard_path[1] = argv[++i];
        else if (!strcmp(argv[i], "--no-memcard")) s_no_memcard = true;
        else if (!strcmp(argv[i], "--no-splash")) splash_set_enabled(false);
        else if (!strcmp(argv[i], "--menu-action") && i + 1 < argc) {
            if (s_menu_script_n >= (int)(sizeof s_menu_script / sizeof s_menu_script[0])) { fprintf(stderr, "--menu-action: at most %d\n", (int)(sizeof s_menu_script / sizeof s_menu_script[0])); return 1; }
            char *s = argv[++i];
            MenuStep *m = &s_menu_script[s_menu_script_n++];
            m->frame = strtoull(s, &s, 0);
            if (*s == ':') s++;
            const char *eq = strchr(s, '=');
            snprintf(m->name, sizeof m->name, "%.*s", (int)(eq ? eq - s : (long)strlen(s)), s);
            snprintf(m->arg, sizeof m->arg, "%s", eq ? eq + 1 : "");
        }
        else if (!strcmp(argv[i], "--import") && i + 1 < argc) import_path = argv[++i];
        else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        else if ((ss_k = ss_parse_arg(argc, argv, i)) > 0) i += ss_k - 1;   /* savestate.c / replay.c */
#ifdef DESTREGA_GAME_MODULE
        else if ((ss_k = launcher_parse_arg(argc, argv, i)) > 0) i += ss_k - 1;
#endif
        else disc_path = argv[i];
    }

    /* Data pack location: SDL's per-user data folder (macOS: ~/Library/Application
     * Support/Destrega/, Linux: ~/.local/share/Destrega/, Windows: %APPDATA%\Destrega\). */
    char pak[2048];
    char *pref = data_dir ? NULL : SDL_GetPrefPath("", "Destrega");
    snprintf(pak, sizeof pak, "%s%sdestrega.pak", data_dir ? data_dir : (pref ? pref : "./"),
             data_dir ? "/" : "");
    char default_card[2048];
    snprintf(default_card, sizeof default_card, "%s%smemcard1.mcd",
             data_dir ? data_dir : (pref ? pref : "./"), data_dir ? "/" : "");
    snprintf(s_pak_path, sizeof s_pak_path, "%s", pak);
    snprintf(s_card_dir, sizeof s_card_dir, "%s%s", data_dir ? data_dir : (pref ? pref : "./"), data_dir ? "/" : "");
    s_data_dir_given = data_dir != NULL;
#ifdef DESTREGA_GAME_MODULE
    {   /* --game-selftest: the launcher's build path without a disc (CI) */
        int st = launcher_selftest(s_card_dir, s_data_dir_given);
        if (st >= 0) return st;
    }
#endif
    s_disc_path = disc_path;
    SDL_free(pref);
    if (getenv("DESTREGA_RESTARTED")) {
        plat_unsetenv("DESTREGA_RESTARTED");
        fprintf(stderr, "menu: restarted on the new data pack\n");
    }

    if (import_path) {
        const char *err = disc_import(import_path, pak, cli_progress, NULL);
        if (err) { fprintf(stderr, "import failed: %s\n", err); return 1; }
        fprintf(stderr, "data pack written to %s\n", pak);
        return 0;
    }
    settings_apply(s_card_dir);
    if (s_headless && !s_res_chosen) s_res_fit = true;   /* headless: --resize keeps fitting the window */
    if (s_headless && input_have_events()) {   /* --input-event: SDL's event queue and virtual controllers */
        if (!SDL_Init(SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        s_host_input = true;
    }
    if (s_widescreen && !s_win_set) { s_win_w = 1280; s_win_h = 720; }
    if (!s_headless) { create_window(); portmap_set_windowed(true); }   /* only windowed play maps real ports */
    const char *src = disc_path ? disc_path : pak;
    if (!disc_path) {
        FILE *f = fopen(pak, "rb");
        if (f) fclose(f);
        else if (s_headless) {
            fprintf(stderr, "no data pack at %s; run: destrega --import /path/to/Destrega.cue\n", pak);
            return 1;
        } else if (!gui_import(pak)) return 0;
    }
    if (!disc_open(src)) { fprintf(stderr, "cannot open %s\n", src); return 1; }
    const char *verr = disc_verify();
    if (verr) {
        fprintf(stderr, "%s\n", verr);
        if (!s_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Destrega", verr, s_win);
        return 1;
    }
#ifdef DESTREGA_GAME_MODULE
    {   /* load the game module, building it from this disc first if needed (runtime/launcher/) */
        int code;
        if (!launcher_prepare(s_card_dir, s_headless, s_win, &code)) return code;
    }
#endif

    if (!s_no_memcard) {
        /* Windowed play defaults to the user's card; headless runs only get a card when asked
         * for, so tests never depend on whatever the user has saved. */
        if (!s_memcard_path[0] && !s_headless) s_memcard_path[0] = default_card;
        for (int p = 0; p < 2; p++) {
            const char *err = memcard_attach(p, s_memcard_path[p]);
            if (err) fprintf(stderr, "%s\n", err);
            else if (s_memcard_path[p]) rt_log("memcard: slot %d = %s", p + 1, s_memcard_path[p]);
        }
        atexit(memcard_flush);
    }

    s_ws_req = s_widescreen;
    s_ws_used = s_widescreen;
    if (s_widescreen) {
        widescreen_enable(true);
        hlr_set_guard(WS_GUARD);
        hires_set_guard(WS_GUARD);
        if (s_opt_sx == 0 || (s_headless && s_opt_sx < 0 && !res_auto() && !s_resize_n))
            rt_log("widescreen: hi-res is off, so nothing beyond 4:3 can be shown");
    }
    s_pres_w = s_win_w; s_pres_h = s_win_h;   /* headless: --window (or --resize) is the window */
    bus_init();
    gte_init();
    gpu_init();
    hires_set_dither(s_hires_dither);
    g_pgxp = s_pgxp > 0;
    { extern u32 g_callstack[1024]; extern u32 g_depth; g_pgxp_callstack = g_callstack; g_pgxp_depth = &g_depth; }
    hires_set_perspective(s_pgxp == 2);
    widescreen_add_state_hook(pgxp_snapshot_save, pgxp_snapshot_restore);   /* shadow pass leaves PGXP alone */
    hires_set_threads(s_hires_threads);
    if (s_renderer != R_SOFTWARE) {
        /* headless: a device of its own; windowed: already done (true if initialised) */
        if (!hires_gpu_init(NULL)) {
            rt_log("hires-gpu: GPU renderer unavailable; using the software renderer");
            s_renderer = R_SOFTWARE;
        }
    }
    if (s_renderer_pref_sw && s_renderer == R_GPU) {   /* settings: software, live switch stays possible */
        s_renderer = R_SOFTWARE;
        hires_set_backend(NULL, NULL);
    }
    s_gpu_used = s_renderer != R_SOFTWARE;
    hires_set_tex_filter(s_opt_filter >= 0 ? s_opt_filter : s_headless ? HIRES_TEX_NEAREST : HIRES_TEX_XBR);
    hires_set_colour(s_opt_full >= 0 ? s_opt_full == 1 : !s_headless);
    hires_set_ssaa(s_opt_ssaa >= 1 ? s_opt_ssaa : ssaa_default(s_renderer));
    /* default SSAA only while the render target costs no more than 4x6 (measured: 4x6 x2 = 8x12
     * took ~4x the GPU time, far over a 60 Hz frame); an explicit choice is never capped */
    hires_set_ssaa_cap(ssaa_cap());
    rt_log("hires: renderer %s", renderer_name(s_renderer));
    if (s_renderer == R_GPU) hires_set_backend(hires_gpu_backend(), NULL);
    else if (s_renderer == R_COMPARE) hires_set_backend(NULL, hires_gpu_backend());
    if (s_opt_sx > 0 && !hires_set_scale(s_opt_sx, s_opt_sy))
        rt_log("hires: cannot allocate scale %dx%d", s_opt_sx, s_opt_sy);
    else if (s_headless && res_auto()) apply_window_size(s_pres_w, s_pres_h, true);   /* --scale auto */
    log_render_quality("");
    {   /* the full 1x picture (gpu.c display copy): presentation only, the emulated VRAM is the same */
        bool early_hires = hires_get_scale(NULL, NULL);
        for (int k = 0; k < s_resize_n; k++) early_hires |= s_resizes[k].frame <= 1;
        if (s_gpu1x == 3) {
            /* draw every primitive into the emulated VRAM, as builds before the skip rule did: the
             * complete state hash then differs from other runs, so never in a netplay session */
            bool net = false;
            for (int k = 1; k < argc; k++) net |= !strcmp(argv[k], "--host") || !strcmp(argv[k], "--join");
            if (net) { rt_log("gpu-1x: draw ignored in a netplay session (both players must hash the same VRAM)"); s_gpu1x = 0; }
            else { gpu_set_skip_rule(false); rt_log("gpu-1x: skip rule off, every primitive drawn into the emulated VRAM"); }
        }
        bool on = s_gpu1x == 1 || (s_gpu1x == 0 && (s_verify || (s_headless && !early_hires)));
        if (on) gpu_enable_display_copy();
        rt_log("gpu-1x: display copy %s", on ? "on (full 1x picture)" : "off (the picture comes from the hi-res target)");
    }
    spu_init();
    mdec_init();

    u32 size;
    u8 *exe = disc_load_file("SLUS_007.32", &size);
    if (!exe || memcmp(exe, "PS-X EXE", 8)) { fprintf(stderr, "SLUS_007.32 not found on disc\n"); return 1; }
    u32 pc0 = *(u32 *)(exe + 0x10), gp = *(u32 *)(exe + 0x14);
    u32 taddr = *(u32 *)(exe + 0x18), tsize = *(u32 *)(exe + 0x1c);
    void bios_init(void);
    bios_init();
    memcpy(ram_ptr(taddr), exe + 0x800, tsize);
    free(exe);

    if (!s_headless) {
        /* the import UI used a fixed 640x480 logical canvas; from here on we letterbox ourselves */
        SDL_SetRenderLogicalPresentation(s_ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (s_audio) SDL_ResumeAudioStreamDevice(s_audio);
        menu_init();   /* File menu; windowed only. Headless runs have no menus. */
        if (s_overlay_menu) overlay_menu_init(s_win, &(OverlayHooks){ handle_event, overlay_shot, false, 0, 0 });
        /* display.fullscreen = on: entered from menu_script_and_poll once the window has focus */
    } else   /* headless: the in-window menu's test hooks only (menu-test=...), never drawn */
        overlay_menu_init(NULL, &(OverlayHooks){ handle_event, overlay_shot, true, s_win_w, s_win_h });

    memset(&g_cpu.r, 0, sizeof g_cpu.r);
    g_cpu.r[29] = 0x801fff00;   /* SYSTEM.CNF STACK */
    g_cpu.r[30] = 0x801fff00;
    g_cpu.r[28] = gp;
    g_cpu.r[31] = 0;
    g_cpu.cop0[COP0_SR] = 0x40000401;
    rt_log("boot: pc=%08x text=%08x+%x", pc0, taddr, tsize);
    rt_call(pc0);
    rt_log("main returned");
    return 0;
}
