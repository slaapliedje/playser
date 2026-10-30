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

QVariantList DiscIndex::dvdTitles(const QString &discRoot)
{
    const QDir dir(QDir(discRoot).filePath("VIDEO_TS"));
    const auto ifo = [&dir](const QString &name) {
        QFile f(dir.filePath(name));
        if (!f.exists())
            f.setFileName(dir.filePath(name.toLower()));
        return f.open(QIODevice::ReadOnly) && f.size() < (1 << 24) ? f.readAll() : QByteArray();
    };
    const auto u16 = [](const QByteArray &b, qsizetype at) { return at + 2 <= b.size() ? qFromBigEndian<quint16>(b.constData() + at) : 0; };
    const auto u32 = [](const QByteArray &b, qsizetype at) { return at + 4 <= b.size() ? qFromBigEndian<quint32>(b.constData() + at) : 0u; };
    const auto bcd = [](quint8 v) { return (v >> 4) * 10 + (v & 0x0f); };

    const QByteArray vmg = ifo("VIDEO_TS.IFO");
    if (!vmg.startsWith("DVDVIDEO-VMG"))
        return {};
    // TT_SRPT (sector at 0xC4): count, then 12-byte entries naming each
    // title's title set and its number within it
    const qsizetype srpt = qsizetype(u32(vmg, 0xC4)) * 2048;
    const int count = u16(vmg, srpt);
    QHash<int, QByteArray> vtsIfo;
    QVariantList titles;
    for (int i = 0; i < count; i++) {
        const qsizetype e = srpt + 8 + 12 * i;
        if (e + 12 > vmg.size())
            break;
        const int vts = quint8(vmg[e + 6]);
        const int ttn = quint8(vmg[e + 7]);
        if (!vtsIfo.contains(vts))
            vtsIfo.insert(vts, ifo(QStringLiteral("VTS_%1_0.IFO").arg(vts, 2, 10, QLatin1Char('0'))));
        const QByteArray &b = vtsIfo[vts];
        double seconds = 0;
        if (b.startsWith("DVDVIDEO-VTS")) {
            // VTS_PTT_SRPT (sector at 0xC8): per title, its chapters as
            // (PGC, program); a title plays each of its PGCs once
            const qsizetype ptt = qsizetype(u32(b, 0xC8)) * 2048;
            const qsizetype start = ptt + u32(b, ptt + 8 + 4 * (ttn - 1));
            const qsizetype end = ttn < u16(b, ptt) ? ptt + u32(b, ptt + 8 + 4 * ttn) : ptt + u32(b, ptt + 4) + 1;
            // VTS_PGCI (sector at 0xCC): 8-byte entries, offset at +4;
            // a PGC's playback time is BCD hh mm ss ff at +4
            const qsizetype pgci = qsizetype(u32(b, 0xCC)) * 2048;
            QSet<int> seen;
            for (qsizetype at = start; at + 4 <= end && at + 4 <= b.size(); at += 4) {
                const int pgcn = u16(b, at);
                if (pgcn < 1 || seen.contains(pgcn))
                    continue;
                seen.insert(pgcn);
                const qsizetype pgc = pgci + u32(b, pgci + 8 + 8 * (pgcn - 1) + 4);
                if (pgc + 8 > b.size())
                    continue;
                const quint8 *t = reinterpret_cast<const quint8 *>(b.constData() + pgc + 4);
                const double fps = (t[3] >> 6) == 1 ? 25.0 : 30000.0 / 1001.0;
                seconds += bcd(t[0]) * 3600 + bcd(t[1]) * 60 + bcd(t[2]) + bcd(t[3] & 0x3f) / fps;
            }
        }
        titles.append(QVariantMap{ { "number", i + 1 }, { "vts", vts }, { "seconds", qRound(seconds) } });
    }
    return titles;
}

void DiscIndex::resolveTitles(QVariantMap &disc, const QString &discRoot)
{
    QVariantList items = disc.value("items").toList();
    const bool dvd = QDir(discRoot).exists("VIDEO_TS") && !QDir(discRoot).exists("BDMV");

    // Blu-ray: one-clip playlists by clip
    QHash<QString, QString> playlistOfClip;
    // DVD: titles by title set; each title is given to one item at most
    QMultiHash<int, QVariantMap> titlesOfVts;
    QSet<int> taken;
    if (dvd) {
        for (const QVariant &v : dvdTitles(discRoot))
            titlesOfVts.insert(v.toMap().value("vts").toInt(), v.toMap());
    } else {
        for (const QVariant &v : playlists(discRoot)) {
            const QVariantMap pl = v.toMap();
            const QStringList clips = pl.value("clips").toStringList();
            if (clips.size() == 1 && !playlistOfClip.contains(clips.first()))
                playlistOfClip.insert(clips.first(), pl.value("name").toString());
        }
    }

    QVariantList episodes, extras;
    for (QVariant &v : items) {
        QVariantMap item = v.toMap();
        const QString source = item.value("source").toString().toLower();
        int order = -1; // disc order: playlist or title number
        if (dvd) {
            // the length MakeMKV measured; the closest within a few seconds
            const int seconds = item.value("seconds").toInt();
            const int tolerance = qMax(5, seconds / 50);
            int best = -1, bestDiff = tolerance + 1;
            for (const QVariantMap &t : titlesOfVts.values(source.toInt())) {
                const int n = t.value("number").toInt();
                const int diff = qAbs(t.value("seconds").toInt() - seconds);
                if (!taken.contains(n) && diff < bestDiff) {
                    best = n;
                    bestDiff = diff;
                }
            }
            if (best > 0) {
                taken.insert(best);
                order = best;
                item.insert("url", QStringLiteral("dvd://%1").arg(best - 1)); // mpv counts from 0
            }
        } else {
            QString playlist;
            if (source.endsWith(".mpls"))
                playlist = source;
            else if (source.endsWith(".m2ts"))
                playlist = playlistOfClip.value(source.chopped(5));
            if (!playlist.isEmpty()) {
                order = playlist.chopped(5).toInt();
                item.insert("url", QStringLiteral("bd://mpls/%1").arg(order)); // "01199.mpls" -> bd://mpls/1199
            }
        }
        item.insert("order", order);
        v = item;
        if (order < 0)
            continue;
        if (item.value("type") == "MainMovie" && !disc.contains("mainUrl"))
            disc.insert("mainUrl", item.value("url"));
        const QString type = item.value("type").toString();
        if (type == "Episode")
            episodes.append(item);
        else if (type != "MainMovie")
            extras.append(item);
    }
    const auto byOrder = [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("order").toInt() < b.toMap().value("order").toInt();
    };
    std::stable_sort(episodes.begin(), episodes.end(), byOrder);
    std::stable_sort(extras.begin(), extras.end(), byOrder);
    disc.insert("items", items);
    disc.insert("episodes", episodes);
    disc.insert("extras", extras);
}

QVariantMap DiscIndex::discMeta(const QString &discRoot)
{
    QVariantMap meta;
    // index.bdmv's version: "0300" is a 4K UHD disc, "0100"/"0200" Blu-ray
    QFile index(QDir(discRoot).filePath("BDMV/index.bdmv"));
    if (index.open(QIODevice::ReadOnly) && index.read(8) == "INDX0300")
        meta.insert("format", QStringLiteral("UHD"));

    const QDir dir(QDir(discRoot).filePath("BDMV/META/DL"));
    QStringList files = dir.entryList({ "bdmt_*.xml", "BDMT_*.XML" }, QDir::Files);
    if (files.isEmpty())
        return meta;
    const int english = files.indexOf(QRegularExpression("bdmt_eng\\.xml", QRegularExpression::CaseInsensitiveOption));
    QFile file(dir.filePath(files.value(qMax(0, english))));
    if (!file.open(QIODevice::ReadOnly))
        return meta;

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
