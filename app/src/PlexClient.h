// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJSValue>
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class QNetworkAccessManager;
class QQmlEngine;
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
class PlexClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // "signedOut", "linking" (showing linkCode), "connecting", "ready",
    // "error" (see message)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString linkCode READ linkCode NOTIFY stateChanged)
    Q_PROPERTY(QString serverName READ serverName NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    // has an account token (start() then connects without a code)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)

public:
    explicit PlexClient(QObject *parent = nullptr);

    // Lets QML images load from the server: the engine's requests to it
    // get the token as a header (kept out of image URLs, which get logged).
    // Call before loading any QML.
    static void installImageAuth(QQmlEngine *engine);

    QString state() const { return m_state; }
    QString linkCode() const { return m_linkCode; }
    QString serverName() const { return m_serverName; }
    QString message() const { return m_message; }
    bool signedIn() const { return !m_userToken.isEmpty(); }

    // signed in: find the server; else ask plex.tv for a code to enter
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void signOut();

    // GET a server path ("/library/sections"); callback(container) with
    // the reply's MediaContainer, or callback(null, message)
    Q_INVOKABLE void get(const QString &path, const QJSValue &callback);
    // a poster or thumbnail (thumb: the item's thumb path) scaled by the
    // server; installImageAuth() adds the token
    Q_INVOKABLE QString imageUrl(const QString &thumb, int width, int height) const;
    // A movie or episode (its Metadata) as a drive for the player:
    // { url, label, kind: "plex", options (mpv: the token header),
    //   ratingKey, plex: true }
    Q_INVOKABLE QVariantMap playable(const QVariantMap &item) const;
    // Playback progress for Plex's "continue watching": state "playing",
    // "paused" or "stopped"
    Q_INVOKABLE void report(const QString &ratingKey, const QString &state, double seconds, double duration);
    // marks it watched
    Q_INVOKABLE void scrobble(const QString &ratingKey);

signals:
    void stateChanged();

private:
    void setState(const QString &state, const QString &message = {});
    QNetworkRequest request(const QUrl &url, const QString &token) const;
    void pollPin();
    void findServer();
    void tryConnections(const QVariantList &candidates, const QString &name, const QString &token);
    void serverGet(const QString &path, const QUrlQuery &query);
    void setServer(const QString &baseUrl, const QString &token);

    QNetworkAccessManager *m_net;
    QString m_state = QStringLiteral("signedOut");
    QString m_message;
    QString m_clientId;
    QString m_userToken;
    // the chosen server: its address and the token for it
    QString m_baseUrl;
    QString m_serverToken;
    QString m_serverName;
    // sign-in
    QString m_linkCode;
    qint64 m_pinId = 0;
    QTimer m_pinPoll;
    int m_pinPolls = 0;
    quint64 m_generation = 0; // drops replies from an abandoned sign-in or search
};
