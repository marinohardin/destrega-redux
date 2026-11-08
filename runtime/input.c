/* Input layer: host keyboard / gamepads -> PS1 pad masks (see input.h). */
#include "input.h"
#include "settings.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void ilog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("input: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

/* ---- PS1 buttons ---------------------------------------------------------------------------- */

static const struct { const char *id, *label; uint16_t mask; } BTN[PSB_COUNT] = {
    { "up", "Up", 0x0010 },        { "down", "Down", 0x0040 },
    { "left", "Left", 0x0080 },    { "right", "Right", 0x0020 },
    { "cross", "Cross", 0x4000 },  { "circle", "Circle", 0x2000 },
    { "square", "Square", 0x8000 }, { "triangle", "Triangle", 0x1000 },
    { "l1", "L1", 0x0400 },        { "r1", "R1", 0x0800 },
    { "l2", "L2", 0x0100 },        { "r2", "R2", 0x0200 },
    { "start", "Start", 0x0008 },  { "select", "Select", 0x0001 },
};
uint16_t input_button_mask(int b) { return b >= 0 && b < PSB_COUNT ? BTN[b].mask : 0; }
const char *input_button_id(int b) { return b >= 0 && b < PSB_COUNT ? BTN[b].id : "?"; }
const char *input_button_label(int b) { return b >= 0 && b < PSB_COUNT ? BTN[b].label : "?"; }
int input_button_from_id(const char *id) {
    for (int b = 0; b < PSB_COUNT; b++) if (ieq(id, BTN[b].id)) return b;
    return -1;
}

/* ---- defaults -------------------------------------------------------------------------------
 * Player 1 keyboard (as before this layer existed): arrows, Z Cross, X Circle, A Square,
 * S Triangle, Q L1, W R1, E L2, R R2, Return Start, Backspace Select.
 * Player 2 keyboard (only used when player 2 is set to the keyboard): I J K L directions,
 * M Cross, Comma Circle, U Square, O Triangle, 7 L1, 8 R1, 9 L2, 0 R2, P Start, Backslash Select.
 * Controllers (both players): south Cross, east Circle, west Square, north Triangle, shoulders
 * L1 / R1, triggers L2 / R2, Start, Back = Select, D-pad and left stick (50 % deadzone). */
void input_defaults_keys(int player, InputPlayer *p) {
    static const uint16_t k1[PSB_COUNT] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE };
    static const uint16_t k2[PSB_COUNT] = {
        SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
        SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_U, SDL_SCANCODE_O,
        SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
        SDL_SCANCODE_P, SDL_SCANCODE_BACKSLASH };
    memcpy(p->key, player ? k2 : k1, sizeof p->key);
}

void input_defaults_pad(InputPlayer *p) {
    static const PadBind d[PSB_COUNT] = {
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_UP, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0 },
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0 },
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_EAST, 0 },
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_WEST, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_NORTH, 0 },
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0 },
        { PADB_AXIS, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1 }, { PADB_AXIS, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1 },
        { PADB_BUTTON, SDL_GAMEPAD_BUTTON_START, 0 }, { PADB_BUTTON, SDL_GAMEPAD_BUTTON_BACK, 0 } };
    memcpy(p->pad, d, sizeof p->pad);
    p->stick = true;
    p->deadzone = 50;
}

void input_defaults(int player, InputPlayer *p) {
    memset(p, 0, sizeof *p);
    p->dev.kind = INDEV_AUTO;
    input_defaults_keys(player, p);
    input_defaults_pad(p);
}

/* ---- mapping ------------------------------------------------------------------------------- */

uint16_t input_map_keys(const InputPlayer *p, const bool *down) {
    uint16_t m = 0;
    for (int b = 0; b < PSB_COUNT; b++)
        if (p->key[b] && p->key[b] < SDL_SCANCODE_COUNT && down[p->key[b]]) m |= BTN[b].mask;
    return m;
}

uint16_t input_stick_dirs(int x, int y, int dz) {
    if (dz < 5) dz = 5;
    if (dz > 95) dz = 95;
    double mag = sqrt((double)x * x + (double)y * y);
    if (mag < 32767.0 * dz / 100.0) return 0;
    double t = mag * 0.38268343;   /* sin(22.5 deg): 8 sectors of 45 degrees */
    uint16_t m = 0;
    if (x > t) m |= BTN[PSB_RIGHT].mask;
    if (x < -t) m |= BTN[PSB_LEFT].mask;
    if (y < -t) m |= BTN[PSB_UP].mask;
    if (y > t) m |= BTN[PSB_DOWN].mask;
    return m;
}

static bool padbind_down(PadBind pb, const PadState *s) {
    if (pb.kind == PADB_BUTTON) return pb.index < SDL_GAMEPAD_BUTTON_COUNT && s->button[pb.index];
    if (pb.kind == PADB_AXIS && pb.index < SDL_GAMEPAD_AXIS_COUNT)
        return (pb.sign < 0 ? -(int)s->axis[pb.index] : (int)s->axis[pb.index]) > INPUT_AXIS_PRESS;
    return false;
}

uint16_t input_map_pad(const InputPlayer *p, const PadState *s) {
    uint16_t m = 0;
    for (int b = 0; b < PSB_COUNT; b++) if (padbind_down(p->pad[b], s)) m |= BTN[b].mask;
    if (p->stick) m |= input_stick_dirs(s->axis[SDL_GAMEPAD_AXIS_LEFTX], s->axis[SDL_GAMEPAD_AXIS_LEFTY], p->deadzone);
    return m;
}

bool input_key_reserved(int sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_TAB: case SDL_SCANCODE_F9: case SDL_SCANCODE_F11:
    case SDL_SCANCODE_F12: case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI:
        return true;
    }
    return false;
}

bool input_bind_key(InputPlayer *p, int b, int sc) {
    if (b < 0 || b >= PSB_COUNT || sc < 0 || sc >= SDL_SCANCODE_COUNT) return false;
    if (sc && input_key_reserved(sc)) return false;
    if (sc) for (int i = 0; i < PSB_COUNT; i++) if (i != b && p->key[i] == sc) p->key[i] = 0;
    p->key[b] = (uint16_t)sc;
    return true;
}

static bool padbind_eq(PadBind a, PadBind b) {
    return a.kind == b.kind && a.kind != PADB_NONE && a.index == b.index && (a.kind != PADB_AXIS || a.sign == b.sign);
}

void input_bind_pad(InputPlayer *p, int b, PadBind pb) {
    if (b < 0 || b >= PSB_COUNT) return;
    for (int i = 0; i < PSB_COUNT; i++) if (i != b && padbind_eq(p->pad[i], pb)) p->pad[i] = (PadBind){ 0 };
    p->pad[b] = pb;
}

/* ---- names ----------------------------------------------------------------------------------- */

static const struct { int sc; const char *id; } KEY_ALIAS[] = {   /* SDL names these by their character */
    { SDL_SCANCODE_COMMA, "Comma" }, { SDL_SCANCODE_PERIOD, "Period" }, { SDL_SCANCODE_SEMICOLON, "Semicolon" },
    { SDL_SCANCODE_APOSTROPHE, "Apostrophe" }, { SDL_SCANCODE_LEFTBRACKET, "LeftBracket" },
    { SDL_SCANCODE_RIGHTBRACKET, "RightBracket" }, { SDL_SCANCODE_BACKSLASH, "Backslash" },
    { SDL_SCANCODE_SLASH, "Slash" }, { SDL_SCANCODE_MINUS, "Minus" }, { SDL_SCANCODE_EQUALS, "Equals" },
    { SDL_SCANCODE_GRAVE, "Grave" }, { SDL_SCANCODE_NONUSBACKSLASH, "NonUSBackslash" },
    { SDL_SCANCODE_NONUSHASH, "NonUSHash" },
};

const char *input_key_id(int sc) {
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "none";
    for (size_t i = 0; i < sizeof KEY_ALIAS / sizeof *KEY_ALIAS; i++) if (KEY_ALIAS[i].sc == sc) return KEY_ALIAS[i].id;
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    if (n && *n && !strchr(n, ',') && !strchr(n, ':') && !strchr(n, '#') && SDL_GetScancodeFromName(n) == (SDL_Scancode)sc) return n;
    static char num[16];
    snprintf(num, sizeof num, "#%d", sc);
    return num;
}

int input_key_from_id(const char *id) {
    if (!id || !*id || ieq(id, "none")) return 0;
    for (size_t i = 0; i < sizeof KEY_ALIAS / sizeof *KEY_ALIAS; i++) if (ieq(id, KEY_ALIAS[i].id)) return KEY_ALIAS[i].sc;
    if (id[0] == '#') { int v = atoi(id + 1); return v > 0 && v < SDL_SCANCODE_COUNT ? v : 0; }
    SDL_Scancode sc = SDL_GetScancodeFromName(id);
    return sc == SDL_SCANCODE_UNKNOWN ? 0 : (int)sc;
}

void input_key_label(int sc, char *out, size_t n) {
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) { snprintf(out, n, "-"); return; }
    SDL_Keycode k = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_KMOD_NONE, false);
    const char *name = k ? SDL_GetKeyName(k) : NULL;
    if (!name || !*name) name = SDL_GetScancodeName((SDL_Scancode)sc);
    snprintf(out, n, "%s", name && *name ? name : "?");
}

static const char *const PADBTN_ID[SDL_GAMEPAD_BUTTON_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
    "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad",
    "misc2", "misc3", "misc4", "misc5", "misc6" };
static const char *const PADAXIS_ID[SDL_GAMEPAD_AXIS_COUNT] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };

static bool is_trigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

void input_padbind_id(PadBind pb, char *out, size_t n) {
    if (pb.kind == PADB_BUTTON && pb.index < SDL_GAMEPAD_BUTTON_COUNT) snprintf(out, n, "%s", PADBTN_ID[pb.index]);
    else if (pb.kind == PADB_AXIS && pb.index < SDL_GAMEPAD_AXIS_COUNT)
        snprintf(out, n, "%s%s", PADAXIS_ID[pb.index], is_trigger(pb.index) ? "" : pb.sign < 0 ? "-" : "+");
    else snprintf(out, n, "none");
}

bool input_padbind_from_id(const char *id, PadBind *pb) {
    *pb = (PadBind){ 0 };
    if (!id || ieq(id, "none") || !*id) return true;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
        if (ieq(id, PADBTN_ID[i])) { *pb = (PadBind){ PADB_BUTTON, (uint8_t)i, 0 }; return true; }
    char base[32];
    snprintf(base, sizeof base, "%s", id);
    size_t l = strlen(base);
    int sign = 1;
    if (l && (base[l - 1] == '+' || base[l - 1] == '-')) { sign = base[l - 1] == '-' ? -1 : 1; base[--l] = 0; }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
        if (ieq(base, PADAXIS_ID[i])) {
            if (is_trigger(i)) sign = 1;
            else if (id[strlen(id) - 1] != '+' && id[strlen(id) - 1] != '-') return false;   /* a stick needs a direction */
            *pb = (PadBind){ PADB_AXIS, (uint8_t)i, (int8_t)sign };
            return true;
        }
    return false;
}

/* The controller's own names. Face buttons from SDL (PlayStation symbols, A/B/X/Y by layout). */
void input_padbind_label(PadBind pb, int type, char *out, size_t n) {
    bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    bool nin = type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    if (pb.kind == PADB_AXIS && pb.index < SDL_GAMEPAD_AXIS_COUNT) {
        static const char *const st[4] = { "Left Stick", "Left Stick", "Right Stick", "Right Stick" };
        if (pb.index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) snprintf(out, n, "%s", ps ? "L2" : nin ? "ZL" : "LT");
        else if (pb.index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) snprintf(out, n, "%s", ps ? "R2" : nin ? "ZR" : "RT");
        else {
            bool x = pb.index == SDL_GAMEPAD_AXIS_LEFTX || pb.index == SDL_GAMEPAD_AXIS_RIGHTX;
            snprintf(out, n, "%s %s", st[pb.index], x ? (pb.sign < 0 ? "Left" : "Right") : (pb.sign < 0 ? "Up" : "Down"));
        }
        return;
    }
    if (pb.kind != PADB_BUTTON || pb.index >= SDL_GAMEPAD_BUTTON_COUNT) { snprintf(out, n, "-"); return; }
    int b = pb.index;
    if (b <= SDL_GAMEPAD_BUTTON_NORTH) {
        static const char *const lab[] = { "?", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle" };
        static const char *const pos[] = { "South", "East", "West", "North" };
        SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, (SDL_GamepadButton)b);
        if (type == SDL_GAMEPAD_TYPE_UNKNOWN || type == SDL_GAMEPAD_TYPE_STANDARD) {
            static const char *const xb[] = { "A", "B", "X", "Y" };   /* the common layout */
            snprintf(out, n, "%s", xb[b]);
        } else snprintf(out, n, "%s", l > 0 && l <= 8 ? lab[l] : pos[b]);
        return;
    }
    const char *s = NULL;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_BACK:
        s = type == SDL_GAMEPAD_TYPE_PS4 ? "Share" : type == SDL_GAMEPAD_TYPE_PS5 ? "Create" : ps ? "Select"
          : nin ? "Minus" : (type == SDL_GAMEPAD_TYPE_XBOXONE ? "View" : "Back"); break;
    case SDL_GAMEPAD_BUTTON_START:
        s = type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5 ? "Options" : nin ? "Plus"
          : type == SDL_GAMEPAD_TYPE_XBOXONE ? "Menu" : "Start"; break;
    case SDL_GAMEPAD_BUTTON_GUIDE: s = ps ? "PS Button" : nin ? "Home" : "Guide"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: s = ps ? "L3" : "Left Stick Press"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: s = ps ? "R3" : "Right Stick Press"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: s = ps ? "L1" : nin ? "L" : "LB"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: s = ps ? "R1" : nin ? "R" : "RB"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: s = "D-pad Up"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: s = "D-pad Down"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: s = "D-pad Left"; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s = "D-pad Right"; break;
    case SDL_GAMEPAD_BUTTON_MISC1: s = type == SDL_GAMEPAD_TYPE_PS5 ? "Mute" : nin ? "Capture" : "Share"; break;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: s = "Touchpad"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: s = "Paddle 1"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: s = "Paddle 3"; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: s = "Paddle 2"; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: s = "Paddle 4"; break;
    }
    if (s) snprintf(out, n, "%s", s);
    else snprintf(out, n, "Button %d", b);
}

void input_device_id(const InputDevice *d, char *out, size_t n) {
    switch (d->kind) {
    case INDEV_KEYBOARD: snprintf(out, n, "keyboard"); break;
    case INDEV_GAMEPAD: snprintf(out, n, "gamepad"); break;
    case INDEV_GAMEPAD_GUID: snprintf(out, n, "gamepad:%s", d->guid); break;
    case INDEV_NONE: snprintf(out, n, "none"); break;
    default: snprintf(out, n, "auto"); break;
    }
}

bool input_device_from_id(const char *id, InputDevice *d) {
    InputDevice r = { 0 };
    if (ieq(id, "auto")) r.kind = INDEV_AUTO;
    else if (ieq(id, "keyboard")) r.kind = INDEV_KEYBOARD;
    else if (ieq(id, "gamepad") || ieq(id, "controller")) r.kind = INDEV_GAMEPAD;
    else if (ieq(id, "none")) r.kind = INDEV_NONE;
    else if (!strncmp(id, "gamepad:", 8) && strlen(id + 8) == 32) {
        for (const char *p = id + 8; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
        r.kind = INDEV_GAMEPAD_GUID;
        snprintf(r.guid, sizeof r.guid, "%s", id + 8);
        for (char *p = r.guid; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else return false;
    *d = r;
    return true;
}

/* ---- settings -------------------------------------------------------------------------------- */

static const char *sec_of(int player) { return player ? "player2" : "player1"; }

void input_player_load(int player, InputPlayer *p) {
    input_defaults(player, p);
    const char *sec = sec_of(player), *v;
    if ((v = settings_get(sec, "device")) && !input_device_from_id(v, &p->dev)) ilog("%s.device: unknown value '%s' (kept auto)", sec, v);
    if ((v = settings_get(sec, "device_name"))) snprintf(p->dev.name, sizeof p->dev.name, "%s", v);
    p->stick = settings_get_bool(sec, "stick", p->stick);
    int dz = settings_get_int(sec, "deadzone", p->deadzone);
    if (dz >= 5 && dz <= 95) p->deadzone = dz;
    bool set_k[PSB_COUNT] = { 0 }, set_p[PSB_COUNT] = { 0 };
    for (int b = 0; b < PSB_COUNT; b++) {
        char k[32];
        snprintf(k, sizeof k, "key.%s", BTN[b].id);
        if ((v = settings_get(sec, k))) {
            int sc = input_key_from_id(v);
            if ((!sc && !ieq(v, "none")) || (sc && input_key_reserved(sc))) ilog("%s.%s: '%s' cannot be used (kept the default)", sec, k, v);
            else { p->key[b] = (uint16_t)sc; set_k[b] = true; }
        }
        snprintf(k, sizeof k, "pad.%s", BTN[b].id);
        if ((v = settings_get(sec, k))) {
            PadBind pb;
            if (input_padbind_from_id(v, &pb)) { p->pad[b] = pb; set_p[b] = true; }
            else ilog("%s.%s: unknown controller input '%s' (kept the default)", sec, k, v);
        }
    }
    /* an input the file moved to another button leaves the button it had by default (as rebinding
     * in the Controls window does); two buttons the file itself gives the same input keep it */
    for (int b = 0; b < PSB_COUNT; b++)
        for (int o = 0; o < PSB_COUNT; o++) {
            if (o == b) continue;
            if (set_k[b] && !set_k[o] && p->key[b] && p->key[o] == p->key[b]) p->key[o] = 0;
            if (set_p[b] && !set_p[o] && padbind_eq(p->pad[o], p->pad[b])) p->pad[o] = (PadBind){ 0 };
        }
}

void input_player_store(int player, const InputPlayer *p) {
    const char *sec = sec_of(player);
    char v[64], k[32];
    input_device_id(&p->dev, v, sizeof v);
    settings_set(sec, "device", v);
    settings_set(sec, "device_name", p->dev.kind == INDEV_GAMEPAD_GUID && p->dev.name[0] ? p->dev.name : NULL);
    settings_set_bool(sec, "stick", p->stick);
    settings_set_int(sec, "deadzone", p->deadzone);
    for (int b = 0; b < PSB_COUNT; b++) {
        snprintf(k, sizeof k, "key.%s", BTN[b].id);
        settings_set(sec, k, input_key_id(p->key[b]));
    }
    for (int b = 0; b < PSB_COUNT; b++) {
        snprintf(k, sizeof k, "pad.%s", BTN[b].id);
        input_padbind_id(p->pad[b], v, sizeof v);
        settings_set(sec, k, v);
    }
}

/* ---- controller assignment ------------------------------------------------------------------ */

void input_assign(const InputPlayer pl[2], const char *const *guids, int n, const int prev[2], int pad_of[2], bool kbd[2]) {
    bool used[16] = { 0 };
    for (int p = 0; p < 2; p++) pad_of[p] = -1;
    for (int p = 0; p < 2; p++)   /* specific controllers first */
        if (pl[p].dev.kind == INDEV_GAMEPAD_GUID)
            for (int i = 0; i < n && i < 16; i++)
                if (!used[i] && ieq(guids[i], pl[p].dev.guid)) { pad_of[p] = i; used[i] = true; break; }
    for (int p = 0; p < 2; p++)   /* "any" / auto keep the controller they had (no reshuffle on unplug) */
        if ((pl[p].dev.kind == INDEV_GAMEPAD || pl[p].dev.kind == INDEV_AUTO) && prev && prev[p] >= 0 &&
            prev[p] < n && prev[p] < 16 && !used[prev[p]]) { pad_of[p] = prev[p]; used[prev[p]] = true; }
    for (int p = 0; p < 2; p++)   /* then the rest, in connection order */
        if (pad_of[p] < 0 && (pl[p].dev.kind == INDEV_GAMEPAD || pl[p].dev.kind == INDEV_AUTO))
            for (int i = 0; i < n && i < 16; i++)
                if (!used[i]) { pad_of[p] = i; used[i] = true; break; }
    bool explicit_kbd = false;
    for (int p = 0; p < 2; p++) { kbd[p] = pl[p].dev.kind == INDEV_KEYBOARD; explicit_kbd |= kbd[p]; }
    if (!explicit_kbd && pl[0].dev.kind == INDEV_AUTO) kbd[0] = true;
}

/* ---- host side ------------------------------------------------------------------------------ */

#define MAX_PADS 8
typedef struct { SDL_JoystickID id; SDL_Gamepad *g; char guid[40], name[128]; int type; } Pad;
static Pad s_pads[MAX_PADS];
static int s_npads;
static InputPlayer s_pl[2];
static int s_pad_of[2] = { -1, -1 };
static bool s_kbd[2];
static bool s_keys[SDL_SCANCODE_COUNT];
static uint16_t s_last[2];
static struct { bool armed; int player, b, kind; } s_cap;
static char s_cap_msg[200];

static SDL_JoystickID s_had[2];   /* controller each player had (sticky across hot-plug) */
static bool s_logged;

static void reassign(void) {
    const char *g[MAX_PADS];
    int prev[2] = { -1, -1 };
    for (int i = 0; i < s_npads; i++) {
        g[i] = s_pads[i].guid;
        for (int p = 0; p < 2; p++) if (s_had[p] && s_pads[i].id == s_had[p]) prev[p] = i;
    }
    SDL_JoystickID old[2] = { s_had[0], s_had[1] };
    bool okb[2] = { s_kbd[0], s_kbd[1] };
    input_assign(s_pl, g, s_npads, prev, s_pad_of, s_kbd);
    for (int p = 0; p < 2; p++) s_had[p] = s_pad_of[p] >= 0 ? s_pads[s_pad_of[p]].id : 0;
    bool first = !s_logged;
    s_logged = true;
    for (int p = 0; p < 2; p++)
        if (first || old[p] != s_had[p] || okb[p] != s_kbd[p])
            ilog("player %d: %s%s%s", p + 1, s_kbd[p] ? "keyboard" : "",
                 s_kbd[p] && s_pad_of[p] >= 0 ? " + " : "",
                 s_pad_of[p] >= 0 ? s_pads[s_pad_of[p]].name : s_kbd[p] ? "" : "no input");
}

void input_init(void) {
    for (int p = 0; p < 2; p++) input_player_load(p, &s_pl[p]);
    reassign();
}

static void pad_added(SDL_JoystickID id) {
    for (int i = 0; i < s_npads; i++) if (s_pads[i].id == id) return;
    if (s_npads >= MAX_PADS) return;
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { ilog("cannot open controller %u: %s", (unsigned)id, SDL_GetError()); return; }
    Pad *p = &s_pads[s_npads++];
    p->id = id;
    p->g = g;
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), p->guid, sizeof p->guid);
    const char *nm = SDL_GetGamepadName(g);
    snprintf(p->name, sizeof p->name, "%s", nm && *nm ? nm : "Controller");
    p->type = SDL_GetGamepadType(g);
    ilog("controller connected: %s (%s)", p->name, p->guid);
    reassign();
}

static void pad_removed(SDL_JoystickID id) {
    for (int i = 0; i < s_npads; i++)
        if (s_pads[i].id == id) {
            ilog("controller disconnected: %s", s_pads[i].name);
            SDL_CloseGamepad(s_pads[i].g);
            memmove(&s_pads[i], &s_pads[i + 1], (size_t)(s_npads - i - 1) * sizeof *s_pads);
            s_npads--;
            reassign();
            return;
        }
}

void input_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int sc = e->key.scancode;
        if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) break;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && (e->key.mod & SDL_KMOD_GUI)) break;   /* Cmd / Windows key combinations are shortcuts */
        if (down && input_key_reserved(sc)) break;
        s_keys[sc] = down;
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: input_release_all(); break;
    }
}

void input_release_all(void) { memset(s_keys, 0, sizeof s_keys); }

static void read_pad(const Pad *p, PadState *s) {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) s->button[i] = SDL_GetGamepadButton(p->g, (SDL_GamepadButton)i);
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++) s->axis[i] = SDL_GetGamepadAxis(p->g, (SDL_GamepadAxis)i);
}

void input_frame(uint16_t out[2]) {
    for (int p = 0; p < 2; p++) {
        uint16_t m = 0;
        if (s_kbd[p]) m |= input_map_keys(&s_pl[p], s_keys);
        if (s_pad_of[p] >= 0 && s_pad_of[p] < s_npads) {
            PadState st;
            read_pad(&s_pads[s_pad_of[p]], &st);
            m |= input_map_pad(&s_pl[p], &st);
        }
        if (s_cap.armed) m = 0;   /* press-to-bind: the game does not see the input being bound */
        s_last[p] = m;
        out[p] = m;
    }
}

InputPlayer *input_player(int player) { return &s_pl[player ? 1 : 0]; }
void input_changed(int player) {
    reassign();
    input_player_store(player ? 1 : 0, &s_pl[player ? 1 : 0]);
    settings_save();
}
void input_reset(int player) {
    input_defaults(player ? 1 : 0, &s_pl[player ? 1 : 0]);
    input_changed(player);
}

int input_pad_count(void) { return s_npads; }
bool input_pad_info(int i, InputPadInfo *o) {
    if (i < 0 || i >= s_npads) return false;
    snprintf(o->name, sizeof o->name, "%s", s_pads[i].name);
    snprintf(o->guid, sizeof o->guid, "%s", s_pads[i].guid);
    o->type = s_pads[i].type;
    o->player = s_pad_of[0] == i ? 0 : s_pad_of[1] == i ? 1 : -1;
    return true;
}
int input_player_pad_type(int player) {
    int i = s_pad_of[player ? 1 : 0];
    if (i >= 0 && i < s_npads) return s_pads[i].type;
    return s_npads ? s_pads[0].type : SDL_GAMEPAD_TYPE_UNKNOWN;
}
uint16_t input_last_mask(int player) { return s_last[player ? 1 : 0]; }
bool input_player_has_keyboard(int player) { return s_kbd[player ? 1 : 0]; }

/* ---- press-to-bind --------------------------------------------------------------------------- */

void input_capture_begin(int player, int b, int kind) {
    s_cap.armed = true;
    s_cap.player = player ? 1 : 0;
    s_cap.b = b;
    s_cap.kind = kind;
    snprintf(s_cap_msg, sizeof s_cap_msg, "Player %d %s: %s", s_cap.player + 1, BTN[b].label,
             kind ? "press a button on the controller (Escape cancels)" : "press a key (Escape cancels)");
}
void input_capture_cancel(void) { if (s_cap.armed) { s_cap.armed = false; s_cap_msg[0] = 0; } }
bool input_capture_armed(int *player, int *b, int *kind) {
    if (player) *player = s_cap.player;
    if (b) *b = s_cap.b;
    if (kind) *kind = s_cap.kind;
    return s_cap.armed;
}
const char *input_capture_message(void) { return s_cap_msg; }

static void cap_done(void) {
    s_cap.armed = false;
    s_cap_msg[0] = 0;
    input_changed(s_cap.player);
}

bool input_capture_key(int sc) {
    if (!s_cap.armed) return false;
    if (sc == SDL_SCANCODE_ESCAPE) { input_capture_cancel(); return true; }
    if (s_cap.kind != 0) return true;   /* waiting for a controller: keys other than Escape do nothing */
    if (!input_bind_key(&s_pl[s_cap.player], s_cap.b, sc)) {
        char l[64];
        input_key_label(sc, l, sizeof l);
        snprintf(s_cap_msg, sizeof s_cap_msg, "%s is used by the game window (quit, fast forward, full screen, "
                 "widescreen, screenshot or menu shortcuts). Press another key.", l);
        return true;
    }
    ilog("player %d %s = key %s", s_cap.player + 1, BTN[s_cap.b].id, input_key_id(sc));
    cap_done();
    return true;
}

bool input_capture_event(const SDL_Event *e) {
    if (!s_cap.armed) return false;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        if (e->key.repeat) return true;
        if ((e->key.mod & SDL_KMOD_GUI) && e->key.scancode != SDL_SCANCODE_ESCAPE) return false;   /* menu shortcut */
        return input_capture_key(e->key.scancode);
    }
    if (s_cap.kind != 1) return false;
    PadBind pb = { 0 };
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pb = (PadBind){ PADB_BUTTON, e->gbutton.button, 0 };
    else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 24000 || e->gaxis.value < -24000)) {
        int a = e->gaxis.axis;
        pb = (PadBind){ PADB_AXIS, (uint8_t)a, (int8_t)(is_trigger(a) || e->gaxis.value > 0 ? 1 : -1) };
    } else return e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    input_bind_pad(&s_pl[s_cap.player], s_cap.b, pb);
    char id[32];
    input_padbind_id(pb, id, sizeof id);
    ilog("player %d %s = controller %s", s_cap.player + 1, BTN[s_cap.b].id, id);
    cap_done();
    return true;
}

void input_set_background(bool on) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
}

/* ---- test hook: injected events ------------------------------------------------------------- */

enum { EV_KEY_DOWN, EV_KEY_UP, EV_PAD_ADD, EV_PAD_REMOVE, EV_PAD_DOWN, EV_PAD_UP, EV_PAD_AXIS };
typedef struct { uint64_t frame; int type, pad, code, value; } InEvent;
static InEvent *s_ev;
static int s_nev, s_cap_ev;
static struct { SDL_JoystickID id; SDL_Joystick *j; } s_vpad[4];

bool input_parse_events(const char *spec) {
    char buf[256];
    const char *p = spec;
    while (*p) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l >= sizeof buf) { ilog("--input-event: entry too long"); return false; }
        memcpy(buf, p, l);
        buf[l] = 0;
        p += l + (c ? 1 : 0);
        char *f[5] = { 0 };
        int nf = 0;
        for (char *t = buf; nf < 5; ) { f[nf++] = t; char *x = strchr(t, ':'); if (!x) break; *x = 0; t = x + 1; }
        InEvent ev = { 0 };
        char *end;
        ev.frame = strtoull(f[0], &end, 0);
        bool ok = nf >= 2 && !*end;
        const char *k = ok ? f[1] : "";
        if (ok && (!strcmp(k, "key-down") || !strcmp(k, "key-up"))) {
            ev.type = k[4] == 'd' ? EV_KEY_DOWN : EV_KEY_UP;
            ok = nf == 3 && (ev.code = input_key_from_id(f[2])) != 0;
        } else if (ok && (!strcmp(k, "pad-add") || !strcmp(k, "pad-remove"))) {
            ev.type = k[4] == 'a' ? EV_PAD_ADD : EV_PAD_REMOVE;
            ev.pad = nf >= 3 ? atoi(f[2]) : 0;
            ev.code = nf >= 4 && !strcmp(f[3], "ps4") ? 1 : nf >= 4 && !strcmp(f[3], "xbox") ? 2 : 0;
        } else if (ok && (!strcmp(k, "pad-down") || !strcmp(k, "pad-up"))) {
            PadBind pb;
            ev.type = k[4] == 'd' ? EV_PAD_DOWN : EV_PAD_UP;
            ok = nf == 4 && input_padbind_from_id(f[3], &pb) && pb.kind == PADB_BUTTON;
            ev.pad = ok ? atoi(f[2]) : 0;
            ev.code = ok ? pb.index : 0;
        } else if (ok && !strcmp(k, "pad-axis")) {
            ev.type = EV_PAD_AXIS;
            ok = nf == 5;
            int a = -1;
            for (int i = 0; ok && i < SDL_GAMEPAD_AXIS_COUNT; i++) if (ieq(f[3], PADAXIS_ID[i])) a = i;
            ok = ok && a >= 0;
            if (ok) { ev.pad = atoi(f[2]); ev.code = a; ev.value = atoi(f[4]); }
        } else ok = false;
        if (!ok || ev.pad < 0 || ev.pad > 3) { ilog("--input-event: cannot parse '%.*s'", (int)l, p - l - (c ? 1 : 0)); return false; }
        if (s_nev == s_cap_ev) {
            s_cap_ev = s_cap_ev ? 2 * s_cap_ev : 256;
            s_ev = realloc(s_ev, (size_t)s_cap_ev * sizeof *s_ev);
            if (!s_ev) return false;
        }
        s_ev[s_nev++] = ev;
    }
    return true;
}

bool input_have_events(void) { return s_nev > 0; }

static void inject_one(const InEvent *ev) {
    switch (ev->type) {
    case EV_KEY_DOWN:
    case EV_KEY_UP: {
        SDL_Event e = { 0 };
        e.type = ev->type == EV_KEY_DOWN ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = (SDL_Scancode)ev->code;
        e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)ev->code, SDL_KMOD_NONE, false);
        e.key.down = ev->type == EV_KEY_DOWN;
        e.key.timestamp = SDL_GetTicksNS();
        if (!SDL_PushEvent(&e)) ilog("inject: %s", SDL_GetError());
        break;
    }
    case EV_PAD_ADD: {
        if (s_vpad[ev->pad].j) break;
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        d.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
        if (ev->code == 1) { d.vendor_id = 0x054c; d.product_id = 0x09cc; d.name = "Virtual PS4 Controller"; }
        else if (ev->code == 2) { d.vendor_id = 0x045e; d.product_id = 0x02ea; d.name = "Virtual Xbox Controller"; }
        else d.name = "Virtual Controller";
        SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        SDL_Joystick *j = id ? SDL_OpenJoystick(id) : NULL;
        if (!j) { ilog("inject: cannot attach a virtual controller: %s", SDL_GetError()); break; }
        s_vpad[ev->pad].id = id;
        s_vpad[ev->pad].j = j;
        break;
    }
    case EV_PAD_REMOVE:
        if (!s_vpad[ev->pad].j) break;
        SDL_CloseJoystick(s_vpad[ev->pad].j);
        SDL_DetachVirtualJoystick(s_vpad[ev->pad].id);
        s_vpad[ev->pad].j = NULL;
        break;
    case EV_PAD_DOWN:
    case EV_PAD_UP:
        if (s_vpad[ev->pad].j) SDL_SetJoystickVirtualButton(s_vpad[ev->pad].j, ev->code, ev->type == EV_PAD_DOWN);
        break;
    case EV_PAD_AXIS: {
        if (!s_vpad[ev->pad].j) break;
        int v = ev->value;
        /* virtual triggers span the whole raw range (rest = -32768); the gamepad reports 0..32767 */
        if (is_trigger(ev->code)) v = v * 2 - 32768;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        SDL_SetJoystickVirtualAxis(s_vpad[ev->pad].j, ev->code, (Sint16)v);
        break;
    }
    }
}

void input_inject(uint64_t frame) {
    for (int i = 0; i < s_nev; i++) if (s_ev[i].frame == frame) inject_one(&s_ev[i]);
}

static FILE *s_log;
void input_set_log(FILE *f) { s_log = f; }
void input_log_frame(uint64_t frame, const uint16_t m[2]) {
    static uint16_t last[2];
    static bool first = true;
    if (!s_log || (!first && m[0] == last[0] && m[1] == last[1])) return;
    fprintf(s_log, "%llu %04x %04x\n", (unsigned long long)frame, m[0], m[1]);
    last[0] = m[0]; last[1] = m[1];
    first = false;
}

/* ---- menu actions (--menu-action and the Controls window share them) -------------------------
 *   controls                         open the Controls window (windowed)
 *   controls-bind=P1:cross=key:Z     also key:none, pad:east, pad:lefttrigger, pad:leftx-, pad:none
 *   controls-device=P2:keyboard      auto | keyboard | gamepad | gamepad:<GUID> | none
 *   controls-stick=P1:on|off         left stick moves too
 *   controls-deadzone=P1:40          percent, 5..95
 *   controls-reset=P1|P2|all         defaults
 *   controls-dump                    log both players' bindings and the controllers */
static int parse_player(const char **arg) {
    const char *a = *arg;
    if ((a[0] == 'P' || a[0] == 'p') && (a[1] == '1' || a[1] == '2') && (a[2] == ':' || !a[2])) {
        *arg = a + (a[2] ? 3 : 2);
        return a[1] - '1';
    }
    return -1;
}

static void dump(void) {
    for (int p = 0; p < 2; p++) {
        char dev[80], line[1600];
        input_device_id(&s_pl[p].dev, dev, sizeof dev);
        int o = snprintf(line, sizeof line, "P%d device=%s stick=%s deadzone=%d |", p + 1, dev, s_pl[p].stick ? "on" : "off", s_pl[p].deadzone);
        for (int b = 0; b < PSB_COUNT && o < (int)sizeof line - 64; b++) {
            char pid[32];
            input_padbind_id(s_pl[p].pad[b], pid, sizeof pid);
            o += snprintf(line + o, sizeof line - (size_t)o, " %s=%s/%s", BTN[b].id, input_key_id(s_pl[p].key[b]), pid);
        }
        ilog("%s", line);
        ilog("P%d reads %s%s%s", p + 1, s_kbd[p] ? "the keyboard" : "", s_kbd[p] && s_pad_of[p] >= 0 ? " and " : "",
             s_pad_of[p] >= 0 ? s_pads[s_pad_of[p]].name : s_kbd[p] ? "" : "nothing");
    }
    for (int i = 0; i < s_npads; i++) ilog("controller %d: %s (%s, type %d)", i + 1, s_pads[i].name, s_pads[i].guid, s_pads[i].type);
}

bool input_menu_action(const char *name, const char *arg) {
    if (!strcmp(name, "controls-dump")) { dump(); return true; }
    if (!strcmp(name, "controls-reset")) {
        if (!strcmp(arg, "all") || !*arg) { input_reset(0); input_reset(1); }
        else { int p = parse_player(&arg); if (p < 0) goto bad; input_reset(p); }
        ilog("controls reset: %s", *arg ? arg : "all");
        return true;
    }
    int p = parse_player(&arg);
    if (p < 0) goto bad;
    InputPlayer *pl = &s_pl[p];
    if (!strcmp(name, "controls-bind")) {   /* BUTTON=key:NAME | BUTTON=pad:ID */
        const char *eq = strchr(arg, '=');
        char bname[32];
        if (!eq || eq - arg >= (int)sizeof bname) goto bad;
        snprintf(bname, sizeof bname, "%.*s", (int)(eq - arg), arg);
        int b = input_button_from_id(bname);
        const char *v = eq + 1;
        if (b < 0) goto bad;
        if (!strncmp(v, "key:", 4) || !strncmp(v, "KEY:", 4)) {
            int sc = input_key_from_id(v + 4);
            if (!sc && !ieq(v + 4, "none")) goto bad;
            if (!input_bind_key(pl, b, sc)) { ilog("menu action %s: %s is used by the game window", name, v + 4); return false; }
        } else if (!strncmp(v, "pad:", 4) || !strncmp(v, "PAD:", 4)) {
            PadBind pb;
            if (!input_padbind_from_id(v + 4, &pb)) goto bad;
            input_bind_pad(pl, b, pb);
        } else goto bad;
    } else if (!strcmp(name, "controls-device")) {
        InputDevice d;
        if (!input_device_from_id(arg, &d)) goto bad;
        for (int i = 0; d.kind == INDEV_GAMEPAD_GUID && i < s_npads; i++)
            if (ieq(s_pads[i].guid, d.guid)) snprintf(d.name, sizeof d.name, "%s", s_pads[i].name);
        pl->dev = d;
    } else if (!strcmp(name, "controls-stick")) {
        if (ieq(arg, "on") || ieq(arg, "1")) pl->stick = true;
        else if (ieq(arg, "off") || ieq(arg, "0")) pl->stick = false;
        else goto bad;
    } else if (!strcmp(name, "controls-deadzone")) {
        int dz = atoi(arg);
        if (dz < 5 || dz > 95) goto bad;
        pl->deadzone = dz;
    } else goto bad;
    input_changed(p);
    ilog("%s P%d %s: done", name, p + 1, arg);
    return true;
bad:
    ilog("menu action %s: cannot use '%s'", name, arg);
    return false;
}
