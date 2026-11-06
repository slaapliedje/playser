// SPDX-License-Identifier: GPL-3.0-or-later
#include "MediaServer.h"
#include <QHash>
#include <QJSEngine>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>

namespace {

// per server: the address QML images may load from, and the header with its
// token. Read from the engine's loader threads.
struct ImageAuth {
    QString base;
    QByteArray header;
    QByteArray value;
};
QMutex imageAuthLock;
QHash<const MediaServer *, ImageAuth> imageAuth;

class ImageAuthNam : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &original, QIODevice *data) override
    {
        QNetworkRequest req(original);
        {
            const QString url = req.url().toString();
            QMutexLocker lock(&imageAuthLock);
            for (const ImageAuth &a : std::as_const(imageAuth)) {
                if (!a.base.isEmpty() && url.startsWith(a.base + '/'))
                    req.setRawHeader(a.header, a.value);
            }
        }
        return QNetworkAccessManager::createRequest(op, req, data);
    }
};

class ImageAuthFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override { return new ImageAuthNam(parent); }
};

} // namespace

MediaServer::MediaServer(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    connect(this, &QObject::destroyed, [this] {
        QMutexLocker lock(&imageAuthLock);
        imageAuth.remove(this);
    });
}

void MediaServer::installImageAuth(QQmlEngine *engine)
{
    static ImageAuthFactory factory;
    engine->setNetworkAccessManagerFactory(&factory);
}

void MediaServer::setImageAuth(const QString &base, const QByteArray &header, const QByteArray &value)
{
    QMutexLocker lock(&imageAuthLock);
    imageAuth.insert(this, { base, header, value });
}

void MediaServer::setState(const QString &state, const QString &message)
{
    m_state = state;
    m_message = message;
    emit stateChanged();
}

void MediaServer::answer(const QJSValue &callback, const QVariant &result)
{
    QJSEngine *engine = qjsEngine(this);
    if (engine && callback.isCallable())
        QJSValue(callback).call({ engine->toScriptValue(result) });
}

void MediaServer::fail(const QJSValue &callback, const QString &message)
{
    if (qjsEngine(this) && callback.isCallable())
        QJSValue(callback).call({ QJSValue::NullValue, message });
}
