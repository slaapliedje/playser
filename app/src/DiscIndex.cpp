#include "DiscIndex.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QtEndian>
#include <zlib.h>

namespace {

QByteArray gunzip(const QByteArray &gz)
{
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) // 16: gzip header
        return {};
    QByteArray out;
    char buf[1 << 16];
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(gz.constData()));
    zs.avail_in = uInt(gz.size());
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof buf;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            out.clear();
            break;
        }
        out.append(buf, int(sizeof buf - zs.avail_out));
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

const QJsonObject &discs()
{
    static QMutex mutex;
    static QJsonObject index;
    static bool loaded = false;
    QMutexLocker lock(&mutex);
    if (!loaded) {
        loaded = true;
        QFile f(QStringLiteral(":/discdb-index.json.gz"));
        if (f.open(QIODevice::ReadOnly))
            index = QJsonDocument::fromJson(gunzip(f.readAll())).object().value("discs").toObject();
        if (index.isEmpty())
            qWarning("disc index: not available");
    }
    return index;
}

} // namespace

QString DiscIndex::contentHash(const QString &discRoot)
{
    QDir root(discRoot);
    QDir dir;
    QStringList names;
    // UDF names are case-sensitive in principle; discs use upper case
    if (root.exists("BDMV/STREAM")) {
        dir = QDir(root.filePath("BDMV/STREAM"));
        names = dir.entryList({ "*.m2ts", "*.M2TS" }, QDir::Files);
    } else if (root.exists("VIDEO_TS")) {
        dir = QDir(root.filePath("VIDEO_TS"));
        names = dir.entryList({ "*.VOB", "*.IFO", "*.BUP", "*.vob", "*.ifo", "*.bup" }, QDir::Files);
    }
    if (names.isEmpty())
        return {};
    // ordinal (byte-wise) order, as TheDiscDB's recipe specifies
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.toUtf8() < b.toUtf8(); });
    QCryptographicHash md5(QCryptographicHash::Md5);
    for (const QString &name : names) {
        const qint64 size = QFileInfo(dir.filePath(name)).size();
        char le[8];
        qToLittleEndian<qint64>(size, le);
        md5.addData(QByteArrayView(le, 8));
    }
    return QString::fromLatin1(md5.result().toHex().toUpper());
}

QVariantMap DiscIndex::lookup(const QString &contentHash)
{
    const QJsonObject disc = discs().value(contentHash).toObject();
    if (disc.isEmpty())
        return {};
    QVariantMap m = disc.toVariantMap();
    QVariantList items;
    for (const QJsonValue &v : disc.value("items").toArray()) {
        const QJsonArray a = v.toArray();
        const QVariantMap item{
            { "source", a.at(0).toString() }, { "seconds", a.at(1).toInt() },
            { "type", a.at(2).toString() },   { "title", a.at(3).toString() },
            { "season", a.at(4).toString() }, { "episode", a.at(5).toString() },
        };
        if (item.value("type") == "MainMovie" && !m.contains("main"))
            m.insert("main", item.value("source"));
        items.append(item);
    }
    m.insert("items", items);
    return m;
}
