#include "dual_view_pipeline.h"

#ifdef _WIN32

#include "../gpu/d3d11_dual_compositor.h"
#include "../gpu/d3d11_video_interop.h"
#include "d3d11_video_decoder.h"
#include "../utils/debug_utils.h"

namespace ump {

DualViewPipeline::DualViewPipeline() = default;

DualViewPipeline::~DualViewPipeline() {
    Shutdown();
}

bool DualViewPipeline::Initialize(ID3D11Device* device,
                                   int left_width, int left_height,
                                   int right_width, int right_height) {
    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("DualViewPipeline: Device is null");
        return false;
    }

    device_ = device;

    // Create compositor
    compositor_ = std::make_unique<D3D11DualCompositor>();
    if (!compositor_->Initialize(device)) {
        Debug::Log("DualViewPipeline: Failed to initialize compositor");
        Shutdown();
        return false;
    }

    // Create single interop (this is the key - only ONE interop instead of two)
    interop_ = std::make_unique<D3D11VideoInterop>();
    if (!interop_->Initialize(device)) {
        Debug::Log("DualViewPipeline: Failed to initialize interop");
        Shutdown();
        return false;
    }

    // Compute initial layout
    if (!UpdateLayout(left_width, left_height, right_width, right_height)) {
        Debug::Log("DualViewPipeline: Failed to compute initial layout");
        Shutdown();
        return false;
    }

    // Create interop texture matching composite dimensions
    if (!interop_->CreateSharedTexture(layout_.composite_width, layout_.composite_height,
                                        DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        Debug::Log("DualViewPipeline: Failed to create interop texture");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("DualViewPipeline: Initialized with composite " +
               std::to_string(layout_.composite_width) + "x" +
               std::to_string(layout_.composite_height));
    return true;
}

void DualViewPipeline::Shutdown() {
    left_decoder_ = nullptr;
    right_decoder_ = nullptr;

    if (interop_) {
        interop_->Shutdown();
        interop_.reset();
    }

    if (compositor_) {
        compositor_->Shutdown();
        compositor_.reset();
    }

    device_.Reset();
    layout_ = DualViewLayout();
    layout_valid_ = false;
    initialized_ = false;
}

void DualViewPipeline::SetDecoders(D3D11VideoDecoder* left, D3D11VideoDecoder* right) {
    left_decoder_ = left;
    right_decoder_ = right;

    // Invalidate layout to force recalculation on next update
    if (left || right) {
        layout_valid_ = false;
    }
}

bool DualViewPipeline::UpdateLayout(int left_w, int left_h, int right_w, int right_h) {
    // Use default dimensions if sources aren't ready
    if (left_w <= 0) left_w = 1920;
    if (left_h <= 0) left_h = 1080;
    if (right_w <= 0) right_w = 1920;
    if (right_h <= 0) right_h = 1080;

    layout_ = ComputeDualViewLayout(left_w, left_h, right_w, right_h);
    layout_valid_ = layout_.IsValid();

    return layout_valid_;
}

bool DualViewPipeline::EnsureCompositorAndInterop() {
    if (!compositor_ || !interop_) {
        return false;
    }

    // Ensure compositor has target matching layout
    if (!compositor_->GetCompositeTexture() ||
        compositor_->GetWidth() != layout_.composite_width ||
        compositor_->GetHeight() != layout_.composite_height) {

        if (!compositor_->CreateCompositeTarget(layout_, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            Debug::Log("DualViewPipeline: Failed to create compositor target");
            return false;
        }
    }

    // Ensure interop texture matches composite dimensions
    if (interop_->GetWidth() != layout_.composite_width ||
        interop_->GetHeight() != layout_.composite_height) {

        interop_->DestroySharedTexture();
        if (!interop_->CreateSharedTexture(layout_.composite_width, layout_.composite_height,
                                            DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            Debug::Log("DualViewPipeline: Failed to resize interop texture");
            return false;
        }
    }

    return true;
}

GLuint DualViewPipeline::UpdateFrame(int64_t frame_left, int64_t frame_right) {
    if (!initialized_) {
        Debug::Log("DualViewPipeline::UpdateFrame: not initialized");
        return 0;
    }

    // Get dimensions from decoders and update layout if needed
    int left_w = left_decoder_ ? left_decoder_->GetWidth() : 0;
    int left_h = left_decoder_ ? left_decoder_->GetHeight() : 0;
    int right_w = right_decoder_ ? right_decoder_->GetWidth() : 0;
    int right_h = right_decoder_ ? right_decoder_->GetHeight() : 0;

    bool layout_changed = false;
    if (!layout_valid_ || layout_.NeedsUpdate(left_w, left_h, right_w, right_h)) {
        if (!UpdateLayout(left_w, left_h, right_w, right_h)) {
            Debug::Log("DualViewPipeline: Layout update failed");
            return 0;
        }

        // Need to resize compositor and interop
        if (!EnsureCompositorAndInterop()) {
            return 0;
        }
        layout_changed = true;
    }

    // Frame-change detection: skip redundant compositing when paused or scrubbing to same frame
    if (!layout_changed &&
        frame_left == last_composited_left_ &&
        frame_right == last_composited_right_ &&
        last_composite_texture_ != 0) {
        // Same frames as last composite - return cached texture
        return last_composite_texture_;
    }

    // Get D3D11 SRVs from both decoders
    // Each decoder renders YUV->RGB to its intermediate texture
    // frame_left/right = -1 indicates a gap (no clip at this position)
    ID3D11ShaderResourceView* left_srv = nullptr;
    ID3D11ShaderResourceView* right_srv = nullptr;

    // Only request frame from decoder if not a gap (frame >= 0)
    if (left_decoder_ && frame_left >= 0) {
        left_srv = left_decoder_->GetFrameAsD3D11SRV(static_cast<int>(frame_left));
    }
    if (right_decoder_ && frame_right >= 0) {
        right_srv = right_decoder_->GetFrameAsD3D11SRV(static_cast<int>(frame_right));
    }

    // Debug: log SRV status periodically
    static int update_count = 0;
    if (++update_count % 100 == 1) {
        Debug::Log("DualViewPipeline::UpdateFrame frame=" + std::to_string(frame_left) +
                   " left_srv=" + (left_srv ? "valid" : "null") +
                   " right_srv=" + (right_srv ? "valid" : "null") +
                   " left_decoder=" + (left_decoder_ ? "yes" : "no") +
                   " right_decoder=" + (right_decoder_ ? "yes" : "no"));
    }

    // Update wait status for audio sync
    waiting_for_both_frames_ = (!left_srv && left_decoder_) || (!right_srv && right_decoder_);

    // If neither source has a frame, return 0
    if (!left_srv && !right_srv) {
        return 0;
    }

    // Lock interop for D3D11 rendering
    if (!interop_->LockForD3D11()) {
        Debug::Log("DualViewPipeline: Failed to lock interop");
        return 0;
    }

    // Composite both sources to interop's render target
    bool composite_ok = compositor_->CompositeToTarget(
        left_srv, right_srv,
        interop_->GetRTV(),
        layout_);

    // Unlock for GL access
    interop_->UnlockForGL();

    if (!composite_ok) {
        Debug::Log("DualViewPipeline: Composite failed");
        return 0;
    }

    // Advance interop triple buffer
    interop_->AdvanceBuffers();

    GLuint gl_tex = interop_->GetGLTexture();

    // Cache the result for frame-change detection
    last_composited_left_ = frame_left;
    last_composited_right_ = frame_right;
    last_composite_texture_ = gl_tex;

    // Debug: log GL texture periodically
    static int gl_log_count = 0;
    if (++gl_log_count % 100 == 1) {
        Debug::Log("DualViewPipeline: composite GL texture = " + std::to_string(gl_tex));
    }

    return gl_tex;
}

void DualViewPipeline::GetOutputDimensions(int& width, int& height) const {
    width = layout_.composite_width;
    height = layout_.composite_height;
}

void DualViewPipeline::InvalidateLayout() {
    layout_valid_ = false;
    // Invalidate frame cache so next UpdateFrame does full composite
    last_composited_left_ = -2;
    last_composited_right_ = -2;
    last_composite_texture_ = 0;
}

void DualViewPipeline::SetPlaybackMode(bool playing) {
    playing_ = playing;
}

void DualViewPipeline::OnSeek(int64_t target_frame) {
    // Invalidate frame cache - decoder state may change after seek
    last_composited_left_ = -2;
    last_composited_right_ = -2;
    last_composite_texture_ = 0;

    // Notify decoders of seek
    if (left_decoder_) {
        left_decoder_->UpdatePlayhead(static_cast<int>(target_frame));
    }
    if (right_decoder_) {
        right_decoder_->UpdatePlayhead(static_cast<int>(target_frame));
    }
}

bool DualViewPipeline::AreBothFramesReady(int64_t frame_left, int64_t frame_right) const {
    bool left_ready = !left_decoder_ || left_decoder_->HasFrame(static_cast<int>(frame_left));
    bool right_ready = !right_decoder_ || right_decoder_->HasFrame(static_cast<int>(frame_right));
    return left_ready && right_ready;
}

} // namespace ump

#endif // _WIN32
