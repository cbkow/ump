#include "native_fullscreen_win.h"

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QHash>
#include <QScreen>
#include <QWindow>
#include <QtLogging>

namespace qcv {

namespace {

struct SavedState {
    WINDOWPLACEMENT placement{};
    LONG_PTR        style    = 0;
    LONG_PTR        exStyle  = 0;
};

// Module-local map of HWND → saved state. WindowManager only ever has
// one UI window in our app, but this keeps the helper general.
QHash<HWND, SavedState> &savedStates()
{
    static QHash<HWND, SavedState> s;
    return s;
}

HWND hwndOf(QWindow *window)
{
    if (!window) return nullptr;
    return reinterpret_cast<HWND>(window->winId());
}

} // namespace

bool enterBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    if (savedStates().contains(hwnd)) return true;   // already in FS

    QScreen *screen = window->screen();
    if (!screen) return false;
    // FULL geometry (includes taskbar area) — not availableGeometry.
    const QRect screenRect = screen->geometry();
    // QScreen geometry is logical pixels; SetWindowPos wants physical.
    const qreal dpr = window->devicePixelRatio();
    const int sx = static_cast<int>(screenRect.x()      * dpr + 0.5);
    const int sy = static_cast<int>(screenRect.y()      * dpr + 0.5);
    const int sw = static_cast<int>(screenRect.width()  * dpr + 0.5);
    const int sh = static_cast<int>(screenRect.height() * dpr + 0.5);

    SavedState s{};
    s.placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &s.placement)) {
        qWarning("enterBorderlessFullscreenWin: GetWindowPlacement failed (GLE=%lu)",
                 GetLastError());
        return false;
    }
    s.style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    s.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    savedStates().insert(hwnd, s);

    // Strip the native chrome (caption, thick resize border, system
    // menu, min/max). Keep WS_VISIBLE + WS_CLIPSIBLINGS + WS_CLIPCHILDREN
    // so child HWNDs (our D3D11 surface) still render. We do NOT
    // touch the window class or recreate the HWND — Qt keeps its
    // handle, and our child HWND keeps its parent.
    const LONG_PTR stripStyle = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                              | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER
                              | WS_DLGFRAME;
    SetWindowLongPtrW(hwnd, GWL_STYLE, (s.style & ~stripStyle) | WS_POPUP);
    // Add WS_EX_TOPMOST so the window draws above the taskbar
    // (Windows' taskbar is a system topmost window — only another
    // topmost window can sit visually above it). Strip
    // WS_EX_WINDOWEDGE / WS_EX_CLIENTEDGE to kill the residual frame
    // bevel some themes still draw.
    const LONG_PTR stripExStyle = WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE
                                | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      (s.exStyle & ~stripExStyle) | WS_EX_TOPMOST);
    SetWindowPos(hwnd, HWND_TOPMOST,
                  sx, sy, sw, sh,
                  SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    qInfo("enterBorderlessFullscreenWin: %p → topmost screen rect (%d,%d %dx%d)",
          hwnd, sx, sy, sw, sh);
    return true;
}

bool exitBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    auto it = savedStates().find(hwnd);
    if (it == savedStates().end()) return false;
    const SavedState s = it.value();
    savedStates().erase(it);

    SetWindowLongPtrW(hwnd, GWL_STYLE,   s.style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, s.exStyle);
    SetWindowPos(hwnd, HWND_NOTOPMOST,
                  0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // Restore the prior placement (position + size + show state) in
    // one go — WINDOWPLACEMENT captures the windowed coords even if
    // the window had been maximized at save time.
    SetWindowPlacement(hwnd, &s.placement);
    qInfo("exitBorderlessFullscreenWin: %p restored", hwnd);
    return true;
}

bool isBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    return savedStates().contains(hwnd);
}

} // namespace qcv

#endif // Q_OS_WIN
