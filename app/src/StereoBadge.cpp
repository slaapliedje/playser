// SPDX-License-Identifier: GPL-3.0-or-later
#include "StereoBadge.h"

#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace {

// above the disc's menu graphics (0) and the UI (1)
constexpr int kBadgeOverlay = 2;
// the block's depth (behind the face, down and right), as a share of its height
constexpr double kExtrusion = 0.14;
// at the front, as a share of one eye's width: about 22 px at 1920, a
// comfortable pop-out
constexpr double kFrontDisparity = 0.0115;

} // namespace

StereoBadge::StereoBadge(QObject *parent) : QObject(parent)
{
}

void StereoBadge::setMpv(Mpv *mpv)
{
    if (mpv == m_mpv)
        return;
    if (m_mpv)
        disconnect(m_mpv, nullptr, this, nullptr);
    m_mpv = mpv;
    if (m_mpv) {
        // the eye's view changes size when the display switches 2D/3D
        connect(m_mpv, &Mpv::osdSizeChanged, this, &StereoBadge::update);
        connect(m_mpv, &Mpv::transferChanged, this, &StereoBadge::update);
    }
    emit mpvChanged();
    update();
}

void StereoBadge::setShown(bool shown)
{
    if (shown == m_shown)
        return;
    m_shown = shown;
    emit shownChanged();
    update();
}

void StereoBadge::update()
{
    if (!m_mpv)
        return;
    const QSize view = m_mpv->osdSize();
    if (!m_shown || view.isEmpty()) {
        m_mpv->setOverlay(kBadgeOverlay, QImage());
        return;
    }
    const int h = view.height() * 13 / 100;
    const QSize size(h * 2, h);
    const int margin = view.height() * 6 / 100;
    const QRect dst(QPoint(view.width() - margin - size.width(), margin), size);
    const Mpv::Transfer transfer = m_mpv->transfer();
    if (m_mpv->sideBySide())
        m_mpv->setOverlayEyes(kBadgeOverlay, m_encoder.encode(draw(size, -1), transfer),
                              m_encoder.encode(draw(size, +1), transfer), dst);
    else
        m_mpv->setOverlay(kBadgeOverlay, m_encoder.encode(draw(size, 0), transfer), dst);
}

QImage StereoBadge::draw(const QSize &size, int eye) const
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    QFont font = QGuiApplication::font();
    font.setPixelSize(size.height() * 80 / 100);
    font.setWeight(QFont::Black);
    QPainterPath text;
    text.addText(0, 0, font, QStringLiteral("3D"));
    // a variable font's glyphs overlap their own contours: merged, or the
    // overlaps are holes and the outline traces them
    text.setFillRule(Qt::WindingFill);
    text = text.simplified();
    const QRectF box = text.boundingRect();
    // the face centred, the block behind it reaching down and right
    const double extrusion = size.height() * kExtrusion;
    const QPointF face(size.width() / 2.0 - box.center().x() - extrusion / 2,
                       size.height() / 2.0 - box.center().y() - extrusion / 2);
    const double frontDisparity = m_mpv ? m_mpv->osdSize().width() * kFrontDisparity : 0;
    // enough layers that each moves at most a pixel from the one behind
    const int layers = qMax(2, int(std::ceil(extrusion + frontDisparity / 2)) + 1);

    for (int i = 0; i < layers; i++) {
        const double depth = double(i) / (layers - 1); // 0 back (the screen) .. 1 front
        // crossed disparity: the left eye's copy moves right, the right's left
        const double shift = -eye * frontDisparity * depth / 2;
        const double back = (1 - depth) * extrusion;
        QPainterPath layer = text.translated(face + QPointF(back + shift, back));
        if (i < layers - 1) {
            // the block's sides: amber, darker towards the back
            const int shade = 70 + int(110 * depth);
            p.fillPath(layer, QColor(shade + 60, shade * 3 / 4 + 20, shade / 6));
        } else {
            QLinearGradient lit(layer.boundingRect().topLeft(), layer.boundingRect().bottomLeft());
            lit.setColorAt(0, QColor("#ffe6a3"));
            lit.setColorAt(0.55, QColor("#f5b942"));
            lit.setColorAt(1, QColor("#d48a1c"));
            p.fillPath(layer, lit);
            p.strokePath(layer, QPen(QColor(255, 248, 225, 200), size.height() * 0.012));
        }
    }
    return img;
}
