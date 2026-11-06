// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QTimer>
#include <functional>
#include "MediaServer.h"

class QNetworkReply;
class QNetworkRequest;
class QUdpSocket;
class QUrl;

// A Jellyfin server. There's no account service to find it through, so it
// is found on the local network (Jellyfin answers broadcasts) or its
// address is typed in. Signing in is by Quick Connect, a code approved in
// the Jellyfin app on a phone or computer that's already signed in, or by
// name and password.
//
// The server and token are kept in the settings (group "jellyfin"); the
// token is never logged and goes in a header, never a URL.
//
// Sign-in states beyond MediaServer's: "address" (choose a found server or
// type its address: useServer()), "password" (signIn()). "linking" is Quick
// Connect, its code in linkCode.
class JellyfinClient : public MediaServer
{
    Q_OBJECT
    QML_ELEMENT

    // servers that answered on the local network: [{ name, address }]
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    // the server's address (set once one has answered)
    Q_PROPERTY(QString address READ address NOTIFY stateChanged)
    // it allows Quick Connect (else only a password signs in)
    Q_PROPERTY(bool quickConnect READ quickConnect NOTIFY stateChanged)
    // the server's users who show on its sign-in page: [name]
    Q_PROPERTY(QStringList users READ users NOTIFY stateChanged)

public:
    explicit JellyfinClient(QObject *parent = nullptr);

    QString service() const override { return QStringLiteral("Jellyfin"); }
    bool signedIn() const override { return !m_token.isEmpty(); }
    QVariantList servers() const { return m_servers; }
    QString address() const { return m_baseUrl; }
    bool quickConnect() const { return m_quickConnect; }
    QStringList users() const { return m_users; }

    void start() override;
    void cancelSignIn() override;
    void signOut() override;

    // the server at address ("192.168.1.5", "nas:8096", "https://…")
    Q_INVOKABLE void useServer(const QString &address);
    // forget the server: back to choosing one
    Q_INVOKABLE void changeServer();
    // look for servers on the local network again
    Q_INVOKABLE void discover();
    // from Quick Connect to name and password, and back
    Q_INVOKABLE void usePassword();
    Q_INVOKABLE void useQuickConnect();
    Q_INVOKABLE void signIn(const QString &user, const QString &password);

    void libraries(const QJSValue &callback) override;
    void continueWatching(const QJSValue &callback) override;
    void items(const QString &libraryId, const QJSValue &callback) override;
    void children(const QVariantMap &item, const QJSValue &callback) override;
    void refresh(const QVariantMap &item, const QJSValue &callback) override;
    void firstEpisode(const QVariantMap &show, const QJSValue &callback) override;
    QString imageUrl(const QString &image, int width, int height) const override;
    QVariantMap playable(const QVariantMap &item) const override;
    void report(const QString &itemId, const QString &state, double seconds, double duration) override;
    void scrobble(const QString &itemId) override;

signals:
    void serversChanged();

private:
    using Done = std::function<void(QNetworkReply *)>;
    QNetworkRequest request(const QString &path, bool withToken = true) const;
    // done(reply) when it finishes, unless abandoned (sign-in moved on)
    void send(QNetworkReply *reply, const Done &done);
    QNetworkReply *postJson(const QString &path, const QVariantMap &body, bool withToken = true);
    void tryAddresses(QStringList candidates);
    void beginSignIn();
    void initiateQuickConnect();
    void pollQuickConnect();
    void finishSignIn(QNetworkReply *reply);
    void connectSaved();
    void setBase(const QString &base);
    // GET a server path (with the user's id where it says {user}); done(json)
    // or fails the callback
    void get(const QString &path, const QJSValue &callback, const std::function<void(const QVariant &)> &done);
    void getItems(const QString &path, const QJSValue &callback);
    QNetworkReply *post(const QString &path, const QVariantMap &body);

    QString m_deviceId;
    QString m_baseUrl;
    QString m_token;
    QString m_userId;
    bool m_quickConnect = false;
    QStringList m_users;
    QVariantList m_servers;
    QUdpSocket *m_discovery = nullptr;
    // Quick Connect
    QString m_secret;
    QTimer m_poll;
    int m_polls = 0;
    // the item a playback session was started for
    QString m_playing;
    // a stop being reported: reads wait for it, so they see the new progress
    QPointer<QNetworkReply> m_stopping;
    quint64 m_generation = 0; // drops replies from an abandoned sign-in
};
