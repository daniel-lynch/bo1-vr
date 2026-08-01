#!/usr/bin/env python3
"""Read the verdict off a screenshot of Monado's compositor output.

The screenshot is the client area of Monado's XRT_WINDOW_PEEK window, which
holds the two eye images side by side: left eye in the left half, right eye in
the right half. visual.c drew a known pattern into each eye's D3D9 render
target *before* submit; this samples the same features out of what came back
and says whether they survived the trip.

The sampling geometry is a transcription of layout_init() in visual.c. If you
change the pattern there, change it here.

    analyse.py SHOT.png --rw 896 --rh 1007 [--json OUT.json]
"""

import argparse
import json
import sys

from PIL import Image

# What visual.c drew, keyed by where it drew it.
TAGS = {
    "TL": ((255, 255, 0), "yellow"),
    "TR": ((0, 255, 255), "cyan"),
    "BL": ((255, 0, 255), "magenta"),
    "BR": ((0, 0, 255), "blue"),
}
BG = {0: ((140, 16, 16), "dark red"), 1: ((16, 110, 32), "dark green")}
RAMP = [i * 255 // 7 for i in range(8)]
TOL = 26          # per-channel tolerance; the peek blit rescales 896x1007 -> 640x720
RAMP_TOL = 12


def layout(rw, rh):
    """Mirror of layout_init() in visual.c."""
    L = {}
    L["tag"] = rw // 8
    L["s_px"] = rh // 40
    L["b_px"] = rh // 18
    L["y_up"] = L["tag"] + rh // 50
    L["y_lane"] = L["y_up"] + 7 * L["s_px"] + rh // 80
    L["y_big"] = L["y_lane"] + L["s_px"] + rh // 25
    L["y_dn"] = L["y_big"] + 7 * L["b_px"] + rh // 60
    L["h_ramp"] = rh // 25
    L["y_ramp"] = rh - L["h_ramp"]
    return L


def nearest(rgb, table):
    best, bestd = None, 1 << 30
    for name, (ref, human) in table.items():
        d = max(abs(a - b) for a, b in zip(rgb, ref))
        if d < bestd:
            best, bestd = (name, human, d), d
    return best


class Eye:
    """One half of the peek window, sampled in source-texture coordinates."""

    def __init__(self, im, x0, x1, rw, rh):
        self.im = im.convert("RGB")
        self.x0, self.w = x0, x1 - x0
        self.h = im.size[1]
        self.rw, self.rh = rw, rh

    def at(self, sx, sy):
        """Median-ish sample of a 5x5 patch around source pixel (sx, sy)."""
        px = self.x0 + int(sx / self.rw * self.w)
        py = int(sy / self.rh * self.h)
        px = min(max(px, self.x0 + 2), self.x0 + self.w - 3)
        py = min(max(py, 2), self.h - 3)
        vals = [self.im.getpixel((px + dx, py + dy))
                for dx in (-2, 0, 2) for dy in (-2, 0, 2)]
        return tuple(sorted(v[c] for v in vals)[len(vals) // 2] for c in range(3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shot")
    ap.add_argument("--rw", type=int, default=896)
    ap.add_argument("--rh", type=int, default=1007)
    ap.add_argument("--single", choices=["left", "right"],
                    help="the shot holds ONE eye filling the frame "
                         "(XRT_WINDOW_PEEK=left|right), not the side-by-side pair")
    ap.add_argument("--json")
    a = ap.parse_args()

    im = Image.open(a.shot)
    W, H = im.size
    L = layout(a.rw, a.rh)
    tag = L["tag"]

    pts = {
        "TL": (tag / 2, tag / 2),
        "TR": (a.rw - tag / 2, tag / 2),
        "BL": (tag / 2, a.rh - tag / 2),
        "BR": (a.rw - tag / 2, a.rh - tag / 2),
    }
    rx = tag + a.rw // 40
    rwid = (a.rw - 2 * rx) / 8
    ramp_pts = [(rx + i * rwid + rwid / 2, L["y_ramp"] + L["h_ramp"] / 2) for i in range(8)]
    bg_pt = (a.rw / 2, tag + a.rh / 100)

    print("screenshot   : %s  %dx%d" % (a.shot, W, H))
    if a.single:
        print("source eye   : %dx%d, shown alone (XRT_WINDOW_PEEK=%s)" % (a.rw, a.rh, a.single))
        eyes = {0 if a.single == "left" else 1: Eye(im, 0, W, a.rw, a.rh)}
        labels = {0: "LEFT eye  ", 1: "RIGHT eye "}
    else:
        print("source eye   : %dx%d, peek half %dx%d" % (a.rw, a.rh, W // 2, H))
        eyes = {0: Eye(im, 0, W // 2, a.rw, a.rh), 1: Eye(im, W // 2, W, a.rw, a.rh)}
        labels = {0: "LEFT half ", 1: "RIGHT half"}
    print()

    result = {"shot": a.shot, "size": [W, H], "single": a.single, "eyes": {}, "verdicts": {}}
    ok = True
    unreadable = False

    for idx, eye in eyes.items():
        side = labels[idx]
        e = {}
        bg = eye.at(*bg_pt)
        e["bg"] = bg
        name, human, d = nearest(bg, BG)
        e["bg_is"] = "LEFT" if name == 0 else "RIGHT"
        print("%s  background      %-15s -> the %s eye's colour (%s), delta %d"
              % (side, str(bg), "LEFT" if name == 0 else "RIGHT", human, d))
        for k in ("TL", "TR", "BL", "BR"):
            v = eye.at(*pts[k])
            e[k] = v
            got, human, d = nearest(v, TAGS)
            e[k + "_is"] = got
            flag = "  <-- expected %s" % k if got != k else ""
            print("%s  corner %s      %-15s -> %-8s %s%s" % (side, k, str(v), got, human, flag))
        e["ramp"] = [eye.at(*p) for p in ramp_pts]
        print("%s  grey ramp       want %s" % (side, " ".join("%3d" % v for v in RAMP)))
        print("%s                  got  %s"
              % (side, " ".join("%3d" % round(sum(c) / 3) for c in e["ramp"])))
        result["eyes"][idx] = e

    # ---- verdict 1: eye order -------------------------------------------
    if a.single:
        idx = 0 if a.single == "left" else 1
        shown = nearest(eyes[idx].at(*bg_pt), BG)[0]
        want = "LEFT" if idx == 0 else "RIGHT"
        got = "LEFT" if shown == 0 else "RIGHT"
        d = nearest(eyes[idx].at(*bg_pt), BG)[2]
        if d > TOL:
            v = "UNREADABLE -- the %s eye holds no recognised background colour" % want
            ok = False
            unreadable = True
        elif got == want:
            v = ("CORRECT -- the %s eye alone was displayed and it holds the %s eye's "
                 "content" % (want, got))
        else:
            v = "SWAPPED -- the %s eye is showing the %s eye's content" % (want, got)
            ok = False
    else:
        lb, rb = eyes[0].at(*bg_pt), eyes[1].at(*bg_pt)
        l_is, _, ld = nearest(lb, BG)
        r_is, _, rd = nearest(rb, BG)
        if max(ld, rd) > TOL:
            v = "UNREADABLE -- neither half matches a per-eye background colour"
            ok = False
            unreadable = True
        elif l_is == 0 and r_is == 1:
            v = "CORRECT -- the left eye's content is in the left eye"
        elif l_is == 1 and r_is == 0:
            v = "SWAPPED -- left content is being presented to the right eye"
            ok = False
        else:
            v = "UNREADABLE -- both halves carry the same eye's background"
            ok = False
            unreadable = True
    result["verdicts"]["eye_order"] = v
    print("\nEYE ORDER        : %s" % v)

    # ---- verdict 2: orientation -----------------------------------------
    for idx, eye in eyes.items():
        got = {k: nearest(eye.at(*pts[k]), TAGS)[0] for k in pts}
        side = "left" if idx == 0 else "right"
        if got == {"TL": "TL", "TR": "TR", "BL": "BL", "BR": "BR"}:
            v = "CORRECT -- up is up, no mirroring"
        elif got == {"TL": "BL", "TR": "BR", "BL": "TL", "BR": "TR"}:
            v = "VERTICALLY FLIPPED -- the image is upside down"
            ok = False
        elif got == {"TL": "TR", "TR": "TL", "BL": "BR", "BR": "BL"}:
            v = "HORIZONTALLY MIRRORED"
            ok = False
        elif got == {"TL": "BR", "TR": "BL", "BL": "TR", "BR": "TL"}:
            v = "ROTATED 180 DEGREES"
            ok = False
        else:
            v = "UNREADABLE -- corner tags are %s" % got
            ok = False
            unreadable = True
        result["verdicts"]["orientation_%s" % side] = v
        print("ORIENTATION %-5s: %s" % (side, v))

    # ---- verdict 3: colour handling -------------------------------------
    worst, worst_i = 0, 0
    for idx, eye in eyes.items():
        for i, p in enumerate(ramp_pts):
            got = round(sum(eye.at(*p)) / 3)
            d = abs(got - RAMP[i])
            if d > worst:
                worst, worst_i = d, i
    if worst <= RAMP_TOL:
        v = "CLEAN -- grey ramp survives end to end, worst step off by %d/255" % worst
    else:
        v = ("SHIFTED -- grey ramp step %d off by %d/255; a double sRGB conversion "
             "looks exactly like this" % (worst_i, worst))
        ok = False
    result["verdicts"]["colour"] = v
    print("COLOUR           : %s" % v)

    print("\nRESULT: %s" % ("PASS" if ok else ("UNREADABLE" if unreadable else "FAIL")))
    result["pass"] = ok
    result["unreadable"] = unreadable
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=2, default=list)
    # 0 = everything as drawn, 1 = read cleanly but something is wrong (which is
    # what the deliberately flipped control run is supposed to produce),
    # 2 = the picture could not be read at all, i.e. no measurement happened.
    return 0 if ok else (2 if unreadable else 1)


if __name__ == "__main__":
    sys.exit(main())
