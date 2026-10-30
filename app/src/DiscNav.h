#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include "Mpv.h"
#include "OverlayEncoder.h"

struct NavContext;

// Disc menus (DVD, Blu-ray): libdiscnav runs the disc's VM and feeds mpv through a
// "discnav://" stream_cb protocol, one mpv file per segment; this reloads
// the next segment when one ends (or at once when a user action jumped),
// selects the tracks the disc asks for, and draws the menu graphics as an
// overlay under the UI.
class DiscNav : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(Mpv *mpv READ mpv WRITE setMpv NOTIFY mpvChanged)
    // a disc is playing through its menus
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // in a menu (d-pad and A go to its buttons) rather than a title
    Q_PROPERTY(bool inMenu READ inMenu NOTIFY inMenuChanged)
    // "dvd" or "bluray" once open, else ""
    Q_PROPERTY(QString format READ format NOTIFY activeChanged)
    // the title's streams as the disc describes them, for the track picker:
    // [{ id (for selectStream), label, selected }]; empty in a menu
    Q_PROPERTY(QVariantList audioStreams READ audioStreams NOTIFY streamsChanged)
    Q_PROPERTY(QVariantList subtitleStreams READ subtitleStreams NOTIFY streamsChanged)

public:
    explicit DiscNav(QObject *parent = nullptr);
    ~DiscNav() override;

    Mpv *mpv() const { return m_mpv; }
    void setMpv(Mpv *mpv);
    bool active() const { return m_ctx != nullptr; }
    bool inMenu() const { return m_inMenu; }
    QString format() const;
    QVariantList audioStreams() const;
    QVariantList subtitleStreams() const;

    // Starts the disc (device, image, or a VIDEO_TS or BDMV directory) from
    // its first play programming, usually ending up in its menu. Opening
    // runs in the background (a Blu-ray takes seconds): active turns true
    // when it's open, openFailed() if it can't be.
    Q_INVOKABLE void open(const QString &device);
    // Stops playback and lets go of the disc.
    Q_INVOKABLE void close();
    // "up", "down", "left", "right", "activate", "menu" (root menu), "title"
    // (title menu), "back" (the menu above), "previous"/"next" (chapter),
    // "popup" (a Blu-ray's pop-up menu).
    // False if the disc refused.
    Q_INVOKABLE bool action(const QString &name);
    // { title (1-based), seconds, length } while a title plays; empty in
    // a menu
    Q_INVOKABLE QVariantMap position() const;
    // Jumps to `seconds` into the current title. False in a menu.
    Q_INVOKABLE bool seek(double seconds);
    // Has the disc play an audio or subtitle stream (an id from
    // audioStreams/subtitleStreams; "no" turns subtitles off).
    Q_INVOKABLE bool selectStream(const QString &type, const QVariant &id);

signals:
    void mpvChanged();
    void activeChanged();
    void inMenuChanged();
    void streamsChanged();
    // the disc's programming ended, or it can't be read any more
    void stopped();
    // open() couldn't open the disc
    void openFailed();

private slots:
    // a discnav event, queued from whichever thread it happened on
    void onNavEvent(int event);

private:
    void opened(NavContext *ctx, unsigned generation);
    void load();
    void onFileLoaded();
    void onEndFile(const QString &reason);
    void applyTracks();
    void scheduleOverlay();
    void renderOverlay();

    QPointer<Mpv> m_mpv;
    NavContext *m_ctx = nullptr;
    unsigned m_generation = 0; // bumped by open() and close(): stale opens are dropped
    bool m_inMenu = false;
    bool m_loadPending = false;
    int m_errors = 0;
    qint64 m_aid = -2, m_sid = -2; // last selected (-2: none yet, -1: off)
    int m_forcedOnly = -1;
    QString m_palette; // as last given to mpv's subtitle decoder
    bool m_overlayPending = false;
    bool m_overlayShown = false;
    OverlayEncoder m_encoder;
};
