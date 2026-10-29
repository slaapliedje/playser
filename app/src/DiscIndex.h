#pragma once

#include <QString>
#include <QVariantMap>

// Offline disc catalogue built from TheDiscDB (tools/build-discdb-index.py),
// embedded as a resource. Discs are looked up by TheDiscDB's content hash,
// which needs only the sizes of the disc's stream files: no decryption.
namespace DiscIndex {

// Content hash of a mounted disc (directory containing BDMV or VIDEO_TS):
// MD5 over the sizes of BDMV/STREAM/*.m2ts (DVD: VIDEO_TS/*.VOB|IFO|BUP),
// sorted by file name, each as an 8-byte little-endian integer, uppercase
// hex. Empty if the layout isn't recognised. Reads the disc: call it off
// the GUI thread.
QString contentHash(const QString &discRoot);

// Blu-ray playlists of a mounted disc (BDMV/PLAYLIST/*.mpls, which are not
// encrypted): [{ name: "00061.mpls", seconds, clips: ["00011", ...] }],
// sorted by name. Reads the disc: call it off the GUI thread.
QVariantList playlists(const QString &discRoot);

// Fills in item.playlist for catalogue items that name a clip ("00011.m2ts")
// with the disc's one-clip playlist for it, since mpv selects playlists, and
// adds `episodes`: the Episode items in playlist (disc) order.
void resolvePlaylists(QVariantMap &disc, const QVariantList &playlists);

// The catalogue entry for a hash, or an empty map:
// { title, year, kind, release, format,
//   main: "01199.mpls" (first MainMovie, if any),
//   items: [{ source, seconds, type, title, season, episode }] }
// Thread-safe; the index is loaded on first use.
QVariantMap lookup(const QString &contentHash);

} // namespace DiscIndex
