#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>

// Read access to a disc's files, either through a mounted directory or
// straight from the device (UDF, via libudfread) so the disc needn't be
// mounted. Paths are relative to the disc root ("BDMV/index.bdmv"). Not
// thread-safe: use one from a single thread.
class DiscFs
{
public:
    // A directory is read as a mounted disc; anything else is opened as a
    // UDF device or image. Null if it can't be read.
    static std::unique_ptr<DiscFs> open(const QString &rootOrDevice);
    virtual ~DiscFs() = default;

    // the device or directory this reads
    virtual QString source() const = 0;
    virtual bool isDir(const QString &path) = 0;
    // names of the files (not directories) in a directory
    virtual QStringList files(const QString &dir) = 0;
    // -1 if there's no such file
    virtual qint64 size(const QString &path) = 0;
    // the whole file, or nothing if it is missing or larger than maxSize
    virtual QByteArray read(const QString &path, qint64 maxSize = 1 << 24) = 0;
    // a local file with the same contents, for what needs a path (images):
    // the file itself when mounted, else a copy in the cache directory
    virtual QString localPath(const QString &path) = 0;
};
