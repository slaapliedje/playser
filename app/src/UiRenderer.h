#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QTimer>

#include "OverlayEncoder.h"

class Mpv;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;

// Renders the QML UI off-screen (Qt Quick software backend, no window of its
// own) and hands each changed frame to mpv as overlay 0, scaled to mpv's
// window and encoded for the video's transfer function. Input is delivered
// with deliverKey(); there is no native input to the off-screen window.
class UiRenderer : public QObject
{
    Q_OBJECT
public:
    // The UI is laid out at this height and mpv scales it to the window,
    // keeping software rendering cheap at 4K.
    static constexpr int kHeight = 1080;

    UiRenderer(Mpv *mpv, QObject *parent = nullptr);
    ~UiRenderer() override;

    // Takes ownership of `root` and lays it out over the whole window.
    void setRootItem(QQuickItem *root);
    QQuickWindow *window() const { return m_window; }
    void deliverKey(int qtKey);
    // The last rendered UI frame (sRGB), for test screenshots.
    QImage lastFrame() const { return m_image; }

private:
    void scheduleRender();
    void render();
    void updateSize();

    Mpv *m_mpv;
    QQuickRenderControl *m_control;
    QQuickWindow *m_window;
    QQuickItem *m_root = nullptr;
    QImage m_image;
    QImage m_sent;
    Mpv::Transfer m_sentTransfer = Mpv::Transfer::Sdr;
    OverlayEncoder m_encoder;
    QTimer m_timer;
    QElapsedTimer m_sinceRender;
    bool m_needSync = true;
};
