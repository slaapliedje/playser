// SPDX-License-Identifier: GPL-3.0-or-later
#include "DiscFs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <udfread.h>

namespace {

class DirFs final : public DiscFs
{
public:
    explicit DirFs(const QString &root) : m_root(root) {}

    QString source() const override { return m_root.path(); }
    bool isDir(const QString &path) override { return QFileInfo(m_root.filePath(path)).isDir(); }
    QStringList files(const QString &dir) override
    {
        return QDir(m_root.filePath(dir)).entryList(QDir::Files);
    }
    qint64 size(const QString &path) override
    {
        const QFileInfo info(m_root.filePath(path));
        return info.isFile() ? info.size() : -1;
    }
    QByteArray read(const QString &path, qint64 maxSize) override
    {
        QFile f(m_root.filePath(path));
        return f.open(QIODevice::ReadOnly) && f.size() <= maxSize ? f.readAll() : QByteArray();
    }
    QString localPath(const QString &path) override
    {
        const QString p = m_root.filePath(path);
        return QFileInfo(p).isFile() ? p : QString();
    }

private:
    QDir m_root;
};

class UdfFs final : public DiscFs
{
public:
    UdfFs(const QString &device, udfread *udf) : m_device(device), m_udf(udf) {}
    ~UdfFs() override { udfread_close(m_udf); }

    QString source() const override { return m_device; }

    bool isDir(const QString &path) override
    {
        UDFDIR *dir = udfread_opendir(m_udf, abs(path).constData());
        if (dir)
            udfread_closedir(dir);
        return dir;
    }

    QStringList files(const QString &path) override
    {
        QStringList names;
        UDFDIR *dir = udfread_opendir(m_udf, abs(path).constData());
        if (!dir)
            return names;
        udfread_dirent entry;
        while (udfread_readdir(dir, &entry))
            if (entry.d_type == UDF_DT_REG)
                names.append(QString::fromUtf8(entry.d_name));
        udfread_closedir(dir);
        return names;
    }

    qint64 size(const QString &path) override
    {
        UDFFILE *f = udfread_file_open(m_udf, abs(path).constData());
        if (!f)
            return -1;
        const qint64 size = udfread_file_size(f);
        udfread_file_close(f);
        return size;
    }

    QByteArray read(const QString &path, qint64 maxSize) override
    {
        UDFFILE *f = udfread_file_open(m_udf, abs(path).constData());
        if (!f)
            return {};
        QByteArray data;
        const qint64 size = udfread_file_size(f);
        if (size >= 0 && size <= maxSize) {
            data.resize(size);
            qint64 got = 0;
            while (got < size) {
                const ssize_t n = udfread_file_read(f, data.data() + got, size_t(size - got));
                if (n <= 0)
                    break;
                got += n;
            }
            if (got != size)
                data.clear();
        }
        udfread_file_close(f);
        return data;
    }

    // named after the disc's volume and the file, so a disc's art is
    // copied once
    QString localPath(const QString &path) override
    {
        const QByteArray key = QByteArray(udfread_get_volume_id(m_udf)) + '/' + path.toUtf8()
                             + '/' + QByteArray::number(size(path));
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/disc-files";
        const QString file = dir + '/' + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex())
                           + '.' + QFileInfo(path).suffix().toLower();
        if (QFileInfo::exists(file))
            return file;
        const QByteArray data = read(path, 1 << 24);
        if (data.isEmpty() || !QDir().mkpath(dir))
            return {};
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
            out.remove();
            return {};
        }
        return file;
    }

private:
    static QByteArray abs(const QString &path) { return ('/' + path).toUtf8(); }

    QString m_device;
    udfread *m_udf;
};

} // namespace

std::unique_ptr<DiscFs> DiscFs::open(const QString &rootOrDevice)
{
    if (QFileInfo(rootOrDevice).isDir())
        return std::make_unique<DirFs>(rootOrDevice);
    udfread *udf = udfread_init();
    if (!udf)
        return nullptr;
    if (udfread_open(udf, QFile::encodeName(rootOrDevice).constData()) < 0) {
        udfread_close(udf);
        return nullptr;
    }
    return std::make_unique<UdfFs>(rootOrDevice, udf);
}
