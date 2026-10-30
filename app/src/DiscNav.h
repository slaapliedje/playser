#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

#include "Mpv.h"
#include "OverlayEncoder.h"

struct NavContext;

// DVD menus: libdiscnav runs the disc's VM and feeds mpv through a
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

public:
    explicit DiscNav(QObject *parent = nullptr);
    ~DiscNav() override;

    Mpv *mpv() const { return m_mpv; }
    void setMpv(Mpv *mpv);
    bool active() const { return m_ctx != nullptr; }
    bool inMenu() const { return m_inMenu; }

    // Starts the disc (device, image or VIDEO_TS directory) from its first
    // play programming, usually ending up in its menu. False if it can't be
    // opened.
    Q_INVOKABLE bool open(const QString &device);
    // Stops playback and lets go of the disc.
    Q_INVOKABLE void close();
    // "up", "down", "left", "right", "activate", "menu" (root menu), "title"
    // (title menu), "back" (the menu above), "previous"/"next" (chapter).
    // False if the disc refused.
    Q_INVOKABLE bool action(const QString &name);

signals:
    void mpvChanged();
    void activeChanged();
    void inMenuChanged();
    // the disc's programming ended, or it can't be read any more
    void stopped();

private slots:
    // a discnav event, queued from whichever thread it happened on
    void onNavEvent(int event);

private:
    void load();
    void onFileLoaded();
    void onEndFile(const QString &reason);
    void applyTracks();
    void scheduleOverlay();
    void renderOverlay();

    QPointer<Mpv> m_mpv;
    NavContext *m_ctx = nullptr;
    bool m_inMenu = false;
    bool m_loadPending = false;
    int m_errors = 0;
    qint64 m_aid = -2, m_sid = -2; // last selected (-2: none yet, -1: off)
    int m_forcedOnly = -1;
    bool m_overlayPending = false;
    bool m_overlayShown = false;
    OverlayEncoder m_encoder;
};
