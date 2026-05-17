#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <QtLogging>

namespace qcv {

using Microsoft::WRL::ComPtr;

struct D3D11DeviceManager::Impl {
    ComPtr<ID3D11Device>         device;
    ComPtr<ID3D11DeviceContext>  context;
    ComPtr<IDXGIFactory2>        factory;

    QString          adapterDesc;
    unsigned int     featureLevel = 0;
    bool             hasDebug     = false;
};

D3D11DeviceManager &D3D11DeviceManager::instance()
{
    static D3D11DeviceManager s;
    return s;
}

D3D11DeviceManager::D3D11DeviceManager() : m_impl(std::make_unique<Impl>()) {}
D3D11DeviceManager::~D3D11DeviceManager() = default;

bool D3D11DeviceManager::initialize()
{
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    m_impl->hasDebug = true;
#endif

    const D3D_FEATURE_LEVEL flReq[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        flReq, _countof(flReq), D3D11_SDK_VERSION,
        m_impl->device.GetAddressOf(), &flOut,
        m_impl->context.GetAddressOf());

    // Some Intel Arc driver builds reject D3D11_CREATE_DEVICE_DEBUG when
    // Windows Graphics Tools isn't installed. Fall back without debug
    // flag in that case (and clear hasDebug).
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        qWarning("D3D11DeviceManager: debug device creation failed "
                 "(hr=0x%08lX); retrying without DEBUG flag",
                 static_cast<unsigned long>(hr));
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        m_impl->hasDebug = false;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            flReq, _countof(flReq), D3D11_SDK_VERSION,
            m_impl->device.GetAddressOf(), &flOut,
            m_impl->context.GetAddressOf());
    }
    if (FAILED(hr)) {
        qCritical("D3D11DeviceManager: D3D11CreateDevice failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }
    m_impl->featureLevel = static_cast<unsigned int>(flOut);

    // Pull IDXGIFactory2 out of the device (avoids creating a separate
    // factory that may not match the adapter we got).
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(hr = m_impl->device.As(&dxgiDevice))) {
        qCritical("D3D11DeviceManager: QI IDXGIDevice failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(hr = dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
        qCritical("D3D11DeviceManager: GetAdapter failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }
    if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(m_impl->factory.GetAddressOf())))) {
        qCritical("D3D11DeviceManager: GetParent IDXGIFactory2 failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }

    // Capture adapter description for diagnostics.
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
        m_impl->adapterDesc = QString::fromWCharArray(desc.Description);
    }

    qInfo("D3D11DeviceManager: initialized — %s "
          "(feature level 0x%X%s)",
          qPrintable(m_impl->adapterDesc),
          m_impl->featureLevel,
          m_impl->hasDebug ? ", debug layer" : "");

    m_initialized = true;
    return true;
}

void D3D11DeviceManager::shutdown()
{
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return;
    m_impl->factory.Reset();
    m_impl->context.Reset();
    m_impl->device.Reset();
    m_impl->adapterDesc.clear();
    m_impl->featureLevel = 0;
    m_impl->hasDebug     = false;
    m_initialized = false;
}

void *D3D11DeviceManager::device()  const { return m_impl ? m_impl->device.Get()  : nullptr; }
void *D3D11DeviceManager::context() const { return m_impl ? m_impl->context.Get() : nullptr; }
void *D3D11DeviceManager::factory() const { return m_impl ? m_impl->factory.Get() : nullptr; }

QString D3D11DeviceManager::adapterDescription() const { return m_impl ? m_impl->adapterDesc : QString(); }
unsigned int D3D11DeviceManager::featureLevel()  const { return m_impl ? m_impl->featureLevel : 0; }
bool         D3D11DeviceManager::hasDebugLayer() const { return m_impl && m_impl->hasDebug; }

} // namespace qcv
