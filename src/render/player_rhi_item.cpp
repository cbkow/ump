#include "player_rhi_item.h"
#include "test_textures.h"
#include "ocio_shader_builder.h"

#include "color/ocio_config_manager.h"
#include "decode/frame_handle.h"
#include "decode/video_decoder.h"

#if defined(Q_OS_MACOS)
#include "zero_copy_bridge_metal.h"
#endif

#include <QElapsedTimer>
#include <QFile>
#include <QFloat16>
#include <QImage>
#include <QQuickRhiItem>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <unordered_map>

namespace qcv {

namespace {

// Load a compiled .qsb shader bundle from the resource system.
QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning("PlayerRhiRenderer: cannot open shader %s", path);
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

// Test source intrinsic size — same for both A and B in Phase 1.7.
// Real video frames in Phase 2+ will arrive at their decoded size and
// per-source sizing will move into the UBO.
constexpr QSize kSourceSize{ 1280, 720 };

// Stage a CPU QImage into a QRhi RGBA16F texture, dispatched on the
// QImage's actual format. The previous implementation always crushed
// to RGBA8 first (Format_RGBA8888) before re-promoting to FP16 — fine
// for the 8-bit test pattern, catastrophic for image-sequence EXR
// frames whose Format_RGBA16FPx4 / Format_RGBA64 source bits got
// thrown away. This dispatcher picks the cheapest path:
//
//   Format_RGBA16FPx4  → direct memcpy of the QImage bits (zero
//                         per-pixel work). EXR fast path.
//   Format_RGBA64      → bit-twiddle uint16 → qfloat16 (1.0 / 65535
//                         scale per channel). 16-bit PNG / TIFF.
//   Format_RGBA8888    → original 8-bit → qfloat16 promotion.
//   anything else      → convertToFormat(RGBA8888) fallback.
void uploadRgba8ImageToFp16(QRhiResourceUpdateBatch *batch,
                            QRhiTexture *tex, const QImage &img)
{
    const int n = img.width() * img.height();
    QByteArray bytes;
    const QImage::Format f = img.format();

    if (f == QImage::Format_RGBA16FPx4) {
        // Zero-copy fast path: bits are already FP16. Pass
        // img.constBits() directly into the upload descriptor —
        // QRhi snapshots/staging-buffers what it needs before the
        // batch is submitted, and our FrameHandle's shared_ptr
        // keepalive keeps the source bits alive across this call.
        // Saves a 70 MB heap alloc + memcpy per cache miss on 4K
        // EXR FP16 vs the previous QByteArray copy path.
        const std::size_t byteCount =
            static_cast<std::size_t>(n) * 4 * sizeof(qfloat16);
        QRhiTextureSubresourceUploadDescription sub(
            img.constBits(), static_cast<quint32>(byteCount));
        QRhiTextureUploadEntry entry(0, 0, sub);
        batch->uploadTexture(tex, QRhiTextureUploadDescription(entry));
        return;
    } else if (f == QImage::Format_RGBA64) {
        // 16-bit unsigned per channel → FP16. One multiply per channel.
        bytes.resize(n * 4 * sizeof(qfloat16));
        qfloat16 *dst = reinterpret_cast<qfloat16 *>(bytes.data());
        const std::uint16_t *src =
            reinterpret_cast<const std::uint16_t *>(img.constBits());
        constexpr float inv65535 = 1.0f / 65535.0f;
        for (int i = 0; i < n; ++i) {
            dst[i * 4 + 0] = qfloat16(src[i * 4 + 0] * inv65535);
            dst[i * 4 + 1] = qfloat16(src[i * 4 + 1] * inv65535);
            dst[i * 4 + 2] = qfloat16(src[i * 4 + 2] * inv65535);
            dst[i * 4 + 3] = qfloat16(src[i * 4 + 3] * inv65535);
        }
    } else {
        // 8-bit (or convert-to-8) path. Identical to the original
        // logic — preserves the test-pattern + LDR upload.
        QImage src = img;
        if (src.format() != QImage::Format_RGBA8888) {
            src = src.convertToFormat(QImage::Format_RGBA8888);
        }
        bytes.resize(n * 4 * sizeof(qfloat16));
        qfloat16 *dst = reinterpret_cast<qfloat16 *>(bytes.data());
        const uchar *p = src.constBits();
        constexpr float inv255 = 1.0f / 255.0f;
        for (int i = 0; i < n; ++i) {
            dst[i * 4 + 0] = qfloat16(p[i * 4 + 0] * inv255);
            dst[i * 4 + 1] = qfloat16(p[i * 4 + 1] * inv255);
            dst[i * 4 + 2] = qfloat16(p[i * 4 + 2] * inv255);
            dst[i * 4 + 3] = qfloat16(p[i * 4 + 3] * inv255);
        }
    }
    QRhiTextureSubresourceUploadDescription sub(
        bytes.constData(), static_cast<quint32>(bytes.size()));
    QRhiTextureUploadEntry entry(0, 0, sub);
    batch->uploadTexture(tex, QRhiTextureUploadDescription(entry));
}

// Phase 1.7 renderer — multi-pass:
//
//   Pass 1: compositor.frag samples srcA + srcB into compositeRaw,
//           selecting between modes (SINGLE / SIDE_BY_SIDE / SPLIT_WIPE).
//           A small uniform buffer carries (resolution, srcSize, mode,
//           splitPos).
//   Pass 2: passthrough.frag samples compositeRaw into the player
//           window's swapchain.
//
// Source textures are RGBA8, uploaded once at init from CPU-generated
// QImages. compositeRaw is sized to the swapchain and rebuilt on
// resize.
//
// Phases 2+ replace the static source uploads with per-frame video
// uploads, add OCIO + stroke + safety + display passes between Pass 1
// and the final swapchain write, and switch compositeRaw to RGBA16F.
class PlayerRhiRenderer : public QQuickRhiItemRenderer
{
public:
    void initialize(QRhiCommandBuffer * /*cb*/) override
    {
        QRhi *r = rhi();
        if (!r) {
            qWarning("PlayerRhiRenderer: rhi() returned null at initialize");
            return;
        }

        // ---- Size-independent resources, created once.
        if (!m_vbuf) {
            createSharedResources(r);
        }

        // ---- Size-dependent resources.
        const QSize size = renderTarget()->pixelSize();
        const bool sizeChanged = (size != m_lastSize);
        const bool firstInit = !m_compositeRaw;
        m_lastSize = size;

        if (firstInit || sizeChanged) {
            createOffscreenTarget(r, size);
        }

        // ---- Pass 2 SRB references compositeRaw. Bindings must be set
        // and create()'d before the pipeline that uses this SRB is
        // created — pipelines capture binding layout at create-time.
        if (firstInit || sizeChanged) {
            m_pass2Srb->setBindings({
                QRhiShaderResourceBinding::sampledTexture(
                    0, QRhiShaderResourceBinding::FragmentStage,
                    m_compositeRaw.get(), m_sampler.get())
            });
            m_pass2Srb->create();

            // m_pass2OcioSrb references the previous m_compositeRaw,
            // which createOffscreenTarget just replaced. Drop the OCIO
            // Pass 2 pipeline + SRB so render() rebuilds them against
            // the new texture next frame. (LUTs are size-independent
            // and get re-uploaded as part of the rebuild — wasteful on
            // resize but rare and < a few ms.) Without this, Metal
            // dereferences a dead QRhiTexture in
            // enqueueShaderResourceBindings → EXC_BAD_ACCESS.
            m_pass2OcioPipeline.reset();
            m_pass2OcioSrb.reset();
        }

        // ---- Pipelines. Render-pass descriptors come from the targets.
        if (!m_pass1Pipeline) {
            createPass1Pipeline(r);
            createPass2Pipeline(r);
        } else if (firstInit || sizeChanged) {
            m_pass1Pipeline->setRenderPassDescriptor(m_offscreenPassDesc.get());
            m_pass1Pipeline->create();
            m_pass2Pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            m_pass2Pipeline->create();
        }
    }

    void synchronize(QQuickRhiItem *item) override
    {
        auto *p = static_cast<PlayerRhiItem *>(item);
        m_clearColor   = p->clearColor();
        m_mode         = static_cast<int>(p->compositorMode());
        m_splitPos     = static_cast<float>(p->splitPos());
        m_videoDecoder = p->videoDecoder();
        m_ocio         = p->ocio();
        m_imageSeqCache  = p->imageSeqCache();
        // Snapshot the playhead from the cache atomic. Pull-model:
        // the cache fills async; the renderer asks for the current
        // playhead frame each present.
        if (m_imageSeqCache) {
            m_imageSeqPlayhead = m_imageSeqCache->currentPlayhead();
        }

        // Detect OCIO chain changes (slot-machine UI edits, bypass
        // toggle, config switch). Bumps the build flag so render()
        // tears down + rebuilds the Pass 2 pipeline + LUTs from the
        // current OCIOConfigManager state.
        if (m_ocio) {
            const int gen = m_ocio->activeChainGeneration();
            if (gen != m_lastChainGeneration) {
                m_lastChainGeneration = gen;
                m_ocioRebuildPending = true;
                m_ocioBuildFailed = false;
            }
        }
    }

    void render(QRhiCommandBuffer *cb) override
    {
        if (!m_pass1Pipeline || !m_pass2Pipeline || !m_compositeRaw) {
            return;
        }

        QRhi *r = rhi();
        QRhiResourceUpdateBatch *uploads = m_initialUpload.release();
        if (!uploads) {
            uploads = r->nextResourceUpdateBatch();
        }

        // Phase 2.3: lazy-build the OCIO Pass 2 pipeline on first
        // frame after m_ocio becomes available. Falls back to the
        // already-set-up passthrough Pass 2 if OCIO can't build.
        // Phase 2.4: also rebuild whenever the active chain changed
        // (slot-machine UI edits) — m_ocioRebuildPending was set in
        // synchronize() if the OCIO generation counter bumped.
        if (m_ocio && !m_ocioBuildFailed
            && (m_ocioRebuildPending || !m_pass2OcioPipeline)) {
            // Drop previous pipeline + LUTs so the rebuild is clean.
            // QRhi keeps internal references on resources used by
            // already-submitted commands until the GPU consumes them,
            // so destroying our unique_ptr handles here is safe even
            // mid-frame: this code runs before Pass 2 records.
            m_pass2OcioPipeline.reset();
            m_pass2OcioSrb.reset();
            m_pass2OcioBuilt = OcioShaderBuilder::Built();

            buildOcioPass2(r, uploads);
            m_ocioRebuildPending = false;
        }

        // ---- Source-frame acquisition.
        //
        // Two source models:
        //  (A) Image-sequence pull (Phase 7.4.b.4). When
        //      m_imageSeqCache is non-null, we ask the cache for
        //      the current playhead's pixel data and build a
        //      non-owning QImage view. NEVER blocks on decode — the
        //      cache fills async and returns nullptr on miss. On
        //      miss we hold m_activeSrcTex (the previously-bound
        //      texture) for last-good fallback, mirroring old
        //      direct_exr_cache.cpp:492-496.
        //  (B) Video / scrub push (FFmpeg). m_videoDecoder->
        //      fetchLatest() returns the latest published frame
        //      from the streaming decoder's output slot.
        // The two are mutually exclusive — image-seq sources don't
        // touch VideoDecoder; video sources don't have an
        // ImageSequenceCache.
        FrameHandle nextFrame;
        bool gotNew = false;
        QImage imgSeqImage;
        std::shared_ptr<PixelData> imgSeqHeld;
        int  imgSeqKey = -1;
        if (m_imageSeqCache) {
            auto pd = m_imageSeqCache->getFrame(m_imageSeqPlayhead);
            int actual = m_imageSeqPlayhead;
            if (!pd) {
                pd = m_imageSeqCache->getClosestFrame(
                    m_imageSeqPlayhead, &actual);
            }
            if (pd && pd->width > 0 && pd->height > 0 &&
                !pd->pixels.empty()) {
                QImage::Format fmt;
                int bpr;
                switch (pd->pixel_format) {
                    case PixelFormat::RGBA8:
                        fmt = QImage::Format_RGBA8888;
                        bpr = pd->width * 4;
                        break;
                    case PixelFormat::RGBA16:
                        fmt = QImage::Format_RGBA64;
                        bpr = pd->width * 8;
                        break;
                    case PixelFormat::RGBA16F:
                    default:
                        fmt = QImage::Format_RGBA16FPx4;
                        bpr = pd->width * 8;
                        break;
                }
                imgSeqImage = QImage(pd->pixels.data(),
                                     pd->width, pd->height, bpr, fmt);
                imgSeqHeld = pd;   // keepalive for the QImage view
                imgSeqKey = actual;
            }
        } else if (m_videoDecoder && m_videoDecoder->fetchLatest(&nextFrame)) {
            m_currentFrame = std::move(nextFrame);
            gotNew = true;
        }

        bool runPass0ThisFrame = false;

        if (m_imageSeqCache && !imgSeqImage.isNull()) {
            // ---- Image-sequence pull-model path.
            //
            // Single persistent m_srcA texture, same shape as the
            // video CPU path. The per-frame texture pool we tried in
            // 7.4.b.4 — both bounded (16-deep LRU) and unbounded —
            // wedged the whole app within ~1 second of playback:
            // QRhi/Metal can't sustain ~32 MB QRhiTexture allocs at
            // 24 fps on the render thread, command buffers backlog,
            // GUI thread blocks at next sync waiting for render.
            // The old app's glTextureCache_ relied on a Metal/Vulkan
            // GPUUploadThread that QRhi can't expose to us.
            //
            // Trade-off: scrub-back into a recently-displayed frame
            // re-uploads pixels (~1–2 ms measured for 4K FP16) rather
            // than re-binding an existing GPU texture. That's well
            // under the vsync budget and the cache HIT still skips
            // the disk-decode (~400 ms saved). Acceptable.
            if (imgSeqImage.size() != m_srcA->pixelSize()) {
                rebuildSrcA(r, imgSeqImage.size());
            }
            uploadRgba8ImageToFp16(uploads, m_srcA.get(), imgSeqImage);
            if (m_activeSrcTex != m_srcA.get()) {
                m_activeSrcTex = m_srcA.get();
                m_pass1Srb->setBindings({
                    QRhiShaderResourceBinding::uniformBuffer(
                        0, QRhiShaderResourceBinding::FragmentStage,
                        m_pass1UBuf.get()),
                    QRhiShaderResourceBinding::sampledTexture(
                        1, QRhiShaderResourceBinding::FragmentStage,
                        m_activeSrcTex, m_sampler.get()),
                    QRhiShaderResourceBinding::sampledTexture(
                        2, QRhiShaderResourceBinding::FragmentStage,
                        m_srcB.get(), m_sampler.get()),
                });
                m_pass1Srb->create();
            }
        } else if (m_currentFrame.kind() == FrameHandle::Kind::Cpu) {
            const QImage &img = m_currentFrame.cpuImage();
            if (!img.isNull() && img.size() != m_srcA->pixelSize()) {
                rebuildSrcA(r, img.size());
            }

            if (!img.isNull() && gotNew) {
                // ---- Video / test-pattern CPU path: upload into
                // the persistent m_srcA texture. m_srcA is RGBA16F;
                // uploadRgba8ImageToFp16 dispatches on QImage format
                // (FP16 fast path is memcpy).
                uploadRgba8ImageToFp16(uploads, m_srcA.get(), img);
                if (m_activeSrcTex != m_srcA.get()) {
                    m_activeSrcTex = m_srcA.get();
                    m_pass1Srb->setBindings({
                        QRhiShaderResourceBinding::uniformBuffer(
                            0, QRhiShaderResourceBinding::FragmentStage,
                            m_pass1UBuf.get()),
                        QRhiShaderResourceBinding::sampledTexture(
                            1, QRhiShaderResourceBinding::FragmentStage,
                            m_activeSrcTex, m_sampler.get()),
                        QRhiShaderResourceBinding::sampledTexture(
                            2, QRhiShaderResourceBinding::FragmentStage,
                            m_srcB.get(), m_sampler.get()),
                    });
                    m_pass1Srb->create();
                }
            }
        }
#if defined(Q_OS_MACOS)
        else if (m_currentFrame.kind() == FrameHandle::Kind::Metal) {
            const QSize frameSize(m_currentFrame.width(), m_currentFrame.height());
            if (frameSize != m_srcA->pixelSize()) {
                rebuildSrcA(r, frameSize);
            }
            if (gotNew) {
                runPass0ThisFrame = ensurePass0AndBridge(r);
            }
        }
#endif

        const QSize compositeSize = m_compositeRaw->pixelSize();

        // Pass 1 UBO (std140, 48 bytes total):
        //   vec2  resolution   @  0
        //   vec2  srcSize      @  8
        //   int   mode         @ 16
        //   float splitPos     @ 20
        //   (8 bytes pad to align next vec4 at 32)
        //   vec4  background  @ 32   source-over composite color
        alignas(16) float ubo[12] = {};
        ubo[0] = static_cast<float>(compositeSize.width());
        ubo[1] = static_cast<float>(compositeSize.height());
        const QSize srcLiveSize = m_srcA->pixelSize();
        ubo[2] = static_cast<float>(srcLiveSize.width());
        ubo[3] = static_cast<float>(srcLiveSize.height());
        // mode is an int laid out at byte offset 16 — write through an
        // int* aliased to the float buffer to honor std140 byte layout.
        *reinterpret_cast<int *>(&ubo[4]) = m_mode;
        ubo[5] = m_splitPos;
        // ubo[6], ubo[7] are pad; left at 0.
        ubo[8]  = static_cast<float>(m_clearColor.redF());
        ubo[9]  = static_cast<float>(m_clearColor.greenF());
        ubo[10] = static_cast<float>(m_clearColor.blueF());
        ubo[11] = 1.0f;
        uploads->updateDynamicBuffer(m_pass1UBuf.get(), 0, sizeof(ubo), ubo);

        QRhiCommandBuffer::VertexInput vbufBinding(m_vbuf.get(), 0);

#if defined(Q_OS_MACOS)
        // ---- Pass 0 (Metal zero-copy path only): YUV planes → m_srcA
        // via yuv_to_rgb.frag. The plane textures are MTLTextures
        // wrapping the CVPixelBuffer's IOSurface — no pixel copy.
        if (runPass0ThisFrame) {
            // Pass 0 UBO (16 bytes): matrixIdx + fullRange + 8B pad.
            int pass0Ubo[4] = { m_pass0MatrixIdx, m_pass0FullRange, 0, 0 };
            uploads->updateDynamicBuffer(m_pass0UBuf.get(), 0, sizeof(pass0Ubo),
                                         pass0Ubo);

            cb->beginPass(m_pass0Target.get(), { 0, 0, 0, 1 }, { 1.0f, 0 }, uploads);
            uploads = nullptr;   // batch consumed by Pass 0
            // Dispatch the right pipeline + SRB based on whether the
            // source was packed (AYpCbCr16) or biplanar (NV12 / P210 /
            // P010). Both pipelines target m_srcA.
            if (m_pass0Packed) {
                cb->setGraphicsPipeline(m_pass0PipelineAyuv.get());
            } else {
                cb->setGraphicsPipeline(m_pass0Pipeline.get());
            }
            const QSize sa = m_srcA->pixelSize();
            cb->setViewport({ 0.0f, 0.0f,
                              static_cast<float>(sa.width()),
                              static_cast<float>(sa.height()) });
            if (m_pass0Packed) {
                cb->setShaderResources(m_pass0SrbAyuv.get());
            } else {
                cb->setShaderResources(m_pass0Srb.get());
            }
            cb->setVertexInput(0, 1, &vbufBinding);
            cb->draw(3);
            cb->endPass();
        }
#endif

        // ---- Pass 1: compositor → compositeRaw
        cb->beginPass(m_offscreenTarget.get(), m_clearColor, { 1.0f, 0 }, uploads);

        cb->setGraphicsPipeline(m_pass1Pipeline.get());
        cb->setViewport({ 0.0f, 0.0f,
                          static_cast<float>(compositeSize.width()),
                          static_cast<float>(compositeSize.height()) });
        // Pass m_pass1Srb explicitly so binding updates from rebuildSrcA
        // are picked up by QRhi (descriptor set is rebuilt on srb create()).
        cb->setShaderResources(m_pass1Srb.get());
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(3);

        cb->endPass();

        // ---- Pass 2: compositeRaw → swapchain
        // OCIO display pass when built; passthrough fallback otherwise.
        const QSize swapSize = renderTarget()->pixelSize();
        cb->beginPass(renderTarget(), QColor(0, 0, 0), { 1.0f, 0 });

        // OCIO engagement (Phase 2.5b): the user must deliberately
        // engage OCIO before the chain applies. Default disengaged,
        // so the app boots showing the raw image. Slot-machine
        // selections persist regardless of engagement; flipping
        // engaged ON is the explicit "apply" action, and once
        // engaged subsequent slot edits live-update.
        const bool useOcio = m_pass2OcioPipeline
                             && m_ocio && m_ocio->engaged();
        if (useOcio) {
            cb->setGraphicsPipeline(m_pass2OcioPipeline.get());
            cb->setViewport({ 0.0f, 0.0f,
                              static_cast<float>(swapSize.width()),
                              static_cast<float>(swapSize.height()) });
            cb->setShaderResources(m_pass2OcioSrb.get());
        } else {
            cb->setGraphicsPipeline(m_pass2Pipeline.get());
            cb->setViewport({ 0.0f, 0.0f,
                              static_cast<float>(swapSize.width()),
                              static_cast<float>(swapSize.height()) });
            cb->setShaderResources(m_pass2Srb.get());
        }
        cb->setVertexInput(0, 1, &vbufBinding);
        cb->draw(3);

        cb->endPass();
    }

private:
    void createSharedResources(QRhi *r)
    {
        // Oversized triangle (3 vertices) — clipped to screen exactly.
        static const float kVertices[] = {
            -1.0f, -1.0f,
             3.0f, -1.0f,
            -1.0f,  3.0f,
        };

        m_vbuf.reset(r->newBuffer(QRhiBuffer::Immutable,
                                  QRhiBuffer::VertexBuffer,
                                  sizeof(kVertices)));
        m_vbuf->create();

        m_initialUpload.reset(r->nextResourceUpdateBatch());
        m_initialUpload->uploadStaticBuffer(m_vbuf.get(), kVertices);

        m_sampler.reset(r->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();

        // Sampler for YUV plane textures: bilinear chroma upsampling.
        // Same params as m_sampler today; broken out so tweaks don't
        // affect the static SOURCE A/B test patterns.
        m_planeSampler.reset(r->newSampler(
            QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_planeSampler->create();

        // Source textures — Phase 2.6.1c: m_srcA is now RGBA16F so
        // the Metal zero-copy Pass 0 (10-bit P010 / 12-bit content)
        // doesn't quantize down to 8 bits. PQ-encoded values fit in
        // 0..1 but the heavy non-linearity makes 8-bit quantization
        // visibly noisy on HDR sources; FP16 preserves the full
        // input range with headroom for >1.0 (extended-linear) too.
        // m_srcB stays RGBA8 — it's the static test pattern, never
        // an HDR source. Pass 1 happily composites mixed formats
        // because everything goes into compositeRaw RGBA16F anyway.
        QImage imgA = generateSourceA(kSourceSize);
        QImage imgB = generateSourceB(kSourceSize);
        if (imgA.format() != QImage::Format_RGBA8888) {
            imgA = imgA.convertToFormat(QImage::Format_RGBA8888);
        }
        if (imgB.format() != QImage::Format_RGBA8888) {
            imgB = imgB.convertToFormat(QImage::Format_RGBA8888);
        }

        // m_srcA is renderable (Pass 0 writes into it for the Metal
        // zero-copy path); m_srcB is a static test pattern.
        m_srcA.reset(r->newTexture(QRhiTexture::RGBA16F, kSourceSize, 1,
                                   QRhiTexture::RenderTarget));
        m_srcA->create();
        m_srcB.reset(r->newTexture(QRhiTexture::RGBA8, kSourceSize, 1));
        m_srcB->create();

        // RGBA16F can't take a QImage upload directly (QImage is
        // 8 bits/channel); convert to qfloat16 ourselves.
        uploadRgba8ImageToFp16(m_initialUpload.get(), m_srcA.get(), imgA);
        m_initialUpload->uploadTexture(m_srcB.get(), imgB);

        // Pass 1 uniform buffer — std140 48-byte block (see render()).
        m_pass1UBuf.reset(r->newBuffer(QRhiBuffer::Dynamic,
                                       QRhiBuffer::UniformBuffer, 48));
        m_pass1UBuf->create();

        // Pass 1 SRB — UBO + 2 source textures (sharing one sampler).
        m_pass1Srb.reset(r->newShaderResourceBindings());
        m_pass1Srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage,
                m_pass1UBuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_srcA.get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                m_srcB.get(), m_sampler.get()),
        });
        m_pass1Srb->create();

        // Pass 2 SRB — bindings populated in initialize() once
        // compositeRaw exists.
        m_pass2Srb.reset(r->newShaderResourceBindings());
    }

    // Recreate m_srcA at a new size (different video resolution) and
    // refresh the Pass 1 SRB. The texture is created with the
    // RenderTarget flag so Pass 0 can write into it on the Metal
    // zero-copy path. Pass 0's offscreen target is invalidated; it's
    // rebuilt lazily on the next ensurePass0AndBridge() call.
    void rebuildSrcA(QRhi *r, QSize size)
    {
        m_srcA.reset(r->newTexture(QRhiTexture::RGBA16F, size, 1,
                                   QRhiTexture::RenderTarget));
        m_srcA->create();

        m_pass1Srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::FragmentStage,
                m_pass1UBuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                m_srcA.get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2, QRhiShaderResourceBinding::FragmentStage,
                m_srcB.get(), m_sampler.get()),
        });
        m_pass1Srb->create();

        // Pass 0 target follows m_srcA. Invalidate; recreated below
        // on demand.
        m_pass0Target.reset();
        m_pass0PassDesc.reset();
    }

#if defined(Q_OS_MACOS)
    // Bridge the current FrameHandle's CVPixelBuffer to plane MTL
    // textures, set up Pass 0's SRB / target / pipeline if needed,
    // and return true if Pass 0 should run this frame. Returns
    // false on unsupported pixel format (caller continues with
    // m_srcA's previous contents — better than a black flash).
    bool ensurePass0AndBridge(QRhi *r)
    {
        if (!m_zeroCopyBridge) {
            m_zeroCopyBridge = std::make_unique<ZeroCopyBridgeMetal>(r);
        }

        ZeroCopyBridgeMetal::Bridged b;
        const auto rc = m_zeroCopyBridge->bridge(m_currentFrame, &b);
        if (rc != ZeroCopyBridgeMetal::Result::Ok) {
            if (rc == ZeroCopyBridgeMetal::Result::Unsupported) {
                qWarning("PlayerRhiRenderer: Metal pixel format not yet supported by Pass 0 bridge — falling back");
            }
            return false;
        }

        m_pass0MatrixIdx = b.colorMatrix;
        m_pass0FullRange = b.fullRange;

        const bool packed = (b.layout ==
                             ZeroCopyBridgeMetal::YuvLayout::AYpCbCr16);
        m_pass0Packed = packed;

        // Lazy create UBO (shared across both pipeline variants).
        if (!m_pass0UBuf) {
            m_pass0UBuf.reset(r->newBuffer(QRhiBuffer::Dynamic,
                                           QRhiBuffer::UniformBuffer, 16));
            m_pass0UBuf->create();
        }

        // Pick the right SRB based on layout. Two SRBs because the
        // packed AYpCbCr16 path has 2 bindings (UBO + single plane)
        // while the biplanar path has 3 (UBO + Y + UV).
        if (packed) {
            if (!m_pass0SrbAyuv) {
                m_pass0SrbAyuv.reset(r->newShaderResourceBindings());
            }
            m_pass0SrbAyuv->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::FragmentStage,
                    m_pass0UBuf.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    b.yPlane, m_planeSampler.get()),
            });
            m_pass0SrbAyuv->create();
        } else {
            if (!m_pass0Srb) {
                m_pass0Srb.reset(r->newShaderResourceBindings());
            }
            m_pass0Srb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::FragmentStage,
                    m_pass0UBuf.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage,
                    b.yPlane,  m_planeSampler.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage,
                    b.uvPlane, m_planeSampler.get()),
            });
            m_pass0Srb->create();
        }

        // Render target tracks m_srcA. Rebuild if missing.
        if (!m_pass0Target) {
            QRhiColorAttachment ca(m_srcA.get());
            m_pass0Target.reset(r->newTextureRenderTarget({ ca }));
            m_pass0PassDesc.reset(m_pass0Target->newCompatibleRenderPassDescriptor());
            m_pass0Target->setRenderPassDescriptor(m_pass0PassDesc.get());
            m_pass0Target->create();
        }

        // Pipeline: created once per variant, render-pass-desc
        // rebound on target recreate.
        if (packed) {
            if (!m_pass0PipelineAyuv) {
                createPass0PipelineAyuv(r);
            } else {
                m_pass0PipelineAyuv->setRenderPassDescriptor(m_pass0PassDesc.get());
                m_pass0PipelineAyuv->create();
            }
        } else {
            if (!m_pass0Pipeline) {
                createPass0Pipeline(r);
            } else {
                m_pass0Pipeline->setRenderPassDescriptor(m_pass0PassDesc.get());
                m_pass0Pipeline->create();
            }
        }
        return true;
    }
#endif

    void createOffscreenTarget(QRhi *r, QSize size)
    {
        // Phase 2.3: RGBA16F as the unconditional working format
        // (Guide 14 §II D2). This is the linear-light intermediate
        // between Pass 1 (compositor) and Pass 2 (OCIO display).
        // Precision matters once OCIO transforms run on this texture
        // — 8-bit linear is unusable for color science.
        m_compositeRaw.reset(r->newTexture(
            QRhiTexture::RGBA16F, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_compositeRaw->create();

        QRhiColorAttachment colorAttachment(m_compositeRaw.get());
        m_offscreenTarget.reset(r->newTextureRenderTarget({ colorAttachment }));
        m_offscreenPassDesc.reset(m_offscreenTarget->newCompatibleRenderPassDescriptor());
        m_offscreenTarget->setRenderPassDescriptor(m_offscreenPassDesc.get());
        m_offscreenTarget->create();
    }

    void createPass1Pipeline(QRhi *r)
    {
        m_pass1Pipeline.reset(r->newGraphicsPipeline());
        m_pass1Pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(":/qcv/render/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, loadShader(":/qcv/render/shaders/compositor.frag.qsb") },
        });

        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pass1Pipeline->setVertexInputLayout(layout);

        m_pass1Pipeline->setShaderResourceBindings(m_pass1Srb.get());
        m_pass1Pipeline->setRenderPassDescriptor(m_offscreenPassDesc.get());
        m_pass1Pipeline->create();
    }

    void createPass2Pipeline(QRhi *r)
    {
        m_pass2Pipeline.reset(r->newGraphicsPipeline());
        m_pass2Pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(":/qcv/render/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, loadShader(":/qcv/render/shaders/passthrough.frag.qsb") },
        });

        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pass2Pipeline->setVertexInputLayout(layout);

        m_pass2Pipeline->setShaderResourceBindings(m_pass2Srb.get());
        m_pass2Pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_pass2Pipeline->create();
    }

    // Phase 2.3: build the OCIO display pass. On success, populates
    // m_pass2OcioBuilt + m_pass2OcioSrb + m_pass2OcioPipeline so Pass 2
    // dispatches OCIO instead of passthrough next frame.
    void buildOcioPass2(QRhi *r, QRhiResourceUpdateBatch *uploads)
    {
        if (!m_ocioBuilder) {
            m_ocioBuilder = std::make_unique<OcioShaderBuilder>(r);
        }

        OcioShaderBuilder::Built built = m_ocioBuilder->build(m_ocio, uploads);
        if (!built.valid) {
            // "active chain incomplete" is the transient case where
            // the user has cleared a slot or is mid-edit — we route
            // to passthrough until they finish. Log at debug so it
            // doesn't dominate the console; other failures (shader
            // compile, OCIO exception) stay loud.
            const bool incomplete =
                built.errorMessage.contains(
                    QStringLiteral("active chain incomplete"));
            if (incomplete) {
                qDebug("PlayerRhiRenderer: OCIO Pass 2 skipped — %s",
                       qPrintable(built.errorMessage));
            } else {
                qWarning("PlayerRhiRenderer: OCIO Pass 2 build failed: %s",
                         qPrintable(built.errorMessage));
            }
            m_ocioBuildFailed = true;
            return;
        }

        // SRB: source texture (compositeRaw) at binding 1, then each
        // OCIO LUT at bindings 2+i. Matches OcioShaderBuilder's
        // rewriteBindings.
        m_pass2OcioSrb.reset(r->newShaderResourceBindings());
        QList<QRhiShaderResourceBinding> bindings;
        bindings.push_back(QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage,
            m_compositeRaw.get(), m_sampler.get()));
        for (size_t i = 0; i < built.luts.size(); ++i) {
            bindings.push_back(QRhiShaderResourceBinding::sampledTexture(
                static_cast<int>(2 + i),
                QRhiShaderResourceBinding::FragmentStage,
                built.luts[i].get(), built.samplers[i].get()));
        }
        m_pass2OcioSrb->setBindings(bindings.cbegin(), bindings.cend());
        m_pass2OcioSrb->create();

        m_pass2OcioPipeline.reset(r->newGraphicsPipeline());
        m_pass2OcioPipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(":/qcv/render/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, built.fragment },
        });

        QRhiVertexInputLayout vlayout;
        vlayout.setBindings({ { 2 * sizeof(float) } });
        vlayout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pass2OcioPipeline->setVertexInputLayout(vlayout);

        m_pass2OcioPipeline->setShaderResourceBindings(m_pass2OcioSrb.get());
        m_pass2OcioPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        m_pass2OcioPipeline->create();

        // Move the Built into a member so its luts + samplers stay
        // alive as long as the pipeline references them.
        m_pass2OcioBuilt = std::move(built);

        qInfo("PlayerRhiRenderer: OCIO Pass 2 built (%lld LUTs)",
              static_cast<long long>(m_pass2OcioBuilt.luts.size()));
    }

#if defined(Q_OS_MACOS)
    void createPass0Pipeline(QRhi *r)
    {
        m_pass0Pipeline.reset(r->newGraphicsPipeline());
        m_pass0Pipeline->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(":/qcv/render/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, loadShader(":/qcv/render/shaders/yuv_to_rgb_biplanar.frag.qsb") },
        });

        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pass0Pipeline->setVertexInputLayout(layout);

        m_pass0Pipeline->setShaderResourceBindings(m_pass0Srb.get());
        m_pass0Pipeline->setRenderPassDescriptor(m_pass0PassDesc.get());
        m_pass0Pipeline->create();
    }

    void createPass0PipelineAyuv(QRhi *r)
    {
        m_pass0PipelineAyuv.reset(r->newGraphicsPipeline());
        m_pass0PipelineAyuv->setShaderStages({
            { QRhiShaderStage::Vertex,   loadShader(":/qcv/render/shaders/fullscreen.vert.qsb") },
            { QRhiShaderStage::Fragment, loadShader(":/qcv/render/shaders/yuv_to_rgb_ayuv.frag.qsb") },
        });

        QRhiVertexInputLayout layout;
        layout.setBindings({ { 2 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float2, 0 } });
        m_pass0PipelineAyuv->setVertexInputLayout(layout);

        m_pass0PipelineAyuv->setShaderResourceBindings(m_pass0SrbAyuv.get());
        m_pass0PipelineAyuv->setRenderPassDescriptor(m_pass0PassDesc.get());
        m_pass0PipelineAyuv->create();
    }
#endif

    // Shared (size-independent)
    std::unique_ptr<QRhiBuffer>                 m_vbuf;
    std::unique_ptr<QRhiBuffer>                 m_pass1UBuf;
    std::unique_ptr<QRhiSampler>                m_sampler;
    std::unique_ptr<QRhiSampler>                m_planeSampler;
    std::unique_ptr<QRhiTexture>                m_srcA;
    std::unique_ptr<QRhiTexture>                m_srcB;
    std::unique_ptr<QRhiShaderResourceBindings> m_pass1Srb;
    std::unique_ptr<QRhiShaderResourceBindings> m_pass2Srb;
    std::unique_ptr<QRhiResourceUpdateBatch>    m_initialUpload;

    // Pass 0 (Metal zero-copy YUV→RGB) — created lazily. Two
    // pipeline / SRB variants:
    //   - "biplanar": Y plane + UV plane (NV12, P010, P210)
    //   - "ayuv":     single packed plane (AYpCbCr16 / ProRes 4444)
    std::unique_ptr<QRhiBuffer>                 m_pass0UBuf;
    std::unique_ptr<QRhiShaderResourceBindings> m_pass0Srb;        // biplanar
    std::unique_ptr<QRhiShaderResourceBindings> m_pass0SrbAyuv;    // packed
    std::unique_ptr<QRhiTextureRenderTarget>    m_pass0Target;
    std::unique_ptr<QRhiRenderPassDescriptor>   m_pass0PassDesc;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pass0Pipeline;     // biplanar
    std::unique_ptr<QRhiGraphicsPipeline>       m_pass0PipelineAyuv; // packed
#if defined(Q_OS_MACOS)
    std::unique_ptr<ZeroCopyBridgeMetal>        m_zeroCopyBridge;
#endif
    int                                          m_pass0MatrixIdx = 1; // BT.709 default
    int                                          m_pass0FullRange = 0;
    bool                                         m_pass0Packed   = false;

    // Currently-displayed frame; held until the next fetchLatest()
    // promotes a new one. Owns the underlying CVPixelBuffer / QImage.
    FrameHandle                                  m_currentFrame;

    // Offscreen target (size-dependent)
    std::unique_ptr<QRhiTexture>                m_compositeRaw;
    std::unique_ptr<QRhiTextureRenderTarget>    m_offscreenTarget;
    std::unique_ptr<QRhiRenderPassDescriptor>   m_offscreenPassDesc;

    // Pipelines
    std::unique_ptr<QRhiGraphicsPipeline>       m_pass1Pipeline;
    std::unique_ptr<QRhiGraphicsPipeline>       m_pass2Pipeline;        // passthrough fallback
    std::unique_ptr<QRhiGraphicsPipeline>       m_pass2OcioPipeline;    // OCIO display pass (Phase 2.3)
    std::unique_ptr<QRhiShaderResourceBindings> m_pass2OcioSrb;
    OcioShaderBuilder::Built                    m_pass2OcioBuilt;
    std::unique_ptr<OcioShaderBuilder>          m_ocioBuilder;
    bool                                        m_ocioBuildFailed = false;
    bool                                        m_ocioRebuildPending = false;
    int                                         m_lastChainGeneration = -1;

    // State synced from PlayerRhiItem
    QSize              m_lastSize;
    QColor             m_clearColor;
    int                m_mode = 0;
    float              m_splitPos = 0.5f;
    VideoDecoder      *m_videoDecoder = nullptr;
    OCIOConfigManager *m_ocio = nullptr;
    ImageSequenceCache *m_imageSeqCache = nullptr;
    int                m_imageSeqPlayhead = 0;

    // Currently-bound source texture for Pass 1. Both the video CPU
    // path and the image-seq pull path upload into m_srcA and rebind
    // here (Pass 1 SRB is rebuilt only when this changes; steady-
    // state playback hits the cheap "no rebind" path).
    QRhiTexture       *m_activeSrcTex = nullptr;
};

} // namespace

PlayerRhiItem::PlayerRhiItem(QQuickItem *parent)
    : QQuickRhiItem(parent)
{
    // Phase 2.6.1d: render into an RGBA16F offscreen so values
    // emitted by Pass 2 (the OCIO display pass) survive the path
    // from PlayerRhiItem → Qt scene graph → window swapchain
    // without being clamped to [0,1]. Default is RGBA8, which
    // tone-maps everything above SDR white before the swapchain
    // ever sees it — visible as a soft roll-off in highlights
    // even with EDR mode active.
    setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA16F);
}

void PlayerRhiItem::setClearColor(const QColor &color)
{
    if (m_clearColor != color) {
        m_clearColor = color;
        emit clearColorChanged();
        update();
    }
}

void PlayerRhiItem::setCompositorMode(CompositorMode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        emit compositorModeChanged();
        update();
    }
}

void PlayerRhiItem::setSplitPos(qreal pos)
{
    pos = qBound(0.0, pos, 1.0);
    if (!qFuzzyCompare(m_splitPos, pos)) {
        m_splitPos = pos;
        emit splitPosChanged();
        update();
    }
}

void PlayerRhiItem::setVideoDecoder(VideoDecoder *decoder)
{
    if (m_videoDecoder == decoder) {
        return;
    }
    if (m_videoDecoder) {
        disconnect(m_videoDecoder, &VideoDecoder::frameAvailable,
                   this, qOverload<>(&QQuickItem::update));
    }
    m_videoDecoder = decoder;
    if (m_videoDecoder) {
        // Queued connection auto-resolved; signal is emitted from the
        // decode thread, slot runs on this item's GUI thread.
        connect(m_videoDecoder, &VideoDecoder::frameAvailable,
                this, qOverload<>(&QQuickItem::update));
    }
    emit videoDecoderChanged();
    update();
}

void PlayerRhiItem::setImageSeqCache(ImageSequenceCache *cache)
{
    if (m_imageSeqCache == cache) return;
    m_imageSeqCache = cache;
    emit imageSeqCacheChanged();
    update();   // wake the render thread so the next render() picks
                // up the new cache pointer (or the null transition).
}

void PlayerRhiItem::setOcio(OCIOConfigManager *ocio)
{
    if (m_ocio == ocio) return;
    if (m_ocio) {
        disconnect(m_ocio, &OCIOConfigManager::activeChainChanged,
                   this, qOverload<>(&QQuickItem::update));
    }
    m_ocio = ocio;
    if (m_ocio) {
        // Without this, slot-machine edits + Engage toggles don't
        // nudge the render thread when no video is playing — the
        // renderer only redraws when something calls update(), and
        // synchronize() reads activeChainGeneration inside that
        // path. Tie the two together so the chain change visibly
        // applies on the next frame.
        connect(m_ocio, &OCIOConfigManager::activeChainChanged,
                this, qOverload<>(&QQuickItem::update));
    }
    emit ocioChanged();
    update();
}

QQuickRhiItemRenderer *PlayerRhiItem::createRenderer()
{
    return new PlayerRhiRenderer;
}

} // namespace qcv
