// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

// The user's video files, for the file browser. Lists the places to start
// from and the videos, disc folders and disc images in a folder, and
// opens a disc folder or image the way DiscMonitor does a disc.
//
// An entry of list(): { name, path, type, size } where type is "folder",
// "video", "dvd" or "bluray" (a folder holding VIDEO_TS or BDMV) or
// "image" (.iso); size is in bytes, for files.
class MediaFiles : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    using QObject::QObject;

    // [{ name, path }]: Videos, Home, and removable drives and SD cards
    // (/run/media), those that exist
    Q_INVOKABLE QVariantList places() const;
    // the folders, videos, disc folders and images in a folder, folders
    // first, each by name as a person would sort them ("2" before "10")
    Q_INVOKABLE QVariantList list(const QString &path) const;
    // A video as a drive for the player: { url, label, kind: "file",
    // contentHash (for resuming), file: true }
    Q_INVOKABLE QVariantMap video(const QString &path) const;
    // Reads a disc folder or image in the background, then opened() with it
    // as DiscMonitor gives a drive (device: the path, kind, url, options,
    // disc, contentHash, file: true), or openFailed() if it isn't a DVD or
    // Blu-ray.
    Q_INVOKABLE void openDisc(const QString &path);

signals:
    void opened(const QString &path, const QVariantMap &drive);
    void openFailed(const QString &path);
};
