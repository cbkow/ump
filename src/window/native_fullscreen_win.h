// Native borderless fullscreen for Windows — bypasses Qt's
// setVisibility(ApplicationWindow.FullScreen), which on Windows
// transitions the HWND into a fullscreen mode that disrupts Qt's
// internal HWND attributes and breaks the child HWND parent
// relationship we rely on for the D3D11 player surface (the
// "viewport detaches off-screen" symptom).
//
// Instead we keep the window in its current Qt visibility mode
// (Windowed) and:
//   1. Save the pre-FS WINDOWPLACEMENT + ex-style.
//   2. Set WS_EX_TOPMOST so the window covers the taskbar.
//   3. Resize to screen.geometry() (full virtual-screen, not just
//      availableGeometry which excludes the taskbar).
//
// Window stays in Qt::Windowed visibility throughout. Qt has nothing
// to transition. The child HWND's parent doesn't move. Panels hide
// via the QML `inFullscreen` binding driven by the `borderlessFs`
// flag the QML toggles when this helper returns true.
//
// Restoring on exit puts the window back exactly where it was —
// position, size, ex-style.

#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
class QWindow;

namespace qcv {

bool enterBorderlessFullscreenWin(QWindow *window);
bool exitBorderlessFullscreenWin(QWindow *window);
bool isBorderlessFullscreenWin(QWindow *window);

} // namespace qcv

#endif // Q_OS_WIN
