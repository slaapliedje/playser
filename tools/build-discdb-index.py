#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the app's offline disc index from TheDiscDB data (MIT licence).

    git clone --depth 1 --filter=blob:none --sparse \\
        https://github.com/TheDiscDb/data.git build/thediscdb
    git -C build/thediscdb sparse-checkout set --no-cone \\
        '/data/**/disc*.json' '/data/**/metadata.json' '/data/**/release.json' '/LICENSE'
    tools/build-discdb-index.py build/thediscdb app/data/discdb-index.json.gz

Output: gzipped JSON {"source": ..., "discs": {CONTENTHASH: disc}}, where
disc = {"title", "year", "kind" (movie|series), "release", "format",
"items": [[source, seconds, type, title, season, episode], ...]}.
`source` is the disc's own name: "01199.mpls", "00007.m2ts", or for DVDs
MakeMKV's title-set number ("08"). Only titles TheDiscDB has identified
(MainMovie, Episode, Extra, ...) are kept.

The content hash (see docs/PLAN.md) is computed by the app from the disc's
stream file sizes, so no decryption is needed to look a disc up.
"""
import argparse
import gzip
import json
import pathlib
import subprocess


def seconds(duration):
    try:
        h, m, s = (int(x) for x in duration.split(":"))
        return h * 3600 + m * 60 + s
    except (ValueError, AttributeError):
        return 0


def load(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", type=pathlib.Path, help="checkout of TheDiscDb/data")
    ap.add_argument("out", type=pathlib.Path)
    opt = ap.parse_args()

    discs = {}
    skipped = 0
    for disc_path in sorted(opt.data.glob("data/*/*/*/disc*.json")):
        disc = load(disc_path)
        content_hash = (disc.get("ContentHash") or "").upper()
        if not content_hash:
            skipped += 1
            continue
        release = load(disc_path.parent / "release.json")
        meta = load(disc_path.parent.parent / "metadata.json")
        items = []
        for t in disc.get("Titles") or []:
            item = t.get("Item")
            if not item or not t.get("SourceFile"):
                continue
            items.append([t["SourceFile"], seconds(t.get("Duration")), item.get("Type") or "Other",
                          item.get("Title") or "", item.get("Season") or "", item.get("Episode") or ""])
        if not items:
            skipped += 1
            continue
        entry = {
            "title": meta.get("Title") or disc_path.parent.parent.name,
            "year": meta.get("Year") or 0,
            "kind": (meta.get("Type") or disc_path.parts[-4]).lower(),
            "release": release.get("Title") or "",
            "format": disc.get("Format") or "",
            "items": items,
        }
        # the same pressing can appear in several releases; keep the first
        discs.setdefault(content_hash, entry)

    try:
        rev = subprocess.run(["git", "-C", str(opt.data), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        rev = ""
    doc = {"source": f"TheDiscDB data (MIT) https://github.com/TheDiscDb/data {rev}".strip(),
           "discs": discs}
    opt.out.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(opt.out, "wt", encoding="utf-8", compresslevel=9) as f:
        json.dump(doc, f, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    print(f"{opt.out}: {len(discs)} discs, {skipped} skipped, {opt.out.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
