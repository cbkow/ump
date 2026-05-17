#include "frame_index.h"

#include <QtLogging>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace qcv {

struct FrameIndex::Impl
{
    // Tier 1 — formula state, set at construction.
    int  tbNum = 0;
    int  tbDen = 1;
    int  frNum = 0;
    int  frDen = 1;
    int  totalFrames = 0;
    bool isIntraOnly = false;

    // Tier 2 — packet scan state.
    struct Entry {
        int64_t pts;
        bool    isKeyframe;
    };
    mutable std::mutex     mutex;     // protects entries
    std::vector<Entry>     entries;   // sorted by pts when state==Ready
    std::atomic<ScanState> state{ScanState::NotStarted};
    std::atomic<int>       framesBuilt{0};

    std::thread       thread;
    std::atomic<bool> stopRequested{false};

    ~Impl()
    {
        stopRequested.store(true, std::memory_order_release);
        if (thread.joinable()) thread.join();
    }

    void runScan(QString path, int videoStreamIdx);
};

void FrameIndex::Impl::runScan(QString path, int videoStreamIdx)
{
    state.store(ScanState::Building, std::memory_order_release);

    AVFormatContext *fmt = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&fmt, pathUtf8.constData(), nullptr, nullptr) < 0) {
        state.store(ScanState::Failed, std::memory_order_release);
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        state.store(ScanState::Failed, std::memory_order_release);
        return;
    }

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        avformat_close_input(&fmt);
        state.store(ScanState::Failed, std::memory_order_release);
        return;
    }

    std::vector<Entry> built;
    if (totalFrames > 0) built.reserve(static_cast<size_t>(totalFrames));

    while (!stopRequested.load(std::memory_order_acquire)) {
        const int rc = av_read_frame(fmt, pkt);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            qWarning("FrameIndex scan: av_read_frame error %d", rc);
            break;
        }
        if (pkt->stream_index == videoStreamIdx) {
            const int64_t pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts
                                                              : pkt->dts;
            if (pts != AV_NOPTS_VALUE) {
                built.push_back({ pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0 });
                framesBuilt.store(static_cast<int>(built.size()),
                                  std::memory_order_release);
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt);

    if (stopRequested.load(std::memory_order_acquire)) {
        state.store(ScanState::Failed, std::memory_order_release);
        return;
    }

    // Sort by PTS (display order); the packet read order is decode
    // order which differs for inter-frame codecs with B-frame reorder.
    std::sort(built.begin(), built.end(),
              [](const Entry &a, const Entry &b) { return a.pts < b.pts; });

    {
        std::lock_guard<std::mutex> lk(mutex);
        entries = std::move(built);
    }
    state.store(ScanState::Ready, std::memory_order_release);
}

// -------- public API --------

FrameIndex::FrameIndex()
    : m_impl(std::make_unique<Impl>())
{}

FrameIndex::FrameIndex(int tbNum, int tbDen, int frNum, int frDen,
                       int totalFrames, bool isIntraOnly)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->tbNum = tbNum;
    m_impl->tbDen = tbDen;
    m_impl->frNum = frNum;
    m_impl->frDen = frDen;
    m_impl->totalFrames = totalFrames;
    m_impl->isIntraOnly = isIntraOnly;
}

FrameIndex::~FrameIndex() = default;
FrameIndex::FrameIndex(FrameIndex &&) noexcept = default;
FrameIndex &FrameIndex::operator=(FrameIndex &&) noexcept = default;

bool FrameIndex::isValid() const
{
    return m_impl && m_impl->tbDen > 0 && m_impl->frNum > 0 && m_impl->frDen > 0;
}

int64_t FrameIndex::ptsForFrame(int frameNo) const
{
    if (!isValid()) return 0;
    if (m_impl->state.load(std::memory_order_acquire) == ScanState::Ready) {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        if (frameNo >= 0 &&
            frameNo < static_cast<int>(m_impl->entries.size())) {
            return m_impl->entries[frameNo].pts;
        }
    }
    AVRational frameDur{ m_impl->frDen, m_impl->frNum };
    AVRational timeBase{ m_impl->tbNum, m_impl->tbDen };
    return av_rescale_q(frameNo, frameDur, timeBase);
}

int FrameIndex::frameForPts(int64_t pts) const
{
    if (!isValid()) return 0;
    if (m_impl->state.load(std::memory_order_acquire) == ScanState::Ready) {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        const auto &e = m_impl->entries;
        // Binary search for nearest pts at-or-before. lower_bound on
        // a comparator that compares pts directly.
        auto it = std::lower_bound(e.begin(), e.end(), pts,
            [](const Impl::Entry &a, int64_t v) { return a.pts < v; });
        if (it == e.end()) return static_cast<int>(e.size()) - 1;
        if (it->pts == pts) return static_cast<int>(it - e.begin());
        // strictly greater — return previous if any
        if (it == e.begin()) return 0;
        return static_cast<int>(it - e.begin()) - 1;
    }
    AVRational timeBase{ m_impl->tbNum, m_impl->tbDen };
    AVRational frameDur{ m_impl->frDen, m_impl->frNum };
    return static_cast<int>(av_rescale_q(pts, timeBase, frameDur));
}

int FrameIndex::frameForTime(double seconds) const
{
    if (!isValid() || seconds <= 0.0) return 0;
    return static_cast<int>(seconds * fps() + 0.5);
}

int  FrameIndex::totalFrames()  const { return m_impl ? m_impl->totalFrames : 0; }
bool FrameIndex::isIntraOnly()  const { return m_impl ? m_impl->isIntraOnly : false; }
int  FrameIndex::timeBaseNum()  const { return m_impl ? m_impl->tbNum : 0; }
int  FrameIndex::timeBaseDen()  const { return m_impl ? m_impl->tbDen : 1; }
int  FrameIndex::frameRateNum() const { return m_impl ? m_impl->frNum : 0; }
int  FrameIndex::frameRateDen() const { return m_impl ? m_impl->frDen : 1; }

double FrameIndex::fps() const
{
    if (!m_impl || m_impl->frDen == 0) return 0.0;
    return static_cast<double>(m_impl->frNum) / static_cast<double>(m_impl->frDen);
}

void FrameIndex::startScan(const QString &path, int videoStreamIdx)
{
    if (!m_impl) return;
    cancelScan();
    m_impl->stopRequested.store(false, std::memory_order_release);
    m_impl->framesBuilt.store(0, std::memory_order_release);
    Impl *impl = m_impl.get();
    m_impl->thread = std::thread([impl, path, videoStreamIdx]() {
        impl->runScan(path, videoStreamIdx);
    });
}

void FrameIndex::cancelScan()
{
    if (!m_impl) return;
    m_impl->stopRequested.store(true, std::memory_order_release);
    if (m_impl->thread.joinable()) m_impl->thread.join();
    // Reset stop flag so a subsequent startScan can run.
    m_impl->stopRequested.store(false, std::memory_order_release);
}

FrameIndex::ScanState FrameIndex::scanState() const
{
    if (!m_impl) return ScanState::NotStarted;
    return m_impl->state.load(std::memory_order_acquire);
}

int FrameIndex::scanFramesBuilt() const
{
    if (!m_impl) return 0;
    return m_impl->framesBuilt.load(std::memory_order_acquire);
}

bool FrameIndex::hasExactMapping() const
{
    return scanState() == ScanState::Ready;
}

int64_t FrameIndex::keyframePtsBefore(int targetFrame) const
{
    if (!isValid() || !hasExactMapping()) return -1;
    std::lock_guard<std::mutex> lk(m_impl->mutex);
    const auto &e = m_impl->entries;
    if (e.empty()) return -1;
    const int idx = std::min(static_cast<int>(e.size()) - 1,
                             std::max(0, targetFrame));
    for (int i = idx; i >= 0; --i) {
        if (e[i].isKeyframe) return e[i].pts;
    }
    return -1;
}

} // namespace qcv
