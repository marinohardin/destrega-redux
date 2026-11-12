/* Input mapping and settings file: defaults, keyboard / controller -> masks, deadzones, rebinding,
 * names, controller assignment, settings parse / write round trip, malformed files.
 * Links input.c, settings.c, platform.c and SDL (names and labels only; SDL is not initialised). */
#include "input.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail, s_n;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { long _a = (long)(a), _b = (long)(b); s_n++; if (_a != _b) { s_fail++; fprintf(stderr, "FAIL %s:%d: %s = 0x%lx, want 0x%lx\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static bool s_down[SDL_SCANCODE_COUNT];
static uint16_t keys(const InputPlayer *p, int a, int b) {
    memset(s_down, 0, sizeof s_down);
    if (a) s_down[a] = true;
    if (b) s_down[b] = true;
    return input_map_keys(p, s_down);
}

static void test_defaults(void) {
    InputPlayer p1, p2;
    input_defaults(0, &p1);
    input_defaults(1, &p2);
    EQ(keys(&p1, SDL_SCANCODE_Z, 0), 0x4000);           /* Cross */
    EQ(keys(&p1, SDL_SCANCODE_X, 0), 0x2000);           /* Circle */
    EQ(keys(&p1, SDL_SCANCODE_A, 0), 0x8000);           /* Square */
    EQ(keys(&p1, SDL_SCANCODE_S, 0), 0x1000);           /* Triangle */
    EQ(keys(&p1, SDL_SCANCODE_Q, SDL_SCANCODE_W), 0x0c00);
    EQ(keys(&p1, SDL_SCANCODE_E, SDL_SCANCODE_R), 0x0300);
    EQ(keys(&p1, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE), 0x0009);
    EQ(keys(&p1, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT), 0x0030);
    EQ(keys(&p1, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT), 0x00c0);
    EQ(keys(&p1, SDL_SCANCODE_I, 0), 0);                /* player 2's key */
    EQ(keys(&p2, SDL_SCANCODE_I, SDL_SCANCODE_M), 0x4010);
    EQ(keys(&p2, SDL_SCANCODE_P, SDL_SCANCODE_BACKSLASH), 0x0009);
    EQ(keys(&p2, SDL_SCANCODE_Z, 0), 0);
    /* no key is used twice, by either player, and none is reserved */
    for (int b = 0; b < PSB_COUNT; b++)
        for (int c = 0; c < PSB_COUNT; c++) {
            CHECK(p1.key[b] != p2.key[c]);
            if (b != c) CHECK(p1.key[b] != p1.key[c] && p2.key[b] != p2.key[c]);
        }
    for (int b = 0; b < PSB_COUNT; b++) CHECK(!input_key_reserved(p1.key[b]) && !input_key_reserved(p2.key[b]));
    EQ(p1.dev.kind, INDEV_AUTO);
    CHECK(p1.stick);
    EQ(p1.deadzone, 50);
    /* masks match --press bits */
    EQ(input_button_mask(PSB_START), 0x8);
    EQ(input_button_mask(PSB_SELECT), 0x1);
    EQ(input_button_from_id("TRIANGLE"), PSB_TRIANGLE);
    EQ(input_button_from_id("l3"), -1);
}

static void test_pad(void) {
    InputPlayer p;
    input_defaults(0, &p);
    PadState s;
    memset(&s, 0, sizeof s);
    EQ(input_map_pad(&p, &s), 0);
    s.button[SDL_GAMEPAD_BUTTON_SOUTH] = true;
    s.button[SDL_GAMEPAD_BUTTON_NORTH] = true;
    EQ(input_map_pad(&p, &s), 0x5000);
    memset(&s, 0, sizeof s);
    s.button[SDL_GAMEPAD_BUTTON_EAST] = s.button[SDL_GAMEPAD_BUTTON_WEST] = true;
    s.button[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = s.button[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = true;
    s.button[SDL_GAMEPAD_BUTTON_START] = s.button[SDL_GAMEPAD_BUTTON_BACK] = true;
    EQ(input_map_pad(&p, &s), 0xac09);
    memset(&s, 0, sizeof s);
    s.button[SDL_GAMEPAD_BUTTON_DPAD_UP] = s.button[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
    EQ(input_map_pad(&p, &s), 0x0090);
    /* triggers: pressed beyond half */
    memset(&s, 0, sizeof s);
    s.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 16000;
    s.axis[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 20000;
    EQ(input_map_pad(&p, &s), 0x0200);
    s.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    EQ(input_map_pad(&p, &s), 0x0300);
    /* stick: radial deadzone, 8 sectors */
    memset(&s, 0, sizeof s);
    s.axis[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
    EQ(input_map_pad(&p, &s), 0x0020);
    s.axis[SDL_GAMEPAD_AXIS_LEFTX] = 15000;                 /* < 50 % */
    EQ(input_map_pad(&p, &s), 0);
    p.deadzone = 30;
    EQ(input_map_pad(&p, &s), 0x0020);
    p.stick = false;
    EQ(input_map_pad(&p, &s), 0);
    EQ(input_stick_dirs(-32768, 0, 50), 0x0080);
    EQ(input_stick_dirs(0, -32768, 50), 0x0010);
    EQ(input_stick_dirs(0, 32767, 50), 0x0040);
    EQ(input_stick_dirs(23170, 23170, 50), 0x0060);          /* 45 degrees: right + down */
    EQ(input_stick_dirs(-23170, -23170, 50), 0x0090);        /* up + left */
    EQ(input_stick_dirs(30000, 10000, 50), 0x0020);          /* 18 degrees: right only */
    EQ(input_stick_dirs(30000, 14000, 50), 0x0060);          /* 25 degrees: diagonal */
    EQ(input_stick_dirs(11000, 11000, 50), 0);               /* radius 15556 < 16384 */
    EQ(input_stick_dirs(12000, 12000, 50), 0x0060);          /* radius 16970 > 16384: diagonal */
    EQ(input_stick_dirs(100, 100, 1), 0);                    /* deadzone clamped to >= 5 % */
    /* a stick axis bound as a button */
    PadBind pb;
    CHECK(input_padbind_from_id("rightx-", &pb));
    input_bind_pad(&p, PSB_SQUARE, pb);
    memset(&s, 0, sizeof s);
    s.axis[SDL_GAMEPAD_AXIS_RIGHTX] = -20000;
    EQ(input_map_pad(&p, &s), 0x8000);
    s.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 20000;
    EQ(input_map_pad(&p, &s), 0);
}

static void test_rebind(void) {
    InputPlayer p;
    input_defaults(0, &p);
    CHECK(input_bind_key(&p, PSB_CROSS, SDL_SCANCODE_X));   /* X was Circle */
    EQ(p.key[PSB_CROSS], SDL_SCANCODE_X);
    EQ(p.key[PSB_CIRCLE], 0);
    EQ(keys(&p, SDL_SCANCODE_X, 0), 0x4000);
    EQ(keys(&p, SDL_SCANCODE_Z, 0), 0);
    static const int reserved[] = { SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
                                    SDL_SCANCODE_F11, SDL_SCANCODE_F12, SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI };
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) {
        CHECK(!input_bind_key(&p, PSB_START, reserved[i]));
        EQ(p.key[PSB_START], SDL_SCANCODE_RETURN);
    }
    CHECK(input_bind_key(&p, PSB_START, SDL_SCANCODE_F8));    /* other function keys are fine */
    CHECK(input_bind_key(&p, PSB_START, 0));                 /* clear */
    EQ(keys(&p, SDL_SCANCODE_RETURN, 0), 0);
    PadBind south = { PADB_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0 };
    input_bind_pad(&p, PSB_TRIANGLE, south);                  /* south was Cross */
    EQ(p.pad[PSB_CROSS].kind, PADB_NONE);
    EQ(p.pad[PSB_TRIANGLE].index, SDL_GAMEPAD_BUTTON_SOUTH);
    input_defaults_keys(0, &p);
    EQ(p.key[PSB_CROSS], SDL_SCANCODE_Z);
    EQ(p.key[PSB_START], SDL_SCANCODE_RETURN);
}

static void test_names(void) {
    EQ(input_key_from_id("Z"), SDL_SCANCODE_Z);
    EQ(input_key_from_id("z"), SDL_SCANCODE_Z);
    EQ(input_key_from_id("Comma"), SDL_SCANCODE_COMMA);
    EQ(input_key_from_id("Left Shift"), SDL_SCANCODE_LSHIFT);
    EQ(input_key_from_id("Return"), SDL_SCANCODE_RETURN);
    EQ(input_key_from_id("none"), 0);
    EQ(input_key_from_id("NoSuchKey"), 0);
    EQ(input_key_from_id("#4"), SDL_SCANCODE_A);
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; sc++) {   /* every named key survives a round trip */
        const char *id = input_key_id(sc);
        if (strcmp(id, "none")) EQ(input_key_from_id(id), sc);
        CHECK(!strchr(id, ',') && !strchr(id, ':'));
    }
    char b[64];
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        PadBind pb = { PADB_BUTTON, (uint8_t)i, 0 }, q;
        input_padbind_id(pb, b, sizeof b);
        CHECK(input_padbind_from_id(b, &q) && q.kind == PADB_BUTTON && q.index == i);
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
        for (int sg = -1; sg <= 1; sg += 2) {
            PadBind pb = { PADB_AXIS, (uint8_t)i, (int8_t)sg }, q;
            input_padbind_id(pb, b, sizeof b);
            CHECK(input_padbind_from_id(b, &q) && q.kind == PADB_AXIS && q.index == i);
            if (i < SDL_GAMEPAD_AXIS_LEFT_TRIGGER) EQ(q.sign, sg); else EQ(q.sign, 1);
        }
    PadBind q;
    CHECK(!input_padbind_from_id("leftx", &q));     /* a stick needs a direction */
    CHECK(input_padbind_from_id("LEFTTRIGGER", &q) && q.kind == PADB_AXIS);
    CHECK(!input_padbind_from_id("banana", &q));
    CHECK(input_padbind_from_id("none", &q) && q.kind == PADB_NONE);
    /* the controller's own names */
    PadBind south = { PADB_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, 0 }, east = { PADB_BUTTON, SDL_GAMEPAD_BUTTON_EAST, 0 };
    PadBind lb = { PADB_BUTTON, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0 }, lt = { PADB_AXIS, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1 };
    input_padbind_label(south, SDL_GAMEPAD_TYPE_PS4, b, sizeof b); CHECK(!strcmp(b, "Cross"));
    input_padbind_label(east, SDL_GAMEPAD_TYPE_PS5, b, sizeof b); CHECK(!strcmp(b, "Circle"));
    input_padbind_label(south, SDL_GAMEPAD_TYPE_XBOXONE, b, sizeof b); CHECK(!strcmp(b, "A"));
    input_padbind_label(south, SDL_GAMEPAD_TYPE_UNKNOWN, b, sizeof b); CHECK(!strcmp(b, "A"));
    input_padbind_label(east, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, b, sizeof b); CHECK(!strcmp(b, "A"));   /* Nintendo: A is east */
    input_padbind_label(lb, SDL_GAMEPAD_TYPE_PS4, b, sizeof b); CHECK(!strcmp(b, "L1"));
    input_padbind_label(lb, SDL_GAMEPAD_TYPE_XBOX360, b, sizeof b); CHECK(!strcmp(b, "LB"));
    input_padbind_label(lt, SDL_GAMEPAD_TYPE_PS5, b, sizeof b); CHECK(!strcmp(b, "L2"));
    input_padbind_label(lt, SDL_GAMEPAD_TYPE_XBOXONE, b, sizeof b); CHECK(!strcmp(b, "LT"));
    PadBind ls = { PADB_AXIS, SDL_GAMEPAD_AXIS_LEFTY, -1 };
    input_padbind_label(ls, SDL_GAMEPAD_TYPE_PS4, b, sizeof b); CHECK(!strcmp(b, "Left Stick Up"));
    /* devices */
    InputDevice d;
    CHECK(input_device_from_id("keyboard", &d) && d.kind == INDEV_KEYBOARD);
    CHECK(input_device_from_id("gamepad:030000004C050000CC09000000016800", &d) && d.kind == INDEV_GAMEPAD_GUID);
    input_device_id(&d, b, sizeof b);
    CHECK(!strcmp(b, "gamepad:030000004c050000cc09000000016800"));
    CHECK(!input_device_from_id("gamepad:xyz", &d));
    CHECK(!input_device_from_id("mouse", &d));
}

static void test_assign(void) {
    InputPlayer pl[2];
    input_defaults(0, &pl[0]);
    input_defaults(1, &pl[1]);
    const char *g[3] = { "aaaa", "bbbb", "aaaa" };
    int po[2];
    bool kb[2];
    input_assign(pl, g, 0, NULL, po, kb);   /* no controllers: keyboard to player 1 */
    CHECK(kb[0] && !kb[1] && po[0] < 0 && po[1] < 0);
    input_assign(pl, g, 2, NULL, po, kb);   /* auto: P1 keyboard + first, P2 second */
    CHECK(kb[0] && !kb[1] && po[0] == 0 && po[1] == 1);
    int prev[2] = { -1, 0 };                /* P2 had controller 0 (P1's was unplugged): it keeps it */
    input_assign(pl, g, 2, prev, po, kb);
    CHECK(po[1] == 0 && po[0] == 1);
    pl[0].dev.kind = INDEV_KEYBOARD;        /* P1 keyboard only: P2 gets the first controller */
    input_assign(pl, g, 2, NULL, po, kb);
    CHECK(kb[0] && !kb[1] && po[0] < 0 && po[1] == 0);
    pl[1].dev.kind = INDEV_KEYBOARD;        /* both keyboard */
    input_assign(pl, g, 2, NULL, po, kb);
    CHECK(kb[0] && kb[1] && po[0] < 0 && po[1] < 0);
    input_device_from_id("gamepad:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &pl[0].dev);
    const char *g2[3] = { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    pl[1].dev.kind = INDEV_GAMEPAD;         /* specific first, then any */
    input_assign(pl, g2, 3, NULL, po, kb);
    CHECK(po[0] == 1 && po[1] == 0 && !kb[0] && !kb[1]);
    input_device_from_id("gamepad:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &pl[1].dev);
    input_device_from_id("gamepad:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &pl[0].dev);   /* two identical: one each */
    input_assign(pl, g2, 3, NULL, po, kb);
    CHECK(po[0] == 0 && po[1] == 2);
    pl[0].dev.kind = INDEV_NONE;
    pl[1].dev.kind = INDEV_AUTO;            /* P2 auto never takes the keyboard */
    input_assign(pl, g2, 0, NULL, po, kb);
    CHECK(!kb[0] && !kb[1] && po[0] < 0 && po[1] < 0);
}

static bool same_player(const InputPlayer *a, const InputPlayer *b) {
    if (a->dev.kind != b->dev.kind || strcmp(a->dev.guid, b->dev.guid) || a->stick != b->stick || a->deadzone != b->deadzone) return false;
    for (int i = 0; i < PSB_COUNT; i++)
        if (a->key[i] != b->key[i] || a->pad[i].kind != b->pad[i].kind || a->pad[i].index != b->pad[i].index ||
            (a->pad[i].kind == PADB_AXIS && a->pad[i].sign != b->pad[i].sign)) return false;
    return true;
}

static void test_settings(void) {
    settings_set_path(NULL);
    settings_clear();
    /* round trip of custom controls through text */
    InputPlayer a, b;
    input_defaults(0, &a);
    input_bind_key(&a, PSB_CROSS, SDL_SCANCODE_COMMA);
    input_bind_key(&a, PSB_L1, SDL_SCANCODE_LSHIFT);
    PadBind pb;
    input_padbind_from_id("lefty-", &pb);
    input_bind_pad(&a, PSB_TRIANGLE, pb);
    input_device_from_id("gamepad:0123456789abcdef0123456789abcdef", &a.dev);
    snprintf(a.dev.name, sizeof a.dev.name, "Some Pad");
    a.stick = false;
    a.deadzone = 35;
    input_player_store(0, &a);
    CHECK(settings_set("display", "widescreen", "on"));
    CHECK(settings_set("netplay", "join_address", "192.168.1.20:7000"));
    CHECK(settings_set("custom", "unknown_key", "kept as is"));
    char *text = settings_format();
    CHECK(text && strstr(text, "key.cross = Comma") && strstr(text, "key.l1 = Left Shift") && strstr(text, "pad.triangle = lefty-"));
    CHECK(settings_parse(text) == 0);
    input_player_load(0, &b);
    CHECK(same_player(&a, &b));
    CHECK(!strcmp(b.dev.name, "Some Pad"));
    CHECK(settings_get_bool("display", "widescreen", false));
    CHECK(!strcmp(settings_get("netplay", "join_address"), "192.168.1.20:7000"));
    CHECK(!strcmp(settings_get("custom", "unknown_key"), "kept as is"));
    char *text2 = settings_format();
    CHECK(!strcmp(text, text2));   /* stable */
    free(text); free(text2);

    /* malformed: bad lines ignored, good ones kept, bad values keep the defaults */
    const char *bad =
        "\xef\xbb\xbf# comment\n"
        "orphan = 1\n"
        "[player1\n"
        "ignored = because the header is bad\n"
        "[player1]\n"
        "key.cross = F9\n"              /* reserved */
        "key.circle = NoSuchKey\n"
        "pad.square = banana\n"
        "deadzone = 500\n"
        "stick = maybe\n"
        "device = mouse\n"
        "key.start = P\n"               /* good */
        "no equals sign here\n"
        "bad key! = 1\n"
        "\x01\x02\x03 = binary\n"
        "[display]\r\n"
        "scale = 2x3\r\n"
        "  window   =   1280x720   \n"
        "[ok] trailing\n";
    int ign = settings_parse(bad);
    EQ(ign, 7);   /* bad values in good lines are the input layer's business (below) */
    InputPlayer d, def;
    input_player_load(0, &d);
    input_defaults(0, &def);
    EQ(d.key[PSB_CROSS], def.key[PSB_CROSS]);
    EQ(d.key[PSB_CIRCLE], def.key[PSB_CIRCLE]);
    EQ(d.pad[PSB_SQUARE].index, def.pad[PSB_SQUARE].index);
    EQ(d.deadzone, 50);
    CHECK(d.stick);
    EQ(d.dev.kind, INDEV_AUTO);
    EQ(d.key[PSB_START], SDL_SCANCODE_P);
    CHECK(!strcmp(settings_get("display", "scale"), "2x3"));
    CHECK(!strcmp(settings_get("display", "window"), "1280x720"));
    CHECK(!settings_get("player1", "ignored"));
    /* a moved input leaves its default button; explicit duplicates stay */
    settings_parse("[player1]\nkey.cross = X\npad.triangle = lefttrigger\n[player2]\nkey.cross = M\nkey.circle = M\n");
    input_player_load(0, &d);
    EQ(d.key[PSB_CROSS], SDL_SCANCODE_X);
    EQ(d.key[PSB_CIRCLE], 0);                       /* X was Circle's default */
    EQ(d.pad[PSB_L2].kind, PADB_NONE);              /* the left trigger was L2's default */
    EQ(d.pad[PSB_TRIANGLE].index, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    input_player_load(1, &d);
    EQ(d.key[PSB_CROSS], SDL_SCANCODE_M);
    EQ(d.key[PSB_CIRCLE], SDL_SCANCODE_M);
    /* huge line, NUL bytes, no trailing newline */
    char *big = malloc(5000);
    memset(big, 'x', 4999);
    big[4999] = 0;
    memcpy(big, "[a]\nk = ", 8);
    EQ(settings_parse(big), 1);
    free(big);
    EQ(settings_parse("[s]\nk = v"), 0);
    CHECK(!strcmp(settings_get("s", "k"), "v"));

    /* locks, removal, dirty, atomic save + load */
    settings_clear();
    CHECK(!settings_dirty());
    CHECK(settings_set("display", "scale", "3"));
    settings_lock("display", "scale");
    CHECK(!settings_set("display", "scale", "4"));
    CHECK(!strcmp(settings_get("display", "scale"), "3"));
    CHECK(settings_set("display", "max_scale", "4x6"));
    CHECK(settings_set("display", "max_scale", NULL));
    CHECK(!settings_get("display", "max_scale"));
    CHECK(!settings_set("bad section", "k", "v"));
    CHECK(!settings_set("s", "k", "line\nbreak"));
    CHECK(settings_dirty());
    const char *path = "build/tests/test_settings.ini";
    remove(path);
    settings_set_path(path);
    CHECK(settings_save());
    CHECK(!settings_dirty());
    FILE *f = fopen("build/tests/test_settings.ini.tmp", "rb");
    CHECK(!f);   /* temp file replaced */
    if (f) fclose(f);
    settings_clear();
    EQ(settings_load(), 0);
    CHECK(!strcmp(settings_get("display", "scale"), "3"));
    /* a binary file is ignored safely */
    f = fopen(path, "wb");
    for (int i = 0; i < 3000; i++) fputc((i * 7919) & 0xff, f);
    fclose(f);
    CHECK(settings_load() >= 0);
    settings_set_path("build/tests/no/such/dir/settings.ini");
    settings_set("x", "y", "z");
    CHECK(!settings_save());   /* unwritable: reported, no crash */
    settings_set_path(NULL);
    remove(path);
}

int main(void) {
    test_defaults();
    test_pad();
    test_rebind();
    test_names();
    test_assign();
    test_settings();
    printf("test_input: %d checks, %d failed\n", s_n, s_fail);
    return s_fail ? 1 : 0;
}
