#!/usr/bin/env python3
"""Convert an RGBA PNG into premultiplied BGRA for mpv overlay-add.

mpv (vo_gpu_next) treats BGRA overlays as being in the *video's* colorspace,
so over HDR10 video the bitmap must already be BT.2020 + PQ. --pq encodes
sRGB UI pixels that way, with SDR white at --white nits (203 = BT.2408
reference white, what gamescope/Steam use for SDR content in HDR).
Premultiplication happens on the encoded values, because mpv blends there.

usage: make-bgra.py in.png out.bgra [--pq] [--white 203]
"""
import argparse

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("dst")
ap.add_argument("--pq", action="store_true")
ap.add_argument("--white", type=float, default=203.0)
opt = ap.parse_args()
pq, white, src, dst = opt.pq, opt.white, opt.src, opt.dst

rgba = np.asarray(Image.open(src).convert("RGBA"), dtype=np.float64) / 255.0
rgb, a = rgba[..., :3], rgba[..., 3:4]

if pq:
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    bt709_to_2020 = np.array([[0.6274, 0.3293, 0.0433],
                              [0.0691, 0.9195, 0.0114],
                              [0.0164, 0.0880, 0.8956]])
    lin = np.clip(lin @ bt709_to_2020.T, 0, None) * (white / 10000.0)
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = lin ** m1
    rgb = ((c1 + c2 * p) / (1 + c3 * p)) ** m2

out = np.concatenate([rgb * a, a], axis=-1)[..., [2, 1, 0, 3]]  # premultiplied BGRA
(np.round(out * 255).astype(np.uint8)).tofile(dst)
print(f"{dst}: {rgba.shape[1]}x{rgba.shape[0]} {'PQ ' + str(white) + ' nits' if pq else 'sRGB'}")
