#ifdef __APPLE__

#import <Metal/Metal.h>
#include <imgui.h>

#include "metal_texture_pool.h"
#include "metal_device_manager.h"
#include "../utils/debug_utils.h"

namespace qcview {

MetalTexturePool& MetalTexturePool::Instance() {
    static MetalTexturePool instance;
    return instance;
}

MetalTexturePool& MetalTexturePool::ThumbnailInstance() {
    static MetalTexturePool instance;
    return instance;
}

MetalTexturePool::MetalTexturePool() = default;

MetalTexturePool::~MetalTexturePool() {
    Shutdown();
}

bool MetalTexturePool::Initialize(size_t max_memory_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    auto& mgr = MetalDeviceManager::Instance();
    if (!mgr.IsInitialized()) {
        Debug::Log("MetalTexturePool: MetalDeviceManager not initialized");
        return false;
    }

    max_memory_bytes_ = max_memory_bytes;
    used_memory_bytes_ = 0;
    initialized_ = true;

    Debug::Log("MetalTexturePool: Initialized (max " +
               std::to_string(max_memory_bytes / (1024*1024)) + " MB)");
    return true;
}

void MetalTexturePool::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;

    Debug::Log("MetalTexturePool: Shutting down (" +
               std::to_string(textures_.size()) + " textures)...");

    for (auto& [id, tex] : textures_) {
        DestroyTexture(tex);
    }
    textures_.clear();
    lru_order_.clear();
    lru_pos_.clear();

    for (auto& entry : graveyard_) {
        DestroyTexture(entry.texture);
    }
    graveyard_.clear();

    used_memory_bytes_ = 0;
    initialized_ = false;
    Debug::Log("MetalTexturePool: Shutdown complete");
}

static MTLPixelFormat FormatFromInt(int format) {
    switch (format) {
        case 0: return MTLPixelFormatRGBA8Unorm;
        case 1: return MTLPixelFormatRGBA16Float;
        case 2: return MTLPixelFormatBGRA8Unorm;
        default: return MTLPixelFormatRGBA8Unorm;
    }
}

static size_t BytesPerPixel(int format) {
    switch (format) {
        case 0: return 4;   // RGBA8
        case 1: return 8;   // RGBA16F
        case 2: return 4;   // BGRA8
        default: return 4;
    }
}

uint64_t MetalTexturePool::CreateTextureFromPixels(int width, int height, int format,
                                                    const void* pixel_data, size_t pixel_data_size) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return 0;
    }

    // Create texture and upload pixels OUTSIDE the pool mutex.
    // Metal API calls (newTexture, replaceRegion) are thread-safe and don't touch pool state.
    // This prevents thumbnail uploads from blocking video frame texture operations.
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    MTLPixelFormat mtl_format = FormatFromInt(format);

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
                                                                                    width:width
                                                                                   height:height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;  // Unified memory on Apple Silicon

    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (!texture) {
        Debug::Log("MetalTexturePool: Failed to create texture " +
                   std::to_string(width) + "x" + std::to_string(height));
        return 0;
    }

    // Upload pixel data (memcpy on Apple Silicon unified memory — no GPU stall)
    if (pixel_data && pixel_data_size > 0) {
        size_t bpp = BytesPerPixel(format);
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:pixel_data
                   bytesPerRow:width * bpp];
    }

    size_t new_bytes = width * height * BytesPerPixel(format);

    // Register in pool under lock — evict first if over budget
    std::lock_guard<std::mutex> lock(mutex_);

    // Evict oldest textures to make room
    while (used_memory_bytes_ + new_bytes > max_memory_bytes_ && !lru_order_.empty()) {
        uint64_t oldest = lru_order_.front();
        lru_order_.pop_front();
        lru_pos_.erase(oldest);

        auto evict_it = textures_.find(oldest);
        if (evict_it != textures_.end()) {
            used_memory_bytes_ -= evict_it->second.byte_size;
            graveyard_.push_back({evict_it->second, graveyard_counter_});
            textures_.erase(evict_it);
        }
    }

    MetalTexture pool_tex;
    pool_tex.texture = (void*)texture;
    pool_tex.descriptor_set = pool_tex.texture;
    pool_tex.width = width;
    pool_tex.height = height;
    pool_tex.pixel_format = format;
    pool_tex.byte_size = new_bytes;
    pool_tex.is_valid = true;

    uint64_t id = next_id_++;
    textures_[id] = pool_tex;
    lru_order_.push_back(id);
    lru_pos_[id] = std::prev(lru_order_.end());
    used_memory_bytes_ += pool_tex.byte_size;

    return id;
}

uint64_t MetalTexturePool::CreateEmptyTexture(int width, int height, int format) {
    return CreateTextureFromPixels(width, height, format, nullptr, 0);
}

uint64_t MetalTexturePool::RegisterExistingTexture(void* mtl_texture, int width, int height, int format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !mtl_texture) return 0;

    size_t new_bytes = width * height * BytesPerPixel(format);

    // Evict oldest textures to stay within budget (same as CreateTextureFromPixels)
    while (used_memory_bytes_ + new_bytes > max_memory_bytes_ && !lru_order_.empty()) {
        uint64_t oldest = lru_order_.front();
        lru_order_.pop_front();
        lru_pos_.erase(oldest);

        auto evict_it = textures_.find(oldest);
        if (evict_it != textures_.end()) {
            used_memory_bytes_ -= evict_it->second.byte_size;
            graveyard_.push_back({evict_it->second, graveyard_counter_});
            textures_.erase(evict_it);
        }
    }

    MetalTexture pool_tex;
    pool_tex.texture = mtl_texture;  // Already retained by caller
    pool_tex.descriptor_set = mtl_texture;  // Non-retaining alias for ImGui
    pool_tex.width = width;
    pool_tex.height = height;
    pool_tex.pixel_format = format;
    pool_tex.byte_size = new_bytes;
    pool_tex.is_valid = true;

    uint64_t id = next_id_++;
    textures_[id] = pool_tex;
    lru_order_.push_back(id);
    lru_pos_[id] = std::prev(lru_order_.end());
    used_memory_bytes_ += new_bytes;

    return id;
}

bool MetalTexturePool::UpdateTexture(uint64_t texture_id, const void* pixel_data, size_t pixel_data_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return false;

    MetalTexture& tex = it->second;
    id<MTLTexture> mtl_tex = (__bridge id<MTLTexture>)tex.texture;

    size_t bpp = BytesPerPixel(tex.pixel_format);
    [mtl_tex replaceRegion:MTLRegionMake2D(0, 0, tex.width, tex.height)
               mipmapLevel:0
                 withBytes:pixel_data
               bytesPerRow:tex.width * bpp];

    Touch(texture_id);
    return true;
}

const MetalTexture* MetalTexturePool::GetTexture(uint64_t texture_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it != textures_.end()) return &it->second;
    return nullptr;
}

ImTextureID MetalTexturePool::GetImTextureID(uint64_t texture_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it != textures_.end() && it->second.descriptor_set) {
        // ImGui 1.92 Metal backend: ImTextureID = (intptr_t)id<MTLTexture>
        return (ImTextureID)(intptr_t)it->second.descriptor_set;
    }
    return (ImTextureID)0;
}

bool MetalTexturePool::GetDimensions(uint64_t texture_id, int& width, int& height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it != textures_.end()) {
        width = it->second.width;
        height = it->second.height;
        return true;
    }
    return false;
}

void MetalTexturePool::Touch(uint64_t texture_id) {
    auto pos_it = lru_pos_.find(texture_id);
    if (pos_it != lru_pos_.end()) {
        // O(1) splice to back — no search, no shift
        lru_order_.splice(lru_order_.end(), lru_order_, pos_it->second);
    }
}

void MetalTexturePool::QueueDelete(uint64_t texture_id) {
    std::lock_guard<std::mutex> lock(deletion_mutex_);
    pending_deletions_.push_back(texture_id);
}

void MetalTexturePool::ProcessPendingDeletions() {
    std::vector<uint64_t> to_delete;
    {
        std::lock_guard<std::mutex> lock(deletion_mutex_);
        to_delete.swap(pending_deletions_);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (uint64_t id : to_delete) {
        auto it = textures_.find(id);
        if (it != textures_.end()) {
            used_memory_bytes_ -= it->second.byte_size;
            graveyard_.push_back({it->second, graveyard_counter_});
            textures_.erase(it);

            auto pos_it = lru_pos_.find(id);
            if (pos_it != lru_pos_.end()) {
                lru_order_.erase(pos_it->second);
                lru_pos_.erase(pos_it);
            }
        }
    }

    // Clean up graveyard entries that are old enough
    graveyard_counter_++;
    graveyard_.erase(
        std::remove_if(graveyard_.begin(), graveyard_.end(),
            [this](GraveyardEntry& entry) {
                if (graveyard_counter_ - entry.death_counter >= kGraveyardDelayCalls) {
                    DestroyTexture(entry.texture);
                    return true;
                }
                return false;
            }),
        graveyard_.end()
    );
}

size_t MetalTexturePool::GetUsedMemory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_memory_bytes_;
}

size_t MetalTexturePool::GetTextureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
}

void MetalTexturePool::EvictToFitBytes(size_t required_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (used_memory_bytes_ + required_bytes > max_memory_bytes_ && !lru_order_.empty()) {
        uint64_t oldest = lru_order_.front();
        lru_order_.pop_front();
        lru_pos_.erase(oldest);

        auto it = textures_.find(oldest);
        if (it != textures_.end()) {
            used_memory_bytes_ -= it->second.byte_size;
            graveyard_.push_back({it->second, graveyard_counter_});
            textures_.erase(it);
        }
    }
}

void MetalTexturePool::DestroyTexture(MetalTexture& tex) {
    // descriptor_set is a non-retaining alias — just clear it
    tex.descriptor_set = nullptr;
    // texture holds the only retained reference — use ObjC release (not CFRelease)
    // since id<MTLTexture> is an ObjC object, not a CF type
    if (tex.texture) {
        [(id)tex.texture release];
        tex.texture = nullptr;
    }
    tex.is_valid = false;
}

bool MetalTexturePool::ReadbackTexture(uint64_t texture_id, unsigned char* out_rgba8) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = textures_.find(texture_id);
    if (it == textures_.end() || !it->second.is_valid || !it->second.texture) {
        return false;
    }

    const MetalTexture& tex = it->second;
    id<MTLTexture> mtl_tex = (__bridge id<MTLTexture>)tex.texture;
    int w = tex.width;
    int h = tex.height;

    MTLPixelFormat fmt = [mtl_tex pixelFormat];

    if (fmt == MTLPixelFormatRGBA8Unorm || fmt == MTLPixelFormatRGBA8Unorm_sRGB) {
        // Direct readback — 4 bytes per pixel
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        [mtl_tex getBytes:out_rgba8 bytesPerRow:w * 4 fromRegion:region mipmapLevel:0];
        return true;
    } else if (fmt == MTLPixelFormatBGRA8Unorm || fmt == MTLPixelFormatBGRA8Unorm_sRGB) {
        // BGRA → RGBA swizzle
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        [mtl_tex getBytes:out_rgba8 bytesPerRow:w * 4 fromRegion:region mipmapLevel:0];
        for (int i = 0; i < w * h; ++i) {
            std::swap(out_rgba8[i * 4 + 0], out_rgba8[i * 4 + 2]);
        }
        return true;
    } else if (fmt == MTLPixelFormatRGBA16Float) {
        // RGBA16F → RGBA8: read half-float data, convert to 8-bit
        size_t row_bytes = w * 8;  // 4 channels * 2 bytes each
        std::vector<uint16_t> half_data(w * h * 4);
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        [mtl_tex getBytes:half_data.data() bytesPerRow:row_bytes fromRegion:region mipmapLevel:0];

        for (int i = 0; i < w * h * 4; ++i) {
            // IEEE 754 half-float → float via bit manipulation
            uint16_t bits = half_data[i];
            uint32_t sign = (bits >> 15) & 0x1;
            uint32_t exponent = (bits >> 10) & 0x1F;
            uint32_t mantissa = bits & 0x3FF;

            // Reconstruct float32 from float16 bit pattern
            uint32_t f32_bits;
            if (exponent == 0) {
                if (mantissa == 0) {
                    f32_bits = sign << 31;  // ±0
                } else {
                    // Denormalized: convert to normalized float32
                    float val = (5.9604645e-8f) * mantissa;  // 2^-24 * mantissa
                    if (sign) val = -val;
                    float clamped = (val < 0.0f) ? 0.0f : (val > 1.0f) ? 1.0f : val;
                    out_rgba8[i] = static_cast<unsigned char>(clamped * 255.0f + 0.5f);
                    continue;
                }
            } else if (exponent == 31) {
                f32_bits = (sign << 31) | 0x7F800000 | (mantissa << 13);  // Inf/NaN
            } else {
                f32_bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
            }
            float val;
            memcpy(&val, &f32_bits, sizeof(float));

            // Clamp [0,1] and convert to 8-bit
            val = (val < 0.0f) ? 0.0f : (val > 1.0f) ? 1.0f : val;
            out_rgba8[i] = static_cast<unsigned char>(val * 255.0f + 0.5f);
        }
        return true;
    }

    Debug::Log("MetalTexturePool::ReadbackTexture: Unsupported pixel format " + std::to_string((int)fmt));
    return false;
}

} // namespace qcview

#endif // __APPLE__
