#include "unified_dual_view.h"

#ifdef _WIN32

#include "d3d11_video_decoder.h"
#include "cpu_frame_pool.h"
#include "../gpu/frame_readback.h"
#include "../utils/debug_utils.h"

#include <chrono>

namespace ump {

UnifiedDualView::UnifiedDualView() = default;

UnifiedDualView::~UnifiedDualView() {
    Shutdown();
}

//=============================================================================
// Initialization
//=============================================================================

bool UnifiedDualView::Initialize(ID3D11Device* device) {
    if (!device) {
        Debug::Log("UnifiedDualView: null device");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    device_ = device;

    // Create frame readback
    readback_ = std::make_unique<FrameReadback>();
    if (!readback_->Initialize(device)) {
        Debug::Log("UnifiedDualView: failed to initialize readback");
        return false;
    }

    // Create CPU frame pool
    frame_pool_ = std::make_unique<CPUFramePool>();
    frame_pool_->SetMaxFramesPerStream(4);  // Keep last 4 frames per view
    frame_pool_->SetMaxMemoryBytes(256 * 1024 * 1024);  // 256MB limit

    initialized_ = true;
    Debug::Log("UnifiedDualView: initialized");
    return true;
}

void UnifiedDualView::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    CloseSources();

    // Clean up GL textures
    if (gl_texture_a_) {
        glDeleteTextures(1, &gl_texture_a_);
        gl_texture_a_ = 0;
    }
    if (gl_texture_b_) {
        glDeleteTextures(1, &gl_texture_b_);
        gl_texture_b_ = 0;
    }

    frame_pool_.reset();
    readback_.reset();
    device_.Reset();

    initialized_ = false;
}

//=============================================================================
// Source Management
//=============================================================================

bool UnifiedDualView::SetSources(const std::string& path_a, const std::string& path_b) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return false;

    CloseSources();

    // Store source paths
    source_a_.path = path_a;
    source_a_.state = SourceState::LOADING;
    source_b_.path = path_b;
    source_b_.state = SourceState::LOADING;

    // Create decoder
    decoder_ = std::make_unique<D3D11VideoDecoder>();
    decoder_->SetVideoPath(path_a);

    if (!decoder_->Initialize()) {
        Debug::Log("UnifiedDualView: failed to initialize decoder for source A");
        source_a_.state = SourceState::FAILED;
        return false;
    }

    // Get source A info
    source_a_.width = decoder_->GetWidth();
    source_a_.height = decoder_->GetHeight();
    source_a_.fps = decoder_->GetFPS();
    source_a_.frame_count = decoder_->GetFrameCount();
    source_a_.state = SourceState::READY;
    current_decoder_source_ = 0;

    // Probe source B (we'll switch to it to get metadata, then switch back)
    // For now, assume it has similar properties - we'll get real dims on first decode
    source_b_.width = source_a_.width;
    source_b_.height = source_a_.height;
    source_b_.fps = source_a_.fps;
    source_b_.frame_count = source_a_.frame_count;
    source_b_.state = SourceState::READY;

    sources_loaded_ = true;
    Debug::Log("UnifiedDualView: sources loaded");
    Debug::Log("  Source A: " + path_a + " (" +
               std::to_string(source_a_.width) + "x" + std::to_string(source_a_.height) + ")");
    Debug::Log("  Source B: " + path_b);

    return true;
}

void UnifiedDualView::CloseSources() {
    // Note: caller should hold mutex_

    if (decoder_) {
        decoder_->Shutdown();
        decoder_.reset();
    }

    if (frame_pool_) {
        frame_pool_->ClearAll();
    }

    source_a_ = SourceInfo{};
    source_b_ = SourceInfo{};
    current_decoder_source_ = -1;
    sources_loaded_ = false;
    current_frame_a_ = -1;
    current_frame_b_ = -1;
}

//=============================================================================
// Frame Access
//=============================================================================

bool UnifiedDualView::UpdateFrame(int64_t frame_a, int64_t frame_b) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_ || !sources_loaded_) return false;

    auto start_time = std::chrono::high_resolution_clock::now();

    bool frame_a_ready = false;
    bool frame_b_ready = false;

    // Determine decode order based on primary view
    int first_source = primary_view_;
    int second_source = 1 - primary_view_;
    int64_t first_frame = (first_source == 0) ? frame_a : frame_b;
    int64_t second_frame = (first_source == 0) ? frame_b : frame_a;

    // Decode primary view first (uses zero-copy GL interop)
    if (DecodeFrame(first_source, first_frame)) {
        if (first_source == 0) {
            // Source A - get GL texture directly from decoder
            GLuint tex = decoder_->GetFrameAsGLTexture(static_cast<int>(first_frame));
            if (tex != 0) {
                gl_texture_a_ = tex;
                gl_texture_a_width_ = source_a_.width;
                gl_texture_a_height_ = source_a_.height;
                current_frame_a_ = frame_a;
                frame_a_ready = true;
                frames_decoded_a_++;
            }
        } else {
            // Source B is primary
            GLuint tex = decoder_->GetFrameAsGLTexture(static_cast<int>(first_frame));
            if (tex != 0) {
                gl_texture_b_ = tex;
                gl_texture_b_width_ = source_b_.width;
                gl_texture_b_height_ = source_b_.height;
                current_frame_b_ = frame_b;
                frame_b_ready = true;
                frames_decoded_b_++;
            }
        }
    }

    // Decode secondary view (uses readback + GL upload)
    if (DecodeFrame(second_source, second_frame)) {
        // Read back to CPU
        if (ReadbackCurrentFrame(second_source, static_cast<int>(second_frame))) {
            // Get frame from pool and upload to GL
            AVFrame* cpu_frame = frame_pool_->GetFrame(second_source, static_cast<int>(second_frame));
            if (cpu_frame) {
                if (second_source == 0) {
                    if (UploadFrameToGL(cpu_frame, gl_texture_a_, source_a_.width, source_a_.height)) {
                        current_frame_a_ = frame_a;
                        frame_a_ready = true;
                        frames_decoded_a_++;
                    }
                } else {
                    if (UploadFrameToGL(cpu_frame, gl_texture_b_, source_b_.width, source_b_.height)) {
                        current_frame_b_ = frame_b;
                        frame_b_ready = true;
                        frames_decoded_b_++;
                    }
                }
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    last_decode_time_ms_ = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return frame_a_ready && frame_b_ready;
}

void UnifiedDualView::GetDimensionsA(int& width, int& height) const {
    width = source_a_.width;
    height = source_a_.height;
}

void UnifiedDualView::GetDimensionsB(int& width, int& height) const {
    width = source_b_.width;
    height = source_b_.height;
}

//=============================================================================
// Seek / Scrub
//=============================================================================

void UnifiedDualView::OnSeek() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frame_pool_) {
        frame_pool_->ClearAll();
    }

    current_frame_a_ = -1;
    current_frame_b_ = -1;
}

void UnifiedDualView::SetPrimaryView(int view) {
    primary_view_ = (view == 0) ? 0 : 1;
}

//=============================================================================
// Playback Control
//=============================================================================

void UnifiedDualView::SetPlaying(bool playing) {
    playing_ = playing;
}

//=============================================================================
// Statistics
//=============================================================================

UnifiedDualView::Stats UnifiedDualView::GetStats() const {
    Stats stats;
    stats.frames_decoded_a = frames_decoded_a_.load();
    stats.frames_decoded_b = frames_decoded_b_.load();
    stats.readback_count = readback_count_.load();
    stats.last_decode_time_ms = last_decode_time_ms_.load();
    return stats;
}

//=============================================================================
// Internal Methods
//=============================================================================

bool UnifiedDualView::DecodeFrame(int source_id, int64_t frame_number) {
    if (!decoder_) return false;

    // Switch source if needed
    if (current_decoder_source_ != source_id) {
        if (!SwitchSource(source_id)) {
            return false;
        }
    }

    // Request the frame
    decoder_->UpdatePlayhead(static_cast<int>(frame_number));

    // Check if frame is available
    return decoder_->HasFrame(static_cast<int>(frame_number));
}

bool UnifiedDualView::SwitchSource(int source_id) {
    if (!decoder_) return false;

    const std::string& path = (source_id == 0) ? source_a_.path : source_b_.path;
    SourceInfo& info = (source_id == 0) ? source_a_ : source_b_;

    Debug::Log("UnifiedDualView: switching to source " + std::to_string(source_id));

    // Shutdown current decoder
    decoder_->Shutdown();

    // Reinitialize with new source
    decoder_->SetVideoPath(path);
    if (!decoder_->Initialize()) {
        Debug::Log("UnifiedDualView: failed to switch to source " + std::to_string(source_id));
        return false;
    }

    // Update source info (in case dimensions differ)
    info.width = decoder_->GetWidth();
    info.height = decoder_->GetHeight();
    info.fps = decoder_->GetFPS();
    info.frame_count = decoder_->GetFrameCount();

    current_decoder_source_ = source_id;
    return true;
}

bool UnifiedDualView::ReadbackCurrentFrame(int source_id, int frame_number) {
    if (!decoder_ || !readback_ || !frame_pool_) return false;

    // Check if we already have this frame in the pool
    if (frame_pool_->HasFrame(source_id, frame_number)) {
        return true;
    }

    // Get the intermediate texture from decoder
    ID3D11Texture2D* texture = decoder_->GetIntermediateTexture();
    if (!texture) {
        Debug::Log("UnifiedDualView: no intermediate texture for readback");
        return false;
    }

    // Read back to new AVFrame
    AVFrame* frame = readback_->ReadbackToNewFrame(texture);
    if (!frame) {
        Debug::Log("UnifiedDualView: readback failed");
        return false;
    }

    // Store in pool (pool takes ownership)
    frame_pool_->StoreFrame(source_id, frame_number, frame);
    readback_count_++;

    return true;
}

bool UnifiedDualView::UploadFrameToGL(AVFrame* frame, GLuint& texture, int width, int height) {
    if (!frame) return false;

    // Ensure texture exists with correct dimensions
    if (texture == 0 || width != frame->width || height != frame->height) {
        if (texture != 0) {
            glDeleteTextures(1, &texture);
        }
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Allocate storage based on frame format
        AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
        if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_YUV420P) {
            // For YUV, we'd need a shader. For now, assume RGBA conversion happened.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height,
                        0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        } else if (fmt == AV_PIX_FMT_RGBA || fmt == AV_PIX_FMT_BGRA) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height,
                        0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        } else if (fmt == AV_PIX_FMT_RGBAF16LE) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, frame->width, frame->height,
                        0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        }
    }

    glBindTexture(GL_TEXTURE_2D, texture);

    // Upload based on format
    AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    if (fmt == AV_PIX_FMT_RGBA) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
                       GL_RGBA, GL_UNSIGNED_BYTE, frame->data[0]);
    } else if (fmt == AV_PIX_FMT_BGRA) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
                       GL_BGRA, GL_UNSIGNED_BYTE, frame->data[0]);
    } else if (fmt == AV_PIX_FMT_RGBAF16LE) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height,
                       GL_RGBA, GL_HALF_FLOAT, frame->data[0]);
    } else {
        // YUV formats need shader conversion - for now, skip
        Debug::Log("UnifiedDualView: unsupported frame format for GL upload: " + std::to_string(fmt));
        return false;
    }

    return true;
}

bool UnifiedDualView::EnsureGLTextures() {
    if (gl_texture_a_ == 0) {
        glGenTextures(1, &gl_texture_a_);
        glBindTexture(GL_TEXTURE_2D, gl_texture_a_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (gl_texture_b_ == 0) {
        glGenTextures(1, &gl_texture_b_);
        glBindTexture(GL_TEXTURE_2D, gl_texture_b_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    return true;
}

} // namespace ump

#endif // _WIN32
