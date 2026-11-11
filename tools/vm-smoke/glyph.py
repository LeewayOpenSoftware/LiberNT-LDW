#!/usr/bin/env python3
"""glyph.py PGM  - judge the min / max / close caption glyphs in a thresholded caption strip.

PGM is an ASCII PGM (P2) of the top-right caption strip, already converted to gray and thresholded so only the
bright glyph pixels are 255. The three glyphs are found as groups of bright columns (left to right: min, max,
close). Each must have the right shape:
  min    wide and flat (width >= 1.6 x height), mirror-symmetric
  max    roughly square (0.7..1.45), mirror-symmetric, a frame (fill < 0.9)
  close  roughly square (0.75..1.33), mirror-symmetric left-right AND top-bottom, not a solid blob (fill < 0.75)
Prints one line per glyph and the reason of the first failure. Exit code = number of bad glyphs (3 if the three
glyphs could not be found). Standard library only.
"""
import sys


def load(path):
    tok = open(path).read().split()
    assert tok[0] == "P2", "not an ASCII PGM"
    w, h, _ = int(tok[1]), int(tok[2]), int(tok[3])
    v = list(map(int, tok[4:]))
    return w, h, [[1 if v[y * w + x] > 127 else 0 for x in range(w)] for y in range(h)]


def groups(cols, gap=3):
    out, cur = [], None
    for x, on in enumerate(cols):
        if on:
            if cur is None:
                cur = [x, x]
            elif x - cur[1] <= gap:
                cur[1] = x
            else:
                out.append(tuple(cur)); cur = [x, x]
    if cur:
        out.append(tuple(cur))
    return out


def shape(g, x0, x1):
    ys = [y for y in range(len(g)) if any(g[y][x0:x1 + 1])]
    y0, y1 = min(ys), max(ys)
    s = [row[x0:x1 + 1] for row in g[y0:y1 + 1]]
    w, h = x1 - x0 + 1, y1 - y0 + 1
    n = sum(map(sum, s))
    flop = sum(1 for r in s for i in range(w) if r[i] != r[w - 1 - i])
    flip = sum(1 for j in range(h) for i in range(w) if s[j][i] != s[h - 1 - j][i])
    return w, h, n, flop / max(n, 1), flip / max(n, 1), n / (w * h)


def main():
    w, h, g = load(sys.argv[1])
    cols = [any(g[y][x] for y in range(h)) for x in range(w)]
    gs = groups(cols)
    # keep the three right-most groups with at least a few pixels
    gs = [t for t in gs if sum(g[y][x] for y in range(h) for x in range(t[0], t[1] + 1)) >= 4][-3:]
    if len(gs) != 3:
        print("glyphs  FAIL  found %d glyph group(s) instead of 3 (min, max, close)" % len(gs))
        return 3
    bad = 0
    for name, (x0, x1) in zip(("min", "max", "close"), gs):
        gw, gh, n, flop, flip, fill = shape(g, x0, x1)
        asp = gw / gh
        why = []
        if name == "min":
            if asp < 1.6: why.append("not flat (w/h %.2f)" % asp)
            if flop > 0.15: why.append("left-right asymmetric (%.2f)" % flop)
        elif name == "max":
            if not 0.7 <= asp <= 1.45: why.append("not square (w/h %.2f)" % asp)
            if flop > 0.15 or flip > 0.15: why.append("asymmetric (%.2f/%.2f)" % (flop, flip))
            if fill >= 0.9: why.append("solid blob (fill %.2f)" % fill)
        else:
            if not 0.75 <= asp <= 1.33: why.append("not square (w/h %.2f)" % asp)
            if flop > 0.2 or flip > 0.2: why.append("asymmetric (lr %.2f, tb %.2f)" % (flop, flip))
            if fill >= 0.75: why.append("solid blob (fill %.2f)" % fill)
        print("glyph.%s  %s  %dx%d px bright=%d w/h=%.2f lr-asym=%.2f tb-asym=%.2f fill=%.2f %s" %
              (name, "FAIL" if why else "PASS", gw, gh, n, asp, flop, flip, fill, "; ".join(why)))
        bad += 1 if why else 0
    return bad


sys.exit(main())
