// Native borderless fullscreen for macOS — bypasses Qt's
// setVisibility(FullScreen), which on macOS triggers the OS's
// "zoom into a new Space" animation. Instead we manipulate the
// underlying NSWindow:
//   styleMask        → Borderless (no title bar)
//   level            → MainMenuWindowLevel + 1 (draws over menu bar)
//   collectionBehavior → FullScreenAuxiliary (don't fight OS FS API)
//   frame            → screen.frame (covers menu bar zone)
//   NSApp.presentationOptions → AutoHideMenuBar | AutoHideDock
//
// Result: instant, no animation, stays in current Space.
//
// All saved state is captured in `enterBorderlessFullscreen` and
// restored in `exitBorderlessFullscreen` so the windowed state is
// exactly preserved.

#pragma once

#include <QtGlobal>

#ifdef Q_OS_MACOS
class QWindow;

namespace qcv {

// Enter borderless fullscreen on the given window. No-op if
// already borderless. Returns true on success.
bool enterBorderlessFullscreen(QWindow *window);

// Exit borderless fullscreen, restoring the prior styleMask, level,
// collectionBehavior, frame, and presentationOptions. No-op if not
// in borderless fullscreen.
bool exitBorderlessFullscreen(QWindow *window);

bool isBorderlessFullscreen(QWindow *window);

// Take macOS's green title-bar button out of the OS-fullscreen path —
// click reverts to standard zoom (maximize within the current Space)
// instead of triggering the slide-into-a-new-Space animation. Called
// once at WindowManager init for the UI window. Borderless fullscreen
// is unaffected: it overrides collectionBehavior on entry and restores
// on exit, so the None set here is the windowed-state default the
// restore path lands back on.
void disableSystemFullscreen(QWindow *window);

} // namespace qcv

#endif // Q_OS_MACOS
