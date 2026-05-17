// MetalGpuUploadThread — Phase 7.5 B.5 / Guide 19.
//
// Standalone thread that drains a queue of `(frame, PixelData,
// generation)` items and creates MTLTextures via MetalTexturePool.
// The renderer reads completed textures by frame number on its own
// thread.
//
// This replaces the Phase 7.4.b.4 pull-model bottleneck: the renderer
// used to pull pixel data from the cache and call MetalTexturePool
// itself on the render thread, which stalled vsync the moment a 4K
// EXR upload took longer than 16ms (rendering wedged the whole app).
// Moving uploads here means the render thread always finds either
// a ready GPU texture or "not yet" — never blocks on disk + decode.
//
// Adapted verbatim from old QCView's
// src/player/direct_exr_cache.cpp:1728-1840, with these changes:
//   - Extracted from DirectEXRCache so it's reusable across all
//     image-sequence sources (Phase B.6.2 wires ImageSequenceCache
//     into it).
//   - PixelFormat → pool format-int mapping is local to this module.
//   - Generation is owned here (atomic), not the cache. Cache calls
//     setGeneration() on each seek.
//
// Threading:
//   I/O thread (cache):    enqueue() pushes new items
//   Upload thread (us):    drains queue, creates textures, stores
//                           handles in m_textureMap
//   Render thread:         getTexture() reads handles
//
// Cross-thread state:
//   m_queue + m_queueMutex + m_queueCv  — producer/consumer
//   m_textureMap + m_textureMapMutex    — read by renderer, write by us
//   m_generation                        — atomic int, set by cache, read by us
//   m_running                           — atomic bool, set by stop()

#pragma once

#include "metal_texture_pool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace qcv {

struct PixelData;

class MetalGpuUploadThread {
public:
    struct GpuTexture {
        MetalTexturePool::Handle handle = 0;
        int                      width  = 0;
        int                      height = 0;
        bool                     valid  = false;
    };

    MetalGpuUploadThread();
    ~MetalGpuUploadThread();

    MetalGpuUploadThread(const MetalGpuUploadThread &)            = delete;
    MetalGpuUploadThread &operator=(const MetalGpuUploadThread &) = delete;

    bool start();   // spawns the thread; safe to call after stop()
    void stop();    // joins the thread; safe to call multiple times

    // Cache pushes pixel data for a given frame. The pool format-int
    // is looked up from PixelData::pixel_format here. `generation`
    // is the cache's monotonic generation counter — items whose
    // generation is older than the upload thread's current
    // generation are silently dropped.
    void enqueue(int frame, std::shared_ptr<const PixelData> pixels,
                 int generation);

    // Bumps the active generation. Items in the queue with older
    // generations are skipped on dequeue. Also clears the texture
    // map (the renderer will see "not ready" until uploads catch up).
    void setGeneration(int generation);

    // Renderer-thread query. Returns {valid=false} if the frame's
    // texture isn't ready yet.
    GpuTexture textureForFrame(int frame) const;

    // Drop a single frame's GPU texture (queued for graveyard via
    // the pool's deferred deletion).
    void evictFrame(int frame);

    // Drop everything. Used on seek + on shutdown.
    void clearAll();

    // Diagnostics.
    std::size_t queuedCount() const;
    std::size_t readyCount() const;

private:
    void threadProc();

    static int poolFormatForPixelFormat(int pixelFormatEnum);

    struct Item {
        int                              frame;
        std::shared_ptr<const PixelData> pixels;
        int                              generation;
    };

    std::thread                  m_thread;
    std::atomic<bool>            m_running{false};
    std::atomic<int>             m_generation{0};

    mutable std::mutex           m_queueMutex;
    std::condition_variable      m_queueCv;
    std::deque<Item>             m_queue;

    mutable std::shared_mutex                  m_textureMapMutex;
    std::unordered_map<int, GpuTexture>        m_textureMap;

    static constexpr int kMaxUploadsPerCycle = 8;
};

} // namespace qcv
