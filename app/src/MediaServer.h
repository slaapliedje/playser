// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJSValue>
#include <QObject>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class QNetworkAccessManager;
class QQmlEngine;

// A personal media server's library (Plex, Jellyfin), for LibraryScreen.
// Each kind signs in its own way; browsing and playing are the same.
//
// Items are maps of the same shape whatever the server:
//   id, type ("movie", "show", "season", "episode"), title, year, summary,
//   contentRating, duration and position (seconds; position is where to
//   resume, 0 for none), watched, show (a season's or episode's show),
//   season and number (an episode's), childCount (a season's episodes),
//   thumb, showThumb, art (images, for imageUrl())
// and whatever else the server needs to find or play it.
//
// Callbacks get the result, or (null, message) when the server didn't
// answer.
class MediaServer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PlexClient or JellyfinClient")

    // "Plex", "Jellyfin"
    Q_PROPERTY(QString service READ service CONSTANT)
    // "signedOut", "linking" (showing linkCode), "connecting", "ready",
    // "error" (see message), or a server's own sign-in steps
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString linkCode READ linkCode NOTIFY stateChanged)
    Q_PROPERTY(QString serverName READ serverName NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    // signed in before: start() connects without asking
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)

public:
    explicit MediaServer(QObject *parent = nullptr);

    // Lets QML images load from the servers: the engine's requests to one
    // get its token as a header (kept out of image URLs, which get logged).
    // Call before loading any QML.
    static void installImageAuth(QQmlEngine *engine);

    virtual QString service() const = 0;
    QString state() const { return m_state; }
    QString linkCode() const { return m_linkCode; }
    QString serverName() const { return m_serverName; }
    QString message() const { return m_message; }
    virtual bool signedIn() const = 0;

    // signed in: connect; else start signing in
    Q_INVOKABLE virtual void start() = 0;
    Q_INVOKABLE virtual void cancelSignIn() = 0;
    Q_INVOKABLE virtual void signOut() = 0;

    // [{ id, title, type: "movie"|"show" }]
    Q_INVOKABLE virtual void libraries(const QJSValue &callback) = 0;
    // what's partly watched, and shows' next episodes
    Q_INVOKABLE virtual void continueWatching(const QJSValue &callback) = 0;
    // a library's films or shows
    Q_INVOKABLE virtual void items(const QString &libraryId, const QJSValue &callback) = 0;
    // a show's seasons, a season's episodes
    Q_INVOKABLE virtual void children(const QVariantMap &item, const QJSValue &callback) = 0;
    // the item again (its progress after playing)
    Q_INVOKABLE virtual void refresh(const QVariantMap &item, const QJSValue &callback) = 0;
    // a show's first episode (its picture stands in for a missing poster)
    Q_INVOKABLE virtual void firstEpisode(const QVariantMap &show, const QJSValue &callback) = 0;
    // an item's thumb, showThumb or art, scaled by the server
    Q_INVOKABLE virtual QString imageUrl(const QString &image, int width, int height) const = 0;

    // A movie or episode as a drive for the player: { url, label, title,
    // kind and library (service().toLower()), itemId, contentHash, options
    // (mpv's: the token header), show }
    Q_INVOKABLE virtual QVariantMap playable(const QVariantMap &item) const = 0;
    // Playback progress, for the server's "continue watching": state
    // "playing", "paused" or "stopped"
    Q_INVOKABLE virtual void report(const QString &itemId, const QString &state, double seconds, double duration) = 0;
    // marks it watched
    Q_INVOKABLE virtual void scrobble(const QString &itemId) = 0;

signals:
    void stateChanged();

protected:
    void setState(const QString &state, const QString &message = {});
    // the token header QML images from base get (empty base: none)
    void setImageAuth(const QString &base, const QByteArray &header, const QByteArray &value);
    // calls a QML callback with the result, or with (null, message)
    void answer(const QJSValue &callback, const QVariant &result);
    void fail(const QJSValue &callback, const QString &message);

    QNetworkAccessManager *m_net;
    QString m_state = QStringLiteral("signedOut");
    QString m_message;
    QString m_linkCode;
    QString m_serverName;
};
