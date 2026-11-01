// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include "Mpv.h"
#include "OverlayEncoder.h"

// A "3D" logo in the top-right corner, shown while 3D glasses are
// connected. It is an extruded block of layers, each further in front of
// the screen than the one behind it: on a side-by-side display every layer
// is drawn shifted towards the nose by half its depth in each eye, so the
// logo stands out of the screen as an object; flat elsewhere. Its own mpv
// overlay, over the UI's.
class StereoBadge : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(Mpv *mpv READ mpv WRITE setMpv NOTIFY mpvChanged)
    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)

public:
    explicit StereoBadge(QObject *parent = nullptr);

    Mpv *mpv() const { return m_mpv; }
    void setMpv(Mpv *mpv);
    bool shown() const { return m_shown; }
    void setShown(bool shown);

signals:
    void mpvChanged();
    void shownChanged();

private:
    void update();
    // one eye's view of the logo, `eye` -1 (left), +1 (right) or 0 (flat)
    QImage draw(const QSize &size, int eye) const;

    QPointer<Mpv> m_mpv;
    bool m_shown = false;
    OverlayEncoder m_encoder;
};
