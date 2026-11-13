/* macOS progress window for the launcher (launcher.h): a small native window with a
 * determinate bar and Cancel, shown over the game window while the game module is built.
 * Events reach it through SDL_PumpEvents (SDL dispatches every NSEvent to NSApp). */
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include "launcher.h"

@interface DLauncherTarget : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL cancelled;
@end
@implementation DLauncherTarget
- (void)cancel:(id)sender { (void)sender; self.cancelled = YES; }
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; self.cancelled = YES; return NO; }
@end

struct LauncherUI {
    void *win, *bar, *step, *target;   /* retained Cocoa objects (bridged) */
};

LauncherUI *lui_open(void *sdl_window)
{
    (void)sdl_window;
    LauncherUI *ui = calloc(1, sizeof *ui);
    NSRect r = NSMakeRect(0, 0, 440, 150);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:r
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    w.title = @"Destrega";
    w.releasedWhenClosed = NO;
    DLauncherTarget *t = [DLauncherTarget new];
    w.delegate = t;
    NSView *v = w.contentView;
    NSTextField *title = [NSTextField labelWithString:@"Preparing the game for this computer…"];
    title.font = [NSFont boldSystemFontOfSize:13];
    title.frame = NSMakeRect(20, 108, 400, 20);
    [v addSubview:title];
    NSTextField *step = [NSTextField labelWithString:@"This happens once and takes a minute or two."];
    step.font = [NSFont systemFontOfSize:11];
    step.textColor = NSColor.secondaryLabelColor;
    step.frame = NSMakeRect(20, 86, 400, 18);
    [v addSubview:step];
    NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 58, 400, 20)];
    bar.indeterminate = NO;
    bar.minValue = 0;
    bar.maxValue = 1;
    bar.style = NSProgressIndicatorStyleBar;
    [v addSubview:bar];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:t action:@selector(cancel:)];
    cancel.keyEquivalent = @"\e";
    cancel.frame = NSMakeRect(330, 14, 96, 32);
    [v addSubview:cancel];
    [w center];
    [w makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    ui->win = (__bridge_retained void *)w;
    ui->bar = (__bridge_retained void *)bar;
    ui->step = (__bridge_retained void *)step;
    ui->target = (__bridge_retained void *)t;
    return ui;
}

void lui_update(LauncherUI *ui, double frac, const char *step)
{
    if (!ui) return;
    NSProgressIndicator *bar = (__bridge NSProgressIndicator *)ui->bar;
    NSTextField *st = (__bridge NSTextField *)ui->step;
    bar.doubleValue = frac;
    if (step && *step) {
        NSString *s = [NSString stringWithUTF8String:step];
        if (s && ![st.stringValue isEqualToString:s]) st.stringValue = s;
    }
}

bool lui_cancelled(LauncherUI *ui)
{
    if (!ui) return false;
    SDL_Event e;
    SDL_PumpEvents();
    bool quit = false;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) quit = true;
    DLauncherTarget *t = (__bridge DLauncherTarget *)ui->target;
    if (quit) t.cancelled = YES;
    return t.cancelled;
}

void lui_close(LauncherUI *ui)
{
    if (!ui) return;
    NSWindow *w = (__bridge_transfer NSWindow *)ui->win;
    w.delegate = nil;
    [w orderOut:nil];
    (void)(__bridge_transfer id)ui->bar;
    (void)(__bridge_transfer id)ui->step;
    (void)(__bridge_transfer id)ui->target;
    free(ui);
}
