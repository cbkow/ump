#include "d3d11_texture_pool.h"
#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <QtLogging>

#include <algorithm>
#include <cstring>

namespace qcv {

namespace {

DXGI_FORMAT dxgiFormatFor(int pixelFormat)
{
    switch (pixelFormat) {
        case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case 1: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 2: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case 3: return DXGI_FORMAT_R16G16B16A16_UNORM;
        default:
            qWarning("D3D11TexturePool: unsupported pixelFormat %d "
                     "(treating as RGBA8)", pixelFormat);
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

int bytesPerPixelFor(int pixelFormat)
{
    switch (pixelFormat) {
        case 0: return 4;   // RGBA8
        case 1: return 8;   // RGBA16F
        case 2: return 4;   // BGRA8
        case 3: return 8;   // RGBA16U
        default: return 4;
    }
}

} // namespace

// ---------------------------------------------------------------------
// Singletons — main pool + thumbnail pool (separate budget so 4K EXR
// uploads don't evict timeline hover thumbnails and vice versa).
// ---------------------------------------------------------------------
D3D11TexturePool &D3D11TexturePool::instance()
{
    static D3D11TexturePool s;
    return s;
}

D3D11TexturePool &D3D11TexturePool::thumbnailInstance()
{
    static D3D11TexturePool s;
    return s;
}

D3D11TexturePool::D3D11TexturePool()  = default;
D3D11TexturePool::~D3D11TexturePool() { shutdown(); }

bool D3D11TexturePool::initialize(std::size_t maxMemoryBytes)
{
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;
    if (!D3D11DeviceManager::instance().isInitialized()) {
        qCritical("D3D11TexturePool::initialize: device manager not ready");
        return false;
    }
    m_maxMemoryBytes   = maxMemoryBytes;
    m_usedMemoryBytes  = 0;
    m_nextId           = 1;
    m_initialized      = true;
    qInfo("D3D11TexturePool: initialized — max %llu MB",
          static_cast<unsigned long long>(maxMemoryBytes / (1024 * 1024)));
    return true;
}

void D3D11TexturePool::shutdown()
{
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return;

    // Drain graveyard + textures synchronously. By shutdown time the
    // render thread is stopped (renderer's shutdown happens before
    // pool's shutdown via WindowManager teardown ordering).
    for (auto &g : m_graveyard) destroyTexture(g.texture);
    m_graveyard.clear();
    for (auto &kv : m_textures) destroyTexture(kv.second);
    m_textures.clear();
    m_lruOrder.clear();
    m_lruPos.clear();
    m_pendingDeletions.clear();

    m_usedMemoryBytes = 0;
    m_initialized     = false;
}

// ---------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------
D3D11TexturePool::Handle
D3D11TexturePool::createTextureFromPixels(int width, int height, int pixelFormat,
                                            const void *pixelData,
                                            std::size_t pixelDataSize)
{
    if (width <= 0 || height <= 0) return 0;
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return 0;

    auto *device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    if (!device) return 0;

    const DXGI_FORMAT fmt = dxgiFormatFor(pixelFormat);
    const int bpp = bytesPerPixelFor(pixelFormat);
    const std::size_t expectedBytes =
        static_cast<std::size_t>(width) * height * bpp;

    D3D11_TEXTURE2D_DESC td{};
    td.Width  = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = fmt;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    if (pixelData && pixelDataSize >= expectedBytes) {
        initData.pSysMem     = pixelData;
        initData.SysMemPitch = static_cast<UINT>(width) * bpp;
    }

    ID3D11Texture2D *tex = nullptr;
    const HRESULT hr = device->CreateTexture2D(
        &td, (pixelData && pixelDataSize >= expectedBytes) ? &initData : nullptr,
        &tex);
    if (FAILED(hr) || !tex) {
        qWarning("D3D11TexturePool: CreateTexture2D %dx%d fmt=%d failed (hr=0x%08lX)",
                 width, height, pixelFormat, static_cast<unsigned long>(hr));
        return 0;
    }

    ID3D11ShaderResourceView *srv = nullptr;
    if (FAILED(device->CreateShaderResourceView(tex, nullptr, &srv))) {
        tex->Release();
        qWarning("D3D11TexturePool: CreateShaderResourceView failed");
        return 0;
    }

    D3D11Texture entry{};
    entry.texture     = tex;
    entry.srv         = srv;
    entry.width       = width;
    entry.height      = height;
    entry.pixelFormat = pixelFormat;
    entry.byteSize    = expectedBytes;
    entry.valid       = true;

    const Handle id = m_nextId++;
    m_textures.emplace(id, entry);
    m_lruOrder.push_back(id);
    auto it = m_lruOrder.end();
    --it;
    m_lruPos.emplace(id, it);
    m_usedMemoryBytes += expectedBytes;

    return id;
}

D3D11TexturePool::Handle
D3D11TexturePool::createEmptyTexture(int width, int height, int pixelFormat)
{
    return createTextureFromPixels(width, height, pixelFormat, nullptr, 0);
}

bool D3D11TexturePool::updateTexture(Handle h, const void *pixelData,
                                       std::size_t pixelDataSize)
{
    if (!pixelData || pixelDataSize == 0) return false;
    std::lock_guard lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end() || !it->second.valid) return false;
    const auto &tex = it->second;
    const int bpp = bytesPerPixelFor(tex.pixelFormat);
    const std::size_t need =
        static_cast<std::size_t>(tex.width) * tex.height * bpp;
    if (pixelDataSize < need) return false;
    auto *ctx = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    auto *t2d = static_cast<ID3D11Texture2D *>(tex.texture);
    ctx->UpdateSubresource(t2d, 0, nullptr, pixelData,
                            static_cast<UINT>(tex.width) * bpp, 0);
    return true;
}

// ---------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------
const D3D11Texture *D3D11TexturePool::texture(Handle h) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end() || !it->second.valid) return nullptr;
    return &it->second;
}

bool D3D11TexturePool::dimensions(Handle h, int &width, int &height) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end() || !it->second.valid) return false;
    width  = it->second.width;
    height = it->second.height;
    return true;
}

void D3D11TexturePool::touch(Handle h)
{
    std::lock_guard lock(m_mutex);
    auto it = m_lruPos.find(h);
    if (it == m_lruPos.end()) return;
    m_lruOrder.splice(m_lruOrder.end(), m_lruOrder, it->second);
    // Iterator stays valid after splice; no need to update m_lruPos.
}

// ---------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------
void D3D11TexturePool::queueDelete(Handle h)
{
    std::lock_guard lock(m_deletionMutex);
    m_pendingDeletions.push_back(h);
}

void D3D11TexturePool::processPendingDeletions()
{
    std::vector<Handle> toProcess;
    {
        std::lock_guard lock(m_deletionMutex);
        toProcess.swap(m_pendingDeletions);
    }

    std::lock_guard lock(m_mutex);
    // Move queued deletions into graveyard with death counter at
    // current frame + delay.
    for (Handle h : toProcess) {
        auto it = m_textures.find(h);
        if (it == m_textures.end()) continue;
        GraveyardEntry g;
        g.texture       = it->second;
        g.deathCounter  = m_graveyardCounter + kGraveyardDelayFrames;
        m_graveyard.push_back(g);
        m_usedMemoryBytes -= it->second.byteSize;
        auto posIt = m_lruPos.find(h);
        if (posIt != m_lruPos.end()) {
            m_lruOrder.erase(posIt->second);
            m_lruPos.erase(posIt);
        }
        m_textures.erase(it);
    }

    // Tick graveyard; release any entries whose counter has elapsed.
    ++m_graveyardCounter;
    auto end = std::remove_if(m_graveyard.begin(), m_graveyard.end(),
        [this](GraveyardEntry &g) {
            if (g.deathCounter <= m_graveyardCounter) {
                destroyTexture(g.texture);
                return true;
            }
            return false;
        });
    m_graveyard.erase(end, m_graveyard.end());
}

void D3D11TexturePool::destroyTexture(D3D11Texture &tex)
{
    if (tex.srv) {
        static_cast<ID3D11ShaderResourceView *>(tex.srv)->Release();
        tex.srv = nullptr;
    }
    if (tex.texture) {
        static_cast<ID3D11Texture2D *>(tex.texture)->Release();
        tex.texture = nullptr;
    }
    tex.valid = false;
}

// ---------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------
std::size_t D3D11TexturePool::usedMemory() const
{
    std::lock_guard lock(m_mutex);
    return m_usedMemoryBytes;
}

std::size_t D3D11TexturePool::textureCount() const
{
    std::lock_guard lock(m_mutex);
    return m_textures.size();
}

void D3D11TexturePool::evictToFitBytes(std::size_t requiredBytes)
{
    std::lock_guard lock(m_mutex);
    while (m_usedMemoryBytes + requiredBytes > m_maxMemoryBytes
           && !m_lruOrder.empty()) {
        const Handle victim = m_lruOrder.front();
        auto it = m_textures.find(victim);
        if (it == m_textures.end()) {
            m_lruOrder.pop_front();
            m_lruPos.erase(victim);
            continue;
        }
        // Push directly into graveyard (skip the pending-deletions
        // queue — caller has the mutex; this is synchronous-safe).
        GraveyardEntry g;
        g.texture      = it->second;
        g.deathCounter = m_graveyardCounter + kGraveyardDelayFrames;
        m_graveyard.push_back(g);
        m_usedMemoryBytes -= it->second.byteSize;
        m_textures.erase(it);
        m_lruOrder.pop_front();
        m_lruPos.erase(victim);
    }
}

// ---------------------------------------------------------------------
// Readback (staging texture + Map; for screenshot / debug paths)
// ---------------------------------------------------------------------
bool D3D11TexturePool::readbackTexture(Handle h, unsigned char *outRgba8) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end() || !it->second.valid || !outRgba8) return false;
    const auto &tex = it->second;

    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    auto *ctx    = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    auto *src    = static_cast<ID3D11Texture2D *>(tex.texture);
    if (!device || !ctx || !src) return false;

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    ID3D11Texture2D *staging = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    ctx->CopyResource(staging, src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = false;
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        // Copy out as RGBA8. Only the RGBA8 / BGRA8 paths are wired
        // for now — F.2.x can add half-float conversion later.
        const std::size_t rowBytes = static_cast<std::size_t>(tex.width) * 4;
        const auto *src8 = static_cast<const unsigned char *>(mapped.pData);
        if (tex.pixelFormat == 2) {
            // BGRA8 → RGBA8 swizzle
            for (int y = 0; y < tex.height; ++y) {
                const unsigned char *row = src8 + y * mapped.RowPitch;
                unsigned char *dst = outRgba8 + y * rowBytes;
                for (int x = 0; x < tex.width; ++x) {
                    dst[x*4 + 0] = row[x*4 + 2];
                    dst[x*4 + 1] = row[x*4 + 1];
                    dst[x*4 + 2] = row[x*4 + 0];
                    dst[x*4 + 3] = row[x*4 + 3];
                }
            }
            ok = true;
        } else if (tex.pixelFormat == 0) {
            for (int y = 0; y < tex.height; ++y) {
                std::memcpy(outRgba8 + y * rowBytes,
                              src8 + y * mapped.RowPitch, rowBytes);
            }
            ok = true;
        }
        // RGBA16F / RGBA16U not supported in F.2.2 — wire when needed.
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

} // namespace qcv
