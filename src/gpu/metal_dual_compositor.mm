#ifdef __APPLE__

#import <Metal/Metal.h>

#include "metal_dual_compositor.h"
#include "metal_device_manager.h"
#include "metal_texture_pool.h"
#include "../utils/debug_utils.h"
#include "dual_view_layout.h"

static const char* kCompositorShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct CompositorParams {
    uint output_width;
    uint output_height;
    // Left rect (in output pixels)
    uint left_x, left_y, left_w, left_h;
    // Right rect
    uint right_x, right_y, right_w, right_h;
    // Presence flags (1 = has texture, 0 = gap)
    uint has_left;
    uint has_right;
};

kernel void composite_dual_view(
    texture2d<float, access::read> left_tex [[texture(0)]],
    texture2d<float, access::read> right_tex [[texture(1)]],
    texture2d<float, access::write> out_tex [[texture(2)]],
    constant CompositorParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.output_width || gid.y >= params.output_height) return;

    float4 color = float4(0.0, 0.0, 0.0, 1.0);  // Black gap

    // Check if pixel is in left region
    if (params.has_left &&
        gid.x >= params.left_x && gid.x < params.left_x + params.left_w &&
        gid.y >= params.left_y && gid.y < params.left_y + params.left_h) {
        uint2 src = uint2(
            (gid.x - params.left_x) * left_tex.get_width() / params.left_w,
            (gid.y - params.left_y) * left_tex.get_height() / params.left_h
        );
        color = left_tex.read(src);
    }
    // Check right region
    else if (params.has_right &&
             gid.x >= params.right_x && gid.x < params.right_x + params.right_w &&
             gid.y >= params.right_y && gid.y < params.right_y + params.right_h) {
        uint2 src = uint2(
            (gid.x - params.right_x) * right_tex.get_width() / params.right_w,
            (gid.y - params.right_y) * right_tex.get_height() / params.right_h
        );
        color = right_tex.read(src);
    }

    out_tex.write(color, gid);
}
)";

namespace qcview {

MetalDualCompositor::MetalDualCompositor() = default;

MetalDualCompositor::~MetalDualCompositor() {
    Shutdown();
}

bool MetalDualCompositor::Initialize() {
    if (initialized_) return true;

    if (!CreateComputePipeline()) {
        Debug::Log("MetalDualCompositor: Failed to create compute pipeline");
        return false;
    }

    // Create 1x1 black dummy texture for gap sides (Metal requires all texture slots bound)
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                    width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;
    id<MTLTexture> dummy = [device newTextureWithDescriptor:desc];
    uint16_t black[4] = {0, 0, 0, 0x3C00};  // RGBA16F: 0,0,0,1.0
    [dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black bytesPerRow:8];
    dummy_texture_ = (void*)dummy;  // retained from newTextureWithDescriptor

    initialized_ = true;
    Debug::Log("MetalDualCompositor: Initialized");
    return true;
}

void MetalDualCompositor::Shutdown() {
    if (!initialized_) return;

    if (compute_pipeline_) {
        CFRelease(compute_pipeline_);
        compute_pipeline_ = nullptr;
    }

    if (dummy_texture_) {
        [(id<MTLTexture>)dummy_texture_ release];
        dummy_texture_ = nullptr;
    }

    // Clean up cached output texture
    if (output_pool_id_ != 0) {
        MetalTexturePool::Instance().QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
        output_width_ = 0;
        output_height_ = 0;
    }

    initialized_ = false;
    Debug::Log("MetalDualCompositor: Shutdown");
}

bool MetalDualCompositor::CreateComputePipeline() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kCompositorShaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        Debug::Log("MetalDualCompositor: Shader compilation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    id<MTLFunction> func = [library newFunctionWithName:@"composite_dual_view"];
    if (!func) return false;

    id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:func error:&error];
    if (!pso) return false;

    compute_pipeline_ = (__bridge_retained void*)pso;
    return true;
}

uint64_t MetalDualCompositor::Composite(uint64_t left_pool_id, uint64_t right_pool_id,
                                         int output_width, int output_height,
                                         const DualViewLayout& layout) {
    if (!initialized_) return 0;

    auto& pool = MetalTexturePool::Instance();

    // Resolve source textures (0 = gap → use dummy)
    bool has_left = false, has_right = false;
    const MetalTexture* left_tex = nullptr;
    const MetalTexture* right_tex = nullptr;

    if (left_pool_id != 0) {
        left_tex = pool.GetTexture(left_pool_id);
        has_left = (left_tex && left_tex->is_valid && left_tex->texture);
    }
    if (right_pool_id != 0) {
        right_tex = pool.GetTexture(right_pool_id);
        has_right = (right_tex && right_tex->is_valid && right_tex->texture);
    }

    if (!has_left && !has_right) return 0;

    // Reuse output texture if dimensions match, otherwise create fresh
    if (output_pool_id_ != 0 &&
        (output_width_ != output_width || output_height_ != output_height)) {
        pool.QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
    }
    if (output_pool_id_ == 0) {
        output_pool_id_ = pool.CreateEmptyTexture(output_width, output_height, 1 /* RGBA16Float */);
        output_width_ = output_width;
        output_height_ = output_height;
    }

    const MetalTexture* out_tex = pool.GetTexture(output_pool_id_);
    if (!out_tex || !out_tex->texture) {
        output_pool_id_ = 0;
        return 0;
    }

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)compute_pipeline_;
    [encoder setComputePipelineState:pso];

    // Bind textures — use dummy for missing sides (Metal requires all slots bound)
    id<MTLTexture> dummy = (__bridge id<MTLTexture>)dummy_texture_;
    [encoder setTexture:(has_left ? (__bridge id<MTLTexture>)left_tex->texture : dummy) atIndex:0];
    [encoder setTexture:(has_right ? (__bridge id<MTLTexture>)right_tex->texture : dummy) atIndex:1];
    [encoder setTexture:(__bridge id<MTLTexture>)out_tex->texture atIndex:2];

    struct {
        uint32_t output_width, output_height;
        uint32_t left_x, left_y, left_w, left_h;
        uint32_t right_x, right_y, right_w, right_h;
        uint32_t has_left, has_right;
    } params = {
        (uint32_t)output_width, (uint32_t)output_height,
        (uint32_t)layout.left_x, (uint32_t)layout.left_y,
        (uint32_t)layout.left_w, (uint32_t)layout.left_h,
        (uint32_t)layout.right_x, (uint32_t)layout.right_y,
        (uint32_t)layout.right_w, (uint32_t)layout.right_h,
        has_left ? 1u : 0u, has_right ? 1u : 0u
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(output_width, output_height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];
    [cmdBuf commit];
    // No waitUntilCompleted — GPU queue ordering handles synchronization.

    // Touch output to prevent LRU eviction
    pool.Touch(output_pool_id_);
    return output_pool_id_;
}

} // namespace qcview

//=============================================================================
// MetalLetterboxComposite — free function used by TimelineCache
// Scales source texture into a letterboxed region of the destination texture.
// rect_x/y/w/h are in normalized [0,1] coordinates within the destination.
//=============================================================================

static const char* kLetterboxShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct LetterboxParams {
    uint canvas_w;
    uint canvas_h;
    float rect_x;  // Normalized left edge of content region
    float rect_y;  // Normalized top edge
    float rect_w;  // Normalized width
    float rect_h;  // Normalized height
};

kernel void letterbox_composite(
    texture2d<float, access::sample> src_tex [[texture(0)]],
    texture2d<float, access::write> dst_tex [[texture(1)]],
    constant LetterboxParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.canvas_w || gid.y >= params.canvas_h) return;

    // Normalized position within canvas
    float nx = float(gid.x) / float(params.canvas_w);
    float ny = float(gid.y) / float(params.canvas_h);

    // Check if pixel is inside the content region
    if (nx >= params.rect_x && nx < params.rect_x + params.rect_w &&
        ny >= params.rect_y && ny < params.rect_y + params.rect_h) {
        // Map canvas position to source UV
        float u = (nx - params.rect_x) / params.rect_w;
        float v = (ny - params.rect_y) / params.rect_h;

        constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
        float4 color = src_tex.sample(s, float2(u, v));
        dst_tex.write(color, gid);
    } else {
        // Outside content region — black bar
        dst_tex.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    }
}
)";

static id<MTLComputePipelineState> g_letterbox_pipeline = nil;

extern "C" bool MetalLetterboxComposite(void* src_texture, void* dst_texture,
                             int canvas_w, int canvas_h,
                             float rect_x, float rect_y,
                             float rect_w, float rect_h) {
    @autoreleasepool {
        auto& mgr = qcview::MetalDeviceManager::Instance();
        if (!mgr.IsInitialized()) return false;

        id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();

        // Create pipeline on first use
        if (!g_letterbox_pipeline) {
            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:kLetterboxShaderSource];
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (!library) {
                Debug::Log("MetalLetterboxComposite: Shader compile failed: " +
                           std::string([[error localizedDescription] UTF8String]));
                return false;
            }
            id<MTLFunction> func = [library newFunctionWithName:@"letterbox_composite"];
            if (!func) {
                Debug::Log("MetalLetterboxComposite: Function not found");
                return false;
            }
            g_letterbox_pipeline = [device newComputePipelineStateWithFunction:func error:&error];
            if (!g_letterbox_pipeline) {
                Debug::Log("MetalLetterboxComposite: Pipeline creation failed");
                return false;
            }
            Debug::Log("MetalLetterboxComposite: Compute pipeline created");
        }

        id<MTLTexture> srcMTL = (__bridge id<MTLTexture>)src_texture;
        id<MTLTexture> dstMTL = (__bridge id<MTLTexture>)dst_texture;

        id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

        [encoder setComputePipelineState:g_letterbox_pipeline];
        [encoder setTexture:srcMTL atIndex:0];
        [encoder setTexture:dstMTL atIndex:1];

        struct {
            uint32_t canvas_w, canvas_h;
            float rect_x, rect_y, rect_w, rect_h;
        } params = {
            (uint32_t)canvas_w, (uint32_t)canvas_h,
            rect_x, rect_y, rect_w, rect_h
        };
        [encoder setBytes:&params length:sizeof(params) atIndex:0];

        MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
        MTLSize gridSize = MTLSizeMake(canvas_w, canvas_h, 1);
        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

        [encoder endEncoding];
        [cmdBuf commit];
        // No waitUntilCompleted — Metal command queue ordering guarantees
        // subsequent GPU reads (OCIO, ImGui render) see completed data.
        // This was blocking the main thread for ~60ms every frame.

        return true;
    }
}

#endif // __APPLE__
