#include "frame_index.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cmath>

namespace qcview {

FrameIndex::FrameIndex() = default;

FrameIndex::~FrameIndex() {
    CancelAsyncScan();
}

//=============================================================================
// Tier 1: Project Cache
//=============================================================================

void FrameIndex::LoadFromCache(const std::vector<int64_t>& keyframe_pts,
                               int total_frames,
                               int timebase_num, int timebase_den,
                               double fps, bool is_all_intra) {
    std::lock_guard<std::mutex> lock(mutex_);

    timebase_num_ = timebase_num;
    timebase_den_ = timebase_den;
    fps_ = fps;
    total_frames_ = total_frames;
    is_all_intra_ = is_all_intra;

    if (is_all_intra) {
        // All-intra: no entries needed, arithmetic is exact per-frame
        complete_.store(true, std::memory_order_release);
        Debug::Log("FrameIndex: Loaded from cache (all-intra, " +
                   std::to_string(total_frames) + " frames)");
        return;
    }

    // Populate keyframe list from cached PTS values
    // Convert PTS → frame numbers using arithmetic (best we can do without full index)
    keyframe_frames_.clear();
    keyframe_frames_.reserve(keyframe_pts.size());
    for (int64_t pts : keyframe_pts) {
        int frame = EstimateFrameForPTS(pts);
        keyframe_frames_.push_back(frame);
    }
    std::sort(keyframe_frames_.begin(), keyframe_frames_.end());

    // entries_ stays empty — per-frame queries use arithmetic fallback
    // complete_ stays false — Tier 2 will fill in the full table
    Debug::Log("FrameIndex: Loaded from cache (" +
               std::to_string(keyframe_frames_.size()) + " keyframes, " +
               std::to_string(total_frames) + " frames, incomplete)");
}

//=============================================================================
// Tier 2: AVStream index_entries (in-memory, no I/O)
//=============================================================================

bool FrameIndex::BuildFromStreamIndex(AVFormatContext* fmt_ctx, int stream_index,
                                      double fps) {
    if (!fmt_ctx || stream_index < 0 ||
        stream_index >= static_cast<int>(fmt_ctx->nb_streams)) {
        return false;
    }

    AVStream* stream = fmt_ctx->streams[stream_index];
    int count = avformat_index_get_entries_count(stream);
    if (count <= 0) {
        Debug::Log("FrameIndex: No index entries in stream (count=" +
                   std::to_string(count) + ")");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    timebase_num_ = stream->time_base.num;
    timebase_den_ = stream->time_base.den;
    fps_ = fps;

    // Read all entries
    std::vector<FrameEntry> raw_entries;
    raw_entries.reserve(count);

    for (int i = 0; i < count; i++) {
        const AVIndexEntry* ie = avformat_index_get_entry(stream, i);
        if (!ie) continue;

        FrameEntry entry;
        entry.pts = ie->timestamp;  // For MOV/MP4 this is composition time (PTS)
        entry.dts = AV_NOPTS_VALUE; // index_entries don't store DTS separately
        entry.is_keyframe = (ie->flags & AVINDEX_KEYFRAME) != 0;
        raw_entries.push_back(entry);
    }

    if (raw_entries.empty()) {
        Debug::Log("FrameIndex: All index entries were null");
        return false;
    }

    // Sort by PTS (presentation order). index_entries may be in decode order
    // for B-frame codecs, but for MOV/MP4 they should already be in PTS order
    // since FFmpeg applies ctts offsets.
    std::sort(raw_entries.begin(), raw_entries.end(),
              [](const FrameEntry& a, const FrameEntry& b) {
                  return a.pts < b.pts;
              });

    // Remove duplicates (same PTS)
    raw_entries.erase(
        std::unique(raw_entries.begin(), raw_entries.end(),
                    [](const FrameEntry& a, const FrameEntry& b) {
                        return a.pts == b.pts;
                    }),
        raw_entries.end());

    entries_ = std::move(raw_entries);
    total_frames_ = static_cast<int>(entries_.size());

    // Build keyframe list
    RebuildKeyframeList();

    complete_.store(true, std::memory_order_release);

    Debug::Log("FrameIndex: Built from stream index (" +
               std::to_string(total_frames_) + " frames, " +
               std::to_string(keyframe_frames_.size()) + " keyframes, " +
               "timebase=" + std::to_string(timebase_num_) + "/" +
               std::to_string(timebase_den_) + ")");
    return true;
}

//=============================================================================
// Tier 3: Async Packet Scan
//=============================================================================

void FrameIndex::BuildFromPacketScanAsync(const std::string& file_path,
                                          int stream_index, double fps,
                                          std::function<void()> on_complete) {
    CancelAsyncScan();  // Cancel any previous scan

    {
        std::lock_guard<std::mutex> lock(mutex_);
        fps_ = fps;
    }

    cancel_scan_.store(false);

    scan_thread_ = std::thread([this, file_path, stream_index, fps,
                                 on_complete = std::move(on_complete)]() {
        Debug::Log("FrameIndex: Starting async packet scan for " + file_path);

        // Open a separate format context (decoder's is in use)
        AVFormatContext* scan_ctx = nullptr;
        int ret = avformat_open_input(&scan_ctx, file_path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            Debug::Log("FrameIndex: Failed to open file for packet scan");
            return;
        }

        ret = avformat_find_stream_info(scan_ctx, nullptr);
        if (ret < 0 || stream_index >= static_cast<int>(scan_ctx->nb_streams)) {
            avformat_close_input(&scan_ctx);
            Debug::Log("FrameIndex: Failed to find stream info for packet scan");
            return;
        }

        AVStream* stream = scan_ctx->streams[stream_index];

        {
            std::lock_guard<std::mutex> lock(mutex_);
            timebase_num_ = stream->time_base.num;
            timebase_den_ = stream->time_base.den;
        }

        // Seek to beginning
        av_seek_frame(scan_ctx, stream_index, 0, AVSEEK_FLAG_BACKWARD);

        // Scan all packets
        AVPacket* pkt = av_packet_alloc();
        std::vector<FrameEntry> scanned;
        scanned.reserve(8192);

        while (av_read_frame(scan_ctx, pkt) >= 0) {
            if (cancel_scan_.load()) {
                av_packet_unref(pkt);
                break;
            }

            if (pkt->stream_index == stream_index) {
                FrameEntry entry;
                entry.pts = pkt->pts;
                entry.dts = pkt->dts;
                entry.is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                scanned.push_back(entry);
            }
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        avformat_close_input(&scan_ctx);

        if (cancel_scan_.load()) {
            Debug::Log("FrameIndex: Packet scan cancelled");
            return;
        }

        // Sort by PTS (presentation order)
        std::sort(scanned.begin(), scanned.end(),
                  [](const FrameEntry& a, const FrameEntry& b) {
                      return a.pts < b.pts;
                  });

        // Remove duplicates
        scanned.erase(
            std::unique(scanned.begin(), scanned.end(),
                        [](const FrameEntry& a, const FrameEntry& b) {
                            return a.pts == b.pts;
                        }),
            scanned.end());

        // Commit results
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_ = std::move(scanned);
            total_frames_ = static_cast<int>(entries_.size());
            RebuildKeyframeList();
            complete_.store(true, std::memory_order_release);
        }

        Debug::Log("FrameIndex: Packet scan complete (" +
                   std::to_string(total_frames_) + " frames, " +
                   std::to_string(keyframe_frames_.size()) + " keyframes)");

        if (on_complete) {
            on_complete();
        }
    });
}

void FrameIndex::CancelAsyncScan() {
    cancel_scan_.store(true);
    if (scan_thread_.joinable()) {
        scan_thread_.join();
    }
}

//=============================================================================
// Runtime Registration
//=============================================================================

void FrameIndex::RegisterDecodedFrame(int64_t pts, int64_t dts, bool is_keyframe) {
    if (pts == AV_NOPTS_VALUE) return;
    if (complete_.load(std::memory_order_acquire)) return;  // Full index already built

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if this PTS is already registered
    auto it = std::lower_bound(entries_.begin(), entries_.end(), pts,
                                [](const FrameEntry& e, int64_t p) {
                                    return e.pts < p;
                                });
    if (it != entries_.end() && it->pts == pts) {
        return;  // Already known
    }

    // Insert maintaining sorted order
    FrameEntry entry;
    entry.pts = pts;
    entry.dts = dts;
    entry.is_keyframe = is_keyframe;
    entries_.insert(it, entry);

    // Update total frames
    total_frames_ = static_cast<int>(entries_.size());

    // Update keyframe list if this is a keyframe
    if (is_keyframe) {
        int frame_num = static_cast<int>(std::distance(entries_.begin(),
            std::lower_bound(entries_.begin(), entries_.end(), pts,
                             [](const FrameEntry& e, int64_t p) {
                                 return e.pts < p;
                             })));
        auto kf_it = std::lower_bound(keyframe_frames_.begin(),
                                       keyframe_frames_.end(), frame_num);
        if (kf_it == keyframe_frames_.end() || *kf_it != frame_num) {
            keyframe_frames_.insert(kf_it, frame_num);
        }
    }
}

//=============================================================================
// Queries
//=============================================================================

int FrameIndex::FrameForPTS(int64_t pts) const {
    if (pts == AV_NOPTS_VALUE) return -1;

    std::lock_guard<std::mutex> lock(mutex_);

    if (is_all_intra_ || entries_.empty()) {
        // Arithmetic fallback
        return EstimateFrameForPTS(pts);
    }

    // Binary search for exact PTS match
    auto it = std::lower_bound(entries_.begin(), entries_.end(), pts,
                                [](const FrameEntry& e, int64_t p) {
                                    return e.pts < p;
                                });

    if (it != entries_.end() && it->pts == pts) {
        return static_cast<int>(std::distance(entries_.begin(), it));
    }

    // No exact match — find closest
    if (it == entries_.end()) {
        // PTS is beyond all entries
        if (!entries_.empty()) {
            return static_cast<int>(entries_.size() - 1);
        }
        return -1;
    }

    if (it == entries_.begin()) {
        return 0;
    }

    // Check which neighbor is closer
    auto prev = std::prev(it);
    int64_t dist_prev = pts - prev->pts;
    int64_t dist_next = it->pts - pts;
    if (dist_prev <= dist_next) {
        return static_cast<int>(std::distance(entries_.begin(), prev));
    } else {
        return static_cast<int>(std::distance(entries_.begin(), it));
    }
}

int64_t FrameIndex::PTSForFrame(int frame) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_all_intra_ || entries_.empty()) {
        return EstimatePTSForFrame(frame);
    }

    if (frame < 0) frame = 0;
    if (frame >= static_cast<int>(entries_.size())) {
        // Extrapolate from last known entry
        if (!entries_.empty()) {
            // Estimate PTS spacing from last two entries
            if (entries_.size() >= 2) {
                int64_t last_pts = entries_.back().pts;
                int64_t prev_pts = entries_[entries_.size() - 2].pts;
                int64_t delta = last_pts - prev_pts;
                int extra = frame - static_cast<int>(entries_.size()) + 1;
                return last_pts + delta * extra;
            }
        }
        return EstimatePTSForFrame(frame);
    }

    return entries_[frame].pts;
}

int FrameIndex::NearestKeyframeBefore(int frame) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_all_intra_) return frame;
    if (keyframe_frames_.empty()) return frame;

    auto it = std::upper_bound(keyframe_frames_.begin(),
                                keyframe_frames_.end(), frame);
    if (it == keyframe_frames_.begin()) {
        return keyframe_frames_.front();
    }
    --it;
    return *it;
}

int64_t FrameIndex::KeyframePTSBefore(int frame) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_all_intra_) {
        if (!entries_.empty() && frame >= 0 &&
            frame < static_cast<int>(entries_.size())) {
            return entries_[frame].pts;
        }
        return EstimatePTSForFrame(frame);
    }

    // Find nearest keyframe frame number
    int kf_frame = frame;
    if (!keyframe_frames_.empty()) {
        auto it = std::upper_bound(keyframe_frames_.begin(),
                                    keyframe_frames_.end(), frame);
        if (it != keyframe_frames_.begin()) {
            --it;
            kf_frame = *it;
        } else {
            kf_frame = keyframe_frames_.front();
        }
    }

    // Look up its PTS from entries
    if (!entries_.empty() && kf_frame >= 0 &&
        kf_frame < static_cast<int>(entries_.size())) {
        return entries_[kf_frame].pts;
    }

    return EstimatePTSForFrame(kf_frame);
}

int FrameIndex::FramesBetween(int keyframe_frame, int target_frame) const {
    if (target_frame <= keyframe_frame) return 0;
    return target_frame - keyframe_frame;
}

int FrameIndex::TotalFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_frames_;
}

//=============================================================================
// Serialization
//=============================================================================

std::vector<int64_t> FrameIndex::GetKeyframePTSList() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<int64_t> result;
    if (is_all_intra_) return result;  // Don't store for all-intra

    result.reserve(keyframe_frames_.size());
    for (int kf : keyframe_frames_) {
        if (kf >= 0 && kf < static_cast<int>(entries_.size())) {
            result.push_back(entries_[kf].pts);
        }
    }
    return result;
}

std::vector<int> FrameIndex::GetKeyframePositions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keyframe_frames_;
}

//=============================================================================
// Private Helpers
//=============================================================================

int64_t FrameIndex::EstimatePTSForFrame(int frame) const {
    // Arithmetic fallback: frame_number / fps * timebase_den / timebase_num
    if (fps_ <= 0 || timebase_num_ <= 0 || timebase_den_ <= 0) return 0;
    double seconds = static_cast<double>(frame) / fps_;
    return static_cast<int64_t>(std::round(seconds * timebase_den_ / timebase_num_));
}

int FrameIndex::EstimateFrameForPTS(int64_t pts) const {
    // Arithmetic fallback: pts * timebase_num / timebase_den * fps
    if (fps_ <= 0 || timebase_den_ <= 0) return 0;
    double seconds = static_cast<double>(pts) * timebase_num_ / timebase_den_;
    return static_cast<int>(std::round(seconds * fps_));
}

void FrameIndex::RebuildKeyframeList() {
    // Caller must hold mutex_
    keyframe_frames_.clear();
    for (int i = 0; i < static_cast<int>(entries_.size()); i++) {
        if (entries_[i].is_keyframe) {
            keyframe_frames_.push_back(i);
        }
    }
}

} // namespace qcview
