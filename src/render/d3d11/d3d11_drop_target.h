// D3D11DropTarget — Phase F.2.11.x.
//
// Minimal IDropTarget COM implementation registered on the D3D11
// player's child HWND. OLE drag-drop on Windows is HWND-scoped via
// RegisterDragDrop — it does NOT fall through HTTRANSPARENT the way
// mouse hit-testing does, so the player's child HWND blocks every
// QML DropArea sitting behind it. This class accepts CF_HDROP drops
// and forwards the extracted URLs to a caller-supplied callback,
// which D3D11PlayerRenderer wires to PlayerWindow::notifyFilesDropped
// (same path WindowManager already connects on macOS).
//
// Lifetime: created with refcount 1 by D3D11PlayerRenderer::init,
// passed to RegisterDragDrop (which AddRefs), and released by the
// renderer's shutdown after RevokeDragDrop. The callback is invoked
// on the GUI thread (OLE drop dispatch runs on the thread that
// called RegisterDragDrop).

#pragma once

#include <QList>
#include <QUrl>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <oleidl.h>

#include <atomic>
#include <functional>

namespace qcv {

class D3D11DropTarget : public IDropTarget {
public:
    using DropCallback = std::function<void(const QList<QUrl> &)>;

    explicit D3D11DropTarget(DropCallback cb);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *pDataObj,
                                         DWORD grfKeyState,
                                         POINTL pt,
                                         DWORD *pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState,
                                        POINTL pt,
                                        DWORD *pdwEffect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *pDataObj,
                                    DWORD grfKeyState,
                                    POINTL pt,
                                    DWORD *pdwEffect) override;

private:
    std::atomic<LONG> m_refCount{1};
    DropCallback      m_callback;
    bool              m_acceptDrop = false;   // set in DragEnter for the in-flight drag
};

} // namespace qcv
