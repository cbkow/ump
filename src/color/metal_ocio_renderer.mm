#ifdef __APPLE__

#import <Metal/Metal.h>

#include "metal_ocio_renderer.h"
#include "../gpu/metal_device_manager.h"
#include "../gpu/metal_texture_pool.h"
#include "../utils/debug_utils.h"
#include "ocio_pipeline.h"

#include <functional>
#include <sstream>

namespace qcview {

MetalOCIORenderer::MetalOCIORenderer() = default;

MetalOCIORenderer::~MetalOCIORenderer() {
    Shutdown();
}

bool MetalOCIORenderer::Initialize() {
    if (initialized_) return true;

    auto& mgr = MetalDeviceManager::Instance();
    if (!mgr.IsInitialized()) return false;

    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    // Create passthrough compute pipeline
    NSString* source = @R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void passthrough(
            texture2d<float, access::read> in_tex [[texture(0)]],
            texture2d<float, access::write> out_tex [[texture(1)]],
            uint2 gid [[thread_position_in_grid]])
        {
            if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
            out_tex.write(in_tex.read(gid), gid);
        }
    )";

    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:source options:nil error:&error];
    if (lib) {
        id<MTLFunction> func = [lib newFunctionWithName:@"passthrough"];
        if (func) {
            id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:func error:&error];
            if (pso) {
                passthrough_pipeline_ = (__bridge_retained void*)pso;
            }
        }
    }

    // Create linear-to-sRGB compute pipeline (for EDR pre-encoding)
    NSString* linearToSrgbSource = @R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void linear_to_srgb(
            texture2d<float, access::read> in_tex [[texture(0)]],
            texture2d<float, access::write> out_tex [[texture(1)]],
            uint2 gid [[thread_position_in_grid]])
        {
            if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
            float4 pixel = in_tex.read(gid);
            // Linear to sRGB transfer function
            float3 linear = pixel.rgb;
            float3 srgb = select(linear * 12.92f,
                                 1.055f * pow(linear, float3(1.0f / 2.4f)) - 0.055f,
                                 linear >= float3(0.0031308f));
            out_tex.write(float4(srgb, pixel.a), gid);
        }
    )";

    id<MTLLibrary> srgbLib = [device newLibraryWithSource:linearToSrgbSource options:nil error:&error];
    if (srgbLib) {
        id<MTLFunction> srgbFunc = [srgbLib newFunctionWithName:@"linear_to_srgb"];
        if (srgbFunc) {
            id<MTLComputePipelineState> srgbPso = [device newComputePipelineStateWithFunction:srgbFunc error:&error];
            if (srgbPso) {
                linear_to_srgb_pipeline_ = (__bridge_retained void*)srgbPso;
            }
        }
    }

    // Create shared linear sampler (for LUT texture sampling)
    MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDesc.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDesc];
    if (sampler) {
        linear_sampler_ = (__bridge_retained void*)sampler;
    }

    initialized_ = true;
    Debug::Log("MetalOCIORenderer: Initialized");
    return true;
}

void MetalOCIORenderer::Shutdown() {
    if (!initialized_) return;

    InvalidateCache();

    if (passthrough_pipeline_) {
        CFRelease(passthrough_pipeline_);
        passthrough_pipeline_ = nullptr;
    }

    if (linear_to_srgb_pipeline_) {
        CFRelease(linear_to_srgb_pipeline_);
        linear_to_srgb_pipeline_ = nullptr;
    }

    if (linear_sampler_) {
        CFRelease(linear_sampler_);
        linear_sampler_ = nullptr;
    }

    // Release persistent output textures (MRC — project convention, see
    // metal_texture_pool.mm DestroyTexture and annotation_thumbnail_metal.mm).
    if (output_texture_) {
        [(id)output_texture_ release];
        output_texture_ = nullptr;
        output_width_ = output_height_ = 0;
    }
    if (srgb_output_texture_) {
        [(id)srgb_output_texture_ release];
        srgb_output_texture_ = nullptr;
        srgb_output_width_ = srgb_output_height_ = 0;
    }

    initialized_ = false;
    Debug::Log("MetalOCIORenderer: Shutdown");
}

void MetalOCIORenderer::InvalidateCache() {
    if (compute_pipeline_) {
        CFRelease(compute_pipeline_);
        compute_pipeline_ = nullptr;
    }
    cached_shader_hash_.clear();

    for (auto& lut : lut_textures_) {
        if (lut.texture) {
            CFRelease(lut.texture);
            lut.texture = nullptr;
        }
    }
    lut_textures_.clear();
}

bool MetalOCIORenderer::BuildPipelineForOCIO(OCIOPipeline* pipeline) {
    if (!pipeline || !initialized_) return false;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    // Get OCIO MSL shader info
    OCIOPipeline::MetalShaderInfo info;
    if (!pipeline->GenerateMetalShaderInfo(info) || !info.valid) {
        Debug::Log("MetalOCIORenderer: Failed to generate Metal shader info");
        return false;
    }

    // Check if we already have a pipeline for this shader
    std::hash<std::string> hasher;
    std::string shader_hash = std::to_string(hasher(info.ocio_function_msl));
    if (compute_pipeline_ && cached_shader_hash_ == shader_hash) {
        return true;  // Already built
    }

    // Invalidate old pipeline and LUTs
    InvalidateCache();

    // Create LUT textures
    for (size_t i = 0; i < info.luts.size(); ++i) {
        const auto& lut_info = info.luts[i];
        LUTTexture lut_entry;
        lut_entry.is_3d = lut_info.is_3d;

        if (lut_info.is_3d) {
            // 3D LUT: MTLTextureType3D, RGBA32Float
            MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
            desc.textureType = MTLTextureType3D;
            desc.pixelFormat = MTLPixelFormatRGBA32Float;
            desc.width = lut_info.edge_len;
            desc.height = lut_info.edge_len;
            desc.depth = lut_info.edge_len;
            desc.storageMode = MTLStorageModeShared;
            desc.usage = MTLTextureUsageShaderRead;

            id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
            if (!tex) {
                Debug::Log("MetalOCIORenderer: Failed to create 3D LUT texture");
                return false;
            }

            // Convert RGB → RGBA
            unsigned edge = lut_info.edge_len;
            std::vector<float> rgba_data(edge * edge * edge * 4);
            for (unsigned z = 0; z < edge; ++z) {
                for (unsigned y = 0; y < edge; ++y) {
                    for (unsigned x = 0; x < edge; ++x) {
                        unsigned src_idx = 3 * (x + edge * (y + edge * z));
                        unsigned dst_idx = 4 * (x + edge * (y + edge * z));
                        rgba_data[dst_idx + 0] = lut_info.data[src_idx + 0];
                        rgba_data[dst_idx + 1] = lut_info.data[src_idx + 1];
                        rgba_data[dst_idx + 2] = lut_info.data[src_idx + 2];
                        rgba_data[dst_idx + 3] = 1.0f;
                    }
                }
            }

            MTLRegion region = MTLRegionMake3D(0, 0, 0, edge, edge, edge);
            [tex replaceRegion:region
                   mipmapLevel:0
                         slice:0
                     withBytes:rgba_data.data()
                   bytesPerRow:edge * 4 * sizeof(float)
                 bytesPerImage:edge * edge * 4 * sizeof(float)];

            lut_entry.texture = (__bridge_retained void*)tex;
            Debug::Log("MetalOCIORenderer: Created 3D LUT '" + lut_info.sampler_name +
                       "' " + std::to_string(edge) + "^3");
        } else {
            unsigned w = lut_info.width;
            unsigned h = std::max(lut_info.height, 1u);
            bool is_1d = (h == 1);

            MTLPixelFormat format;
            if (lut_info.is_red_channel) {
                format = MTLPixelFormatR32Float;
            } else {
                format = MTLPixelFormatRGBA32Float;
            }

            MTLTextureDescriptor* desc;
            if (is_1d) {
                // True 1D texture — OCIO MSL expects texture1d<float>
                desc = [MTLTextureDescriptor new];
                desc.textureType = MTLTextureType1D;
                desc.pixelFormat = format;
                desc.width = w;
                desc.height = 1;
                desc.depth = 1;
            } else {
                desc = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
            }
            desc.storageMode = MTLStorageModeShared;
            desc.usage = MTLTextureUsageShaderRead;

            id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
            if (!tex) {
                Debug::Log("MetalOCIORenderer: Failed to create " +
                           std::string(is_1d ? "1D" : "2D") + " LUT texture");
                return false;
            }

            if (lut_info.is_red_channel) {
                MTLRegion region = is_1d ? MTLRegionMake1D(0, w) : MTLRegionMake2D(0, 0, w, h);
                [tex replaceRegion:region
                       mipmapLevel:0
                         withBytes:lut_info.data.data()
                       bytesPerRow:w * sizeof(float)];
            } else {
                unsigned total_pixels = w * h;
                std::vector<float> rgba_data(total_pixels * 4);
                for (unsigned j = 0; j < total_pixels; ++j) {
                    rgba_data[j * 4 + 0] = lut_info.data[j * 3 + 0];
                    rgba_data[j * 4 + 1] = lut_info.data[j * 3 + 1];
                    rgba_data[j * 4 + 2] = lut_info.data[j * 3 + 2];
                    rgba_data[j * 4 + 3] = 1.0f;
                }
                MTLRegion region = is_1d ? MTLRegionMake1D(0, w) : MTLRegionMake2D(0, 0, w, h);
                [tex replaceRegion:region
                       mipmapLevel:0
                         withBytes:rgba_data.data()
                       bytesPerRow:w * 4 * sizeof(float)];
            }

            lut_entry.texture = (__bridge_retained void*)tex;
            Debug::Log("MetalOCIORenderer: Created " + std::string(is_1d ? "1D" : "2D") +
                       " LUT '" + lut_info.sampler_name + "' " +
                       std::to_string(w) + (is_1d ? "" : "x" + std::to_string(h)) +
                       " " + (lut_info.is_red_channel ? "R32F" : "RGBA32F"));
        }

        lut_textures_.push_back(lut_entry);
    }

    // Build the compute kernel MSL source
    // OCIO MSL 2.0 generates a class wrapper: struct ocio_OCIODisplay { OCIODisplay(float4) ... }
    // The constructor takes LUT textures + samplers. We construct an instance and call the method.
    std::stringstream msl;
    msl << "#include <metal_stdlib>\n";
    msl << "using namespace metal;\n\n";

    // Insert OCIO-generated MSL code (contains ocio_OCIODisplay struct with OCIODisplay method)
    msl << info.ocio_function_msl << "\n\n";

    // Build compute kernel signature
    msl << "kernel void ocio_color_transform(\n";
    msl << "    texture2d<float, access::read> in_tex [[texture(0)]],\n";
    msl << "    texture2d<float, access::write> out_tex [[texture(1)]]";

    // Add LUT texture and sampler arguments using OCIO's exact names
    for (size_t i = 0; i < info.luts.size(); ++i) {
        const auto& lut = info.luts[i];
        if (lut.is_3d) {
            msl << ",\n    texture3d<float> " << lut.texture_name << " [[texture(" << (i + 2) << ")]]";
        } else if (lut.height <= 1) {
            msl << ",\n    texture1d<float> " << lut.texture_name << " [[texture(" << (i + 2) << ")]]";
        } else {
            msl << ",\n    texture2d<float> " << lut.texture_name << " [[texture(" << (i + 2) << ")]]";
        }
        msl << ",\n    sampler " << lut.sampler_name << " [[sampler(" << i << ")]]";
    }

    msl << ",\n    uint2 gid [[thread_position_in_grid]])\n";
    msl << "{\n";
    msl << "    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;\n";
    msl << "    float4 pixel = in_tex.read(gid);\n";

    // OCIO MSL 2.0 always generates a class wrapper "ocio_OCIODisplay"
    // Constructor takes LUT textures + samplers; OCIODisplay(float4) is a member method
    if (info.luts.empty()) {
        // No LUTs: constructor takes no arguments
        msl << "    ocio_OCIODisplay ocio;\n";
    } else {
        msl << "    ocio_OCIODisplay ocio(";
        for (size_t i = 0; i < info.luts.size(); ++i) {
            if (i > 0) msl << ", ";
            msl << info.luts[i].texture_name << ", " << info.luts[i].sampler_name;
        }
        msl << ");\n";
    }
    msl << "    float4 result = ocio.OCIODisplay(pixel);\n";

    msl << "    out_tex.write(result, gid);\n";
    msl << "}\n";

    std::string msl_source = msl.str();

    // Compile MSL
    NSString* nsSource = [NSString stringWithUTF8String:msl_source.c_str()];
    NSError* compileError = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:nsSource options:nil error:&compileError];
    if (!lib) {
        std::string err_msg = compileError ? [[compileError localizedDescription] UTF8String] : "Unknown error";
        Debug::Log("MetalOCIORenderer: MSL compilation failed: " + err_msg);
        // Log first 500 chars of source for debugging
        if (msl_source.length() > 0) {
            Debug::Log("MetalOCIORenderer: MSL preview: " +
                       msl_source.substr(0, std::min(size_t(500), msl_source.length())));
        }
        return false;
    }

    id<MTLFunction> func = [lib newFunctionWithName:@"ocio_color_transform"];
    if (!func) {
        Debug::Log("MetalOCIORenderer: Could not find ocio_color_transform function");
        return false;
    }

    id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:func error:&compileError];
    if (!pso) {
        std::string err_msg = compileError ? [[compileError localizedDescription] UTF8String] : "Unknown error";
        Debug::Log("MetalOCIORenderer: Pipeline state creation failed: " + err_msg);
        return false;
    }

    compute_pipeline_ = (__bridge_retained void*)pso;
    cached_shader_hash_ = shader_hash;

    Debug::Log("MetalOCIORenderer: OCIO pipeline built successfully (" +
               std::to_string(info.luts.size()) + " LUT(s))");
    return true;
}

void MetalOCIORenderer::EnsureOutputTexture(int width, int height) {
    if (output_texture_ && output_width_ == width && output_height_ == height)
        return;

    if (output_texture_) {
        [(id)output_texture_ release];
        output_texture_ = nullptr;
    }

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    if (!device) return;

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    if (!tex) {
        Debug::Log("MetalOCIORenderer: EnsureOutputTexture failed "
                   + std::to_string(width) + "x" + std::to_string(height));
        return;
    }
    output_texture_ = (void*)tex;  // MRC: +1 retained from newTextureWithDescriptor
    output_width_ = width;
    output_height_ = height;
}

void MetalOCIORenderer::EnsureSRGBOutputTexture(int width, int height) {
    if (srgb_output_texture_ && srgb_output_width_ == width && srgb_output_height_ == height)
        return;

    if (srgb_output_texture_) {
        [(id)srgb_output_texture_ release];
        srgb_output_texture_ = nullptr;
    }

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    if (!device) return;

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
    if (!tex) {
        Debug::Log("MetalOCIORenderer: EnsureSRGBOutputTexture failed "
                   + std::to_string(width) + "x" + std::to_string(height));
        return;
    }
    srgb_output_texture_ = (void*)tex;
    srgb_output_width_ = width;
    srgb_output_height_ = height;
}

void MetalOCIORenderer::DispatchCompute(void* pipeline_state, void* src_texture, void* dst_texture,
                                         int width, int height) {
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)pipeline_state;
    [encoder setComputePipelineState:pso];
    [encoder setTexture:(__bridge id<MTLTexture>)src_texture atIndex:0];
    [encoder setTexture:(__bridge id<MTLTexture>)dst_texture atIndex:1];

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];
    // Fire-and-forget: Metal guarantees command buffer ordering within a queue.
    // ImGui render pass will naturally wait for this to finish.
    [cmdBuf commit];
}

void* MetalOCIORenderer::Apply(OCIOPipeline* pipeline, uint64_t source_pool_id,
                                int width, int height) {
    if (!initialized_ || !pipeline) return nullptr;

    // Passthrough: no GPU work needed, return source MTLTexture directly
    if (pipeline->IsPassthrough()) {
        return ApplyPassthrough(source_pool_id, width, height);
    }

    // Lazy-build the OCIO compute pipeline
    if (!compute_pipeline_) {
        if (!BuildPipelineForOCIO(pipeline)) {
            Debug::Log("MetalOCIORenderer: Pipeline build failed, falling back to passthrough");
            return ApplyPassthrough(source_pool_id, width, height);
        }
    }

    auto& pool = MetalTexturePool::Instance();
    const MetalTexture* src = pool.GetTexture(source_pool_id);
    if (!src || !src->texture) return nullptr;

    // Reuse persistent output texture (only reallocate on dimension change).
    // Owned directly by this renderer — not in MetalTexturePool, cannot be evicted.
    EnsureOutputTexture(width, height);
    if (!output_texture_) return nullptr;

    // Record and submit OCIO compute pass with LUT bindings
    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmdBuf computeCommandEncoder];

    id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)compute_pipeline_;
    [encoder setComputePipelineState:pso];
    [encoder setTexture:(__bridge id<MTLTexture>)src->texture atIndex:0];
    [encoder setTexture:(id)output_texture_ atIndex:1];

    // Bind LUT textures and samplers
    id<MTLSamplerState> sampler = (__bridge id<MTLSamplerState>)linear_sampler_;
    for (size_t i = 0; i < lut_textures_.size(); ++i) {
        [encoder setTexture:(__bridge id<MTLTexture>)lut_textures_[i].texture atIndex:(i + 2)];
        [encoder setSamplerState:sampler atIndex:i];
    }

    MTLSize threadGroupSize = MTLSizeMake(16, 16, 1);
    MTLSize gridSize = MTLSizeMake(width, height, 1);
    [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];

    [encoder endEncoding];
    [cmdBuf commit];
    // No waitUntilCompleted — GPU ordering within the queue guarantees
    // this finishes before the ImGui render pass reads the texture.

    return output_texture_;
}

void* MetalOCIORenderer::ApplyPassthrough(uint64_t source_pool_id, int /*width*/, int /*height*/) {
    // No GPU work: resolve the pool id to its MTLTexture pointer. The pool
    // retains ownership; caller must not release.
    auto& pool = MetalTexturePool::Instance();
    const MetalTexture* src = pool.GetTexture(source_pool_id);
    if (!src || !src->texture) return nullptr;
    return src->texture;
}

void* MetalOCIORenderer::ApplyLinearToSRGB(void* source_mtl_texture, int width, int height) {
    if (!linear_to_srgb_pipeline_ || !source_mtl_texture) return nullptr;

    EnsureSRGBOutputTexture(width, height);
    if (!srgb_output_texture_) return nullptr;

    DispatchCompute(linear_to_srgb_pipeline_, source_mtl_texture, srgb_output_texture_, width, height);
    return srgb_output_texture_;
}

uint64_t MetalOCIORenderer::CopyTextureSync(void* source_mtl_texture, int width, int height) {
    if (!source_mtl_texture) return 0;

    auto& pool = MetalTexturePool::Instance();
    uint64_t out_id = pool.CreateEmptyTexture(width, height, 1);
    const MetalTexture* dst = pool.GetTexture(out_id);
    if (!dst || !dst->texture) return 0;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mgr.GetCommandQueue();
    id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
    [blit copyFromTexture:(id)source_mtl_texture
              sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                toTexture:(__bridge id<MTLTexture>)dst->texture
         destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cmdBuf commit];
    [cmdBuf waitUntilCompleted];

    return out_id;
}

} // namespace qcview

#endif // __APPLE__
