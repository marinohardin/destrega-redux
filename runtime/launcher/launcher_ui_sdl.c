/* Progress UI of the launcher drawn into the game window with SDL (Linux, and wherever there is
 * no native one): the same text, a bar, Esc or closing the window cancels. */
#include "launcher.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

struct LauncherUI {
    SDL_Window *win;
    SDL_Renderer *ren;
    bool cancelled;
};

LauncherUI *lui_open(void *sdl_window)
{
    LauncherUI *ui = calloc(1, sizeof *ui);
    ui->win = sdl_window;
    ui->ren = sdl_window ? SDL_GetRenderer(sdl_window) : NULL;
    return ui;
}

void lui_update(LauncherUI *ui, double frac, const char *step)
{
    if (!ui || !ui->ren) return;
    SDL_Renderer *r = ui->ren;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
    SDL_RenderDebugText(r, 120, 180, "Preparing the game for this computer...");
    SDL_SetRenderDrawColor(r, 160, 160, 160, 255);
    SDL_RenderDebugText(r, 120, 200, step && *step ? step : "This happens once and takes a minute or two.");
    SDL_SetRenderDrawColor(r, 90, 20, 20, 255);
    SDL_RenderRect(r, &(SDL_FRect){120, 228, 400, 24});
    SDL_SetRenderDrawColor(r, 200, 40, 40, 255);
    SDL_RenderFillRect(r, &(SDL_FRect){122, 230, 396 * (float)frac, 20});
    SDL_SetRenderDrawColor(r, 160, 160, 160, 255);
    SDL_RenderDebugText(r, 120, 270, "Press Esc to cancel.");
    SDL_RenderPresent(r);
}

bool lui_cancelled(LauncherUI *ui)
{
    if (!ui) return false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) ui->cancelled = true;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) ui->cancelled = true;
    }
    return ui->cancelled;
}

void lui_close(LauncherUI *ui) { free(ui); }
