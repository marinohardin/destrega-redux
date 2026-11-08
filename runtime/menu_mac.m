/* macOS menu bar: File menu (game image, game files, memory cards). SDL3 builds the standard
 * application/Window/View menus itself; this only inserts one more menu after the app menu.
 * Every item just queues a named action for main.c (see menu.h), so nothing here touches the
 * emulated machine. Compiled with ARC (see CMakeLists.txt). */
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "menu.h"

@interface DestregaMenu : NSObject <NSMenuDelegate, NSMenuItemValidation>
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
/* Insert/Eject titles follow the slot's state each time the submenu opens. */
- (void)menuNeedsUpdate:(NSMenu *)menu {
    for (NSMenuItem *it in menu.itemArray) {
        NSString *s = it.representedObject;
        if ([s hasPrefix:@"card-eject="] || [s hasPrefix:@"card-insert="]) {
            int slot = [[s substringFromIndex:[s rangeOfString:@"="].location + 1] intValue] - 1;
            bool in = frontend_card_inserted(slot);
            it.title = in ? @"Eject Card" : @"Insert Card";
            it.representedObject = [NSString stringWithFormat:@"%@=%d", in ? @"card-eject" : @"card-insert", slot + 1];
        }
    }
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

static NSMenu *card_menu(int slot) {
    NSMenu *m = [[NSMenu alloc] initWithTitle:[NSString stringWithFormat:@"Card %d", slot]];
    m.delegate = s_target;
    add(m, @"Import Save Card…", [NSString stringWithFormat:@"card-import=%d", slot], nil, 0);
    add(m, @"Export Save Card…", [NSString stringWithFormat:@"card-export=%d", slot], nil, 0);
    [m addItem:NSMenuItem.separatorItem];
    add(m, @"Eject Card", [NSString stringWithFormat:@"card-eject=%d", slot], nil, 0);   /* retitled by menuNeedsUpdate */
    add(m, @"Delete Save…", [NSString stringWithFormat:@"card-delete=%d", slot], nil, 0);
    return m;
}

void menu_init(void) {
    @autoreleasepool {
        s_target = [DestregaMenu new];
        NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
        add(file, @"Load Game Image…", @"load", @"o", NSEventModifierFlagCommand);
        add(file, @"Show Game Files in Finder", @"reveal-game", @"r",
            NSEventModifierFlagCommand | NSEventModifierFlagShift);
        [file addItem:NSMenuItem.separatorItem];

        /* Memory cards. Slot 1 is the default card (memcard1.mcd beside the data pack); slot 2
         * starts empty. Further card items belong in this submenu. */
        NSMenu *cards = [[NSMenu alloc] initWithTitle:@"Memory Card"];
        add(cards, @"Show Memory Cards in Finder", @"reveal-cards", nil, 0);
        [cards addItem:NSMenuItem.separatorItem];
        for (int slot = 1; slot <= 2; slot++) {
            NSMenuItem *sub = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Card %d", slot]
                                                         action:nil keyEquivalent:@""];
            sub.submenu = card_menu(slot);
            [cards addItem:sub];
        }
        NSMenuItem *cardsItem = [[NSMenuItem alloc] initWithTitle:@"Memory Card" action:nil keyEquivalent:@""];
        cardsItem.submenu = cards;
        [file addItem:cardsItem];

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
