#include "frame_readback.h"

#ifdef _WIN32

#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/imgutils.h>
}

namespace ump {

FrameReadback::FrameReadback() = default;

FrameReadback::~FrameReadback() {
    Shutdown();
}

bool FrameReadback::Initialize(ID3D11Device* device) {
    if (!device) {
        Debug::Log("FrameReadback: null device");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);

    if (!context_) {
        Debug::Log("FrameReadback: failed to get device context");
        return false;
    }

    // Clear slots
    for (auto& slot : slots_) {
        slot = StagingSlot{};
    }

    initialized_ = true;
    Debug::Log("FrameReadback: initialized");
    return true;
}

void FrameReadback::Shutdown() {
    std::lock_guard<std::mutex> lock(slots_mutex_);

    for (auto& slot : slots_) {
        slot.staging_texture.Reset();
        slot.query.Reset();
        slot = StagingSlot{};
    }

    context_.Reset();
    device_.Reset();
    initialized_ = false;
}

//=============================================================================
// Synchronous Readback
//=============================================================================

AVFrame* FrameReadback::ReadbackToNewFrame(ID3D11Texture2D* src_texture,
                                            AVPixelFormat dest_format) {
    if (!initialized_ || !src_texture) return nullptr;

    // Get texture info
    D3D11_TEXTURE2D_DESC desc;
    src_texture->GetDesc(&desc);

    // Allocate AVFrame
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->format = dest_format;
    frame->width = desc.Width;
    frame->height = desc.Height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    if (!ReadbackToFrame(src_texture, frame)) {
        av_frame_free(&frame);
        return nullptr;
    }

    return frame;
}

bool FrameReadback::ReadbackToFrame(ID3D11Texture2D* src_texture, AVFrame* dest_frame) {
    if (!initialized_ || !src_texture || !dest_frame) return false;

    D3D11_TEXTURE2D_DESC desc;
    src_texture->GetDesc(&desc);

    // Allocate slot for staging
    int slot_id = AllocateSlot(desc.Width, desc.Height, desc.Format);
    if (slot_id < 0) {
        Debug::Log("FrameReadback: failed to allocate staging slot");
        return false;
    }

    auto& slot = slots_[slot_id];

    // Copy GPU texture to staging
    context_->CopyResource(slot.staging_texture.Get(), src_texture);

    // Map and copy to AVFrame (blocking)
    bool success = MapAndCopyToFrame(slot, dest_frame);

    FreeSlot(slot_id);
    return success;
}

//=============================================================================
// Async Readback
//=============================================================================

int FrameReadback::QueueReadback(ID3D11Texture2D* src_texture) {
    if (!initialized_ || !src_texture) return -1;

    D3D11_TEXTURE2D_DESC desc;
    src_texture->GetDesc(&desc);

    std::lock_guard<std::mutex> lock(slots_mutex_);

    // Find free slot
    int slot_id = -1;
    for (int i = 0; i < kStagingSlotCount; i++) {
        if (!slots_[i].in_use) {
            slot_id = i;
            break;
        }
    }

    if (slot_id < 0) {
        Debug::Log("FrameReadback: no free staging slots");
        return -1;
    }

    auto& slot = slots_[slot_id];

    // Ensure staging texture
    if (!EnsureStagingTexture(slot, desc.Width, desc.Height, desc.Format)) {
        return -1;
    }

    // Create query for completion check if needed
    if (!slot.query) {
        D3D11_QUERY_DESC query_desc = {};
        query_desc.Query = D3D11_QUERY_EVENT;
        HRESULT hr = device_->CreateQuery(&query_desc, &slot.query);
        if (FAILED(hr)) {
            Debug::Log("FrameReadback: failed to create query");
            return -1;
        }
    }

    // Copy GPU texture to staging
    context_->CopyResource(slot.staging_texture.Get(), src_texture);

    // Insert query to track completion
    context_->End(slot.query.Get());

    slot.in_use = true;
    slot.pending = true;
    slot.ready = false;

    return slot_id;
}

bool FrameReadback::IsReadbackComplete(int slot_id) {
    if (slot_id < 0 || slot_id >= kStagingSlotCount) return false;

    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto& slot = slots_[slot_id];

    if (!slot.in_use || !slot.pending) return false;
    if (slot.ready) return true;

    // Check query
    BOOL data = FALSE;
    HRESULT hr = context_->GetData(slot.query.Get(), &data, sizeof(data), D3D11_ASYNC_GETDATA_DONOTFLUSH);

    if (hr == S_OK && data) {
        slot.ready = true;
        slot.pending = false;
        return true;
    }

    return false;
}

AVFrame* FrameReadback::GetReadbackResult(int slot_id) {
    if (slot_id < 0 || slot_id >= kStagingSlotCount) return nullptr;

    // Wait for completion if not ready
    while (!IsReadbackComplete(slot_id)) {
        // Flush and wait
        context_->Flush();
        Sleep(1);
    }

    std::lock_guard<std::mutex> lock(slots_mutex_);
    auto& slot = slots_[slot_id];

    if (!slot.in_use || !slot.ready) return nullptr;

    // Allocate frame
    AVPixelFormat format = DXGIToAVPixelFormat(slot.format);
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        FreeSlot(slot_id);
        return nullptr;
    }

    frame->format = format;
    frame->width = slot.width;
    frame->height = slot.height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        FreeSlot(slot_id);
        return nullptr;
    }

    if (!MapAndCopyToFrame(slot, frame)) {
        av_frame_free(&frame);
        FreeSlot(slot_id);
        return nullptr;
    }

    // Free slot (don't hold lock during MapAndCopyToFrame so unlock/relock not needed here)
    slot.in_use = false;
    slot.pending = false;
    slot.ready = false;

    return frame;
}

void FrameReadback::CancelReadback(int slot_id) {
    if (slot_id < 0 || slot_id >= kStagingSlotCount) return;

    std::lock_guard<std::mutex> lock(slots_mutex_);
    FreeSlot(slot_id);
}

//=============================================================================
// Internal Helpers
//=============================================================================

int FrameReadback::AllocateSlot(int width, int height, DXGI_FORMAT format) {
    std::lock_guard<std::mutex> lock(slots_mutex_);

    // Find free slot
    int slot_id = -1;
    for (int i = 0; i < kStagingSlotCount; i++) {
        if (!slots_[i].in_use) {
            slot_id = i;
            break;
        }
    }

    if (slot_id < 0) return -1;

    auto& slot = slots_[slot_id];

    if (!EnsureStagingTexture(slot, width, height, format)) {
        return -1;
    }

    slot.in_use = true;
    slot.pending = false;
    slot.ready = false;

    return slot_id;
}

void FrameReadback::FreeSlot(int slot_id) {
    // Caller must hold slots_mutex_ or slot must be exclusively owned
    if (slot_id < 0 || slot_id >= kStagingSlotCount) return;

    auto& slot = slots_[slot_id];
    slot.in_use = false;
    slot.pending = false;
    slot.ready = false;
    // Keep staging texture allocated for reuse
}

bool FrameReadback::EnsureStagingTexture(StagingSlot& slot, int width, int height, DXGI_FORMAT format) {
    if (slot.staging_texture &&
        slot.width == width &&
        slot.height == height &&
        slot.format == format) {
        return true;
    }

    // Create new staging texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    slot.staging_texture.Reset();
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot.staging_texture);
    if (FAILED(hr)) {
        Debug::Log("FrameReadback: failed to create staging texture");
        return false;
    }

    slot.width = width;
    slot.height = height;
    slot.format = format;

    return true;
}

bool FrameReadback::MapAndCopyToFrame(StagingSlot& slot, AVFrame* dest_frame) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(slot.staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        Debug::Log("FrameReadback: failed to map staging texture");
        return false;
    }

    // Copy based on format
    AVPixelFormat src_format = DXGIToAVPixelFormat(slot.format);
    AVPixelFormat dest_format = static_cast<AVPixelFormat>(dest_frame->format);

    bool success = false;

    if (slot.format == DXGI_FORMAT_NV12 || slot.format == DXGI_FORMAT_P010) {
        // NV12/P010: Y plane followed by interleaved UV plane
        int y_height = slot.height;
        int uv_height = slot.height / 2;
        int bytes_per_pixel = (slot.format == DXGI_FORMAT_P010) ? 2 : 1;

        // Y plane
        const uint8_t* src_y = static_cast<const uint8_t*>(mapped.pData);
        uint8_t* dst_y = dest_frame->data[0];
        int src_pitch = mapped.RowPitch;
        int dst_pitch = dest_frame->linesize[0];
        int copy_width = slot.width * bytes_per_pixel;

        for (int y = 0; y < y_height; y++) {
            memcpy(dst_y + y * dst_pitch, src_y + y * src_pitch, copy_width);
        }

        // UV plane (NV12 keeps interleaved, or we might need to deinterleave for planar)
        if (dest_format == AV_PIX_FMT_NV12 || dest_format == AV_PIX_FMT_P010LE) {
            // Keep interleaved
            const uint8_t* src_uv = src_y + mapped.RowPitch * y_height;
            uint8_t* dst_uv = dest_frame->data[1];
            int dst_uv_pitch = dest_frame->linesize[1];

            for (int y = 0; y < uv_height; y++) {
                memcpy(dst_uv + y * dst_uv_pitch, src_uv + y * src_pitch, copy_width);
            }
        } else if (dest_format == AV_PIX_FMT_YUV420P || dest_format == AV_PIX_FMT_YUV420P10LE) {
            // Deinterleave UV to separate U and V planes
            const uint8_t* src_uv = src_y + mapped.RowPitch * y_height;
            uint8_t* dst_u = dest_frame->data[1];
            uint8_t* dst_v = dest_frame->data[2];
            int dst_u_pitch = dest_frame->linesize[1];
            int dst_v_pitch = dest_frame->linesize[2];
            int uv_width = slot.width / 2;

            if (bytes_per_pixel == 1) {
                // 8-bit
                for (int y = 0; y < uv_height; y++) {
                    const uint8_t* row = src_uv + y * src_pitch;
                    uint8_t* u_row = dst_u + y * dst_u_pitch;
                    uint8_t* v_row = dst_v + y * dst_v_pitch;
                    for (int x = 0; x < uv_width; x++) {
                        u_row[x] = row[x * 2];
                        v_row[x] = row[x * 2 + 1];
                    }
                }
            } else {
                // 10-bit (16-bit per sample)
                for (int y = 0; y < uv_height; y++) {
                    const uint16_t* row = reinterpret_cast<const uint16_t*>(src_uv + y * src_pitch);
                    uint16_t* u_row = reinterpret_cast<uint16_t*>(dst_u + y * dst_u_pitch);
                    uint16_t* v_row = reinterpret_cast<uint16_t*>(dst_v + y * dst_v_pitch);
                    for (int x = 0; x < uv_width; x++) {
                        u_row[x] = row[x * 2];
                        v_row[x] = row[x * 2 + 1];
                    }
                }
            }
        }

        success = true;
    } else if (slot.format == DXGI_FORMAT_R8G8B8A8_UNORM ||
               slot.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // RGBA - simple copy
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        uint8_t* dst = dest_frame->data[0];
        int src_pitch = mapped.RowPitch;
        int dst_pitch = dest_frame->linesize[0];
        int copy_width = slot.width * 4;

        for (int y = 0; y < slot.height; y++) {
            memcpy(dst + y * dst_pitch, src + y * src_pitch, copy_width);
        }
        success = true;
    } else if (slot.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // HDR RGBA16F - copy as-is
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        uint8_t* dst = dest_frame->data[0];
        int src_pitch = mapped.RowPitch;
        int dst_pitch = dest_frame->linesize[0];
        int copy_width = slot.width * 8;  // 4 channels * 2 bytes

        for (int y = 0; y < slot.height; y++) {
            memcpy(dst + y * dst_pitch, src + y * src_pitch, copy_width);
        }
        success = true;
    }

    context_->Unmap(slot.staging_texture.Get(), 0);
    return success;
}

//=============================================================================
// Format Conversion Helpers
//=============================================================================

AVPixelFormat FrameReadback::DXGIToAVPixelFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_NV12:
            return AV_PIX_FMT_NV12;
        case DXGI_FORMAT_P010:
            return AV_PIX_FMT_P010LE;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return AV_PIX_FMT_RGBA;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return AV_PIX_FMT_BGRA;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return AV_PIX_FMT_RGBAF16LE;
        default:
            return AV_PIX_FMT_NONE;
    }
}

DXGI_FORMAT FrameReadback::AVPixelFormatToDXGI(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_NV12:
            return DXGI_FORMAT_NV12;
        case AV_PIX_FMT_P010LE:
            return DXGI_FORMAT_P010;
        case AV_PIX_FMT_RGBA:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case AV_PIX_FMT_BGRA:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace ump

#endif // _WIN32
