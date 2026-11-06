// SPDX-License-Identifier: GPL-3.0-or-later
#include "JellyfinClient.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QSettings>
#include <QSysInfo>
#include <QUdpSocket>
#include <QUrlQuery>
#include <QUuid>
#include <QSet>
#include <algorithm>
#include <memory>

namespace {

constexpr quint16 kDiscoveryPort = 7359;
constexpr int kPollMs = 2000;
// Quick Connect codes last 10 minutes; ask for a new one before
constexpr int kPolls = 9 * 60 * 1000 / kPollMs;
constexpr int kCheckTimeoutMs = 5000;
// Jellyfin counts time in 100 ns ticks
constexpr double kTicks = 10000000.0;
const QString kSigningIn = QStringLiteral("Signing in…");

QVariant parseJson(QNetworkReply *reply)
{
    return QJsonDocument::fromJson(reply->readAll()).toVariant();
}

int status(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// a header value can't hold quotes
QString quoted(QString s)
{
    return '"' + s.remove('"').remove(',') + '"';
}

// "id/Primary/tag" for imageUrl(), or "" when there's no such image
QString image(const QString &id, const QString &type, const QString &tag)
{
    return id.isEmpty() || tag.isEmpty() ? QString() : id + '/' + type + '/' + tag;
}

// Jellyfin's BaseItemDto as a MediaServer item
QVariantMap toItem(const QVariantMap &m)
{
    static const QHash<QString, QString> types = {
        { "Movie", "movie" }, { "Series", "show" }, { "Season", "season" }, { "Episode", "episode" },
    };
    const QString type = types.value(m.value("Type").toString(), m.value("Type").toString().toLower());
    const bool episode = type == "episode";
    const QString id = m.value("Id").toString();
    const QVariantMap user = m.value("UserData").toMap();
    const QString seriesId = m.value("SeriesId").toString();
    const QString showThumb = image(seriesId, "Primary", m.value("SeriesPrimaryImageTag").toString());
    QString thumb = image(id, "Primary", m.value("ImageTags").toMap().value("Primary").toString());
    if (thumb.isEmpty() && type == "season")
        thumb = showThumb;
    QString art = image(id, "Backdrop", m.value("BackdropImageTags").toList().value(0).toString());
    if (art.isEmpty())
        art = image(m.value("ParentBackdropItemId").toString(), "Backdrop",
                    m.value("ParentBackdropImageTags").toList().value(0).toString());
    return {
        { "id", id },
        { "type", type },
        { "title", m.value("Name").toString() },
        { "year", m.value("ProductionYear").toInt() },
        { "summary", m.value("Overview").toString() },
        { "contentRating", m.value("OfficialRating").toString() },
        { "duration", m.value("RunTimeTicks").toDouble() / kTicks },
        { "position", user.value("PlaybackPositionTicks").toDouble() / kTicks },
        { "watched", user.value("Played").toBool() },
        { "show", m.value("SeriesName").toString() },
        { "season", episode ? m.value("ParentIndexNumber").toInt() : type == "season" ? m.value("IndexNumber").toInt() : 0 },
        { "number", episode ? m.value("IndexNumber").toInt() : 0 },
        { "childCount", m.value("ChildCount").toInt() },
        { "thumb", thumb },
        { "showThumb", showThumb },
        { "art", art },
        // a season's episodes are listed through its show
        { "seriesId", seriesId },
    };
}

QVariantList toItems(const QVariant &json)
{
    QVariantList out;
    for (const QVariant &v : json.toMap().value("Items").toList())
        out.append(toItem(v.toMap()));
    return out;
}

// what's typed as a server address, as the URLs it may mean: Jellyfin's
// own ports unless one is given, and plain http unless https is
QStringList candidatesFor(QString address)
{
    address = address.trimmed();
    while (address.endsWith('/'))
        address.chop(1);
    if (address.isEmpty())
        return {};
    if (address.contains("://"))
        return { address };
    const QUrl url("http://" + address);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    if (url.port() != -1)
        return { "http://" + address, "https://" + address };
    // the default ports, then a reverse proxy's
    return { "http://" + url.host() + ":8096" + url.path(), "https://" + url.host() + ":8920" + url.path(),
             "https://" + address, "http://" + address };
}

} // namespace

JellyfinClient::JellyfinClient(QObject *parent)
    : MediaServer(parent)
{
    QSettings settings;
    settings.beginGroup("jellyfin");
    // identifies this install to the server: its devices and sessions
    m_deviceId = settings.value("deviceId").toString();
    if (m_deviceId.isEmpty()) {
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("deviceId", m_deviceId);
    }
    m_baseUrl = settings.value("server").toString();
    m_serverName = settings.value("serverName").toString();
    m_token = settings.value("token").toString();
    m_userId = settings.value("userId").toString();
    setImageAuth(m_baseUrl, "Authorization", "MediaBrowser Token=" + quoted(m_token).toUtf8());
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &JellyfinClient::pollQuickConnect);
}

void JellyfinClient::setBase(const QString &base)
{
    m_baseUrl = base;
    setImageAuth(base, "Authorization", "MediaBrowser Token=" + quoted(m_token).toUtf8());
}

QNetworkRequest JellyfinClient::request(const QString &path, bool withToken) const
{
    QNetworkRequest req(QUrl(m_baseUrl + path));
    QString auth = QStringLiteral("MediaBrowser Client=\"Playser\", Device=%1, DeviceId=%2, Version=%3")
                       .arg(quoted(QSysInfo::machineHostName()), quoted(m_deviceId),
                            quoted(QCoreApplication::applicationVersion()));
    if (withToken && !m_token.isEmpty())
        auth += ", Token=" + quoted(m_token);
    req.setRawHeader("Authorization", auth.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(15000);
    return req;
}

void JellyfinClient::send(QNetworkReply *reply, const Done &done)
{
    const quint64 gen = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, done] {
        reply->deleteLater();
        if (gen == m_generation)
            done(reply);
    });
}

QNetworkReply *JellyfinClient::postJson(const QString &path, const QVariantMap &body, bool withToken)
{
    QNetworkRequest req = request(path, withToken);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    return m_net->post(req, QJsonDocument::fromVariant(body).toJson(QJsonDocument::Compact));
}

void JellyfinClient::start()
{
    if (m_state != "signedOut" && m_state != "error")
        return;
    ++m_generation;
    if (m_baseUrl.isEmpty()) {
        setState("address");
        discover();
    } else if (m_token.isEmpty()) {
        tryAddresses({ m_baseUrl });
    } else {
        connectSaved();
    }
}

// signed in before: check the token still works
void JellyfinClient::connectSaved()
{
    setState("connecting");
    send(m_net->get(request("/Users/Me")), [this](QNetworkReply *reply) {
        if (status(reply) == 401) { // signed out on the server, or the token was revoked
            qInfo("jellyfin: token no longer valid");
            m_token.clear();
            QSettings().remove("jellyfin/token");
            tryAddresses({ m_baseUrl });
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("jellyfin: can't reach the server: %s", qPrintable(reply->errorString()));
            setState("error", QStringLiteral("Can't reach %1").arg(m_serverName.isEmpty() ? m_baseUrl : m_serverName));
            return;
        }
        m_userId = parseJson(reply).toMap().value("Id").toString();
        qInfo("jellyfin: using %s", qPrintable(m_serverName));
        setState("ready");
    });
}

void JellyfinClient::discover()
{
    if (!m_discovery) {
        m_discovery = new QUdpSocket(this);
        m_discovery->bind(QHostAddress::AnyIPv4, 0);
        connect(m_discovery, &QUdpSocket::readyRead, this, [this] {
            bool changed = false;
            while (m_discovery->hasPendingDatagrams()) {
                const QVariantMap r = QJsonDocument::fromJson(m_discovery->receiveDatagram().data()).toVariant().toMap();
                const QString address = r.value("Address").toString();
                if (address.isEmpty() || std::any_of(m_servers.cbegin(), m_servers.cend(), [&r](const QVariant &s) {
                        return s.toMap().value("id") == r.value("Id"); }))
                    continue;
                m_servers.append(QVariantMap{ { "name", r.value("Name").toString() }, { "address", address },
                                              { "id", r.value("Id").toString() } });
                changed = true;
            }
            if (changed)
                emit serversChanged();
        });
    }
    // every network this device is on (a broadcast doesn't cross routers:
    // a server on another subnet has to be typed in)
    const QByteArray ask("Who is JellyfinServer?");
    m_discovery->writeDatagram(ask, QHostAddress::Broadcast, kDiscoveryPort);
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &e : i.addressEntries()) {
            if (!e.broadcast().isNull())
                m_discovery->writeDatagram(ask, e.broadcast(), kDiscoveryPort);
        }
    }
}

void JellyfinClient::useServer(const QString &address)
{
    const QStringList candidates = candidatesFor(address);
    if (candidates.isEmpty()) {
        setState("address", QStringLiteral("That isn't a server address"));
        return;
    }
    ++m_generation;
    m_token.clear();
    tryAddresses(candidates);
}

// the first address that answers as a Jellyfin server, in turn
void JellyfinClient::tryAddresses(QStringList candidates)
{
    if (candidates.isEmpty()) {
        setState("address", QStringLiteral("No Jellyfin server answered there"));
        return;
    }
    const QString base = candidates.takeFirst();
    setState("connecting");
    QNetworkRequest req(QUrl(base + "/System/Info/Public"));
    req.setTransferTimeout(kCheckTimeoutMs);
    send(m_net->get(req), [this, base, candidates](QNetworkReply *reply) {
        const QVariantMap info = parseJson(reply).toMap();
        if (reply->error() != QNetworkReply::NoError || info.value("Id").toString().isEmpty()) {
            tryAddresses(candidates);
            return;
        }
        setBase(base);
        m_serverName = info.value("ServerName").toString();
        QSettings settings;
        settings.setValue("jellyfin/server", base);
        settings.setValue("jellyfin/serverName", m_serverName);
        qInfo("jellyfin: found %s, version %s", qPrintable(m_serverName), qPrintable(info.value("Version").toString()));
        beginSignIn();
    });
}

// Quick Connect if the server allows it, else a password
void JellyfinClient::beginSignIn()
{
    send(m_net->get(request("/Users/Public", false)), [this](QNetworkReply *reply) {
        m_users.clear();
        for (const QVariant &u : parseJson(reply).toList())
            m_users.append(u.toMap().value("Name").toString());
    });
    send(m_net->get(request("/QuickConnect/Enabled", false)), [this](QNetworkReply *reply) {
        m_quickConnect = reply->readAll().trimmed() == "true"; // a bare value, not a JSON document
        if (m_quickConnect)
            initiateQuickConnect();
        else
            setState("password");
    });
}

void JellyfinClient::initiateQuickConnect()
{
    m_poll.stop();
    m_linkCode.clear();
    send(postJson("/QuickConnect/Initiate", {}, false), [this](QNetworkReply *reply) {
        const QVariantMap r = parseJson(reply).toMap();
        if (reply->error() != QNetworkReply::NoError || r.value("Code").toString().isEmpty()) {
            qWarning("jellyfin: Quick Connect didn't start: %s", qPrintable(reply->errorString()));
            m_quickConnect = false;
            setState("password");
            return;
        }
        m_secret = r.value("Secret").toString();
        m_linkCode = r.value("Code").toString();
        m_polls = 0;
        m_poll.start();
        setState("linking");
    });
}

void JellyfinClient::pollQuickConnect()
{
    if (++m_polls > kPolls) { // the code has run out: a new one
        initiateQuickConnect();
        return;
    }
    QUrlQuery q;
    q.addQueryItem("secret", m_secret);
    send(m_net->get(request("/QuickConnect/Connect?" + q.toString(QUrl::FullyEncoded), false)), [this](QNetworkReply *reply) {
        if (m_state != "linking" || !parseJson(reply).toMap().value("Authenticated").toBool())
            return; // not approved yet
        m_poll.stop();
        send(postJson("/Users/AuthenticateWithQuickConnect", { { "Secret", m_secret } }, false),
             [this](QNetworkReply *reply) { finishSignIn(reply); });
    });
}

void JellyfinClient::usePassword()
{
    if (m_state != "linking")
        return;
    m_poll.stop();
    setState("password");
}

void JellyfinClient::useQuickConnect()
{
    if (m_state == "password" && m_quickConnect)
        initiateQuickConnect();
}

void JellyfinClient::signIn(const QString &user, const QString &password)
{
    if (m_state != "password" || m_message == kSigningIn)
        return;
    setState("password", kSigningIn); // the form stays, for another try
    send(postJson("/Users/AuthenticateByName", { { "Username", user }, { "Pw", password } }, false),
         [this](QNetworkReply *reply) {
             if (status(reply) == 401) {
                 setState("password", QStringLiteral("That name and password didn't work"));
                 return;
             }
             finishSignIn(reply);
         });
}

void JellyfinClient::finishSignIn(QNetworkReply *reply)
{
    const QVariantMap r = parseJson(reply).toMap();
    const QString token = r.value("AccessToken").toString();
    if (reply->error() != QNetworkReply::NoError || token.isEmpty()) {
        qWarning("jellyfin: sign-in failed: %s", qPrintable(reply->errorString()));
        setState("password", QStringLiteral("Jellyfin didn't sign you in. Try again"));
        return;
    }
    m_token = token;
    m_userId = r.value("User").toMap().value("Id").toString();
    setBase(m_baseUrl); // images: the new token
    QSettings settings;
    settings.setValue("jellyfin/token", m_token);
    settings.setValue("jellyfin/userId", m_userId);
    qInfo("jellyfin: signed in");
    setState("ready");
}

void JellyfinClient::cancelSignIn()
{
    if (m_state != "address" && m_state != "linking" && m_state != "password"
        && !(m_state == "connecting" && m_token.isEmpty()))
        return;
    ++m_generation;
    m_poll.stop();
    m_linkCode.clear();
    setState("signedOut");
}

void JellyfinClient::changeServer()
{
    ++m_generation;
    m_poll.stop();
    m_linkCode.clear();
    m_token.clear();
    m_serverName.clear();
    setBase({});
    QSettings settings;
    for (const char *key : { "jellyfin/server", "jellyfin/serverName", "jellyfin/token", "jellyfin/userId" })
        settings.remove(key);
    setState("address");
    discover();
}

// the server is kept: signing in again needs no address
void JellyfinClient::signOut()
{
    if (!m_token.isEmpty())
        m_net->post(request("/Sessions/Logout"), QByteArray()); // ends the session on the server
    ++m_generation;
    m_poll.stop();
    m_token.clear();
    m_linkCode.clear();
    setBase(m_baseUrl);
    QSettings settings;
    settings.remove("jellyfin/token");
    settings.remove("jellyfin/userId");
    setState("signedOut");
}

void JellyfinClient::get(const QString &path, const QJSValue &callback, const std::function<void(const QVariant &)> &done)
{
    if (m_state != "ready") {
        fail(callback, QStringLiteral("Not connected"));
        return;
    }
    if (m_stopping && m_stopping->isRunning()) {
        connect(m_stopping, &QNetworkReply::finished, this, [=, this] { get(path, callback, done); });
        return;
    }
    QString p = path;
    p.replace("{user}", m_userId);
    QNetworkReply *reply = m_net->get(request(p));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("jellyfin: %s: %s", qPrintable(reply->url().path()), qPrintable(reply->errorString()));
            fail(callback, QStringLiteral("Your Jellyfin server didn't answer"));
            return;
        }
        done(parseJson(reply));
    });
}

void JellyfinClient::getItems(const QString &path, const QJSValue &callback)
{
    get(path, callback, [this, callback](const QVariant &json) { answer(callback, toItems(json)); });
}

void JellyfinClient::libraries(const QJSValue &callback)
{
    get("/UserViews?userId={user}", callback, [this, callback](const QVariant &json) {
        QVariantList out;
        for (const QVariant &v : json.toMap().value("Items").toList()) {
            const QVariantMap m = v.toMap();
            const QString kind = m.value("CollectionType").toString();
            if (kind == "movies" || kind == "tvshows")
                out.append(QVariantMap{ { "id", m.value("Id").toString() }, { "title", m.value("Name").toString() },
                                        { "type", kind == "movies" ? "movie" : "show" } });
        }
        answer(callback, out);
    });
}

// what's partly watched, then shows' next episodes
void JellyfinClient::continueWatching(const QJSValue &callback)
{
    get("/UserItems/Resume?userId={user}&MediaTypes=Video&Fields=Overview&Limit=30", callback,
        [this, callback](const QVariant &json) {
        const QVariantList resume = toItems(json);
        get("/Shows/NextUp?userId={user}&Fields=Overview&Limit=30", callback, [this, callback, resume](const QVariant &json) {
            QSet<QString> shows;
            for (const QVariant &v : resume)
                shows.insert(v.toMap().value("seriesId").toString());
            QVariantList out = resume;
            for (const QVariant &v : toItems(json)) {
                if (!shows.contains(v.toMap().value("seriesId").toString()))
                    out.append(v);
            }
            answer(callback, out);
        });
    });
}

void JellyfinClient::items(const QString &libraryId, const QJSValue &callback)
{
    getItems("/Items?userId={user}&ParentId=" + libraryId
             + "&IncludeItemTypes=Movie,Series&Recursive=true&SortBy=SortName&SortOrder=Ascending&Fields=Overview",
             callback);
}

void JellyfinClient::children(const QVariantMap &item, const QJSValue &callback)
{
    const QString id = item.value("id").toString();
    if (item.value("type") == "show")
        getItems("/Shows/" + id + "/Seasons?userId={user}&Fields=ChildCount", callback);
    else
        getItems("/Shows/" + item.value("seriesId").toString() + "/Episodes?userId={user}&seasonId=" + id
                 + "&Fields=Overview", callback);
}

void JellyfinClient::refresh(const QVariantMap &item, const QJSValue &callback)
{
    get("/Items/" + item.value("id").toString() + "?userId={user}", callback,
        [this, callback](const QVariant &json) { answer(callback, toItem(json.toMap())); });
}

void JellyfinClient::firstEpisode(const QVariantMap &show, const QJSValue &callback)
{
    get("/Shows/" + show.value("id").toString() + "/Episodes?userId={user}&Limit=1", callback,
        [this, callback](const QVariant &json) { answer(callback, toItems(json).value(0)); });
}

QString JellyfinClient::imageUrl(const QString &image, int width, int height) const
{
    const QStringList parts = image.split('/');
    if (parts.size() != 3 || m_baseUrl.isEmpty())
        return {};
    QUrlQuery q;
    q.addQueryItem("fillWidth", QString::number(width));
    q.addQueryItem("fillHeight", QString::number(height));
    q.addQueryItem("quality", "90");
    q.addQueryItem("tag", parts[2]);
    QUrl url(m_baseUrl + "/Items/" + parts[0] + "/Images/" + parts[1]);
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

QVariantMap JellyfinClient::playable(const QVariantMap &item) const
{
    const QString id = item.value("id").toString();
    if (id.isEmpty() || m_baseUrl.isEmpty())
        return {};
    const QString title = item.value("title").toString();
    return {
        // the file as it is: mpv plays anything Jellyfin would transcode
        { "url", m_baseUrl + "/Videos/" + id + "/stream?static=true" },
        { "label", title },
        { "title", title },
        { "kind", QStringLiteral("jellyfin") },
        { "library", QStringLiteral("jellyfin") },
        { "itemId", id },
        { "contentHash", "jellyfin:" + id },
        // mpv splits this option at commas: the token alone
        { "options", QVariantMap{ { "http-header-fields", "Authorization: MediaBrowser Token=" + quoted(m_token) } } },
        { "show", item.value("show").toString() },
    };
}

QNetworkReply *JellyfinClient::post(const QString &path, const QVariantMap &body)
{
    if (m_state != "ready")
        return nullptr;
    QNetworkReply *reply = postJson(QString(path).replace("{user}", m_userId), body);
    connect(reply, &QNetworkReply::finished, reply, [reply] {
        if (reply->error() != QNetworkReply::NoError)
            qWarning("jellyfin: %s: %s", qPrintable(reply->url().path()), qPrintable(reply->errorString()));
        reply->deleteLater();
    });
    return reply;
}

// a playback session: started, progress, stopped (stopping past 90% marks
// it watched)
void JellyfinClient::report(const QString &itemId, const QString &state, double seconds, double duration)
{
    Q_UNUSED(duration)
    if (itemId.isEmpty())
        return;
    QVariantMap body{ { "ItemId", itemId }, { "PositionTicks", qint64(seconds * kTicks) } };
    if (state == "stopped") {
        m_stopping = post("/Sessions/Playing/Stopped", body);
        m_playing.clear();
        return;
    }
    body.insert("IsPaused", state == "paused");
    body.insert("CanSeek", true);
    body.insert("PlayMethod", "DirectPlay");
    if (m_playing != itemId) {
        m_playing = itemId;
        post("/Sessions/Playing", body);
    } else {
        post("/Sessions/Playing/Progress", body);
    }
}

void JellyfinClient::scrobble(const QString &itemId)
{
    if (!itemId.isEmpty())
        post("/UserPlayedItems/" + itemId + "?userId={user}", {});
}
