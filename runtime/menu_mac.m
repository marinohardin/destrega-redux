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

@interface DestregaMenu : NSObject <NSMenuItemValidation>
@end

static DestregaMenu *s_target;   /* menu items hold their target weakly */

@implementation DestregaMenu
- (void)fire:(NSMenuItem *)item {
    /* representedObject: @"action" or @"action=arg" */
    NSString *s = item.representedObject;
    NSRange r = [s rangeOfString:@"="];
    if (r.location == NSNotFound) frontend_menu_action(s.UTF8String, NULL);
    else frontend_menu_action([s substringToIndex:r.location].UTF8String,
                              [s substringFromIndex:r.location + 1].UTF8String);
}
/* check marks follow the state (requested state, so a click shows at once) */
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    NSString *a = item.representedObject;
    if ([a isEqual:@"fullscreen"])
        item.title = frontend_fullscreen() ? @"Exit Full Screen" : @"Enter Full Screen";
    if ([a isEqual:@"widescreen"])
        item.state = frontend_widescreen() ? NSControlStateValueOn : NSControlStateValueOff;
    if ([a hasPrefix:@"renderer="]) {
        NSString *r = [a substringFromIndex:9];
        item.state = [r isEqualToString:@(frontend_renderer())] ? NSControlStateValueOn : NSControlStateValueOff;
        if ([r isEqualToString:@"gpu"]) return frontend_gpu_available();
    }
    return YES;
}
@end

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

        /* Enter / Exit Full Screen (title follows the state; also Alt+Enter and F11, handled in
         * main.c's event loop so they work on every platform) */
        [view insertItem:NSMenuItem.separatorItem atIndex:2];
        NSMenuItem *fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(fire:)
                                              keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        fs.target = s_target;
        fs.representedObject = @"fullscreen";
        [view insertItem:fs atIndex:3];
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
