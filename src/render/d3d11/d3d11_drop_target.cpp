#include "d3d11_drop_target.h"

#include <QString>
#include <QtLogging>

#include <shellapi.h>
#include <shlobj.h>

#include <vector>

namespace qcv {

namespace {

bool dataObjectHasFiles(IDataObject *pDataObj)
{
    if (!pDataObj) return false;
    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.ptd      = nullptr;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex   = -1;
    fmt.tymed    = TYMED_HGLOBAL;
    return pDataObj->QueryGetData(&fmt) == S_OK;
}

QList<QUrl> extractFileUrls(IDataObject *pDataObj)
{
    QList<QUrl> urls;
    if (!pDataObj) return urls;

    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.ptd      = nullptr;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex   = -1;
    fmt.tymed    = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(pDataObj->GetData(&fmt, &medium))) return urls;

    // HDROP is an HGLOBAL — DragQueryFile takes the handle directly and
    // locks internally. Don't GlobalLock here; the locked pointer is a
    // DROPFILES* and passing it to DragQueryFile gives garbage.
    HDROP hDrop = static_cast<HDROP>(medium.hGlobal);
    if (hDrop) {
        const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
        urls.reserve(static_cast<int>(count));
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            if (len == 0) continue;
            std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
            DragQueryFileW(hDrop, i, buf.data(), len + 1);
            const QString path = QString::fromWCharArray(buf.data(),
                                                          static_cast<int>(len));
            if (!path.isEmpty()) urls.append(QUrl::fromLocalFile(path));
        }
    }
    ReleaseStgMedium(&medium);
    return urls;
}

} // namespace

D3D11DropTarget::D3D11DropTarget(DropCallback cb)
    : m_callback(std::move(cb))
{
}

// ---- IUnknown ----

HRESULT STDMETHODCALLTYPE D3D11DropTarget::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget *>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D11DropTarget::AddRef()
{
    return static_cast<ULONG>(m_refCount.fetch_add(1) + 1);
}

ULONG STDMETHODCALLTYPE D3D11DropTarget::Release()
{
    const LONG c = m_refCount.fetch_sub(1) - 1;
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
}

// ---- IDropTarget ----

HRESULT STDMETHODCALLTYPE D3D11DropTarget::DragEnter(IDataObject *pDataObj,
                                                      DWORD /*grfKeyState*/,
                                                      POINTL /*pt*/,
                                                      DWORD *pdwEffect)
{
    if (!pdwEffect) return E_INVALIDARG;
    m_acceptDrop = dataObjectHasFiles(pDataObj);
    *pdwEffect = m_acceptDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D11DropTarget::DragOver(DWORD /*grfKeyState*/,
                                                     POINTL /*pt*/,
                                                     DWORD *pdwEffect)
{
    if (!pdwEffect) return E_INVALIDARG;
    *pdwEffect = m_acceptDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D11DropTarget::DragLeave()
{
    m_acceptDrop = false;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D3D11DropTarget::Drop(IDataObject *pDataObj,
                                                 DWORD /*grfKeyState*/,
                                                 POINTL /*pt*/,
                                                 DWORD *pdwEffect)
{
    if (!pdwEffect) return E_INVALIDARG;
    const QList<QUrl> urls = extractFileUrls(pDataObj);
    if (!urls.isEmpty() && m_callback) {
        m_callback(urls);
        *pdwEffect = DROPEFFECT_COPY;
    } else {
        *pdwEffect = DROPEFFECT_NONE;
    }
    m_acceptDrop = false;
    return S_OK;
}

} // namespace qcv
