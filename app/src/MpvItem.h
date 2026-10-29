#pragma once

#include <QQuickFramebufferObject>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

struct mpv_handle;
struct mpv_render_context;

// Qt Quick item rendering libmpv through its OpenGL render API.
//
// Threading rule (docs/spike-results.md, 0b): never make a blocking libmpv
// call from the GUI or render thread. Qt's threaded render loop syncs the
// two, and mpv's core can wait on the render thread, so a synchronous
// get/set/command can deadlock all three. State comes in through observed
// properties; everything going out is async.
class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(bool idle READ idle NOTIFY idleChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY chapterChanged)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY chaptersChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;
    mpv_handle *handle() const { return m_mpv; }
    // Created by the renderer on the render thread, owned by the item: mpv
    // requires it to be freed before the core is destroyed.
    mpv_render_context *renderContext = nullptr;

    // Sets per-file options (e.g. bluray-device), then loads url.
    Q_INVOKABLE void load(const QString &url, const QVariantMap &options = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void command(const QVariantList &args);
    Q_INVOKABLE void setMpvProperty(const QString &name, const QVariant &value);

    bool paused() const { return m_paused; }
    bool idle() const { return m_idle; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    QString mediaTitle() const { return m_mediaTitle; }
    int chapter() const { return m_chapter; }
    QVariantList chapters() const { return m_chapters; }
    QVariantList tracks() const { return m_tracks; }

signals:
    void pausedChanged();
    void idleChanged();
    void positionChanged();
    void durationChanged();
    void mediaTitleChanged();
    void chapterChanged();
    void chaptersChanged();
    void tracksChanged();
    void fileLoaded();
    void endFile(const QString &reason);
    void requestUpdate();

private slots:
    void processEvents();

private:
    mpv_handle *m_mpv = nullptr;
    bool m_paused = false;
    bool m_idle = true;
    double m_position = 0;
    double m_duration = 0;
    QString m_mediaTitle;
    int m_chapter = -1;
    QVariantList m_chapters;
    QVariantList m_tracks;
};
