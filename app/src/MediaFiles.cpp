// SPDX-License-Identifier: GPL-3.0-or-later
#include "MediaFiles.h"
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThreadPool>
#include <algorithm>
#include "DiscFs.h"
#include "DiscIndex.h"

namespace {

// what mpv plays that a person would keep as a film or episode
const QStringList kVideoSuffixes = {
    "mkv", "mp4", "m4v", "avi", "mov", "webm", "ts", "m2ts", "mts", "mpg", "mpeg",
    "vob", "wmv", "flv", "ogv", "3gp",
};

QString discType(const QDir &dir)
{
    if (dir.exists("VIDEO_TS") || dir.exists("video_ts"))
        return QStringLiteral("dvd");
    if (dir.exists("BDMV"))
        return QStringLiteral("bluray");
    return {};
}

} // namespace

QVariantList MediaFiles::places() const
{
    QVariantList out;
    auto add = [&out](const QString &name, const QString &path) {
        if (!path.isEmpty() && QFileInfo(path).isDir())
            out.append(QVariantMap{ { "name", name }, { "path", path } });
    };
    const QString home = QDir::homePath();
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (videos != home)
        add(QStringLiteral("Videos"), videos);
    add(QStringLiteral("Home"), home);
    // SteamOS mounts SD cards and USB drives at /run/media/<user>/<label>
    // (older releases at /run/media/<label>); desktops use /media too
    const QString user = qEnvironmentVariable("USER");
    for (const QString &root : { QStringLiteral("/run/media"), QStringLiteral("/media") }) {
        const QFileInfoList entries = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (e.fileName() == user) {
                const QFileInfoList mounts = QDir(e.filePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QFileInfo &m : mounts)
                    add(m.fileName(), m.filePath());
            } else if (QStorageInfo(e.filePath()).rootPath() == e.filePath()) {
                add(e.fileName(), e.filePath()); // not another user's folder
            }
        }
    }
    return out;
}

QVariantList MediaFiles::list(const QString &path) const
{
    QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);
    QVariantList folders, files;
    for (const QFileInfo &e : entries) {
        QVariantMap m{ { "name", e.fileName() }, { "path", e.filePath() } };
        if (e.isDir()) {
            const QString disc = discType(QDir(e.filePath()));
            m.insert("type", disc.isEmpty() ? QStringLiteral("folder") : disc);
            folders.append(m);
            continue;
        }
        const QString suffix = e.suffix().toLower();
        if (suffix == "iso")
            m.insert("type", QStringLiteral("image"));
        else if (kVideoSuffixes.contains(suffix))
            m.insert("type", QStringLiteral("video"));
        else
            continue;
        m.insert("size", e.size());
        files.append(m);
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    auto byName = [&collator](const QVariant &a, const QVariant &b) {
        return collator.compare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    return folders + files;
}

QVariantMap MediaFiles::video(const QString &path) const
{
    const QFileInfo info(path);
    return {
        { "url", info.filePath() },
        { "label", info.completeBaseName() },
        { "kind", QStringLiteral("file") },
        { "contentHash", "file:" + info.filePath() },
        { "file", true },
    };
}

void MediaFiles::openDisc(const QString &path)
{
    // an image or a disc's worth of files: keep the reading off the GUI thread
    QPointer<MediaFiles> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        const QFileInfo info(path);
        const std::unique_ptr<DiscFs> fs = DiscFs::open(path);
        const QString kind = !fs ? QString()
            : fs->isDir("BDMV") ? QStringLiteral("bluray")
            : fs->isDir("VIDEO_TS") ? QStringLiteral("dvd") : QString();
        QVariantMap drive;
        if (!kind.isEmpty()) {
            QString hash;
            bool known = false;
            const QVariantMap disc = DiscIndex::identify(*fs, &hash, &known);
            const QString main = disc.value("mainUrl").toString();
            const bool bluray = kind == "bluray";
            drive = {
                { "device", info.filePath() },
                { "label", info.isDir() ? info.fileName() : info.completeBaseName() },
                { "kind", kind },
                { "hasMedia", true },
                { "disc", disc },
                // the same disc as a folder or image resumes where the disc left off
                { "contentHash", hash.isEmpty() ? "file:" + info.filePath() : hash },
                { "url", main.startsWith(bluray ? "bd://" : "dvd://") ? main
                         : QString(bluray ? "bd://" : "dvd://") },
                { "options", QVariantMap{ { bluray ? "bluray-device" : "dvd-device", info.filePath() } } },
                { "file", true },
            };
            qInfo("files %s: %s %s", qPrintable(path), qPrintable(hash),
                  known ? qPrintable(disc.value("title").toString()) : "(not in index)");
        }
        QMetaObject::invokeMethod(self, [self, path, drive] {
            if (!self)
                return;
            if (drive.isEmpty())
                emit self->openFailed(path);
            else
                emit self->opened(path, drive);
        });
    });
}
