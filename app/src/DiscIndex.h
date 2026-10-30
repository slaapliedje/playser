#pragma once

#include <QString>
#include <QVariantMap>

class DiscFs;

// Offline disc catalogue built from TheDiscDB (tools/build-discdb-index.py),
// embedded as a resource. Discs are looked up by TheDiscDB's content hash,
// which needs only the sizes of the disc's stream files: no decryption.
namespace DiscIndex {

// Content hash of a disc (DiscFs: mounted or read from the device):
// MD5 over the sizes of BDMV/STREAM/*.m2ts (DVD: VIDEO_TS/*.VOB|IFO|BUP),
// sorted by file name, each as an 8-byte little-endian integer, uppercase
// hex. Empty if the layout isn't recognised. Reads the disc: call it off
// the GUI thread.
QString contentHash(DiscFs &fs);

// Blu-ray playlists of a disc (BDMV/PLAYLIST/*.mpls, which are not
// encrypted): [{ name: "00061.mpls", seconds, clips: ["00011", ...] }],
// sorted by name. Reads the disc: call it off the GUI thread.
QVariantList playlists(DiscFs &fs);

// DVD titles of a disc, from the unencrypted IFO files:
// [{ number (1-based, as the disc counts them), vts, seconds }]. Reads the
// disc: call it off the GUI thread.
QVariantList dvdTitles(DiscFs &fs);

// Gives catalogue items the mpv URL that plays them (item.url) and adds
// `mainUrl` (the first MainMovie's), `episodes` (the Episode items) and
// `extras` (the rest: featurettes, trailers, deleted scenes...), both in
// disc order and only those that can be played. Blu-ray items name a
// playlist, or a clip ("00011.m2ts") that is played through its one-clip
// playlist. DVD items name a title set (VTS), which can hold several
// titles: the title is the one in that set whose length matches. Reads the
// disc: call it off the GUI thread.
void resolveTitles(QVariantMap &disc, DiscFs &fs);

// The catalogue entry for a hash, or an empty map:
// { title, year, kind, release, format,
//   items: [{ source, seconds, type, title, season, episode }] }
// Thread-safe; the index is loaded on first use.
QVariantMap lookup(const QString &contentHash);

// What a Blu-ray says about itself (BDMV/META/DL/bdmt_<lang>.xml, the
// "disc library" metadata, English preferred): { title, art, format } where
// art is a local path to its largest thumbnail (DiscFs::localPath) and
// format is "UHD" for a 4K disc (from BDMV/index.bdmv); any may be missing,
// and DVDs have none. Reads the disc: call it off the GUI thread.
QVariantMap discMeta(DiscFs &fs);

} // namespace DiscIndex
