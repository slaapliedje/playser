// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlexClient.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QSysInfo>
#include <QUrlQuery>
#include <algorithm>
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

// Plex's Metadata as a MediaServer item
QVariantMap toItem(const QVariantMap &m)
{
    const QString type = m.value("type").toString();
    const bool episode = type == "episode";
    const bool season = type == "season";
    const int leaves = m.value("leafCount").toInt();
    return {
        { "id", m.value("ratingKey").toString() },
        { "type", type },
        { "title", m.value("title").toString() },
        { "year", m.value("year").toInt() },
        { "summary", m.value("summary").toString() },
        { "contentRating", m.value("contentRating").toString() },
        { "duration", m.value("duration").toDouble() / 1000 },
        { "position", m.value("viewOffset").toDouble() / 1000 },
        { "watched", type == "show" || season ? leaves > 0 && m.value("viewedLeafCount").toInt() >= leaves
                                              : m.value("viewCount").toInt() > 0 },
        { "show", episode ? m.value("grandparentTitle").toString() : season ? m.value("parentTitle").toString() : QString() },
        { "season", episode ? m.value("parentIndex").toInt() : season ? m.value("index").toInt() : 0 },
        { "number", episode ? m.value("index").toInt() : 0 },
        { "childCount", leaves },
        { "thumb", m.value("thumb").toString() },
        { "showThumb", m.value("grandparentThumb").toString() },
        { "art", m.value("art").toString() },
        // what plays
        { "part", m.value("Media").toList().value(0).toMap().value("Part").toList().value(0).toMap().value("key").toString() },
    };
}

QVariantList toItems(const QVariantMap &container)
{
    QVariantList out;
    for (const QVariant &v : container.value("Metadata").toList())
        out.append(toItem(v.toMap()));
    return out;
}

} // namespace

PlexClient::PlexClient(QObject *parent)
    : MediaServer(parent)
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

void PlexClient::setServer(const QString &baseUrl, const QString &token)
{
    m_baseUrl = baseUrl;
    m_serverToken = token;
    setImageAuth(baseUrl, "X-Plex-Token", token.toUtf8());
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
    m_serverId.clear();
    m_resources.clear();
    m_wanted.clear();
    m_linkCode.clear();
    QSettings settings;
    settings.remove("plex/token");
    settings.remove("plex/server");
    setState("signedOut");
}

QVariantList PlexClient::servers() const
{
    QVariantList out;
    for (const QVariant &v : m_resources) {
        const QVariantMap r = v.toMap();
        const QString id = r.value("clientIdentifier").toString();
        out.append(QVariantMap{ { "id", id }, { "name", r.value("name").toString() },
                                { "owned", r.value("owned").toBool() },
                                { "owner", r.value("sourceTitle").toString() },
                                { "online", r.value("presence").toBool() },
                                { "current", id == m_serverId && m_state == "ready" } });
    }
    std::stable_partition(out.begin(), out.end(), [](const QVariant &v) { return v.toMap().value("online").toBool(); });
    return out;
}

void PlexClient::useServer(const QString &id)
{
    if (id == m_serverId && m_state == "ready")
        return;
    ++m_generation;
    m_wanted = id;
    findServer();
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
        // the server asked for, else the one used last, else the account's
        // own, else any shared one
        const QString saved = QSettings().value("plex/server").toString();
        QVariantMap chosen;
        int best = -1;
        m_resources.clear();
        for (const QVariant &v : parseJson(reply).toList()) {
            const QVariantMap r = v.toMap();
            if (!r.value("provides").toString().split(',').contains("server"))
                continue;
            m_resources.append(r);
            const QString id = r.value("clientIdentifier").toString();
            const int rank = id == m_wanted ? 3 : id == saved ? 2 : r.value("owned").toBool() ? 1 : 0;
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
        tryConnections(candidates, chosen.value("clientIdentifier").toString(), chosen.value("name").toString(),
                       token.isEmpty() ? m_userToken : token);
    });
}

// Asks every address at once; the best-ranked that answers wins as soon as
// every better one has failed.
void PlexClient::tryConnections(const QVariantList &candidates, const QString &id, const QString &name,
                                const QString &token)
{
    const quint64 gen = m_generation;
    // per candidate: 0 waiting, 1 answered, -1 failed
    auto results = std::make_shared<QVector<int>>(candidates.size(), 0);
    auto decide = [this, candidates, results, id, name, token, gen] {
        if (gen != m_generation || m_state != "connecting")
            return;
        for (int i = 0; i < candidates.size(); ++i) {
            if ((*results)[i] == 0)
                return; // a better one may still answer
            if ((*results)[i] == 1) {
                const QVariantMap c = candidates[i].toMap();
                setServer(c.value("uri").toString(), token);
                m_serverId = id;
                m_serverName = name;
                m_wanted.clear();
                QSettings().setValue("plex/server", id); // kept once it answers
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

void PlexClient::get(const QString &path, const QJSValue &callback,
                     const std::function<void(const QVariantMap &)> &done)
{
    if (m_state != "ready") {
        fail(callback, QStringLiteral("Not connected"));
        return;
    }
    QNetworkReply *reply = m_net->get(request(QUrl(m_baseUrl + path), m_serverToken));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("plex: %s: %s", qPrintable(reply->url().path()), qPrintable(reply->errorString()));
            fail(callback, QStringLiteral("Your Plex server didn't answer"));
            return;
        }
        done(parseJson(reply).toMap().value("MediaContainer").toMap());
    });
}

void PlexClient::getItems(const QString &path, const QJSValue &callback)
{
    get(path, callback, [this, callback](const QVariantMap &c) { answer(callback, toItems(c)); });
}

void PlexClient::libraries(const QJSValue &callback)
{
    get("/library/sections", callback, [this, callback](const QVariantMap &c) {
        QVariantList out;
        for (const QVariant &v : c.value("Directory").toList()) {
            const QVariantMap d = v.toMap();
            const QString type = d.value("type").toString();
            if (type == "movie" || type == "show")
                out.append(QVariantMap{ { "id", d.value("key").toString() }, { "title", d.value("title").toString() },
                                        { "type", type } });
        }
        answer(callback, out);
    });
}

void PlexClient::continueWatching(const QJSValue &callback)
{
    getItems("/library/onDeck", callback);
}

void PlexClient::items(const QString &libraryId, const QJSValue &callback)
{
    getItems("/library/sections/" + libraryId + "/all", callback);
}

void PlexClient::children(const QVariantMap &item, const QJSValue &callback)
{
    getItems("/library/metadata/" + item.value("id").toString() + "/children", callback);
}

void PlexClient::refresh(const QVariantMap &item, const QJSValue &callback)
{
    get("/library/metadata/" + item.value("id").toString(), callback, [this, callback](const QVariantMap &c) {
        const QVariantList items = toItems(c);
        if (items.isEmpty())
            fail(callback, QStringLiteral("It's no longer on your Plex server"));
        else
            answer(callback, items.first());
    });
}

// the server's search: a hub of results per type, best matches first
void PlexClient::search(const QString &query, const QJSValue &callback)
{
    get("/hubs/search?limit=30&query=" + QString::fromUtf8(QUrl::toPercentEncoding(query)), callback,
        [this, callback](const QVariantMap &c) {
        QVariantList out;
        for (const QVariant &h : c.value("Hub").toList()) {
            const QVariantMap hub = h.toMap();
            const QString type = hub.value("type").toString();
            if (type == "movie" || type == "show" || type == "episode")
                out += toItems(hub);
        }
        answer(callback, out);
    });
}

void PlexClient::firstEpisode(const QVariantMap &show, const QJSValue &callback)
{
    get("/library/metadata/" + show.value("id").toString() + "/allLeaves?X-Plex-Container-Start=0&X-Plex-Container-Size=1",
        callback, [this, callback](const QVariantMap &c) { answer(callback, toItems(c).value(0)); });
}

QString PlexClient::imageUrl(const QString &image, int width, int height) const
{
    if (image.isEmpty() || m_baseUrl.isEmpty())
        return {};
    QUrlQuery q;
    q.addQueryItem("width", QString::number(width));
    q.addQueryItem("height", QString::number(height));
    q.addQueryItem("minSize", "1");
    q.addQueryItem("upscale", "1");
    q.addQueryItem("url", image);
    QUrl url(m_baseUrl + "/photo/:/transcode");
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

QVariantMap PlexClient::playable(const QVariantMap &item) const
{
    const QString part = item.value("part").toString();
    if (part.isEmpty() || m_baseUrl.isEmpty())
        return {};
    const QString title = item.value("title").toString();
    return {
        { "url", m_baseUrl + part },
        { "label", title },
        { "title", title },
        { "kind", QStringLiteral("plex") },
        { "library", QStringLiteral("plex") },
        { "itemId", item.value("id").toString() },
        { "contentHash", "plex:" + item.value("id").toString() },
        { "options", QVariantMap{ { "http-header-fields", "X-Plex-Token: " + m_serverToken } } },
        // an episode's show, for the player's title
        { "show", item.value("show").toString() },
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
