/* Windows Controls window (View > Controls…): the same contents and rules as the Mac one
 * (controls_mac.m): per player the device and press-to-bind for every PS1 button, keyboard and
 * controller side by side, the left-stick option and its deadzone, Reset to Defaults. Changes apply
 * at once and are saved (input.c / settings.c).
 *
 * A plain modeless Win32 window built in code (no resource script), owned by the game window and
 * run by SDL's own message pump, so the game keeps running. While a key binding is armed the window
 * takes the keyboard focus and WM_KEYDOWN / WM_SYSKEYDOWN give the hardware scan code, mapped to an
 * SDL scancode (set 1 table below, positional like SDL's own). Controller bindings come from SDL's
 * gamepad events through the input layer (input_set_background keeps them flowing while this
 * window has the focus). A 100 ms timer refreshes labels, the device list and the "pressed" line. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "input.h"

void rt_log(const char *fmt, ...);

static int scancode_from_win(UINT sc, bool ext) {
    static const unsigned char base[0x59] = {
        0, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
        SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS,
        SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_W,
        SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
        SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_LCTRL, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G,
        SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
        SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_Z,
        SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M,
        SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY,
        SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2,
        SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
        SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_KP_7,
        SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
        SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
        SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD, 0, 0, SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,
        SDL_SCANCODE_F12 };
    if (ext) {
        switch (sc) {
        case 0x1c: return SDL_SCANCODE_KP_ENTER;  case 0x1d: return SDL_SCANCODE_RCTRL;
        case 0x35: return SDL_SCANCODE_KP_DIVIDE; case 0x38: return SDL_SCANCODE_RALT;
        case 0x47: return SDL_SCANCODE_HOME;      case 0x48: return SDL_SCANCODE_UP;
        case 0x49: return SDL_SCANCODE_PAGEUP;    case 0x4b: return SDL_SCANCODE_LEFT;
        case 0x4d: return SDL_SCANCODE_RIGHT;     case 0x4f: return SDL_SCANCODE_END;
        case 0x50: return SDL_SCANCODE_DOWN;      case 0x51: return SDL_SCANCODE_PAGEDOWN;
        case 0x52: return SDL_SCANCODE_INSERT;    case 0x53: return SDL_SCANCODE_DELETE;
        case 0x5b: return SDL_SCANCODE_LGUI;      case 0x5c: return SDL_SCANCODE_RGUI;
        case 0x5d: return SDL_SCANCODE_APPLICATION;
        }
        return 0;
    }
    if (sc == 0x36) return SDL_SCANCODE_RSHIFT;   /* (no E0 prefix, unlike right Ctrl / Alt) */
    return sc < sizeof base ? base[sc] : 0;
}

static void to_w(const char *s, wchar_t *out, int n) { if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0; }

enum { ID_P1 = 100, ID_P2, ID_DEVICE, ID_STICK, ID_DEADZONE, ID_RESET, ID_DONE, ID_STATUS, ID_BIND = 200 };
static HWND s_win, s_game, s_p1, s_p2, s_device, s_stick, s_dz, s_status, s_label[PSB_COUNT], s_btn[PSB_COUNT][2];
static HFONT s_font, s_bold;
static int s_dpi = 96;
static int s_player;
static char s_dev_rep[16][48];
static int s_ndev;
static char s_padkey[1024] = "?";   /* list of connected pads the device box was built for; "?" = rebuild (never a real list, which is "" or ends in ";") */
static int px(int v) { return MulDiv(v, s_dpi, 96); }

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), s_win,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static void set_text(HWND h, const char *s) {
    wchar_t w[512], old[512];
    to_w(s, w, 512);
    GetWindowTextW(h, old, 512);
    if (wcscmp(w, old)) SetWindowTextW(h, w);
}

static void rebuild_devices(void) {
    InputPlayer *p = input_player(s_player);
    SendMessageW(s_device, CB_RESETCONTENT, 0, 0);
    s_ndev = 0;
    char title[200];
#define ADD(T, R) do { if (s_ndev < 16) { wchar_t w_[200]; to_w(T, w_, 200); snprintf(s_dev_rep[s_ndev], sizeof s_dev_rep[0], "%s", R); \
        SendMessageW(s_device, CB_ADDSTRING, 0, (LPARAM)w_); s_ndev++; } } while (0)
    ADD(s_player == 0 ? "Automatic (keyboard and the first controller)" : "Automatic (the next free controller)", "auto");
    ADD("Keyboard", "keyboard");
    ADD("Any controller", "gamepad");
    bool have = false;
    for (int i = 0; i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (!input_pad_info(i, &pi)) continue;
        char rep[48];
        snprintf(rep, sizeof rep, "gamepad:%s", pi.guid);
        bool dup = false;
        for (int k = 0; k < s_ndev; k++) if (!strcmp(s_dev_rep[k], rep)) dup = true;
        if (dup) continue;
        snprintf(title, sizeof title, "Controller: %s", pi.name);
        ADD(title, rep);
        if (p->dev.kind == INDEV_GAMEPAD_GUID && !_stricmp(pi.guid, p->dev.guid)) have = true;
    }
    if (p->dev.kind == INDEV_GAMEPAD_GUID && !have) {
        char rep[48];
        snprintf(rep, sizeof rep, "gamepad:%s", p->dev.guid);
        snprintf(title, sizeof title, "Controller: %s (not connected)", p->dev.name[0] ? p->dev.name : "unknown");
        ADD(title, rep);
    }
    ADD("None", "none");
#undef ADD
    char id[64];
    input_device_id(&p->dev, id, sizeof id);
    for (int k = 0; k < s_ndev; k++) if (!_stricmp(s_dev_rep[k], id)) SendMessageW(s_device, CB_SETCURSEL, k, 0);
}

static void refresh(void) {
    InputPlayer *p = input_player(s_player);
    char key[1024] = "";
    for (int i = 0; i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (input_pad_info(i, &pi)) snprintf(key + strlen(key), sizeof key - strlen(key), "%s|%d;", pi.guid, pi.player);
    }
    if (strcmp(key, s_padkey)) { snprintf(s_padkey, sizeof s_padkey, "%s", key); rebuild_devices(); }
    int cp, cb, ck;
    bool armed = input_capture_armed(&cp, &cb, &ck);
    int type = input_player_pad_type(s_player);
    uint16_t pressed = input_last_mask(s_player);
    for (int b = 0; b < PSB_COUNT; b++) {
        char k[64], g[64], l[64];
        input_key_label(p->key[b], k, sizeof k);
        input_padbind_label(p->pad[b], type, g, sizeof g);
        set_text(s_btn[b][0], armed && cp == s_player && cb == b && ck == 0 ? "Press a key…" : k);
        set_text(s_btn[b][1], armed && cp == s_player && cb == b && ck == 1 ? "Press a button…" : g);
        snprintf(l, sizeof l, "%s%s", input_button_label(b), (pressed & input_button_mask(b)) ? "   ●" : "");
        set_text(s_label[b], l);
    }
    SendMessageW(s_stick, BM_SETCHECK, p->stick ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_dz, CB_SETCURSEL, (p->deadzone - 5) / 5, 0);
    const char *msg = input_capture_message();
    if (armed || *msg) set_text(s_status, msg);
    else {
        char s[400];
        int pad = -1;
        InputPadInfo pi = { 0 };
        for (int i = 0; i < input_pad_count(); i++) { InputPadInfo q; if (input_pad_info(i, &q) && q.player == s_player) { pad = i; pi = q; } }
        bool kb = input_player_has_keyboard(s_player);
        snprintf(s, sizeof s, "Player %d reads %s%s%s. Pressed buttons are marked. During netplay you play with Player 1's controls.",
                 s_player + 1, kb ? "the keyboard" : "", kb && pad >= 0 ? " and " : "",
                 pad >= 0 ? pi.name : kb ? "" : "nothing (no controller for it is connected)");
        set_text(s_status, s);
    }
    SendMessageW(s_p1, BM_SETCHECK, s_player == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s_p2, BM_SETCHECK, s_player == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void close_window(void) {
    input_capture_cancel();
    input_set_background(false);
    KillTimer(s_win, 1);
    ShowWindow(s_win, SW_HIDE);
    if (s_game) SetForegroundWindow(s_game);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: refresh(); return 0;
    case WM_CLOSE: close_window(); return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT sc = (UINT)(lp >> 16) & 0xff;
        bool ext = (lp >> 24) & 1, repeat = (lp >> 30) & 1;
        if (input_capture_armed(NULL, NULL, NULL)) {
            if (!repeat) { int s = scancode_from_win(sc, ext); if (s) input_capture_key(s); }
            refresh();
            return 0;   /* also keeps Alt / F10 from opening the menu */
        }
        if (wp == VK_ESCAPE) { close_window(); return 0; }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        InputPlayer *p = input_player(s_player);
        if (id == ID_P1 || id == ID_P2) { input_capture_cancel(); s_player = id - ID_P1; strcpy(s_padkey, "?"); refresh(); }
        else if (id == ID_DEVICE && code == CBN_SELCHANGE) {
            int k = (int)SendMessageW(s_device, CB_GETCURSEL, 0, 0);
            InputDevice d;
            if (k >= 0 && k < s_ndev && input_device_from_id(s_dev_rep[k], &d)) {
                for (int i = 0; d.kind == INDEV_GAMEPAD_GUID && i < input_pad_count(); i++) {
                    InputPadInfo pi;
                    if (input_pad_info(i, &pi) && !_stricmp(pi.guid, d.guid)) snprintf(d.name, sizeof d.name, "%s", pi.name);
                }
                if (d.kind == INDEV_GAMEPAD_GUID && !d.name[0]) snprintf(d.name, sizeof d.name, "%s", p->dev.name);
                p->dev = d;
                input_changed(s_player);
                strcpy(s_padkey, "?");
                refresh();
            }
        } else if (id == ID_STICK) {
            p->stick = SendMessageW(s_stick, BM_GETCHECK, 0, 0) == BST_CHECKED;
            input_changed(s_player);
        } else if (id == ID_DEADZONE && code == CBN_SELCHANGE) {
            int k = (int)SendMessageW(s_dz, CB_GETCURSEL, 0, 0);
            if (k >= 0) { p->deadzone = 5 + 5 * k; input_changed(s_player); }
        } else if (id == ID_RESET) {
            wchar_t q[200];
            _snwprintf(q, 200, L"Reset Player %d's controls to the defaults?\n\nThe device goes back to Automatic and every key and controller button to its default.", s_player + 1);
            q[199] = 0;
            if (MessageBoxW(h, q, L"Controls", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                input_capture_cancel();
                input_reset(s_player);
                strcpy(s_padkey, "?");
                refresh();
            }
        } else if (id == ID_DONE) close_window();
        else if (id >= ID_BIND && id < ID_BIND + 2 * PSB_COUNT) {
            int b = (id - ID_BIND) / 2, kind = (id - ID_BIND) % 2;
            int cp, cb, ck;
            if (input_capture_armed(&cp, &cb, &ck) && cp == s_player && cb == b && ck == kind) input_capture_cancel();
            else { input_capture_begin(s_player, b, kind); SetFocus(h); }   /* keys come to this window */
            refresh();
        }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void build(void) {
    int n = 0;
    SDL_Window **ws = SDL_GetWindows(&n);
    SDL_Window *win = ws && n > 0 ? ws[0] : NULL;
    SDL_free(ws);
    s_game = win ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
    HDC dc = GetDC(s_game);
    if (dc) { s_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(s_game, dc); }
    NONCLIENTMETRICSW m = { .cbSize = sizeof m };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0)) {
        s_font = CreateFontIndirectW(&m.lfMessageFont);
        m.lfMessageFont.lfWeight = FW_BOLD;
        s_bold = CreateFontIndirectW(&m.lfMessageFont);
    }
    if (!s_font) s_font = s_bold = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    WNDCLASSW c = { 0 };
    c.lpfnWndProc = proc;
    c.hInstance = GetModuleHandleW(NULL);
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    c.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    c.lpszClassName = L"DestregaControls";
    RegisterClassW(&c);
    const int W = 520, rowH = 27;
    const int H = 112 + PSB_COUNT * rowH + 130;
    RECT r = { 0, 0, px(W), px(H) };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    s_win = CreateWindowExW(0, L"DestregaControls", L"Controls", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, s_game, NULL, GetModuleHandleW(NULL), NULL);
    s_p1 = child(L"BUTTON", L"Player 1", BS_AUTORADIOBUTTON | WS_GROUP, 20, 14, 90, 22, ID_P1);
    s_p2 = child(L"BUTTON", L"Player 2", BS_AUTORADIOBUTTON, 120, 14, 90, 22, ID_P2);
    child(L"STATIC", L"Device:", SS_LEFT, 20, 50, 60, 20, 0);
    s_device = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 80, 46, W - 100, 300, ID_DEVICE);
    int y = 84;
    HWND hb = child(L"STATIC", L"Button", SS_LEFT, 20, y, 120, 20, 0);
    HWND hk = child(L"STATIC", L"Keyboard", SS_LEFT, 150, y, 170, 20, 0);
    HWND hc = child(L"STATIC", L"Controller", SS_LEFT, 330, y, 170, 20, 0);
    SendMessageW(hb, WM_SETFONT, (WPARAM)s_bold, TRUE);
    SendMessageW(hk, WM_SETFONT, (WPARAM)s_bold, TRUE);
    SendMessageW(hc, WM_SETFONT, (WPARAM)s_bold, TRUE);
    y += 24;
    for (int b = 0; b < PSB_COUNT; b++, y += rowH) {
        wchar_t w[64];
        to_w(input_button_label(b), w, 64);
        s_label[b] = child(L"STATIC", w, SS_LEFT, 20, y + 4, 120, 20, 0);
        for (int k = 0; k < 2; k++)
            s_btn[b][k] = child(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, k ? 330 : 150, y, 170, 25, ID_BIND + b * 2 + k);
    }
    y += 8;
    s_stick = child(L"BUTTON", L"Left stick moves too", BS_AUTOCHECKBOX | WS_TABSTOP, 20, y, 200, 22, ID_STICK);
    child(L"STATIC", L"Stick deadzone:", SS_LEFT, 240, y + 3, 105, 20, 0);
    s_dz = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 350, y, 90, 300, ID_DEADZONE);
    for (int d = 5; d <= 95; d += 5) {
        wchar_t w[16];
        _snwprintf(w, 16, L"%d %%", d);
        w[15] = 0;
        SendMessageW(s_dz, CB_ADDSTRING, 0, (LPARAM)w);
    }
    y += 32;
    s_status = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 20, y, W - 40, 36, ID_STATUS);
    child(L"BUTTON", L"Reset to Defaults", BS_PUSHBUTTON | WS_TABSTOP, 16, H - 44, 150, 30, ID_RESET);
    child(L"BUTTON", L"Done", BS_PUSHBUTTON | WS_TABSTOP, W - 116, H - 44, 100, 30, ID_DONE);
    /* centre on the game window */
    RECT g, me;
    if (s_game && GetWindowRect(s_game, &g) && GetWindowRect(s_win, &me)) {
        int x = g.left + ((g.right - g.left) - (me.right - me.left)) / 2, yy = g.top + ((g.bottom - g.top) - (me.bottom - me.top)) / 3;
        SetWindowPos(s_win, NULL, x, yy < 0 ? 0 : yy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void controls_open(void) {
    if (!s_win) build();
    if (!s_win) { rt_log("menu: cannot create the Controls window"); return; }
    input_set_background(true);
    strcpy(s_padkey, "?");
    refresh();
    SetTimer(s_win, 1, 100, NULL);
    ShowWindow(s_win, SW_SHOW);
    SetForegroundWindow(s_win);
    rt_log("menu: Controls window open");
}
#endif
