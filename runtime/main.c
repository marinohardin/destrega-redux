/* Frontend: boot, SDL3 window/audio/input, headless test mode. */
#include "recomp.h"
#include "mdec.h"
#include "memcard.h"
#include "menu.h"
#include "hires.h"
#include "widescreen.h"
#include "hires_gpu.h"
#include "overrides/override.h"
#include "savestate.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <zlib.h>

void bus_init(void);
#include "disc.h"

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
static int s_opt_sx = -1, s_opt_sy = -1;   /* --scale N|NxM: -1 = auto from the window, 0 = off */
static int s_max_sx = 4, s_max_sy = 6;     /* auto-scale caps per axis (see AGENTS.md) */
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
static MenuStep s_menu_script[16];     /* --menu-action FRAME:NAME[=ARG] test hook */
static int s_menu_script_n;
static void menu_script_and_poll(void);
static const char *s_ram_dump;     /* --ram-dump FILE: main RAM at the end of a headless run */
static FILE *s_state_hash;         /* --state-hash FILE */
static FILE *s_gameplay;           /* --gameplay-trace FILE */
static u64 s_ws_wide[2];           /* widescreen: frames presented wider than 4:3 (all, fight window) */
static const char *s_ram_snap_dir; /* --ram-snap DIR: raw RAM + scratchpad every --ram-snap-every frames */
static u64 s_ram_snap_every = 100;
typedef struct { u64 frame; int w, h; } ResizeStep;
static ResizeStep s_resizes[256];
static int s_resize_n;
static SDL_Texture *s_hitex;
static int s_hitex_w, s_hitex_h;
/* --renderer software|gpu|compare (see AGENTS.md "GPU renderer") */
enum { R_SOFTWARE, R_GPU, R_COMPARE };
static int s_renderer = R_SOFTWARE;
static bool s_renderer_given;              /* --renderer on the command line */
static bool s_gpu_ok;                      /* GPU backend initialised (live switching possible) */
static bool s_gpu_used;                    /* the GPU backend ran at some point (stats) */
static SDL_GPUTexture *s_gputex_src;       /* GPU texture wrapped by s_gputex */
static SDL_Texture *s_gputex;
static int s_compare_every = 30;           /* --compare-every N */
static double s_compare_max = -1;          /* --compare-max PCT: fail if a fight frame differs more */
static struct { u64 frames, exact, px, diff, big, mask; double worst; u64 worst_frame; } s_cmp[2];

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
    u16 *vram = gpu_vram();
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

/* Convert a hi-res view to RGBA8888 (R in the low byte, as ABGR8888 on little endian). */
typedef struct { const HiresView *v; u32 *dst; size_t pitch; } ConvJob;
static void convert_rows(void *c, int y0, int y1) {
    const ConvJob *j = c;
    if (y1 > j->v->h) y1 = j->v->h;
    for (int y = y0; y < y1; y++) {
        const u16 *src = j->v->pixels + (size_t)y * j->v->stride;
        u32 *d = j->dst + y * j->pitch;
        for (int x = 0; x < j->v->w; x++) {   /* plain arithmetic: auto-vectorizes */
            u32 p = src[x];
            d[x] = 0xff000000u | ((p & 0x7c00) << 9) | ((p & 0x03e0) << 6) | ((p & 0x001f) << 3);
        }
    }
}
static void convert_view(const HiresView *v, u32 *dst, size_t dst_pitch_px) {
    ConvJob j = { v, dst, dst_pitch_px };
    hires_parallel_rows(convert_rows, &j, v->h);
}

static int ws_present_extra(const GpuDisplay *d);
void frontend_screenshot(const char *path) {
    GpuDisplay d;
    HiresView hv;
    gpu_get_display(&d);
    if (hires_read_view_wide(&d, ws_present_extra(&d), &hv)) {
        u32 *buf = malloc((size_t)hv.w * hv.h * 4);
        if (buf) {
            convert_view(&hv, buf, (size_t)hv.w);
            write_png(path, buf, hv.w, hv.h);
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
    u16 *v = gpu_vram();
    for (int i = 0; i < 1024 * 512; i++) {
        u16 p = v[i];
        buf[i] = 0xff000000u | (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) | ((p & 31) << 3);
    }
    write_png(path, buf, 1024, 512);
}

/* ---- input ----------------------------------------------------------------------------- */

enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT,
       B_L2, B_R2, B_L1, B_R1, B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };

static int key_to_button(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return B_UP; case SDLK_DOWN: return B_DOWN;
    case SDLK_LEFT: return B_LEFT; case SDLK_RIGHT: return B_RIGHT;
    case SDLK_Z: return B_CROSS; case SDLK_X: return B_CIRCLE;
    case SDLK_A: return B_SQUARE; case SDLK_S: return B_TRIANGLE;
    case SDLK_Q: return B_L1; case SDLK_W: return B_R1;
    case SDLK_E: return B_L2; case SDLK_R: return B_R2;
    case SDLK_RETURN: return B_START; case SDLK_BACKSPACE: case SDLK_RSHIFT: return B_SELECT;
    }
    return -1;
}

static u16 s_keys_down;   /* pressed bits (active high) */

static void poll_input(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) exit(0);
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (e.key.key == SDLK_ESCAPE && down) exit(0);
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
                continue;
            }
            if (e.key.key == SDLK_F12 && down) {
                char p[64]; snprintf(p, sizeof p, "shot_%06llu.png", (unsigned long long)s_frame);
                frontend_screenshot(p);
            }
            int b = key_to_button(e.key.key);
            if (b >= 0) { if (down) s_keys_down |= 1u << b; else s_keys_down &= ~(1u << b); }
        }
        if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s_win, &pw, &ph);   /* the auto scale follows in present_frame */
            rt_log("fullscreen: %s, window %dx%d px", e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
                   ? "entered" : "left", pw, ph);
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
    }
    u16 pads[2] = {s_keys_down, 0};
    /* gamepads: first -> port 1 (together with the keyboard), second -> port 2 */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int k = 0; ids && k < n && k < 2; k++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[k]);
        if (!g) continue;
        static const struct { SDL_GamepadButton b; int bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, B_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, B_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST, B_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, B_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R1},
            {SDL_GAMEPAD_BUTTON_START, B_START}, {SDL_GAMEPAD_BUTTON_BACK, B_SELECT},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT},
        };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GetGamepadButton(g, map[i].b)) pads[k] |= 1u << map[i].bit;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) pads[k] |= 1u << B_L2;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) pads[k] |= 1u << B_R2;
        /* left stick as d-pad */
        int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -16000) pads[k] |= 1u << B_LEFT;
        if (ax > 16000) pads[k] |= 1u << B_RIGHT;
        if (ay < -16000) pads[k] |= 1u << B_UP;
        if (ay > 16000) pads[k] |= 1u << B_DOWN;
    }
    SDL_free(ids);
    for (int i = 0; i < s_script_n; i++)
        if (s_script[i].frame <= s_frame && s_frame < s_script[i].frame + 6) pads[0] |= s_script[i].mask;
    g_pad_buttons[0] = (u16)~pads[0];
    g_pad_buttons[1] = (u16)~pads[1];
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
    h = hash_mix(h, spu_ram_ptr(&spu_n), spu_n);
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

/* Pick the render scales for a window of pw x ph physical pixels, from the display mode
 * currently shown (24-bit / disabled displays keep the 512x240 reference). */
static void apply_window_size(int pw, int ph, bool simulated) {
    if (!simulated && s_opt_sx >= 0) return;
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
    hires_get_scale(&cx, &cy);
    if (sx != cx || sy != cy) {
        rt_log("hires: window %dx%d, display %dx%d -> render scale %dx%d", pw, ph, dw, dh, sx, sy);
        hires_set_scale(sx, sy);
    }
}

/* ---- runtime renderer switch (View > Renderer, --menu-action renderer=software|gpu) ----
 * Between frames: select the backend and reallocate the hi-res target at the current scale,
 * seeded from the 1x VRAM like a resize (hires_set_scale destroys the old backend's target).
 * Nothing emulated is touched. The SDL renderer stays (the GPU renderer presents both). */
static const char *renderer_name(int r) { return r == R_GPU ? "gpu" : r == R_COMPARE ? "compare" : "software"; }
const char *frontend_renderer(void) { return renderer_name(s_renderer); }
bool frontend_gpu_available(void) { return s_gpu_ok || s_headless; }

static void renderer_switch(int r) {
    if (r == s_renderer) return;
    if (r != R_SOFTWARE && !s_gpu_ok) {
        /* headless: create the backend's own device on first use; windowed: needs the GPU renderer */
        if (s_headless) s_gpu_ok = hires_gpu_init(NULL);
        if (!s_gpu_ok) { rt_log("hires: renderer %s unavailable; staying on %s", renderer_name(r), renderer_name(s_renderer)); return; }
    }
    if (r == R_GPU) hires_set_backend(hires_gpu_backend(), NULL);
    else if (r == R_COMPARE) hires_set_backend(NULL, hires_gpu_backend());
    else hires_set_backend(NULL, NULL);
    int sx, sy;
    bool on = hires_get_scale(&sx, &sy);
    if (on && !hires_set_scale(sx, sy)) {
        rt_log("hires: renderer %s cannot allocate %dx%d; back to %s", renderer_name(r), sx, sy, renderer_name(s_renderer));
        if (s_renderer == R_GPU) hires_set_backend(hires_gpu_backend(), NULL);
        else if (s_renderer == R_COMPARE) hires_set_backend(NULL, hires_gpu_backend());
        else hires_set_backend(NULL, NULL);
        hires_set_scale(sx, sy);
        return;
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

extern u64 g_hires_stat_inline_px, g_hires_stat_par_px, g_hires_stat_par_jobs, g_hires_stat_inline_jobs;
static void finish(void) {
    ss_finish();   /* savestate / replay / netplay reports (savestate.c) */
    void widescreen_report(void); widescreen_report();
    if (g_trace) rt_log("hires-pool: triangles inline %llu (bbox px %llu), parallel %llu (bbox px %llu)", (unsigned long long)g_hires_stat_inline_jobs, (unsigned long long)g_hires_stat_inline_px, (unsigned long long)g_hires_stat_par_jobs, (unsigned long long)g_hires_stat_par_px);
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
    if (g_pgxp_stat_tris) {
        rt_log("pgxp-check: %llu triangles with precise vertices; %llu with an on-screen vertex > 1.5 px from its integer, "
               "%llu with depth ratio > 2, %llu using a value older than 2 frames, %llu split vertices",
               (unsigned long long)g_pgxp_stat_tris, (unsigned long long)g_pgxp_stat_bad_d,
               (unsigned long long)g_pgxp_stat_bad_w, (unsigned long long)g_pgxp_stat_old,
               (unsigned long long)g_pgxp_stat_split);
        rt_log("pgxp-weld: %llu vertex uses snapped to another precise value of the same word, %llu to the "
               "integer, %llu table-full fallbacks",
               (unsigned long long)g_pgxp_stat_welded, (unsigned long long)g_pgxp_stat_weld_int,
               (unsigned long long)g_pgxp_stat_weld_full);
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
            rt_log("hires-compare: %s: %llu frames compared, %llu identical; %llu of %llu pixels differ (%.5f%%), "
                   "%llu by more than 1 step in a channel, %llu in the mask bit only; worst frame %llu: %.4f%%",
                   k ? "fight" : "all", (unsigned long long)s_cmp[k].frames, (unsigned long long)s_cmp[k].exact,
                   (unsigned long long)s_cmp[k].diff, (unsigned long long)s_cmp[k].px,
                   100.0 * (double)s_cmp[k].diff / (double)s_cmp[k].px, (unsigned long long)s_cmp[k].big,
                   (unsigned long long)s_cmp[k].mask, (unsigned long long)s_cmp[k].worst_frame, s_cmp[k].worst);
        }
        if (s_compare_max >= 0 && (!s_cmp[1].frames || s_cmp[1].worst > s_compare_max)) {
            rt_log("hires-compare: FAIL: worst fight frame %.4f%% > %.4f%% (or no fight frames compared)",
                   s_cmp[1].worst, s_compare_max);
            rc = 6;
        }
    }
    if (s_gpu_used) {
        HiresGpuStats st;
        hires_gpu_get_stats(&st);
        double n = st.submits ? (double)st.submits : 1;
        rt_log("hires-gpu: %llu submits, %.1f render passes / %.1f draws / %.0f primitives per submit "
               "(%llu triangles coverage-tested in the shader), "
               "%llu VRAM texels uploaded, %llu VRAM copies, %llu readbacks",
               (unsigned long long)st.submits, st.passes / n, st.draws / n, st.prims / n,
               (unsigned long long)st.edge_tris,
               (unsigned long long)st.vram_up_texels, (unsigned long long)st.tcopies, (unsigned long long)st.readbacks);
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

/* --press / --press2 scripts for the current frame (taps held 6 frames). */
static void script_pads(void) {
    u16 pad = 0;
    for (int i = 0; i < s_script_n; i++)
        if (s_script[i].frame <= s_frame && s_frame < s_script[i].frame + 6) pad |= s_script[i].mask;
    g_pad_buttons[0] = (u16)~pad;
    u16 pad2 = 0;
    for (int i = 0; i < s_script2_n; i++)
        if (s_script2[i].frame <= s_frame && s_frame < s_script2[i].frame + 6) pad2 |= s_script2[i].mask;
    if (s_script2_n) g_pad_buttons[1] = (u16)~pad2;
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
    ws_apply();                            /* a View-menu switch takes effect this vblank */
    for (int i = 0; i < s_resize_n; i++)
        if (s_resizes[i].frame == s_frame) {
            rt_log("resize %dx%d", s_resizes[i].w, s_resizes[i].h);
            apply_window_size(s_resizes[i].w, s_resizes[i].h, true);
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
        if (s_verify && hires_get_scale(&cx, &cy) && cx == 1 && cy == 1) {
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
            for (int k = 0; k < 2; k++) {
                if (k && s_frame < s_fight_from) continue;
                s_cmp[k].frames++;
                if (!c.diff) s_cmp[k].exact++;
                s_cmp[k].px += c.pixels; s_cmp[k].diff += c.diff; s_cmp[k].big += c.diff_big; s_cmp[k].mask += c.mask_diff;
                if (pct > s_cmp[k].worst || !s_cmp[k].worst_frame) { s_cmp[k].worst = pct; s_cmp[k].worst_frame = s_frame; }
            }
            if (c.diff && g_trace)
                rt_log("hires-compare: frame %llu: %llu pixels differ (%.4f%%), %llu by > 1, first at target (%d,%d)",
                       (unsigned long long)s_frame, (unsigned long long)c.diff, pct,
                       (unsigned long long)c.diff_big, c.first_x, c.first_y);
        }
    }
    if (s_state_hash) write_state_hash();
    if (s_gameplay) write_gameplay_trace();
    if (s_ram_snap_dir && s_frame % s_ram_snap_every == 0) {
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
        if (t0 && !s_headless) rt_log("frontend: %.1f fps", 600.0 * 1e9 / (double)(now - t0));
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
    u64 t_start = SDL_GetTicksNS();
    if (t_prev_end) t_emu += t_start - t_prev_end;
    poll_input();
    present_frame();
    u64 t_after_present = SDL_GetTicksNS();
    t_present += t_after_present - t_start;
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

    /* audio: keep ~3 frames queued */
    spu_render(audio, 735);
    if (SDL_GetAudioStreamQueued(s_audio) < 735 * 4 * 6 && !s_fast)
        SDL_PutAudioStreamData(s_audio, audio, 735 * 4);
    if (SDL_GetAudioStreamQueued(s_audio) < 735 * 4 * 2 && !s_fast) {
        /* the extra render advances the SPU: not allowed when emulation must be reproducible */
        if (!ss_deterministic_audio()) spu_render(audio, 367);
        SDL_PutAudioStreamData(s_audio, audio, 367 * 4);
    }

    /* pacing to 60Hz */
    u64 now = SDL_GetTicksNS();
    const u64 frame_ns = 1000000000ull / 60;
    if (s_frame % 600 == 0) {
        rt_log("host per frame: emu %.2f ms, present+convert %.2f ms, sleep %.2f ms",
               t_emu / 600.0 / 1e6, t_present / 600.0 / 1e6, t_sleep / 600.0 / 1e6);
        t_emu = t_present = t_sleep = 0;
    }
    if (s_fast) { s_next_tick_ns = now; t_prev_end = SDL_GetTicksNS(); return; }
    if (s_next_tick_ns == 0 || now > s_next_tick_ns + frame_ns * 4) s_next_tick_ns = now;
    s_next_tick_ns += frame_ns;
    if (s_next_tick_ns > now) SDL_DelayNS(s_next_tick_ns - now);
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
    SDL_RenderClear(s_ren);
    bool have = hires_get_view_wide(&d, extra, &hv);
    if (have && extra) dst = letterbox_ar(pw, ph, 4.0 / 3.0 * (d.w + 2 * extra) / d.w);
    if (have && hv.gpu) {
        /* GPU backend: present straight from the GPU (target -> RGBA8 conversion pass), no readback */
        SDL_GPUTexture *t = hires_gpu_present(&hv);
        if (t && (t != s_gputex_src || !s_gputex || s_gputex->w != hv.w || s_gputex->h != hv.h)) {
            if (s_gputex) SDL_DestroyTexture(s_gputex);
            SDL_PropertiesID pr = SDL_CreateProperties();
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, hv.w);
            SDL_SetNumberProperty(pr, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, hv.h);
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
        if (!s_hitex || s_hitex_w != hv.w || s_hitex_h != hv.h) {
            if (s_hitex) SDL_DestroyTexture(s_hitex);
            s_hitex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                        hv.w, hv.h);
            if (s_hitex) SDL_SetTextureScaleMode(s_hitex, SDL_SCALEMODE_LINEAR);
            s_hitex_w = hv.w; s_hitex_h = hv.h;
        }
        /* Upload only if the displayed rows were drawn since the last upload (fights draw at
         * 30 fps but present at 60), or the view/target changed. */
        static const u16 *up_pixels; static int up_stride, up_w, up_h; static u64 up_epoch; static bool up_ok;
        bool same = up_ok && up_pixels == hv.pixels && up_stride == hv.stride && up_w == hv.w &&
                    up_h == hv.h && up_epoch == hires_epoch();
        if (!same || hires_dirty_rows(d.y, d.y + d.h - 1)) {
            void *px; int pitch;
            up_ok = false;
            if (s_hitex && SDL_LockTexture(s_hitex, NULL, &px, &pitch)) {
                convert_view(&hv, px, (size_t)pitch / 4);
                SDL_UnlockTexture(s_hitex);
                up_pixels = hv.pixels; up_stride = hv.stride; up_w = hv.w; up_h = hv.h;
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
    SDL_RenderPresent(s_ren);
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
        "  --scale N|NxM         internal render scale (horizontal x vertical): 0 = hi-res off, 1..8\n"
        "                        fixed (default: auto from the window size; headless: off)\n"
        "  --max-scale N|NxM     per-axis cap for the automatic scale (default 4x6)\n"
        "  --window WxH          initial window size in points (default 960x720)\n"
        "  --hires-threads N     hi-res rasterizer worker threads: -1 auto (performance cores - 1), 0 serial\n"
        "  --segments F,F,...    frame boundaries for the per-segment timing report (default 1200,1800,2400,3000)\n"
        "  --fight-from F        first frame counted as 'fight' in the hi-res frame stats (default 3700)\n"
        "  --hires-min-fight P   exit non-zero unless P%% of fight frames were shown from the hi-res target\n"
        "  --hires-dither        keep PS1 dithering in the hi-res target at scale > 1\n"
        "  --pgxp on|off|affine  sub-pixel GTE vertices + perspective-correct textures in the hi-res\n"
        "                        target (default on; affine = precise positions, PS1 texture mapping)\n"
        "  --resize F:WxH,...    headless: simulate window resizes (physical pixels) on given frames\n"
        "  --ram-dump FILE       headless: write main RAM to FILE at the end\n"
        "  --memcard1 PATH       memory card image for slot 1 (raw 128 KiB .mcd; created if missing)\n"
        "  --memcard2 PATH       memory card image for slot 2 (default: empty slot)\n"
        "  --no-memcard          no memory cards at all (the game cannot save)\n"
        "                        (default: <data dir>/memcard1.mcd; headless runs: no card unless given)\n"
        "  --state-hash FILE     write a per-frame hash of all emulated state (determinism testing)\n"
        "  --gameplay-trace FILE per frame: fighters' health/gauge/position, round time, wins, RNG seed\n"
        "  --ram-snap DIR        write raw guest RAM + scratchpad to DIR every --ram-snap-every N frames (100)\n"
        "  --hires-verify        at scale 1, check the hi-res target equals the 1x VRAM every frame\n"
        "  --renderer R          hi-res renderer: software, gpu (SDL_GPU/Metal), or compare (default: gpu\n"
        "                        in a macOS window, else software; View > Renderer switches at runtime)\n"
        "                        (software shown, gpu fed the same commands and compared to it)\n"
        "  --compare-every N     compare mode: compare the targets every N frames (default 30)\n"
        "  --compare-max PCT     compare mode: exit non-zero if a fight frame differs in more than PCT%% of pixels\n"
        "  --widescreen          16:9 fights: wider stage culling + hi-res guard band (menus/FMV stay 4:3;\n"
        "                        needs hi-res; the window aspect picks 4:3..16:9; default window 1280x720;\n"
        "                        View > Widescreen (16:9) or F9 switches at runtime)\n"
        "  --widescreen-at F:0|1,...  headless: switch widescreen off/on at the given frames\n"
        "  --menu-action F:NAME[=ARG]  (testing) run a menu action on frame F: load=IMAGE, reveal-game,\n"
        "                        reveal-saves, save-import=FILE, save-export=FILE, save-delete,\n"
        "                        widescreen[=on|off], renderer[=software|gpu|compare] (View menu;\n"
        "                        no argument = toggle)\n"
        "                        (save actions use card slot 1; headless needs --data-dir or --memcard1)\n"
        "  --trace               verbose runtime logging (repeat for more)\n"
        "  --no-interp           make any MIPS-interpreter fallback fatal (testing)\n"
        "  --verify-overrides    run hand-written overrides against the recompiled originals\n");
    ss_usage();
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

static bool same_file(const char *a, const char *b) {
    if (!strcmp(a, b)) return true;
#ifndef _WIN32
    char ra[PATH_MAX], rb[PATH_MAX];
    return realpath(a, ra) && realpath(b, rb) && !strcmp(ra, rb);
#else
    return false;
#endif
}

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
    if (ok) ok = rename(tmp, dst) == 0;
    if (!ok) remove(tmp);
    return ok;
}

static void dir_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *sl = strrchr(out, '/');
    if (sl && sl != out) *sl = 0;
    else if (!sl) snprintf(out, n, ".");
}

static void import_progress(void *ud, float frac) {
    (void)ud;
    if (!s_headless) draw_progress(NULL, frac);
}

/* After a new data pack is in place: re-exec the process (a clean machine, never a half-reset one). */
static void restart_process(void) {
    char exe[PATH_MAX];
    if (!menu_executable_path(exe, sizeof exe)) snprintf(exe, sizeof exe, "%s", s_argv[0]);
    char **av = calloc((size_t)s_argc + 2, sizeof *av);
    int n = 0;
    av[n++] = exe;
    for (int i = 1; i < s_argc; i++) {
        if (!strcmp(s_argv[i], "--menu-action") && i + 1 < s_argc) { i++; continue; }
        if (s_disc_path && s_argv[i] == s_disc_path) continue;   /* run from the new pack instead */
        av[n++] = s_argv[i];
    }
    rt_log("menu: restarting on the new data pack");
    memcard_flush();
    if (s_state_hash) fflush(s_state_hash);
    fflush(NULL);
    setenv("DESTREGA_RESTARTED", "1", 1);
    if (!s_headless) { if (s_audio) SDL_DestroyAudioStream(s_audio); SDL_Quit(); }
    execv(exe, av);
    fprintf(stderr, "menu: ERROR: cannot restart (%s)\n", exe);
    exit(1);
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
        if (l > 1 && dir[l - 1] == '/') dir[l - 1] = 0;
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
    localtime_r(&t, &tm);
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

static void menu_run(const char *name, const char *arg) {
    rt_log("menu: action %s%s%s", name, *arg ? " " : "", arg);
    if (!strcmp(name, "load")) action_load(arg);
    else if (!strcmp(name, "reveal-game")) action_reveal(false);
    else if (!strcmp(name, "reveal-saves")) action_reveal(true);
    else if (!strcmp(name, "save-import")) action_card_import(arg);
    else if (!strcmp(name, "save-export")) action_card_export(arg);
    else if (!strcmp(name, "save-delete")) action_card_delete(arg);
    else if (!strcmp(name, "renderer")) {   /* View > Renderer; arg software|gpu|compare, none = toggle */
        if (!strcmp(arg, "gpu")) renderer_switch(R_GPU);
        else if (!strcmp(arg, "software")) renderer_switch(R_SOFTWARE);
        else if (!strcmp(arg, "compare")) renderer_switch(R_COMPARE);
        else if (!*arg) renderer_switch(s_renderer == R_SOFTWARE ? R_GPU : R_SOFTWARE);
        else rt_log("menu: renderer: software, gpu or compare");
    }
    else if (!strcmp(name, "fullscreen"))   /* View > Enter/Exit Full Screen; arg on/off, none/toggle */
        fullscreen_set(!strcmp(arg, "on") || !strcmp(arg, "1") ? true
                       : !strcmp(arg, "off") || !strcmp(arg, "0") ? false
                       : !frontend_fullscreen());
    else if (!strcmp(name, "widescreen"))   /* View > Widescreen (16:9); arg on/off, none = toggle */
        frontend_set_widescreen(!strcmp(arg, "on") || !strcmp(arg, "1") ? true
                                : !strcmp(arg, "off") || !strcmp(arg, "0") ? false
                                : !frontend_widescreen());
    else rt_log("menu: unknown action %s", name);
}

static void menu_script_and_poll(void) {
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
    s_win = SDL_CreateWindow("Destrega (recompiled)", s_win_w, s_win_h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    s_ren = NULL;
#ifdef __APPLE__
    if (!s_renderer_given) s_renderer = R_GPU;   /* windowed default on macOS */
#endif
    /* SDL's GPU renderer, whose device the hi-res backend draws with. Created whenever the GPU
     * backend may be used, so View > Renderer can switch at any time; --renderer software
     * forces the plain renderer (and software only). */
    if (s_renderer != R_SOFTWARE || !s_renderer_given) {
        s_ren = SDL_CreateGPURenderer(NULL, s_win);
        s_gpu_ok = s_ren && hires_gpu_init(SDL_GetGPURendererDevice(s_ren));
        if (!s_gpu_ok) {
            rt_log("hires-gpu: GPU renderer unavailable (%s); using the software renderer", SDL_GetError());
            if (s_ren) SDL_DestroyRenderer(s_ren);
            s_ren = NULL;
            s_renderer = R_SOFTWARE;
        }
    }
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, NULL);
    SDL_SetRenderLogicalPresentation(s_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
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
            char *e; s_opt_sx = (int)strtol(argv[++i], &e, 10);
            s_opt_sy = (*e == 'x' || *e == 'X') ? (int)strtol(e + 1, NULL, 10) : s_opt_sx;
        }
        else if (!strcmp(argv[i], "--max-scale") && i + 1 < argc) {
            char *e; s_max_sx = (int)strtol(argv[++i], &e, 10);
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
        else if (!strcmp(argv[i], "--pgxp-diag") && i + 1 < argc) g_pgxp_diag = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pgxp") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "on")) s_pgxp = 2;
            else if (!strcmp(m, "off")) s_pgxp = 0;
            else if (!strcmp(m, "affine")) s_pgxp = 1;
            else { fprintf(stderr, "--pgxp: expected on, off or affine\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--hires-verify")) s_verify = true;
        else if (!strcmp(argv[i], "--widescreen")) s_widescreen = true;
        else if (!strcmp(argv[i], "--ram-snap") && i + 1 < argc) s_ram_snap_dir = argv[++i];
        else if (!strcmp(argv[i], "--gameplay-trace") && i + 1 < argc) {
            s_gameplay = fopen(argv[++i], "w");
            if (!s_gameplay) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
        }
        else if (!strcmp(argv[i], "--ram-snap-every") && i + 1 < argc) s_ram_snap_every = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) {
            const char *r = argv[++i];
            s_renderer_given = true;
            if (!strcmp(r, "software")) s_renderer = R_SOFTWARE;
            else if (!strcmp(r, "gpu")) s_renderer = R_GPU;
            else if (!strcmp(r, "compare")) s_renderer = R_COMPARE;
            else { fprintf(stderr, "--renderer: software, gpu or compare\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--compare-every") && i + 1 < argc) s_compare_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--compare-max") && i + 1 < argc) s_compare_max = atof(argv[++i]);
        else if (!strcmp(argv[i], "--state-hash") && i + 1 < argc) {
            s_state_hash = fopen(argv[++i], "w");
            if (!s_state_hash) { fprintf(stderr, "cannot write %s\n", argv[i]); return 1; }
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
        else if (!strcmp(argv[i], "--menu-action") && i + 1 < argc && s_menu_script_n < 16) {
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
    s_disc_path = disc_path;
    SDL_free(pref);
    if (getenv("DESTREGA_RESTARTED")) {
        unsetenv("DESTREGA_RESTARTED");
        fprintf(stderr, "menu: restarted on the new data pack\n");
    }

    if (import_path) {
        const char *err = disc_import(import_path, pak, cli_progress, NULL);
        if (err) { fprintf(stderr, "import failed: %s\n", err); return 1; }
        fprintf(stderr, "data pack written to %s\n", pak);
        return 0;
    }
    if (s_widescreen && !s_win_set) { s_win_w = 1280; s_win_h = 720; }
    if (!s_headless) create_window();
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
        hires_set_guard(WS_GUARD);
        if (s_opt_sx == 0 || (s_headless && s_opt_sx < 0 && !s_resize_n))
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
        s_gpu_ok = hires_gpu_init(NULL);   /* headless: a device of its own; windowed: already done */
        if (!s_gpu_ok) {
            rt_log("hires-gpu: GPU renderer unavailable; using the software renderer");
            s_renderer = R_SOFTWARE;
        }
    }
    s_gpu_used = s_renderer != R_SOFTWARE;
    rt_log("hires: renderer %s", s_renderer == R_GPU ? "gpu" : s_renderer == R_COMPARE ? "compare" : "software");
    if (s_renderer == R_GPU) hires_set_backend(hires_gpu_backend(), NULL);
    else if (s_renderer == R_COMPARE) hires_set_backend(NULL, hires_gpu_backend());
    if (s_opt_sx > 0 && !hires_set_scale(s_opt_sx, s_opt_sy))
        rt_log("hires: cannot allocate scale %dx%d", s_opt_sx, s_opt_sy);
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
    }

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
