#include "hdr_direct_decoder.h"

#ifdef _WIN32

#include <chrono>
#include "streaming_video_decoder.h"
#include "video_decoder_factory.h"  // For g_video_range_override
#include "../gpu/d3d11_hwframe_extractor.h"
#include "../gpu/d3d11_yuv_renderer.h"
#include "../gpu/d3d11_video_interop.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace ump {

HDRDirectVideoDecoder::HDRDirectVideoDecoder() = default;

HDRDirectVideoDecoder::~HDRDirectVideoDecoder() {
    Shutdown();
}

bool HDRDirectVideoDecoder::Initialize(ID3D11Device* device, const std::string& video_path) {
    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("HDRDirectVideoDecoder: Device is null");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);
    video_path_ = video_path;

    //=========================================================================
    // Initialize Streaming Video Decoder
    //=========================================================================
    decoder_ = std::make_unique<StreamingVideoDecoder>(video_path);

    // Enable hardware acceleration with D3D11VA preference for zero-copy interop
    StreamingDecoderConfig config;
    config.useHardwareAccel = true;
    config.preferD3D11VA = true;  // D3D11VA outputs directly to D3D11 textures
    decoder_->SetConfig(config);

    if (!decoder_->Initialize()) {
        Debug::Log("HDRDirectVideoDecoder: Failed to initialize decoder");
        decoder_.reset();
        return false;
    }

    // Enable raw HW frame mode for zero-copy access
    decoder_->SetRawHWFrameMode(true);

    //=========================================================================
    // Initialize D3D11 Components
    //=========================================================================

    // HW Frame Extractor
    extractor_ = std::make_unique<D3D11HWFrameExtractor>();
    if (!extractor_->Initialize(device)) {
        Debug::Log("HDRDirectVideoDecoder: Failed to initialize HW frame extractor");
        Shutdown();
        return false;
    }

    // YUV Renderer
    yuv_renderer_ = std::make_unique<D3D11YUVRenderer>();
    if (!yuv_renderer_->Initialize(device)) {
        Debug::Log("HDRDirectVideoDecoder: Failed to initialize YUV renderer");
        Shutdown();
        return false;
    }

    // Video Interop (D3D11 -> GL)
    interop_ = std::make_unique<D3D11VideoInterop>();
    if (!interop_->Initialize(device)) {
        Debug::Log("HDRDirectVideoDecoder: Failed to initialize video interop");
        Shutdown();
        return false;
    }

    initialized_ = true;

    Debug::Log("HDRDirectVideoDecoder: Initialized");
    Debug::Log("  Video: " + video_path);
    Debug::Log("  Resolution: " + std::to_string(GetWidth()) + "x" + std::to_string(GetHeight()));
    Debug::Log("  HW Decode: " + std::string(IsHardwareDecodeActive() ? "Yes" : "No"));
    Debug::Log("  Zero-Copy: " + std::string(interop_->GetInteropMethodName()));

    return true;
}

void HDRDirectVideoDecoder::Shutdown() {
    interop_.reset();
    yuv_renderer_.reset();
    extractor_.reset();

    if (decoder_) {
        decoder_->Shutdown();
        decoder_.reset();
    }

    context_.Reset();
    device_.Reset();

    video_path_.clear();
    current_frame_ = -1;
    is_hdr_content_ = false;
    initialized_ = false;
}

GLuint HDRDirectVideoDecoder::GetFrameAsGLTexture(int frame_number) {
    if (!initialized_) {
        return 0;
    }

    // Update playhead to ensure frame is decoded
    decoder_->UpdatePlayhead(frame_number);

    // Check if we already have this frame processed
    if (frame_number == current_frame_ && interop_->GetGLTexture()) {
        return interop_->GetGLTexture();
    }

    // Process the frame through the GPU pipeline
    if (!ProcessFrame(frame_number)) {
        // GPU pipeline failed - fall back to software decode path
        static int fail_count = 0;
        if (++fail_count <= 5) {
            Debug::Log("HDRDirectVideoDecoder::GetFrameAsGLTexture: ProcessFrame failed for frame " +
                       std::to_string(frame_number) + " (fail #" + std::to_string(fail_count) + ")");
        }
        return 0;
    }

    current_frame_ = frame_number;
    GLuint tex = interop_->GetGLTexture();

    // Log first successful frame
    static bool first_success = true;
    if (first_success && tex != 0) {
        Debug::Log("HDRDirectVideoDecoder: First successful frame " + std::to_string(frame_number) +
                   " -> GL texture " + std::to_string(tex));
        first_success = false;
    }

    return tex;
}

void HDRDirectVideoDecoder::UpdatePlayhead(int frame_number) {
    if (decoder_) {
        decoder_->UpdatePlayhead(frame_number);
    }
}

bool HDRDirectVideoDecoder::ProcessFrame(int frame_number) {
    if (!decoder_ || !extractor_ || !yuv_renderer_ || !interop_) {
        Debug::Log("HDRDirectVideoDecoder::ProcessFrame: Missing component");
        return false;
    }

    if (!IsHardwareDecodeActive()) {
        // No hardware decode - can't use zero-copy path
        Debug::Log("HDRDirectVideoDecoder::ProcessFrame: HW decode not active");
        return false;
    }

    //=========================================================================
    // Step 1: Get raw hardware frame from decoder
    //=========================================================================
    AVFrame* hw_frame = decoder_->GetRawHWFrame(frame_number);
    if (!hw_frame) {
        // Frame not available yet - log once per second max
        static auto last_log = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() > 1000) {
            Debug::Log("HDRDirectVideoDecoder::ProcessFrame: No HW frame for frame " + std::to_string(frame_number));
            last_log = now;
        }
        return false;
    }

    //=========================================================================
    // Step 2: Extract D3D11 texture from AVFrame
    //=========================================================================
    auto extracted = extractor_->Extract(hw_frame);
    if (!extracted) {
        Debug::Log("HDRDirectVideoDecoder: Failed to extract D3D11 texture from frame");
        decoder_->ReleaseRawHWFrame();
        return false;
    }

    // Update HDR content flag based on format
    is_hdr_content_ = extracted->is_hdr;

    //=========================================================================
    // Step 3: Ensure interop texture is created with correct dimensions
    //=========================================================================
    if (!EnsureInteropTexture(extracted->width, extracted->height)) {
        Debug::Log("HDRDirectVideoDecoder: Failed to create interop texture");
        decoder_->ReleaseRawHWFrame();
        return false;
    }

    //=========================================================================
    // Step 4: Lock interop for D3D11 rendering
    //=========================================================================
    if (!interop_->LockForD3D11()) {
        Debug::Log("HDRDirectVideoDecoder: Failed to lock interop for D3D11");
        decoder_->ReleaseRawHWFrame();
        return false;
    }

    //=========================================================================
    // Step 5: Render YUV to RGB via D3D11 shader
    //=========================================================================
    YUVRenderParams params;
    params.width = extracted->width;
    params.height = extracted->height;
    params.bit_depth = extracted->bit_depth;
    params.is_hdr = extracted->is_hdr;
    // Respect user video range override (FULL/LIMITED/AUTO)
    // AUTO uses detected range from content metadata
    switch (g_video_range_override) {
        case VideoRangeMode::FULL: params.is_full_range = true; break;
        case VideoRangeMode::LIMITED: params.is_full_range = false; break;
        default: params.is_full_range = extracted->is_hdr; break;  // AUTO: HDR=full, SDR=limited
    }
    params.color_space = extracted->is_hdr ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;

    bool render_success = yuv_renderer_->Render(
        extracted->srv_y.Get(),
        extracted->srv_uv.Get(),
        interop_->GetRTV(),
        params
    );

    //=========================================================================
    // Step 6: Unlock interop for OpenGL access
    //=========================================================================
    interop_->UnlockForGL();

    // Release the raw frame back to decoder
    decoder_->ReleaseRawHWFrame();

    if (!render_success) {
        Debug::Log("HDRDirectVideoDecoder: YUV render failed");
        return false;
    }

    return true;
}

bool HDRDirectVideoDecoder::EnsureInteropTexture(int width, int height) {
    if (!interop_) {
        return false;
    }

    // Use RGBA16F for HDR content, RGBA8 for SDR
    DXGI_FORMAT format = is_hdr_content_ ?
        DXGI_FORMAT_R16G16B16A16_FLOAT :
        DXGI_FORMAT_R8G8B8A8_UNORM;

    return interop_->CreateSharedTexture(width, height, format);
}

bool HDRDirectVideoDecoder::IsHardwareDecodeActive() const {
    if (!decoder_) {
        return false;
    }
    return decoder_->IsHardwareAccelerated() &&
           decoder_->GetHWAccelType() == HWAccelType::D3D11VA;
}

bool HDRDirectVideoDecoder::IsZeroCopyActive() const {
    if (!interop_) {
        return false;
    }
    return interop_->HasZeroCopyInterop() && IsHardwareDecodeActive();
}

std::string HDRDirectVideoDecoder::GetDecodeModeDescription() const {
    std::string desc;

    // Decode method
    if (decoder_) {
        desc += HWAccelTypeToString(decoder_->GetHWAccelType());
    } else {
        desc += "Not initialized";
    }

    // Interop method
    if (interop_) {
        desc += " + ";
        desc += interop_->GetInteropMethodName();
    }

    // HDR status
    if (is_hdr_content_) {
        desc += " (HDR)";
    }

    return desc;
}

int HDRDirectVideoDecoder::GetWidth() const {
    return decoder_ ? decoder_->GetWidth() : 0;
}

int HDRDirectVideoDecoder::GetHeight() const {
    return decoder_ ? decoder_->GetHeight() : 0;
}

double HDRDirectVideoDecoder::GetFPS() const {
    return decoder_ ? decoder_->GetFPS() : 0.0;
}

double HDRDirectVideoDecoder::GetDuration() const {
    return decoder_ ? decoder_->GetDuration() : 0.0;
}

int HDRDirectVideoDecoder::GetFrameCount() const {
    return decoder_ ? decoder_->GetFrameCount() : 0;
}

} // namespace ump

#endif // _WIN32
