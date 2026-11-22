#!/usr/bin/env python3
"""Pixel check: letitsnowd's frame against the original Python snow.

    reference_snow.py DUMP BASE SHOT [OUT_EXPECTED]

DUMP is what letitsnowd writes with LETITSNOW_DUMP=file (and LETITSNOW_FREEZE=t
so the frame holds still); BASE is a screenshot of the same output without the
snow; SHOT is a screenshot with it. The reference below is the Joyful reel's
Snow class unchanged except that the flakes come from the dump instead of
numpy's generator: same sprites, same 12 alpha levels, same pasting. It draws
the dumped flakes onto BASE and reports how far SHOT is from that.
"""
import json
import math
import sys

import numpy as np
from PIL import Image

LEVELS = 12


def paste(dst, src, x, y, W, H):
    x, y = int(round(x)), int(round(y))
    sw, sh = src.size
    l, t = max(0, -x), max(0, -y)
    r, b = min(sw, W - x), min(sh, H - y)
    if r > l and b > t:
        dst.alpha_composite(src, dest=(x + l, y + t), source=(l, t, r, b))


class Snow:
    """compose.py's Snow, fed from a dump."""

    def __init__(self, flakes):
        f = np.array(flakes, dtype=np.float64)
        self.ux, self.uy, self.s, self.amp, self.f, self.ph, self.a = f.T
        self.v = 32 + 20 * self.s
        self.bucket = np.round(self.s * 2).astype(int)
        self.sprites = {}
        for b in set(self.bucket.tolist()):
            r = b / 2
            k = int(math.ceil(r * 3)) * 2 + 1
            yy, xx = np.mgrid[0:k, 0:k].astype(np.float32) - (k - 1) / 2
            al = np.exp(-(xx * xx + yy * yy) / (r * r) * (1.4 if r < 4.5 else 0.9))
            for lv in range(1, LEVELS + 1):
                a = (al * 255 * lv / LEVELS).astype(np.uint8)
                self.sprites[b, lv] = Image.fromarray(np.dstack([np.full_like(a, 250), np.full_like(a, 248),
                                                                 np.full_like(a, 240), a]), "RGBA")

    def draw(self, fr, t, inten, W, H):
        x0, y0 = self.ux * (W + 40), self.uy * (H + 60)
        y = (y0 + self.v * t) % (H + 60) - 30
        x = (x0 + 14 * t + self.amp * np.sin(2 * np.pi * self.f * t + self.ph)) % (W + 40) - 20
        lv = np.round(self.a * inten * LEVELS).astype(int)
        for i in range(len(y)):
            if lv[i] > 0:
                spr = self.sprites[self.bucket[i], lv[i]]
                paste(fr, spr, x[i] - spr.width / 2, y[i] - spr.height / 2, W, H)


def flat(path):
    """A screenshot as opaque RGB. Where nothing opaque is under the snow (a
    headless output with no wallpaper), the capture keeps alpha; those pixels
    are what they would be over black."""
    im = Image.open(path)
    if "A" in im.getbands():
        black = Image.new("RGBA", im.size, (0, 0, 0, 255))
        im = Image.alpha_composite(black, im.convert("RGBA"))
    return im.convert("RGB")


def main():
    dump_path, base_path, shot_path = sys.argv[1:4]
    out = sys.argv[4] if len(sys.argv) > 4 else None
    d = json.loads(open(dump_path).readline())
    W, H = d["width"], d["height"]
    fr = flat(base_path).convert("RGBA")
    assert fr.size == (W, H), (fr.size, W, H)
    Snow(d["flakes"]).draw(fr, d["tau"], d["intensity"], W, H)
    exp = np.asarray(fr.convert("RGB")).astype(int)
    got = np.asarray(flat(shot_path)).astype(int)
    base = np.asarray(flat(base_path)).astype(int)
    if out:
        fr.convert("RGB").save(out)
    # Rows the bar and dock cover are left out: they sit above the snow and
    # blur it, which the reference does not model.
    y0, y1 = int(sys.argv[5]) if len(sys.argv) > 5 else 0, int(sys.argv[6]) if len(sys.argv) > 6 else H
    e, g, b = exp[y0:y1], got[y0:y1], base[y0:y1]
    diff = np.abs(e - g).max(axis=2)
    snow_px = (np.abs(e - b).max(axis=2) > 0)
    print(f"{len(d['flakes'])} flakes, t={d['tau']}, intensity {d['intensity']}, rows {y0}-{y1}")
    print(f"pixels the snow changes: {snow_px.sum()}")
    print(f"max channel difference: {diff.max()}, mean over snow pixels: {diff[snow_px].mean():.3f}")
    for lim in (0, 1, 2, 3):
        print(f"  pixels off by more than {lim}: {(diff > lim).sum()}")
    return 0 if diff.max() <= 3 else 1


if __name__ == "__main__":
    sys.exit(main())
