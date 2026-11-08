// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QTimer>
#include <functional>
#include "MediaServer.h"

class QNetworkReply;
class QNetworkRequest;
class QUrl;
class QUrlQuery;

// A Plex Media Server, signed in to with a code entered at plex.tv/link
// (the PIN flow, made for TVs). plex.tv lists the account's servers and
// their addresses, so the server is found on any subnet; the first address
// that answers is used, local ones before remote and relayed ones.
//
// The account token is kept in the settings (group "plex") and never
// logged; playback sends it as a header, not in the URL.
class PlexClient : public MediaServer
{
    Q_OBJECT
    QML_ELEMENT

    // the servers this account can use, its own and others' shared with it:
    // [{ id, name, owned, owner (who shares it), online (as plex.tv last
    // heard), current }], online ones first
    Q_PROPERTY(QVariantList servers READ servers NOTIFY stateChanged)

public:
    explicit PlexClient(QObject *parent = nullptr);

    QString service() const override { return QStringLiteral("Plex"); }
    bool signedIn() const override { return !m_userToken.isEmpty(); }
    QVariantList servers() const;

    void start() override;
    void cancelSignIn() override;
    void signOut() override;
    // switch to another of servers (kept once it answers)
    Q_INVOKABLE void useServer(const QString &id);

    void libraries(const QJSValue &callback) override;
    void continueWatching(const QJSValue &callback) override;
    void items(const QString &libraryId, const QJSValue &callback) override;
    void children(const QVariantMap &item, const QJSValue &callback) override;
    void refresh(const QVariantMap &item, const QJSValue &callback) override;
    void search(const QString &query, const QJSValue &callback) override;
    void firstEpisode(const QVariantMap &show, const QJSValue &callback) override;
    QString imageUrl(const QString &image, int width, int height) const override;
    QVariantMap playable(const QVariantMap &item) const override;
    void report(const QString &itemId, const QString &state, double seconds, double duration) override;
    void scrobble(const QString &itemId) override;

private:
    QNetworkRequest request(const QUrl &url, const QString &token) const;
    void pollPin();
    void findServer();
    void tryConnections(const QVariantList &candidates, const QString &id, const QString &name, const QString &token);
    // GET a server path; done(MediaContainer) or fails the callback
    void get(const QString &path, const QJSValue &callback, const std::function<void(const QVariantMap &)> &done);
    // a container's Metadata as items
    void getItems(const QString &path, const QJSValue &callback);
    void serverGet(const QString &path, const QUrlQuery &query);
    void setServer(const QString &baseUrl, const QString &token);

    QString m_clientId;
    QString m_userToken;
    // the chosen server: its address and the token for it
    QString m_baseUrl;
    QString m_serverToken;
    QString m_serverId;
    // plex.tv's list of servers (resources that provide "server")
    QVariantList m_resources;
    // a server asked for by useServer(), until it answers or fails
    QString m_wanted;
    // sign-in
    qint64 m_pinId = 0;
    QTimer m_pinPoll;
    int m_pinPolls = 0;
    quint64 m_generation = 0; // drops replies from an abandoned sign-in
};
