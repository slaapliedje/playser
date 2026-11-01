// SPDX-License-Identifier: GPL-3.0-or-later
#include "Mpv.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <mpv/client.h>

namespace {

enum PropId : uint64_t {
    PropPause = 1, PropIdle, PropTimePos, PropDuration, PropTitle, PropChapter,
    PropChapterList, PropTrackList, PropOsdDimensions, PropVideoParams,
    PropSeeking, PropPausedForCache, PropAudioId, PropSpdif, PropTargetParams,
};

// passthrough found the audio output in use: retry after this long, by when
// Steam has closed its UI-sound stream
constexpr int kBusyRetryMs = 20000;

// the clock standing still this long while playing is an audio stall;
// longer while (re)starting playback, as a disc seek can be slow
constexpr int kStallSecs = 4;
constexpr int kRestartStallSecs = 8;

// Keys mpv's window forwards to the UI (as "script-message dp-key NAME").
// Everything else is unbound: input-default-bindings is off.
const char *const kForwardedKeys[] = {
    "UP", "DOWN", "LEFT", "RIGHT", "ENTER", "KP_ENTER", "ESC", "BS", "SPACE",
    "PGUP", "PGDWN", "MENU", "i", "m", "t",
};

QVariant nodeToVariant(const mpv_node *n)
{
    switch (n->format) {
    case MPV_FORMAT_STRING: return QString::fromUtf8(n->u.string);
    case MPV_FORMAT_FLAG: return bool(n->u.flag);
    case MPV_FORMAT_INT64: return qlonglong(n->u.int64);
    case MPV_FORMAT_DOUBLE: return n->u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList list;
        for (int i = 0; i < n->u.list->num; i++)
            list.append(nodeToVariant(&n->u.list->values[i]));
        return list;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap map;
        for (int i = 0; i < n->u.list->num; i++)
            map.insert(QString::fromUtf8(n->u.list->keys[i]), nodeToVariant(&n->u.list->values[i]));
        return map;
    }
    default: return {};
    }
}

// Builds an mpv_node tree for async commands/property sets. Strings are kept
// alive in `store` for the duration of the call (mpv copies them).
// Builds an mpv_node tree from a QVariant; the builder owns the memory.
// Deques: nodes point into them, and they never move what they hold.
struct NodeBuilder {
    std::deque<QByteArray> store;
    std::deque<std::vector<mpv_node>> lists;
    std::deque<std::vector<char *>> keyLists;
    std::deque<mpv_node_list> nodeLists;

    mpv_node build(const QVariant &v)
    {
        mpv_node n{};
        switch (v.typeId()) {
        case QMetaType::Bool:
            n.format = MPV_FORMAT_FLAG;
            n.u.flag = v.toBool();
            break;
        case QMetaType::Int: case QMetaType::LongLong: case QMetaType::UInt: case QMetaType::ULongLong:
            n.format = MPV_FORMAT_INT64;
            n.u.int64 = v.toLongLong();
            break;
        case QMetaType::Double: case QMetaType::Float: {
            // JS numbers arrive as doubles; integer-only properties (aid,
            // sid, chapter) reject 2.0, while double properties accept an
            // int64, so send whole numbers as integers
            const double d = v.toDouble();
            if (d == std::trunc(d) && std::abs(d) < 9007199254740992.0) {
                n.format = MPV_FORMAT_INT64;
                n.u.int64 = int64_t(d);
            } else {
                n.format = MPV_FORMAT_DOUBLE;
                n.u.double_ = d;
            }
            break;
        }
        case QMetaType::QVariantList: {
            const QVariantList src = v.toList();
            std::vector<mpv_node> &values = lists.emplace_back();
            for (const QVariant &e : src)
                values.push_back(build(e));
            nodeLists.push_back(mpv_node_list{ int(values.size()), values.data(), nullptr });
            n.format = MPV_FORMAT_NODE_ARRAY;
            n.u.list = &nodeLists.back();
            break;
        }
        case QMetaType::QVariantMap: {
            const QVariantMap src = v.toMap();
            std::vector<mpv_node> &values = lists.emplace_back();
            std::vector<char *> &keys = keyLists.emplace_back();
            for (auto it = src.cbegin(); it != src.cend(); ++it) {
                keys.push_back(store.emplace_back(it.key().toUtf8()).data());
                values.push_back(build(it.value()));
            }
            nodeLists.push_back(mpv_node_list{ int(values.size()), values.data(), keys.data() });
            n.format = MPV_FORMAT_NODE_MAP;
            n.u.list = &nodeLists.back();
            break;
        }
        default:
            n.format = MPV_FORMAT_STRING;
            n.u.string = store.emplace_back(v.toString().toUtf8()).data();
            break;
        }
        return n;
    }
};

void onMpvWakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<Mpv *>(ctx), "processEvents", Qt::QueuedConnection);
}

Mpv::Transfer transferOf(const QVariantMap &videoParams)
{
    const QString gamma = videoParams.value("gamma").toString();
    if (gamma == "pq")
        return Mpv::Transfer::Pq;
    if (gamma == "hlg")
        return Mpv::Transfer::Hlg;
    return Mpv::Transfer::Sdr;
}

} // namespace

Mpv::Mpv(bool fullscreen, const QStringList &options, QObject *parent)
    : QObject(parent), m_fullscreen(fullscreen)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        qFatal("mpv_create failed");

    const QByteArray debug = qgetenv("PLAYSER_MPV_LOG");
    if (!debug.isEmpty()) {
        mpv_set_option_string(m_mpv, "log-file", debug.constData());
        mpv_set_option_string(m_mpv, "msg-level", "all=v");
    }
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "vo", "gpu-next");
    // HDR10 passthrough to gamescope/the compositor (spike 0c)
    mpv_set_option_string(m_mpv, "target-colorspace-hint", "yes");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "force-window", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "keep-open", "no");
    mpv_set_option_string(m_mpv, "fullscreen", fullscreen ? "yes" : "no");
    mpv_set_option_string(m_mpv, "geometry", "1280x720");
    mpv_set_option_string(m_mpv, "keepaspect-window", "no");
    mpv_set_option_string(m_mpv, "title", "Playser");
    mpv_set_option_string(m_mpv, "cursor-autohide", "always");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "sid", "auto");
    mpv_set_option_string(m_mpv, "audio-channels", "auto-safe");
    // no audio device at all: play silently instead of failing the file
    mpv_set_option_string(m_mpv, "audio-fallback-to-null", "yes");
    // Our mpv (flatpak/patches) composites overlay-add bitmaps as given, in
    // the display's colorspace, so the UI is encoded for the display: stock
    // mpv converts them from the video's, which over HDR video on an SDR
    // screen tone-maps the UI and lifts its dark panels. (Before the test
    // options, which may set it back to "video".)
    mpv_set_option_string(m_mpv, "overlay-colorspace", "display");
    for (const QString &option : options) {
        const int eq = option.indexOf('=');
        if (eq <= 0 || mpv_set_option_string(m_mpv, option.left(eq).toUtf8().constData(),
                                              option.mid(eq + 1).toUtf8().constData()) < 0)
            qWarning("mpv: bad option '%s'", qPrintable(option));
    }
    if (mpv_initialize(m_mpv) < 0)
        qFatal("mpv_initialize failed");
    if (char *oc = mpv_get_property_string(m_mpv, "overlay-colorspace")) {
        m_displayOverlays = qstrcmp(oc, "display") == 0;
        mpv_free(oc);
    }
    // "no target node available": the audio output is busy (onAudioOutputBusy)
    mpv_request_log_messages(m_mpv, "warn");

    for (const char *key : kForwardedKeys)
        commandAsync({ "keybind", key, QStringLiteral("script-message dp-key %1").arg(key) });
    commandAsync({ "keybind", "CLOSE_WIN", "script-message dp-close" });

    mpv_observe_property(m_mpv, PropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropIdle, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropTitle, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropChapter, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropChapterList, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropTrackList, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropOsdDimensions, "osd-dimensions", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropVideoParams, "video-params", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropTargetParams, "video-target-params", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropSeeking, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropAudioId, "current-tracks/audio/id", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropSpdif, "audio-spdif", MPV_FORMAT_STRING);

    m_audioWatchdog.setInterval(1000);
    connect(&m_audioWatchdog, &QTimer::timeout, this, &Mpv::checkAudioStall);

    mpv_set_wakeup_callback(m_mpv, onMpvWakeup, this);
}

Mpv::~Mpv()
{
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    // waits for pending commands, so overlay bitmaps are still alive here
    mpv_terminate_destroy(m_mpv);
}

void Mpv::load(const QString &url, const QVariantMap &options, double start)
{
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        setMpvProperty(it.key(), it.value());
    if (start > 0)
        command({ "loadfile", url, "replace", -1, QStringLiteral("start=%1").arg(start, 0, 'f', 1) });
    else
        command({ "loadfile", url });
}

// The video fitted into the window and centred, as mpv draws it (the app
// uses no zoom or pan). Worked out here rather than from osd-dimensions'
// margins, which read as no video for a moment between files.
void Mpv::updateVideoRect()
{
    QRect rect;
    QSize display = m_videoDisplay;
    // one eye's half, shown or cropped (the decoded frame's: mpv's display
    // size follows the crop); Blu-ray 3D has square pixels
    if (m_stereoVideo && !m_videoSize.isEmpty())
        display = QSize(m_videoSize.width() / 2, m_videoSize.height());
    if (!display.isEmpty() && !m_osdSize.isEmpty()) {
        const QSize fit = display.scaled(m_osdSize, Qt::KeepAspectRatio);
        rect = QRect(QPoint((m_osdSize.width() - fit.width()) / 2, (m_osdSize.height() - fit.height()) / 2), fit);
    }
    if (rect != m_videoRect) {
        m_videoRect = rect;
        emit videoGeometryChanged();
    }
}

void Mpv::loadFile(const QString &url, const QStringList &fileOptions)
{
    command({ "loadfile", url, "replace", -1, fileOptions.join(',') });
}

void Mpv::stop()
{
    command({ "stop" });
}

void Mpv::command(const QVariantList &args)
{
    commandAsync(args);
}

void Mpv::commandAsync(const QVariantList &args, quint64 replyId)
{
    NodeBuilder b;
    mpv_node node = b.build(args);
    mpv_command_node_async(m_mpv, replyId, &node);
}

void Mpv::setMpvProperty(const QString &name, const QVariant &value)
{
    NodeBuilder b;
    mpv_node node = b.build(value);
    mpv_set_property_async(m_mpv, 0, name.toUtf8().constData(), MPV_FORMAT_NODE, &node);
}

float Mpv::gain() const
{
    int muted = 0;
    double volume = 100;
    mpv_get_property(m_mpv, "mute", MPV_FORMAT_FLAG, &muted);
    mpv_get_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
    return muted ? 0.0f : float(volume / 100);
}

void Mpv::screenshot(const QString &path)
{
    command({ "screenshot-to-file", path, "window" });
}

// the other eye's copy of overlay `id` on a side-by-side display
constexpr int kSecondEye = 32;

void Mpv::setOverlay(int id, const QImage &image, const QRect &dst)
{
    Q_ASSERT(id < kSecondEye);
    if (!sideBySide()) {
        setOverlaySlot(id, image, dst);
        return;
    }
    const QRect to = !dst.isEmpty() ? dst : QRect(QPoint(0, 0), m_osdSize);
    setOverlaySlot(id, image, to);
    setOverlaySlot(id + kSecondEye, image, to.translated(m_osdSize.width(), 0));
}

void Mpv::setSideBySide(const QSize &size)
{
    if (size == m_sbsSize)
        return;
    m_sbsSize = size;
    if (sideBySide()) {
        qInfo("display: side-by-side 3D, %dx%d", size.width(), size.height());
        setMpvProperty("fullscreen", false);
        setMpvProperty("geometry", QStringLiteral("%1x%2+0+0").arg(size.width()).arg(size.height()));
    } else {
        qInfo("display: 2D");
        for (auto it = m_overlays.cbegin(); it != m_overlays.cend(); ++it)
            if (it.key() >= kSecondEye && it->shown)
                QMetaObject::invokeMethod(this, [this, id = it.key()] { setOverlaySlot(id, QImage(), {}); },
                                          Qt::QueuedConnection);
        setMpvProperty("geometry", "1280x720");
        setMpvProperty("fullscreen", m_fullscreen);
    }
    applyStereo();
}

void Mpv::setStereoVideo(bool on, bool baseRight)
{
    baseRight &= on;
    if (on == m_stereoVideo && baseRight == m_baseRight)
        return;
    m_stereoVideo = on;
    m_baseRight = baseRight;
    applyStereo();
}

// 3D video: libedge264mvc decodes both eyes, side by side (in software:
// with hwdec mpv swaps in the hardware h264 decoder, which can't take both
// views interleaved). A 2D display gets the left eye, cropped: unlike a
// decoder change, a crop applies mid-film without restarting anything.
// 2D video on a side-by-side display: both eyes get the same picture (hwdec
// copies frames back for the filter).
void Mpv::applyStereo()
{
    const bool flat = !m_stereoVideo && sideBySide();
    // only on change: setting vd or hwdec, even to the same value, restarts
    // the decoder
    auto set = [this](const char *name, const QString &value) {
        QString &applied = m_stereoOptions[name];
        if (applied != value) {
            applied = value;
            setMpvProperty(name, value);
        }
    };
    set("vd", m_stereoVideo ? "libedge264mvc" : "");
    // applies when the decoder starts (the next file)
    set("vd-lavc-o", m_baseRight ? "base_view_right=1" : "");
    set("hwdec", m_stereoVideo ? "no" : flat ? "auto-copy-safe" : "auto-safe");
    set("vf", flat ? "lavfi=[split[l][r];[l][r]hstack]" : "");
    const bool crop = m_stereoVideo && !sideBySide() && !m_videoSize.isEmpty();
    set("video-crop", crop ? QStringLiteral("%1x%2+0+0").arg(m_videoSize.width() / 2).arg(m_videoSize.height()) : "");
    updateOsdSize();
}

void Mpv::updateOsdSize()
{
    QSize size = m_windowSize;
    if (sideBySide())
        size.setWidth(size.width() / 2);
    if (size != m_osdSize) {
        m_osdSize = size;
        emit osdSizeChanged();
    }
    updateVideoRect();
}

void Mpv::setOverlaySlot(int id, const QImage &image, const QRect &dst)
{
    OverlaySlot &slot = m_overlays[id];
    if (slot.inFlight) {
        slot.queued = image;
        slot.queuedDst = dst;
        slot.hasQueued = true;
        return;
    }
    sendOverlay(id, image, dst);
}

void Mpv::sendOverlay(int id, const QImage &image, const QRect &dst)
{
    OverlaySlot &slot = m_overlays[id];
    if (image.isNull() && !slot.shown)
        return;
    slot.inFlight = ++m_nextReply;
    slot.sending = image;
    if (image.isNull()) {
        slot.shown = false;
        commandAsync({ "overlay-remove", id }, slot.inFlight);
        return;
    }
    Q_ASSERT(image.format() == QImage::Format_ARGB32_Premultiplied);
    slot.shown = true;
    const QRect to = !dst.isEmpty() ? dst : QRect(QPoint(0, 0), m_osdSize.isEmpty() ? image.size() : m_osdSize);
    // mpv reads (and copies) the memory when the command runs, so `sending`
    // keeps the image alive until the reply arrives
    const QString address = QStringLiteral("&%1").arg(quintptr(image.constBits()));
    commandAsync({ "overlay-add", id, to.x(), to.y(), address, 0, "bgra", image.width(), image.height(),
                   qlonglong(image.bytesPerLine()), to.width(), to.height() },
                 slot.inFlight);
}

void Mpv::setPassthrough(const QString &codecs)
{
    if (codecs == m_passthrough)
        return;
    m_passthrough = codecs;
    m_passthroughFailed = false;
    applySpdif(true);
    emit passthroughChanged();
}

void Mpv::applySpdif(bool on)
{
    setMpvProperty("audio-spdif", on && !m_passthroughFailed && m_glassesAudio.isEmpty() ? m_passthrough : QString());
}

void Mpv::setGlassesAudio(const QString &card)
{
    if (card == m_glassesAudio)
        return;
    m_glassesAudio = card;
    QString device = QStringLiteral("auto");
    mpv_node list;
    if (!card.isEmpty() && mpv_get_property(m_mpv, "audio-device-list", MPV_FORMAT_NODE, &list) >= 0) {
        for (const QVariant &d : nodeToVariant(&list).toList()) {
            const QString name = d.toMap().value("name").toString();
            if (name.contains(card) && name.contains(QLatin1String("hdmi"))) {
                device = name;
                break;
            }
        }
        mpv_free_node_contents(&list);
    }
    qInfo("audio: %s", device == QLatin1String("auto") ? "the default output" : qPrintable(device));
    setMpvProperty("audio-device", device);
    applySpdif(true);
}

void Mpv::onAudioOutputBusy()
{
    if (m_spdif.isEmpty() || m_outputBusy)
        return;
    m_outputBusy = true;
    // decode now rather than wait, frozen, for the output
    applySpdif(false);
    command({ "ao-reload" });
    if (m_busyRetried) {
        qWarning("audio: output still busy; decoding this file");
        emit audioProblem(QStringLiteral("Another app was using the audio output, so audio is decoded instead."));
        return;
    }
    qWarning("audio: output busy (another stream on it); decoding, passthrough again in %d s", kBusyRetryMs / 1000);
    const qint64 entry = m_currentEntry;
    QTimer::singleShot(kBusyRetryMs, this, [this, entry] {
        if (entry != m_currentEntry || !m_loaded || m_passthroughFailed)
            return;
        m_busyRetried = true;
        m_outputBusy = false;
        applySpdif(true);
        command({ "ao-reload" });
    });
}

void Mpv::checkAudioStall()
{
    // only while playing with audio. A slow disc read can look the same (the
    // demuxer buffer reads empty in both cases), but the first steps below
    // are harmless then
    const bool moving = m_position != m_lastPosition;
    m_lastPosition = m_position;
    // a failed passthrough start never finishes the restart ("seeking")
    if (moving || !m_stallCheck || !m_loaded || m_paused || m_pausedForCache || !m_hasAudio) {
        m_stalledSecs = 0;
        return;
    }
    if (++m_stalledSecs < (m_seeking ? kRestartStallSecs : kStallSecs))
        return;
    m_stalledSecs = 0;

    // each step gets kStallSecs to work before the next
    const int step = m_recovery++;
    if (step == 0 && !m_spdif.isEmpty()) {
        qWarning("audio: playback stalled; passthrough (%s) off, decoding instead", qPrintable(m_spdif));
        m_passthroughFailed = true;
        applySpdif(false);
        command({ "ao-reload" });
        emit audioProblem(QStringLiteral("Audio passthrough didn't work, so audio is decoded instead."));
    } else if (step <= 1) {
        qWarning("audio: playback stalled; reopening the audio output");
        command({ "ao-reload" });
    } else if (step == 2) {
        qWarning("audio: playback stalled; continuing without sound");
        setMpvProperty("ao", "null");
        command({ "ao-reload" });
        emit audioProblem(QStringLiteral("The audio output stopped responding. Playing without sound."));
    }
}

void Mpv::processEvents()
{
    while (m_mpv) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *p = static_cast<mpv_event_property *>(ev->data);
            const bool has = p->format != MPV_FORMAT_NONE;
            switch (ev->reply_userdata) {
            case PropPause: m_paused = has && *static_cast<int *>(p->data); emit pausedChanged(); break;
            case PropIdle: m_idle = has && *static_cast<int *>(p->data); emit idleChanged(); break;
            case PropTimePos: m_position = has ? *static_cast<double *>(p->data) : 0; emit positionChanged(); break;
            case PropDuration: m_duration = has ? *static_cast<double *>(p->data) : 0; emit durationChanged(); break;
            case PropTitle:
                m_mediaTitle = has ? QString::fromUtf8(*static_cast<char **>(p->data)) : QString();
                emit mediaTitleChanged();
                break;
            case PropChapter: m_chapter = has ? int(*static_cast<int64_t *>(p->data)) : -1; emit chapterChanged(); break;
            case PropChapterList:
                m_chapters = has ? nodeToVariant(static_cast<mpv_node *>(p->data)).toList() : QVariantList();
                emit chaptersChanged();
                break;
            case PropTrackList:
                m_tracks = has ? nodeToVariant(static_cast<mpv_node *>(p->data)).toList() : QVariantList();
                emit tracksChanged();
                break;
            case PropOsdDimensions: {
                const QVariantMap d = has ? nodeToVariant(static_cast<mpv_node *>(p->data)).toMap() : QVariantMap();
                m_windowSize = QSize(d.value("w").toInt(), d.value("h").toInt());
                updateOsdSize();
                break;
            }
            case PropSeeking: m_seeking = has && *static_cast<int *>(p->data); break;
            case PropPausedForCache: m_pausedForCache = has && *static_cast<int *>(p->data); break;
            case PropAudioId: m_hasAudio = has; break;
            case PropSpdif: m_spdif = has ? QString::fromUtf8(*static_cast<char **>(p->data)) : QString(); break;
            case PropVideoParams:
            case PropTargetParams: {
                const QVariantMap params = has ? nodeToVariant(static_cast<mpv_node *>(p->data)).toMap() : QVariantMap();
                if (ev->reply_userdata == PropVideoParams) {
                    const QSize size(params.value("w").toInt(), params.value("h").toInt());
                    const QSize display(params.value("dw").toInt(), params.value("dh").toInt());
                    if (size != m_videoSize || display != m_videoDisplay) {
                        m_videoSize = size;
                        m_videoDisplay = display;
                        applyStereo(); // the crop follows the video's size
                        emit videoGeometryChanged();
                    }
                }
                if (ev->reply_userdata == PropTargetParams && !params.isEmpty()) {
                    const QString display = QStringList{ params.value("primaries").toString(), params.value("gamma").toString(),
                                                         params.value("max-luma").toString() + " nits" }.join(' ');
                    if (display != m_display) {
                        m_display = display;
                        qInfo("mpv: display %s", qPrintable(display));
                    }
                }
                if ((ev->reply_userdata == PropTargetParams) != m_displayOverlays)
                    break;
                const Transfer t = transferOf(params);
                if (t != m_transfer) {
                    m_transfer = t;
                    emit transferChanged();
                }
                break;
            }
            }
            break;
        }
        case MPV_EVENT_COMMAND_REPLY:
            for (auto it = m_overlays.begin(); it != m_overlays.end(); ++it) {
                OverlaySlot &slot = it.value();
                if (!slot.inFlight || slot.inFlight != ev->reply_userdata)
                    continue;
                if (ev->error < 0)
                    qWarning("mpv: overlay %d: %s", it.key(), mpv_error_string(ev->error));
                slot.inFlight = 0;
                slot.sending = QImage();
                if (slot.hasQueued) {
                    slot.hasQueued = false;
                    sendOverlay(it.key(), std::exchange(slot.queued, QImage()), slot.queuedDst);
                }
                break;
            }
            break;
        case MPV_EVENT_CLIENT_MESSAGE: {
            auto *msg = static_cast<mpv_event_client_message *>(ev->data);
            if (msg->num_args == 2 && qstrcmp(msg->args[0], "dp-key") == 0)
                emit keyPressed(QString::fromUtf8(msg->args[1]));
            else if (msg->num_args == 1 && qstrcmp(msg->args[0], "dp-close") == 0)
                emit closeRequested();
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(ev->data);
            if (qstrncmp(msg->prefix, "ao", 2) == 0 && strstr(msg->text, "no target node"))
                onAudioOutputBusy();
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            emit closeRequested();
            break;
        case MPV_EVENT_FILE_LOADED:
            m_loaded = true;
            m_audioWatchdog.start();
            emit fileLoaded();
            break;
        case MPV_EVENT_START_FILE:
            m_currentEntry = static_cast<mpv_event_start_file *>(ev->data)->playlist_entry_id;
            m_loaded = false;
            m_audioWatchdog.stop();
            m_stalledSecs = 0;
            // try the real output again for each file; passthrough stays off
            // once it stalled (until the next start of the app)
            if (m_recovery >= 3)
                setMpvProperty("ao", "");
            m_recovery = 0;
            if (m_outputBusy || m_busyRetried)
                applySpdif(true);
            m_outputBusy = m_busyRetried = false;
            break;
        case MPV_EVENT_END_FILE: {
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            // a cancelled disc open can finish long after the next one
            // started (libbluray opens aren't interruptible): ignore it
            if (ef->playlist_entry_id != m_currentEntry)
                break;
            static const char *const reasons[] = { "eof", "", "stop", "quit", "error", "redirect" };
            const int r = int(ef->reason);
            emit endFile(QString::fromLatin1(r >= 0 && r <= 5 ? reasons[r] : "unknown"));
            break;
        }
        default:
            break;
        }
    }
}
