// MetalGpuUploadThread — see header for adaptation notes.

#include "gpu_upload_thread.h"

#include "decode/image_loader.h"

#import <Metal/Metal.h>

#include <QtLogging>

#include <chrono>
#include <pthread.h>
#include <vector>

namespace qcv {

MetalGpuUploadThread::MetalGpuUploadThread() = default;

MetalGpuUploadThread::~MetalGpuUploadThread()
{
    stop();
}

bool MetalGpuUploadThread::start()
{
    if (m_running.load()) return true;
    m_running.store(true);
    m_thread = std::thread(&MetalGpuUploadThread::threadProc, this);
    return true;
}

void MetalGpuUploadThread::stop()
{
    if (!m_running.load() && !m_thread.joinable()) return;

    m_running.store(false);
    m_queueCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    clearAll();
}

void MetalGpuUploadThread::enqueue(int frame,
                                   std::shared_ptr<const PixelData> pixels,
                                   int generation)
{
    if (!pixels) return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back({frame, std::move(pixels), generation});
    }
    m_queueCv.notify_one();
}

void MetalGpuUploadThread::setGeneration(int generation)
{
    // Prune, don't clearAll: new-generation textures uploaded before
    // the render thread noticed the bump must survive (the express
    // seek-target frame is decoded and pushed within ~70 ms — often
    // beating the render thread here). Stale-generation entries are
    // content from a different epoch and are dropped.
    if (m_generation.exchange(generation) == generation) return;
    pruneToGeneration(generation);
}

void MetalGpuUploadThread::pruneToGeneration(int gen)
{
    std::vector<MetalTexturePool::Handle> dead;
    {
        std::unique_lock<std::shared_mutex> lock(m_textureMapMutex);
        for (auto it = m_textureMap.begin(); it != m_textureMap.end();) {
            if (it->second.generation != gen) {
                if (it->second.handle) dead.push_back(it->second.handle);
                it = m_textureMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    auto &pool = MetalTexturePool::instance();
    for (auto h : dead) pool.queueDelete(h);
}

MetalGpuUploadThread::GpuTexture
MetalGpuUploadThread::textureForFrame(int frame) const
{
    std::shared_lock<std::shared_mutex> lock(m_textureMapMutex);
    auto it = m_textureMap.find(frame);
    if (it == m_textureMap.end()) return {};
    return it->second;
}

void MetalGpuUploadThread::evictFrame(int frame)
{
    std::unique_lock<std::shared_mutex> lock(m_textureMapMutex);
    auto it = m_textureMap.find(frame);
    if (it == m_textureMap.end()) return;
    if (it->second.valid && it->second.handle) {
        MetalTexturePool::instance().queueDelete(it->second.handle);
    }
    m_textureMap.erase(it);
}

void MetalGpuUploadThread::clearAll()
{
    std::unique_lock<std::shared_mutex> mapLock(m_textureMapMutex);
    for (auto &kv : m_textureMap) {
        if (kv.second.valid && kv.second.handle) {
            MetalTexturePool::instance().queueDelete(kv.second.handle);
        }
    }
    m_textureMap.clear();

    std::lock_guard<std::mutex> qLock(m_queueMutex);
    m_queue.clear();
}

std::size_t MetalGpuUploadThread::queuedCount() const
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}

std::size_t MetalGpuUploadThread::readyCount() const
{
    std::shared_lock<std::shared_mutex> lock(m_textureMapMutex);
    return m_textureMap.size();
}

int MetalGpuUploadThread::poolFormatForPixelFormat(int pixelFormatEnum)
{
    // Maps qcv::PixelFormat (RGBA8 / RGBA16 / RGBA16F) → pool format
    // int (0 = RGBA8Unorm, 1 = RGBA16Float, 3 = RGBA16Unorm).
    switch (static_cast<PixelFormat>(pixelFormatEnum)) {
        case PixelFormat::RGBA8:   return 0;
        case PixelFormat::RGBA16F: return 1;
        case PixelFormat::RGBA16:  return 3;
    }
    return 0;
}

void MetalGpuUploadThread::threadProc()
{
    // QOS_CLASS_USER_INITIATED matches the old app — high but not
    // realtime; gives us pre-emptive priority over background work
    // without competing with the render thread.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);

    while (m_running.load()) {
        // Wait for work or shutdown.
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait_for(lock, std::chrono::milliseconds(16),
                [this] { return !m_running.load() || !m_queue.empty(); });
        }
        if (!m_running.load()) break;

        // Drain via swap — leaves the cache's queue empty for the
        // next round of pushes without holding the lock during work.
        std::deque<Item> items;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
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
                std::shared_lock<std::shared_mutex> lock(m_textureMapMutex);
                const auto it = m_textureMap.find(item.frame);
                if (it != m_textureMap.end()
                    && it->second.generation >= item.generation) {
                    continue;   // same/fresher content already up
                }
            }

            // Cap uploads per cycle so a heavy seek doesn't starve
            // the render thread of GPU command queue access.
            if (++uploadsThisCycle > kMaxUploadsPerCycle) break;

            const int width  = item.pixels->width;
            const int height = item.pixels->height;
            if (width <= 0 || height <= 0 || item.pixels->pixels.empty()) {
                continue;
            }

            const int poolFormat =
                poolFormatForPixelFormat(static_cast<int>(item.pixels->pixel_format));

            const MetalTexturePool::Handle h =
                MetalTexturePool::instance().createTextureFromPixels(
                    width, height, poolFormat,
                    item.pixels->pixels.data(),
                    item.pixels->pixels.size());
            if (!h) {
                qWarning("MetalGpuUploadThread: pool createTextureFromPixels "
                         "failed for frame %d (%dx%d fmt=%d)",
                         item.frame, width, height, poolFormat);
                continue;
            }

            MetalTexturePool::Handle replaced = 0;
            {
                std::unique_lock<std::shared_mutex> lock(m_textureMapMutex);
                auto &slot = m_textureMap[item.frame];
                if (slot.handle && slot.handle != h) replaced = slot.handle;
                slot = {h, width, height, true, item.generation};
            }
            if (replaced) {
                MetalTexturePool::instance().queueDelete(replaced);
            }
        }

        // Re-queue anything we didn't get to (cap-hit case).
        if (i < items.size()) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            for (std::size_t j = i; j < items.size(); ++j) {
                m_queue.push_back(std::move(items[j]));
            }
        }
    }
}

} // namespace qcv
