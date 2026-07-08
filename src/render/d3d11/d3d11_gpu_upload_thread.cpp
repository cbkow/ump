#include "d3d11_gpu_upload_thread.h"

#include "decode/image_loader.h"

#include <QtLogging>

#include <chrono>
#include <vector>

namespace qcv {

D3D11GpuUploadThread::D3D11GpuUploadThread() = default;

D3D11GpuUploadThread::~D3D11GpuUploadThread()
{
    stop();
}

bool D3D11GpuUploadThread::start()
{
    if (m_running.load()) return true;
    m_running.store(true);
    m_thread = std::thread(&D3D11GpuUploadThread::threadProc, this);
    return true;
}

void D3D11GpuUploadThread::stop()
{
    if (!m_running.exchange(false)) return;
    m_queueCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    clearAll();
}

void D3D11GpuUploadThread::enqueue(int frame,
                                     std::shared_ptr<const PixelData> pixels,
                                     int generation)
{
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back({frame, std::move(pixels), generation});
    }
    m_queueCv.notify_one();
}

void D3D11GpuUploadThread::setGeneration(int generation)
{
    // Prune, don't clearAll: new-generation textures uploaded before
    // the render thread noticed the bump must survive (the express
    // seek-target frame is decoded and pushed within ~70 ms — often
    // beating the render thread here). Stale-generation entries are
    // content from a different epoch and are dropped.
    if (m_generation.exchange(generation) == generation) return;
    pruneToGeneration(generation);
}

void D3D11GpuUploadThread::pruneToGeneration(int gen)
{
    std::vector<D3D11TexturePool::Handle> dead;
    {
        std::unique_lock lock(m_textureMapMutex);
        for (auto it = m_textureMap.begin(); it != m_textureMap.end();) {
            if (it->second.generation != gen) {
                if (it->second.handle) dead.push_back(it->second.handle);
                it = m_textureMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    auto &pool = D3D11TexturePool::instance();
    for (auto h : dead) pool.queueDelete(h);
}

D3D11GpuUploadThread::GpuTexture
D3D11GpuUploadThread::textureForFrame(int frame) const
{
    std::shared_lock lock(m_textureMapMutex);
    auto it = m_textureMap.find(frame);
    return (it == m_textureMap.end()) ? GpuTexture{} : it->second;
}

void D3D11GpuUploadThread::evictFrame(int frame)
{
    GpuTexture tex;
    {
        std::unique_lock lock(m_textureMapMutex);
        auto it = m_textureMap.find(frame);
        if (it == m_textureMap.end()) return;
        tex = it->second;
        m_textureMap.erase(it);
    }
    if (tex.handle) D3D11TexturePool::instance().queueDelete(tex.handle);
}

void D3D11GpuUploadThread::clearAll()
{
    std::unordered_map<int, GpuTexture> snapshot;
    {
        std::unique_lock lock(m_textureMapMutex);
        snapshot.swap(m_textureMap);
    }
    auto &pool = D3D11TexturePool::instance();
    for (auto &kv : snapshot) {
        if (kv.second.handle) pool.queueDelete(kv.second.handle);
    }
}

std::size_t D3D11GpuUploadThread::queuedCount() const
{
    std::lock_guard lock(m_queueMutex);
    return m_queue.size();
}

std::size_t D3D11GpuUploadThread::readyCount() const
{
    std::shared_lock lock(m_textureMapMutex);
    return m_textureMap.size();
}

int D3D11GpuUploadThread::poolFormatForPixelFormat(int pixelFormatEnum)
{
    // qcv::PixelFormat (RGBA8 / RGBA16 / RGBA16F) → pool format
    // int (0 = RGBA8Unorm, 1 = RGBA16Float, 3 = RGBA16Unorm). Same
    // mapping Metal uses.
    switch (static_cast<PixelFormat>(pixelFormatEnum)) {
        case PixelFormat::RGBA8:   return 0;
        case PixelFormat::RGBA16F: return 1;
        case PixelFormat::RGBA16:  return 3;
    }
    return 0;
}

void D3D11GpuUploadThread::threadProc()
{
    using namespace std::chrono_literals;
    // (Windows has no QOS_CLASS_USER_INITIATED equivalent. The thread
    // runs at THREAD_PRIORITY_NORMAL — acceptable since the upload
    // path doesn't compete with vsync as aggressively as the macOS
    // pattern was designed to avoid.)

    while (m_running.load()) {
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait_for(lock, 16ms, [this] {
                return !m_running.load() || !m_queue.empty();
            });
        }
        if (!m_running.load()) break;

        std::deque<Item> items;
        {
            std::lock_guard lock(m_queueMutex);
            items.swap(m_queue);
        }

        int uploadsThisCycle = 0;
        std::size_t i = 0;
        for (; i < items.size(); ++i) {
            Item &item = items[i];
            if (!m_running.load()) break;

            // Generation gate (2026-07-08 audit). Stale-epoch items
            // drop; a NEWER-epoch item means the cache seeked before
            // the render thread told us — self-synchronize so the
            // fresh upload isn't misjudged stale later. The cache's
            // generation is monotonic within a bind; rebinds reset
            // us via setGeneration.
            {
                const int cur = m_generation.load();
                if (item.generation < cur) continue;
                if (item.generation > cur) {
                    m_generation.store(item.generation);
                    pruneToGeneration(item.generation);
                }
            }

            {
                std::shared_lock lock(m_textureMapMutex);
                const auto it = m_textureMap.find(item.frame);
                if (it != m_textureMap.end()
                    && it->second.generation >= item.generation) {
                    continue;   // same/fresher content already up
                }
            }

            if (++uploadsThisCycle > kMaxUploadsPerCycle) break;

            const int width  = item.pixels->width;
            const int height = item.pixels->height;
            if (width <= 0 || height <= 0 || item.pixels->pixels.empty()) {
                continue;
            }

            const int poolFormat =
                poolFormatForPixelFormat(static_cast<int>(item.pixels->pixel_format));

            const D3D11TexturePool::Handle h =
                D3D11TexturePool::instance().createTextureFromPixels(
                    width, height, poolFormat,
                    item.pixels->pixels.data(),
                    item.pixels->pixels.size());
            if (!h) {
                qWarning("D3D11GpuUploadThread: pool createTextureFromPixels "
                         "failed for frame %d (%dx%d fmt=%d)",
                         item.frame, width, height, poolFormat);
                continue;
            }

            D3D11TexturePool::Handle replaced = 0;
            {
                std::unique_lock lock(m_textureMapMutex);
                auto &slot = m_textureMap[item.frame];
                if (slot.handle && slot.handle != h) replaced = slot.handle;
                slot = {h, width, height, true, item.generation};
            }
            if (replaced) {
                D3D11TexturePool::instance().queueDelete(replaced);
            }
        }

        if (i < items.size()) {
            std::lock_guard lock(m_queueMutex);
            for (std::size_t j = i; j < items.size(); ++j) {
                m_queue.push_back(std::move(items[j]));
            }
        }
    }
}

} // namespace qcv
