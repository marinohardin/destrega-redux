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
#include "menu.h"
#include "widescreen.h"

static void netplay_dialog(bool host);

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
/* check marks follow the state (requested state, so a click shows at once) */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    NSString *a = item.representedObject;
    if ([a isEqual:@"netplay-status"]) {   /* informational line */
        item.title = @(netplay_status_text());
        return NO;
    }
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
    if ([a hasPrefix:@"renderer="]) {
        NSString *r = [a substringFromIndex:9];
        item.state = [r isEqualToString:@(frontend_renderer())] ? NSControlStateValueOn : NSControlStateValueOff;
        if ([r isEqualToString:@"gpu"]) return frontend_gpu_available();
    }
    return YES;
}
@end

/* ---- Netplay dialogs ------------------------------------------------------------------------
 * Host Game / Join Game ask for the settings in a modal alert (the game pauses while it is open),
 * then queue netplay-host / netplay-join. The values are remembered (NSUserDefaults). While the
 * host waits or the joiner connects, a small non-modal panel says so and offers Cancel; the game
 * keeps running offline meanwhile. */
static NSPanel *s_np_panel;
static NSTextField *s_np_label;

void menu_netplay_progress(const char *text) {
    if (!s_target) return;   /* no menus (headless) */
    @autoreleasepool {
        if (!text) { [s_np_panel orderOut:nil]; return; }
        if (!s_np_panel) {
            s_np_panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 380, 120)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskUtilityWindow
                                                      backing:NSBackingStoreBuffered defer:NO];
            s_np_panel.title = @"Netplay";
            s_np_panel.floatingPanel = YES;
            s_np_panel.hidesOnDeactivate = NO;
            s_np_panel.releasedWhenClosed = NO;
            s_np_label = [NSTextField wrappingLabelWithString:@""];
            s_np_label.frame = NSMakeRect(16, 52, 348, 56);
            [s_np_panel.contentView addSubview:s_np_label];
            NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:s_target action:@selector(netplayCancel:)];
            cancel.frame = NSMakeRect(270, 12, 96, 32);
            cancel.keyEquivalent = @"\e";
            [s_np_panel.contentView addSubview:cancel];
        }
        s_np_label.stringValue = @(text);
        [s_np_panel center];
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

static void netplay_dialog(bool host) {
    @autoreleasepool {
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        NSString *port = [d stringForKey:@"NetplayPort"] ?: @"7000";
        NSString *addr = [d stringForKey:@"NetplayJoinAddress"] ?: @"";
        NSString *delay = [d stringForKey:@"NetplayDelay"] ?: @"2";
        NSAlert *a = [NSAlert new];
        a.messageText = host ? @"Host Game" : @"Join Game";
        if (host) {
            char ip[256];
            net_local_addresses(ip, sizeof ip);
            a.informativeText = [NSString stringWithFormat:
                @"Your opponent chooses Join Game and types one of this computer's addresses and the port.\n\n"
                 "This computer's addresses: %s\n\nOver the internet, UDP port %@ must be forwarded to this computer. "
                 "Both games restart from the beginning when your opponent connects.", *ip ? ip : "none found", port];
        } else {
            a.informativeText = @"Type the address and port your opponent gave you, for example 192.168.1.20:7000. "
                                 "Both games restart from the beginning when you connect.";
        }
        NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 60)];
        NSTextField *f1 = host ? field(v, @"Port:", port, 32, 90) : field(v, @"Address:port:", addr, 32, 180);
        NSTextField *f2 = field(v, @"Input delay:", delay, 0, 50);
        a.accessoryView = v;
        [a addButtonWithTitle:host ? @"Host" : @"Join"];
        [a addButtonWithTitle:@"Cancel"];
        a.window.initialFirstResponder = f1;
        if ([a runModal] != NSAlertFirstButtonReturn) return;
        int dl = f2.intValue;
        if (dl < 1 || dl > 30) dl = 2;
        [d setObject:[NSString stringWithFormat:@"%d", dl] forKey:@"NetplayDelay"];
        char arg[400];
        if (host) {
            int p = f1.intValue;
            if (p <= 0 || p > 65535) p = 7000;
            [d setObject:[NSString stringWithFormat:@"%d", p] forKey:@"NetplayPort"];
            snprintf(arg, sizeof arg, "%d,%d", p, dl);
            frontend_menu_action("netplay-host", arg);
        } else {
            NSString *s = [f1.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if (![s containsString:@":"]) s = [s stringByAppendingFormat:@":%@", port];
            [d setObject:s forKey:@"NetplayJoinAddress"];
            snprintf(arg, sizeof arg, "%s,%d", s.UTF8String, dl);
            frontend_menu_action("netplay-join", arg);
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

void menu_init(void) {
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

        /* View > Renderer: Software / GPU (radio-style check marks; switches between frames) */
        NSMenu *ren = [[NSMenu alloc] initWithTitle:@"Renderer"];
        add(ren, @"Software", @"renderer=software", nil, 0);
        add(ren, @"GPU", @"renderer=gpu", nil, 0);
        NSMenuItem *renItem = [[NSMenuItem alloc] initWithTitle:@"Renderer" action:nil keyEquivalent:@""];
        renItem.submenu = ren;
        [view insertItem:renItem atIndex:1];

        /* render quality (presentation only; each switch reseeds the hi-res target like a resize) */
        NSMenu *tf = [[NSMenu alloc] initWithTitle:@"Texture Filtering"];
        add(tf, @"Off (Sharp Pixels)", @"tex-filter=nearest", nil, 0);
        add(tf, @"Bilinear", @"tex-filter=bilinear", nil, 0);
        add(tf, @"Smooth 2D Edges + Bilinear 3D", @"tex-filter=xbr", nil, 0);
        NSMenuItem *tfItem = [[NSMenuItem alloc] initWithTitle:@"Texture Filtering" action:nil keyEquivalent:@""];
        tfItem.submenu = tf;
        [view insertItem:tfItem atIndex:2];
        NSMenuItem *sc = [[NSMenuItem alloc] initWithTitle:@"Smooth Colours" action:@selector(fire:) keyEquivalent:@""];
        sc.target = s_target;
        sc.representedObject = @"smooth-colours";
        [view insertItem:sc atIndex:3];
        NSMenu *aa = [[NSMenu alloc] initWithTitle:@"Anti-aliasing"];
        add(aa, @"Off", @"ssaa=1", nil, 0);
        add(aa, @"2x", @"ssaa=2", nil, 0);
        add(aa, @"3x", @"ssaa=3", nil, 0);
        add(aa, @"4x", @"ssaa=4", nil, 0);
        NSMenuItem *aaItem = [[NSMenuItem alloc] initWithTitle:@"Anti-aliasing" action:nil keyEquivalent:@""];
        aaItem.submenu = aa;
        [view insertItem:aaItem atIndex:4];

        /* Enter / Exit Full Screen (title follows the state; also Alt+Enter and F11, handled in
         * main.c's event loop so they work on every platform) */
        [view insertItem:NSMenuItem.separatorItem atIndex:5];
        NSMenuItem *fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(fire:)
                                              keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        fs.target = s_target;
        fs.representedObject = @"fullscreen";
        [view insertItem:fs atIndex:6];

        /* Netplay: play against someone on another computer (rollback netplay, netplay.c) */
        NSMenu *np = [[NSMenu alloc] initWithTitle:@"Netplay"];
        add(np, @"Host Game…", @"netplay-host-dialog", nil, 0);
        add(np, @"Join Game…", @"netplay-join-dialog", nil, 0);
        [np addItem:NSMenuItem.separatorItem];
        add(np, @"Disconnect", @"netplay-disconnect", nil, 0);
        [np addItem:NSMenuItem.separatorItem];
        add(np, @"Not connected", @"netplay-status", nil, 0);
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
