/* Frontend: boot, SDL3 window/audio/input, headless test mode. */
#include "recomp.h"
#include "mdec.h"
#include "overrides/override.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static u64 s_next_tick_ns;
static u32 s_rgba[640 * 512];
static FILE *s_wav;
static u32 s_watch[8]; static int s_watch_n;   /* --watch ADDR: log 32-bit value changes per frame */
static u64 s_profile_from = 1;
static int s_profile_on;
static u32 s_wav_bytes;

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

void frontend_screenshot(const char *path) {
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
            if (e.key.key == SDLK_F12 && down) {
                char p[64]; snprintf(p, sizeof p, "shot_%06llu.png", (unsigned long long)s_frame);
                frontend_screenshot(p);
            }
            int b = key_to_button(e.key.key);
            if (b >= 0) { if (down) s_keys_down |= 1u << b; else s_keys_down &= ~(1u << b); }
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

void frontend_vblank(void) {
    s_frame++;
    static s16 audio[2048 * 2];
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
        u16 pad = 0;
        for (int i = 0; i < s_script_n; i++)
            if (s_script[i].frame <= s_frame && s_frame < s_script[i].frame + 6) pad |= s_script[i].mask;
        g_pad_buttons[0] = (u16)~pad;
        if (s_dump_dir && s_dump_every && s_frame % s_dump_every == 0) {
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
            if (s_wav) { wav_header(s_wav, s_wav_bytes); fclose(s_wav); }
            rt_log("headless: reached %llu frames", (unsigned long long)s_frame);
            void rt_dump_stack(void); rt_dump_stack();
            void rt_profile_dump(void); rt_profile_dump();
            exit(0);
        }
        return;
    }
    static u64 t_emu, t_present, t_sleep, t_prev_end;
    u64 t_start = SDL_GetTicksNS();
    if (t_prev_end) t_emu += t_start - t_prev_end;
    poll_input();
    int w, h;
    convert_display(&w, &h);
    SDL_UpdateTexture(s_tex, &(SDL_Rect){0, 0, w, h}, s_rgba, w * 4);
    SDL_RenderClear(s_ren);
    SDL_FRect src = {0, 0, (float)w, (float)h};
    SDL_RenderTexture(s_ren, s_tex, &src, NULL);
    SDL_RenderPresent(s_ren);
    u64 t_after_present = SDL_GetTicksNS();
    t_present += t_after_present - t_start;

    /* audio: keep ~3 frames queued */
    spu_render(audio, 735);
    if (SDL_GetAudioStreamQueued(s_audio) < 735 * 4 * 6 && !s_fast)
        SDL_PutAudioStreamData(s_audio, audio, 735 * 4);
    if (SDL_GetAudioStreamQueued(s_audio) < 735 * 4 * 2 && !s_fast) {
        spu_render(audio, 367);
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
        "  --press F:MASK,...    headless input script (MASK = hex PSX button bits, active high)\n"
        "  --wav FILE            record audio output (headless)\n"
        "  --trace               verbose runtime logging (repeat for more)\n"
        "  --no-interp           make any MIPS-interpreter fallback fatal (testing)\n"
        "  --verify-overrides    run hand-written overrides against the recompiled originals\n");
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

/* Windowed first launch: ask for the user's own disc image and import it.
 * Returns true once `pak` exists. */
static bool gui_import(const char *pak) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega",
        "Destrega needs your own copy of the game (North American release, SLUS-00732).\n\n"
        "Select your disc image (.cue or .bin). It is converted once into a private data pack; "
        "afterwards the original image is no longer needed.", s_win);
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image", "cue;bin"}};
    for (;;) {
        s_pick_state = 0;
        SDL_ShowOpenFileDialog(pick_cb, NULL, s_win, filters, 1, NULL, false);
        while (s_pick_state == 0) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) return false;
        }
        if (s_pick_state < 0) return false;
        const char *err = disc_import(s_pick, pak, draw_progress, NULL);
        if (!err) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Destrega",
                "Import complete. You can now delete or move the original disc image.", s_win);
            return true;
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Destrega", err, s_win);
    }
}

static void create_window(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    s_win = SDL_CreateWindow("Destrega (recompiled)", 960, 720, SDL_WINDOW_RESIZABLE);
    s_ren = SDL_CreateRenderer(s_win, NULL);
    SDL_SetRenderLogicalPresentation(s_ren, 640, 480, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
}

int main(int argc, char **argv) {
    const char *disc_path = NULL, *import_path = NULL, *data_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) s_headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) s_max_frames = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) s_dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) s_dump_every = atoi(argv[++i]);
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
        else if (!strcmp(argv[i], "--import") && i + 1 < argc) import_path = argv[++i];
        else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        else disc_path = argv[i];
    }

    /* Data pack location: SDL's per-user data folder (macOS: ~/Library/Application
     * Support/Destrega/, Linux: ~/.local/share/Destrega/, Windows: %APPDATA%\Destrega\). */
    char pak[2048];
    char *pref = data_dir ? NULL : SDL_GetPrefPath("", "Destrega");
    snprintf(pak, sizeof pak, "%s%sdestrega.pak", data_dir ? data_dir : (pref ? pref : "./"),
             data_dir ? "/" : "");
    SDL_free(pref);

    if (import_path) {
        const char *err = disc_import(import_path, pak, cli_progress, NULL);
        if (err) { fprintf(stderr, "import failed: %s\n", err); return 1; }
        fprintf(stderr, "data pack written to %s\n", pak);
        return 0;
    }
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

    bus_init();
    gte_init();
    gpu_init();
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
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (s_audio) SDL_ResumeAudioStreamDevice(s_audio);
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
