#include "DiscNav.h"

#include <QFile>
#include <QLocale>
#include <QLoggingCategory>
#include <QTimer>
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <mutex>
#include <thread>

#include <discnav.h>

Q_LOGGING_CATEGORY(lcNav, "playser.discnav", QtInfoMsg)

namespace {

// Settings for DVD segments (docs/spike-results.md, 0b), per file so other
// playback keeps mpv's defaults: MPEG-PS has no header, so the format is
// given and probing kept short but long enough (1 MB) to see the first
// audio packet; a one-picture still needs the first frame shown without
// waiting for a second, and no frame threading (nothing drains it during a
// still); the VM chooses the tracks (applyTracks), never mpv.
const QStringList kDvdSegmentOptions = {
    "demuxer-lavf-format=mpeg", "demuxer-lavf-probesize=1048576", "demuxer-lavf-analyzeduration=0.5",
    "cache=no", "demuxer-readahead-secs=0.5", "video-latency-hacks=yes", "vd-lavc-threads=1",
    "sid=no",
};

// Blu-ray segments: M2TS (192-byte packets, which lavf's mpegts detects);
// little read-ahead so a user's jump flushes little; frame threading stays
// (UHD HEVC needs it).
const QStringList kBdSegmentOptions = {
    "demuxer-lavf-format=mpegts", "demuxer-lavf-probesize=5000000", "demuxer-lavf-analyzeduration=1",
    "cache=no", "demuxer-readahead-secs=0.5", "sid=no",
};

// the disc's menu graphics, under the UI (UiRenderer: overlay 1)
constexpr int kMenuOverlay = 0;

constexpr int kMaxLoadErrors = 5;

} // namespace

// One opened disc. mpv's reader thread can still hold a stream of it after
// close(), so it lives until its last stream closes.
struct NavContext
{
    discnav_t *nav = nullptr;
    std::mutex mutex;
    int streams = 0;
    bool closing = false;
};

namespace {

// mpv's stream_cb registration outlives any one disc: it finds the current
// one here. Lock order: a discnav lock (held during its callbacks), then
// this; this, then a NavContext's.
struct Router
{
    std::mutex mutex;
    NavContext *current = nullptr;
    DiscNav *receiver = nullptr;
} g_router;

void freeIfDone(NavContext *ctx)
{
    bool done;
    {
        std::lock_guard lock(ctx->mutex);
        done = ctx->closing && ctx->streams == 0;
    }
    if (done) {
        discnav_close(ctx->nav);
        delete ctx;
    }
}

struct Stream
{
    NavContext *ctx;
    discnav_stream_t *s;
};

int64_t readFn(void *cookie, char *buf, uint64_t nbytes)
{
    auto *st = static_cast<Stream *>(cookie);
    return discnav_stream_read(st->s, buf, nbytes);
}

void cancelFn(void *cookie)
{
    discnav_stream_cancel(static_cast<Stream *>(cookie)->s);
}

void closeFn(void *cookie)
{
    auto *st = static_cast<Stream *>(cookie);
    discnav_stream_close(st->s);
    {
        std::lock_guard lock(st->ctx->mutex);
        st->ctx->streams--;
    }
    freeIfDone(st->ctx);
    delete st;
}

int openFn(void *, char *, mpv_stream_cb_info *info)
{
    NavContext *ctx;
    {
        std::lock_guard lock(g_router.mutex);
        ctx = g_router.current;
        if (!ctx)
            return MPV_ERROR_LOADING_FAILED;
        std::lock_guard ctxLock(ctx->mutex);
        ctx->streams++;
    }
    discnav_stream_t *s = discnav_stream_open(ctx->nav);
    if (!s) {
        {
            std::lock_guard lock(ctx->mutex);
            ctx->streams--;
        }
        freeIfDone(ctx);
        return MPV_ERROR_LOADING_FAILED;
    }
    info->cookie = new Stream{ ctx, s };
    info->read_fn = readFn;
    info->close_fn = closeFn;
    info->cancel_fn = cancelFn;
    return 0;
}

// called with discnav's lock held, on mpv's reader thread or in action():
// only queue
void onEvent(void *user, discnav_event_t event)
{
    std::lock_guard lock(g_router.mutex);
    if (g_router.current != user || !g_router.receiver)
        return;
    QMetaObject::invokeMethod(g_router.receiver, "onNavEvent", Qt::QueuedConnection, Q_ARG(int, int(event)));
}

void onLog(void *, int level, const char *message)
{
    if (level == 0)
        qCInfo(lcNav, "%s", message);
    else
        qCDebug(lcNav, "%s", message);
}

} // namespace

DiscNav::DiscNav(QObject *parent) : QObject(parent)
{
    std::lock_guard lock(g_router.mutex);
    g_router.receiver = this;
}

DiscNav::~DiscNav()
{
    close();
    std::lock_guard lock(g_router.mutex);
    if (g_router.receiver == this)
        g_router.receiver = nullptr;
}

void DiscNav::setMpv(Mpv *mpv)
{
    if (mpv == m_mpv)
        return;
    if (m_mpv)
        disconnect(m_mpv, nullptr, this, nullptr);
    m_mpv = mpv;
    if (m_mpv) {
        static bool registered = false;
        if (!registered)
            registered = mpv_stream_cb_add_ro(m_mpv->handle(), "discnav", nullptr, openFn) >= 0;
        connect(m_mpv, &Mpv::fileLoaded, this, &DiscNav::onFileLoaded);
        connect(m_mpv, &Mpv::endFile, this, &DiscNav::onEndFile);
        connect(m_mpv, &Mpv::tracksChanged, this, &DiscNav::applyTracks);
        connect(m_mpv, &Mpv::videoGeometryChanged, this, &DiscNav::scheduleOverlay);
        connect(m_mpv, &Mpv::transferChanged, this, &DiscNav::scheduleOverlay);
    }
    emit mpvChanged();
}

QString DiscNav::format() const
{
    if (!m_ctx)
        return {};
    return discnav_format(m_ctx->nav) == DISCNAV_BLURAY ? QStringLiteral("bluray") : QStringLiteral("dvd");
}

void DiscNav::open(const QString &device)
{
    close();
    if (!m_mpv) {
        emit openFailed();
        return;
    }
    const unsigned generation = ++m_generation;
    auto *ctx = new NavContext;
    // a Blu-ray's open takes seconds (AACS, BD-J's JVM): off the UI thread
    std::thread([this, ctx, generation, path = QFile::encodeName(device)] {
        ctx->nav = discnav_open(path.constData(), "en", onEvent, onLog, ctx);
        QMetaObject::invokeMethod(this, [this, ctx, generation] { opened(ctx, generation); }, Qt::QueuedConnection);
    }).detach();
}

void DiscNav::opened(NavContext *ctx, unsigned generation)
{
    if (generation != m_generation || !ctx->nav) {
        // closed meanwhile, or not a disc: let it go (off this thread)
        const bool failed = generation == m_generation;
        std::thread([ctx] {
            discnav_close(ctx->nav);
            delete ctx;
        }).detach();
        if (failed)
            emit openFailed();
        return;
    }
    {
        std::lock_guard lock(g_router.mutex);
        g_router.current = ctx;
    }
    m_ctx = ctx;
    m_loadPending = false;
    m_errors = 0;
    m_aid = m_sid = -2;
    m_forcedOnly = -1;
    m_palette.clear();
    m_mpv->setStallCheck(false);
    emit activeChanged();
    load();
}

void DiscNav::close()
{
    m_generation++; // an open still running is dropped when it ends
    if (!m_ctx)
        return;
    NavContext *ctx = m_ctx;
    m_ctx = nullptr;
    {
        std::lock_guard lock(g_router.mutex);
        if (g_router.current == ctx)
            g_router.current = nullptr;
        std::lock_guard ctxLock(ctx->mutex);
        ctx->closing = true;
    }
    if (m_mpv) {
        m_mpv->stop(); // cancels and closes the stream; the context goes with it
        m_mpv->setOverlay(kMenuOverlay, QImage());
        m_mpv->setStallCheck(true);
        m_mpv->setMpvProperty("sub-lavc-o", QVariantMap());
    }
    m_overlayShown = false;
    freeIfDone(ctx);
    if (m_inMenu) {
        m_inMenu = false;
        emit inMenuChanged();
    }
    emit activeChanged();
}

bool DiscNav::action(const QString &name)
{
    static const QHash<QString, discnav_action_t> actions = {
        { "up", DISCNAV_UP }, { "down", DISCNAV_DOWN }, { "left", DISCNAV_LEFT },
        { "right", DISCNAV_RIGHT }, { "activate", DISCNAV_ACTIVATE }, { "menu", DISCNAV_ROOT_MENU },
        { "title", DISCNAV_TITLE_MENU }, { "back", DISCNAV_GO_UP },
        { "previous", DISCNAV_PREV_CHAPTER }, { "next", DISCNAV_NEXT_CHAPTER },
        { "popup", DISCNAV_POPUP_MENU },
    };
    const auto it = actions.constFind(name);
    if (!m_ctx || it == actions.cend())
        return false;
    return discnav_action(m_ctx->nav, it.value());
}

QVariantMap DiscNav::position() const
{
    int title = 0;
    double seconds = 0, length = 0;
    if (!m_ctx || !discnav_get_position(m_ctx->nav, &title, &seconds, &length))
        return {};
    return { { "title", title }, { "seconds", seconds }, { "length", length } };
}

bool DiscNav::seek(double seconds)
{
    return m_ctx && discnav_seek(m_ctx->nav, seconds);
}

namespace {

QVariantList streamList(NavContext *ctx, discnav_stream_type_t type)
{
    if (!ctx)
        return {};
    discnav_stream_info_t streams[32];
    const int n = discnav_get_streams(ctx->nav, type, streams, 32);
    QVariantList list;
    for (int i = 0; i < n; i++) {
        const discnav_stream_info_t &st = streams[i];
        QStringList parts;
        const QLocale::Language language = st.lang[0] ? QLocale(QString::fromLatin1(st.lang)).language()
                                                      : QLocale::AnyLanguage;
        parts << (language > QLocale::C ? QLocale::languageToString(language)
                                        : QStringLiteral("%1 %2").arg(type == DISCNAV_AUDIO ? "Audio" : "Subtitles")
                                                                  .arg(i + 1));
        switch (st.kind) {
        case DISCNAV_KIND_COMMENTARY:        parts << QStringLiteral("Commentary"); break;
        case DISCNAV_KIND_VISUALLY_IMPAIRED: parts << QStringLiteral("Described"); break;
        case DISCNAV_KIND_LARGE:             parts << QStringLiteral("Large"); break;
        case DISCNAV_KIND_CHILDREN:          parts << QStringLiteral("Children's"); break;
        case DISCNAV_KIND_CAPTIONS:          parts << QStringLiteral("Captions"); break;
        case DISCNAV_KIND_FORCED:            parts << QStringLiteral("Forced"); break;
        case DISCNAV_KIND_NORMAL:            break;
        }
        if (type == DISCNAV_AUDIO) {
            static const QHash<QString, QString> formats = {
                { "ac3", "Dolby Digital" }, { "eac3", "Dolby Digital Plus" }, { "truehd", "Dolby TrueHD" },
                { "dts", "DTS" }, { "dtshd", "DTS-HD" }, { "lpcm", "PCM" }, { "mpeg", "MPEG" }, { "sdds", "SDDS" },
            };
            // a Blu-ray only says mono, stereo or more (0)
            const QString layout = st.channels == 0 ? QString()
                                 : st.channels == 1 ? QStringLiteral("Mono")
                                 : st.channels == 2 ? QStringLiteral("Stereo")
                                 : st.channels == 6 ? QStringLiteral("5.1")
                                 : st.channels == 8 ? QStringLiteral("7.1")
                                 : QStringLiteral("%1ch").arg(st.channels);
            const QString audio = QStringLiteral("%1 %2").arg(formats.value(QString::fromLatin1(st.format)), layout).trimmed();
            if (!audio.isEmpty())
                parts << audio;
        }
        list.append(QVariantMap{ { "id", st.index }, { "label", parts.join(QStringLiteral("  ·  ")) },
                                 { "selected", st.selected } });
    }
    return list;
}

} // namespace

QVariantList DiscNav::audioStreams() const
{
    return streamList(m_ctx, DISCNAV_AUDIO);
}

QVariantList DiscNav::subtitleStreams() const
{
    return streamList(m_ctx, DISCNAV_SUBTITLES);
}

bool DiscNav::selectStream(const QString &type, const QVariant &id)
{
    if (!m_ctx)
        return false;
    const int index = id.toString() == QLatin1String("no") ? -1 : id.toInt();
    return discnav_select_stream(m_ctx->nav, type == QLatin1String("audio") ? DISCNAV_AUDIO : DISCNAV_SUBTITLES,
                                 index);
}

void DiscNav::load()
{
    m_loadPending = true;
    m_mpv->loadFile(QStringLiteral("discnav://"),
                    discnav_format(m_ctx->nav) == DISCNAV_BLURAY ? kBdSegmentOptions : kDvdSegmentOptions);
}

void DiscNav::onNavEvent(int event)
{
    if (!m_ctx)
        return;
    switch (event) {
    case DISCNAV_EVENT_OVERLAY:
        scheduleOverlay();
        break;
    case DISCNAV_EVENT_FLUSH:
        // drop mpv's buffered old content rather than let it play out
        load();
        break;
    case DISCNAV_EVENT_TRACKS:
        applyTracks();
        break;
    case DISCNAV_EVENT_STOP:
        break; // the segment's end follows (onEndFile)
    }
}

void DiscNav::onFileLoaded()
{
    if (!m_ctx)
        return;
    m_loadPending = false;
    m_aid = m_sid = -2; // a new file: new track ids
    m_forcedOnly = -1;
    applyTracks();
    discnav_segment_opened(m_ctx->nav);
}

void DiscNav::onEndFile(const QString &reason)
{
    if (!m_ctx)
        return;
    if (discnav_stopped(m_ctx->nav)) {
        close();
        emit stopped();
        return;
    }
    m_errors = reason == u"error" ? m_errors + 1 : 0;
    if (m_errors > kMaxLoadErrors) {
        qCWarning(lcNav, "the disc can't be read any more");
        close();
        emit stopped();
        return;
    }
    // a flush already loaded the next segment: loading again would cancel
    // it and lose its first blocks (a still menu's only picture)
    if (!m_loadPending || m_errors)
        load();
}

// The VM's choice of streams, applied once mpv lists them (MPEG-PS streams
// show up only when their first packet does).
void DiscNav::applyTracks()
{
    if (!m_ctx || !m_mpv)
        return;
    discnav_tracks_t t;
    discnav_get_tracks(m_ctx->nav, &t);
    if (t.in_menu != m_inMenu) {
        m_inMenu = t.in_menu;
        emit inMenuChanged();
    }
    emit streamsChanged();
    const auto find = [this](const char *type, int srcId) -> qint64 {
        if (srcId < 0)
            return -1;
        for (const QVariant &v : m_mpv->tracks()) {
            const QVariantMap track = v.toMap();
            if (track.value("type").toString() == QLatin1String(type) && track.value("src-id").toInt() == srcId)
                return track.value("id").toLongLong();
        }
        return -2; // not listed yet
    };
    const qint64 aid = find("audio", t.audio_id);
    if (aid >= 0 && aid != m_aid) {
        qCInfo(lcNav, "audio track %lld (stream 0x%x)", aid, t.audio_id);
        m_mpv->setMpvProperty("aid", aid);
        m_aid = aid;
    }
    // in menus discnav draws the subpictures (with the disc's palette and
    // highlights), so mpv must not
    const qint64 sid = t.in_menu ? -1 : find("sub", t.sub_id);
    if (sid == -1 && m_sid != -1) {
        m_mpv->setMpvProperty("sid", "no");
        m_sid = -1;
    } else if (sid >= 0 && sid != m_sid) {
        qCInfo(lcNav, "subtitle track %lld (stream 0x%x)", sid, t.sub_id);
        m_mpv->setMpvProperty("sid", sid);
        m_sid = sid;
    }
    if (int(t.sub_forced_only) != m_forcedOnly) {
        m_mpv->setMpvProperty("sub-forced-events-only", t.sub_forced_only);
        m_forcedOnly = t.sub_forced_only;
    }
    // title subtitles are decoded by mpv, which has no way to learn the
    // disc's palette through a stream: give it to FFmpeg's decoder
    uint32_t rgb[16];
    if (discnav_get_palette(m_ctx->nav, rgb)) {
        QStringList colours;
        for (uint32_t c : rgb)
            colours.append(QStringLiteral("%1").arg(c, 6, 16, QLatin1Char('0')));
        const QString palette = colours.join(',');
        if (palette != m_palette) {
            m_mpv->setMpvProperty("sub-lavc-o", QVariantMap{ { "palette", palette } });
            m_palette = palette;
        }
    }
}

void DiscNav::scheduleOverlay()
{
    if (!m_ctx || m_overlayPending)
        return;
    m_overlayPending = true;
    QTimer::singleShot(0, this, &DiscNav::renderOverlay);
}

// The menu graphics are composited at the video's storage size (DVD:
// 720x480) or their own (Blu-ray: 1920x1080) and scaled by mpv onto the
// video's rectangle, like the picture; a Blu-ray menu may have no video
// behind it, then a 16:9 frame in the window.
void DiscNav::renderOverlay()
{
    m_overlayPending = false;
    if (!m_ctx || !m_mpv)
        return;
    int w = 0, h = 0;
    const bool ownSize = discnav_overlay_size(m_ctx->nav, &w, &h);
    const QSize frame = ownSize ? QSize(w, h) : m_mpv->videoSize();
    QRect rect = m_mpv->videoRect();
    if (ownSize && rect.isEmpty() && !m_mpv->osdSize().isEmpty()) {
        const QSize fit = frame.scaled(m_mpv->osdSize(), Qt::KeepAspectRatio);
        rect = QRect(QPoint((m_mpv->osdSize().width() - fit.width()) / 2,
                            (m_mpv->osdSize().height() - fit.height()) / 2), fit);
    }
    if (frame.isEmpty() || rect.isEmpty())
        return;
    QImage rgba(frame, QImage::Format_RGBA8888);
    if (!discnav_render_overlay(m_ctx->nav, rgba.bits(), frame.width(), frame.height())) {
        if (m_overlayShown)
            m_mpv->setOverlay(kMenuOverlay, QImage());
        m_overlayShown = false;
        return;
    }
    qCDebug(lcNav, "overlay %dx%d onto %d,%d %dx%d", frame.width(), frame.height(), rect.x(), rect.y(),
            rect.width(), rect.height());
    const QImage bgra = rgba.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_mpv->setOverlay(kMenuOverlay, m_encoder.encode(bgra, m_mpv->transfer()), rect);
    m_overlayShown = true;
}
