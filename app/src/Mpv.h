#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

struct mpv_handle;

// libmpv player that owns its own window (vo=gpu-next). The app has no other
// window: video, disc menus and the QML UI are all composited by mpv, the UI
// as an overlay-add bitmap (see UiRenderer). This keeps HDR output intact,
// which the OpenGL render API can't provide (docs/spike-results.md, 0c/0d).
//
// Everything going to mpv is async; state comes back as observed properties.
class Mpv : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("created by the application")

    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(bool idle READ idle NOTIFY idleChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY chapterChanged)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY chaptersChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)

public:
    // Transfer function the video (and so every overlay) is encoded with.
    enum class Transfer { Sdr, Pq, Hlg };

    explicit Mpv(bool fullscreen, QObject *parent = nullptr);
    ~Mpv() override;

    // Sets per-file options (e.g. bluray-device), then loads url.
    Q_INVOKABLE void load(const QString &url, const QVariantMap &options = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void command(const QVariantList &args);
    Q_INVOKABLE void setMpvProperty(const QString &name, const QVariant &value);

    // Shows a premultiplied BGRA image (QImage::Format_ARGB32_Premultiplied)
    // scaled over the whole window; a null image removes the overlay. Only
    // one overlay-add per id is in flight; newer images replace queued ones.
    void setOverlay(int id, const QImage &image);
    void screenshot(const QString &path);

    bool paused() const { return m_paused; }
    bool idle() const { return m_idle; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    QString mediaTitle() const { return m_mediaTitle; }
    int chapter() const { return m_chapter; }
    QVariantList chapters() const { return m_chapters; }
    QVariantList tracks() const { return m_tracks; }
    QSize osdSize() const { return m_osdSize; }
    Transfer transfer() const { return m_transfer; }

signals:
    void pausedChanged();
    void idleChanged();
    void positionChanged();
    void durationChanged();
    void mediaTitleChanged();
    void chapterChanged();
    void chaptersChanged();
    void tracksChanged();
    void osdSizeChanged();
    void transferChanged();
    void fileLoaded();
    void endFile(const QString &reason);
    // a key pressed in mpv's window, by mpv key name ("UP", "ENTER", "i")
    void keyPressed(const QString &name);
    void closeRequested();

private slots:
    void processEvents();

private:
    struct OverlaySlot {
        quint64 inFlight = 0;   // reply id of the pending overlay-add/remove
        QImage sending;         // keeps the bitmap alive until mpv copied it
        QImage queued;
        bool hasQueued = false;
        bool shown = false;
    };
    void sendOverlay(int id, const QImage &image);
    void commandAsync(const QVariantList &args, quint64 replyId = 0);

    mpv_handle *m_mpv = nullptr;
    bool m_paused = false;
    bool m_idle = true;
    double m_position = 0;
    double m_duration = 0;
    QString m_mediaTitle;
    int m_chapter = -1;
    QVariantList m_chapters;
    QVariantList m_tracks;
    QSize m_osdSize;
    Transfer m_transfer = Transfer::Sdr;
    QHash<int, OverlaySlot> m_overlays;
    quint64 m_nextReply = 1000;
};
