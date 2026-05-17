// PlayerRhiItem — the QQuickRhiItem subclass that hosts the player
// window's render pipeline. Composite + OCIO + (eventually) stroke
// + safety-guide passes; HDR-aware swapchain.

#pragma once

#include "color/ocio_config_manager.h"
#include "decode/image_sequence_cache.h"
#include "decode/video_decoder.h"

#include <QColor>
#include <QQuickRhiItem>
#include <QtQmlIntegration>

namespace qcv {

class PlayerRhiItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(QColor clearColor READ clearColor WRITE setClearColor NOTIFY clearColorChanged FINAL)
    Q_PROPERTY(CompositorMode compositorMode READ compositorMode WRITE setCompositorMode NOTIFY compositorModeChanged FINAL)
    Q_PROPERTY(qreal splitPos READ splitPos WRITE setSplitPos NOTIFY splitPosChanged FINAL)
    Q_PROPERTY(qcv::VideoDecoder *videoDecoder READ videoDecoder WRITE setVideoDecoder NOTIFY videoDecoderChanged FINAL)
    Q_PROPERTY(qcv::OCIOConfigManager *ocio READ ocio WRITE setOcio NOTIFY ocioChanged FINAL)
    // Phase 7.4.b.4 — pull-model image sequence. The renderer asks
    // this cache for the current playhead frame each present, builds
    // a non-owning QImage view, and uploads to m_srcA. When set, the
    // renderer takes the image-seq pull path; when null, the video
    // fetchLatest() path. Replaces the FFmpeg-shaped per-frame publish.
    Q_PROPERTY(qcv::ImageSequenceCache *imageSeqCache
               READ imageSeqCache WRITE setImageSeqCache
               NOTIFY imageSeqCacheChanged FINAL)
    QML_ELEMENT

public:
    enum CompositorMode : int {
        Single      = 0,
        SideBySide  = 1,
        SplitWipe   = 2,
    };
    Q_ENUM(CompositorMode)

    explicit PlayerRhiItem(QQuickItem *parent = nullptr);

    QColor clearColor() const { return m_clearColor; }
    void setClearColor(const QColor &color);

    CompositorMode compositorMode() const { return m_mode; }
    void setCompositorMode(CompositorMode mode);

    qreal splitPos() const { return m_splitPos; }
    void setSplitPos(qreal pos);

    VideoDecoder *videoDecoder() const { return m_videoDecoder; }
    void setVideoDecoder(VideoDecoder *decoder);

    OCIOConfigManager *ocio() const { return m_ocio; }
    void setOcio(OCIOConfigManager *ocio);

    ImageSequenceCache *imageSeqCache() const { return m_imageSeqCache; }
    void setImageSeqCache(ImageSequenceCache *cache);

signals:
    void clearColorChanged();
    void compositorModeChanged();
    void splitPosChanged();
    void videoDecoderChanged();
    void ocioChanged();
    void imageSeqCacheChanged();

protected:
    QQuickRhiItemRenderer *createRenderer() override;

private:
    QColor             m_clearColor{ 0x0a, 0x1a, 0x2a };  // dark blue-gray
    CompositorMode     m_mode = Single;
    qreal              m_splitPos = 0.5;
    VideoDecoder      *m_videoDecoder = nullptr;
    OCIOConfigManager *m_ocio = nullptr;
    ImageSequenceCache *m_imageSeqCache = nullptr;
};

} // namespace qcv
