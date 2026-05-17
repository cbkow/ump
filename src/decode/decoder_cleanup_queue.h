// DecoderCleanupQueue — Phase 1.8.6.
//
// Process-wide singleton that owns one background thread. Decoders
// post their slow shutdown work (thread joins + ffmpeg context
// releases, especially av_buffer_unref of hwframes_ctx which can
// take ~50 ms on VideoToolbox) here so the UI thread doesn't block
// when a file is closed.
//
// Per Guide 13 §I.9: caller doesn't block; the queue thread runs
// the destruction asynchronously. On app shutdown the queue's
// destructor drains pending tasks and joins the thread.
//
// The queue is intentionally generic — it takes std::function<void()>
// — so any decode-side cleanup can ride on it. A free helper
// postFFmpegCleanup() packages the typical (thread + fmt + cctx +
// sws + hwDevice) tear-down into one call site.

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct SwsContext;

namespace qcv {

class DecoderCleanupQueue
{
public:
    static DecoderCleanupQueue &instance();

    void post(std::function<void()> task);

    ~DecoderCleanupQueue();
    DecoderCleanupQueue(const DecoderCleanupQueue &) = delete;
    DecoderCleanupQueue &operator=(const DecoderCleanupQueue &) = delete;

private:
    DecoderCleanupQueue();
    void run();

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cond;
    std::queue<std::function<void()>> m_tasks;
    bool                    m_stop = false;
};

// Convenience for the VideoDecoder / ScrubDecoder close path.
// Caller MUST have already joined the decoder thread before calling
// — synchronous join is required for safety (the decode loop reads
// the ffmpeg contexts as members each iteration; freeing them while
// the thread is still active is a use-after-free). The async win
// here is solely the slow av_buffer_unref of hwframes_ctx (~50 ms
// on VideoToolbox session teardown), which now runs off the UI
// thread.
void postFFmpegCleanup(AVFormatContext *fmt,
                       AVCodecContext  *cctx,
                       SwsContext      *sws,
                       AVBufferRef     *hwDevice);

} // namespace qcv
