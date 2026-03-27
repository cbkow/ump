#ifdef __APPLE__

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>

#include "metal_yuv_renderer.h"
#include "metal_device_manager.h"
#include "../utils/debug_utils.h"

#include <thread>

// Embedded MSL shader source for YUV→RGBA conversion
// Supports 4:2:0, 4:2:2, and 4:4:4 chroma subsampling via params.subsampling:
//   0 = 4:2:0 (UV half width, half height) — H.264, HEVC, VP9
//   1 = 4:2:2 (UV half width, full height) — ProRes 422
//   2 = 4:4:4 (UV full width, full height) — ProRes 4444/XQ
static const char* kYUVToRGBShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct YUVParams {
    uint width;
    uint height;
    uint bit_depth;
    uint is_full_range;
    uint is_bt2020;
    uint is_hdr;
    uint subsampling;  // 0=4:2:0, 1=4:2:2, 2=4:4:4
};

kernel void yuv_to_rgba(
    texture2d<float, access::read> y_tex [[texture(0)]],
    texture2d<float, access::read> uv_tex [[texture(1)]],
    texture2d<float, access::write> out_tex [[texture(2)]],
    constant YUVParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    // Read Y value
    float y_val = y_tex.read(gid).r;

    // Read UV — position depends on chroma subsampling
    uint2 uv_pos;
    if (params.subsampling == 2) {
        // 4:4:4 — no subsampling
        uv_pos = gid;
    } else if (params.subsampling == 1) {
        // 4:2:2 — half horizontal, full vertical
        uv_pos = uint2(gid.x / 2, gid.y);
    } else {
        // 4:2:0 — half horizontal, half vertical
        uv_pos = uint2(gid.x / 2, gid.y / 2);
    }
    float2 uv_val = uv_tex.read(uv_pos).rg;

    // Adjust for video range vs full range
    float y, cb, cr;
    if (params.is_full_range) {
        y = y_val;
        cb = uv_val.x - 0.5;
        cr = uv_val.y - 0.5;
    } else {
        // Video range: Y [16/255, 235/255], UV [16/255, 240/255]
        y = (y_val - 16.0/255.0) * (255.0/219.0);
        cb = (uv_val.x - 128.0/255.0) * (255.0/224.0);
        cr = (uv_val.y - 128.0/255.0) * (255.0/224.0);
    }

    float r, g, b;
    if (params.is_bt2020) {
        // BT.2020 NCL
        r = y + 1.4746 * cr;
        g = y - 0.16455 * cb - 0.57135 * cr;
        b = y + 1.8814 * cb;
    } else {
        // BT.709
        r = y + 1.5748 * cr;
        g = y - 0.1873 * cb - 0.4681 * cr;
        b = y + 1.8556 * cb;
    }

    // Clamp and write output
    float4 rgba = float4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);

    // For HDR content (PQ/ST.2084), leave in linear-light for EDR processing
    // (no additional tone mapping here — that's handled by the OCIO/HDR pipeline)

    out_tex.write(rgba, gid);
}

// Interleaved 4444+Alpha kernel (y416: R=A, G=Y, B=Cb, A=Cr)
kernel void yuv_interleaved_to_rgba(
    texture2d<float, access::read> in_tex [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant YUVParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float4 sample = in_tex.read(gid);
    // y416 layout: A,Y,Cb,Cr → RGBA16Unorm: R=A, G=Y, B=Cb, A=Cr
    float alpha  = sample.r;
    float y_val  = sample.g;
    float cb_raw = sample.b;
    float cr_raw = sample.a;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = cb_raw - 0.5;
        cr = cr_raw - 0.5;
    } else {
        y  = (y_val  - 16.0/255.0) * (255.0/219.0);
        cb = (cb_raw - 128.0/255.0) * (255.0/224.0);
        cr = (cr_raw - 128.0/255.0) * (255.0/224.0);
    }

    float r, g, b;
    if (params.is_bt2020) {
        r = y + 1.4746 * cr;
        g = y - 0.16455 * cb - 0.57135 * cr;
        b = y + 1.8814 * cb;
    } else {
        r = y + 1.5748 * cr;
        g = y - 0.1873 * cb - 0.4681 * cr;
        b = y + 1.8556 * cb;
    }

    out_tex.write(float4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), alpha), gid);
}
)";

namespace qcview {

MetalYUVRenderer::MetalYUVRenderer() = default;

MetalYUVRenderer::~MetalYUVRenderer() {
    Shutdown();
}

bool MetalYUVRenderer::Initialize() {
    if (initialized_) return true;

    if (!CreateComputePipeline()) {
        Debug::Log("MetalYUVRenderer: Failed to create compute pipeline");
        return false;
    }

    initialized_ = true;
    Debug::Log("MetalYUVRenderer: Initialized");
    return true;
}

void MetalYUVRenderer::Shutdown() {
    if (!initialized_) return;

    FlushPendingWork();

    if (compute_pipeline_) {
        [(id)compute_pipeline_ release];
        compute_pipeline_ = nullptr;
    }

    if (interleaved_pipeline_) {
        [(id)interleaved_pipeline_ release];
        interleaved_pipeline_ = nullptr;
    }

    if (cached_output_desc_) {
        [(id)cached_output_desc_ release];
        cached_output_desc_ = nullptr;
        cached_desc_width_ = 0;
        cached_desc_height_ = 0;
    }

    initialized_ = false;
    Debug::Log("MetalYUVRenderer: Shutdown");
}

void MetalYUVRenderer::FlushPendingWork() {
    // Wait for all in-flight async command buffers to complete.
    // In practice there are at most 1-2 in flight during playback.
    int spins = 0;
    while (in_flight_count_->load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (++spins > 50000) {  // 5 second safety timeout
            Debug::Log("MetalYUVRenderer: FlushPendingWork timeout, " +
                       std::to_string(in_flight_count_->load()) + " still in flight");
            break;
        }
    }
}

bool MetalYUVRenderer::CreateComputePipeline() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kYUVToRGBShaderSource];

    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        Debug::Log("MetalYUVRenderer: Shader compilation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    id<MTLFunction> kernel = [library newFunctionWithName:@"yuv_to_rgba"];
    if (!kernel) {
        Debug::Log("MetalYUVRenderer: yuv_to_rgba function not found");
        return false;
    }

    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:kernel error:&error];
    if (!pipeline) {
        Debug::Log("MetalYUVRenderer: Pipeline creation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    // In non-ARC, newComputePipelineState returns +1 retained — just store the pointer
    compute_pipeline_ = (void*)pipeline;

    // Create interleaved 4444+alpha pipeline
    id<MTLFunction> interleavedKernel = [library newFunctionWithName:@"yuv_interleaved_to_rgba"];
    if (interleavedKernel) {
        id<MTLComputePipelineState> interleavedPipeline = [device newComputePipelineStateWithFunction:interleavedKernel error:&error];
        if (interleavedPipeline) {
            interleaved_pipeline_ = (void*)interleavedPipeline;
        } else {
            Debug::Log("MetalYUVRenderer: Interleaved pipeline creation failed: " +
                       std::string([error.localizedDescription UTF8String]));
        }
    }

    return true;
}

void* MetalYUVRenderer::RenderToRGBA(void* y_texture, void* uv_texture,
                                      int width, int height,
                                      int bit_depth, bool is_full_range,
                                      bool is_bt2020, bool is_hdr,
                                      int subsampling) {
    if (!initialized_ || !y_texture || !uv_texture) return nullptr;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();

    // Cache output descriptor — same dimensions for all frames of a given file
    if (width != cached_desc_width_ || height != cached_desc_height_) {
        if (cached_output_desc_) [(id)cached_output_desc_ release];
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        [desc retain];
        cached_output_desc_ = (void*)desc;
        cached_desc_width_ = width;
        cached_desc_height_ = height;
    }

    id<MTLTexture> output = [device newTextureWithDescriptor:(__bridge MTLTextureDescriptor*)cached_output_desc_];
    if (!output) return nullptr;

    // Encode compute command
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)compute_pipeline_;
    [encoder setComputePipelineState:pipeline];

    [encoder setTexture:(__bridge id<MTLTexture>)y_texture atIndex:0];
    [encoder setTexture:(__bridge id<MTLTexture>)uv_texture atIndex:1];
    [encoder setTexture:output atIndex:2];

    struct {
        uint32_t width, height, bit_depth, is_full_range, is_bt2020, is_hdr, subsampling;
    } params = {
        (uint32_t)width, (uint32_t)height, (uint32_t)bit_depth,
        is_full_range ? 1u : 0u, is_bt2020 ? 1u : 0u, is_hdr ? 1u : 0u,
        (uint32_t)subsampling
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    return (void*)output;
}

void* MetalYUVRenderer::RenderToRGBAAsync(void* y_texture, void* uv_texture,
                                            int width, int height,
                                            int bit_depth, bool is_full_range,
                                            bool is_bt2020, bool is_hdr,
                                            int subsampling,
                                            void (*cleanup_fn)(void* ctx),
                                            void* cleanup_ctx) {
    if (!initialized_ || !y_texture || !uv_texture) {
        // If we can't render, still need to clean up caller's resources
        if (cleanup_fn) cleanup_fn(cleanup_ctx);
        return nullptr;
    }

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();

    // Cache output descriptor — same dimensions for all frames of a given file
    if (width != cached_desc_width_ || height != cached_desc_height_) {
        if (cached_output_desc_) [(id)cached_output_desc_ release];
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        [desc retain];
        cached_output_desc_ = (void*)desc;
        cached_desc_width_ = width;
        cached_desc_height_ = height;
    }

    id<MTLTexture> output = [device newTextureWithDescriptor:(__bridge MTLTextureDescriptor*)cached_output_desc_];
    if (!output) {
        if (cleanup_fn) cleanup_fn(cleanup_ctx);
        return nullptr;
    }

    // Encode compute command
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)compute_pipeline_;
    [encoder setComputePipelineState:pipeline];

    [encoder setTexture:(__bridge id<MTLTexture>)y_texture atIndex:0];
    [encoder setTexture:(__bridge id<MTLTexture>)uv_texture atIndex:1];
    [encoder setTexture:output atIndex:2];

    struct {
        uint32_t width, height, bit_depth, is_full_range, is_bt2020, is_hdr, subsampling;
    } params = {
        (uint32_t)width, (uint32_t)height, (uint32_t)bit_depth,
        is_full_range ? 1u : 0u, is_bt2020 ? 1u : 0u, is_hdr ? 1u : 0u,
        (uint32_t)subsampling
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];

    // Capture cleanup context and shared counter for the completion handler.
    // The shared_ptr keeps the atomic counter alive even if the renderer is
    // destroyed before the handler fires (shutdown ordering safety).
    void (*fn)(void*) = cleanup_fn;
    void* ctx = cleanup_ctx;
    std::shared_ptr<std::atomic<int>> counter = in_flight_count_;

    in_flight_count_->fetch_add(1, std::memory_order_relaxed);

    [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
        if (fn) fn(ctx);
        counter->fetch_sub(1, std::memory_order_release);
    }];

    [cmdBuf commit];
    // NO waitUntilCompleted — decode thread continues immediately.
    // Output texture is safe for pool registration: Metal's same-queue ordering
    // guarantees the compute finishes before any later command buffer reads it.

    return (void*)output;
}

void* MetalYUVRenderer::RenderInterleavedToRGBA(void* in_texture,
                                                  int width, int height,
                                                  int bit_depth, bool is_full_range,
                                                  bool is_bt2020, bool is_hdr) {
    if (!initialized_ || !in_texture || !interleaved_pipeline_) return nullptr;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();

    if (width != cached_desc_width_ || height != cached_desc_height_) {
        if (cached_output_desc_) [(id)cached_output_desc_ release];
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        [desc retain];
        cached_output_desc_ = (void*)desc;
        cached_desc_width_ = width;
        cached_desc_height_ = height;
    }

    id<MTLTexture> output = [device newTextureWithDescriptor:(__bridge MTLTextureDescriptor*)cached_output_desc_];
    if (!output) return nullptr;

    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)interleaved_pipeline_;
    [encoder setComputePipelineState:pipeline];

    [encoder setTexture:(__bridge id<MTLTexture>)in_texture atIndex:0];
    [encoder setTexture:output atIndex:1];

    struct {
        uint32_t width, height, bit_depth, is_full_range, is_bt2020, is_hdr, subsampling;
    } params = {
        (uint32_t)width, (uint32_t)height, (uint32_t)bit_depth,
        is_full_range ? 1u : 0u, is_bt2020 ? 1u : 0u, is_hdr ? 1u : 0u, 2u
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    return (void*)output;
}

void* MetalYUVRenderer::RenderInterleavedToRGBAAsync(void* in_texture,
                                                       int width, int height,
                                                       int bit_depth, bool is_full_range,
                                                       bool is_bt2020, bool is_hdr,
                                                       void (*cleanup_fn)(void* ctx),
                                                       void* cleanup_ctx) {
    if (!initialized_ || !in_texture || !interleaved_pipeline_) {
        if (cleanup_fn) cleanup_fn(cleanup_ctx);
        return nullptr;
    }

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();

    if (width != cached_desc_width_ || height != cached_desc_height_) {
        if (cached_output_desc_) [(id)cached_output_desc_ release];
        MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        [desc retain];
        cached_output_desc_ = (void*)desc;
        cached_desc_width_ = width;
        cached_desc_height_ = height;
    }

    id<MTLTexture> output = [device newTextureWithDescriptor:(__bridge MTLTextureDescriptor*)cached_output_desc_];
    if (!output) {
        if (cleanup_fn) cleanup_fn(cleanup_ctx);
        return nullptr;
    }

    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)interleaved_pipeline_;
    [encoder setComputePipelineState:pipeline];

    [encoder setTexture:(__bridge id<MTLTexture>)in_texture atIndex:0];
    [encoder setTexture:output atIndex:1];

    struct {
        uint32_t width, height, bit_depth, is_full_range, is_bt2020, is_hdr, subsampling;
    } params = {
        (uint32_t)width, (uint32_t)height, (uint32_t)bit_depth,
        is_full_range ? 1u : 0u, is_bt2020 ? 1u : 0u, is_hdr ? 1u : 0u, 2u
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];

    void (*fn)(void*) = cleanup_fn;
    void* ctx = cleanup_ctx;
    std::shared_ptr<std::atomic<int>> counter = in_flight_count_;

    in_flight_count_->fetch_add(1, std::memory_order_relaxed);

    [cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull) {
        if (fn) fn(ctx);
        counter->fetch_sub(1, std::memory_order_release);
    }];

    [cmdBuf commit];

    return (void*)output;
}

} // namespace qcview

#endif // __APPLE__
