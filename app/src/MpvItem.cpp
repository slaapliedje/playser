#include "MpvItem.h"

#include <QOpenGLContext>
#include <cmath>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <mpv/client.h>
#include <mpv/render_gl.h>

namespace {

enum PropId : uint64_t {
    PropPause = 1, PropIdle, PropTimePos, PropDuration, PropTitle, PropChapter,
    PropChapterList, PropTrackList,
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
struct NodeBuilder {
    QList<QByteArray> store;
    QList<QList<mpv_node>> lists;
    QList<mpv_node_list> nodeLists;

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
            lists.append(QList<mpv_node>());
            QList<mpv_node> &values = lists.last();
            values.reserve(src.size());
            for (const QVariant &e : src)
                values.append(build(e));
            nodeLists.append(mpv_node_list{ int(values.size()), values.data(), nullptr });
            n.format = MPV_FORMAT_NODE_ARRAY;
            n.u.list = &nodeLists.last();
            break;
        }
        default:
            store.append(v.toString().toUtf8());
            n.format = MPV_FORMAT_STRING;
            n.u.string = store.last().data();
            break;
        }
        return n;
    }

    NodeBuilder() { lists.reserve(64); nodeLists.reserve(64); store.reserve(64); }
};

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(name)) : nullptr;
}

void onMpvUpdate(void *ctx)
{
    // render thread or mpv thread: just schedule a frame on the GUI thread
    emit static_cast<MpvItem *>(ctx)->requestUpdate();
}

void onMpvWakeup(void *ctx)
{
    QMetaObject::invokeMethod(static_cast<MpvItem *>(ctx), "processEvents", Qt::QueuedConnection);
}

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(MpvItem *item) : m_item(item) {}

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        mpv_render_context *&m_ctx = m_item->renderContext;
        if (!m_ctx) {
            mpv_opengl_init_params gl{ getProcAddress, nullptr };
            int advanced = 1;
            mpv_render_param params[] = {
                { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
                { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl },
                { MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced },
                { MPV_RENDER_PARAM_INVALID, nullptr },
            };
            if (mpv_render_context_create(&m_ctx, m_item->handle(), params) < 0)
                qFatal("mpv_render_context_create failed");
            mpv_render_context_set_update_callback(m_ctx, onMpvUpdate, m_item);
        }
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override
    {
        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mfbo{ int(fbo->handle()), fbo->width(), fbo->height(), 0 };
        int flipY = 0;
        int block = 0; // never sleep on the render thread (0b lesson)
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &mfbo },
            { MPV_RENDER_PARAM_FLIP_Y, &flipY },
            { MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };
        mpv_render_context_update(m_item->renderContext);
        mpv_render_context_render(m_item->renderContext, params);
        QQuickOpenGLUtils::resetOpenGLState(); // mpv leaves raw GL state behind
    }

private:
    MpvItem *m_item;
};

} // namespace

MpvItem::MpvItem(QQuickItem *parent) : QQuickFramebufferObject(parent)
{
    m_mpv = mpv_create();
    if (!m_mpv)
        qFatal("mpv_create failed");

    const QByteArray debug = qgetenv("DISCPLAYER_MPV_LOG");
    if (!debug.isEmpty()) {
        mpv_set_option_string(m_mpv, "log-file", debug.constData());
        mpv_set_option_string(m_mpv, "msg-level", "all=v");
    }
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "keep-open", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "osd-level", "0");
    mpv_set_option_string(m_mpv, "sid", "auto");
    mpv_set_option_string(m_mpv, "audio-channels", "auto-safe");
    if (mpv_initialize(m_mpv) < 0)
        qFatal("mpv_initialize failed");

    mpv_observe_property(m_mpv, PropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropIdle, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, PropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, PropTitle, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, PropChapter, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, PropChapterList, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, PropTrackList, "track-list", MPV_FORMAT_NODE);

    connect(this, &MpvItem::requestUpdate, this, &QQuickItem::update, Qt::QueuedConnection);
    mpv_set_wakeup_callback(m_mpv, onMpvWakeup, this);
}

MpvItem::~MpvItem()
{
    mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    // Same order as mpv's Qt example: the item is destroyed at shutdown,
    // after the window has stopped rendering.
    if (renderContext)
        mpv_render_context_free(renderContext);
    mpv_terminate_destroy(m_mpv);
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    window()->setPersistentGraphics(true);
    window()->setPersistentSceneGraph(true);
    return new MpvRenderer(const_cast<MpvItem *>(this));
}

void MpvItem::load(const QString &url, const QVariantMap &options)
{
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        setMpvProperty(it.key(), it.value());
    command({ "loadfile", url });
}

void MpvItem::stop()
{
    command({ "stop" });
}

void MpvItem::command(const QVariantList &args)
{
    NodeBuilder b;
    mpv_node node = b.build(args);
    mpv_command_node_async(m_mpv, 0, &node);
}

void MpvItem::setMpvProperty(const QString &name, const QVariant &value)
{
    NodeBuilder b;
    mpv_node node = b.build(value);
    mpv_set_property_async(m_mpv, 0, name.toUtf8().constData(), MPV_FORMAT_NODE, &node);
}

void MpvItem::processEvents()
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
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            emit fileLoaded();
            break;
        case MPV_EVENT_END_FILE: {
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            static const char *const reasons[] = { "eof", "", "stop", "quit", "error", "redirect" };
            const int r = int(ef->reason);
            emit endFile(QString::fromLatin1(r >= 0 && r <= 5 ? reasons[r] : "unknown"));
            break;
        }
        case MPV_EVENT_LOG_MESSAGE:
        default:
            break;
        }
    }
}
