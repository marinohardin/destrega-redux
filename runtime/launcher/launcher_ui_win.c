/* Windows progress window of the launcher (launcher.h): a small native window owned by the
 * game window, with a progress bar (common controls) and Cancel. Its messages are dispatched by
 * SDL_PumpEvents (SDL's message loop serves every window of the thread). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include "launcher.h"

struct LauncherUI {
    HWND wnd, bar, step;
    HFONT font, bold;
    bool cancelled;
    wchar_t last[160];
};
static LauncherUI *s_ui;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (s_ui && ((m == WM_COMMAND && LOWORD(w) == IDCANCEL) || m == WM_CLOSE)) { s_ui->cancelled = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}

LauncherUI *lui_open(void *sdl_window)
{
    LauncherUI *ui = calloc(1, sizeof *ui);
    s_ui = ui;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DestregaLauncher";
    RegisterClassW(&wc);
    HWND owner = sdl_window ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window),
                                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL)
                            : NULL;
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ui->font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    ui->bold = CreateFontIndirectW(&ncm.lfMessageFont);
    UINT dpi = GetDpiForSystem();
#define S(v) MulDiv((v), (int)dpi, 96)
    RECT r = {0, 0, S(440), S(150)};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT o;
    if (owner && GetWindowRect(owner, &o)) { r.left = (o.left + o.right - ww) / 2; r.top = (o.top + o.bottom - wh) / 2; }
    else { r.left = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2; r.top = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2; }
    ui->wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DestregaLauncher", L"Destrega", style, r.left, r.top, ww, wh,
                              owner, NULL, inst, NULL);
    HWND title = CreateWindowExW(0, L"STATIC", L"Preparing the game for this computer…", WS_CHILD | WS_VISIBLE,
                                 S(20), S(16), S(400), S(20), ui->wnd, NULL, inst, NULL);
    ui->step = CreateWindowExW(0, L"STATIC", L"This happens once and takes a minute or two.", WS_CHILD | WS_VISIBLE,
                               S(20), S(40), S(400), S(18), ui->wnd, NULL, inst, NULL);
    ui->bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, S(20), S(68), S(400), S(20), ui->wnd,
                              NULL, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  S(330), S(106), S(90), S(28), ui->wnd, (HMENU)(INT_PTR)IDCANCEL, inst, NULL);
#undef S
    SendMessageW(title, WM_SETFONT, (WPARAM)ui->bold, TRUE);
    SendMessageW(ui->step, WM_SETFONT, (WPARAM)ui->font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)ui->font, TRUE);
    SendMessageW(ui->bar, PBM_SETRANGE32, 0, 1000);
    ShowWindow(ui->wnd, SW_SHOW);
    UpdateWindow(ui->wnd);
    return ui;
}

void lui_update(LauncherUI *ui, double frac, const char *step)
{
    if (!ui) return;
    SendMessageW(ui->bar, PBM_SETPOS, (WPARAM)(int)(frac * 1000), 0);
    if (step && *step) {
        wchar_t w[160];
        if (MultiByteToWideChar(CP_UTF8, 0, step, -1, w, 160) && wcscmp(w, ui->last)) {
            wcscpy(ui->last, w);
            SetWindowTextW(ui->step, w);
        }
    }
}

bool lui_cancelled(LauncherUI *ui)
{
    if (!ui) return false;
    SDL_PumpEvents();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) ui->cancelled = true;
    return ui->cancelled;
}

void lui_close(LauncherUI *ui)
{
    if (!ui) return;
    DestroyWindow(ui->wnd);
    DeleteObject(ui->font);
    DeleteObject(ui->bold);
    s_ui = NULL;
    free(ui);
}
