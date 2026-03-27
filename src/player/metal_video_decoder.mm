#ifdef __APPLE__

#include "metal_video_decoder.h"
#include "../gpu/metal_device_manager.h"
#include "../gpu/metal_texture_pool.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace qcview {

//=============================================================================
// NEON-accelerated UV plane interleave (Apple Silicon)
// ~8x faster than scalar: processes 16 pixels per iteration via vld1q/vst2q.
//=============================================================================

#if defined(__aarch64__)
static void InterleaveUV_NEON_8bit(const uint8_t* u_plane, int u_stride,
                                    const uint8_t* v_plane, int v_stride,
                                    uint8_t* uv_out, int uv_stride,
                                    int width, int height) {
    for (int row = 0; row < height; row++) {
        const uint8_t* u_row = u_plane + row * u_stride;
        const uint8_t* v_row = v_plane + row * v_stride;
        uint8_t* dst = uv_out + row * uv_stride;

        int col = 0;
        for (; col + 16 <= width; col += 16) {
            uint8x16_t u = vld1q_u8(u_row + col);
            uint8x16_t v = vld1q_u8(v_row + col);
            uint8x16x2_t uv = {u, v};
            vst2q_u8(dst + col * 2, uv);
        }
        // Scalar tail for non-aligned widths
        for (; col < width; col++) {
            dst[col * 2]     = u_row[col];
            dst[col * 2 + 1] = v_row[col];
        }
    }
}

static void InterleaveUV_NEON_16bit(const uint8_t* u_plane, int u_stride,
                                     const uint8_t* v_plane, int v_stride,
                                     uint8_t* uv_out, int uv_stride,
                                     int width, int height) {
    for (int row = 0; row < height; row++) {
        const uint16_t* u_row = reinterpret_cast<const uint16_t*>(u_plane + row * u_stride);
        const uint16_t* v_row = reinterpret_cast<const uint16_t*>(v_plane + row * v_stride);
        uint16_t* dst = reinterpret_cast<uint16_t*>(uv_out + row * uv_stride);

        int col = 0;
        for (; col + 8 <= width; col += 8) {
            uint16x8_t u = vld1q_u16(u_row + col);
            uint16x8_t v = vld1q_u16(v_row + col);
            uint16x8x2_t uv = {u, v};
            vst2q_u16(dst + col * 2, uv);
        }
        for (; col < width; col++) {
            dst[col * 2]     = u_row[col];
            dst[col * 2 + 1] = v_row[col];
        }
    }
}
#endif // __aarch64__

// Codecs that VideoToolbox can HW-decode on Apple Silicon
static bool IsVideoToolboxSupported(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_PRORES:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_AV1:       // M3+ only, but FFmpeg handles fallback
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_MPEG2VIDEO:
            return true;
        default:
            return false;
    }
}

// FFmpeg callback: select the HW pixel format when VideoToolbox is available
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                         const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            return AV_PIX_FMT_VIDEOTOOLBOX;
        }
    }
    // Fallback to software format
    return pix_fmts[0];
}

//=============================================================================
// BufferedFrame
//=============================================================================

void MetalVideoDecoder::BufferedFrame::Reset() {
    // DON'T QueueDelete pool textures here.
    // The timeline cache may still reference this texture as last_good_texture_.
    // Pool LRU eviction handles memory cleanup when space is needed.
    frame_number = -1;
    valid = false;
    pool_texture_id = 0;
    gpu_uploaded = false;
    pixel_data.reset();
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

MetalVideoDecoder::MetalVideoDecoder(const std::string& file_path, const StreamingDecoderConfig& config)
    : file_path_(file_path), config_(config) {
}

MetalVideoDecoder::~MetalVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool MetalVideoDecoder::Initialize() {
    if (initialized_.load()) return true;

    Debug::Log("MetalVideoDecoder: Initializing for " + file_path_);

    if (!OpenFile()) {
        Debug::Log("MetalVideoDecoder: Failed to open file");
        return false;
    }

    // Build frame index from container metadata (replaces BuildKeyframeIndex)
    frame_index_ = std::make_shared<FrameIndex>();
    bool index_built = frame_index_->BuildFromStreamIndex(format_ctx_, video_stream_idx_, fps_);
    if (!index_built && !is_intra_codec_) {
        // Fallback: async packet scan for containers without index entries
        frame_index_->BuildFromPacketScanAsync(file_path_, video_stream_idx_, fps_);
    } else if (is_intra_codec_) {
        // All-intra: load with arithmetic-only mode
        AVStream* stream = format_ctx_->streams[video_stream_idx_];
        frame_index_->LoadFromCache({}, frame_count_,
                                     stream->time_base.num, stream->time_base.den,
                                     fps_, true);
    }
    // Update frame count from index for intra codecs only.
    // For inter-frame codecs (H.264/HEVC), the FrameIndex PTS deduplication
    // can produce fewer entries than the actual frame count, causing the
    // decoder to clamp seeks short of the real end.
    if (is_intra_codec_ && frame_index_->IsComplete() && frame_index_->TotalFrames() > 0) {
        int old_count = frame_count_;
        frame_count_ = frame_index_->TotalFrames();
        if (old_count != frame_count_) {
            Debug::Log("MetalVideoDecoder: Frame count updated from " +
                       std::to_string(old_count) + " to " +
                       std::to_string(frame_count_) + " (from FrameIndex)");
        }
    }
    Debug::Log("MetalVideoDecoder: FrameIndex status: complete=" +
               std::string(frame_index_->IsComplete() ? "YES" : "NO") +
               " all_intra=" + std::string(frame_index_->IsAllIntra() ? "YES" : "NO") +
               " total_frames=" + std::to_string(frame_index_->TotalFrames()) +
               " (index_built=" + std::string(index_built ? "STREAM" : (is_intra_codec_ ? "INTRA" : "ASYNC_SCAN")) + ")");
    // Log keyframe positions for debugging seek behavior
    if (frame_index_ && !is_intra_codec_) {
        auto kf_positions = frame_index_->GetKeyframePositions();
        std::string kf_str;
        for (int kf : kf_positions) {
            if (!kf_str.empty()) kf_str += ",";
            kf_str += std::to_string(kf);
        }
        Debug::Log("MetalVideoDecoder: Keyframes at frames [" + kf_str + "]");
    }

    // Detect HDR/10-bit/color properties from codec context
    if (codec_ctx_) {
        is_hdr_ = (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
                   codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);
        is_bt2020_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020);
        is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);

        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_->pix_fmt);
        if (desc && desc->comp[0].depth > 8) {
            is_10bit_ = true;
        }
        // ProRes is always treated as 10-bit quality
        if (codec_ctx_->codec_id == AV_CODEC_ID_PRORES) {
            is_10bit_ = true;
        }
    }

    // Initialize GPU pipeline — needed for both HW (VideoToolbox) and SW (YUV plane upload) paths
    if (hw_accel_type_ == HWAccelType::VIDEOTOOLBOX) {
        hw_extractor_ = std::make_unique<MetalHWFrameExtractor>();
        yuv_renderer_ = std::make_unique<MetalYUVRenderer>();

        if (hw_extractor_->Initialize() && yuv_renderer_->Initialize()) {
            gpu_decode_available_ = true;
            Debug::Log("MetalVideoDecoder: GPU decode pipeline ready (zero-copy HW)");
        } else {
            Debug::Log("MetalVideoDecoder: GPU pipeline init failed, will use CPU fallback");
            hw_extractor_.reset();
            yuv_renderer_.reset();
            gpu_decode_available_ = false;
        }
    } else {
        // SW decode: initialize YUV renderer for GPU-side YUV→RGBA conversion
        // (same shader as HW path, but fed with uploaded YUV plane textures)
        yuv_renderer_ = std::make_unique<MetalYUVRenderer>();
        if (yuv_renderer_->Initialize()) {
            gpu_decode_available_ = true;
            Debug::Log("MetalVideoDecoder: GPU decode pipeline ready (SW YUV upload)");
        } else {
            Debug::Log("MetalVideoDecoder: YUV renderer init failed, will use sws_scale fallback");
            yuv_renderer_.reset();
            gpu_decode_available_ = false;
        }
    }

    // Initialize B-frame delay queue state (matches Vulkan/D3D11)
    frames_since_seek_ = 0;
    if (codec_ctx_ && codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
    } else {
        delay_queue_filling_ = false;
    }

    // Start decode thread
    shutdown_requested_ = false;
    eof_reached_ = false;
    thread_running_ = true;
    decode_thread_ = std::thread(&MetalVideoDecoder::DecodeThreadFunc, this);

    initialized_ = true;
    Debug::Log("MetalVideoDecoder: Initialized (" +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(frame_count_) + " frames, " +
               std::string(IsHardwareAccelerated() ? "VideoToolbox HW" : "Software") +
               (gpu_decode_available_ ? " zero-copy" : "") + ")");
    return true;
}

void MetalVideoDecoder::RequestShutdown() {
    // Signal decode thread to stop without joining — allows parallel shutdown of multiple decoders.
    // Shutdown() or destructor will join the thread later.
    if (initialized_.load() || thread_running_.load()) {
        shutdown_requested_ = true;
        decode_cv_.notify_all();
    }
}

void MetalVideoDecoder::Shutdown() {
    if (!initialized_.load() && !thread_running_.load()) return;

    Debug::Log("MetalVideoDecoder: Shutting down...");

    // Cancel any async frame index scan before stopping decode thread
    if (frame_index_) {
        frame_index_->CancelAsyncScan();
    }

    // Stop decode thread (signal + join)
    shutdown_requested_ = true;
    decode_cv_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    thread_running_ = false;

    // Clear ring buffer — DON'T delete pool textures (ImGui draw list may reference them).
    // They'll be cleaned up by the pool's LRU eviction or pool shutdown.
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (auto& bf : frame_buffer_) {
            bf.Reset();
        }
        frame_map_.clear();
        buffer_head_ = 0;
        buffer_count_ = 0;
    }

    // Flush any in-flight async GPU work before destroying the pipeline.
    // Decode thread is already joined above, so no new work can be submitted.
    if (yuv_renderer_) yuv_renderer_->FlushPendingWork();

    // Release GPU pipeline
    yuv_renderer_.reset();
    hw_extractor_.reset();
    gpu_decode_available_ = false;

    // Close FFmpeg contexts
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(static_cast<struct SwsContext*>(sws_ctx_));
        sws_ctx_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("MetalVideoDecoder: Shutdown complete");
}

void MetalVideoDecoder::HardReset(int target_frame) {
    Debug::Log("MetalVideoDecoder: Hard reset to frame " + std::to_string(target_frame));
    Shutdown();
    Initialize();
    UpdatePlayhead(target_frame);
}

//=============================================================================
// File Opening — VideoToolbox HW accel set up BEFORE avcodec_open2
//=============================================================================

bool MetalVideoDecoder::OpenFile() {
    int ret = avformat_open_input(&format_ctx_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("MetalVideoDecoder: avformat_open_input failed: " + std::string(errbuf));
        return false;
    }

    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        Debug::Log("MetalVideoDecoder: avformat_find_stream_info failed");
        return false;
    }

    // Find best video stream
    video_stream_idx_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        Debug::Log("MetalVideoDecoder: No video stream found");
        return false;
    }

    AVStream* stream = format_ctx_->streams[video_stream_idx_];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        Debug::Log("MetalVideoDecoder: No decoder found for codec");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, stream->codecpar);

    // Set decode threading for software decode (matches D3D11/Vulkan)
    codec_ctx_->thread_count = 0;  // Auto-detect thread count
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // *** Set up VideoToolbox BEFORE opening codec ***
    if (config_.useHardwareAccel && IsVideoToolboxSupported(codec_ctx_->codec_id)) {
        if (SetupHWAccel()) {
            Debug::Log("MetalVideoDecoder: VideoToolbox enabled for " +
                       std::string(avcodec_get_name(codec_ctx_->codec_id)));
        }
    }

    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("MetalVideoDecoder: avcodec_open2 failed: " + std::string(errbuf));
        return false;
    }

    // Extract metadata
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // FPS from stream
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        fps_ = av_q2d(stream->avg_frame_rate);
    } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        fps_ = av_q2d(stream->r_frame_rate);
    } else {
        fps_ = 24.0;
    }

    // Duration: prefer stream duration over container duration
    if (stream->duration > 0 && stream->time_base.den > 0) {
        duration_ = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    } else if (format_ctx_->duration > 0) {
        duration_ = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
    } else {
        duration_ = 0.0;
    }

    // Frame count: prefer container's reported count (exact for MOV/MP4),
    // fall back to arithmetic from duration
    if (stream->nb_frames > 0) {
        frame_count_ = static_cast<int>(stream->nb_frames);
    } else if (fps_ > 0 && duration_ > 0) {
        frame_count_ = static_cast<int>(std::round(duration_ * fps_));
    } else {
        frame_count_ = 0;
    }

    Debug::Log("MetalVideoDecoder: " + std::to_string(frame_count_) + " frames" +
               " (nb_frames=" + std::to_string(stream->nb_frames) +
               ", duration=" + std::to_string(duration_) + "s" +
               ", arithmetic=" + std::to_string(static_cast<int>(std::round(duration_ * fps_))) + ")");

    // Check if intra-frame codec
    is_intra_codec_ = (codec_ctx_->codec_id == AV_CODEC_ID_PRORES ||
                       codec_ctx_->codec_id == AV_CODEC_ID_DNXHD ||
                       codec_ctx_->codec_id == AV_CODEC_ID_MJPEG ||
                       codec_ctx_->codec_id == AV_CODEC_ID_RAWVIDEO);

    return true;
}

//=============================================================================
// Hardware Acceleration Setup
//=============================================================================

bool MetalVideoDecoder::SetupHWAccel() {
    int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                      nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("MetalVideoDecoder: VideoToolbox not available: " + std::string(errbuf));
        hw_accel_type_ = HWAccelType::NONE;
        return false;
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    codec_ctx_->get_format = get_hw_format;
    hw_accel_type_ = HWAccelType::VIDEOTOOLBOX;

    Debug::Log("MetalVideoDecoder: VideoToolbox HW acceleration enabled");
    return true;
}

//=============================================================================
// FrameIndex Delegating Methods
//=============================================================================

bool MetalVideoDecoder::HasKeyframeIndex() const {
    return frame_index_ && (frame_index_->IsComplete() || frame_index_->IsAllIntra());
}

const std::vector<int>& MetalVideoDecoder::GetKeyframePositions() const {
    if (frame_index_) {
        cached_keyframe_positions_ = frame_index_->GetKeyframePositions();
    } else {
        cached_keyframe_positions_.clear();
    }
    return cached_keyframe_positions_;
}

//=============================================================================
// Frame Access
//=============================================================================

std::shared_ptr<PixelData> MetalVideoDecoder::GetFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto it = frame_map_.find(frame_number);
    if (it != frame_map_.end()) {
        auto& bf = frame_buffer_[it->second];
        if (bf.valid && bf.pixel_data) {
            return bf.pixel_data;
        }
    }
    return nullptr;
}

std::shared_ptr<PixelData> MetalVideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (frame_map_.empty()) return nullptr;

    int closest = frame_number;
    int min_dist = INT_MAX;

    for (const auto& [frame_num, idx] : frame_map_) {
        int dist = std::abs(frame_num - frame_number);
        if (dist < min_dist) {
            min_dist = dist;
            closest = frame_num;
        }
    }

    if (actual_frame) *actual_frame = closest;
    auto it = frame_map_.find(closest);
    if (it != frame_map_.end()) {
        auto& bf = frame_buffer_[it->second];
        if (bf.valid && bf.pixel_data) return bf.pixel_data;
    }
    return nullptr;
}

bool MetalVideoDecoder::HasFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return frame_map_.count(frame_number) > 0;
}

std::shared_ptr<PixelData> MetalVideoDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    int kf = GetNearestKeyframePosition(target_frame);
    if (actual_keyframe) *actual_keyframe = kf;
    return GetFrame(kf);
}

int MetalVideoDecoder::GetNearestKeyframePosition(int target_frame) const {
    if (frame_index_) {
        return frame_index_->NearestKeyframeBefore(target_frame);
    }
    return target_frame;
}

//=============================================================================
// GetFrameAsPoolTexture — Main thread GPU conversion
//=============================================================================

uint64_t MetalVideoDecoder::GetFrameAsPoolTexture(int frame_number) {
    if (!initialized_) return 0;

    int raw_request = frame_number;
    frame_number = std::clamp(frame_number, 0,
                              frame_count_ > 0 ? frame_count_ - 1 : 0);

    // Update decode target (matches D3D11/Vulkan: separate playhead from decode target)
    playhead_ = frame_number;
    decode_target_ = frame_number;
    decode_cv_.notify_one();

    // Pool texture lookup — ZERO GPU work on main thread.
    auto lookupPoolTexture = [this](int fn) -> uint64_t {
        auto it = frame_map_.find(fn);
        if (it == frame_map_.end()) return 0;
        auto& bf = frame_buffer_[it->second];
        if (!bf.valid || !bf.gpu_uploaded || bf.pool_texture_id == 0) return 0;
        MetalTexturePool::Instance().Touch(bf.pool_texture_id);
        return bf.pool_texture_id;
    };

    // Try immediately
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        uint64_t result = lookupPoolTexture(frame_number);
        if (result != 0) {
            last_displayed_pool_id_ = result;
            return result;
        }
        // Log buffer state on miss (throttled to avoid spam)
        static int miss_log_counter = 0;
        if (++miss_log_counter % 30 == 1) {
            std::string buf_frames;
            for (const auto& [fn, _] : frame_map_) {
                if (!buf_frames.empty()) buf_frames += ",";
                buf_frames += std::to_string(fn);
            }
            Debug::Log("PLAYBACK MISS: req=" + std::to_string(frame_number) +
                       " head=" + std::to_string(current_decode_frame_.load()) +
                       " buf=[" + buf_frames + "]");
        }
    }

    // Frame not ready — trigger seek if needed
    {
        int head = current_decode_frame_.load();
        bool need_seek = false;

        if (head < 0) {
            need_seek = true;  // Initial state
        } else if (is_intra_codec_ && shuttle_mode_) {
            // Intra codecs during scrub: always seek directly
            need_seek = true;
        } else if (is_intra_codec_) {
            // Intra codecs during playback: distance thresholds (sequential decode)
            bool need_backward = (frame_number < head - kFrameBufferSize);
            bool need_forward = (frame_number > head + kFrameBufferSize);
            bool frame_evicted = (frame_number < head) &&
                                 !HasFrame(frame_number) &&
                                 std::abs(frame_number - last_seek_target_.load()) > 2;
            need_seek = need_backward || need_forward || frame_evicted;
        } else {
            // Inter-frame codecs: use distance thresholds
            bool need_backward = (frame_number < head - kFrameBufferSize);
            bool need_forward = (frame_number > head + 60);
            // Also seek if frame was evicted by FIFO (backward stepping)
            bool frame_evicted = (frame_number < head) &&
                                 !HasFrame(frame_number) &&
                                 std::abs(frame_number - last_seek_target_.load()) > 2;
            need_seek = need_backward || need_forward || frame_evicted;

            // Don't re-seek to the same target (matches D3D11).
            // After a seek, the decoder decodes forward from the keyframe — let it
            // catch up. Without this, every frame re-triggers the seek, clearing the
            // buffer and resetting decode progress (infinite seek loop).
            if (need_seek && frame_number == last_seek_target_.load()) {
                need_seek = false;
            }
        }

        if (need_seek && !seek_pending_.load() && !decode_seeking_.load()) {
            last_seek_target_ = frame_number;
            seek_pending_ = true;
            eof_reached_ = false;
            decode_cv_.notify_one();
        }
    }

    // Wait for frame ready (matches Vulkan/D3D11 for inter-frame codecs).
    // Intra codecs use fast path when shuttling (ScrubDecoder handles scrub).
    {
        int timeout_ms;
        if (is_intra_codec_ && shuttle_mode_) {
            timeout_ms = 0;  // Intra scrub: ScrubDecoder handles it, don't block
        } else {
            // Standard timeout: 150ms for B-frame, 32ms for non-B-frame
            timeout_ms = (codec_ctx_ && codec_ctx_->has_b_frames > 0) ? 150 : 32;
        }

        {
            std::unique_lock<std::mutex> lock(frame_ready_mutex_);
            frame_ready_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                bool queue_ready = !delay_queue_filling_.load();
                bool frame_ready = HasFrame(frame_number);
                return (queue_ready && frame_ready) || shutdown_requested_.load();
            });
        }

        // Don't return frames while delay queue is still filling (matches Vulkan/D3D11).
        // Return last displayed frame instead to avoid black flash.
        if (delay_queue_filling_.load()) {
            if (last_displayed_pool_id_ != 0) {
                const auto* tex = MetalTexturePool::Instance().GetTexture(last_displayed_pool_id_);
                if (tex && tex->is_valid) {
                    MetalTexturePool::Instance().Touch(last_displayed_pool_id_);
                    return last_displayed_pool_id_;
                }
            }
            return 0;
        }

        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Try exact frame first
        uint64_t result = lookupPoolTexture(frame_number);
        if (result != 0) {
            last_displayed_pool_id_ = result;
            return result;
        }

        // Fall back to closest buffered frame with B-frame awareness (matches D3D11)
        if (!frame_map_.empty()) {
            int closest = frame_number;
            int min_dist = INT_MAX;
            for (const auto& [fn, _] : frame_map_) {
                int dist = std::abs(fn - frame_number);
                if (dist < min_dist) {
                    min_dist = dist;
                    closest = fn;
                }
            }
            if (min_dist <= kFrameBufferSize) {
                bool is_bframe_codec = codec_ctx_ && codec_ctx_->has_b_frames > 0;
                if (!is_bframe_codec || closest >= frame_number) {
                    result = lookupPoolTexture(closest);
                    if (result != 0) {
                        last_displayed_pool_id_ = result;
                        return result;
                    }
                }
            }
        }
    }

    // Diagnostic: log when we fail to deliver the requested frame
    {
        int head = current_decode_frame_.load();
        bool dq = delay_queue_filling_.load();
        bool sp = seek_pending_.load();
        bool eof = eof_reached_.load();
        std::string buffered;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            for (const auto& [fn, _] : frame_map_) {
                if (!buffered.empty()) buffered += ",";
                buffered += std::to_string(fn);
            }
        }
        bool ds = decode_seeking_.load();
        int lst = last_seek_target_.load();
        Debug::Log("GetFrameAsPoolTexture MISS: req=" + std::to_string(frame_number) +
                   " head=" + std::to_string(head) +
                   " dq=" + std::to_string(dq) +
                   " seek=" + std::to_string(sp) +
                   " seeking=" + std::to_string(ds) +
                   " eof=" + std::to_string(eof) +
                   " last_seek=" + std::to_string(lst) +
                   " buf=[" + buffered + "]");
    }

    // Return last displayed frame to avoid black flash during fast scrubbing.
    if (last_displayed_pool_id_ != 0) {
        const auto* tex = MetalTexturePool::Instance().GetTexture(last_displayed_pool_id_);
        if (tex && tex->is_valid) {
            MetalTexturePool::Instance().Touch(last_displayed_pool_id_);
            return last_displayed_pool_id_;
        }
        last_displayed_pool_id_ = 0;
    }

    return 0;
}

//=============================================================================
// Playhead Management
//=============================================================================

void MetalVideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    int prev_playhead = playhead_.exchange(frame_number);
    bool playhead_changed = (frame_number != prev_playhead);

    playhead_ = frame_number;
    decode_target_ = frame_number;

    if (!playhead_changed && !force_seek) return;

    // Force seek (play-from-scrub): clear scattered buffer for intra codecs.
    // After random clicking, the buffer has frames from wildly different positions.
    // Playback needs sequential frames — clear the junk so FIFO fills cleanly.
    // For inter-frame codecs, skip if frame already buffered (burst decode may
    // have just filled the buffer with this frame).
    if (force_seek) {
        if (is_intra_codec_) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            for (auto& bf : frame_buffer_) {
                if (bf.valid) bf.Reset();
            }
            frame_map_.clear();
            buffer_head_ = 0;
            buffer_count_ = 0;
        } else if (HasFrame(frame_number)) {
            decode_cv_.notify_one();
            return;
        }
    }

    // Check if seek is needed
    int head = current_decode_frame_.load();
    bool need_seek = false;

    if (head < 0) {
        // Initial state — always seek
        need_seek = true;
    } else if (is_intra_codec_) {
        // Intra codecs during scrub (shuttle): seek directly to any frame.
        // During playback: use distance thresholds (sequential decode is faster).
        if (shuttle_mode_) {
            need_seek = !HasFrame(frame_number) && (frame_number != head);
        } else {
            bool need_backward = (frame_number < head - kFrameBufferSize);
            bool need_forward = (frame_number > head + kFrameBufferSize);
            need_seek = need_backward || need_forward || force_seek;
        }
    } else {
        // Inter-frame codecs: match Vulkan/D3D11 thresholds exactly
        constexpr int kSeekThresholdFrames = 60;
        bool need_backward = (frame_number < head - kFrameBufferSize);
        bool need_forward = (frame_number > head + kSeekThresholdFrames);
        need_seek = need_backward || need_forward || force_seek;

        // Don't re-seek to the same target — let the decoder catch up (matches D3D11)
        if (need_seek && !force_seek && frame_number == last_seek_target_.load()) {
            need_seek = false;
        }
    }

    if (need_seek && !seek_pending_.load() && !decode_seeking_.load()) {
        last_seek_target_ = frame_number;
        seek_pending_ = true;
        eof_reached_ = false;
        // Clear stale fallback — show nothing while seeking rather than
        // flashing the old frame from a completely different position.
        last_displayed_pool_id_ = 0;
    }

    decode_cv_.notify_one();
}

//=============================================================================
// Demand-Driven Decode API
//=============================================================================

void MetalVideoDecoder::SetNeededFrames(const std::vector<int>& frames_by_priority) {
    {
        std::lock_guard<std::mutex> lock(needed_mutex_);
        needed_frames_ = frames_by_priority;
    }
    if (!frames_by_priority.empty()) {
        decode_target_ = frames_by_priority.front();
    }
    decode_cv_.notify_one();
}

void MetalVideoDecoder::SetPlaying(bool playing) {
    bool was_playing = playing_.exchange(playing);
    if (playing && !was_playing) {
        // Resuming playback — wake decode thread to start prefetching
        decode_cv_.notify_one();
    }
}

DecodeStatus MetalVideoDecoder::GetDecodeStatus() const {
    DecodeStatus status;
    std::lock_guard<std::mutex> lock1(buffer_mutex_);
    for (const auto& [frame_num, _] : frame_map_) {
        status.have.push_back(frame_num);
    }

    {
        std::lock_guard<std::mutex> lock2(needed_mutex_);
        for (int f : needed_frames_) {
            if (frame_map_.find(f) == frame_map_.end()) {
                status.missing.push_back(f);
            }
        }
    }
    return status;
}

void MetalVideoDecoder::EvictOutsideWindow(const std::set<int>& keep_frames) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (auto it = frame_map_.begin(); it != frame_map_.end(); ) {
        if (keep_frames.find(it->first) == keep_frames.end()) {
            frame_buffer_[it->second].Reset();
            it = frame_map_.erase(it);
        } else {
            ++it;
        }
    }
}

std::set<int> MetalVideoDecoder::GetBufferedFramesSet() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    std::set<int> result;
    for (const auto& [frame_num, _] : frame_map_) {
        result.insert(frame_num);
    }
    return result;
}

//=============================================================================
// Buffer Status
//=============================================================================

int MetalVideoDecoder::GetBufferedAhead() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    int ph = playhead_.load();
    int count = 0;
    for (const auto& [f, _] : frame_map_) {
        if (f >= ph) count++;
    }
    return count;
}

int MetalVideoDecoder::GetBufferedBehind() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    int ph = playhead_.load();
    int count = 0;
    for (const auto& [f, _] : frame_map_) {
        if (f < ph) count++;
    }
    return count;
}

int MetalVideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return static_cast<int>(frame_map_.size());
}

void MetalVideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (frame_map_.empty()) {
        start_frame = end_frame = 0;
        return;
    }
    start_frame = INT_MAX;
    end_frame = INT_MIN;
    for (const auto& [f, _] : frame_map_) {
        start_frame = std::min(start_frame, f);
        end_frame = std::max(end_frame, f);
    }
}

void MetalVideoDecoder::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (auto& bf : frame_buffer_) {
        bf.Reset();
    }
    frame_map_.clear();
    buffer_head_ = 0;
    buffer_count_ = 0;
}

//=============================================================================
// Loop Points
//=============================================================================

void MetalVideoDecoder::SetLoopPoints(int start_frame, int end_frame) {
    loop_start_ = start_frame;
    loop_end_ = end_frame;
}

void MetalVideoDecoder::ClearLoopPoints() {
    loop_start_ = -1;
    loop_end_ = -1;
}

//=============================================================================
// Decode Thread
//=============================================================================

void MetalVideoDecoder::DecodeThreadFunc() {
    Debug::Log("MetalVideoDecoder: Decode thread started");

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    int consecutive_failures = 0;

    // Adaptive timeout scaled with frame rate (matches D3D11/Vulkan)
    int timeout_ms = (fps_ > 0)
        ? std::max(15, std::min(30, static_cast<int>(1000.0 / fps_)))
        : 30;

    while (!shutdown_requested_.load()) {
        // Wait for work: shutdown, seek, or buffer needs frames.
        {
            std::unique_lock<std::mutex> lock(decode_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
                if (eof_reached_.load() && !seek_pending_.load()) return false;
                if (shutdown_requested_.load() || seek_pending_.load()) return true;
                return NeedsMoreFrames();
            });
        }

        if (shutdown_requested_.load()) break;

        // Handle seek — clear buffer, reset state (matches D3D11/Vulkan)
        if (seek_pending_.load()) {
            decode_seeking_ = true;
            eof_reached_ = false;
            consecutive_failures = 0;

            int target = last_seek_target_.load();
            Debug::Log("MetalVideoDecoder: SEEK HANDLER entered target=" + std::to_string(target) +
                       " intra=" + std::to_string(is_intra_codec_));
            int64_t timestamp;
            if (is_intra_codec_ && frame_index_) {
                // Intra codecs: use FrameIndex for exact keyframe PTS
                timestamp = frame_index_->KeyframePTSBefore(target);
            } else {
                // Inter-frame codecs: use proven arithmetic (matches backup/Vulkan/D3D11).
                // FrameIndex PTS may be wrong for B-frame codecs (DTS vs PTS ordering).
                double target_sec = (fps_ > 0) ? static_cast<double>(target) / fps_ : 0.0;
                AVStream* stream = format_ctx_->streams[video_stream_idx_];
                timestamp = static_cast<int64_t>(target_sec / av_q2d(stream->time_base));
            }

            if (is_intra_codec_) {
                // Intra codecs: preserve buffer when target is nearby (scrubbing),
                // but clear it when target is far outside the buffered range (loop seek).
                // Without clearing, NeedsMoreFrames() sees a full buffer of stale frames
                // that are all >= target and never wakes the decode thread.
                bool target_in_range = false;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    if (buffer_count_ > 0) {
                        // Check if target is within or near the buffered range
                        int buf_min = INT_MAX, buf_max = INT_MIN;
                        for (int i = 0; i < buffer_count_; i++) {
                            int idx = (buffer_head_ + i) % kFrameBufferSize;
                            if (frame_buffer_[idx].valid) {
                                buf_min = std::min(buf_min, frame_buffer_[idx].frame_number);
                                buf_max = std::max(buf_max, frame_buffer_[idx].frame_number);
                            }
                        }
                        // "Nearby" = within one buffer size of the range
                        target_in_range = (target >= buf_min - kFrameBufferSize &&
                                           target <= buf_max + kFrameBufferSize);
                    }
                }

                if (!target_in_range) {
                    // Far seek (e.g., loop from end→start): clear buffer
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    for (auto& bf : frame_buffer_) {
                        if (bf.valid) {
                            bf.Reset();
                        }
                    }
                    frame_map_.clear();
                    buffer_head_ = 0;
                    buffer_count_ = 0;
                    Debug::Log("MetalVideoDecoder: Intra seek far — cleared buffer for target=" + std::to_string(target));
                }

                avcodec_flush_buffers(codec_ctx_);
                av_seek_frame(format_ctx_, video_stream_idx_, timestamp, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec_ctx_);
            } else {
                // Inter-frame codecs: clear buffer completely (frames depend on prior context)
                // Don't delete pool textures — timeline cache may reference them as last_good_texture_.
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    for (auto& bf : frame_buffer_) {
                        if (bf.valid) {
                            bf.Reset();
                        }
                    }
                    frame_map_.clear();
                    buffer_head_ = 0;
                    buffer_count_ = 0;
                }

                avcodec_flush_buffers(codec_ctx_);
                int seek_ret = av_seek_frame(format_ctx_, video_stream_idx_, timestamp, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(codec_ctx_);

                Debug::Log("MetalVideoDecoder: SEEK target=" + std::to_string(target) +
                           " pts=" + std::to_string(timestamp) +
                           " ret=" + std::to_string(seek_ret) +
                           " stream_idx=" + std::to_string(video_stream_idx_));

                // Reset B-frame delay queue (matches Vulkan/D3D11: simple counter)
                frames_since_seek_ = 0;
                if (codec_ctx_ && codec_ctx_->has_b_frames > 0) {
                    delay_queue_filling_ = true;
                }
            }

            current_decode_frame_ = target;  // Prevent re-seek
            seek_pending_ = false;
            decode_seeking_ = false;
            {
                std::lock_guard<std::mutex> lock(frame_ready_mutex_);
            }
            frame_ready_cv_.notify_all();
            continue;
        }

        if (eof_reached_.load()) continue;

        // Check if buffer needs more frames (decode_target-driven, like D3D11/Vulkan)
        if (!NeedsMoreFrames()) continue;

        // Decode frames. For inter-frame codecs catching up after a seek,
        // burst-decode until the target is buffered. For normal playback
        // and intra codecs, decode one frame at a time.
        {
            int target = decode_target_.load();
            // Burst mode: only for seeking (not playing). During playback, every
            // frame needs full GPU treatment since it'll be displayed.
            bool is_playing = playing_.load(std::memory_order_acquire);
            bool burst_mode = !is_intra_codec_ && !HasFrame(target) && !is_playing;
            int max_frames = burst_mode ? 256 : 1;
            int decoded = 0;
            auto burst_start = std::chrono::steady_clock::now();

            if (burst_mode) {
                skip_gpu_until_frame_ = target - 8;
                Debug::Log("MetalVideoDecoder: BURST start target=" + std::to_string(target));
            }

            while (decoded < max_frames && NeedsMoreFrames() &&
                   !shutdown_requested_.load() && !seek_pending_.load()) {

                if (DecodeOneFrame(frame, packet)) {
                    consecutive_failures = 0;
                    decoded++;
                    if (burst_mode && HasFrame(target)) {
                        skip_gpu_until_frame_ = -1;  // Resume normal GPU pipeline
                        {
                            std::lock_guard<std::mutex> lock(frame_ready_mutex_);
                        }
                        frame_ready_cv_.notify_all();
                        break;
                    }
                } else {
                    consecutive_failures++;
                    if (consecutive_failures >= 10) {
                        int ls = loop_start_.load();
                        int le = loop_end_.load();
                        if (ls >= 0 && le > ls && fps_ > 0) {
                            int64_t ts;
                            if (is_intra_codec_ && frame_index_) {
                                ts = frame_index_->KeyframePTSBefore(ls);
                            } else {
                                double loop_sec = static_cast<double>(ls) / fps_;
                                AVStream* stream = format_ctx_->streams[video_stream_idx_];
                                ts = static_cast<int64_t>(loop_sec / av_q2d(stream->time_base));
                            }
                            av_seek_frame(format_ctx_, video_stream_idx_, ts, AVSEEK_FLAG_BACKWARD);
                            avcodec_flush_buffers(codec_ctx_);
                            consecutive_failures = 0;
                        } else {
                            eof_reached_ = true;
                            if (delay_queue_filling_.load()) {
                                delay_queue_filling_ = false;
                                {
                                    std::lock_guard<std::mutex> frl(frame_ready_mutex_);
                                }
                                frame_ready_cv_.notify_all();
                            }
                        }
                        break;
                    }
                }
            }

            // Log burst results
            if (burst_mode && decoded > 0) {
                auto burst_end = std::chrono::steady_clock::now();
                auto burst_ms = std::chrono::duration_cast<std::chrono::milliseconds>(burst_end - burst_start).count();
                Debug::Log("MetalVideoDecoder: BURST done, decoded=" + std::to_string(decoded) +
                           " frames in " + std::to_string(burst_ms) + "ms" +
                           " (" + std::to_string(burst_ms > 0 ? decoded * 1000 / burst_ms : 0) + " fps)" +
                           " has_target=" + std::to_string(HasFrame(target)));
            }

            // Notify after batch (or single frame).
            // Lock frame_ready_mutex_ around the notify to prevent missed wakeups:
            // without the lock, the notify can fire before the main thread enters
            // wait_for, causing it to sleep for the full timeout.
            if (decoded > 0) {
                {
                    std::lock_guard<std::mutex> lock(frame_ready_mutex_);
                }
                frame_ready_cv_.notify_one();
            }
        }
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    Debug::Log("MetalVideoDecoder: Decode thread stopped");
}

bool MetalVideoDecoder::NeedsMoreFrames() const {
    int target = decode_target_.load();

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // If buffer is full and target frame is IN the buffer, don't decode more.
    // Without this check, backward stepping causes the decoder to keep running
    // forward, and FIFO eviction removes the very frame we're trying to display.
    if (buffer_count_ >= kFrameBufferSize && frame_map_.count(target) > 0) {
        return false;
    }

    // Always prefetch buffer/2 ahead (matches Vulkan/D3D11).
    // No play/pause awareness — decoder runs same regardless of state.
    // Application controls via UpdatePlayhead frequency.
    int ahead = 0;
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number >= target) {
            ahead++;
        }
    }

    return ahead < kFrameBufferSize / 2;
}

bool MetalVideoDecoder::DecodeOneFrame(AVFrame* frame, AVPacket* packet) {
    // Receive-first pattern with attempt limit (matches D3D11/Vulkan)
    constexpr int kMaxReceiveAttempts = 100;

    for (int attempt = 0; attempt < kMaxReceiveAttempts; attempt++) {
        int ret = avcodec_receive_frame(codec_ctx_, frame);

        if (ret == 0) {
            // Got a frame — use best_effort_timestamp for correct B-frame order
            // (matches D3D11/Vulkan; raw pts can be wrong with B-frame reordering)
            int64_t timestamp = frame->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) {
                timestamp = frame->pts;
            }
            // Use arithmetic frame numbering: round(pts_seconds * fps).
            // This MUST match the playback controller's frame calculation
            // (timer_pos * fps + 0.5) to ensure the decoder produces the exact
            // frame numbers the display path requests. FrameIndex is used for
            // PTS lookups during seeking, not for frame numbering.
            int frame_num;
            if (timestamp != AV_NOPTS_VALUE) {
                AVStream* stream = format_ctx_->streams[video_stream_idx_];
                double pts_sec = timestamp * av_q2d(stream->time_base);
                frame_num = static_cast<int>(std::round(pts_sec * fps_));
            } else {
                frame_num = current_decode_frame_.load() + 1;
            }

            // Register in FrameIndex for seek accuracy (if index incomplete)
            if (frame_index_ && !frame_index_->IsComplete() && timestamp != AV_NOPTS_VALUE) {
                bool is_key = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
                frame_index_->RegisterDecodedFrame(timestamp, frame->pkt_dts, is_key);
            }

            AddCurrentFrameToBuffer(frame, frame_num);
            av_frame_unref(frame);
            return true;
        }

        if (ret == AVERROR_EOF) return false;
        if (ret != AVERROR(EAGAIN)) return false;

        // EAGAIN — decoder needs more packets
        ret = av_read_frame(format_ctx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Flush decoder to drain B-frame reorder queue (matches D3D11/Vulkan)
                avcodec_send_packet(codec_ctx_, nullptr);
                continue;
            }
            return false;
        }

        // Skip non-video packets
        if (packet->stream_index != video_stream_idx_) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet);
        av_packet_unref(packet);
        // EAGAIN from send_packet is OK — just try receive again
    }

    return false;  // Exceeded max attempts
}

//=============================================================================
// AddCurrentFrameToBuffer — Ring buffer insertion (decode thread)
//=============================================================================

void MetalVideoDecoder::AddCurrentFrameToBuffer(AVFrame* frame, int frame_num) {
    // Burst decode optimization: skip GPU pipeline for intermediate frames.
    // Just track the frame number and advance the codec state.
    int skip_until = skip_gpu_until_frame_.load();
    if (skip_until >= 0 && frame_num < skip_until) {
        current_decode_frame_ = frame_num;
        // Still track B-frame delay queue
        int count = ++frames_since_seek_;
        int delay_depth = kDelayQueueDepth;
        if (codec_ctx_ && codec_ctx_->has_b_frames > 0) {
            delay_depth = std::max(kDelayQueueDepth, codec_ctx_->has_b_frames + 2);
        }
        if (delay_queue_filling_.load() && count >= delay_depth) {
            delay_queue_filling_ = false;
        }
        return;
    }

    // 3-phase lock pattern (matches D3D11/Vulkan):
    // Phase 1: Reserve slot under lock (fast)
    // Phase 2: Do expensive work (YUV copy/interleave) OUTSIDE lock
    // Phase 3: Finalize under lock (fast)

    int slot = -1;
    bool was_full = false;
    BufferedFrame* bf = nullptr;

    // PHASE 1: Reserve slot
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Skip if already buffered
        if (frame_map_.count(frame_num) > 0) return;

        // FIFO ring buffer: insert at tail, evict oldest if full
        if (buffer_count_ < kFrameBufferSize) {
            slot = (buffer_head_ + buffer_count_) % kFrameBufferSize;
        } else {
            slot = buffer_head_;
            was_full = true;
        }

        bf = &frame_buffer_[slot];
        if (was_full && bf->valid) {
            frame_map_.erase(bf->frame_number);
            bf->Reset();
        }

        // Mark slot invalid during upload (prevents main thread from using half-written data)
        bf->valid = false;
    }
    // LOCK RELEASED — main thread can access other frames while we work

    // PHASE 2: ALL GPU work on decode thread (main thread does ZERO GPU work)
    // Metal API is thread-safe: newTextureWithDescriptor, replaceRegion,
    // command buffer creation/commit, waitUntilCompleted can all run from any thread.
    //
    // Early-out: skip GPU work if shutdown is in progress (avoids blocking thread join)
    if (shutdown_requested_.load()) return;

    bool upload_ok = false;
    uint64_t pool_id = 0;

    if (gpu_decode_available_ && frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        // HW path: CVPixelBuffer → extract Y+UV → async compute shader → RGBA16F → pool
        CVPixelBufferRef pb = (CVPixelBufferRef)frame->data[3];
        if (pb && hw_extractor_) {
            CVPixelBufferRetain(pb);

            // Extract MTLTextures from CVPixelBuffer (zero-copy via IOSurface)
            MetalTextureFrame tex_frame = hw_extractor_->ExtractFromPixelBuffer((void*)pb);
            if (tex_frame.valid) {
                int frame_w = tex_frame.width;
                int frame_h = tex_frame.height;
                void* rgba_texture = nullptr;

                if (tex_frame.is_interleaved) {
                    // Interleaved 4444+alpha path (y416/y408): single texture
                    struct HWCleanupInterleaved {
                        void* tex;
                        void* pixel_buffer;
                    };
                    auto* cleanup = new HWCleanupInterleaved{
                        tex_frame.y_texture, tex_frame.pixel_buffer
                    };
                    tex_frame.y_texture = nullptr;
                    tex_frame.pixel_buffer = nullptr;

                    rgba_texture = yuv_renderer_->RenderInterleavedToRGBAAsync(
                        cleanup->tex, frame_w, frame_h,
                        tex_frame.bit_depth, GetEffectiveFullRange(),
                        is_bt2020_, is_hdr_,
                        [](void* ctx) {
                            auto* c = static_cast<HWCleanupInterleaved*>(ctx);
                            [(id<MTLTexture>)c->tex release];
                            CVPixelBufferRelease((CVPixelBufferRef)c->pixel_buffer);
                            delete c;
                        },
                        cleanup);
                } else {
                    // Biplanar Y+UV path: two textures
                    struct HWCleanup {
                        void* y_tex;
                        void* uv_tex;
                        void* pixel_buffer;
                    };
                    auto* cleanup = new HWCleanup{
                        tex_frame.y_texture, tex_frame.uv_texture, tex_frame.pixel_buffer
                    };
                    tex_frame.y_texture = nullptr;
                    tex_frame.uv_texture = nullptr;
                    tex_frame.pixel_buffer = nullptr;

                    rgba_texture = yuv_renderer_->RenderToRGBAAsync(
                        cleanup->y_tex, cleanup->uv_tex,
                        frame_w, frame_h,
                        tex_frame.bit_depth, GetEffectiveFullRange(),
                        is_bt2020_, is_hdr_,
                        static_cast<int>(tex_frame.subsampling),
                        [](void* ctx) {
                            auto* c = static_cast<HWCleanup*>(ctx);
                            [(id<MTLTexture>)c->y_tex release];
                            [(id<MTLTexture>)c->uv_tex release];
                            CVPixelBufferRelease((CVPixelBufferRef)c->pixel_buffer);
                            delete c;
                        },
                        cleanup);
                }

                // Register IMMEDIATELY — safe because Metal's same-queue ordering
                // guarantees the compute finishes before ImGui's render pass reads it.
                if (rgba_texture) {
                    pool_id = MetalTexturePool::Instance().RegisterExistingTexture(
                        rgba_texture, frame_w, frame_h, 1 /* RGBA16Float */);
                    if (pool_id != 0) {
                        bf->pool_texture_id = pool_id;
                        bf->gpu_uploaded = true;
                        upload_ok = true;
                    }
                }
            } else {
                hw_extractor_->ReleaseFrame(tex_frame);
            }

            CVPixelBufferRelease(pb);
        }

        // HW fallback: if GPU pipeline failed, use sws_scale → PixelData
        if (!upload_ok) {
            auto pixel_data = ConvertFrame(frame);
            if (pixel_data) {
                int mtl_format = (pixel_data->pixel_format == PixelFormat::RGBA16F) ? 1 : 0;
                pool_id = MetalTexturePool::Instance().CreateTextureFromPixels(
                    pixel_data->width, pixel_data->height, mtl_format,
                    pixel_data->pixels.data(), pixel_data->pixels.size());
                if (pool_id != 0) {
                    bf->pool_texture_id = pool_id;
                    bf->gpu_uploaded = true;
                    upload_ok = true;
                } else {
                    // Last resort: store PixelData for main thread upload
                    bf->pixel_data = pixel_data;
                    upload_ok = true;
                }
            }
        }
    } else if (gpu_decode_available_ && frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
        // SW path: extract YUV planes → create Metal textures → compute shader → RGBA16F → pool
        AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);
        int bit_depth = desc ? desc->comp[0].depth : 8;
        int bytes_per_sample = (bit_depth > 8) ? 2 : 1;

        int chroma_w = desc ? (1 << desc->log2_chroma_w) : 2;
        int chroma_h = desc ? (1 << desc->log2_chroma_h) : 2;

        int w = frame->width;
        int h = frame->height;
        int uv_w = w / chroma_w;
        int uv_h = h / chroma_h;

        bool is_planar_yuv = (desc && desc->nb_components >= 3 &&
                              !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
                              frame->data[0] && frame->data[1] && frame->data[2]);

        if (is_planar_yuv) {
            int subsampling = 0;
            if (chroma_w == 1 && chroma_h == 1) subsampling = 2;
            else if (chroma_w == 2 && chroma_h == 1) subsampling = 1;

            auto& mgr = MetalDeviceManager::Instance();
            id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
            bool is_16bit = (bit_depth > 8);

            // Create Y texture (R8Unorm or R16Unorm) and upload plane data
            MTLPixelFormat y_fmt = is_16bit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
            MTLTextureDescriptor* y_desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:y_fmt width:w height:h mipmapped:NO];
            y_desc.usage = MTLTextureUsageShaderRead;
            y_desc.storageMode = MTLStorageModeManaged;
            id<MTLTexture> y_tex = [device newTextureWithDescriptor:y_desc];

            int y_stride = frame->linesize[0];
            int y_row_bytes = w * bytes_per_sample;
            // Upload row-by-row if stride != row_bytes, otherwise bulk upload
            if (y_stride == y_row_bytes) {
                [y_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                         mipmapLevel:0
                           withBytes:frame->data[0]
                         bytesPerRow:y_row_bytes];
            } else {
                for (int row = 0; row < h; row++) {
                    [y_tex replaceRegion:MTLRegionMake2D(0, row, w, 1)
                             mipmapLevel:0
                               withBytes:frame->data[0] + row * y_stride
                             bytesPerRow:y_row_bytes];
                }
            }

            // Create UV texture (RG8Unorm or RG16Unorm) — interleave U+V planes
            MTLPixelFormat uv_fmt = is_16bit ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
            MTLTextureDescriptor* uv_desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:uv_fmt width:uv_w height:uv_h mipmapped:NO];
            uv_desc.usage = MTLTextureUsageShaderRead;
            uv_desc.storageMode = MTLStorageModeManaged;
            id<MTLTexture> uv_tex = [device newTextureWithDescriptor:uv_desc];

            int uv_row_bytes = uv_w * 2 * bytes_per_sample;
            int u_stride = frame->linesize[1];
            int v_stride = frame->linesize[2];

            // Interleave U+V into temporary buffer then upload
            std::vector<uint8_t> uv_interleaved(uv_row_bytes * uv_h);
#if defined(__aarch64__)
            // NEON-accelerated interleave (~8x faster than scalar)
            if (bytes_per_sample == 1) {
                InterleaveUV_NEON_8bit(
                    frame->data[1], u_stride, frame->data[2], v_stride,
                    uv_interleaved.data(), uv_row_bytes, uv_w, uv_h);
            } else {
                InterleaveUV_NEON_16bit(
                    frame->data[1], u_stride, frame->data[2], v_stride,
                    uv_interleaved.data(), uv_row_bytes, uv_w, uv_h);
            }
#else
            if (bytes_per_sample == 1) {
                for (int row = 0; row < uv_h; row++) {
                    const uint8_t* u_row = frame->data[1] + row * u_stride;
                    const uint8_t* v_row = frame->data[2] + row * v_stride;
                    uint8_t* uv_dst = uv_interleaved.data() + row * uv_row_bytes;
                    for (int col = 0; col < uv_w; col++) {
                        uv_dst[col * 2]     = u_row[col];
                        uv_dst[col * 2 + 1] = v_row[col];
                    }
                }
            } else {
                for (int row = 0; row < uv_h; row++) {
                    const uint16_t* u_row = reinterpret_cast<const uint16_t*>(frame->data[1] + row * u_stride);
                    const uint16_t* v_row = reinterpret_cast<const uint16_t*>(frame->data[2] + row * v_stride);
                    uint16_t* uv_dst = reinterpret_cast<uint16_t*>(uv_interleaved.data() + row * uv_row_bytes);
                    for (int col = 0; col < uv_w; col++) {
                        uv_dst[col * 2]     = u_row[col];
                        uv_dst[col * 2 + 1] = v_row[col];
                    }
                }
            }
#endif

            [uv_tex replaceRegion:MTLRegionMake2D(0, 0, uv_w, uv_h)
                      mipmapLevel:0
                        withBytes:uv_interleaved.data()
                      bytesPerRow:uv_row_bytes];

            // Async cleanup: release staging Y+UV textures when GPU finishes.
            struct SWCleanup {
                void* y_tex;
                void* uv_tex;
            };
            auto* sw_cleanup = new SWCleanup{(void*)y_tex, (void*)uv_tex};

            // GPU YUV→RGBA16F via Metal compute shader (async — no GPU wait on decode thread)
            void* rgba_texture = yuv_renderer_->RenderToRGBAAsync(
                sw_cleanup->y_tex, sw_cleanup->uv_tex,
                w, h, bit_depth, GetEffectiveFullRange(),
                is_bt2020_, is_hdr_, subsampling,
                [](void* ctx) {
                    auto* c = static_cast<SWCleanup*>(ctx);
                    [(id<MTLTexture>)c->y_tex release];
                    [(id<MTLTexture>)c->uv_tex release];
                    delete c;
                },
                sw_cleanup);
            // DON'T release y_tex/uv_tex here — async cleanup handles it

            if (rgba_texture) {
                pool_id = MetalTexturePool::Instance().RegisterExistingTexture(
                    rgba_texture, w, h, 1 /* RGBA16Float */);
                if (pool_id != 0) {
                    bf->pool_texture_id = pool_id;
                    bf->gpu_uploaded = true;
                    upload_ok = true;
                }
            }
        }

        // SW fallback: sws_scale → PixelData → pool texture
        if (!upload_ok) {
            auto pixel_data = ConvertFrame(frame);
            if (pixel_data) {
                int mtl_format = (pixel_data->pixel_format == PixelFormat::RGBA16F) ? 1 : 0;
                pool_id = MetalTexturePool::Instance().CreateTextureFromPixels(
                    pixel_data->width, pixel_data->height, mtl_format,
                    pixel_data->pixels.data(), pixel_data->pixels.size());
                if (pool_id != 0) {
                    bf->pool_texture_id = pool_id;
                    bf->gpu_uploaded = true;
                    upload_ok = true;
                } else {
                    bf->pixel_data = pixel_data;
                    upload_ok = true;
                }
            }
        }
    } else {
        // No GPU pipeline: sws_scale → PixelData → pool texture
        auto pixel_data = ConvertFrame(frame);
        if (pixel_data) {
            int mtl_format = (pixel_data->pixel_format == PixelFormat::RGBA16F) ? 1 : 0;
            pool_id = MetalTexturePool::Instance().CreateTextureFromPixels(
                pixel_data->width, pixel_data->height, mtl_format,
                pixel_data->pixels.data(), pixel_data->pixels.size());
            if (pool_id != 0) {
                bf->pool_texture_id = pool_id;
                bf->gpu_uploaded = true;
                upload_ok = true;
            } else {
                bf->pixel_data = pixel_data;
                upload_ok = true;
            }
        }
    }

    // PHASE 3: Finalize under lock (fast)
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        if (upload_ok) {
            bf->frame_number = frame_num;
            bf->valid = true;
            frame_map_[frame_num] = slot;

            if (!was_full) {
                buffer_count_++;
            } else {
                buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
            }

            current_decode_frame_ = frame_num;

            // B-frame delay queue tracking (matches Vulkan/D3D11: simple counter)
            int count = ++frames_since_seek_;
            int delay_depth = kDelayQueueDepth;
            if (codec_ctx_ && codec_ctx_->has_b_frames > 0) {
                delay_depth = std::max(kDelayQueueDepth, codec_ctx_->has_b_frames + 2);
            }
            if (delay_queue_filling_.load() && count >= delay_depth) {
                delay_queue_filling_ = false;
                {
                    std::lock_guard<std::mutex> frl(frame_ready_mutex_);
                }
                frame_ready_cv_.notify_all();
            }
        } else if (was_full) {
            // Upload failed — slot stays invalid, don't advance head
            // (buffer_count_ stays the same, slot is effectively empty)
        }
    }
}

//=============================================================================
// Frame Conversion (SW fallback path)
//
// HW path (VideoToolbox): CVPixelBuffer → CPU transfer → sws_scale → RGBA
// SW path: AVFrame → sws_scale → RGBA
//=============================================================================

std::shared_ptr<PixelData> MetalVideoDecoder::ConvertFrame(AVFrame* frame) {
    int w = frame->width;
    int h = frame->height;

    AVPixelFormat src_format;
    uint8_t* src_data[4] = {};
    int src_linesize[4] = {};
    AVFrame* tmp_frame = nullptr;

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        // VideoToolbox HW decode: transfer CVPixelBuffer to CPU memory
        tmp_frame = av_frame_alloc();

        int ret = av_hwframe_transfer_data(tmp_frame, frame, 0);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("MetalVideoDecoder: HW frame transfer failed: " + std::string(errbuf));
            av_frame_free(&tmp_frame);
            return nullptr;
        }

        src_format = static_cast<AVPixelFormat>(tmp_frame->format);
        for (int i = 0; i < 4; i++) {
            src_data[i] = tmp_frame->data[i];
            src_linesize[i] = tmp_frame->linesize[i];
        }
    } else {
        // Software decode: use frame data directly
        src_format = static_cast<AVPixelFormat>(frame->format);
        for (int i = 0; i < 4; i++) {
            src_data[i] = frame->data[i];
            src_linesize[i] = frame->linesize[i];
        }
    }

    // Determine output format based on bit depth
    AVPixelFormat dst_format = AV_PIX_FMT_RGBA;
    PixelFormat pixel_fmt = PixelFormat::RGBA8;
    int bpp = 4;

    // Check if source is >8-bit (P010, YUV420P10, ProRes 10-bit, etc.)
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src_format);
    if (desc && desc->comp[0].depth > 8) {
        dst_format = AV_PIX_FMT_RGBA64LE;  // 16-bit per channel
        pixel_fmt = PixelFormat::RGBA16;
        bpp = 8;
    }

    // Cache the SwsContext across frames (matches D3D11 pattern).
    // sws_getCachedContext reuses the existing context if parameters match,
    // only recreating if dimensions or formats change.
    auto* cached = static_cast<struct SwsContext*>(sws_ctx_);
    cached = sws_getCachedContext(
        cached,
        w, h, src_format,
        w, h, dst_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    sws_ctx_ = cached;

    if (!cached) {
        if (tmp_frame) av_frame_free(&tmp_frame);
        Debug::Log("MetalVideoDecoder: sws_getCachedContext failed for format " +
                   std::string(av_get_pix_fmt_name(src_format)));
        return nullptr;
    }

    auto pixel_data = std::make_shared<PixelData>();
    pixel_data->width = w;
    pixel_data->height = h;
    pixel_data->SetFormat(pixel_fmt);
    pixel_data->pixels.resize(w * h * bpp);

    uint8_t* dst_data[1] = { pixel_data->pixels.data() };
    int dst_linesize[1] = { w * bpp };

    sws_scale(cached, src_data, src_linesize, 0, h, dst_data, dst_linesize);

    if (tmp_frame) av_frame_free(&tmp_frame);

    return pixel_data;
}

} // namespace qcview

#endif // __APPLE__
