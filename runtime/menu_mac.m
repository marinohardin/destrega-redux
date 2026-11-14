/* macOS menu bar: File menu (game image, game files, save games) and View menu (widescreen,
 * renderer, full screen).
 * SDL3 builds the standard application/Window menus itself (and a View menu only for
 * fullscreen-capable windows); this inserts File after the app menu and adds to SDL's View menu,
 * or creates one after File.
 * Every item just queues a named action for main.c (see menu.h), so nothing here touches the
 * emulated machine. Compiled with ARC (see CMakeLists.txt). */
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "menu.h"
#include "netplay_ui.h"
#include "widescreen.h"
#include "settings.h"
#include "input.h"

static void netplay_dialog(bool host);
static NSTextField *s_paste_field;   /* Join Game: the field the Paste button fills */

@interface DestregaMenu : NSObject <NSMenuItemValidation>
@end

static DestregaMenu *s_target;   /* menu items hold their target weakly */

@implementation DestregaMenu
- (void)fire:(NSMenuItem *)item {
    /* representedObject: @"action" or @"action=arg" */
    NSString *s = item.representedObject;
    if ([s isEqual:@"netplay-host-dialog"] || [s isEqual:@"netplay-join-dialog"]) {
        netplay_dialog([s isEqual:@"netplay-host-dialog"]);
        return;
    }
    NSRange r = [s rangeOfString:@"="];
    if (r.location == NSNotFound) frontend_menu_action(s.UTF8String, NULL);
    else frontend_menu_action([s substringToIndex:r.location].UTF8String,
                              [s substringFromIndex:r.location + 1].UTF8String);
}
- (void)netplayCancel:(id)sender {
    (void)sender;
    frontend_menu_action("netplay-disconnect", NULL);
}
- (void)netplayCopy:(id)sender {
    (void)sender;
    frontend_menu_action("netplay-copy-address", NULL);
}
/* Join Game's Paste button: the game has no Edit menu, so Cmd+V does not reach the text field.
 * A pasted address is tidied (spaces, line breaks, quotes) when it parses; else pasted as it is. */
- (void)netplayPaste:(id)sender {
    (void)sender;
    NSString *t = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    if (!t || !s_paste_field) return;
    char host[256], err[300];
    int port = 0;
    if (netplay_ui_parse_address(t.UTF8String, 0, host, sizeof host, &port, err, sizeof err))
        s_paste_field.stringValue = [NSString stringWithFormat:@"%s:%d", host, port];
    else
        s_paste_field.stringValue = [t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}
/* check marks follow the state (requested state, so a click shows at once) */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    NSString *a = item.representedObject;
    if ([a isEqual:@"netplay-status"]) {   /* informational line */
        item.title = @(netplay_status_text());
        return NO;
    }
    if ([a isEqual:@"netplay-copy-address"]) item.title = @(netplay_ui_copy_title());
    if (a) {   /* greyed out while refused (netplay: game image and save games) */
        NSRange r = [a rangeOfString:@"="];
        NSString *name = r.location == NSNotFound ? a : [a substringToIndex:r.location];
        if ([name hasSuffix:@"-dialog"]) name = [name substringToIndex:name.length - 7];
        if (!frontend_menu_action_allowed(name.UTF8String)) return NO;
    }
    if ([a isEqual:@"fullscreen"])
        item.title = frontend_fullscreen() ? @"Exit Full Screen" : @"Enter Full Screen";
    if ([a isEqual:@"widescreen"])
        item.state = frontend_widescreen() ? NSControlStateValueOn : NSControlStateValueOff;
    bool vchk;
    if (a && frontend_view_checked(a.UTF8String, &vchk))
        item.state = vchk ? NSControlStateValueOn : NSControlStateValueOff;
    if ([a hasPrefix:@"hlr"]) return frontend_hlr_available();   /* View > 3D Scene: GPU renderer only */
    if ([a hasPrefix:@"renderer="]) {
        NSString *r = [a substringFromIndex:9];
        item.state = [r isEqualToString:@(frontend_renderer())] ? NSControlStateValueOn : NSControlStateValueOff;
        return frontend_renderer_available(r.UTF8String);   /* greyed: not available on this Mac */
    }
    return YES;
}
@end

/* ---- Netplay dialogs ------------------------------------------------------------------------
 * Host Game / Join Game ask for the settings in a modal alert (the game pauses while it is open),
 * then queue netplay-host / netplay-join. The values are remembered in settings.ini ([netplay] port,
 * delay, join_address; read once from the old NSUserDefaults keys if the file has none). While the
 * host waits or the joiner connects, a small non-modal panel says so and offers Cancel; the game
 * keeps running offline meanwhile. */
static NSPanel *s_np_panel;
static NSTextField *s_np_label;
static NSButton *s_np_copy;

void menu_netplay_progress(const char *text, bool copy) {
    if (!s_target) return;   /* no menus (headless) */
    @autoreleasepool {
        if (!text) { [s_np_panel orderOut:nil]; return; }
        if (!s_np_panel) {
            s_np_panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 220)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskUtilityWindow
                                                      backing:NSBackingStoreBuffered defer:NO];
            s_np_panel.title = @"Netplay";
            s_np_panel.floatingPanel = YES;
            s_np_panel.hidesOnDeactivate = NO;
            s_np_panel.releasedWhenClosed = NO;
            s_np_label = [NSTextField wrappingLabelWithString:@""];
            s_np_label.frame = NSMakeRect(16, 52, 428, 156);   /* status, addresses, the router's answer */
            s_np_label.selectable = YES;                       /* the address can be selected by hand too */
            [s_np_panel.contentView addSubview:s_np_label];
            s_np_copy = [NSButton buttonWithTitle:@"Copy Address" target:s_target action:@selector(netplayCopy:)];
            s_np_copy.frame = NSMakeRect(12, 12, 130, 32);
            [s_np_panel.contentView addSubview:s_np_copy];
            NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:s_target action:@selector(netplayCancel:)];
            cancel.frame = NSMakeRect(350, 12, 96, 32);
            cancel.keyEquivalent = @"\e";
            [s_np_panel.contentView addSubview:cancel];
        }
        s_np_label.stringValue = @(text);
        s_np_copy.hidden = !copy;
        if (!s_np_panel.visible) [s_np_panel center];   /* updates (the router's answer) leave it where it is */
        [s_np_panel orderFront:nil];
    }
}

static NSTextField *field(NSView *v, NSString *label, NSString *value, CGFloat y, CGFloat w) {
    NSTextField *l = [NSTextField labelWithString:label];
    l.frame = NSMakeRect(0, y + 3, 110, 20);
    [v addSubview:l];
    NSTextField *f = [NSTextField textFieldWithString:value];
    f.frame = NSMakeRect(115, y, w, 24);
    [v addSubview:f];
    return f;
}

static void dialog_error(NSString *title, const char *err) {
    NSAlert *e = [NSAlert new];
    e.alertStyle = NSAlertStyleWarning;
    e.messageText = title;
    e.informativeText = @(err);
    [e addButtonWithTitle:@"OK"];
    [e runModal];
}

static void netplay_dialog(bool host) {
    @autoreleasepool {
        /* migration: earlier versions kept these in NSUserDefaults */
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        static const char *const keys[3][2] = { { "port", "NetplayPort" }, { "join_address", "NetplayJoinAddress" },
                                                { "delay", "NetplayDelay" } };
        bool migrated = false;
        for (int i = 0; i < 3; i++) {
            NSString *old = [d stringForKey:@(keys[i][1])];
            if (!settings_get("netplay", keys[i][0]) && old.length) migrated |= settings_set("netplay", keys[i][0], old.UTF8String);
        }
        if (migrated) settings_save();
        const char *sv;
        NSString *port = (sv = settings_get("netplay", "port")) ? @(sv) : @"7000";
        NSString *addr = (sv = settings_get("netplay", "join_address")) ? @(sv) : @"";
        NSString *delay = (sv = settings_get("netplay", "delay")) ? @(sv) : @"2";
        bool pm_on = settings_get_bool("netplay", "port_mapping", true);
        /* shown again with what was typed until the values are usable or the player cancels */
        for (;;) {
            NSAlert *a = [NSAlert new];
            a.messageText = host ? @"Host Game" : @"Join Game";
            if (host) {
                char ip[256];
                net_local_addresses(ip, sizeof ip);
                a.informativeText = [NSString stringWithFormat:
                    @"Your opponent chooses Join Game and types your address and port.\n\n"
                     "On your home network: %s\n\nOver the internet, the game asks your router to open the port and then "
                     "shows your internet address, with a Copy Address button; if the router can't, forward the UDP "
                     "port to this computer by hand. Both games restart from the beginning when your opponent connects.",
                    *ip ? ip : "no network address found"];
            } else {
                a.informativeText = @"Type or paste the address your opponent gave you, for example 192.168.1.20:7000. "
                                     "Both games restart from the beginning when you connect.";
            }
            const CGFloat y0 = host ? 30 : 0;   /* host: room for the router checkbox below the fields */
            NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 60 + y0)];
            NSTextField *f1 = host ? field(v, @"Port:", port, 32 + y0, 90) : field(v, @"Address:port:", addr, 32 + y0, 170);
            NSTextField *f2 = field(v, @"Input delay:", delay, y0, 50);
            NSButton *pm = nil;
            if (host) {   /* [netplay] port_mapping (portmap.c) */
                pm = [NSButton checkboxWithTitle:@"Ask the router to open the port automatically" target:nil action:nil];
                pm.frame = NSMakeRect(0, 0, 360, 22);
                pm.state = pm_on ? NSControlStateValueOn : NSControlStateValueOff;
                [v addSubview:pm];
            } else {      /* no Edit menu in the game, so Cmd+V does not work in the field: a button does */
                NSButton *paste = [NSButton buttonWithTitle:@"Paste" target:s_target action:@selector(netplayPaste:)];
                paste.frame = NSMakeRect(290, 32 + y0 - 4, 70, 32);
                [v addSubview:paste];
                s_paste_field = f1;
            }
            a.accessoryView = v;
            [a addButtonWithTitle:host ? @"Host" : @"Join"];
            [a addButtonWithTitle:@"Cancel"];
            a.window.initialFirstResponder = f1;
            NSModalResponse r = [a runModal];
            s_paste_field = nil;
            if (r != NSAlertFirstButtonReturn) return;
            /* keep what was typed for the next round */
            if (host) port = f1.stringValue; else addr = f1.stringValue;
            delay = f2.stringValue;
            if (pm) pm_on = pm.state == NSControlStateValueOn;
            char err[400], host_s[256];
            int dl = 2, p = 7000;
            if (!netplay_ui_parse_delay(delay.UTF8String, &dl, err, sizeof err)) {
                dialog_error(a.messageText, err);
                continue;
            }
            if (host) {
                if (!netplay_ui_parse_port(port.UTF8String, &p, err, sizeof err)) { dialog_error(@"Host Game", err); continue; }
                settings_set_int("netplay", "delay", dl);
                settings_set_int("netplay", "port", p);
                settings_set_bool("netplay", "port_mapping", pm_on);
                settings_save();
                char arg[64];
                snprintf(arg, sizeof arg, "%d,%d", p, dl);
                frontend_menu_action("netplay-host", arg);
            } else {
                int defp = atoi(settings_get("netplay", "port") ? settings_get("netplay", "port") : "7000");
                if (!netplay_ui_parse_address(addr.UTF8String, defp > 0 && defp <= 65535 ? defp : 7000, host_s,
                                              sizeof host_s, &p, err, sizeof err)) {
                    dialog_error(@"Join Game", err);
                    continue;
                }
                char arg[400], hp[300];
                snprintf(hp, sizeof hp, "%s:%d", host_s, p);
                settings_set_int("netplay", "delay", dl);
                settings_set("netplay", "join_address", hp);
                settings_save();
                snprintf(arg, sizeof arg, "%s,%d", hp, dl);
                frontend_menu_action("netplay-join", arg);
            }
            return;
        }
    }
}

static NSMenuItem *add(NSMenu *m, NSString *title, NSString *action, NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:@selector(fire:) keyEquivalent:key ?: @""];
    it.target = s_target;
    it.representedObject = action;
    if (key.length) it.keyEquivalentModifierMask = mods;
    [m addItem:it];
    return it;
}

static bool s_menus_built;

/* How the pacing wait runs (DESTREGA_MAC_WAIT, for diagnosis): "dispatch" (default) is a normal
 * Cocoa event loop, the same as SDL's own pump: dequeue each event and [NSApp sendEvent:] it (NSApp
 * is SDL's SDLApplication, so SDL sees every event exactly as from SDL_PumpEvents) until the
 * deadline; "peek" waits for an event without dequeuing it and lets SDL_PumpEvents take it (hid the
 * full-screen menu bar completely); "sleep" is the old SDL_DelayNS. */
static int wait_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("DESTREGA_MAC_WAIT");
        mode = (e && !strcmp(e, "sleep")) ? 0 : (e && !strcmp(e, "peek")) ? 2 : 1;
        if (e) fprintf(stderr, "menu: pacing wait mode %s\n", mode == 0 ? "sleep" : mode == 2 ? "peek" : "dispatch");
    }
    return mode;
}

bool menu_wait_until(unsigned long long deadline_ns) {
    if (!s_menus_built) return false;
    const int mode = wait_mode();
    if (mode == 0) return false;
    @autoreleasepool {
        for (int spins = 0;; spins++) {
            const Uint64 now = SDL_GetTicksNS();
            if (now >= deadline_ns) return true;
            if (spins > 1000) { SDL_DelayNS(deadline_ns - now); return true; }
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:(double)(deadline_ns - now) / 1e9];
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until
                                                inMode:NSDefaultRunLoopMode dequeue:(mode == 1)];
            if (!ev) continue;
            if (mode == 1) [NSApp sendEvent:ev];
            else SDL_PumpEvents();
        }
    }
}

void menu_init(void) {
    s_menus_built = true;
    @autoreleasepool {
        s_target = [DestregaMenu new];
        NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
        add(file, @"Load Game Image…", @"load", @"o", NSEventModifierFlagCommand);
        add(file, @"Show Game Files in Finder", @"reveal-game", @"r",
            NSEventModifierFlagCommand | NSEventModifierFlagShift);
        [file addItem:NSMenuItem.separatorItem];

        /* Save Games: one slot in the UI (the runtime still has two; see main.c). Files exchanged
         * here are standard PlayStation memory card images (.mcd). */
        NSMenu *saves = [[NSMenu alloc] initWithTitle:@"Save Games"];
        add(saves, @"Show Save Games in Finder", @"reveal-saves", nil, 0);
        [saves addItem:NSMenuItem.separatorItem];
        add(saves, @"Import Save Games…", @"save-import", nil, 0);
        add(saves, @"Export Save Games…", @"save-export", nil, 0);
        add(saves, @"Delete Save Game…", @"save-delete", nil, 0);
        NSMenuItem *savesItem = [[NSMenuItem alloc] initWithTitle:@"Save Games" action:nil keyEquivalent:@""];
        savesItem.submenu = saves;
        [file addItem:savesItem];

        NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        fileItem.submenu = file;
        NSMenu *bar = NSApp.mainMenu;
        if (!bar) { bar = [[NSMenu alloc] initWithTitle:@"MainMenu"]; NSApp.mainMenu = bar; }
        [bar insertItem:fileItem atIndex:bar.numberOfItems > 0 ? 1 : 0];

        /* View: 4:3 / 16:9 fights (runtime switch; also F9) */
        NSMenu *view = nil;
        for (NSMenuItem *it in bar.itemArray)
            if ([it.submenu.title isEqualToString:@"View"]) { view = it.submenu; break; }
        if (!view) {
            view = [[NSMenu alloc] initWithTitle:@"View"];
            NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
            viewItem.submenu = view;
            [bar insertItem:viewItem atIndex:[bar indexOfItem:fileItem] + 1];
        } else {
            /* SDL's own "Toggle Full Screen" calls -[NSWindow toggleFullScreen:] directly; ours
             * below goes through the queued action (SDL_SetWindowFullscreen) instead */
            for (NSMenuItem *it in [view.itemArray copy])
                if (it.action == @selector(toggleFullScreen:)) [view removeItem:it];
            while (view.numberOfItems > 0 && view.itemArray[0].isSeparatorItem) [view removeItemAtIndex:0];
            if (view.numberOfItems > 0) [view insertItem:NSMenuItem.separatorItem atIndex:0];
        }
        NSMenuItem *ws = [[NSMenuItem alloc] initWithTitle:@"Widescreen (16:9)" action:@selector(fire:)
                                              keyEquivalent:@""];
        ws.target = s_target;
        ws.representedObject = @"widescreen";
        [view insertItem:ws atIndex:0];

        /* View > Renderer: Metal / Vulkan / Software (radio-style check marks; switches between
         * frames, an API switch rebuilds the window's renderer; Vulkan greyed out without MoltenVK) */
        NSMenu *ren = [[NSMenu alloc] initWithTitle:@"Renderer"];
        const char *gn[4], *gt[4];
        const int ng = frontend_graphics_apis(gn, gt, 4);
        for (int i = 0; i < ng; i++)
            add(ren, @(gt[i]), [@"renderer=" stringByAppendingString:@(gn[i])], nil, 0);
        [ren addItem:[NSMenuItem separatorItem]];
        add(ren, @"Software", @"renderer=software", nil, 0);
        /* GPU precision (radio pair; presentation only): exact = bit-identical to Software, fast =
         * hardware interpolation (approximate, quicker); applies whenever a GPU API runs */
        [ren addItem:[NSMenuItem separatorItem]];
        add(ren, @"Exact", @"gpu-precision=exact", nil, 0);
        add(ren, @"Fast (Approximate)", @"gpu-precision=fast", nil, 0);
        NSMenuItem *renItem = [[NSMenuItem alloc] initWithTitle:@"Renderer" action:nil keyEquivalent:@""];
        renItem.submenu = ren;
        [view insertItem:renItem atIndex:1];

        /* View > Resolution: Auto (4x6, lowered and raised by the scale governor when this computer
         * cannot keep up) or a fixed scale the governor never touches; radio check marks */
        NSMenu *res = [[NSMenu alloc] initWithTitle:@"Resolution"];
        add(res, @"Auto", @"resolution=auto", nil, 0);
        [res addItem:[NSMenuItem separatorItem]];
        add(res, @"4×6 (Sharpest)", @"resolution=4x6", nil, 0);
        add(res, @"2×3 (Balanced)", @"resolution=2x3", nil, 0);
        add(res, @"1×1 (Original)", @"resolution=1x1", nil, 0);
        NSMenuItem *resItem = [[NSMenuItem alloc] initWithTitle:@"Resolution" action:nil keyEquivalent:@""];
        resItem.submenu = res;
        [view insertItem:resItem atIndex:2];

        /* render quality (presentation only; each switch reseeds the hi-res target like a resize) */
        NSMenu *tf = [[NSMenu alloc] initWithTitle:@"Texture Filtering"];
        add(tf, @"Off (Sharp Pixels)", @"tex-filter=nearest", nil, 0);
        add(tf, @"Bilinear", @"tex-filter=bilinear", nil, 0);
        add(tf, @"Smooth 2D Edges + Bilinear 3D", @"tex-filter=xbr", nil, 0);
        /* texture cache of the Fast renderer precision: mipmapped, anisotropic 3D textures (check mark) */
        [tf addItem:[NSMenuItem separatorItem]];
        add(tf, @"Smooth Distant Textures (Fast Renderer)", @"tex-cache", nil, 0);
        NSMenuItem *tfItem = [[NSMenuItem alloc] initWithTitle:@"Texture Filtering" action:nil keyEquivalent:@""];
        tfItem.submenu = tf;
        [view insertItem:tfItem atIndex:3];
        NSMenuItem *sc = [[NSMenuItem alloc] initWithTitle:@"Smooth Colours" action:@selector(fire:) keyEquivalent:@""];
        sc.target = s_target;
        sc.representedObject = @"smooth-colours";
        [view insertItem:sc atIndex:4];
        NSMenu *aa = [[NSMenu alloc] initWithTitle:@"Anti-aliasing"];
        add(aa, @"Off", @"ssaa=1", nil, 0);
        add(aa, @"2x", @"ssaa=2", nil, 0);
        add(aa, @"3x", @"ssaa=3", nil, 0);
        add(aa, @"4x", @"ssaa=4", nil, 0);
        NSMenuItem *aaItem = [[NSMenuItem alloc] initWithTitle:@"Anti-aliasing" action:nil keyEquivalent:@""];
        aaItem.submenu = aa;
        [view insertItem:aaItem atIndex:5];
        /* View > 3D Scene: Original (the game's own packets) or Native (the high-level renderer,
         * runtime/hlr/; presentation only), and which parts it draws. Greyed out unless a GPU API
         * is the renderer (validateMenuItem, frontend_hlr_available). */
        NSMenu *scene = [[NSMenu alloc] initWithTitle:@"3D Scene"];
        add(scene, @"Original", @"hlr=off", nil, 0);
        add(scene, @"Native", @"hlr=on", nil, 0);
        [scene addItem:[NSMenuItem separatorItem]];
        add(scene, @"Stage", @"hlr-part=stage", nil, 0);
        add(scene, @"Characters", @"hlr-part=chars", nil, 0);
        add(scene, @"Shadows", @"hlr-part=shadows", nil, 0);
        add(scene, @"Smooth Character Motion", @"hlr-bones", nil, 0);   /* bone filter (hlr_char.c) */
        [scene addItem:[NSMenuItem separatorItem]];
        add(scene, @"Enhanced Look", @"hlr-look=enhanced", nil, 0);   /* per-pixel lighting, filtered textures */
        add(scene, @"PS1 Look", @"hlr-look=ps1", nil, 0);             /* per-vertex, point-sampled */
        [scene addItem:[NSMenuItem separatorItem]];
        add(scene, @"Clip Near Polygons", @"hlr-near=clip", nil, 0);
        add(scene, @"Drop Near Polygons (PS1)", @"hlr-near=ps1", nil, 0);
        [scene addItem:[NSMenuItem separatorItem]];
        add(scene, @"Edge Smoothing Off", @"hlr-msaa=1", nil, 0);
        add(scene, @"Edge Smoothing 2x", @"hlr-msaa=2", nil, 0);
        add(scene, @"Edge Smoothing 4x", @"hlr-msaa=4", nil, 0);
        add(scene, @"Edge Smoothing 8x", @"hlr-msaa=8", nil, 0);
        NSMenuItem *sceneItem = [[NSMenuItem alloc] initWithTitle:@"3D Scene" action:nil keyEquivalent:@""];
        sceneItem.submenu = scene;
        [view insertItem:sceneItem atIndex:6];

        /* Enter / Exit Full Screen (title follows the state; also Alt+Enter and F11, handled in
         * main.c's event loop so they work on every platform) */
        [view insertItem:NSMenuItem.separatorItem atIndex:7];
        NSMenuItem *fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(fire:)
                                              keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        fs.target = s_target;
        fs.representedObject = @"fullscreen";
        [view insertItem:fs atIndex:8];

        /* View > Controls… (also Settings… in the application menu, Cmd+,) */
        [view insertItem:NSMenuItem.separatorItem atIndex:9];
        NSMenuItem *ctl = [[NSMenuItem alloc] initWithTitle:@"Controls…" action:@selector(fire:) keyEquivalent:@""];
        ctl.target = s_target;
        ctl.representedObject = @"controls";
        [view insertItem:ctl atIndex:10];
        NSMenu *app = bar.numberOfItems > 0 ? bar.itemArray[0].submenu : nil;
        if (app && app != file) {
            NSInteger at = app.numberOfItems > 0 ? 1 : 0;   /* after About */
            for (NSMenuItem *it in app.itemArray)
                if (it.action == @selector(orderFrontPreferencesPanel:) || [it.title hasPrefix:@"Preferences"] ||
                    [it.title hasPrefix:@"Settings"]) { at = -1; break; }
            if (at >= 0) {
                NSMenuItem *st = [[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(fire:) keyEquivalent:@","];
                st.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                st.target = s_target;
                st.representedObject = @"settings";
                [app insertItem:NSMenuItem.separatorItem atIndex:at];
                [app insertItem:st atIndex:at + 1];
            }
        }

        /* Netplay: play against someone on another computer (rollback netplay, netplay.c) */
        NSMenu *np = [[NSMenu alloc] initWithTitle:@"Netplay"];
        add(np, @"Host Game…", @"netplay-host-dialog", nil, 0);
        add(np, @"Join Game…", @"netplay-join-dialog", nil, 0);
        add(np, @"Copy My Address", @"netplay-copy-address", nil, 0);   /* while hosting; title: netplay_ui.c */
        [np addItem:NSMenuItem.separatorItem];
        add(np, @"Disconnect", @"netplay-disconnect", nil, 0);
        [np addItem:NSMenuItem.separatorItem];
        add(np, @"Not connected", @"netplay-status", nil, 0);
        add(np, @"Copy Out-of-Sync Report Location", @"netplay-copy-report", nil, 0);   /* after a desync */
        NSMenuItem *npItem = [[NSMenuItem alloc] initWithTitle:@"Netplay" action:nil keyEquivalent:@""];
        npItem.submenu = np;
        NSInteger vi = -1;
        for (NSMenuItem *it in bar.itemArray) if (it.submenu == view) vi = [bar indexOfItem:it];
        [bar insertItem:npItem atIndex:vi >= 0 ? vi + 1 : [bar indexOfItem:fileItem] + 1];
    }
}

void menu_reveal(const char *path) {
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
        BOOL isDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:u.path isDirectory:&isDir];
        if (isDir) [[NSWorkspace sharedWorkspace] openURL:u];
        else [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[u]];
    }
}

bool menu_executable_path(char *out, size_t n) {
    char buf[PATH_MAX * 2];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz)) return false;
    char *real = realpath(buf, NULL);
    snprintf(out, n, "%s", real ? real : buf);
    free(real);
    return true;
}

bool menu_test(const char *cmd) {   /* the Windows menus' test hook (menu.h); not on the Mac */
    fprintf(stderr, "menu: test %s: only the Windows menus have this hook\n", cmd);
    return false;
}
