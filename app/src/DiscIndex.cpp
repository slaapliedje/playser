#include "DiscIndex.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QtEndian>
#include <QVariantList>
#include <QXmlStreamReader>
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

QVariantList DiscIndex::playlists(const QString &discRoot)
{
    QVariantList result;
    const QDir dir(QDir(discRoot).filePath("BDMV/PLAYLIST"));
    QStringList names = dir.entryList({ "*.mpls", "*.MPLS" }, QDir::Files, QDir::Name);
    for (const QString &name : names) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly) || f.size() > (1 << 20))
            continue;
        const QByteArray b = f.readAll();
        const auto u16 = [&b](qsizetype at) { return at + 2 <= b.size() ? qFromBigEndian<quint16>(b.constData() + at) : 0; };
        const auto u32 = [&b](qsizetype at) { return at + 4 <= b.size() ? qFromBigEndian<quint32>(b.constData() + at) : 0u; };
        if (!b.startsWith("MPLS"))
            continue;
        // PlayList(): length(4) reserved(2) items(2) subpaths(2), then PlayItems
        qsizetype at = u32(8);
        const int items = u16(at + 6);
        at += 10;
        QStringList clips;
        double seconds = 0;
        for (int i = 0; i < items && at + 22 <= b.size(); i++) {
            const int length = u16(at);
            clips.append(QString::fromLatin1(b.mid(at + 2, 5)));
            // IN_time/OUT_time in 45 kHz ticks
            seconds += (double(u32(at + 18)) - double(u32(at + 14))) / 45000.0;
            at += 2 + length;
        }
        result.append(QVariantMap{ { "name", name.toLower() }, { "seconds", qRound(seconds) }, { "clips", clips } });
    }
    return result;
}

void DiscIndex::resolvePlaylists(QVariantMap &disc, const QVariantList &playlists)
{
    QHash<QString, QString> playlistOfClip; // one-clip playlists only
    for (const QVariant &v : playlists) {
        const QVariantMap pl = v.toMap();
        const QStringList clips = pl.value("clips").toStringList();
        if (clips.size() == 1 && !playlistOfClip.contains(clips.first()))
            playlistOfClip.insert(clips.first(), pl.value("name").toString());
    }
    QVariantList items = disc.value("items").toList();
    QVariantList episodes;
    for (QVariant &v : items) {
        QVariantMap item = v.toMap();
        const QString source = item.value("source").toString().toLower();
        if (source.endsWith(".mpls"))
            item.insert("playlist", source);
        else if (source.endsWith(".m2ts") && playlistOfClip.contains(source.chopped(5)))
            item.insert("playlist", playlistOfClip.value(source.chopped(5)));
        v = item;
        if (item.value("type") == "Episode" && item.contains("playlist"))
            episodes.append(item);
    }
    std::sort(episodes.begin(), episodes.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("playlist").toString() < b.toMap().value("playlist").toString();
    });
    disc.insert("items", items);
    disc.insert("episodes", episodes);
}

QVariantMap DiscIndex::discMeta(const QString &discRoot)
{
    const QDir dir(QDir(discRoot).filePath("BDMV/META/DL"));
    QStringList files = dir.entryList({ "bdmt_*.xml", "BDMT_*.XML" }, QDir::Files);
    if (files.isEmpty())
        return {};
    const int english = files.indexOf(QRegularExpression("bdmt_eng\\.xml", QRegularExpression::CaseInsensitiveOption));
    QFile file(dir.filePath(files.value(qMax(0, english))));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QVariantMap meta;
    qint64 artSize = 0;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() == u"name" && !meta.contains("title")) {
            const QString title = xml.readElementText().simplified();
            if (!title.isEmpty())
                meta.insert("title", title);
        } else if (xml.name() == u"thumbnail") {
            const QFileInfo art(dir.filePath(xml.attributes().value("href").toString()));
            if (art.isFile() && art.size() > artSize) {
                artSize = art.size();
                meta.insert("art", art.filePath());
            }
        }
    }
    return meta;
}
