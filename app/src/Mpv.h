#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>
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
    // codecs to send to the receiver undecoded (mpv audio-spdif), e.g.
    // "ac3,truehd"; empty decodes everything
    Q_PROPERTY(QString passthrough READ passthrough WRITE setPassthrough NOTIFY passthroughChanged)

public:
    // Transfer function the video (and so every overlay) is encoded with.
    enum class Transfer { Sdr, Pq, Hlg };

    // options: extra "name=value" mpv options (testing)
    explicit Mpv(bool fullscreen, const QStringList &options = {}, QObject *parent = nullptr);
    ~Mpv() override;

    // Sets options (e.g. bluray-device), then loads url, starting at
    // `start` seconds if > 0 (a per-file option, so it doesn't stick).
    Q_INVOKABLE void load(const QString &url, const QVariantMap &options = {}, double start = 0);
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
    QString passthrough() const { return m_passthrough; }
    void setPassthrough(const QString &codecs);
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
    void passthroughChanged();
    void osdSizeChanged();
    void transferChanged();
    void fileLoaded();
    // audio output stalled playback and was worked around (AudioWatchdog);
    // message is for the viewer
    void audioProblem(const QString &message);
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
    void checkAudioStall();
    void applySpdif(bool on);
    void onAudioOutputBusy();
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
    qint64 m_currentEntry = -1; // playlist entry of the latest START_FILE

    // audio watchdog: a failing audio output (e.g. PipeWire "no target node"
    // for passthrough, docs/spike-results.md 0c) stops mpv's clock and
    // freezes the picture
    QTimer m_audioWatchdog;
    bool m_loaded = false;
    bool m_seeking = false;
    bool m_pausedForCache = false;
    bool m_hasAudio = false;
    QString m_spdif;
    double m_lastPosition = -1;
    int m_stalledSecs = 0;
    int m_recovery = 0; // steps taken for this file

    // passthrough: off for the session once it stalled; while the output is
    // busy (another stream on it, e.g. Steam's UI sounds for ~18 s after one
    // plays) decoded now and retried once later in the file
    QString m_passthrough;
    bool m_passthroughFailed = false;
    bool m_outputBusy = false;
    bool m_busyRetried = false;
};
