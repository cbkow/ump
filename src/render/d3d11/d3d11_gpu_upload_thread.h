// D3D11GpuUploadThread — Phase F.2.2.
//
// Standalone thread that drains a queue of (frame, PixelData,
// generation) items and creates ID3D11Texture2Ds via D3D11TexturePool.
// Consumed by D3D11PlayerRenderer (F.2.6+) for the image-sequence
// path; mirrors MetalGpuUploadThread structurally so ImageSequenceCache's
// GpuPushCallback / FrameEvictedCallback wiring is identical on both
// platforms.
//
// Threading model (identical to Metal):
//   I/O thread (cache):    enqueue() pushes new items
//   Upload thread (us):    drains queue, creates textures, stores
//                           handles in m_textureMap
//   Render thread:         textureForFrame() reads handles

#pragma once

#include "d3d11_texture_pool.h"

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

class D3D11GpuUploadThread {
public:
    struct GpuTexture {
        D3D11TexturePool::Handle handle = 0;
        int                      width  = 0;
        int                      height = 0;
        bool                     valid  = false;
    };

    D3D11GpuUploadThread();
    ~D3D11GpuUploadThread();

    D3D11GpuUploadThread(const D3D11GpuUploadThread &)            = delete;
    D3D11GpuUploadThread &operator=(const D3D11GpuUploadThread &) = delete;

    bool start();
    void stop();

    void enqueue(int frame, std::shared_ptr<const PixelData> pixels,
                 int generation);

    void setGeneration(int generation);

    GpuTexture textureForFrame(int frame) const;

    void evictFrame(int frame);
    void clearAll();

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
