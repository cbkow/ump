// MetalOcioRenderer — see header for adaptation notes.

#include "metal_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QFile>
#include <QtLogging>

#include <algorithm>
#include <cstring>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

namespace {

// Compute-kernel template that wraps the OCIO function. Bindings:
//   texture(0)  = source (RGBA16F, decoded frame)
//   texture(1)  = output (RGBA16F, post-OCIO)
//   texture(2..) = OCIO LUTs (1D promoted to 2D, 2D, 3D), in OCIO's order
// OCIO's emitted MSL function takes a sampler set as its argument
// list. We declare all the bindings + samplers in the kernel and
// call OCIODisplay() with them.
//
// The wrapper is constructed at rebuild() time after we know how
// many LUTs OCIO needs and their types/sampler names. We compose
// the binding declarations + the call site dynamically.
constexpr const char *kKernelHeader = R"(
#include <metal_stdlib>
using namespace metal;

)";

constexpr const char *kKernelMainPrefix = R"(
kernel void ocio_apply(
    texture2d<float, access::sample>  src     [[texture(0)]],
    texture2d<float, access::write>   dst     [[texture(1)]],
    sampler                           src_smp [[sampler(0)]],
)";

constexpr const char *kKernelMainSuffix = R"(
    uint2 gid                                  [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 uv = (float2(gid) + 0.5) / float2(dst.get_width(), dst.get_height());
    float4 color = src.sample(src_smp, uv);
)";

constexpr const char *kKernelFooter = R"(
    dst.write(color, gid);
}
)";

} // namespace

struct MetalOcioRenderer::Impl {
    id<MTLDevice>               device   = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLSamplerState>         sampler  = nil;

    // 1D/2D LUTs (sampler2D in MSL — we promote 1D to 2D Nx1).
    std::vector<id<MTLTexture>>  lut2dTextures;
    std::vector<id<MTLSamplerState>> lut2dSamplers;
    // 3D LUTs (sampler3D).
    std::vector<id<MTLTexture>>  lut3dTextures;
    std::vector<id<MTLSamplerState>> lut3dSamplers;

    id<MTLTexture> outputTex = nil;
    int            outputW   = 0;
    int            outputH   = 0;

    int     lastChainGeneration = -1;
    QString lastError;
};

MetalOcioRenderer::MetalOcioRenderer()
    : m_impl(new Impl())
{
}

MetalOcioRenderer::~MetalOcioRenderer()
{
    shutdown();
    delete m_impl;
}

bool MetalOcioRenderer::initialize()
{
    auto &mgr = MetalDeviceManager::instance();
    m_impl->device = (__bridge id<MTLDevice>)mgr.device();
    if (!m_impl->device) {
        qWarning("MetalOcioRenderer: no Metal device");
        return false;
    }

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter   = MTLSamplerMinMagFilterLinear;
    sd.magFilter   = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m_impl->sampler = [m_impl->device newSamplerStateWithDescriptor:sd];
    return m_impl->sampler != nil;
}

void MetalOcioRenderer::shutdown()
{
    if (!m_impl) return;
    m_impl->pipeline      = nil;
    m_impl->sampler       = nil;
    m_impl->lut2dTextures.clear();
    m_impl->lut2dSamplers.clear();
    m_impl->lut3dTextures.clear();
    m_impl->lut3dSamplers.clear();
    m_impl->outputTex     = nil;
    m_impl->outputW       = 0;
    m_impl->outputH       = 0;
    m_impl->device        = nil;
    m_impl->lastChainGeneration = -1;
}

bool MetalOcioRenderer::isInitialized() const
{
    return m_impl && m_impl->device != nil && m_impl->sampler != nil;
}

bool MetalOcioRenderer::hasPipeline() const
{
    return m_impl && m_impl->pipeline != nil;
}

const QString &MetalOcioRenderer::lastError() const
{
    return m_impl->lastError;
}

bool MetalOcioRenderer::rebuild(OCIOConfigManager *ocio)
{
    if (!ocio || !isInitialized()) return false;

    // Cached on chain generation.
    const int gen = ocio->activeChainGeneration();
    if (gen == m_impl->lastChainGeneration && m_impl->pipeline) {
        return true;
    }

    OcioChain chain =
        OcioChainBuilder::build(ocio, OcioChainBuilder::Language::Msl_2_0);
    if (!chain.ok) {
        m_impl->lastError = chain.errorMessage;
        m_impl->pipeline  = nil;
        return false;
    }

    // Gather OCIO's LUT inventory + binding declarations.
    QString lutDecls;
    QString kernelArgs;     // declarations in the kernel signature

    auto desc = chain.desc;
    int nextTexBinding = 2;     // 0 = src, 1 = dst, 2.. = LUTs
    int nextSmpBinding = 1;     // 0 = src_smp, 1.. = LUT samplers

    std::vector<id<MTLTexture>>     newLut2d;
    std::vector<id<MTLSamplerState>> newLut2dSmp;
    std::vector<id<MTLTexture>>     newLut3d;
    std::vector<id<MTLSamplerState>> newLut3dSmp;

    // OCIO's emitted OCIODisplay() function takes its texture +
    // sampler arguments in a specific order: ALL 3D LUTs first (in
    // their registration order within 3D), then ALL 1D/2D LUTs (in
    // their registration order within 1D/2D), then float4 inPixel.
    // The trailing digits of OCIO's texName (e.g. "ocio_lut3d_2"
    // → 2) give the global registration index used to sort within
    // each kind. Our kernel's [[texture(N)]] declaration order can
    // stay arbitrary — Metal resolves function arguments by variable
    // name + type — but the call expression MUST follow OCIO's order.
    struct CallArg { int regIndex; QString tname; QString sname; };
    std::vector<CallArg> callArgList3D;       // emitted first
    std::vector<CallArg> callArgList2D;       // emitted second
    auto parseTrailingInt = [](const QString &s) -> int {
        const int us = s.lastIndexOf(QLatin1Char('_'));
        if (us < 0) return 0;
        bool ok = false;
        const int n = s.mid(us + 1).toInt(&ok);
        return ok ? n : 0;
    };

    // --- 1D + 2D LUTs ---
    for (int i = 0; i < desc->getNumTextures(); ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan =
            OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim =
            OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(i, texName, smpName, w, h, chan, dim, interp);
        if (!texName || !*texName || !smpName || !*smpName) continue;

        // OCIO returns DISTINCT names for the texture vs sampler
        // (e.g. texName="ocio_lut3d_0", smpName="ocio_lut3d_0Sampler").
        // The emitted OCIODisplay() function declares its parameters
        // using exactly these names, so the kernel must declare them
        // the same way — using smpName for the texture binding name
        // would not match OCIO's signature and the compile fails with
        // "no matching function for call to 'OCIODisplay'".
        const QString tname = QString::fromUtf8(texName);
        const QString sname = QString::fromUtf8(smpName);
        const bool isRgb = (chan == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        // OCIO emits actual `texture1d<float>` declarations for 1D
        // LUTs. Our kernel arg type and the underlying MTLTexture's
        // textureType MUST match OCIO's emitted parameter type or
        // `OCIODisplay()` won't resolve. 2D LUTs (rare; chan=R w/ h>1)
        // stay on the texture2d path.
        const bool is1D = (dim == OCIO::GpuShaderDesc::TEXTURE_1D);

        const float *values = nullptr;
        desc->getTextureValues(i, values);
        if (!values) continue;

        const int width  = static_cast<int>(w > 0 ? w : 1);
        const int height = is1D ? 1 : static_cast<int>(h > 0 ? h : 1);

        // Pack to RGBA32Float if RGB (Metal has no RGB32F).
        const MTLPixelFormat fmt = isRgb ? MTLPixelFormatRGBA32Float
                                         : MTLPixelFormatR32Float;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = fmt;
        td.width       = width;
        td.height      = height;
        td.depth       = 1;
        td.textureType = is1D ? MTLTextureType1D : MTLTextureType2D;
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [m_impl->device newTextureWithDescriptor:td];
        if (!tex) {
            m_impl->lastError = QStringLiteral("MetalOcioRenderer: LUT texture create failed");
            return false;
        }

        // Upload pixel data. 1D textures use MTLRegionMake1D and
        // bytesPerRow = 0 (a single row's worth of bytes; Metal
        // infers from width × bytesPerPixel).
        const int n = width * height;
        const MTLRegion region = is1D
            ? MTLRegionMake1D(0, width)
            : MTLRegionMake2D(0, 0, width, height);
        const NSUInteger bpr = is1D
            ? 0
            : static_cast<NSUInteger>(width) *
              (isRgb ? 4u : 1u) * sizeof(float);
        if (isRgb) {
            std::vector<float> rgba(static_cast<std::size_t>(n) * 4);
            for (int p = 0; p < n; ++p) {
                rgba[p * 4 + 0] = values[p * 3 + 0];
                rgba[p * 4 + 1] = values[p * 3 + 1];
                rgba[p * 4 + 2] = values[p * 3 + 2];
                rgba[p * 4 + 3] = 1.0f;
            }
            [tex replaceRegion:region
                   mipmapLevel:0
                     withBytes:rgba.data()
                   bytesPerRow:bpr];
        } else {
            [tex replaceRegion:region
                   mipmapLevel:0
                     withBytes:values
                   bytesPerRow:bpr];
        }

        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter   = MTLSamplerMinMagFilterLinear;
        sd.magFilter   = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> smp = [m_impl->device newSamplerStateWithDescriptor:sd];

        newLut2d.push_back(tex);
        newLut2dSmp.push_back(smp);

        // Kernel argument declaration. Match OCIO's emitted parameter
        // type (texture1d for 1D LUTs).
        const char *texMslType = is1D ? "texture1d" : "texture2d";
        kernelArgs += QStringLiteral(
            "    %5<float, access::sample> %1 [[texture(%2)]],\n"
            "    sampler                   %3 [[sampler(%4)]],\n")
            .arg(tname).arg(nextTexBinding++)
            .arg(sname).arg(nextSmpBinding++)
            .arg(QString::fromUtf8(texMslType));
        callArgList2D.push_back({ parseTrailingInt(tname), tname, sname });
    }

    // --- 3D LUTs ---
    for (int i = 0; i < desc->getNum3DTextures(); ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned edgeLen = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(i, texName, smpName, edgeLen, interp);
        if (!texName || !*texName || !smpName || !*smpName) continue;

        const QString tname = QString::fromUtf8(texName);
        const QString sname = QString::fromUtf8(smpName);

        const float *values = nullptr;
        desc->get3DTextureValues(i, values);
        if (!values || edgeLen == 0) continue;

        const int E = static_cast<int>(edgeLen);

        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = MTLPixelFormatRGBA32Float;
        td.width  = E;
        td.height = E;
        td.depth  = E;
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [m_impl->device newTextureWithDescriptor:td];
        if (!tex) {
            m_impl->lastError = QStringLiteral("MetalOcioRenderer: 3D LUT texture create failed");
            return false;
        }

        // Pack RGB → RGBA per slice.
        const int sliceTexels = E * E;
        std::vector<float> slice(static_cast<std::size_t>(sliceTexels) * 4);
        for (int z = 0; z < E; ++z) {
            const float *src = values + z * sliceTexels * 3;
            for (int p = 0; p < sliceTexels; ++p) {
                slice[p * 4 + 0] = src[p * 3 + 0];
                slice[p * 4 + 1] = src[p * 3 + 1];
                slice[p * 4 + 2] = src[p * 3 + 2];
                slice[p * 4 + 3] = 1.0f;
            }
            [tex replaceRegion:MTLRegionMake3D(0, 0, z, E, E, 1)
                   mipmapLevel:0
                         slice:0
                     withBytes:slice.data()
                   bytesPerRow:E * 4 * sizeof(float)
                 bytesPerImage:0];
        }

        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter   = MTLSamplerMinMagFilterLinear;
        sd.magFilter   = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
        id<MTLSamplerState> smp = [m_impl->device newSamplerStateWithDescriptor:sd];

        newLut3d.push_back(tex);
        newLut3dSmp.push_back(smp);

        kernelArgs += QStringLiteral(
            "    texture3d<float, access::sample> %1 [[texture(%2)]],\n"
            "    sampler                          %3 [[sampler(%4)]],\n")
            .arg(tname).arg(nextTexBinding++)
            .arg(sname).arg(nextSmpBinding++);
        callArgList3D.push_back({ parseTrailingInt(tname), tname, sname });
    }

    // OCIO's signature: 3D LUTs (reg-order), then 1D/2D LUTs
    // (reg-order), then color. Stable-sort each list by reg index;
    // concatenate 3D before 2D.
    auto byRegIndex = [](const CallArg &a, const CallArg &b) {
        return a.regIndex < b.regIndex;
    };
    std::stable_sort(callArgList3D.begin(), callArgList3D.end(), byRegIndex);
    std::stable_sort(callArgList2D.begin(), callArgList2D.end(), byRegIndex);
    QString callArgs;       // additional arguments to OCIODisplay()
    for (const auto &c : callArgList3D) {
        callArgs += QStringLiteral("%1, %2, ").arg(c.tname, c.sname);
    }
    for (const auto &c : callArgList2D) {
        callArgs += QStringLiteral("%1, %2, ").arg(c.tname, c.sname);
    }

    // Compose the full kernel: header + OCIO function + main(...) wrapper.
    const QString kernel =
        QString::fromUtf8(kKernelHeader)
        + chain.shaderText                       // OCIO's MSL function
        + QString::fromUtf8(kKernelMainPrefix)
        + kernelArgs
        + QString::fromUtf8(kKernelMainSuffix)
        // OCIO's emitted OCIODisplay() takes textures + samplers
        // FIRST, inPixel LAST. callArgs accumulates "tex, smp, "
        // pairs with trailing separator so we can splice color in
        // at the end. With zero LUTs, callArgs is empty and the
        // call collapses to OCIODisplay(color), which matches the
        // no-LUT signature OCIO emits in that branch.
        + QStringLiteral("    color = OCIODisplay(%1color);\n").arg(callArgs)
        + QString::fromUtf8(kKernelFooter);

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kernel.toUtf8().constData()];
    id<MTLLibrary> lib =
        [m_impl->device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        m_impl->lastError = QStringLiteral("MetalOcioRenderer: kernel compile failed: %1")
            .arg(err ? QString::fromUtf8([err.localizedDescription UTF8String]) : QStringLiteral("(no error)"));
        qWarning("%s", qPrintable(m_impl->lastError));

        // Dump the failing kernel + OCIO's raw shader text to disk
        // so we can inspect the OCIODisplay() signature mismatch.
        const QString tmpDir = QStringLiteral("/tmp");
        const QString fullPath = tmpDir + QStringLiteral("/qcv-ocio-kernel-fail.metal");
        const QString ocioOnly = tmpDir + QStringLiteral("/qcv-ocio-chain-only.metal");
        if (QFile f(fullPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(kernel.toUtf8());
            qInfo("MetalOcioRenderer: dumped failing kernel to %s",
                  qPrintable(fullPath));
        }
        if (QFile f(ocioOnly); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(chain.shaderText.toUtf8());
            qInfo("MetalOcioRenderer: dumped OCIO chain text to %s",
                  qPrintable(ocioOnly));
        }

        m_impl->pipeline = nil;
        // Mark this gen as failed so we don't retry compiling the
        // same broken kernel every frame (which thrashes the render
        // thread and tanks playback). The user's next config change
        // bumps gen and gives us a fresh shot.
        m_impl->lastChainGeneration = gen;
        return false;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"ocio_apply"];
    if (!fn) {
        m_impl->lastError = QStringLiteral("MetalOcioRenderer: ocio_apply not found");
        m_impl->pipeline = nil;
        return false;
    }
    id<MTLComputePipelineState> pso =
        [m_impl->device newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
        m_impl->lastError = QStringLiteral("MetalOcioRenderer: pipeline creation failed: %1")
            .arg(err ? QString::fromUtf8([err.localizedDescription UTF8String]) : QStringLiteral("(no error)"));
        qWarning("%s", qPrintable(m_impl->lastError));
        m_impl->pipeline = nil;
        return false;
    }

    // Swap in the new pipeline + LUT set; the previous set is dropped
    // (ARC releases textures + samplers).
    m_impl->pipeline      = pso;
    m_impl->lut2dTextures = std::move(newLut2d);
    m_impl->lut2dSamplers = std::move(newLut2dSmp);
    m_impl->lut3dTextures = std::move(newLut3d);
    m_impl->lut3dSamplers = std::move(newLut3dSmp);
    m_impl->lastChainGeneration = gen;
    m_impl->lastError.clear();

    qInfo("MetalOcioRenderer: rebuilt for chain gen %d (%zu 2D LUTs, %zu 3D LUTs)",
          gen, m_impl->lut2dTextures.size(), m_impl->lut3dTextures.size());
    return true;
}

void *MetalOcioRenderer::apply(void *cmdBufPtr, void *sourceMtlTexture,
                               int width, int height)
{
    if (!hasPipeline() || !cmdBufPtr || !sourceMtlTexture ||
        width <= 0 || height <= 0) {
        return nullptr;
    }

    id<MTLCommandBuffer> cb     = (__bridge id<MTLCommandBuffer>)cmdBufPtr;
    id<MTLTexture>       source = (__bridge id<MTLTexture>)sourceMtlTexture;

    // Resize / create the persistent output texture if needed.
    if (m_impl->outputTex == nil ||
        width != m_impl->outputW || height != m_impl->outputH) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:width
                                        height:height
                                     mipmapped:NO];
        td.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        m_impl->outputTex = [m_impl->device newTextureWithDescriptor:td];
        m_impl->outputW   = width;
        m_impl->outputH   = height;
    }
    if (!m_impl->outputTex) return nullptr;

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->pipeline];
    [enc setTexture:source           atIndex:0];
    [enc setTexture:m_impl->outputTex atIndex:1];
    [enc setSamplerState:m_impl->sampler atIndex:0];

    int texBind = 2;
    int smpBind = 1;
    for (std::size_t i = 0; i < m_impl->lut2dTextures.size(); ++i) {
        [enc setTexture:m_impl->lut2dTextures[i] atIndex:texBind++];
        [enc setSamplerState:m_impl->lut2dSamplers[i] atIndex:smpBind++];
    }
    for (std::size_t i = 0; i < m_impl->lut3dTextures.size(); ++i) {
        [enc setTexture:m_impl->lut3dTextures[i] atIndex:texBind++];
        [enc setSamplerState:m_impl->lut3dSamplers[i] atIndex:smpBind++];
    }

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    return (__bridge void *)m_impl->outputTex;
}

} // namespace qcv
