// SPDX-License-Identifier: GPL-3.0-or-later
#include "UiRenderer.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>

#include "Mpv.h"

namespace {

constexpr int kMinFrameMs = 33;
// above the disc's menu graphics (DiscNav, overlay 0)
constexpr int kUiOverlay = 1;

bool fullyTransparent(const QImage &img)
{
    for (int y = 0; y < img.height(); y++) {
        const auto *px = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++)
            if (qAlpha(px[x]))
                return false;
    }
    return true;
}

} // namespace

UiRenderer::UiRenderer(Mpv *mpv, QObject *parent)
    : QObject(parent), m_mpv(mpv), m_control(new QQuickRenderControl),
      m_window(new QQuickWindow(m_control))
{
    Q_ASSERT(QQuickWindow::graphicsApi() == QSGRendererInterface::Software);
    m_window->setColor(Qt::transparent);
    m_sinceRender.start();

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &UiRenderer::render);
    connect(m_control, &QQuickRenderControl::renderRequested, this, &UiRenderer::scheduleRender);
    connect(m_control, &QQuickRenderControl::sceneChanged, this, &UiRenderer::scheduleRender);
    connect(m_mpv, &Mpv::osdSizeChanged, this, &UiRenderer::updateSize);
    // the same pixels need re-encoding (and resending) for the new video
    connect(m_mpv, &Mpv::transferChanged, this, &UiRenderer::scheduleRender);

    updateSize();
    // no initialize(): that sets up a QRhi, which the software backend
    // doesn't use (and which fails without a GPU context)
    // the window never gets real focus; without this nothing has active focus
    QFocusEvent focusIn(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(m_window, &focusIn);
}

UiRenderer::~UiRenderer()
{
    delete m_root;
    delete m_control;
    delete m_window;
}

void UiRenderer::setRootItem(QQuickItem *root)
{
    m_root = root;
    root->setParentItem(m_window->contentItem());
    root->setSize(m_window->size());
    root->forceActiveFocus();
    scheduleRender();
}

void UiRenderer::deliverKey(int qtKey)
{
    QKeyEvent press(QEvent::KeyPress, qtKey, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, qtKey, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &press);
    QCoreApplication::sendEvent(m_window, &release);
}

void UiRenderer::deliverText(QChar c)
{
    // Qt's key codes for printable characters are their upper case
    const int key = c.toUpper().unicode();
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(c));
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(c));
    QCoreApplication::sendEvent(m_window, &press);
    QCoreApplication::sendEvent(m_window, &release);
}

void UiRenderer::updateSize()
{
    // fixed height, width following the window's aspect ratio
    const QSize osd = m_mpv->osdSize();
    const int width = osd.isEmpty() ? kHeight * 16 / 9 : int(qRound(double(kHeight) * osd.width() / osd.height()));
    const QSize size(width, kHeight);
    // the display size mpv scales to changed: resend even if ours didn't
    m_sent = QImage();
    if (size == m_image.size()) {
        scheduleRender();
        return;
    }
    m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    m_image.fill(Qt::transparent);
    m_window->setGeometry(0, 0, size.width(), size.height());
    m_window->setRenderTarget(QQuickRenderTarget::fromPaintDevice(&m_image));
    if (m_root)
        m_root->setSize(size);
    m_sent = QImage();
    scheduleRender();
}

void UiRenderer::scheduleRender()
{
    if (m_timer.isActive())
        return;
    m_timer.start(qMax<qint64>(0, kMinFrameMs - m_sinceRender.elapsed()));
}

void UiRenderer::render()
{
    m_sinceRender.restart();
    // software backend: no beginFrame()/endFrame(), those drive a QRhi
    m_control->polishItems();
    m_control->sync();
    m_control->render();

    // m_image is painted in place (the software renderer only repaints dirty
    // regions); m_sent shares the previous frame until the next paint detaches
    const Mpv::Transfer transfer = m_mpv->transfer();
    if (m_image == m_sent && transfer == m_sentTransfer)
        return;
    m_sent = m_image;
    m_sentTransfer = transfer;
    if (fullyTransparent(m_image))
        m_mpv->setOverlay(kUiOverlay, QImage());
    else
        m_mpv->setOverlay(kUiOverlay, m_encoder.encode(m_image, transfer));
}
