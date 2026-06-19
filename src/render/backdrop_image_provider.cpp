#include "render/backdrop_image_provider.h"

namespace qcv {

void BackdropImageProvider::setImage(const QImage &img)
{
    QMutexLocker lock(&m_mutex);
    m_image = img;
}

QImage BackdropImageProvider::requestImage(const QString &id, QSize *size,
                                           const QSize &requestedSize)
{
    Q_UNUSED(id);
    Q_UNUSED(requestedSize);   // QML scales; we always serve native dims.
    QMutexLocker lock(&m_mutex);
    if (size) *size = m_image.size();
    return m_image;
}

} // namespace qcv
