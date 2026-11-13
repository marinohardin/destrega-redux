/* Splash caption over the KOEI logo movie (splash.h). Presentation only.
 *
 * When: the first STR movie (the KOEI logo, then "Produced by Omega Force") is streamed from
 * LBA 152757 (MSF 33:58:57, ReadS at ~frame 171 after power-on) up to the intro movie at
 * LBA 155253, at ~2.2 sectors per vblank (verified: --trace CD log and dumps every 30 frames of
 * the US disc, the only one the runtime accepts). The progress through the movie is the drive's
 * read position, so the caption follows the movie whatever the frame rate or a skip: "KOEI
 * PRESENTS" is complete on screen once ~1010 sectors of the movie have been read, the logo has
 * faded out by ~1580 (checked with test screenshots, menu-test shot=, every 30-60 frames).
 * The caption fades in after the lettering and out with the logo. */
#include "splash.h"
#include "splash_text.h"
#include "psx.h"
#include <stdlib.h>
#include <zlib.h>

#define LOGO_LBA0 152757u
#define LOGO_LBA1 155253u
#define IN0 1010u               /* sectors read into the movie (the drive reads ahead of the */
#define IN1 1120u               /* picture by ~0.3 s): fade in once "KOEI PRESENTS" is complete */
#define OUT0 1490u              /* fade out with the logo */
#define OUT1 1580u

static bool s_on = true;
static SDL_Texture *s_tex;
static SDL_Renderer *s_tex_ren;
static unsigned char *s_rgba;   /* decoded once */
static bool s_failed;

void splash_set_enabled(bool on) { s_on = on; }

static float splash_alpha(void) {
    u32 lba = cdrom_stream_lba();
    if (lba < LOGO_LBA0 || lba >= LOGO_LBA1) return 0;
    u32 o = lba - LOGO_LBA0;
    if (o < IN0 || o >= OUT1) return 0;
    if (o < IN1) return (float)(o - IN0) / (IN1 - IN0);
    if (o >= OUT0) return 1.0f - (float)(o - OUT0) / (OUT1 - OUT0);
    return 1;
}

static bool decode(void) {
    if (s_rgba) return true;
    if (s_failed) return false;
    size_t n = (size_t)SPLASH_TEXT_W * SPLASH_TEXT_H;
    unsigned char *planes = malloc(2 * n), *rgba = malloc(4 * n);
    uLongf len = (uLongf)(2 * n);
    if (!planes || !rgba || uncompress(planes, &len, k_splash_text_z, SPLASH_TEXT_ZLEN) != Z_OK || len != 2 * n) {
        free(planes); free(rgba);
        s_failed = true;
        rt_log("splash: cannot decode the caption");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = planes[i];
        rgba[4 * i + 3] = planes[n + i];
    }
    free(planes);
    s_rgba = rgba;
    return true;
}

static SDL_Texture *make_texture(SDL_Renderer *r) {
    if (!decode()) return NULL;
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                       SPLASH_TEXT_W, SPLASH_TEXT_H);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, s_rgba, SPLASH_TEXT_W * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

void splash_release(void) {
    if (s_tex) SDL_DestroyTexture(s_tex);
    s_tex = NULL;
    s_tex_ren = NULL;
}

void splash_draw(SDL_Renderer *r, const SDL_FRect *pic, bool persistent) {
    float a = s_on ? splash_alpha() : 0;
    if (a <= 0 || !r) return;
    SDL_Texture *t;
    if (persistent) {
        if (s_tex_ren != r) { splash_release(); s_tex = make_texture(r); s_tex_ren = r; }
        t = s_tex;
    } else t = make_texture(r);
    if (!t) return;
    /* bottom half of the 4:3 picture, below "KOEI PRESENTS" (which ends at ~68 % of the height) */
    float w = pic->w * 0.84f, h = w * SPLASH_TEXT_H / SPLASH_TEXT_W;
    SDL_FRect dst = { pic->x + (pic->w - w) / 2, pic->y + pic->h * 0.73f, w, h };
    SDL_SetTextureAlphaModFloat(t, a);
    SDL_RenderTexture(r, t, NULL, &dst);
    if (!persistent) SDL_DestroyTexture(t);
}
