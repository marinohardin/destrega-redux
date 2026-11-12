/* In-window menu bar (see menu_overlay.h): Linux's menus, and macOS / Windows with --overlay-menu.
 *
 * Why drawn by the frontend and not a toolkit: SDL3 has no menu API on Linux, and putting a GTK
 * menu bar into an SDL window is fragile under X11 and Wayland alike (and absent in full screen,
 * on Steam Deck-style sessions and on a bare compositor). An overlay looks and works the same
 * everywhere, also in full screen, and needs nothing beyond SDL: rectangles and lines from
 * SDL_Renderer and SDL's built-in 8x8 debug font scaled by whole pixels. File pickers and alerts
 * stay SDL's own dialogs (main.c; portals or zenity on Linux).
 *
 * Rules (the same as the native menus):
 *  - every item only queues a named action (frontend_menu_action); main.c runs it between frames;
 *  - check marks, titles and greyed-out items follow frontend_menu_action_allowed and the
 *    frontend_* getters, as menu_win.c's update_items and the Mac's validateMenuItem: do;
 *  - while a menu or form is open (overlay_menu_modal) the game is paused: main.c stops emulating,
 *    presents the last picture with the overlay and hands every input event here, so nothing
 *    reaches a pad. The overlay never touches emulated state (tools/test_overlay_menu.py compares
 *    every frame's --state-hash with a plain run).
 *
 * Coordinates are the renderer's output pixels (the window's pixel size); mouse events arrive in
 * window points and are scaled by the window's pixel density. */
#include "menu_overlay.h"
#include "netplay_ui.h"
#include "menu.h"
#include "input.h"
#include "platform.h"
#include "settings.h"
#include "widescreen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_log(const char *fmt, ...);

/* ---- the item table ------------------------------------------------------------------------
 * path: "Menu/Submenu/Title"; "-" = separator; "*" in View/Renderer = this platform's graphics
 * APIs (frontend_graphics_apis). action: "name" or "name=arg" as frontend_menu_action takes it, or
 * one handled here: quit, netplay-host-dialog, netplay-join-dialog, controls, netplay-status (the
 * disabled status line). Titles are ASCII (the font has nothing else). */
static const struct { const char *path, *action, *shortcut; } k_items[] = {
    { "File/Load Game Image...", "load", NULL },
    { "File/Show Game Files", "reveal-game", NULL },
    { "File/-", NULL, NULL },
    { "File/Save Games/Show Save Games", "reveal-saves", NULL },
    { "File/Save Games/-", NULL, NULL },
    { "File/Save Games/Import Save Games...", "save-import", NULL },
    { "File/Save Games/Export Save Games...", "save-export", NULL },
    { "File/Save Games/Delete Save Game...", "save-delete", NULL },
    { "File/-", NULL, NULL },
    { "File/Quit", "quit", NULL },
    { "View/Widescreen (16:9)", "widescreen", "F9" },
    { "View/Renderer/*", NULL, NULL },
    { "View/Renderer/Software", "renderer=software", NULL },
    { "View/Renderer/-", NULL, NULL },
    { "View/Renderer/Exact", "gpu-precision=exact", NULL },
    { "View/Renderer/Fast (Approximate)", "gpu-precision=fast", NULL },
    { "View/Resolution/Auto", "resolution=auto", NULL },
    { "View/Resolution/-", NULL, NULL },
    { "View/Resolution/4x6 (Sharpest)", "resolution=4x6", NULL },
    { "View/Resolution/2x3 (Balanced)", "resolution=2x3", NULL },
    { "View/Resolution/1x1 (Original)", "resolution=1x1", NULL },
    { "View/Texture Filtering/Off (Sharp Pixels)", "tex-filter=nearest", NULL },
    { "View/Texture Filtering/Bilinear", "tex-filter=bilinear", NULL },
    { "View/Texture Filtering/Smooth 2D Edges + Bilinear 3D", "tex-filter=xbr", NULL },
    { "View/Texture Filtering/-", NULL, NULL },
    { "View/Texture Filtering/Smooth Distant Textures (Fast Renderer)", "tex-cache", NULL },
    { "View/Smooth Colours", "smooth-colours", NULL },
    { "View/Anti-aliasing/Off", "ssaa=1", NULL },
    { "View/Anti-aliasing/2x", "ssaa=2", NULL },
    { "View/Anti-aliasing/3x", "ssaa=3", NULL },
    { "View/Anti-aliasing/4x", "ssaa=4", NULL },
    { "View/-", NULL, NULL },
    { "View/Enter Full Screen", "fullscreen", "F11" },
    { "View/-", NULL, NULL },
    { "View/Controls...", "controls", NULL },
    { "Netplay/Host Game...", "netplay-host-dialog", NULL },
    { "Netplay/Join Game...", "netplay-join-dialog", NULL },
    { "Netplay/Copy My Address", "netplay-copy-address", NULL },   /* while hosting; title: netplay_ui.c */
    { "Netplay/-", NULL, NULL },
    { "Netplay/Disconnect", "netplay-disconnect", NULL },
    { "Netplay/-", NULL, NULL },
    { "Netplay/Not connected", "netplay-status", NULL },
    { "Netplay/Copy Out-of-Sync Report Location", "netplay-copy-report", NULL },   /* after a desync */
};

typedef struct { char title[64], action[40]; const char *shortcut; int parent; bool sep, sub; } Node;
#define MAXN 160
static Node s_n[MAXN];   /* [0] = the root (the bar) */
static int s_nn;

static int add_node(int parent, const char *title, const char *action, const char *shortcut, bool sep, bool sub) {
    if (s_nn >= MAXN) return -1;
    Node *n = &s_n[s_nn];
    snprintf(n->title, sizeof n->title, "%s", title);
    snprintf(n->action, sizeof n->action, "%s", action ? action : "");
    n->shortcut = shortcut;
    n->parent = parent;
    n->sep = sep;
    n->sub = sub;
    return s_nn++;
}

static void add_path(const char *path, const char *action, const char *shortcut) {
    int p = 0;
    const char *s = path, *slash;
    while ((slash = strchr(s, '/'))) {
        char seg[64];
        snprintf(seg, sizeof seg, "%.*s", (int)(slash - s), s);
        int c = -1;
        for (int i = 1; i < s_nn; i++) if (s_n[i].parent == p && s_n[i].sub && !strcmp(s_n[i].title, seg)) c = i;
        if (c < 0) c = add_node(p, seg, NULL, NULL, false, true);
        if (c < 0) return;
        p = c;
        s = slash + 1;
    }
    if (!strcmp(s, "-")) { add_node(p, "", NULL, NULL, true, false); return; }
    if (!strcmp(s, "*")) {   /* graphics APIs, then a separator */
        const char *gn[4], *gt[4];
        const int ng = frontend_graphics_apis(gn, gt, 4);
        for (int i = 0; i < ng; i++) {
            char a[40];
            snprintf(a, sizeof a, "renderer=%s", gn[i]);
            add_node(p, gt[i], a, NULL, false, false);
        }
        add_node(p, "", NULL, NULL, true, false);
        return;
    }
    add_node(p, s, action, shortcut, false, false);
}

static int children(int p, int *out, int max) {
    int n = 0;
    for (int i = 1; i < s_nn && n < max; i++) if (s_n[i].parent == p) out[n++] = i;
    return n;
}

/* ---- item state (= menu_win.c update_items) ------------------------------------------------- */

typedef struct { bool enabled, checked; char title[160]; } ItemState;

static void action_name(const char *action, char *name, size_t n) {
    const char *eq = strchr(action, '=');
    snprintf(name, n, "%.*s", eq ? (int)(eq - action) : (int)strlen(action), action);
    size_t l = strlen(name);
    if (l > 7 && !strcmp(name + l - 7, "-dialog")) name[l - 7] = 0;
}

static void item_state(int i, ItemState *st) {
    const Node *n = &s_n[i];
    st->enabled = true;
    st->checked = false;
    snprintf(st->title, sizeof st->title, "%s", n->title);
    if (n->sub || n->sep) return;
    char name[48];
    action_name(n->action, name, sizeof name);
    const char *eq = strchr(n->action, '=');
    if (!strcmp(name, "netplay-status")) {
        snprintf(st->title, sizeof st->title, "%s", netplay_status_text());
        st->enabled = false;
    } else if (strcmp(name, "quit") && !frontend_menu_action_allowed(name)) {
        st->enabled = false;
    }
    if (!strcmp(name, "netplay-copy-address")) snprintf(st->title, sizeof st->title, "%s", netplay_ui_copy_title());
    if (!strcmp(name, "fullscreen")) snprintf(st->title, sizeof st->title, "%s", frontend_fullscreen() ? "Exit Full Screen" : "Enter Full Screen");
    if (!strcmp(name, "widescreen")) st->checked = frontend_widescreen();
    frontend_view_checked(n->action, &st->checked);
    if (!strcmp(name, "renderer") && eq) {
        st->checked = !strcmp(eq + 1, frontend_renderer());
        if (!frontend_renderer_available(eq + 1)) st->enabled = false;   /* not on this machine */
    }
}

/* ---- state ------------------------------------------------------------------------------------ */

enum { M_CLOSED, M_MENU, M_FORM, M_CONTROLS };
static int s_mode;
static SDL_Window *s_win;
static OverlayHooks s_hooks;
static bool s_inited, s_enabled;
static int s_cw = 960, s_ch = 720;   /* canvas (pixels) */
static float s_density = 1;
#define MAXD 6
static int s_lvl[MAXD], s_sel[MAXD], s_depth;   /* open popups: node, selected child (-1) */
static bool s_ctx;                              /* opened by a right-click: popup of the root */
static int s_ctx_x, s_ctx_y;
static uint64_t s_bar_until;                    /* hover-revealed bar visible until */
static uint64_t s_hint_until;                   /* "Menu: F10..." hint at start-up */
static bool s_alt_armed;
static int s_axis[2];                           /* controller left stick direction (edge) */
static char s_panel[1024];                      /* netplay waiting panel text ("" = hidden) */
static SDL_FRect s_panel_cancel;
static uint64_t s_hold_ns;                      /* test hook hold=MS: close at this time */
static bool s_in_test;                         /* running test hook commands (headless too) */

static int U(void) {
    int u = (s_ch + 180) / 360, v = (s_cw + 160) / 320;
    if (v < u) u = v;
    return u < 1 ? 1 : u > 8 ? 8 : u;
}
static int CH(void) { return 8 * U(); }                 /* character size */
static int ITEM_H(void) { return 14 * U(); }
static int BAR_H(void) { return 14 * U(); }

bool overlay_menu_enabled(void) { return s_enabled; }
bool overlay_menu_modal(void) { return s_mode != M_CLOSED; }

static void set_mode(int m) {
    if (m != M_CLOSED && s_mode == M_CLOSED) {   /* keys held now must not stay held in the game */
        rt_log("menu: overlay open (game paused)");
        input_release_all();
    }
    if (m == M_CLOSED && s_mode != M_CLOSED) {   /* nor the key / button that closed it */
        rt_log("menu: overlay closed");
        input_release_all();
        input_hold_pads();
    }
    if (m != M_FORM && s_mode == M_FORM && s_win) SDL_StopTextInput(s_win);
    if (m != M_CONTROLS && s_mode == M_CONTROLS) input_capture_cancel();
    s_mode = m;
    if (m != M_MENU) s_depth = 0;
}

void overlay_menu_close(void) { set_mode(M_CLOSED); }

void overlay_menu_init(SDL_Window *win, const OverlayHooks *hooks) {
    if (!s_inited) {
        s_nn = 0;
        add_node(-1, "", NULL, NULL, false, true);
        for (size_t i = 0; i < sizeof k_items / sizeof *k_items; i++) add_path(k_items[i].path, k_items[i].action, k_items[i].shortcut);
        s_inited = true;
    }
    if (hooks) s_hooks = *hooks;
    s_win = win;
    s_enabled = win != NULL;
    if (s_hooks.canvas_w > 0) { s_cw = s_hooks.canvas_w; s_ch = s_hooks.canvas_h; }
    if (s_enabled) {
        s_hint_until = SDL_GetTicksNS() + 6000000000ull;
        rt_log("menu: in-window menu bar (F10, Alt, right-click or the mouse at the top of the window)");
    }
}

/* ---- drawing helpers ------------------------------------------------------------------------- */

typedef struct { Uint8 r, g, b, a; } Col;
static const Col C_BAR = { 22, 24, 30, 238 }, C_POP = { 30, 33, 41, 248 }, C_BORDER = { 86, 92, 108, 255 },
                 C_HI = { 50, 98, 190, 255 }, C_HI_OFF = { 58, 62, 74, 255 }, C_TEXT = { 234, 236, 242, 255 },
                 C_OFF = { 118, 124, 138, 255 }, C_SEP = { 66, 70, 82, 255 }, C_DIM = { 0, 0, 0, 96 },
                 C_FIELD = { 14, 15, 19, 255 }, C_ACCENT = { 120, 170, 255, 255 };

static void color(SDL_Renderer *r, Col c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }
static void fill(SDL_Renderer *r, float x, float y, float w, float h, Col c) {
    color(r, c);
    SDL_RenderFillRect(r, &(SDL_FRect){ x, y, w, h });
}
static void frame(SDL_Renderer *r, float x, float y, float w, float h, Col c) {
    color(r, c);
    const float t = (float)(U() > 2 ? U() / 2 : 1);
    SDL_RenderFillRect(r, &(SDL_FRect){ x, y, w, t });
    SDL_RenderFillRect(r, &(SDL_FRect){ x, y + h - t, w, t });
    SDL_RenderFillRect(r, &(SDL_FRect){ x, y, t, h });
    SDL_RenderFillRect(r, &(SDL_FRect){ x + w - t, y, t, h });
}
/* text at (x, y) (top left), at most max_chars characters (0 = all) */
static void text(SDL_Renderer *r, float x, float y, const char *s, Col c, int max_chars) {
    char buf[256];
    int n = (int)strlen(s);
    if (max_chars > 0 && n > max_chars) {
        n = max_chars > 3 ? max_chars - 3 : max_chars;
        snprintf(buf, sizeof buf, "%.*s%s", n, s, max_chars > 3 ? "..." : "");
    } else snprintf(buf, sizeof buf, "%s", s);
    const float u = (float)U();
    color(r, c);
    SDL_SetRenderScale(r, u, u);
    SDL_RenderDebugText(r, x / u, y / u, buf);
    SDL_SetRenderScale(r, 1, 1);
}
static int text_w(const char *s) { return (int)strlen(s) * CH(); }
static void checkmark(SDL_Renderer *r, float x, float y, Col c) {   /* in a CH x CH box */
    const float u = (float)U();
    color(r, c);
    for (float t = 0; t < u * 1.5f; t += 1) {
        SDL_RenderLine(r, x + 1 * u, y + 4 * u + t, x + 3 * u, y + 6 * u + t);
        SDL_RenderLine(r, x + 3 * u, y + 6 * u + t, x + 7 * u, y + 1 * u + t);
    }
}
static void arrow(SDL_Renderer *r, float x, float y, Col c) {   /* submenu: a small right-pointing triangle */
    const float u = (float)U();
    color(r, c);
    for (float k = 0; k < 3 * u; k += 1) SDL_RenderLine(r, x + k, y + 1 * u + k, x + k, y + 7 * u - k);
}
static bool inside(const SDL_FRect *f, float x, float y) { return x >= f->x && y >= f->y && x < f->x + f->w && y < f->y + f->h; }

/* word wrap s into lines of at most `cols` characters: returns the count, lines in out[][] */
static int wrap(const char *s, int cols, char out[][128], int max) {
    int n = 0;
    if (cols > 127) cols = 127;
    if (cols < 8) cols = 8;
    while (*s && n < max) {
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        if (len <= cols) {
            snprintf(out[n++], 128, "%.*s", len, s);
            s += len + (nl ? 1 : 0);
            continue;
        }
        int cut = cols;
        while (cut > 0 && s[cut] != ' ') cut--;
        if (cut == 0) cut = cols;
        snprintf(out[n++], 128, "%.*s", cut, s);
        s += cut;
        while (*s == ' ') s++;
    }
    return n;
}

/* ---- the bar and the popups: layout ----------------------------------------------------------- */

static int bar_items(SDL_FRect *rects, int *nodes, int max) {
    int kids[16];
    int n = children(0, kids, 16);
    float x = (float)(4 * U());
    for (int i = 0; i < n && i < max; i++) {
        const float w = (float)(text_w(s_n[kids[i]].title) + 16 * U());
        rects[i] = (SDL_FRect){ x, 0, w, (float)BAR_H() };
        nodes[i] = kids[i];
        x += w;
    }
    return n < max ? n : max;
}

typedef struct { SDL_FRect r; int n, node[48]; SDL_FRect item[48]; } Pop;

static void pop_layout(int k, Pop *p) {
    const int u = U(), node = s_lvl[k];
    p->n = children(node, p->node, 48);
    int wmax = 0, h = 2 * u;
    for (int i = 0; i < p->n; i++) {
        ItemState st;
        item_state(p->node[i], &st);
        const Node *n = &s_n[p->node[i]];
        int w = 16 * u + text_w(st.title) + 10 * u;
        if (n->shortcut) w += text_w(n->shortcut) + 16 * u;
        if (n->sub) w += 12 * u;
        if (w > wmax) wmax = w;
        h += n->sep ? 7 * u : ITEM_H();
    }
    h += 2 * u;
    if (wmax < 120 * u) wmax = 120 * u;
    if (wmax > s_cw - 8 * u) wmax = s_cw - 8 * u;
    float x, y;
    if (k == 0 && !s_ctx) {
        SDL_FRect br[16]; int bn[16];
        const int nb = bar_items(br, bn, 16);
        x = br[0].x; y = (float)BAR_H();
        for (int i = 0; i < nb; i++) if (bn[i] == node) x = br[i].x;
    } else if (k == 0) {
        x = (float)s_ctx_x; y = (float)s_ctx_y;
    } else {
        Pop pp;
        pop_layout(k - 1, &pp);
        x = pp.r.x + pp.r.w - (float)u; y = pp.r.y;
        for (int i = 0; i < pp.n; i++) if (pp.node[i] == node) y = pp.item[i].y - 2 * (float)u;
        if (x + (float)wmax > (float)s_cw) x = pp.r.x - (float)wmax + (float)u;
    }
    if (x + (float)wmax > (float)s_cw) x = (float)(s_cw - wmax);
    if (x < 0) x = 0;
    if (y + (float)h > (float)s_ch) y = (float)(s_ch - h);
    if (y < 0) y = 0;
    p->r = (SDL_FRect){ x, y, (float)wmax, (float)h };
    float iy = y + 2 * (float)u;
    for (int i = 0; i < p->n; i++) {
        const float ih = (float)(s_n[p->node[i]].sep ? 7 * u : ITEM_H());
        p->item[i] = (SDL_FRect){ x, iy, (float)wmax, ih };
        iy += ih;
    }
}

static bool selectable(int i) {
    if (s_n[i].sep) return false;
    ItemState st;
    item_state(i, &st);
    return st.enabled;
}

/* move the selection of the deepest popup by d (+1 / -1), skipping separators and greyed items */
static void move_sel(int d) {
    if (s_depth <= 0) return;
    int kids[48];
    const int n = children(s_lvl[s_depth - 1], kids, 48);
    int cur = -1;
    for (int i = 0; i < n; i++) if (kids[i] == s_sel[s_depth - 1]) cur = i;
    for (int step = 1; step <= n; step++) {
        int j = cur < 0 ? (d > 0 ? step - 1 : n - step) : ((cur + d * step) % n + n) % n;
        if (selectable(kids[j])) { s_sel[s_depth - 1] = kids[j]; return; }
    }
}

static void open_top(int node, bool keyboard) {
    set_mode(M_MENU);
    s_ctx = false;
    s_depth = 1;
    s_lvl[0] = node;
    s_sel[0] = -1;
    if (keyboard) move_sel(1);
}
static void open_ctx(int x, int y) {
    set_mode(M_MENU);
    s_ctx = true;
    s_ctx_x = x; s_ctx_y = y;
    s_depth = 1;
    s_lvl[0] = 0;
    s_sel[0] = -1;
}
static int first_top(void) { int k[16]; return children(0, k, 16) ? k[0] : -1; }

/* step the bar's open menu left / right */
static void next_top(int d) {
    int k[16];
    const int n = children(0, k, 16);
    int cur = 0;
    for (int i = 0; i < n; i++) if (k[i] == s_lvl[0]) cur = i;
    open_top(k[((cur + d) % n + n) % n], true);
}

static void open_sub(int node, bool keyboard) {
    if (s_depth >= MAXD) return;
    s_lvl[s_depth] = node;
    s_sel[s_depth] = -1;
    s_depth++;
    if (keyboard) move_sel(1);
}

/* ---- forms (Host Game / Join Game) ------------------------------------------------------------ */

enum { FK_TEXT, FK_NUM, FK_CHECK, FK_OK, FK_CANCEL };
typedef struct { int kind; char label[48], val[160]; bool on; int lo, hi; SDL_FRect r; } Field;
static struct { char title[32], text[1024], ok[16]; bool host; Field f[8]; int n, focus; SDL_FRect r; } s_form;

static Field *fadd(int kind, const char *label, const char *val) {
    Field *f = &s_form.f[s_form.n++];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    snprintf(f->label, sizeof f->label, "%s", label);
    snprintf(f->val, sizeof f->val, "%s", val ? val : "");
    return f;
}

static void netplay_form(bool host) {
    memset(&s_form, 0, sizeof s_form);
    s_form.host = host;
    const char *port = settings_get("netplay", "port"), *addr = settings_get("netplay", "join_address"),
               *delay = settings_get("netplay", "delay");
    if (host) {
        char ip[256];
        plat_local_ipv4(ip, sizeof ip);
        snprintf(s_form.title, sizeof s_form.title, "Host Game");
        snprintf(s_form.ok, sizeof s_form.ok, "Host");
        snprintf(s_form.text, sizeof s_form.text,
                 "Your opponent chooses Join Game and types one of this computer's addresses and the port.\n\n"
                 "This computer's addresses: %s\n\nOver the internet, the game asks your router to forward UDP port %s "
                 "to this computer and shows the address to give your opponent; if the router can't, forward the port "
                 "by hand. Both games restart from the beginning when your opponent connects.",
                 *ip ? ip : "none found", port ? port : "7000");
        Field *f = fadd(FK_NUM, "Port:", port ? port : "7000");
        f->lo = 1; f->hi = 65535;
    } else {
        snprintf(s_form.title, sizeof s_form.title, "Join Game");
        snprintf(s_form.ok, sizeof s_form.ok, "Join");
        snprintf(s_form.text, sizeof s_form.text, "Type the address and port your opponent gave you, for example "
                 "192.168.1.20:7000. Both games restart from the beginning when you connect.");
        fadd(FK_TEXT, "Address:port:", addr ? addr : "");
    }
    Field *d = fadd(FK_NUM, "Input delay:", delay ? delay : "2");
    d->lo = 1; d->hi = 30;
    if (host) fadd(FK_CHECK, "Ask the router to open the port automatically", NULL)->on = settings_get_bool("netplay", "port_mapping", true);
    fadd(FK_OK, s_form.ok, NULL);
    fadd(FK_CANCEL, "Cancel", NULL);
    set_mode(M_FORM);
    if (s_win) SDL_StartTextInput(s_win);
    rt_log("menu: %s form open", s_form.title);
}

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
}

/* OK / Cancel (same post-processing as menu_win.c's netplay_dialog) */
static void form_finish(bool ok) {
    rt_log("menu: %s form %s", s_form.title, ok ? "accepted" : "cancelled");
    set_mode(M_CLOSED);
    if (!ok) return;
    int dl = atoi(s_form.f[1].val);
    if (dl < 1 || dl > 30) dl = 2;
    char delay[16], arg[400];
    snprintf(delay, sizeof delay, "%d", dl);
    settings_set("netplay", "delay", delay);
    if (s_form.host) {
        int p = atoi(s_form.f[0].val);
        if (p <= 0 || p > 65535) p = 7000;
        char port[16];
        snprintf(port, sizeof port, "%d", p);
        settings_set("netplay", "port", port);
        settings_set_bool("netplay", "port_mapping", s_form.f[2].on);
        settings_save();
        snprintf(arg, sizeof arg, "%d,%d", p, dl);
        frontend_menu_action("netplay-host", arg);
    } else {
        char addr[200];
        snprintf(addr, sizeof addr, "%s", s_form.f[0].val);
        trim(addr);
        const char *port = settings_get("netplay", "port");
        if (!strchr(addr, ':') && strlen(addr) + 8 < sizeof addr) { strcat(addr, ":"); strcat(addr, port ? port : "7000"); }
        settings_set("netplay", "join_address", addr);
        settings_save();
        snprintf(arg, sizeof arg, "%s,%d", addr, dl);
        frontend_menu_action("netplay-join", arg);
    }
}

static void form_layout(void) {
    const int u = U(), ch = CH();
    float W = (float)(70 * ch + 24 * u);
    if (W > (float)(s_cw - 16 * u)) W = (float)(s_cw - 16 * u);
    char lines[24][128];
    const int nl = wrap(s_form.text, (int)((W - 24 * (float)u) / (float)ch), lines, 24);
    float y = 12 * (float)u + (float)ITEM_H() + (float)(nl * (ch + 3 * u)) + 8 * (float)u;
    float H = y;
    for (int i = 0; i < s_form.n; i++) {
        const Field *f = &s_form.f[i];
        if (f->kind == FK_CANCEL) continue;
        H += (float)(ITEM_H() + 6 * u);
    }
    H += 8 * (float)u;
    const float X = ((float)s_cw - W) / 2, Y = ((float)s_ch - H) / 3 > 0 ? ((float)s_ch - H) / 3 : 0;
    s_form.r = (SDL_FRect){ X, Y, W, H };
    const float lw = (float)(15 * ch);
    float fy = Y + y;
    for (int i = 0; i < s_form.n; i++) {
        Field *f = &s_form.f[i];
        if (f->kind == FK_TEXT || f->kind == FK_NUM) {
            const float vw = f->kind == FK_NUM ? (float)(8 * ch) : W - 24 * (float)u - lw;
            f->r = (SDL_FRect){ X + 12 * (float)u + lw, fy, vw, (float)ITEM_H() };
            fy += (float)(ITEM_H() + 6 * u);
        } else if (f->kind == FK_CHECK) {
            f->r = (SDL_FRect){ X + 12 * (float)u, fy, (float)(text_w(f->label) + 14 * u), (float)ITEM_H() };
            fy += (float)(ITEM_H() + 6 * u);
        }
    }
    const float bw = (float)(10 * ch);
    for (int i = 0; i < s_form.n; i++) {
        Field *f = &s_form.f[i];
        if (f->kind == FK_OK) f->r = (SDL_FRect){ X + W - 12 * (float)u - 2 * bw - 6 * (float)u, fy, bw, (float)ITEM_H() };
        if (f->kind == FK_CANCEL) f->r = (SDL_FRect){ X + W - 12 * (float)u - bw, fy, bw, (float)ITEM_H() };
    }
}

static void form_draw(SDL_Renderer *r) {
    form_layout();
    const int u = U(), ch = CH();
    const SDL_FRect R = s_form.r;
    fill(r, R.x, R.y, R.w, R.h, C_POP);
    frame(r, R.x, R.y, R.w, R.h, C_BORDER);
    fill(r, R.x, R.y, R.w, (float)ITEM_H(), C_BAR);
    text(r, R.x + 12 * (float)u, R.y + 3 * (float)u, s_form.title, C_ACCENT, 0);
    char lines[24][128];
    const int nl = wrap(s_form.text, (int)((R.w - 24 * (float)u) / (float)ch), lines, 24);
    for (int i = 0; i < nl; i++)
        text(r, R.x + 12 * (float)u, R.y + 12 * (float)u + (float)ITEM_H() + (float)(i * (ch + 3 * u)), lines[i], C_TEXT, 0);
    for (int i = 0; i < s_form.n; i++) {
        const Field *f = &s_form.f[i];
        const bool foc = i == s_form.focus;
        const float ty = f->r.y + 3 * (float)u;
        if (f->kind == FK_TEXT || f->kind == FK_NUM) {
            text(r, R.x + 12 * (float)u, ty, f->label, C_TEXT, 0);
            fill(r, f->r.x, f->r.y, f->r.w, f->r.h, C_FIELD);
            frame(r, f->r.x, f->r.y, f->r.w, f->r.h, foc ? C_HI : C_BORDER);
            const int cols = (int)((f->r.w - 8 * (float)u) / (float)ch) - 1;
            const int l = (int)strlen(f->val);
            const char *v = l > cols && cols > 0 ? f->val + (l - cols) : f->val;   /* show the end while typing */
            text(r, f->r.x + 4 * (float)u, ty, v, C_TEXT, 0);
            if (foc) fill(r, f->r.x + 4 * (float)u + (float)text_w(v), ty, (float)u, (float)ch, C_TEXT);   /* caret */
        } else if (f->kind == FK_CHECK) {
            if (foc) fill(r, f->r.x - 2 * (float)u, f->r.y, f->r.w + 4 * (float)u, f->r.h, C_HI_OFF);
            frame(r, f->r.x, ty, (float)ch, (float)ch, C_TEXT);
            if (f->on) checkmark(r, f->r.x, ty, C_TEXT);
            text(r, f->r.x + 14 * (float)u, ty, f->label, C_TEXT, 0);
        } else {
            fill(r, f->r.x, f->r.y, f->r.w, f->r.h, foc ? C_HI : C_HI_OFF);
            text(r, f->r.x + (f->r.w - (float)text_w(f->label)) / 2, ty, f->label, C_TEXT, 0);
        }
    }
}

static void form_activate(int i) {
    if (i < 0 || i >= s_form.n) return;
    Field *f = &s_form.f[i];
    if (f->kind == FK_CHECK) f->on = !f->on;
    else if (f->kind == FK_CANCEL) form_finish(false);
    else form_finish(true);
}

static void form_step(int d) { s_form.focus = ((s_form.focus + d) % s_form.n + s_form.n) % s_form.n; }

static void form_adjust(int d) {   /* left / right */
    Field *f = &s_form.f[s_form.focus];
    if (f->kind == FK_NUM) {
        int v = atoi(f->val) + d;
        if (v < f->lo) v = f->lo;
        if (v > f->hi) v = f->hi;
        snprintf(f->val, sizeof f->val, "%d", v);
    } else if (f->kind == FK_OK || f->kind == FK_CANCEL) form_step(d);
}

static void form_type(const char *s) {
    Field *f = &s_form.f[s_form.focus];
    if (f->kind != FK_TEXT && f->kind != FK_NUM) return;
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (f->kind == FK_NUM ? (c < '0' || c > '9') : (c <= ' ' || c > '~')) continue;   /* ASCII only */
        size_t l = strlen(f->val);
        if (l + 1 < sizeof f->val && (f->kind != FK_NUM || l < 5)) { f->val[l] = (char)c; f->val[l + 1] = 0; }
    }
}

static void form_key(SDL_Keycode k, SDL_Keymod mod) {
    Field *f = &s_form.f[s_form.focus];
    switch (k) {
    case SDLK_TAB: form_step(mod & SDL_KMOD_SHIFT ? -1 : 1); break;
    case SDLK_DOWN: form_step(1); break;
    case SDLK_UP: form_step(-1); break;
    case SDLK_LEFT: form_adjust(-1); break;
    case SDLK_RIGHT: form_adjust(1); break;
    case SDLK_ESCAPE: form_finish(false); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (f->kind == FK_CANCEL) form_finish(false);
        else if (f->kind == FK_CHECK) f->on = !f->on;
        else form_finish(true);
        break;
    case SDLK_SPACE: if (f->kind != FK_TEXT && f->kind != FK_NUM) form_activate(s_form.focus); break;
    case SDLK_BACKSPACE: if ((f->kind == FK_TEXT || f->kind == FK_NUM) && *f->val) f->val[strlen(f->val) - 1] = 0; break;
    default: break;
    }
}

/* ---- Controls (= controls_win.c, as an overlay form) ------------------------------------------ */

enum { CR_PLAYER, CR_DEVICE, CR_BTN0, CR_STICK = CR_BTN0 + PSB_COUNT, CR_DZ, CR_END, CR_N };
static struct { int player, row, col; char dev_rep[16][48], dev_title[16][160]; int ndev; SDL_FRect r, cell[CR_N][2]; } s_ctl;

static void ctl_devices(void) {
    InputPlayer *p = input_player(s_ctl.player);
    s_ctl.ndev = 0;
#define ADD(T, R) do { if (s_ctl.ndev < 16) { snprintf(s_ctl.dev_title[s_ctl.ndev], 160, "%s", T); \
        snprintf(s_ctl.dev_rep[s_ctl.ndev], 48, "%s", R); s_ctl.ndev++; } } while (0)
    ADD(s_ctl.player == 0 ? "Automatic (keyboard and the first controller)" : "Automatic (the next free controller)", "auto");
    ADD("Keyboard", "keyboard");
    ADD("Any controller", "gamepad");
    bool have = false;
    char title[200], rep[48];
    for (int i = 0; i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (!input_pad_info(i, &pi)) continue;
        snprintf(rep, sizeof rep, "gamepad:%s", pi.guid);
        bool dup = false;
        for (int k = 0; k < s_ctl.ndev; k++) if (!strcmp(s_ctl.dev_rep[k], rep)) dup = true;
        if (dup) continue;
        snprintf(title, sizeof title, "Controller: %s", pi.name);
        ADD(title, rep);
        if (p->dev.kind == INDEV_GAMEPAD_GUID && !SDL_strcasecmp(pi.guid, p->dev.guid)) have = true;
    }
    if (p->dev.kind == INDEV_GAMEPAD_GUID && !have) {
        snprintf(rep, sizeof rep, "gamepad:%s", p->dev.guid);
        snprintf(title, sizeof title, "Controller: %s (not connected)", p->dev.name[0] ? p->dev.name : "unknown");
        ADD(title, rep);
    }
    ADD("None", "none");
#undef ADD
}
static int ctl_cur_dev(void) {
    char id[64];
    input_device_id(&input_player(s_ctl.player)->dev, id, sizeof id);
    for (int k = 0; k < s_ctl.ndev; k++) if (!SDL_strcasecmp(s_ctl.dev_rep[k], id)) return k;
    return 0;
}
static void ctl_set_dev(int k) {
    InputPlayer *p = input_player(s_ctl.player);
    InputDevice d;
    if (k < 0 || k >= s_ctl.ndev || !input_device_from_id(s_ctl.dev_rep[k], &d)) return;
    for (int i = 0; d.kind == INDEV_GAMEPAD_GUID && i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (input_pad_info(i, &pi) && !SDL_strcasecmp(pi.guid, d.guid)) snprintf(d.name, sizeof d.name, "%s", pi.name);
    }
    if (d.kind == INDEV_GAMEPAD_GUID && !d.name[0]) snprintf(d.name, sizeof d.name, "%s", p->dev.name);
    p->dev = d;
    input_changed(s_ctl.player);
    rt_log("menu: controls: player %d device %s", s_ctl.player + 1, s_ctl.dev_rep[k]);
}

void overlay_menu_controls(void) {
    if (!s_inited) overlay_menu_init(NULL, NULL);
    memset(&s_ctl, 0, sizeof s_ctl);
    s_ctl.row = CR_BTN0;
    set_mode(M_CONTROLS);
    rt_log("menu: Controls open");
}

static bool confirm(const char *msg, const char *yes) {
    if (!s_win) { rt_log("menu: confirm \"%s\" -> %s (headless: automatic)", msg, yes); return true; }
    const SDL_MessageBoxButtonData b[] = { { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
                                           { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes } };
    SDL_MessageBoxData d = { SDL_MESSAGEBOX_WARNING, s_win, "Controls", msg, 2, b, NULL };
    int hit = 0;
    return SDL_ShowMessageBox(&d, &hit) && hit == 1;
}

static void ctl_activate(void) {
    InputPlayer *p = input_player(s_ctl.player);
    const int row = s_ctl.row;
    if (row == CR_PLAYER) { input_capture_cancel(); s_ctl.player = s_ctl.col; }
    else if (row == CR_DEVICE) { ctl_devices(); ctl_set_dev((ctl_cur_dev() + 1) % s_ctl.ndev); }
    else if (row >= CR_BTN0 && row < CR_BTN0 + PSB_COUNT) {
        int cp, cb, ck;
        const int b = row - CR_BTN0;
        if (input_capture_armed(&cp, &cb, &ck) && cp == s_ctl.player && cb == b && ck == s_ctl.col) input_capture_cancel();
        else input_capture_begin(s_ctl.player, b, s_ctl.col);
    } else if (row == CR_STICK) { p->stick = !p->stick; input_changed(s_ctl.player); }
    else if (row == CR_DZ) { p->deadzone = p->deadzone >= 95 ? 5 : p->deadzone + 5; input_changed(s_ctl.player); }
    else if (row == CR_END) {
        if (s_ctl.col == 1) { set_mode(M_CLOSED); return; }
        char q[200];
        snprintf(q, sizeof q, "Reset Player %d's controls to the defaults?\n\nThe device goes back to Automatic and every "
                 "key and controller button to its default.", s_ctl.player + 1);
        if (confirm(q, "Reset")) { input_capture_cancel(); input_reset(s_ctl.player); }
    }
}

static void ctl_adjust(int d) {
    InputPlayer *p = input_player(s_ctl.player);
    const int row = s_ctl.row;
    if (row == CR_PLAYER) { input_capture_cancel(); s_ctl.player = s_ctl.col = d > 0; }
    else if (row == CR_DEVICE) { ctl_devices(); ctl_set_dev(((ctl_cur_dev() + d) % s_ctl.ndev + s_ctl.ndev) % s_ctl.ndev); }
    else if (row == CR_STICK) { p->stick = !p->stick; input_changed(s_ctl.player); }
    else if (row == CR_DZ) {
        int v = p->deadzone + 5 * d;
        p->deadzone = v < 5 ? 5 : v > 95 ? 95 : v;
        input_changed(s_ctl.player);
    } else s_ctl.col = d > 0;
}

static void ctl_move(int d) {
    s_ctl.row = ((s_ctl.row + d) % CR_N + CR_N) % CR_N;
    if (s_ctl.row == CR_PLAYER) s_ctl.col = s_ctl.player;
    else if (s_ctl.row == CR_DEVICE || s_ctl.row == CR_STICK || s_ctl.row == CR_DZ) s_ctl.col = 0;
}

static void ctl_draw(SDL_Renderer *r) {
    const int u = U(), ch = CH();
    ctl_devices();
    InputPlayer *p = input_player(s_ctl.player);
    int rh = ITEM_H();
    const int rows = CR_N + 4;   /* title, header, two status lines */
    if (rh * rows > s_ch - 8 * u) rh = (s_ch - 8 * u) / rows;
    if (rh < ch + 2) rh = ch + 2;
    float W = (float)(66 * ch);
    if (W > (float)(s_cw - 8 * u)) W = (float)(s_cw - 8 * u);
    const float H = (float)(rh * rows + 4 * u);
    const float X = ((float)s_cw - W) / 2, Y = ((float)s_ch - H) / 2 > 0 ? ((float)s_ch - H) / 2 : 0;
    s_ctl.r = (SDL_FRect){ X, Y, W, H };
    fill(r, X, Y, W, H, C_POP);
    frame(r, X, Y, W, H, C_BORDER);
    fill(r, X, Y, W, (float)rh, C_BAR);
    const float ty = ((float)rh - (float)ch) / 2;
    text(r, X + 12 * (float)u, Y + ty, "Controls", C_ACCENT, 0);
    const float c0 = X + 12 * (float)u, c1 = X + 12 * (float)u + 14 * (float)ch, cw = (W - 30 * (float)u - 14 * (float)ch) / 2;
    const float c2 = c1 + cw + 6 * (float)u;
    int cp = -1, cb = -1, ck = -1;
    const bool armed = input_capture_armed(&cp, &cb, &ck);
    const int type = input_player_pad_type(s_ctl.player);
    float y = Y + (float)rh;
    for (int row = 0; row < CR_N; row++) {
        if (row == CR_BTN0) {   /* column header */
            text(r, c0, y + ty, "Button", C_OFF, 0);
            text(r, c1, y + ty, "Keyboard", C_OFF, 0);
            text(r, c2, y + ty, "Controller", C_OFF, 0);
            y += (float)rh;
        }
        const bool selrow = row == s_ctl.row;
        SDL_FRect a = { c1, y + 1, cw, (float)rh - 2 }, b = { c2, y + 1, cw, (float)rh - 2 };
        char l0[64] = "", v0[160] = "", v1[160] = "";
        bool two = false;
        if (row == CR_PLAYER) { snprintf(l0, sizeof l0, "Player"); snprintf(v0, sizeof v0, "Player 1"); snprintf(v1, sizeof v1, "Player 2"); two = true; }
        else if (row == CR_DEVICE) {
            snprintf(l0, sizeof l0, "Device");
            snprintf(v0, sizeof v0, "< %s >", s_ctl.dev_title[ctl_cur_dev()]);
            a.w = W - (a.x - X) - 12 * (float)u;
        } else if (row < CR_BTN0 + PSB_COUNT) {
            const int bt = row - CR_BTN0;
            snprintf(l0, sizeof l0, "%s", input_button_label(bt));
            if (armed && cp == s_ctl.player && cb == bt && ck == 0) snprintf(v0, sizeof v0, "Press a key...");
            else input_key_label(p->key[bt], v0, sizeof v0);
            if (armed && cp == s_ctl.player && cb == bt && ck == 1) snprintf(v1, sizeof v1, "Press a button...");
            else input_padbind_label(p->pad[bt], type, v1, sizeof v1);
            two = true;
        } else if (row == CR_STICK) {
            snprintf(l0, sizeof l0, "Left stick");
            snprintf(v0, sizeof v0, "[%c] moves too", p->stick ? 'x' : ' ');
            a.w = W - (a.x - X) - 12 * (float)u;
        } else if (row == CR_DZ) {
            snprintf(l0, sizeof l0, "Deadzone");
            snprintf(v0, sizeof v0, "< %d %% >", p->deadzone);
        } else {
            a = (SDL_FRect){ c0, y + 1, 20 * (float)ch, (float)rh - 2 };
            b = (SDL_FRect){ X + W - 12 * (float)u - 10 * (float)ch, y + 1, 10 * (float)ch, (float)rh - 2 };
            snprintf(v0, sizeof v0, "Reset to Defaults");
            snprintf(v1, sizeof v1, "Done");
            two = true;
        }
        s_ctl.cell[row][0] = a;
        s_ctl.cell[row][1] = two ? b : a;
        if (*l0) text(r, c0, y + ty, l0, C_TEXT, 13);
        const bool on0 = row == CR_PLAYER ? s_ctl.player == 0 : false, on1 = row == CR_PLAYER ? s_ctl.player == 1 : false;
        fill(r, a.x, a.y, a.w, a.h, selrow && s_ctl.col == 0 ? C_HI : on0 ? C_HI_OFF : C_FIELD);
        text(r, a.x + 4 * (float)u, y + ty, v0, C_TEXT, (int)((a.w - 8 * (float)u) / (float)ch));
        if (two) {
            fill(r, b.x, b.y, b.w, b.h, selrow && s_ctl.col == 1 ? C_HI : on1 ? C_HI_OFF : C_FIELD);
            text(r, b.x + 4 * (float)u, y + ty, v1, C_TEXT, (int)((b.w - 8 * (float)u) / (float)ch));
        }
        y += (float)rh;
    }
    /* status (as controls_win.c) */
    char s[400];
    const char *msg = input_capture_message();
    if (armed || *msg) snprintf(s, sizeof s, "%s", armed && !*msg ? "Press the key or button (Escape cancels)." : msg);
    else {
        int pad = -1;
        InputPadInfo pi = { 0 };
        for (int i = 0; i < input_pad_count(); i++) { InputPadInfo q; if (input_pad_info(i, &q) && q.player == s_ctl.player) { pad = i; pi = q; } }
        const bool kb = input_player_has_keyboard(s_ctl.player);
        snprintf(s, sizeof s, "Player %d reads %s%s%s. During netplay you play with Player 1's controls.",
                 s_ctl.player + 1, kb ? "the keyboard" : "", kb && pad >= 0 ? " and " : "",
                 pad >= 0 ? pi.name : kb ? "" : "nothing (no controller for it is connected)");
    }
    char lines[4][128];
    const int nl = wrap(s, (int)((W - 24 * (float)u) / (float)ch), lines, 2);
    for (int i = 0; i < nl; i++) text(r, c0, y + ty + (float)(i * (ch + 2 * u)), lines[i], C_OFF, 0);
}

static void ctl_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: ctl_move(-1); break;
    case SDLK_DOWN: case SDLK_TAB: ctl_move(1); break;
    case SDLK_LEFT: ctl_adjust(-1); break;
    case SDLK_RIGHT: ctl_adjust(1); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: ctl_activate(); break;
    case SDLK_ESCAPE: case SDLK_F10: set_mode(M_CLOSED); break;
    default: break;
    }
}

static void ctl_click(float x, float y) {
    for (int row = 0; row < CR_N; row++)
        for (int c = 0; c < 2; c++)
            if (inside(&s_ctl.cell[row][c], x, y)) {
                s_ctl.row = row; s_ctl.col = c;
                if (row == CR_DEVICE) ctl_adjust(x < s_ctl.cell[row][0].x + s_ctl.cell[row][0].w / 2 ? -1 : 1);
                else if (row == CR_DZ) ctl_adjust(x < s_ctl.cell[row][0].x + s_ctl.cell[row][0].w / 2 ? -1 : 1);
                else ctl_activate();
                return;
            }
}

/* ---- firing an item ------------------------------------------------------------------------- */

static void fire(int i) {
    const char *action = s_n[i].action;
    rt_log("menu: overlay item %s", s_n[i].title);
    set_mode(M_CLOSED);
    if (!strcmp(action, "quit")) {
        if (!s_win) { rt_log("menu: quit (headless: ignored)"); return; }
        SDL_Event q = { 0 };
        q.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&q);
        return;
    }
    if (!strcmp(action, "netplay-host-dialog") || !strcmp(action, "netplay-join-dialog")) {
        netplay_form(!strcmp(action, "netplay-host-dialog"));
        return;
    }
    if (!strcmp(action, "controls")) { overlay_menu_controls(); return; }
    char name[64];
    const char *eq = strchr(action, '=');
    if (!eq) { frontend_menu_action(action, NULL); return; }
    snprintf(name, sizeof name, "%.*s", (int)(eq - action), action);
    frontend_menu_action(name, eq + 1);
}

static void activate(int i) {
    if (i < 0) return;
    if (s_n[i].sub) { open_sub(i, true); return; }
    if (selectable(i)) fire(i);
}

/* ---- events ----------------------------------------------------------------------------------- */

static bool is_input_event(Uint32 t) {
    switch (t) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION:
        return true;
    }
    return false;
}

static bool alt_bound(void) {   /* a player uses Alt as a pad button: Alt alone does not open the menu */
    for (int p = 0; p < 2; p++)
        for (int b = 0; b < PSB_COUNT; b++) {
            const int k = input_player(p)->key[b];
            if (k == SDL_SCANCODE_LALT || k == SDL_SCANCODE_RALT) return true;
        }
    return false;
}

/* menu mode: hit test from the deepest popup up; the bar */
static bool menu_mouse(float x, float y, bool click) {
    for (int k = s_depth - 1; k >= 0; k--) {
        Pop p;
        pop_layout(k, &p);
        if (!inside(&p.r, x, y)) continue;
        for (int i = 0; i < p.n; i++) {
            if (!inside(&p.item[i], x, y)) continue;
            const int node = p.node[i];
            s_depth = k + 1;
            s_sel[k] = s_n[node].sep ? -1 : node;
            if (s_n[node].sub) { if (s_depth < MAXD) open_sub(node, false); }
            else if (click) activate(node);
            return true;
        }
        return true;
    }
    if (!s_ctx) {
        SDL_FRect br[16]; int bn[16];
        const int nb = bar_items(br, bn, 16);
        for (int i = 0; i < nb; i++) {
            if (!inside(&br[i], x, y)) continue;
            if (click && bn[i] == s_lvl[0]) set_mode(M_CLOSED);
            else if (bn[i] != s_lvl[0]) open_top(bn[i], false);
            return true;
        }
    }
    if (click) set_mode(M_CLOSED);   /* a click outside closes, as in any menu */
    return true;
}

static void menu_key(SDL_Keycode k) {
    const int sel = s_depth > 0 ? s_sel[s_depth - 1] : -1;
    switch (k) {
    case SDLK_UP: move_sel(-1); break;
    case SDLK_DOWN: move_sel(1); break;
    case SDLK_RIGHT:
        if (sel >= 0 && s_n[sel].sub) open_sub(sel, true);
        else if (!s_ctx) next_top(1);
        break;
    case SDLK_LEFT:
        if (s_depth > 1) s_depth--;
        else if (!s_ctx) next_top(-1);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate(sel); break;
    case SDLK_ESCAPE: case SDLK_BACKSPACE:
        if (s_depth > 1) s_depth--;
        else set_mode(M_CLOSED);
        break;
    case SDLK_F10: case SDLK_LALT: case SDLK_RALT: set_mode(M_CLOSED); break;
    default: break;
    }
}

/* controller buttons as keys */
static SDL_Keycode pad_key(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return SDLK_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SDLK_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return SDLK_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SDLK_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDLK_RETURN;
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: return SDLK_ESCAPE;
    case SDL_GAMEPAD_BUTTON_GUIDE: case SDL_GAMEPAD_BUTTON_START: return SDLK_F10;
    }
    return SDLK_UNKNOWN;
}

static void modal_key(SDL_Keycode k, SDL_Keymod mod) {
    if (s_mode == M_MENU) menu_key(k);
    else if (s_mode == M_FORM) form_key(k, mod);
    else if (s_mode == M_CONTROLS) ctl_key(k);
}

bool overlay_menu_event(const SDL_Event *e) {
    if (!s_inited || (!s_enabled && !s_in_test)) return false;   /* headless: only the test hook's events */
    const uint64_t now = SDL_GetTicksNS();
    float mx = 0, my = 0;
    if (e->type == SDL_EVENT_MOUSE_MOTION) { mx = e->motion.x * s_density; my = e->motion.y * s_density; }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) { mx = e->button.x * s_density; my = e->button.y * s_density; }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) s_alt_armed = false;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key != SDLK_LALT && e->key.key != SDLK_RALT) s_alt_armed = false;

    if (s_mode == M_CLOSED) {
        switch (e->type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (my < (float)BAR_H() * 1.5f) s_bar_until = now + 1200000000ull;
            return false;   /* the game has no mouse */
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e->button.button == SDL_BUTTON_RIGHT) { open_ctx((int)mx, (int)my); return true; }
            if (s_panel[0] && inside(&s_panel_cancel, mx, my)) {
                rt_log("menu: netplay panel: Cancel");
                frontend_menu_action("netplay-disconnect", NULL);
                return true;
            }
            if (now < s_bar_until) {
                SDL_FRect br[16]; int bn[16];
                const int nb = bar_items(br, bn, 16);
                for (int i = 0; i < nb; i++) if (inside(&br[i], mx, my)) { open_top(bn[i], false); return true; }
            }
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (e->key.key == SDLK_F10) { if (!e->key.repeat) open_top(first_top(), true); return true; }
            if ((e->key.key == SDLK_LALT || e->key.key == SDLK_RALT) && !e->key.repeat) s_alt_armed = !alt_bound();
            return false;
        case SDL_EVENT_KEY_UP:
            if (e->key.key == SDLK_F10) return true;
            if ((e->key.key == SDLK_LALT || e->key.key == SDLK_RALT) && s_alt_armed) {
                s_alt_armed = false;
                open_top(first_top(), true);
                return true;
            }
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) { open_top(first_top(), true); return true; }
            return false;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
        }
        return false;
    }

    /* a menu or form is open: everything that is input is ours */
    if (!is_input_event(e->type)) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        modal_key(e->key.key, e->key.mod);
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (s_mode == M_FORM) form_type(e->text.text);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (s_mode == M_MENU) menu_mouse(mx, my, false);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (s_mode == M_MENU) {
            if (e->button.button == SDL_BUTTON_RIGHT) open_ctx((int)mx, (int)my);
            else menu_mouse(mx, my, true);
        } else if (s_mode == M_FORM) {
            for (int i = 0; i < s_form.n; i++)
                if (inside(&s_form.f[i].r, mx, my)) {
                    s_form.focus = i;
                    if (s_form.f[i].kind != FK_TEXT && s_form.f[i].kind != FK_NUM) form_activate(i);
                    break;
                }
        } else if (s_mode == M_CONTROLS) ctl_click(mx, my);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        SDL_Keycode k = pad_key(e->gbutton.button);
        if (k == SDLK_F10 && s_mode != M_MENU) k = SDLK_ESCAPE;
        if (k != SDLK_UNKNOWN) modal_key(k, 0);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const int a = e->gaxis.axis;
        if (a != SDL_GAMEPAD_AXIS_LEFTX && a != SDL_GAMEPAD_AXIS_LEFTY) break;
        const int d = e->gaxis.value > 16000 ? 1 : e->gaxis.value < -16000 ? -1 : 0, k = a == SDL_GAMEPAD_AXIS_LEFTY;
        if (d && d != s_axis[k]) modal_key(k ? (d > 0 ? SDLK_DOWN : SDLK_UP) : (d > 0 ? SDLK_RIGHT : SDLK_LEFT), 0);
        s_axis[k] = d;
        break;
    }
    }
    return true;
}

/* ---- drawing ---------------------------------------------------------------------------------- */

static void draw_bar(SDL_Renderer *r) {
    const int u = U();
    fill(r, 0, 0, (float)s_cw, (float)BAR_H(), C_BAR);
    fill(r, 0, (float)BAR_H() - (float)u, (float)s_cw, (float)u, C_SEP);
    SDL_FRect br[16]; int bn[16];
    const int nb = bar_items(br, bn, 16);
    for (int i = 0; i < nb; i++) {
        const bool open = s_mode == M_MENU && !s_ctx && s_depth > 0 && s_lvl[0] == bn[i];
        if (open) fill(r, br[i].x, br[i].y, br[i].w, br[i].h - (float)u, C_HI);
        text(r, br[i].x + 8 * (float)u, 3 * (float)u, s_n[bn[i]].title, C_TEXT, 0);
    }
    const char *hint = "F10 / Alt";
    if (s_cw > 60 * CH()) text(r, (float)(s_cw - text_w(hint) - 8 * u), 3 * (float)u, hint, C_OFF, 0);
}

static void draw_pop(SDL_Renderer *r, int k) {
    const int u = U(), ch = CH();
    Pop p;
    pop_layout(k, &p);
    fill(r, p.r.x + 2 * (float)u, p.r.y + 2 * (float)u, p.r.w, p.r.h, (Col){ 0, 0, 0, 90 });   /* shadow */
    fill(r, p.r.x, p.r.y, p.r.w, p.r.h, C_POP);
    frame(r, p.r.x, p.r.y, p.r.w, p.r.h, C_BORDER);
    for (int i = 0; i < p.n; i++) {
        const int node = p.node[i];
        const Node *n = &s_n[node];
        const SDL_FRect it = p.item[i];
        if (n->sep) { fill(r, it.x + 4 * (float)u, it.y + 3 * (float)u, it.w - 8 * (float)u, (float)u, C_SEP); continue; }
        ItemState st;
        item_state(node, &st);
        const bool sel = s_sel[k] == node || (k + 1 < s_depth && s_lvl[k + 1] == node);
        if (sel) fill(r, it.x + (float)u, it.y, it.w - 2 * (float)u, it.h, st.enabled ? C_HI : C_HI_OFF);
        const Col tc = st.enabled ? C_TEXT : C_OFF;
        const float ty = it.y + 3 * (float)u;
        if (st.checked) checkmark(r, it.x + 4 * (float)u, ty, tc);
        int room = (int)((it.w - 24 * (float)u) / (float)ch) - (n->shortcut ? (int)strlen(n->shortcut) + 2 : 0) - (n->sub ? 2 : 0);
        text(r, it.x + 16 * (float)u, ty, st.title, tc, room);
        if (n->shortcut) text(r, it.x + it.w - (float)text_w(n->shortcut) - 8 * (float)u, ty, n->shortcut, C_OFF, 0);
        if (n->sub) arrow(r, it.x + it.w - 9 * (float)u, ty, tc);
    }
}

static void draw_panel(SDL_Renderer *r) {
    const int u = U(), ch = CH();
    float W = (float)(60 * ch);
    if (W > (float)(s_cw - 16 * u)) W = (float)(s_cw - 16 * u);
    char lines[12][128];
    const int nl = wrap(s_panel, (int)((W - 24 * (float)u) / (float)ch), lines, 12);
    const float H = (float)(ITEM_H() + nl * (ch + 3 * u) + ITEM_H() + 24 * u);
    const float X = ((float)s_cw - W) / 2, Y = (float)s_ch - H - 12 * (float)u;
    fill(r, X, Y, W, H, C_POP);
    frame(r, X, Y, W, H, C_BORDER);
    text(r, X + 12 * (float)u, Y + 6 * (float)u, "Netplay", C_ACCENT, 0);
    for (int i = 0; i < nl; i++) text(r, X + 12 * (float)u, Y + (float)ITEM_H() + 4 * (float)u + (float)(i * (ch + 3 * u)), lines[i], C_TEXT, 0);
    const float bw = (float)(10 * ch);
    s_panel_cancel = (SDL_FRect){ X + W - bw - 12 * (float)u, Y + H - (float)ITEM_H() - 8 * (float)u, bw, (float)ITEM_H() };
    fill(r, s_panel_cancel.x, s_panel_cancel.y, bw, s_panel_cancel.h, C_HI_OFF);
    text(r, s_panel_cancel.x + (bw - (float)text_w("Cancel")) / 2, s_panel_cancel.y + 3 * (float)u, "Cancel", C_TEXT, 0);
}

void overlay_menu_draw(SDL_Renderer *r, int w, int h) {
    if (!s_inited || !r || w <= 0 || h <= 0) return;
    s_cw = w; s_ch = h;
    if (s_win) {
        const float d = SDL_GetWindowPixelDensity(s_win);
        s_density = d > 0 ? d : 1;
    }
    const uint64_t now = SDL_GetTicksNS();
    const bool bar = (s_mode == M_MENU && !s_ctx) || now < s_bar_until;
    if (s_mode == M_CLOSED && !bar && !s_panel[0] && now >= s_hint_until) return;
    SDL_BlendMode bm;
    SDL_GetRenderDrawBlendMode(r, &bm);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (s_mode != M_CLOSED) fill(r, 0, 0, (float)w, (float)h, C_DIM);   /* paused */
    if (s_panel[0]) draw_panel(r);
    if (bar) draw_bar(r);
    if (s_mode == M_MENU) for (int k = 0; k < s_depth; k++) draw_pop(r, k);
    if (s_mode == M_FORM) form_draw(r);
    if (s_mode == M_CONTROLS) ctl_draw(r);
    if (s_mode == M_CLOSED && !bar && now < s_hint_until) {
        const char *hint = "Menu: F10, Alt, right-click or the mouse at the top";
        const int u = U();
        const float tw = (float)text_w(hint) + 16 * (float)u;
        fill(r, 8 * (float)u, (float)h - 22 * (float)u, tw, 14 * (float)u, C_BAR);
        text(r, 16 * (float)u, (float)h - 19 * (float)u, hint, C_TEXT, 0);
    }
    SDL_SetRenderDrawBlendMode(r, bm);
}

void overlay_menu_progress(const char *text_) {
    snprintf(s_panel, sizeof s_panel, "%s", text_ ? text_ : "");
    if (text_) rt_log("menu: netplay panel: %s", text_);
}

bool overlay_menu_hold_expired(void) {
    if (!s_hold_ns || SDL_GetTicksNS() < s_hold_ns) return false;
    s_hold_ns = 0;
    set_mode(M_CLOSED);
    rt_log("menu: test: hold over");
    return true;
}

/* ---- test hook: --menu-action F:menu-test=CMD;CMD;... ------------------------------------------
 *   dump                  every item: "menu: item File/Save Games/Import Save Games... [greyed] [checked]"
 *   click=PATH            choose that item as a click does (refused if greyed out)
 *   open=PATH             open the bar's menu PATH (View, View/Renderer), as with the keyboard
 *   context=X,Y           a right-click there
 *   bar                   the hover-revealed bar (as the mouse at the top)
 *   key=NAME              key down + up (SDL scancode names: Down, Return, Escape, F10, Left Alt, Z)
 *   key-down=NAME, key-up=NAME
 *   pad=BUTTON            controller button down + up (south, east, dpdown, guide, ...)
 *   mouse=X,Y  mclick=X,Y  rclick=X,Y   pointer move / left click / right click (canvas pixels)
 *   type=TEXT             text input into the focused form field
 *   controls              open the Controls form
 *   panel=TEXT            show the netplay panel (empty: hide)
 *   state                 log what is open
 *   shot=PATH             the picture with the overlay as PNG
 *   hold=MS               windowed: keep the overlay open MS ms, then close it
 *   close
 * Events go through the frontend's own handler (OverlayHooks.route): overlay first, then the
 * game, exactly as SDL's would. */

static void route(SDL_Event *e) {
    if (s_hooks.route) s_hooks.route(e);
    else overlay_menu_event(e);
}

static void key_event(const char *name, bool down) {
    const SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN) { rt_log("menu: test: unknown key %s", name); return; }
    SDL_Event e = { 0 };
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    e.key.down = down;
    e.key.timestamp = SDL_GetTicksNS();
    route(&e);
}

static void pad_event(const char *name, bool down) {
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    static const char *const face[] = { "south", "east", "west", "north" };   /* positional names, as input.c */
    for (int i = 0; i < 4; i++) if (!strcmp(name, face[i])) b = (SDL_GamepadButton)(SDL_GAMEPAD_BUTTON_SOUTH + i);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { rt_log("menu: test: unknown controller button %s", name); return; }
    SDL_Event e = { 0 };
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.button = (Uint8)b;
    e.gbutton.down = down;
    route(&e);
}

static void mouse_event(const char *xy, int kind) {   /* 0 move, 1 left click, 2 right click */
    float x = 0, y = 0;
    if (sscanf(xy, "%f,%f", &x, &y) != 2) { rt_log("menu: test: X,Y expected"); return; }
    x /= s_density; y /= s_density;
    SDL_Event e = { 0 };
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x; e.motion.y = y;
    route(&e);
    if (!kind) return;
    for (int k = 0; k < 2; k++) {
        SDL_Event b = { 0 };
        b.type = k ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
        b.button.button = kind == 2 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        b.button.down = !k;
        b.button.clicks = 1;
        b.button.x = x; b.button.y = y;
        route(&b);
    }
}

static void dump(int p, const char *prefix) {
    int kids[48];
    const int n = children(p, kids, 48);
    for (int i = 0; i < n; i++) {
        const int c = kids[i];
        if (s_n[c].sep) continue;
        ItemState st;
        item_state(c, &st);
        char path[512];
        snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", st.title);
        if (s_n[c].sub) { dump(c, path); continue; }
        rt_log("menu: item %s%s%s", path, st.enabled ? "" : " [greyed]", st.checked ? " [checked]" : "");
    }
}

/* node of PATH (titles as shown), or -1 */
static int find_path(int p, const char *path) {
    const char *slash = strchr(path, '/');
    const size_t k = slash ? (size_t)(slash - path) : strlen(path);
    int kids[48];
    const int n = children(p, kids, 48);
    for (int i = 0; i < n; i++) {
        if (s_n[kids[i]].sep) continue;
        ItemState st;
        item_state(kids[i], &st);
        if (strlen(st.title) != k || strncmp(st.title, path, k)) continue;
        if (!slash) return kids[i];
        if (s_n[kids[i]].sub) return find_path(kids[i], slash + 1);
    }
    return -1;
}

static void log_state(void) {
    if (s_mode == M_CLOSED) { rt_log("menu: overlay state: closed"); return; }
    if (s_mode == M_FORM) { rt_log("menu: overlay state: form %s, focus %s", s_form.title, s_form.f[s_form.focus].label); return; }
    if (s_mode == M_CONTROLS) { rt_log("menu: overlay state: Controls, player %d, row %d, column %d", s_ctl.player + 1, s_ctl.row, s_ctl.col); return; }
    char path[512] = "";
    for (int k = 0; k < s_depth; k++) {
        if (k == 0 && s_ctx) continue;
        snprintf(path + strlen(path), sizeof path - strlen(path), "%s%s", *path ? "/" : "", s_n[s_lvl[k]].title);
    }
    const int sel = s_depth ? s_sel[s_depth - 1] : -1;
    ItemState st = { 0 };
    if (sel >= 0) item_state(sel, &st);
    rt_log("menu: overlay state: menu %s%s, selected %s", s_ctx ? "(context) " : "", path, sel >= 0 ? st.title : "none");
}

static bool test_one(char *c) {
    char *eq = strchr(c, '=');
    const char *arg = eq ? eq + 1 : "";
    if (eq) *eq = 0;
    if (!strcmp(c, "dump")) { dump(0, ""); return true; }
    if (!strcmp(c, "click")) {
        const int i = find_path(0, arg);
        if (i < 0 || s_n[i].sub) { rt_log("menu: test: no item %s", arg); return false; }
        if (!selectable(i)) { rt_log("menu: test: %s is greyed out", arg); return false; }
        rt_log("menu: test: click %s", arg);
        fire(i);
        return true;
    }
    if (!strcmp(c, "open")) {
        char first[64];
        const char *slash = strchr(arg, '/');
        snprintf(first, sizeof first, "%.*s", slash ? (int)(slash - arg) : (int)strlen(arg), arg);
        const int top = find_path(0, first);
        if (top < 0 || !s_n[top].sub) { rt_log("menu: test: no menu %s", arg); return false; }
        open_top(top, true);
        while (slash) {
            const char *rest = slash + 1;
            slash = strchr(rest, '/');
            char seg[64];
            snprintf(seg, sizeof seg, "%.*s", slash ? (int)(slash - rest) : (int)strlen(rest), rest);
            const int i = find_path(s_lvl[s_depth - 1], seg);
            if (i < 0 || !s_n[i].sub) { rt_log("menu: test: no submenu %s", seg); return false; }
            s_sel[s_depth - 1] = i;
            open_sub(i, true);
        }
        return true;
    }
    if (!strcmp(c, "context")) {
        int x = 0, y = 0;
        sscanf(arg, "%d,%d", &x, &y);
        open_ctx(x, y);
        return true;
    }
    if (!strcmp(c, "bar")) { s_bar_until = SDL_GetTicksNS() + 60000000000ull; return true; }
    if (!strcmp(c, "key")) { key_event(arg, true); key_event(arg, false); return true; }
    if (!strcmp(c, "key-down")) { key_event(arg, true); return true; }
    if (!strcmp(c, "key-up")) { key_event(arg, false); return true; }
    if (!strcmp(c, "pad")) { pad_event(arg, true); pad_event(arg, false); return true; }
    if (!strcmp(c, "mouse")) { mouse_event(arg, 0); return true; }
    if (!strcmp(c, "mclick")) { mouse_event(arg, 1); return true; }
    if (!strcmp(c, "rclick")) { mouse_event(arg, 2); return true; }
    if (!strcmp(c, "type")) {
        SDL_Event e = { 0 };
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = arg;
        route(&e);
        return true;
    }
    if (!strcmp(c, "controls")) { overlay_menu_controls(); return true; }
    if (!strcmp(c, "panel")) { overlay_menu_progress(*arg ? arg : NULL); return true; }
    if (!strcmp(c, "state")) { log_state(); return true; }
    if (!strcmp(c, "shot")) {
        if (s_hooks.shot) s_hooks.shot(arg);
        rt_log("menu: test: shot %s", arg);
        return true;
    }
    if (!strcmp(c, "hold")) { s_hold_ns = SDL_GetTicksNS() + (uint64_t)atoi(arg) * 1000000ull; return true; }
    if (!strcmp(c, "close")) { set_mode(M_CLOSED); s_bar_until = 0; return true; }
    rt_log("menu: test: unknown command %s (dump, click=, open=, key=, pad=, mouse=, mclick=, type=, shot=, close, ...)", c);
    return false;
}

bool overlay_menu_test(const char *cmds) {
    if (!s_inited) overlay_menu_init(NULL, NULL);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", cmds);
    bool ok = true;
    s_in_test = true;
    for (char *c = buf, *next; c && *c; c = next) {
        next = strchr(c, ';');
        if (next) *next++ = 0;
        ok &= test_one(c);
    }
    s_in_test = false;
    return ok;
}
