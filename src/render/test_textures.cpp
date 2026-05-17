#include "test_textures.h"

#include <QPainter>
#include <QPen>
#include <QFont>
#include <QLinearGradient>

namespace qcv {

namespace {

// 75% intensity SMPTE bars in standard left→right order
// (white / yellow / cyan / green / magenta / red / blue).
constexpr QColor kBars[7] = {
    QColor(191, 191, 191),   // White 75%
    QColor(191, 191,   0),   // Yellow
    QColor(  0, 191, 191),   // Cyan
    QColor(  0, 191,   0),   // Green
    QColor(191,   0, 191),   // Magenta
    QColor(191,   0,   0),   // Red
    QColor(  0,   0, 191),   // Blue
};

void drawBadge(QPainter &p, const QRect &rect, const QString &text, QColor fg)
{
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(qMax(48, rect.height() / 4));
    p.setFont(f);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 200));
    p.drawRoundedRect(rect, 8, 8);

    p.setPen(fg);
    p.drawText(rect, Qt::AlignCenter, text);
}

} // namespace

QImage generateSourceA(QSize size)
{
    QImage img(size, QImage::Format_RGBA8888);
    img.fill(Qt::black);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);

    // 7 vertical bars (75% saturation) in the top 75% of the image.
    const int barsBottom = size.height() * 3 / 4;
    const float barW = static_cast<float>(size.width()) / 7.0f;
    for (int i = 0; i < 7; ++i) {
        QRectF r(i * barW, 0.0f, barW + 1.0f, barsBottom);
        p.fillRect(r, kBars[i]);
    }

    // 4 luminance reference patches in the bottom 25%.
    const int luma[4] = { 0, 84, 168, 255 };
    const float patchW = static_cast<float>(size.width()) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        QRectF r(i * patchW, barsBottom, patchW + 1.0f,
                 size.height() - barsBottom);
        p.fillRect(r, QColor(luma[i], luma[i], luma[i]));
    }

    // Thick blue border so SOURCE A is identifiable at a glance.
    QPen border(QColor(50, 130, 255));
    border.setWidth(8);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(4, 4, size.width() - 8, size.height() - 8));

    // "A" badge in the upper-left corner.
    drawBadge(p, QRect(24, 24, 96, 96), "A", QColor(50, 130, 255));

    p.end();
    return img;
}

QImage generateSourceB(QSize size)
{
    QImage img(size, QImage::Format_RGBA8888);
    img.fill(QColor(40, 40, 40));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Four quadrants, each a different solid color so the source's
    // orientation (top vs bottom, left vs right) is unambiguous.
    const int hw = size.width()  / 2;
    const int hh = size.height() / 2;
    p.fillRect(QRect(0,  0,  hw, hh),                QColor(230, 130,  40)); // TL orange
    p.fillRect(QRect(hw, 0,  size.width() - hw, hh), QColor( 40, 180, 180)); // TR teal
    p.fillRect(QRect(0,  hh, hw, size.height() - hh),QColor(200,  60, 180)); // BL magenta
    p.fillRect(QRect(hw, hh, size.width() - hw,
                     size.height() - hh),            QColor(140, 210,  60)); // BR lime

    // Thin black cross between the quadrants for visual separation.
    QPen sep(QColor(0, 0, 0, 180));
    sep.setWidth(2);
    p.setPen(sep);
    p.drawLine(hw, 0, hw, size.height());
    p.drawLine(0, hh, size.width(), hh);

    // Thick yellow border so SOURCE B is identifiable at a glance.
    QPen border(QColor(255, 220, 50));
    border.setWidth(8);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRect(4, 4, size.width() - 8, size.height() - 8));

    // "B" badge in the upper-left corner.
    drawBadge(p, QRect(24, 24, 96, 96), "B", QColor(255, 220, 50));

    p.end();
    return img;
}

} // namespace qcv
