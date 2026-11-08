/* macOS menu bar: File menu (game image, game files, save games). SDL3 builds the standard
 * application/Window/View menus itself; this only inserts one more menu after the app menu.
 * Every item just queues a named action for main.c (see menu.h), so nothing here touches the
 * emulated machine. Compiled with ARC (see CMakeLists.txt). */
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "menu.h"

@interface DestregaMenu : NSObject
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
