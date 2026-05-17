// macOS borderless fullscreen — see header for design notes.

#include <QtGlobal>

#include "native_fullscreen_macos.h"

#ifdef Q_OS_MACOS

#include <QWindow>

#import <AppKit/AppKit.h>

#include <unordered_map>
#include <mutex>

namespace qcv {

namespace {

// Per-window saved state. Keyed by NSWindow* (taken as a void* so
// we don't cross AppKit into the header). Concurrent access from
// QML callbacks (GUI thread only) is fine without a mutex; we keep
// one anyway since these are called rarely and correctness > perf.
struct SavedState {
    NSWindowStyleMask         styleMask;
    NSInteger                 level;
    NSWindowCollectionBehavior collectionBehavior;
    NSRect                    frame;
    NSApplicationPresentationOptions presentationOptions;
    BOOL                      hasShadow;
    BOOL                      opaque;
};

std::unordered_map<void *, SavedState> &savedStateMap()
{
    static std::unordered_map<void *, SavedState> map;
    return map;
}
std::mutex &savedStateMutex()
{
    static std::mutex m;
    return m;
}

NSWindow *nsWindowFor(QWindow *qwindow)
{
    if (!qwindow) return nil;
    NSView *view = reinterpret_cast<NSView *>(qwindow->winId());
    return view ? view.window : nil;
}

} // namespace

bool enterBorderlessFullscreen(QWindow *qwindow)
{
    NSWindow *win = nsWindowFor(qwindow);
    if (!win) return false;

    {
        std::lock_guard<std::mutex> lk(savedStateMutex());
        if (savedStateMap().count(win)) return false; // already in BF
        SavedState s;
        s.styleMask           = win.styleMask;
        s.level               = win.level;
        s.collectionBehavior  = win.collectionBehavior;
        s.frame               = win.frame;
        s.presentationOptions = NSApp.presentationOptions;
        s.hasShadow           = win.hasShadow;
        s.opaque              = win.opaque;
        savedStateMap().emplace(win, s);
    }

    // Stop the OS from offering / treating this as a fullscreen
    // Space target. Without this, setting MainMenuWindowLevel would
    // get fought by the WindowServer's fullscreen policy.
    win.collectionBehavior =
        NSWindowCollectionBehaviorFullScreenAuxiliary;

    // Drop title bar — Borderless mask removes the standard chrome
    // (close/min/zoom buttons + title bar area).
    win.styleMask = NSWindowStyleMaskBorderless;

    // Raise above the menu bar so the window can paint over it.
    // MainMenuWindowLevel + 1 puts us just above the menu bar but
    // below the Dock's overlay levels — good middle ground.
    win.level = NSMainMenuWindowLevel + 1;

    // Cover the entire screen (including the menu-bar zone).
    NSScreen *screen = win.screen ?: NSScreen.mainScreen;
    if (screen) {
        [win setFrame:screen.frame display:YES];
    }

    // opaque = YES is the load-bearing flag here. Flipping styleMask
    // to NSWindowStyleMaskBorderless auto-clears opaque to NO, which
    // routes WindowServer down the per-pixel-alpha compositing path —
    // and that path applies a subtle edge-aware blend (key-window
    // highlight + rounded-corner AA) right at the window boundary.
    // At normal window sizes the drop shadow hides it; in borderless
    // fullscreen with frame == screen.frame the shadow gets clipped
    // off-screen and the inner stroke reads as a ~1-2 px hairline on
    // all four edges. opaque = YES forces the fast opaque-blit path —
    // no edge AA, no transition zone, Metal pixels straight to the
    // framebuffer. Our drawable is opaque (clear alpha = 1) so the
    // flag matches reality.
    //
    // hasShadow = NO is belt-and-suspenders: the drop shadow itself
    // sits outside the frame and isn't visible at screen edges, but
    // skipping it dodges a redundant compositing pass on enter/exit.
    win.opaque    = YES;
    win.hasShadow = NO;

    // Hide the menu bar + dock so the window is truly edge-to-edge.
    NSApp.presentationOptions =
        NSApplicationPresentationAutoHideMenuBar |
        NSApplicationPresentationAutoHideDock;

    // Re-claim key window status — flipping styleMask + level
    // sometimes drops keyboard focus mid-transition. Without this
    // the user has to click into the window before F / Esc / Space
    // routes through Qt's shortcut chain.
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
    [win makeKeyWindow];
    [win makeMainWindow];
    return true;
}

bool exitBorderlessFullscreen(QWindow *qwindow)
{
    NSWindow *win = nsWindowFor(qwindow);
    if (!win) return false;

    SavedState s;
    {
        std::lock_guard<std::mutex> lk(savedStateMutex());
        auto it = savedStateMap().find(win);
        if (it == savedStateMap().end()) return false;
        s = it->second;
        savedStateMap().erase(it);
    }

    // Restore in reverse order: presentation flags first so the
    // menu bar reappears as the window de-grows; styleMask resets
    // chrome; level lowers; frame settles back to its windowed rect.
    NSApp.presentationOptions = s.presentationOptions;
    win.styleMask          = s.styleMask;
    win.level              = s.level;
    win.collectionBehavior = s.collectionBehavior;
    win.hasShadow          = s.hasShadow;
    win.opaque             = s.opaque;
    [win setFrame:s.frame display:YES];

    // Same focus-reclaim as enter — exit also drops key state.
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
    [win makeKeyWindow];
    [win makeMainWindow];
    return true;
}

bool isBorderlessFullscreen(QWindow *qwindow)
{
    NSWindow *win = nsWindowFor(qwindow);
    if (!win) return false;
    std::lock_guard<std::mutex> lk(savedStateMutex());
    return savedStateMap().count(win) > 0;
}

void disableSystemFullscreen(QWindow *qwindow)
{
    NSWindow *win = nsWindowFor(qwindow);
    if (!win) return;

    // FullScreenNone tells AppKit this window doesn't participate in
    // the OS fullscreen Space — the green title-bar button reverts to
    // standard zoom, and the hover menu drops the "Enter Full Screen"
    // entry. Mask out any pre-existing Primary/Auxiliary bits before
    // OR'ing None in so the flags aren't internally contradictory
    // (the docs say None is exclusive of Primary/Auxiliary).
    NSWindowCollectionBehavior cb = win.collectionBehavior;
    cb &= ~(NSWindowCollectionBehaviorFullScreenPrimary
            | NSWindowCollectionBehaviorFullScreenAuxiliary);
    cb |=   NSWindowCollectionBehaviorFullScreenNone;
    win.collectionBehavior = cb;
}

} // namespace qcv

#endif // Q_OS_MACOS
