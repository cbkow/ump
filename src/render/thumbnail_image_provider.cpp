#include "thumbnail_image_provider.h"

#include "decode/exr_image_loader.h"
#include "decode/image_loader.h"
#include "decode/jpeg_image_loader.h"
#include "decode/png_image_loader.h"
#include "decode/tiff_image_loader.h"
#include "decode/video_image_loader.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace qcv {

namespace {

// Master long-edge — the largest size the inspector requests (the
// hero image's sourceSize). Layer tiles (240) downscale from it.
constexpr int kMasterSize = 480;

// IEEE half → float. Denormals flush to zero, Inf/NaN clamp — fine
// for display thumbnails, and it keeps the provider free of an
// Imath link (Imath is a PRIVATE dep of qcv_decode).
inline float halfToFloat(std::uint16_t h)
{
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp  = (h >> 10) & 0x1Fu;
    const std::uint32_t man  = h & 0x3FFu;
    if (exp == 0) return 0.0f;                       // zero/denormal
    if (exp == 31) {                                  // inf/nan
        float out;
        const std::uint32_t bits = sign | 0x7F7FFFFFu; // ±FLT_MAX
        std::memcpy(&out, &bits, 4);
        return out;
    }
    const std::uint32_t bits = sign | ((exp + 112u) << 23) | (man << 13);
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// Linear → sRGB transfer, clamped to [0,1]. EXR thumbnails carry
// scene-linear values; without the encode they read several stops
// dark. Data AOVs (N, depth, motion) come out "wrong" either way —
// the tiles only need to be tellable-apart, not color-managed.
inline std::uint8_t linearToSrgb8(float c)
{
    if (!(c > 0.0f)) return 0;                        // also catches NaN
    if (c >= 1.0f)   return 255;
    const float s = (c <= 0.0031308f)
        ? c * 12.92f
        : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(s * 255.0f + 0.5f);
}

// PixelData → opaque RGBX8888 QImage. Alpha is composited "over
// black" (RGB kept as-is, alpha forced opaque) — EXR RGB is
// premultiplied by convention, so dropping alpha IS over-black,
// matching what the viewport shows on a fresh load.
QImage pixelDataToImage(const PixelData &pd)
{
    if (pd.width <= 0 || pd.height <= 0 || pd.pixels.empty())
        return QImage();

    QImage img(pd.width, pd.height, QImage::Format_RGBX8888);
    const std::size_t pixelCount =
        static_cast<std::size_t>(pd.width) * pd.height;

    switch (pd.pixel_format) {
    case PixelFormat::RGBA8: {
        if (pd.pixels.size() < pixelCount * 4) return QImage();
        const std::uint8_t *src = pd.pixels.data();
        for (int y = 0; y < pd.height; ++y) {
            std::uint8_t *dst = img.scanLine(y);
            const std::uint8_t *row =
                src + static_cast<std::size_t>(y) * pd.width * 4;
            for (int x = 0; x < pd.width; ++x) {
                dst[x * 4 + 0] = row[x * 4 + 0];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = row[x * 4 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        return img;
    }
    case PixelFormat::RGBA16: {
        if (pd.pixels.size() < pixelCount * 8) return QImage();
        const std::uint16_t *src =
            reinterpret_cast<const std::uint16_t *>(pd.pixels.data());
        for (int y = 0; y < pd.height; ++y) {
            std::uint8_t *dst = img.scanLine(y);
            const std::uint16_t *row =
                src + static_cast<std::size_t>(y) * pd.width * 4;
            for (int x = 0; x < pd.width; ++x) {
                dst[x * 4 + 0] = row[x * 4 + 0] >> 8;
                dst[x * 4 + 1] = row[x * 4 + 1] >> 8;
                dst[x * 4 + 2] = row[x * 4 + 2] >> 8;
                dst[x * 4 + 3] = 255;
            }
        }
        return img;
    }
    case PixelFormat::RGBA16F: {
        if (pd.pixels.size() < pixelCount * 8) return QImage();
        const std::uint16_t *src =
            reinterpret_cast<const std::uint16_t *>(pd.pixels.data());
        for (int y = 0; y < pd.height; ++y) {
            std::uint8_t *dst = img.scanLine(y);
            const std::uint16_t *row =
                src + static_cast<std::size_t>(y) * pd.width * 4;
            for (int x = 0; x < pd.width; ++x) {
                dst[x * 4 + 0] = linearToSrgb8(halfToFloat(row[x * 4 + 0]));
                dst[x * 4 + 1] = linearToSrgb8(halfToFloat(row[x * 4 + 1]));
                dst[x * 4 + 2] = linearToSrgb8(halfToFloat(row[x * 4 + 2]));
                dst[x * 4 + 3] = 255;
            }
        }
        return img;
    }
    }
    return QImage();
}

// Downscale a master to what QML asked for (never upscale — the
// master is the largest size we serve; QML fits the rest).
QImage scaleForRequest(const QImage &master, const QSize &req)
{
    if (master.isNull() || (req.width() <= 0 && req.height() <= 0))
        return master;
    const QSize target = master.size().scaled(
        req.width()  > 0 ? req.width()  : master.width(),
        req.height() > 0 ? req.height() : master.height(),
        Qt::KeepAspectRatio);
    if (target.isEmpty()
        || (target.width() >= master.width()
            && target.height() >= master.height())) {
        return master;
    }
    return master.scaled(target, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
}

QString masterKey(const QString &path, const QString &layer,
                  const QString &frame)
{
    return path + QLatin1Char('\x1f') + layer
         + QLatin1Char('\x1f') + frame;
}

// Async response with cancellation (ufb's UfbImageResponse pattern).
// cancel() flips an atomic the runnable checks before decoding, so
// tiles torn down while queued cost nothing.
class ThumbResponse final : public QQuickImageResponse {
public:
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    // A null decode must surface as Image.Error, not a blank Ready —
    // the QML placeholder icons gate on status === Image.Error.
    QString errorString() const override {
        return m_image.isNull()
            ? QStringLiteral("thumb: no image produced") : QString();
    }
    void cancel() override { m_cancelled.storeRelease(1); }
    bool isCancelled() const { return m_cancelled.loadAcquire() != 0; }
    void resolve(const QImage &img) {
        // Queued onto the response's thread (GUI) — set the image
        // before QML sees finished().
        QMetaObject::invokeMethod(this, [this, img]() {
            m_image = img;
            emit finished();
        }, Qt::QueuedConnection);
    }
private:
    QImage m_image;
    QAtomicInt m_cancelled{0};
};

class ThumbRunnable final : public QRunnable {
public:
    ThumbRunnable(ThumbnailImageProvider *provider, ThumbResponse *response,
                  QString path, QString layer, QString frame, QSize req)
        : m_provider(provider), m_response(response)
        , m_path(std::move(path)), m_layer(std::move(layer))
        , m_frame(std::move(frame)), m_req(req) {}

    void run() override {
        if (m_response->isCancelled()) {
            m_response->resolve(QImage());
            return;
        }
        const QImage master =
            m_provider->masterFor(m_path, m_layer, m_frame);
        m_response->resolve(scaleForRequest(master, m_req));
    }

private:
    ThumbnailImageProvider *m_provider;
    ThumbResponse *m_response;
    QString m_path, m_layer, m_frame;
    QSize m_req;
};

} // namespace

ThumbnailImageProvider::ThumbnailImageProvider()
    // Cost unit = bytes. 64 MB absorbs a hero + a few dozen layer
    // masters across a couple of A/B flips before evicting.
    : m_cache(64 * 1024 * 1024)
{
    // 4 workers (ufb-tuned): enough to keep a layer grid filling
    // concurrently without saturating the CPU under playback.
    m_pool.setMaxThreadCount(4);
    m_pool.setExpiryTimeout(5000);
}

ThumbnailImageProvider::~ThumbnailImageProvider()
{
    // Runnables capture `this` — drain before members die.
    m_pool.clear();
    m_pool.waitForDone();
}

QQuickImageResponse *ThumbnailImageProvider::requestImageResponse(
    const QString &id, const QSize &requestedSize)
{
    // Split "<path>?layer=...&frame=..." — '?' cannot occur in a
    // file path, so the first one starts the query. QML callers
    // percent-encode the path and layer; async providers receive
    // the id still-encoded (Qt does NOT decode it — see ufb's
    // providers, which hit the same thing), so decode explicitly.
    QString path  = id;
    QString layer;
    QString frame;
    const int qPos = id.indexOf(QLatin1Char('?'));
    if (qPos >= 0) {
        path = id.left(qPos);
        const QUrlQuery query(id.mid(qPos + 1));
        layer = query.queryItemValue(QStringLiteral("layer"),
                                     QUrl::FullyDecoded);
        frame = query.queryItemValue(QStringLiteral("frame"),
                                     QUrl::FullyDecoded);
    }
    if (path.contains(QLatin1Char('%'))) {
        const QString decoded =
            QUrl::fromPercentEncoding(path.toUtf8());
        if (!decoded.isEmpty() && QFileInfo::exists(decoded))
            path = decoded;
    }

    auto *response = new ThumbResponse;
    m_pool.start(new ThumbRunnable(this, response, path, layer, frame,
                                   requestedSize));
    return response;
}

QImage ThumbnailImageProvider::masterFor(const QString &path,
                                         const QString &layer,
                                         const QString &frame)
{
    const QString key = masterKey(path, layer, frame);
    QImage master = cacheGet(key);
    if (!master.isNull()) return master;

    // EXR contact-sheet batch: the first requester decodes EVERY
    // layer in one scanline sweep and caches them all; concurrent
    // requesters for sibling layers wait here instead of paying a
    // full decompress each. Multi-part files return an empty map
    // (per-part reads share no work) and fall through to the
    // single-layer path below.
    const bool isExr = frame.isEmpty()
        && QFileInfo(path).suffix().compare(
               QLatin1String("exr"), Qt::CaseInsensitive) == 0;
    if (isExr) {
        {
            QMutexLocker lock(&m_batchMutex);
            while (m_batchInFlight.contains(path)) {
                m_batchDone.wait(&m_batchMutex);
            }
            master = cacheGet(key);
            if (!master.isNull()) return master;
            m_batchInFlight.insert(path);
        }

        QElapsedTimer batchTimer;
        batchTimer.start();
        const auto all = EXRImageLoader::loadThumbnailsAllLayers(
            path.toStdString(), kMasterSize);
        for (const auto &[layerName, pd] : all) {
            if (!pd) continue;
            const QImage img = pixelDataToImage(*pd);
            if (img.isNull()) continue;
            cachePut(masterKey(path,
                               QString::fromStdString(layerName), frame),
                     img);
        }
        if (!all.empty()) {
            qInfo("ThumbnailImageProvider: batched %zu EXR layer "
                  "thumbnails in %lld ms (%s)",
                  all.size(),
                  static_cast<long long>(batchTimer.elapsed()),
                  qPrintable(QFileInfo(path).fileName()));
        }

        {
            QMutexLocker lock(&m_batchMutex);
            m_batchInFlight.remove(path);
            m_batchDone.wakeAll();
        }

        master = cacheGet(key);
        if (!master.isNull()) return master;
        // Batch didn't cover this request (multi-part file, or a
        // layer name the sweep didn't resolve) — single-layer path.
    }

    master = decodeMaster(path, layer, frame);
    if (master.isNull()) {
        qWarning("ThumbnailImageProvider: no thumbnail for '%s' "
                 "(layer='%s' frame='%s')",
                 qPrintable(path), qPrintable(layer), qPrintable(frame));
        return QImage();
    }
    cachePut(key, master);
    return master;
}

QImage ThumbnailImageProvider::decodeMaster(const QString &path,
                                            const QString &layer,
                                            const QString &frame)
{
    const std::string pathStd = path.toStdString();

    // frame present ⇒ video container (QML only sends it for
    // MediaType::Video). One throwaway FFmpeg context per request —
    // this runs once per media load, not per hover.
    if (!frame.isEmpty()) {
        VideoImageLoader loader(pathStd);
        if (!loader.isInitialized()) return QImage();
        int frameNo = 0;
        if (frame == QLatin1String("mid")) {
            frameNo = loader.frameCount() / 2;
        } else {
            bool ok = false;
            frameNo = frame.toInt(&ok);
            if (!ok) frameNo = 0;
        }
        const auto pd = loader.loadThumbnail(
            std::to_string(frameNo), kMasterSize);
        return pd ? pixelDataToImage(*pd) : QImage();
    }

    const QString ext = QFileInfo(path).suffix().toLower();
    std::unique_ptr<IImageLoader> loader;
    if (ext == QLatin1String("exr")) {
        auto exr = std::make_unique<EXRImageLoader>();
        // "default" is discoverLayers' synthetic name for bare-RGB
        // files; the loader's channel fallback resolves it, so pass
        // it through like the timeline cache does.
        exr->setLayer(layer.toStdString());
        loader = std::move(exr);
    } else if (ext == QLatin1String("png")) {
        loader = std::make_unique<PNGImageLoader>();
    } else if (ext == QLatin1String("tif")
               || ext == QLatin1String("tiff")) {
        loader = std::make_unique<TIFFImageLoader>();
    } else if (ext == QLatin1String("jpg")
               || ext == QLatin1String("jpeg")) {
        loader = std::make_unique<JPEGImageLoader>();
    }

    if (loader) {
        const auto pd = loader->loadThumbnail(pathStd, kMasterSize);
        if (pd && !pd->is_sentinel) {
            QImage img = pixelDataToImage(*pd);
            // Not every loader has a real thumbnail fast path (TIFF
            // falls back to a full-res loadFrame) — clamp so QML
            // never holds a full-res still.
            if (!img.isNull()
                && std::max(img.width(), img.height()) > kMasterSize) {
                img = img.scaled(kMasterSize, kMasterSize,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
            }
            if (!img.isNull()) return img;
        }
        return QImage();
    }

    // Anything else (bmp, webp, gif, …): Qt's own readers, scaled
    // at decode time so we never hold a full-res still.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid()
        && std::max(full.width(), full.height()) > kMasterSize) {
        const QSize scaled =
            full.scaled(kMasterSize, kMasterSize, Qt::KeepAspectRatio);
        reader.setScaledSize(scaled);
    }
    return reader.read();
}

QImage ThumbnailImageProvider::cacheGet(const QString &key)
{
    QMutexLocker lock(&m_cacheMutex);
    const QImage *hit = m_cache.object(key);
    return hit ? *hit : QImage();
}

void ThumbnailImageProvider::cachePut(const QString &key, const QImage &img)
{
    QMutexLocker lock(&m_cacheMutex);
    m_cache.insert(key, new QImage(img),
                   std::max<qsizetype>(1, img.sizeInBytes()));
}

} // namespace qcv
