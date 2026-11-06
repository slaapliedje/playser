// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlexClient.h"
#include <QCoreApplication>
#include <QJSEngine>
#include <QJsonDocument>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QNetworkReply>
#include <QSettings>
#include <QSysInfo>
#include <QUrlQuery>
#include <QUuid>
#include <memory>

namespace {

const QString kPlexTv = QStringLiteral("https://plex.tv");
constexpr int kPinPollMs = 2000;
// plex.tv's codes last 15 minutes; stop asking well before
constexpr int kPinPolls = 14 * 60 * 1000 / kPinPollMs;
constexpr int kConnectTimeoutMs = 4000;

QVariant parseJson(QNetworkReply *reply)
{
    return QJsonDocument::fromJson(reply->readAll()).toVariant();
}

// the server QML images may load from, and its token: read from the
// engine's loader threads
QMutex imageAuthLock;
QString imageAuthBase;
QByteArray imageAuthToken;

class ImageAuthNam : public QNetworkAccessManager
{
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &original, QIODevice *data) override
    {
        QNetworkRequest req(original);
        {
            QMutexLocker lock(&imageAuthLock);
            if (!imageAuthBase.isEmpty() && req.url().toString().startsWith(imageAuthBase + '/'))
                req.setRawHeader("X-Plex-Token", imageAuthToken);
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

PlexClient::PlexClient(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    QSettings settings;
    settings.beginGroup("plex");
    // identifies this install to Plex: its devices list and sessions
    m_clientId = settings.value("clientId").toString();
    if (m_clientId.isEmpty()) {
        m_clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("clientId", m_clientId);
    }
    m_userToken = settings.value("token").toString();
    m_pinPoll.setInterval(kPinPollMs);
    connect(&m_pinPoll, &QTimer::timeout, this, &PlexClient::pollPin);
}

void PlexClient::installImageAuth(QQmlEngine *engine)
{
    static ImageAuthFactory factory;
    engine->setNetworkAccessManagerFactory(&factory);
}

void PlexClient::setServer(const QString &baseUrl, const QString &token)
{
    m_baseUrl = baseUrl;
    m_serverToken = token;
    QMutexLocker lock(&imageAuthLock);
    imageAuthBase = baseUrl;
    imageAuthToken = token.toUtf8();
}

void PlexClient::setState(const QString &state, const QString &message)
{
    m_state = state;
    m_message = message;
    emit stateChanged();
}

QNetworkRequest PlexClient::request(const QUrl &url, const QString &token) const
{
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("X-Plex-Product", "Playser");
    req.setRawHeader("X-Plex-Version", QCoreApplication::applicationVersion().toUtf8());
    req.setRawHeader("X-Plex-Client-Identifier", m_clientId.toUtf8());
    req.setRawHeader("X-Plex-Platform", "Linux");
    req.setRawHeader("X-Plex-Device", "SteamOS");
    req.setRawHeader("X-Plex-Device-Name", QSysInfo::machineHostName().toUtf8());
    if (!token.isEmpty())
        req.setRawHeader("X-Plex-Token", token.toUtf8());
    req.setTransferTimeout(15000);
    return req;
}

void PlexClient::start()
{
    if (m_state == "ready" || m_state == "connecting" || m_state == "linking")
        return;
    const quint64 gen = ++m_generation;
    if (!m_userToken.isEmpty()) {
        findServer();
        return;
    }
    setState("linking");
    m_linkCode.clear();
    QNetworkReply *reply = m_net->post(request(QUrl(kPlexTv + "/api/v2/pins"), {}),
                                       QByteArray("strong=false"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != m_generation)
            return;
        const QVariantMap pin = parseJson(reply).toMap();
        if (reply->error() != QNetworkReply::NoError || pin.value("code").toString().isEmpty()) {
            qWarning("plex: can't get a sign-in code: %s", qPrintable(reply->errorString()));
            setState("error", QStringLiteral("Can't reach plex.tv"));
            return;
        }
        m_pinId = pin.value("id").toLongLong();
        m_linkCode = pin.value("code").toString();
        m_pinPolls = 0;
        m_pinPoll.start();
        setState("linking");
    });
}

void PlexClient::pollPin()
{
    if (++m_pinPolls > kPinPolls) { // the code has run out: a new one
        m_pinPoll.stop();
        m_state.clear();
        start();
        return;
    }
    const quint64 gen = m_generation;
    QNetworkReply *reply = m_net->get(request(QUrl(kPlexTv + "/api/v2/pins/" + QString::number(m_pinId)), {}));
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != m_generation || m_state != "linking")
            return;
        const QString token = parseJson(reply).toMap().value("authToken").toString();
        if (token.isEmpty())
            return; // not entered yet
        m_pinPoll.stop();
        m_userToken = token;
        QSettings().setValue("plex/token", token);
        qInfo("plex: signed in");
        findServer();
    });
}

void PlexClient::cancelSignIn()
{
    if (m_state != "linking")
        return;
    ++m_generation;
    m_pinPoll.stop();
    m_linkCode.clear();
    setState("signedOut");
}

void PlexClient::signOut()
{
    ++m_generation;
    m_pinPoll.stop();
    m_userToken.clear();
    setServer({}, {});
    m_serverName.clear();
    m_linkCode.clear();
    QSettings settings;
    settings.remove("plex/token");
    settings.remove("plex/server");
    setState("signedOut");
}

void PlexClient::findServer()
{
    setState("connecting");
    const quint64 gen = m_generation;
    QUrl url(kPlexTv + "/api/v2/resources");
    url.setQuery("includeHttps=1&includeRelay=1");
    QNetworkReply *reply = m_net->get(request(url, m_userToken));
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != m_generation)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401) { // signed out elsewhere, or the token was revoked
            qInfo("plex: token no longer valid");
            signOut();
            start();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("plex: can't list servers: %s", qPrintable(reply->errorString()));
            setState("error", QStringLiteral("Can't reach plex.tv"));
            return;
        }
        // the server used last, else the account's own, else any shared one
        const QString saved = QSettings().value("plex/server").toString();
        QVariantMap chosen;
        int best = -1;
        for (const QVariant &v : parseJson(reply).toList()) {
            const QVariantMap r = v.toMap();
            if (!r.value("provides").toString().split(',').contains("server"))
                continue;
            const int rank = r.value("clientIdentifier").toString() == saved ? 2
                : r.value("owned").toBool() ? 1 : 0;
            if (rank > best) {
                best = rank;
                chosen = r;
            }
        }
        if (chosen.isEmpty()) {
            setState("error", QStringLiteral("No Plex Media Server on this account"));
            return;
        }
        // local addresses first (plex.direct HTTPS, then the plain address,
        // which a server allows unless it requires secure connections), then
        // remote, then Plex's relay
        QVariantList candidates;
        auto add = [&candidates](const QString &uri, const QString &kind, int rank) {
            candidates.append(QVariantMap{ { "uri", uri }, { "kind", kind }, { "rank", rank } });
        };
        for (const QVariant &v : chosen.value("connections").toList()) {
            const QVariantMap c = v.toMap();
            const bool local = c.value("local").toBool();
            const bool relay = c.value("relay").toBool();
            const QString kind = relay ? "relay" : local ? "local" : "remote";
            const int rank = relay ? 4 : local ? 0 : 2;
            add(c.value("uri").toString(), kind, rank);
            if (local && !relay && c.value("protocol").toString() == "https")
                add(QStringLiteral("http://%1:%2").arg(c.value("address").toString(),
                                                       c.value("port").toString()), kind, rank + 1);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("rank").toInt() < b.toMap().value("rank").toInt();
        });
        const QString token = chosen.value("accessToken").toString();
        tryConnections(candidates, chosen.value("name").toString(), token.isEmpty() ? m_userToken : token);
        QSettings().setValue("plex/server", chosen.value("clientIdentifier").toString());
    });
}

// Asks every address at once; the best-ranked that answers wins as soon as
// every better one has failed.
void PlexClient::tryConnections(const QVariantList &candidates, const QString &name, const QString &token)
{
    const quint64 gen = m_generation;
    // per candidate: 0 waiting, 1 answered, -1 failed
    auto results = std::make_shared<QVector<int>>(candidates.size(), 0);
    auto decide = [this, candidates, results, name, token, gen] {
        if (gen != m_generation || m_state != "connecting")
            return;
        for (int i = 0; i < candidates.size(); ++i) {
            if ((*results)[i] == 0)
                return; // a better one may still answer
            if ((*results)[i] == 1) {
                const QVariantMap c = candidates[i].toMap();
                setServer(c.value("uri").toString(), token);
                m_serverName = name;
                qInfo("plex: using %s (%s)", qPrintable(name), qPrintable(c.value("kind").toString()));
                setState("ready");
                return;
            }
        }
        qWarning("plex: %s didn't answer at any address", qPrintable(name));
        setState("error", QStringLiteral("Can't reach %1").arg(name));
    };
    if (candidates.isEmpty()) {
        decide();
        return;
    }
    for (int i = 0; i < candidates.size(); ++i) {
        QNetworkRequest req = request(QUrl(candidates[i].toMap().value("uri").toString() + "/identity"), token);
        req.setTransferTimeout(kConnectTimeoutMs);
        QNetworkReply *reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, results, i, decide] {
            reply->deleteLater();
            (*results)[i] = reply->error() == QNetworkReply::NoError ? 1 : -1;
            decide();
        });
    }
}

void PlexClient::get(const QString &path, const QJSValue &callback)
{
    QJSEngine *engine = qjsEngine(this);
    if (m_state != "ready") {
        if (callback.isCallable() && engine)
            QJSValue(callback).call({ QJSValue::NullValue, QStringLiteral("Not connected") });
        return;
    }
    QNetworkReply *reply = m_net->get(request(QUrl(m_baseUrl + path), m_serverToken));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        QJSEngine *engine = qjsEngine(this);
        if (!engine || !callback.isCallable())
            return;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("plex: %s: %s", qPrintable(reply->url().path()), qPrintable(reply->errorString()));
            QJSValue(callback).call({ QJSValue::NullValue, QStringLiteral("Your Plex server didn't answer") });
            return;
        }
        const QVariant container = parseJson(reply).toMap().value("MediaContainer");
        QJSValue(callback).call({ engine->toScriptValue(container) });
    });
}

QString PlexClient::imageUrl(const QString &thumb, int width, int height) const
{
    if (thumb.isEmpty() || m_baseUrl.isEmpty())
        return {};
    QUrlQuery q;
    q.addQueryItem("width", QString::number(width));
    q.addQueryItem("height", QString::number(height));
    q.addQueryItem("minSize", "1");
    q.addQueryItem("upscale", "1");
    q.addQueryItem("url", thumb);
    QUrl url(m_baseUrl + "/photo/:/transcode");
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

QVariantMap PlexClient::playable(const QVariantMap &item) const
{
    const QVariantMap media = item.value("Media").toList().value(0).toMap();
    const QString part = media.value("Part").toList().value(0).toMap().value("key").toString();
    if (part.isEmpty() || m_baseUrl.isEmpty())
        return {};
    const QString type = item.value("type").toString();
    const QString title = item.value("title").toString();
    return {
        { "url", m_baseUrl + part },
        { "label", title },
        { "title", title },
        { "kind", QStringLiteral("plex") },
        { "ratingKey", item.value("ratingKey").toString() },
        { "contentHash", "plex:" + item.value("ratingKey").toString() },
        { "options", QVariantMap{ { "http-header-fields", "X-Plex-Token: " + m_serverToken } } },
        { "plex", true },
        // an episode's show, for the player's title
        { "show", type == "episode" ? item.value("grandparentTitle").toString() : QString() },
    };
}

void PlexClient::serverGet(const QString &path, const QUrlQuery &query)
{
    if (m_state != "ready")
        return;
    QUrl url(m_baseUrl + path);
    url.setQuery(query);
    QNetworkReply *reply = m_net->get(request(url, m_serverToken));
    connect(reply, &QNetworkReply::finished, reply, [reply] {
        if (reply->error() != QNetworkReply::NoError)
            qWarning("plex: %s: %s", qPrintable(reply->url().path()), qPrintable(reply->errorString()));
        reply->deleteLater();
    });
}

void PlexClient::report(const QString &ratingKey, const QString &state, double seconds, double duration)
{
    if (ratingKey.isEmpty())
        return;
    QUrlQuery q;
    q.addQueryItem("ratingKey", ratingKey);
    q.addQueryItem("key", "/library/metadata/" + ratingKey);
    q.addQueryItem("state", state);
    q.addQueryItem("time", QString::number(qint64(seconds * 1000)));
    if (duration > 0)
        q.addQueryItem("duration", QString::number(qint64(duration * 1000)));
    serverGet("/:/timeline", q);
}

void PlexClient::scrobble(const QString &ratingKey)
{
    if (ratingKey.isEmpty())
        return;
    QUrlQuery q;
    q.addQueryItem("identifier", "com.plexapp.plugins.library");
    q.addQueryItem("key", ratingKey);
    serverGet("/:/scrobble", q);
}
