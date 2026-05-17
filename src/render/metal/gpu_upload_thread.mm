// MetalGpuUploadThread — see header for adaptation notes.

#include "gpu_upload_thread.h"

#include "decode/image_loader.h"

#import <Metal/Metal.h>

#include <QtLogging>

#include <chrono>
#include <pthread.h>

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

void MetalGpuUploadThread::setGeneration(int /*generation*/)
{
    // Generation tracking lives on the cache side now. Upload thread
    // accepts whatever it's enqueued with and trusts the cache's
    // I/O worker (which filters stale results before they reach the
    // push callback) + the eviction callback (which prunes the
    // textureMap as the cache window slides) to keep state coherent.
    // Kept as a no-op to preserve the IPlayerRenderer-side hook.
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

            // No generation filtering here — the cache's I/O worker
            // already drops stale results before they reach the
            // push callback (image_sequence_cache.cpp ~line 614).
            // Anything that lands in our queue is current-gen by
            // construction.

            // Already have a texture for this frame? Skip.
            {
                std::shared_lock<std::shared_mutex> lock(m_textureMapMutex);
                if (m_textureMap.count(item.frame)) continue;
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

            std::unique_lock<std::shared_mutex> lock(m_textureMapMutex);
            m_textureMap[item.frame] = {h, width, height, true};
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
