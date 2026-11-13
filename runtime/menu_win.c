/* Windows menu bar on the SDL window: File (game image, game files, save games), View
 * (widescreen, renderer, full screen) and Netplay (host, join, disconnect, status), with the same
 * items and rules as the macOS menus (menu_mac.m).
 *
 * The bar is attached to SDL's HWND (SDL_PROP_WINDOW_WIN32_HWND_POINTER) and the window procedure
 * is subclassed for WM_COMMAND (an item was chosen) and WM_INITMENUPOPUP (a menu is about to open:
 * check marks, titles, greyed-out items and the status line follow the current state, like
 * -validateMenuItem: on the Mac). Every item only queues a named action (frontend_menu_action),
 * which main.c runs between frames, so nothing here touches the emulated machine.
 *
 * Dialogs are plain Win32 windows built here (no resource script): a modal form for Host Game /
 * Join Game (values remembered in settings.ini [netplay]; read once from the registry key
 * HKCU\Software\Destrega\Netplay that earlier versions used) and a small modeless panel with
 * Cancel while the host waits or the joiner connects. Alerts are SDL message boxes (main.c). The
 * windows run on SDL's own message pump (SDL_PumpEvents dispatches every window of the thread);
 * the modal form runs its own loop while open, so the game pauses there, as with the Mac alert. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"
#include "netplay_ui.h"
#include "platform.h"
#include "widescreen.h"
#include "settings.h"

void rt_log(const char *fmt, ...);

/* ---- UTF-8 <-> UTF-16 --------------------------------------------------------------------- */

static void to_w(const char *s, wchar_t *out, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
}
static void from_w(const wchar_t *s, char *out, int n) {
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL)) out[0] = 0;
}

/* ---- the menu bar -------------------------------------------------------------------------- */

enum { ID_FIRST = 0x7100 };
typedef struct { UINT id; const char *action; } Item;   /* action: "name" or "name=arg" */
static Item s_items[48];
static int s_nitems;
static HMENU s_bar;
static HWND s_hwnd;
static WNDPROC s_sdl_proc;
static int s_auto_dialogs;    /* test hook DESTREGA_MENU_AUTOTEST: 1 accepts the Host / Join form by itself after
                               * 0.7 s, 2 cancels it after 3 s (time to take a screenshot) */

static void netplay_dialog(bool host);

static UINT add(HMENU m, const char *title, const char *action) {
    wchar_t w[128];
    to_w(title, w, 128);
    UINT id = ID_FIRST + (UINT)s_nitems;
    if (s_nitems < (int)(sizeof s_items / sizeof *s_items)) s_items[s_nitems++] = (Item){ id, action };
    AppendMenuW(m, MF_STRING, id, w);
    return id;
}
static void add_sub(HMENU m, const char *title, HMENU sub) {
    wchar_t w[128];
    to_w(title, w, 128);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, w);
}
static const Item *item_by_id(UINT id) {
    for (int i = 0; i < s_nitems; i++) if (s_items[i].id == id) return &s_items[i];
    return NULL;
}

static void fire(const char *action) {
    if (!strcmp(action, "netplay-host-dialog") || !strcmp(action, "netplay-join-dialog")) {
        netplay_dialog(!strcmp(action, "netplay-host-dialog"));
        return;
    }
    if (!strcmp(action, "exit-window")) {   /* as the close box: SDL turns it into a quit */
        PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
        return;
    }
    char name[64];
    const char *eq = strchr(action, '=');
    if (!eq) { frontend_menu_action(action, NULL); return; }
    snprintf(name, sizeof name, "%.*s", (int)(eq - action), action);
    frontend_menu_action(name, eq + 1);
}

static void set_text(UINT id, const char *text) {
    wchar_t w[256];
    to_w(text, w, 256);
    MENUITEMINFOW mi = { .cbSize = sizeof mi, .fMask = MIIM_STRING, .dwTypeData = w };
    SetMenuItemInfoW(s_bar, id, FALSE, &mi);
}

/* check marks, titles and greyed-out items follow the state (as validateMenuItem: on the Mac) */
static void update_items(void) {
    for (int i = 0; i < s_nitems; i++) {
        const Item *it = &s_items[i];
        char name[64];
        const char *eq = strchr(it->action, '=');
        snprintf(name, sizeof name, "%.*s", eq ? (int)(eq - it->action) : (int)strlen(it->action), it->action);
        size_t l = strlen(name);
        if (l > 7 && !strcmp(name + l - 7, "-dialog")) name[l - 7] = 0;
        bool enabled = true, checked = false;
        if (!strcmp(name, "netplay-copy-address")) set_text(it->id, netplay_ui_copy_title());
        if (!strcmp(name, "netplay-status")) {
            set_text(it->id, netplay_status_text());
            enabled = false;
        } else if (!frontend_menu_action_allowed(name)) {
            enabled = false;
        }
        if (!strcmp(name, "fullscreen"))
            set_text(it->id, frontend_fullscreen() ? "Exit Full Screen\tAlt+Enter" : "Enter Full Screen\tAlt+Enter");
        if (!strcmp(name, "widescreen")) checked = frontend_widescreen();
        frontend_view_checked(it->action, &checked);
        if (!strncmp(name, "hlr", 3) && !frontend_hlr_available()) enabled = false;   /* GPU renderer only */
        if (!strcmp(name, "renderer") && eq) {
            checked = !strcmp(eq + 1, frontend_renderer());
            if (!frontend_renderer_available(eq + 1)) enabled = false;   /* not on this machine */
        }
        EnableMenuItem(s_bar, it->id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(s_bar, it->id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }
}

static LRESULT CALLBACK win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITMENUPOPUP) update_items();
    if (msg == WM_COMMAND && HIWORD(wp) == 0 && lp == 0) {   /* from a menu */
        const Item *it = item_by_id(LOWORD(wp));
        if (it) { fire(it->action); return 0; }
    }
    return CallWindowProcW(s_sdl_proc, h, msg, wp, lp);
}

void menu_init(void) {
    int n = 0;
    SDL_Window **ws = SDL_GetWindows(&n);
    SDL_Window *win = ws && n > 0 ? ws[0] : NULL;
    SDL_free(ws);
    if (!win) return;
    HWND h = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!h) return;
    const char *t = getenv("DESTREGA_MENU_AUTOTEST");
    s_auto_dialogs = t ? atoi(t) : 0;

    s_bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    add(file, "Load Game Image…", "load");
    add(file, "Show Game Files in Explorer", "reveal-game");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    /* Save Games: one slot in the UI (the runtime still has two; see main.c) */
    HMENU saves = CreatePopupMenu();
    add(saves, "Show Save Games in Explorer", "reveal-saves");
    AppendMenuW(saves, MF_SEPARATOR, 0, NULL);
    add(saves, "Import Save Games…", "save-import");
    add(saves, "Export Save Games…", "save-export");
    add(saves, "Delete Save Game…", "save-delete");
    add_sub(file, "Save Games", saves);
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    add(file, "Exit", "exit-window");
    add_sub(s_bar, "File", file);

    HMENU view = CreatePopupMenu();
    add(view, "Widescreen (16:9)\tF9", "widescreen");
    HMENU ren = CreatePopupMenu();
    {   /* Direct3D 12 / Vulkan / Software: radio-style checks; an API switch rebuilds the renderer */
        static char acts[4][24];
        const char *gn[4], *gt[4];
        const int ng = frontend_graphics_apis(gn, gt, 4);
        for (int i = 0; i < ng; i++) {
            snprintf(acts[i], sizeof acts[i], "renderer=%s", gn[i]);
            add(ren, gt[i], acts[i]);
        }
        AppendMenuW(ren, MF_SEPARATOR, 0, NULL);
    }
    add(ren, "Software", "renderer=software");
    AppendMenuW(ren, MF_SEPARATOR, 0, NULL);   /* GPU precision (radio pair, presentation only) */
    add(ren, "Exact", "gpu-precision=exact");
    add(ren, "Fast (Approximate)", "gpu-precision=fast");
    add_sub(view, "Renderer", ren);
    HMENU res = CreatePopupMenu();   /* Auto: governed 4x6; the fixed scales are never changed */
    add(res, "Auto", "resolution=auto");
    AppendMenuW(res, MF_SEPARATOR, 0, NULL);
    add(res, "4×6 (Sharpest)", "resolution=4x6");
    add(res, "2×3 (Balanced)", "resolution=2x3");
    add(res, "1×1 (Original)", "resolution=1x1");
    add_sub(view, "Resolution", res);
    HMENU tf = CreatePopupMenu();   /* render quality: presentation only, reseeds like a resize */
    add(tf, "Off (Sharp Pixels)", "tex-filter=nearest");
    add(tf, "Bilinear", "tex-filter=bilinear");
    add(tf, "Smooth 2D Edges + Bilinear 3D", "tex-filter=xbr");
    AppendMenuW(tf, MF_SEPARATOR, 0, NULL);   /* texture cache of the Fast precision (check mark) */
    add(tf, "Smooth Distant Textures (Fast Renderer)", "tex-cache");
    add_sub(view, "Texture Filtering", tf);
    add(view, "Smooth Colours", "smooth-colours");
    HMENU aa = CreatePopupMenu();
    add(aa, "Off", "ssaa=1");
    add(aa, "2x", "ssaa=2");
    add(aa, "3x", "ssaa=3");
    add(aa, "4x", "ssaa=4");
    add_sub(view, "Anti-aliasing", aa);
    HMENU scene = CreatePopupMenu();   /* the native high-level renderer (runtime/hlr/); GPU only */
    add(scene, "Original", "hlr=off");
    add(scene, "Native", "hlr=on");
    AppendMenuW(scene, MF_SEPARATOR, 0, NULL);
    add(scene, "Stage", "hlr-part=stage");
    add(scene, "Characters", "hlr-part=chars");
    add(scene, "Smooth Character Motion", "hlr-bones");
    AppendMenuW(scene, MF_SEPARATOR, 0, NULL);
    add(scene, "Enhanced Look", "hlr-look=enhanced");
    add(scene, "PS1 Look", "hlr-look=ps1");
    AppendMenuW(scene, MF_SEPARATOR, 0, NULL);
    add(scene, "Clip Near Polygons", "hlr-near=clip");
    add(scene, "Drop Near Polygons (PS1)", "hlr-near=ps1");
    AppendMenuW(scene, MF_SEPARATOR, 0, NULL);
    add(scene, "Edge Smoothing Off", "hlr-msaa=1");
    add(scene, "Edge Smoothing 2x", "hlr-msaa=2");
    add(scene, "Edge Smoothing 4x", "hlr-msaa=4");
    add(scene, "Edge Smoothing 8x", "hlr-msaa=8");
    add_sub(view, "3D Scene", scene);
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    add(view, "Enter Full Screen\tAlt+Enter", "fullscreen");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    add(view, "Controls…", "controls");
    add_sub(s_bar, "View", view);

    HMENU np = CreatePopupMenu();
    add(np, "Host Game…", "netplay-host-dialog");
    add(np, "Join Game…", "netplay-join-dialog");
    add(np, "Copy My Address", "netplay-copy-address");   /* while hosting; title follows netplay_ui.c */
    AppendMenuW(np, MF_SEPARATOR, 0, NULL);
    add(np, "Disconnect", "netplay-disconnect");
    AppendMenuW(np, MF_SEPARATOR, 0, NULL);
    add(np, "Not connected", "netplay-status");
    add(np, "Copy Out-of-Sync Report Location", "netplay-copy-report");   /* after a desync */
    add_sub(s_bar, "Netplay", np);

    /* keep the client area (the game picture) the same size: SDL sizes the window by its client
     * area and accounts for a menu bar when it adjusts the frame */
    int cw = 0, ch = 0;
    SDL_GetWindowSize(win, &cw, &ch);
    s_hwnd = h;
    s_sdl_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)win_proc);
    SetMenu(h, s_bar);
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(win, cw, ch);
    update_items();
    rt_log("menu: Windows menu bar attached");
}

/* ---- small Win32 windows: font, scaling --------------------------------------------------- */

static HFONT s_font;
static int s_dpi = 96;
static int px(int v) { return MulDiv(v, s_dpi, 96); }

static void ui_init(void) {
    if (s_font) return;
    HDC dc = GetDC(s_hwnd);
    if (dc) { s_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(s_hwnd, dc); }
    NONCLIENTMETRICSW m = { .cbSize = sizeof m };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0)) s_font = CreateFontIndirectW(&m.lfMessageFont);
    if (!s_font) s_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static HWND child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             px(x), px(y), px(w), px(h), parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

/* height of `text` wrapped to `w` (unscaled) pixels */
static int text_height(const wchar_t *text, int w) {
    HDC dc = GetDC(s_hwnd);
    HGDIOBJ old = SelectObject(dc, s_font);
    RECT r = { 0, 0, px(w), 0 };
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(s_hwnd, dc);
    return MulDiv(r.bottom, 96, s_dpi) + 4;
}

static void center_on_game(HWND w) {
    RECT p, r;
    GetWindowRect(s_hwnd, &p);
    GetWindowRect(w, &r);
    int x = p.left + ((p.right - p.left) - (r.right - r.left)) / 2;
    int y = p.top + ((p.bottom - p.top) - (r.bottom - r.top)) / 3;
    SetWindowPos(w, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void register_class(const wchar_t *name, WNDPROC proc) {
    WNDCLASSW c = { 0 };
    if (GetClassInfoW(GetModuleHandleW(NULL), name, &c)) return;
    c.lpfnWndProc = proc;
    c.hInstance = GetModuleHandleW(NULL);
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    c.lpszClassName = name;
    RegisterClassW(&c);
}

/* ---- Host Game / Join Game form ------------------------------------------------------------ */

static int s_form_result;   /* 0 open, 1 OK, -1 cancel */

/* Join Game's Paste button: the clipboard's text into the address field, tidied (spaces, line
 * breaks, quotes) when it parses as an address; Ctrl+V in the field works as well. */
static void form_paste(HWND form) {
    if (!OpenClipboard(form)) return;
    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = hd ? (const wchar_t *)GlobalLock(hd) : NULL;
    char t[512] = "";
    if (w) { from_w(w, t, sizeof t); GlobalUnlock(hd); }
    CloseClipboard();
    if (!*t) return;
    char host[256], err[300], out[300];
    int port = 0;
    if (netplay_ui_parse_address(t, 0, host, sizeof host, &port, err, sizeof err)) snprintf(out, sizeof out, "%s:%d", host, port);
    else snprintf(out, sizeof out, "%s", t);
    wchar_t wo[300];
    to_w(out, wo, 300);
    SetWindowTextW(GetDlgItem(form, 101), wo);
}

static LRESULT CALLBACK form_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == 104) { form_paste(h); return 0; }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        s_form_result = LOWORD(wp) == IDOK ? 1 : -1;
        return 0;
    }
    if (msg == WM_TIMER) { s_form_result = s_auto_dialogs == 1 ? 1 : -1; return 0; }   /* DESTREGA_MENU_AUTOTEST */
    if (msg == WM_CLOSE) { s_form_result = -1; return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* Modal form: title, explanation, two labelled fields (values in/out, UTF-8), an optional Paste
 * button for the first field, an optional check box (check = its label, *checked in/out), OK button
 * text. Returns true on OK. */
static bool form_run(const char *title, const char *text, const char *l1, char *v1, int w1, bool paste,
                     const char *l2, char *v2, const char *check, bool *checked, const char *ok) {
    ui_init();
    register_class(L"DestregaForm", form_proc);
    wchar_t wt[128], wx[1024], wl1[64], wl2[64], wv1[256], wv2[64], wok[32], wck[128];
    to_w(title, wt, 128); to_w(text, wx, 1024); to_w(l1, wl1, 64); to_w(l2, wl2, 64);
    to_w(check ? check : "", wck, 128);
    to_w(v1, wv1, 256); to_w(v2, wv2, 64); to_w(ok, wok, 32);
    const int W = 440, M = 14;
    int th = text_height(wx, W - 2 * M);
    int y = M + th + 10;
    int H = y + 30 + 30 + 46 + (check ? 28 : 0);
    RECT r = { 0, 0, px(W), px(H) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&r, style, FALSE, ex);
    HWND f = CreateWindowExW(ex, L"DestregaForm", wt, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, s_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!f) return false;
    child(f, L"STATIC", wx, SS_LEFT | SS_NOPREFIX, M, M, W - 2 * M, th, 0);
    child(f, L"STATIC", wl1, SS_LEFT, M, y + 3, 110, 20, 0);
    HWND e1 = child(f, L"EDIT", wv1, WS_TABSTOP | ES_AUTOHSCROLL, M + 115, y, w1, 24, 101);
    if (paste) child(f, L"BUTTON", L"Paste", WS_TABSTOP | BS_PUSHBUTTON, M + 115 + w1 + 6, y - 1, 70, 26, 104);
    y += 30;
    child(f, L"STATIC", wl2, SS_LEFT, M, y + 3, 110, 20, 0);
    HWND e2 = child(f, L"EDIT", wv2, WS_TABSTOP | ES_NUMBER, M + 115, y, 50, 24, 102);
    HWND ck = NULL;
    if (check) {
        y += 32;
        ck = child(f, L"BUTTON", wck, WS_TABSTOP | BS_AUTOCHECKBOX, M, y, W - 2 * M, 22, 103);
        SendMessageW(ck, BM_SETCHECK, *checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    y += 40;
    child(f, L"BUTTON", wok, WS_TABSTOP | BS_DEFPUSHBUTTON, W - M - 2 * 96 - 8, y, 96, 28, IDOK);
    child(f, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, W - M - 96, y, 96, 28, IDCANCEL);
    center_on_game(f);
    EnableWindow(s_hwnd, FALSE);
    ShowWindow(f, SW_SHOW);
    SetFocus(e1);
    SendMessageW(e1, EM_SETSEL, 0, -1);
    if (s_auto_dialogs) SetTimer(f, 1, s_auto_dialogs == 1 ? 700 : 3000, NULL);
    s_form_result = 0;
    MSG m;
    while (!s_form_result && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); s_form_result = -1; break; }
        if (!IsDialogMessageW(f, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    GetWindowTextW(e1, wv1, 256);
    GetWindowTextW(e2, wv2, 64);
    if (ck) *checked = SendMessageW(ck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(s_hwnd, TRUE);
    DestroyWindow(f);
    SetForegroundWindow(s_hwnd);
    if (s_form_result != 1) return false;
    from_w(wv1, v1, 256);
    from_w(wv2, v2, 64);
    return true;
}

/* remembered values: settings.ini [netplay] port / join_address / delay. Earlier versions kept them
 * in HKCU\Software\Destrega\Netplay (Port, JoinAddress, Delay): copied over once if the file has
 * none (the registry values are left alone). */
static bool reg_read(const char *name, char *out, int n) {
    wchar_t wn[64], wv[256];
    DWORD sz = sizeof wv;
    to_w(name, wn, 64);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Destrega\\Netplay", wn, RRF_RT_REG_SZ, NULL, wv, &sz) != ERROR_SUCCESS) return false;
    from_w(wv, out, n);
    return true;
}
static void reg_get(const char *name, char *out, int n, const char *def) {
    static const char *const map[3][2] = { { "Port", "port" }, { "JoinAddress", "join_address" }, { "Delay", "delay" } };
    const char *key = name;
    for (int i = 0; i < 3; i++) if (!strcmp(name, map[i][0])) key = map[i][1];
    const char *v = settings_get("netplay", key);
    char old[256];
    if (!v && reg_read(name, old, sizeof old) && *old && settings_set("netplay", key, old)) {
        settings_save();
        v = settings_get("netplay", key);
    }
    snprintf(out, (size_t)n, "%s", v ? v : def);
}
static void reg_set(const char *name, const char *value) {
    static const char *const map[3][2] = { { "Port", "port" }, { "JoinAddress", "join_address" }, { "Delay", "delay" } };
    for (int i = 0; i < 3; i++) if (!strcmp(name, map[i][0])) settings_set("netplay", map[i][1], value);
    settings_save();
}

static void form_error(const char *title, const char *err) {
    rt_log("menu: %s: %s", title, err);
    if (s_auto_dialogs) return;   /* test hook: no click would come */
    wchar_t wt[64], we[512];
    to_w(title, wt, 64);
    to_w(err, we, 512);
    MessageBoxW(s_hwnd, we, wt, MB_OK | MB_ICONWARNING);
}

static void netplay_dialog(bool host) {
    char port[256], addr[256], delay[64], text[1024];
    reg_get("Port", port, sizeof port, "7000");
    reg_get("JoinAddress", addr, sizeof addr, "");
    reg_get("Delay", delay, sizeof delay, "2");
    bool portmap = settings_get_bool("netplay", "port_mapping", true);   /* portmap.c */
    const char *title = host ? "Host Game" : "Join Game";
    /* shown again with what was typed until the values are usable or the player cancels */
    for (;;) {
        bool ok;
        if (host) {
            char ip[256];
            plat_local_ipv4(ip, sizeof ip);
            snprintf(text, sizeof text,
                     "Your opponent chooses Join Game and types your address and port.\n\n"
                     "On your home network: %s\n\nOver the internet, the game asks your router to open the port and then "
                     "shows your internet address, with a Copy Address button; if the router can't, forward the UDP "
                     "port to this computer by hand. Both games restart from the beginning when your opponent connects.",
                     *ip ? ip : "no network address found");
            ok = form_run(title, text, "Port:", port, 90, false, "Input delay:", delay,
                          "Ask the router to open the port automatically", &portmap, "Host");
        } else {
            snprintf(text, sizeof text, "Type or paste the address your opponent gave you, for example 192.168.1.20:7000. "
                                        "Both games restart from the beginning when you connect.");
            ok = form_run(title, text, "Address:port:", addr, 200, true, "Input delay:", delay, NULL, NULL, "Join");
        }
        rt_log("menu: %s dialog %s", title, ok ? "accepted" : "cancelled");
        if (!ok) return;
        char err[400];
        int dl = 2, p = 7000;
        if (!netplay_ui_parse_delay(delay, &dl, err, sizeof err)) { form_error(title, err); if (s_auto_dialogs) return; continue; }
        char arg[400];
        if (host) {
            if (!netplay_ui_parse_port(port, &p, err, sizeof err)) { form_error(title, err); if (s_auto_dialogs) return; continue; }
            snprintf(delay, sizeof delay, "%d", dl);
            reg_set("Delay", delay);
            snprintf(port, sizeof port, "%d", p);
            reg_set("Port", port);
            settings_set_bool("netplay", "port_mapping", portmap);
            settings_save();
            snprintf(arg, sizeof arg, "%d,%d", p, dl);
            frontend_menu_action("netplay-host", arg);
        } else {
            char h[200], defp[256];
            reg_get("Port", defp, sizeof defp, "7000");
            int dp = atoi(defp);
            if (!netplay_ui_parse_address(addr, dp > 0 && dp <= 65535 ? dp : 7000, h, sizeof h, &p, err, sizeof err)) {
                form_error(title, err);
                if (s_auto_dialogs) return;
                continue;
            }
            snprintf(delay, sizeof delay, "%d", dl);
            reg_set("Delay", delay);
            snprintf(addr, sizeof addr, "%s:%d", h, p);
            reg_set("JoinAddress", addr);
            snprintf(arg, sizeof arg, "%s,%d", addr, dl);
            frontend_menu_action("netplay-join", arg);
        }
        return;
    }
}

/* ---- Netplay progress panel ("Waiting for an opponent..." + Cancel) ------------------------- */

static HWND s_panel, s_panel_label;

static HWND s_panel_copy;

static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == 201) { frontend_menu_action("netplay-copy-address", NULL); return 0; }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        frontend_menu_action("netplay-disconnect", NULL);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void menu_netplay_progress(const char *text, bool copy) {
    if (!s_hwnd) return;   /* no menus (headless) */
    if (!text) { if (s_panel) ShowWindow(s_panel, SW_HIDE); return; }
    ui_init();
    if (!s_panel) {
        register_class(L"DestregaPanel", panel_proc);
        const int W = 460, H = 220;   /* status, addresses, the router's answer (netplay_ui.c) */
        RECT r = { 0, 0, px(W), px(H) };
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOOLWINDOW;
        AdjustWindowRectEx(&r, style, FALSE, ex);
        s_panel = CreateWindowExW(ex, L"DestregaPanel", L"Netplay", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top, s_hwnd, NULL, GetModuleHandleW(NULL), NULL);
        if (!s_panel) return;
        s_panel_label = child(s_panel, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 16, 14, W - 32, H - 72, 0);
        s_panel_copy = child(s_panel, L"BUTTON", L"Copy Address", WS_TABSTOP | BS_PUSHBUTTON, 16, H - 16 - 28, 120, 28, 201);
        child(s_panel, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, W - 16 - 96, H - 16 - 28, 96, 28, IDCANCEL);
    }
    wchar_t w[1024];
    to_w(text, w, 1024);
    SetWindowTextW(s_panel_label, w);
    ShowWindow(s_panel_copy, copy ? SW_SHOWNA : SW_HIDE);
    if (!IsWindowVisible(s_panel)) center_on_game(s_panel);   /* updates leave it where it is */
    ShowWindow(s_panel, SW_SHOWNOACTIVATE);   /* the text is logged by netplay_ui.c */
}

/* ---- Explorer ------------------------------------------------------------------------------ */

void menu_reveal(const char *path) {
    wchar_t w[2048], args[2100];
    to_w(path, w, 2048);
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    } else {
        _snwprintf(args, 2100, L"/select,\"%ls\"", w);
        args[2099] = 0;
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
}

bool menu_executable_path(char *out, size_t n) { return plat_executable_path(out, n); }

/* ---- test hook (--menu-action F:menu-test=...) -------------------------------------------------
 * "dump": log every item with its enabled / checked state, after the same update a menu opening
 * does. "click=File/Save Games/Export Save Games…": post that item's WM_COMMAND to the window, as a
 * click does (handled by the subclassed window procedure on the next message pump). */

static void dump(HMENU m, const char *prefix) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        wchar_t w[256] = { 0 };
        MENUITEMINFOW mi = { .cbSize = sizeof mi, .fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_STATE | MIIM_FTYPE | MIIM_ID,
                             .dwTypeData = w, .cch = 255 };
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR)) continue;
        char t[256], path[512];
        from_w(w, t, sizeof t);
        char *tab = strchr(t, '\t');
        if (tab) *tab = 0;
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", t);
        if (mi.hSubMenu) { dump(mi.hSubMenu, path); continue; }
        rt_log("menu: item %s%s%s", path, (mi.fState & MFS_GRAYED) ? " [greyed]" : "", (mi.fState & MFS_CHECKED) ? " [checked]" : "");
    }
}

static bool find_path(HMENU m, const char *path, UINT *id) {
    const char *slash = strchr(path, '/');
    size_t k = slash ? (size_t)(slash - path) : strlen(path);
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        wchar_t w[256] = { 0 };
        MENUITEMINFOW mi = { .cbSize = sizeof mi, .fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID | MIIM_FTYPE,
                             .dwTypeData = w, .cch = 255 };
        if (!GetMenuItemInfoW(m, (UINT)i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR)) continue;
        char t[256];
        from_w(w, t, sizeof t);
        char *tab = strchr(t, '\t');
        if (tab) *tab = 0;
        if (strlen(t) != k || strncmp(t, path, k)) continue;
        if (slash) return mi.hSubMenu && find_path(mi.hSubMenu, slash + 1, id);
        *id = mi.wID;
        return !mi.hSubMenu;
    }
    return false;
}

bool menu_test(const char *cmd) {
    if (!s_bar) { rt_log("menu: test: no menu bar"); return false; }
    update_items();
    if (!strcmp(cmd, "dump")) { dump(s_bar, ""); return true; }
    if (!strncmp(cmd, "click=", 6)) {
        UINT id;
        if (!find_path(s_bar, cmd + 6, &id)) { rt_log("menu: test: no item %s", cmd + 6); return false; }
        UINT st = GetMenuState(s_bar, id, MF_BYCOMMAND);
        if (st & MF_GRAYED) { rt_log("menu: test: %s is greyed out", cmd + 6); return false; }
        rt_log("menu: test: click %s", cmd + 6);
        PostMessageW(s_hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        return true;
    }
    rt_log("menu: test: dump or click=PATH");
    return false;
}
#endif

bool menu_wait_until(unsigned long long deadline_ns) { (void)deadline_ns; return false; }
