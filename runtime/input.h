/* Input layer: the one place where host input (keyboard, SDL gamepads) becomes the two PS1 pad
 * masks per frame.
 *
 * Each player (1, 2) has a device choice and two binding sets, one for the keyboard and one for
 * gamepads, for the 14 buttons of the PS1 digital pad (the game only ever reads a digital pad:
 * analysis/NOTES.md "Input / hardware use"). The D-pad buttons and, optionally, the left stick
 * (8-way, radial deadzone) give the directions.
 *
 * Devices: auto (player 1: the keyboard plus the first controller; player 2: the next
 * controller), keyboard, any controller (the next free one), a specific controller (by SDL GUID;
 * with several identical ones, the first free one), none. Controllers are claimed in connection
 * order after the specific ones; hot-plug re-runs the assignment, and a player keeps the
 * controller it has while it stays connected.
 *
 * What it does not do: it never writes g_pad_buttons itself. main.c asks for the masks
 * (input_frame) and puts them into the pads, after which replays and netplay may override them
 * exactly as before (netplay sends pad 1 = this machine's player 1 as the local input).
 *
 * Keys used by the game window itself are never bound and never reach a pad: Escape (quit), Tab
 * (fast forward), F9 (widescreen), F11 / Alt+Enter / Ctrl+Cmd+F (full screen), F12 (screenshot)
 * and the Cmd / Windows keys (menu shortcuts; any key pressed with them held is ignored). */
#ifndef INPUT_H
#define INPUT_H
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PSB_UP, PSB_DOWN, PSB_LEFT, PSB_RIGHT,
    PSB_CROSS, PSB_CIRCLE, PSB_SQUARE, PSB_TRIANGLE,
    PSB_L1, PSB_R1, PSB_L2, PSB_R2,
    PSB_START, PSB_SELECT,
    PSB_COUNT
};
/* PS1 pad bit (active high, as --press) of each button */
uint16_t input_button_mask(int b);
const char *input_button_id(int b);      /* "cross" (settings / menu actions) */
const char *input_button_label(int b);   /* "Cross" (UI) */
int input_button_from_id(const char *id);   /* case-insensitive; -1 if unknown */

/* A gamepad binding: a button, or one direction of an axis (triggers: sign +1). */
enum { PADB_NONE, PADB_BUTTON, PADB_AXIS };
typedef struct { uint8_t kind, index; int8_t sign; } PadBind;

enum { INDEV_AUTO, INDEV_KEYBOARD, INDEV_GAMEPAD, INDEV_GAMEPAD_GUID, INDEV_NONE };
typedef struct {
    int kind;
    char guid[40];       /* INDEV_GAMEPAD_GUID */
    char name[128];      /* its name, for display while it is not connected */
} InputDevice;

typedef struct {
    InputDevice dev;
    uint16_t key[PSB_COUNT];   /* SDL_Scancode, 0 = unbound */
    PadBind pad[PSB_COUNT];
    bool stick;                /* left stick moves too */
    int deadzone;              /* percent of full deflection, 5..95 */
} InputPlayer;

/* ---- pure mapping (no SDL state; unit-tested) ---- */
void input_defaults(int player, InputPlayer *p);   /* player 0 or 1 */
void input_defaults_keys(int player, InputPlayer *p);
void input_defaults_pad(InputPlayer *p);
uint16_t input_map_keys(const InputPlayer *p, const bool *down);   /* down[SDL_SCANCODE_COUNT] */
typedef struct { bool button[SDL_GAMEPAD_BUTTON_COUNT]; int16_t axis[SDL_GAMEPAD_AXIS_COUNT]; } PadState;
uint16_t input_map_pad(const InputPlayer *p, const PadState *s);
/* Direction bits for stick position x, y (-32768..32767; y down positive) with a radial deadzone
 * of dz percent: none inside it, else 8 sectors of 45 degrees. */
uint16_t input_stick_dirs(int x, int y, int dz);
#define INPUT_AXIS_PRESS 16384   /* an axis bound as a button is pressed beyond 50 % */
bool input_key_reserved(int scancode);
/* Bind a key / gamepad input to button b; another button of the same player and kind with the
 * same input is cleared (one input, one button). False for reserved keys. scancode 0 / kind
 * PADB_NONE clears. */
bool input_bind_key(InputPlayer *p, int b, int scancode);
void input_bind_pad(InputPlayer *p, int b, PadBind pb);

/* names used in settings.ini and --menu-action / --input-event */
const char *input_key_id(int scancode);                 /* "Z", "Return", "Comma"; "none" */
int input_key_from_id(const char *id);                  /* 0 if unknown */
void input_padbind_id(PadBind pb, char *out, size_t n); /* "south", "lefttrigger", "leftx-"; "none" */
bool input_padbind_from_id(const char *id, PadBind *pb);
/* What the player sees: the key's label on the current layout, or the controller's own name for a
 * button (PlayStation symbols on a PS controller, A/B/X/Y on Xbox-style, ...). */
void input_key_label(int scancode, char *out, size_t n);
void input_padbind_label(PadBind pb, int gamepad_type, char *out, size_t n);
void input_device_id(const InputDevice *d, char *out, size_t n);   /* "auto", "gamepad:<guid>", ... */
bool input_device_from_id(const char *id, InputDevice *d);

/* Settings (section "player1" / "player2"); missing or bad values keep the defaults. */
void input_player_load(int player, InputPlayer *p);
void input_player_store(int player, const InputPlayer *p);

/* Controller assignment (pure): guids[i] are the connected controllers in connection order;
 * prev[p] = index of the controller player p had before (or -1; prev may be NULL): an auto / any
 * player keeps it, so unplugging one player's controller never moves the other's.
 * pad_of[p] = index into guids or -1; kbd[p] = player p reads the keyboard. */
void input_assign(const InputPlayer pl[2], const char *const *guids, int n, const int prev[2], int pad_of[2], bool kbd[2]);

/* ---- host side (SDL) ---- */
/* open_gamepads: open controllers as they are added (needs SDL_INIT_GAMEPAD). Loads the players
 * from the settings store. */
void input_init(void);
void input_set_virtual_only(bool on);   /* headless: ignore physical controllers */
/* Feed an SDL event. Frontend keys must already be filtered out by the caller. */
void input_handle_event(const SDL_Event *e);
/* Press-to-bind: true if the event was consumed by an armed capture (call before anything else). */
bool input_capture_event(const SDL_Event *e);
/* This frame's masks, active high. */
void input_frame(uint16_t out[2]);
/* Release everything held (focus lost). */
void input_release_all(void);

/* live configuration for the Controls window / menu actions; call input_changed after edits
 * (re-assigns controllers, stores to settings and saves) */
InputPlayer *input_player(int player);
void input_changed(int player);
void input_reset(int player);

typedef struct { char name[128], guid[40]; int type; int player; /* -1 none */ } InputPadInfo;
int input_pad_count(void);
bool input_pad_info(int i, InputPadInfo *out);
/* gamepad type whose button names the Controls window shows for this player (its controller,
 * else the first connected one, else SDL_GAMEPAD_TYPE_UNKNOWN = Xbox-style names) */
int input_player_pad_type(int player);
uint16_t input_last_mask(int player);   /* last frame's mask, for the Controls window */
bool input_player_has_keyboard(int player);

/* capture: kind 0 = keyboard, 1 = controller */
void input_capture_begin(int player, int b, int kind);
void input_capture_cancel(void);
bool input_capture_armed(int *player, int *b, int *kind);
/* A key pressed in a native Controls window (already an SDL scancode). Consumed if armed. */
bool input_capture_key(int scancode);
/* last capture message ("F9 is used by the game window ...") or "" */
const char *input_capture_message(void);
/* keep receiving controller input while a native window has focus (Controls window open) */
void input_set_background(bool on);

/* ---- test hook: --input-event F:EVENT[:ARGS],...  (headless or windowed) ----
 *   F:key-down:NAME   F:key-up:NAME        (scancode names as settings.ini: Z, Return, Comma)
 *   F:pad-add:N[:ps4|xbox]                 attach virtual controller N (0..3; SDL virtual joystick)
 *   F:pad-remove:N
 *   F:pad-down:N:BUTTON  F:pad-up:N:BUTTON (button ids as settings.ini: south, dpup, ...)
 *   F:pad-axis:N:AXIS:VALUE                (leftx, lefty, ..., lefttrigger; -32768..32767)
 * Events of frame F are delivered through SDL's own event queue / joystick code at the start of
 * that frame's input poll, so they behave exactly like real ones. */
bool input_parse_events(const char *spec);
bool input_have_events(void);
void input_inject(uint64_t frame);
/* --input-log FILE: "frame pad1 pad2" (hex, active high) whenever a mask changes */
void input_set_log(FILE *f);
void input_log_frame(uint64_t frame, const uint16_t m[2]);

/* menu actions controls, controls-bind, controls-device, controls-stick, controls-deadzone,
 * controls-reset, controls-dump (syntax in input.c). False if refused. */
bool input_menu_action(const char *name, const char *arg);

/* ---- native Controls window (controls_mac.m / controls_win.c; no-op elsewhere) ---- */
void controls_open(void);
#endif
