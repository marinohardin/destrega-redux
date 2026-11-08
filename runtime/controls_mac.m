/* macOS Controls window (Settings… / View > Controls…): per player, the device and press-to-bind
 * for every PS1 button, keyboard and controller side by side, the left-stick option and its
 * deadzone, and Reset to Defaults. Changes apply at once and are saved (input.c / settings.c).
 *
 * Non-modal: the game keeps running. Keys pressed while the window is in front go to it, not to the
 * game; a key binding is captured here (macOS virtual key code -> SDL scancode, table below) and
 * handed to input_capture_key(). A controller binding is captured by the input layer from SDL's
 * gamepad events, which keep arriving while this window has focus (input_set_background).
 * A 10 Hz timer refreshes the labels, the device list (hot-plug) and the live "pressed" line.
 * Compiled with ARC. */
#import <Cocoa/Cocoa.h>
#include "input.h"

/* Carbon kVK_* virtual key codes -> SDL scancodes (positional, like SDL's own Cocoa table) */
static int scancode_from_mac(unsigned short k) {
    static const struct { unsigned short vk; int sc; } T[] = {
        {0, SDL_SCANCODE_A}, {1, SDL_SCANCODE_S}, {2, SDL_SCANCODE_D}, {3, SDL_SCANCODE_F}, {4, SDL_SCANCODE_H},
        {5, SDL_SCANCODE_G}, {6, SDL_SCANCODE_Z}, {7, SDL_SCANCODE_X}, {8, SDL_SCANCODE_C}, {9, SDL_SCANCODE_V},
        {10, SDL_SCANCODE_NONUSBACKSLASH}, {11, SDL_SCANCODE_B}, {12, SDL_SCANCODE_Q}, {13, SDL_SCANCODE_W},
        {14, SDL_SCANCODE_E}, {15, SDL_SCANCODE_R}, {16, SDL_SCANCODE_Y}, {17, SDL_SCANCODE_T},
        {18, SDL_SCANCODE_1}, {19, SDL_SCANCODE_2}, {20, SDL_SCANCODE_3}, {21, SDL_SCANCODE_4},
        {22, SDL_SCANCODE_6}, {23, SDL_SCANCODE_5}, {24, SDL_SCANCODE_EQUALS}, {25, SDL_SCANCODE_9},
        {26, SDL_SCANCODE_7}, {27, SDL_SCANCODE_MINUS}, {28, SDL_SCANCODE_8}, {29, SDL_SCANCODE_0},
        {30, SDL_SCANCODE_RIGHTBRACKET}, {31, SDL_SCANCODE_O}, {32, SDL_SCANCODE_U}, {33, SDL_SCANCODE_LEFTBRACKET},
        {34, SDL_SCANCODE_I}, {35, SDL_SCANCODE_P}, {36, SDL_SCANCODE_RETURN}, {37, SDL_SCANCODE_L},
        {38, SDL_SCANCODE_J}, {39, SDL_SCANCODE_APOSTROPHE}, {40, SDL_SCANCODE_K}, {41, SDL_SCANCODE_SEMICOLON},
        {42, SDL_SCANCODE_BACKSLASH}, {43, SDL_SCANCODE_COMMA}, {44, SDL_SCANCODE_SLASH}, {45, SDL_SCANCODE_N},
        {46, SDL_SCANCODE_M}, {47, SDL_SCANCODE_PERIOD}, {48, SDL_SCANCODE_TAB}, {49, SDL_SCANCODE_SPACE},
        {50, SDL_SCANCODE_GRAVE}, {51, SDL_SCANCODE_BACKSPACE}, {53, SDL_SCANCODE_ESCAPE},
        {54, SDL_SCANCODE_RGUI}, {55, SDL_SCANCODE_LGUI}, {56, SDL_SCANCODE_LSHIFT}, {57, SDL_SCANCODE_CAPSLOCK},
        {58, SDL_SCANCODE_LALT}, {59, SDL_SCANCODE_LCTRL}, {60, SDL_SCANCODE_RSHIFT}, {61, SDL_SCANCODE_RALT},
        {62, SDL_SCANCODE_RCTRL}, {64, SDL_SCANCODE_F17}, {65, SDL_SCANCODE_KP_PERIOD}, {67, SDL_SCANCODE_KP_MULTIPLY},
        {69, SDL_SCANCODE_KP_PLUS}, {71, SDL_SCANCODE_NUMLOCKCLEAR}, {75, SDL_SCANCODE_KP_DIVIDE},
        {76, SDL_SCANCODE_KP_ENTER}, {78, SDL_SCANCODE_KP_MINUS}, {81, SDL_SCANCODE_KP_EQUALS},
        {82, SDL_SCANCODE_KP_0}, {83, SDL_SCANCODE_KP_1}, {84, SDL_SCANCODE_KP_2}, {85, SDL_SCANCODE_KP_3},
        {86, SDL_SCANCODE_KP_4}, {87, SDL_SCANCODE_KP_5}, {88, SDL_SCANCODE_KP_6}, {89, SDL_SCANCODE_KP_7},
        {91, SDL_SCANCODE_KP_8}, {92, SDL_SCANCODE_KP_9}, {96, SDL_SCANCODE_F5}, {97, SDL_SCANCODE_F6},
        {98, SDL_SCANCODE_F7}, {99, SDL_SCANCODE_F3}, {100, SDL_SCANCODE_F8}, {101, SDL_SCANCODE_F9},
        {103, SDL_SCANCODE_F11}, {105, SDL_SCANCODE_F13}, {107, SDL_SCANCODE_F14}, {109, SDL_SCANCODE_F10},
        {111, SDL_SCANCODE_F12}, {113, SDL_SCANCODE_F15}, {114, SDL_SCANCODE_INSERT}, {115, SDL_SCANCODE_HOME},
        {116, SDL_SCANCODE_PAGEUP}, {117, SDL_SCANCODE_DELETE}, {118, SDL_SCANCODE_F4}, {119, SDL_SCANCODE_END},
        {120, SDL_SCANCODE_F2}, {121, SDL_SCANCODE_PAGEDOWN}, {122, SDL_SCANCODE_F1}, {123, SDL_SCANCODE_LEFT},
        {124, SDL_SCANCODE_RIGHT}, {125, SDL_SCANCODE_DOWN}, {126, SDL_SCANCODE_UP},
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (T[i].vk == k) return T[i].sc;
    return 0;
}

@interface ControlsWindow : NSWindow
@end

@interface ControlsController : NSObject <NSWindowDelegate>
@property ControlsWindow *window;
@property NSSegmentedControl *player;
@property NSPopUpButton *device;
@property NSButton *stick;
@property NSSlider *deadzone;
@property NSTextField *deadzoneLabel, *status, *padHeader;
@property NSMutableArray<NSTextField *> *labels;
@property NSMutableArray<NSButton *> *keyButtons, *padButtons;
@property NSTimer *timer;
@property NSString *padListKey;
@end

static ControlsController *s_ctl;

@implementation ControlsWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)keyDown:(NSEvent *)ev {
    if (input_capture_armed(NULL, NULL, NULL) && !(ev.modifierFlags & NSEventModifierFlagCommand)) {
        if (!ev.isARepeat) {
            int sc = scancode_from_mac(ev.keyCode);
            if (sc) input_capture_key(sc);
        }
        return;
    }
    if (ev.keyCode == 53) { [self performClose:nil]; return; }   /* Escape closes */
    [super keyDown:ev];
}
- (void)flagsChanged:(NSEvent *)ev {   /* modifier keys bound on their own (Shift, Control, Option) */
    int kind;
    if (input_capture_armed(NULL, NULL, &kind) && kind == 0) {
        NSEventModifierFlags f = ev.modifierFlags;
        unsigned short k = ev.keyCode;
        bool down = ((k == 56 || k == 60) && (f & NSEventModifierFlagShift)) ||
                    ((k == 59 || k == 62) && (f & NSEventModifierFlagControl)) ||
                    ((k == 58 || k == 61) && (f & NSEventModifierFlagOption)) ||
                    ((k == 54 || k == 55) && (f & NSEventModifierFlagCommand));
        if (down) { int sc = scancode_from_mac(k); if (sc) input_capture_key(sc); }
        return;
    }
    [super flagsChanged:ev];
}
@end

@implementation ControlsController

- (int)cur { return (int)self.player.selectedSegment; }

static NSTextField *label(NSView *v, NSString *s, NSRect r) {
    NSTextField *l = [NSTextField labelWithString:s];
    l.frame = r;
    [v addSubview:l];
    return l;
}

- (void)build {
    const CGFloat W = 520, rowH = 27, top = 108;
    const CGFloat H = top + PSB_COUNT * rowH + 150;
    self.window = [[ControlsWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                      backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Controls";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    NSView *v = self.window.contentView;
    CGFloat y = H - 44;
    self.player = [NSSegmentedControl segmentedControlWithLabels:@[ @"Player 1", @"Player 2" ]
                                                    trackingMode:NSSegmentSwitchTrackingSelectOne
                                                          target:self action:@selector(playerChanged:)];
    self.player.frame = NSMakeRect(20, y, 200, 26);
    self.player.selectedSegment = 0;
    [v addSubview:self.player];
    y -= 36;
    label(v, @"Device:", NSMakeRect(20, y + 3, 60, 20));
    self.device = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(80, y, W - 100, 26) pullsDown:NO];
    self.device.target = self;
    self.device.action = @selector(deviceChanged:);
    [v addSubview:self.device];
    y -= 30;
    NSFont *bold = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    label(v, @"Button", NSMakeRect(20, y, 120, 20)).font = bold;
    label(v, @"Keyboard", NSMakeRect(150, y, 170, 20)).font = bold;
    self.padHeader = label(v, @"Controller", NSMakeRect(330, y, 170, 20));
    self.padHeader.font = bold;
    self.labels = [NSMutableArray new];
    self.keyButtons = [NSMutableArray new];
    self.padButtons = [NSMutableArray new];
    for (int b = 0; b < PSB_COUNT; b++) {
        y -= rowH;
        [self.labels addObject:label(v, @(input_button_label(b)), NSMakeRect(20, y + 4, 120, 20))];
        for (int kind = 0; kind < 2; kind++) {
            NSButton *btn = [NSButton buttonWithTitle:@"" target:self action:@selector(bind:)];
            btn.frame = NSMakeRect(kind ? 330 : 150, y, 170, 26);
            btn.tag = b * 2 + kind;
            [v addSubview:btn];
            [(kind ? self.padButtons : self.keyButtons) addObject:btn];
        }
    }
    y -= 36;
    self.stick = [NSButton checkboxWithTitle:@"Left stick moves too" target:self action:@selector(stickChanged:)];
    self.stick.frame = NSMakeRect(20, y, 200, 22);
    [v addSubview:self.stick];
    label(v, @"Stick deadzone:", NSMakeRect(230, y + 1, 105, 20));
    self.deadzone = [NSSlider sliderWithValue:50 minValue:5 maxValue:95 target:self action:@selector(deadzoneChanged:)];
    self.deadzone.frame = NSMakeRect(335, y, 120, 22);
    self.deadzone.continuous = YES;
    [v addSubview:self.deadzone];
    self.deadzoneLabel = label(v, @"50 %", NSMakeRect(460, y + 1, 50, 20));
    y -= 34;
    self.status = [NSTextField wrappingLabelWithString:@""];
    self.status.frame = NSMakeRect(20, y - 6, W - 40, 34);
    self.status.textColor = NSColor.secondaryLabelColor;
    [v addSubview:self.status];
    NSButton *reset = [NSButton buttonWithTitle:@"Reset to Defaults" target:self action:@selector(reset:)];
    reset.frame = NSMakeRect(16, 14, 160, 30);
    [v addSubview:reset];
    NSButton *done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(done:)];
    done.frame = NSMakeRect(W - 116, 14, 100, 30);
    [v addSubview:done];
    [self.window center];
}

/* device popup: auto, keyboard, any controller, each connected controller, the configured one if
 * missing, none */
- (void)rebuildDevices {
    InputPlayer *p = input_player([self cur]);
    [self.device removeAllItems];
    NSMenu *m = self.device.menu;
    void (^add)(NSString *, NSString *) = ^(NSString *title, NSString *rep) {
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        it.representedObject = rep;
        [m addItem:it];
    };
    add([self cur] == 0 ? @"Automatic (keyboard and the first controller)" : @"Automatic (the next free controller)", @"auto");
    add(@"Keyboard", @"keyboard");
    add(@"Any controller", @"gamepad");
    bool have = false;
    for (int i = 0; i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (!input_pad_info(i, &pi)) continue;
        NSString *rep = [NSString stringWithFormat:@"gamepad:%s", pi.guid];
        bool dup = false;
        for (NSMenuItem *it in m.itemArray) if ([it.representedObject isEqual:rep]) dup = true;
        if (dup) continue;   /* identical controllers share a GUID: one entry */
        add([NSString stringWithFormat:@"Controller: %s", pi.name], rep);
        if (p->dev.kind == INDEV_GAMEPAD_GUID && !strcasecmp(pi.guid, p->dev.guid)) have = true;
    }
    if (p->dev.kind == INDEV_GAMEPAD_GUID && !have)
        add([NSString stringWithFormat:@"Controller: %s (not connected)", p->dev.name[0] ? p->dev.name : "unknown"],
            [NSString stringWithFormat:@"gamepad:%s", p->dev.guid]);
    add(@"None", @"none");
    char id[64];
    input_device_id(&p->dev, id, sizeof id);
    for (NSMenuItem *it in m.itemArray)
        if ([it.representedObject caseInsensitiveCompare:@(id)] == NSOrderedSame) [self.device selectItem:it];
}

- (NSString *)padList {
    NSMutableString *s = [NSMutableString new];
    for (int i = 0; i < input_pad_count(); i++) {
        InputPadInfo pi;
        if (input_pad_info(i, &pi)) [s appendFormat:@"%s|%s|%d;", pi.guid, pi.name, pi.player];
    }
    return s;
}

- (void)refresh {
    int pl = [self cur];
    InputPlayer *p = input_player(pl);
    NSString *pk = [self padList];
    if (![pk isEqual:self.padListKey]) { self.padListKey = pk; [self rebuildDevices]; }
    int cp, cb, ck;
    bool armed = input_capture_armed(&cp, &cb, &ck);
    int type = input_player_pad_type(pl);
    uint16_t pressed = input_last_mask(pl);
    for (int b = 0; b < PSB_COUNT; b++) {
        char k[64], g[64];
        input_key_label(p->key[b], k, sizeof k);
        input_padbind_label(p->pad[b], type, g, sizeof g);
        bool ak = armed && cp == pl && cb == b && ck == 0, ag = armed && cp == pl && cb == b && ck == 1;
        self.keyButtons[b].title = ak ? @"Press a key…" : @(k);
        self.padButtons[b].title = ag ? @"Press a button…" : @(g);
        self.labels[b].textColor = (pressed & input_button_mask(b)) ? NSColor.controlAccentColor : NSColor.labelColor;
    }
    self.stick.state = p->stick ? NSControlStateValueOn : NSControlStateValueOff;
    if (!self.deadzone.highlighted) self.deadzone.intValue = p->deadzone;
    self.deadzoneLabel.stringValue = [NSString stringWithFormat:@"%d %%", p->deadzone];
    const char *msg = input_capture_message();
    if (armed || *msg) self.status.stringValue = @(msg);
    else {
        NSMutableString *s = [NSMutableString stringWithFormat:@"Player %d reads ", pl + 1];
        int pad = -1;
        for (int i = 0; i < input_pad_count(); i++) { InputPadInfo pi; if (input_pad_info(i, &pi) && pi.player == pl) pad = i; }
        bool kb = input_player_has_keyboard(pl);
        InputPadInfo pi = { 0 };
        if (pad >= 0) input_pad_info(pad, &pi);
        if (kb && pad >= 0) [s appendFormat:@"the keyboard and %s.", pi.name];
        else if (kb) [s appendString:@"the keyboard."];
        else if (pad >= 0) [s appendFormat:@"%s.", pi.name];
        else [s appendString:@"nothing (no controller for it is connected)."];
        [s appendString:@" Pressed buttons light up. During netplay you play with Player 1's controls."];
        self.status.stringValue = s;
    }
}

- (void)tick:(NSTimer *)t { (void)t; [self refresh]; }
- (void)playerChanged:(id)s { (void)s; input_capture_cancel(); self.padListKey = nil; [self refresh]; }
- (void)deviceChanged:(id)s {
    (void)s;
    NSString *rep = self.device.selectedItem.representedObject;
    InputPlayer *p = input_player([self cur]);
    InputDevice d;
    if (!rep || !input_device_from_id(rep.UTF8String, &d)) return;
    if (d.kind == INDEV_GAMEPAD_GUID) {
        for (int i = 0; i < input_pad_count(); i++) {
            InputPadInfo pi;
            if (input_pad_info(i, &pi) && !strcasecmp(pi.guid, d.guid)) snprintf(d.name, sizeof d.name, "%s", pi.name);
        }
        if (!d.name[0] && p->dev.kind == INDEV_GAMEPAD_GUID) snprintf(d.name, sizeof d.name, "%s", p->dev.name);
    }
    p->dev = d;
    input_changed([self cur]);
    self.padListKey = nil;
    [self refresh];
}
- (void)bind:(NSButton *)b {
    int btn = (int)b.tag / 2, kind = (int)b.tag % 2;
    int cp, cb, ck;
    if (input_capture_armed(&cp, &cb, &ck) && cp == [self cur] && cb == btn && ck == kind) {
        input_capture_cancel();   /* second click: cancel */
    } else {
        input_capture_begin([self cur], btn, kind);
        [self.window makeFirstResponder:nil];   /* keys go to the window, not to a focused button */
    }
    [self refresh];
}
- (void)stickChanged:(id)s { (void)s; input_player([self cur])->stick = self.stick.state == NSControlStateValueOn; input_changed([self cur]); }
- (void)deadzoneChanged:(id)s {
    (void)s;
    input_player([self cur])->deadzone = self.deadzone.intValue;
    self.deadzoneLabel.stringValue = [NSString stringWithFormat:@"%d %%", self.deadzone.intValue];
    if (!NSApp.currentEvent || NSApp.currentEvent.type == NSEventTypeLeftMouseUp) input_changed([self cur]);
}
- (void)reset:(id)s {
    (void)s;
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"Reset Player %d's controls to the defaults?", [self cur] + 1];
    a.informativeText = @"The device goes back to Automatic and every key and controller button to its default.";
    [a addButtonWithTitle:@"Reset"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] != NSAlertFirstButtonReturn) return;
    input_capture_cancel();
    input_reset([self cur]);
    self.padListKey = nil;
    [self refresh];
}
- (void)done:(id)s { (void)s; [self.window performClose:nil]; }
- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    input_capture_cancel();
    input_changed([self cur]);   /* deadzone drags end here at the latest */
    input_set_background(false);
    [self.timer invalidate];
    self.timer = nil;
}
- (void)show {
    input_set_background(true);
    self.padListKey = nil;
    [self refresh];
    if (!self.timer) {
        self.timer = [NSTimer timerWithTimeInterval:0.1 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    }
    [self.window makeKeyAndOrderFront:nil];
}
@end

void controls_open(void) {
    @autoreleasepool {
        if (!s_ctl) { s_ctl = [ControlsController new]; [s_ctl build]; }
        [s_ctl show];
    }
}
