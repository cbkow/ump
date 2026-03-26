#ifdef __APPLE__

#import <Metal/Metal.h>

#include "metal_annotation_renderer.h"
#include "../gpu/metal_device_manager.h"
#include "../gpu/metal_texture_pool.h"
#include "../utils/debug_utils.h"
#include "viewport_annotator.h"
#include "../annotations/stroke_tessellator.h"
#include <imgui.h>

static const char* kAnnotationShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut annotation_vertex(VertexIn in [[stage_in]],
                                    constant float4x4& mvp [[buffer(1)]]) {
    VertexOut out;
    out.position = mvp * float4(in.position, 0.0, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 annotation_fragment(VertexOut in [[stage_in]]) {
    return in.color;
}

kernel void unpremultiply_alpha(
    texture2d<float, access::read_write> tex [[texture(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= tex.get_width() || gid.y >= tex.get_height()) return;
    float4 c = tex.read(gid);
    if (c.a > 0.001) { c.rgb /= c.a; }
    tex.write(c, gid);
}
)";

namespace qcview {
namespace Annotations {

MetalAnnotationRenderer::MetalAnnotationRenderer() = default;

MetalAnnotationRenderer::~MetalAnnotationRenderer() {
    Shutdown();
}

bool MetalAnnotationRenderer::Initialize() {
    if (initialized_) return true;

    if (!CreateRenderPipeline()) {
        Debug::Log("MetalAnnotationRenderer: Failed to create render pipeline");
        return false;
    }

    if (!CreateExportPipeline()) {
        Debug::Log("MetalAnnotationRenderer: Failed to create export pipeline");
        return false;
    }

    if (!CreateExportPipelineRGBA8()) {
        Debug::Log("MetalAnnotationRenderer: Failed to create RGBA8 export pipeline");
        return false;
    }

    if (!CreateUnpremultiplyPipeline()) {
        Debug::Log("MetalAnnotationRenderer: Failed to create unpremultiply pipeline");
        return false;
    }

    // Create initial vertex buffer (1 MB, will grow as needed)
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    vertex_buffer_size_ = 1024 * 1024;  // 1 MB
    id<MTLBuffer> buffer = [device newBufferWithLength:vertex_buffer_size_
                                              options:MTLResourceStorageModeShared];
    if (buffer) {
        vertex_buffer_ = (__bridge_retained void*)buffer;
    }

    initialized_ = true;
    Debug::Log("MetalAnnotationRenderer: Initialized");
    return true;
}

void MetalAnnotationRenderer::Shutdown() {
    if (!initialized_) return;

    if (output_pool_id_ != 0) {
        MetalTexturePool::Instance().QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
    }

    if (render_pipeline_) {
        CFRelease(render_pipeline_);
        render_pipeline_ = nullptr;
    }
    if (export_pipeline_) {
        CFRelease(export_pipeline_);
        export_pipeline_ = nullptr;
    }
    if (export_pipeline_rgba8_) {
        CFRelease(export_pipeline_rgba8_);
        export_pipeline_rgba8_ = nullptr;
    }
    if (unpremul_pipeline_) {
        CFRelease(unpremul_pipeline_);
        unpremul_pipeline_ = nullptr;
    }
    if (vertex_buffer_) {
        CFRelease(vertex_buffer_);
        vertex_buffer_ = nullptr;
    }
    if (msaa_texture_) {
        CFRelease(msaa_texture_);
        msaa_texture_ = nullptr;
    }
    if (resolve_texture_) {
        CFRelease(resolve_texture_);
        resolve_texture_ = nullptr;
    }

    cached_width_ = 0;
    cached_height_ = 0;
    initialized_ = false;
    Debug::Log("MetalAnnotationRenderer: Shutdown");
}

bool MetalAnnotationRenderer::CreateRenderPipeline() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kAnnotationShaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        Debug::Log("MetalAnnotationRenderer: Shader compilation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"annotation_vertex"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"annotation_fragment"];
    if (!vertFunc || !fragFunc) return false;

    // Vertex descriptor
    MTLVertexDescriptor* vertDesc = [MTLVertexDescriptor new];
    vertDesc.attributes[0].format = MTLVertexFormatFloat2;
    vertDesc.attributes[0].offset = 0;
    vertDesc.attributes[0].bufferIndex = 0;
    vertDesc.attributes[1].format = MTLVertexFormatFloat4;
    vertDesc.attributes[1].offset = sizeof(float) * 2;
    vertDesc.attributes[1].bufferIndex = 0;
    vertDesc.layouts[0].stride = sizeof(float) * 6;  // float2 pos + float4 color
    vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
    pipeDesc.vertexFunction = vertFunc;
    pipeDesc.fragmentFunction = fragFunc;
    pipeDesc.vertexDescriptor = vertDesc;
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.rasterSampleCount = 4;  // MSAA 4x

    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!pso) {
        Debug::Log("MetalAnnotationRenderer: Pipeline creation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    render_pipeline_ = (__bridge_retained void*)pso;
    return true;
}

bool MetalAnnotationRenderer::CreateExportPipeline() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kAnnotationShaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return false;

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"annotation_vertex"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"annotation_fragment"];
    if (!vertFunc || !fragFunc) return false;

    MTLVertexDescriptor* vertDesc = [MTLVertexDescriptor new];
    vertDesc.attributes[0].format = MTLVertexFormatFloat2;
    vertDesc.attributes[0].offset = 0;
    vertDesc.attributes[0].bufferIndex = 0;
    vertDesc.attributes[1].format = MTLVertexFormatFloat4;
    vertDesc.attributes[1].offset = sizeof(float) * 2;
    vertDesc.attributes[1].bufferIndex = 0;
    vertDesc.layouts[0].stride = sizeof(float) * 6;
    vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
    pipeDesc.vertexFunction = vertFunc;
    pipeDesc.fragmentFunction = fragFunc;
    pipeDesc.vertexDescriptor = vertDesc;
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.rasterSampleCount = 1;  // No MSAA for export

    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!pso) {
        Debug::Log("MetalAnnotationRenderer: Export pipeline creation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    export_pipeline_ = (__bridge_retained void*)pso;
    return true;
}

bool MetalAnnotationRenderer::CreateExportPipelineRGBA8() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kAnnotationShaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return false;

    id<MTLFunction> vertFunc = [library newFunctionWithName:@"annotation_vertex"];
    id<MTLFunction> fragFunc = [library newFunctionWithName:@"annotation_fragment"];
    if (!vertFunc || !fragFunc) return false;

    MTLVertexDescriptor* vertDesc = [MTLVertexDescriptor new];
    vertDesc.attributes[0].format = MTLVertexFormatFloat2;
    vertDesc.attributes[0].offset = 0;
    vertDesc.attributes[0].bufferIndex = 0;
    vertDesc.attributes[1].format = MTLVertexFormatFloat4;
    vertDesc.attributes[1].offset = sizeof(float) * 2;
    vertDesc.attributes[1].bufferIndex = 0;
    vertDesc.layouts[0].stride = sizeof(float) * 6;
    vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
    pipeDesc.vertexFunction = vertFunc;
    pipeDesc.fragmentFunction = fragFunc;
    pipeDesc.vertexDescriptor = vertDesc;
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.rasterSampleCount = 1;

    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!pso) {
        Debug::Log("MetalAnnotationRenderer: RGBA8 export pipeline creation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    export_pipeline_rgba8_ = (__bridge_retained void*)pso;
    return true;
}

bool MetalAnnotationRenderer::CreateUnpremultiplyPipeline() {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kAnnotationShaderSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return false;

    id<MTLFunction> kernelFunc = [library newFunctionWithName:@"unpremultiply_alpha"];
    if (!kernelFunc) {
        Debug::Log("MetalAnnotationRenderer: unpremultiply_alpha kernel not found");
        return false;
    }

    id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:kernelFunc error:&error];
    if (!pso) {
        Debug::Log("MetalAnnotationRenderer: Compute pipeline creation failed: " +
                   std::string([error.localizedDescription UTF8String]));
        return false;
    }

    unpremul_pipeline_ = (__bridge_retained void*)pso;
    return true;
}

bool MetalAnnotationRenderer::EnsureVertexBuffer(size_t required_bytes) {
    if (vertex_buffer_ && vertex_buffer_size_ >= required_bytes) return true;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    // Double until it fits
    size_t new_size = vertex_buffer_size_ > 0 ? vertex_buffer_size_ : 1024 * 1024;
    while (new_size < required_bytes) new_size *= 2;

    id<MTLBuffer> new_buffer = [device newBufferWithLength:new_size
                                                   options:MTLResourceStorageModeShared];
    if (!new_buffer) return false;

    if (vertex_buffer_) CFRelease(vertex_buffer_);
    vertex_buffer_ = (__bridge_retained void*)new_buffer;
    vertex_buffer_size_ = new_size;
    return true;
}

bool MetalAnnotationRenderer::EnsureRenderTargets(int width, int height) {
    if (msaa_texture_ && resolve_texture_ && cached_width_ == width && cached_height_ == height)
        return true;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    // Release old textures
    if (msaa_texture_) { CFRelease(msaa_texture_); msaa_texture_ = nullptr; }
    if (resolve_texture_) { CFRelease(resolve_texture_); resolve_texture_ = nullptr; }

    // Queue-delete old pool entry since dimensions changed
    if (output_pool_id_ != 0) {
        MetalTexturePool::Instance().QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
    }

    // MSAA texture (4x, private storage, render target only)
    MTLTextureDescriptor* msaaDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                        width:width
                                                                                       height:height
                                                                                    mipmapped:NO];
    msaaDesc.textureType = MTLTextureType2DMultisample;
    msaaDesc.sampleCount = 4;
    msaaDesc.storageMode = MTLStorageModePrivate;
    msaaDesc.usage = MTLTextureUsageRenderTarget;

    id<MTLTexture> msaa = [device newTextureWithDescriptor:msaaDesc];
    if (!msaa) return false;
    msaa_texture_ = (__bridge_retained void*)msaa;

    // Resolve texture (1x, shared storage for pool registration + compute shader write)
    MTLTextureDescriptor* resolveDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                           width:width
                                                                                          height:height
                                                                                       mipmapped:NO];
    resolveDesc.textureType = MTLTextureType2D;
    resolveDesc.sampleCount = 1;
    resolveDesc.storageMode = MTLStorageModeShared;
    resolveDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;

    id<MTLTexture> resolve = [device newTextureWithDescriptor:resolveDesc];
    if (!resolve) return false;
    resolve_texture_ = (__bridge_retained void*)resolve;

    cached_width_ = width;
    cached_height_ = height;
    return true;
}

uint64_t MetalAnnotationRenderer::RenderAnnotations(
    const std::vector<ActiveStroke>& strokes,
    int video_width, int video_height,
    float display_x, float display_y,
    float display_width, float display_height,
    float line_width_scale,
    bool use_smoothing) {

    if (!initialized_ || strokes.empty()) return 0;

    // On Retina displays, ImGui display sizes are in logical points but Metal
    // textures need to be in physical pixels. Scale up by framebuffer scale.
    float fb_scale = ImGui::GetIO().DisplayFramebufferScale.x;
    if (fb_scale < 1.0f) fb_scale = 1.0f;

    // Tessellate at Retina resolution: scale display dimensions and line width
    TessellatedMesh combined;
    for (const auto& stroke : strokes) {
        TessellatedMesh mesh = StrokeTessellator::Tessellate(
            stroke, display_x * fb_scale, display_y * fb_scale,
            display_width * fb_scale, display_height * fb_scale,
            line_width_scale * fb_scale, use_smoothing);
        combined.vertices.insert(combined.vertices.end(),
                                  mesh.vertices.begin(), mesh.vertices.end());
    }

    if (combined.vertices.empty()) return 0;

    int w = (int)(display_width * fb_scale);
    int h = (int)(display_height * fb_scale);
    if (w <= 0 || h <= 0) return 0;

    if (!EnsureRenderTargets(w, h)) return 0;

    size_t vb_bytes = combined.vertices.size() * sizeof(TessVertex);
    if (!EnsureVertexBuffer(vb_bytes)) return 0;

    // Upload vertices
    id<MTLBuffer> vbuf = (__bridge id<MTLBuffer>)vertex_buffer_;
    memcpy(vbuf.contents, combined.vertices.data(), vb_bytes);

    // Orthographic MVP: pixel coords → NDC (column-major for Metal float4x4)
    // Metal NDC has Y pointing up, but tessellator produces Y-down screen coords,
    // so negate Y to flip the vertical axis.
    float mvp[16] = {
         2.0f / w,  0,          0, 0,
         0,        -2.0f / h,   0, 0,
         0,         0,          1, 0,
        -1.0f,      1.0f,       0, 1
    };

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    if (!cmdBuf) return 0;

    // MSAA render pass
    MTLRenderPassDescriptor* rpDesc = [MTLRenderPassDescriptor new];
    rpDesc.colorAttachments[0].texture = (__bridge id<MTLTexture>)msaa_texture_;
    rpDesc.colorAttachments[0].resolveTexture = (__bridge id<MTLTexture>)resolve_texture_;
    rpDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpDesc.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rpDesc.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;

    id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:rpDesc];
    if (!enc) return 0;

    [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)render_pipeline_];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:mvp length:sizeof(mvp) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combined.vertices.size()];
    [enc endEncoding];

    // Un-premultiply compute pass
    id<MTLComputeCommandEncoder> compEnc = [cmdBuf computeCommandEncoder];
    [compEnc setComputePipelineState:(__bridge id<MTLComputePipelineState>)unpremul_pipeline_];
    [compEnc setTexture:(__bridge id<MTLTexture>)resolve_texture_ atIndex:0];

    MTLSize threadgroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1);
    [compEnc dispatchThreadgroups:gridSize threadsPerThreadgroup:threadgroupSize];
    [compEnc endEncoding];

    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    // Register resolve texture in pool if needed
    if (output_pool_id_ == 0) {
        // Retain before handing to pool (pool takes ownership)
        CFRetain(resolve_texture_);
        output_pool_id_ = MetalTexturePool::Instance().RegisterExistingTexture(
            resolve_texture_, w, h, 1 /* RGBA16Float */);
    }

    return output_pool_id_;
}

void MetalAnnotationRenderer::RenderAnnotationsToImage(
    const std::vector<ActiveStroke>& strokes,
    void* target_texture,
    int width, int height,
    float line_width_scale) {

    if (!initialized_ || !target_texture || strokes.empty()) return;

    // Tessellate all strokes
    TessellatedMesh combined;
    for (const auto& stroke : strokes) {
        TessellatedMesh mesh = StrokeTessellator::Tessellate(
            stroke, 0.0f, 0.0f, (float)width, (float)height,
            line_width_scale, true);
        combined.vertices.insert(combined.vertices.end(),
                                  mesh.vertices.begin(), mesh.vertices.end());
    }

    if (combined.vertices.empty()) return;

    size_t vb_bytes = combined.vertices.size() * sizeof(TessVertex);
    if (!EnsureVertexBuffer(vb_bytes)) return;

    id<MTLBuffer> vbuf = (__bridge id<MTLBuffer>)vertex_buffer_;
    memcpy(vbuf.contents, combined.vertices.data(), vb_bytes);

    // Y-flipped orthographic projection (Metal NDC Y-up, tessellator Y-down)
    float mvp[16] = {
         2.0f / width,  0,               0, 0,
         0,            -2.0f / height,   0, 0,
         0,             0,               1, 0,
        -1.0f,          1.0f,            0, 1
    };

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    if (!cmdBuf) return;

    // Render directly to target texture (no MSAA, load existing content)
    MTLRenderPassDescriptor* rpDesc = [MTLRenderPassDescriptor new];
    rpDesc.colorAttachments[0].texture = (__bridge id<MTLTexture>)target_texture;
    rpDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rpDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:rpDesc];
    if (!enc) return;

    // Select pipeline matching target texture pixel format
    id<MTLTexture> target_mtl = (__bridge id<MTLTexture>)target_texture;
    void* pipeline = (target_mtl.pixelFormat == MTLPixelFormatRGBA8Unorm)
                     ? export_pipeline_rgba8_ : export_pipeline_;
    [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pipeline];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:mvp length:sizeof(mvp) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:combined.vertices.size()];
    [enc endEncoding];

    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];
}

} // namespace Annotations
} // namespace qcview

#endif // __APPLE__
