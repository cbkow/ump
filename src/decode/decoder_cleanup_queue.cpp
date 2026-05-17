#include "decoder_cleanup_queue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libswscale/swscale.h>
}

namespace qcv {

DecoderCleanupQueue &DecoderCleanupQueue::instance()
{
    static DecoderCleanupQueue inst;
    return inst;
}

DecoderCleanupQueue::DecoderCleanupQueue()
{
    m_thread = std::thread([this] { run(); });
}

DecoderCleanupQueue::~DecoderCleanupQueue()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
    }
    m_cond.notify_one();
    if (m_thread.joinable()) m_thread.join();
}

void DecoderCleanupQueue::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_tasks.push(std::move(task));
    }
    m_cond.notify_one();
}

void DecoderCleanupQueue::run()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cond.wait(lk, [this] {
                return m_stop || !m_tasks.empty();
            });
            // Drain pending tasks even on shutdown — leaking ffmpeg
            // contexts at exit is technically fine but noisy in
            // Instruments / leak detectors.
            if (m_tasks.empty()) return;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        try {
            task();
        } catch (...) {
            // Swallow — cleanup tasks must not propagate exceptions.
        }
    }
}

void postFFmpegCleanup(AVFormatContext *fmt,
                       AVCodecContext  *cctx,
                       SwsContext      *sws,
                       AVBufferRef     *hwDevice)
{
    if (!fmt && !cctx && !sws && !hwDevice) return;

    DecoderCleanupQueue::instance().post([fmt, cctx, sws, hwDevice]() mutable {
        if (sws)      sws_freeContext(sws);
        if (cctx)     avcodec_free_context(&cctx);
        if (fmt)      avformat_close_input(&fmt);
        if (hwDevice) av_buffer_unref(&hwDevice);
    });
}

} // namespace qcv
