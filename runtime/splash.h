/* "Destrega Redux / Marino's Build" caption over the KOEI logo movie. Presentation only: drawn by
 * the frontend on top of the presented picture, never into VRAM or the hi-res target; it reads the
 * CD drive's stream position (cdrom_stream_lba) and nothing else of the emulated machine. */
#ifndef SPLASH_H
#define SPLASH_H
#include <SDL3/SDL.h>
#include <stdbool.h>

void splash_set_enabled(bool on);        /* --no-splash */
/* Draw the caption into `pic` (the letterboxed 4:3 movie picture) if the logo movie is playing.
 * `persistent`: r is the window's renderer (the texture is kept until splash_release); else the
 * texture is made and freed in this call (test screenshots). */
void splash_draw(SDL_Renderer *r, const SDL_FRect *pic, bool persistent);
void splash_release(void);               /* before the window's renderer is destroyed */
#endif
