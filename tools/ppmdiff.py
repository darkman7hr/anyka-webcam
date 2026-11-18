#!/usr/bin/env python3
"""Compare two PPM frames from rgbshot.py and say what actually changed.

Mean brightness alone cannot answer "did the IR illuminator do anything": the ISP
runs automatic exposure/gain, so taking the light away mostly raises the gain and
the mean stays put. What does not lie is the per-pixel difference between two
frames of a static scene - if the illuminator switched, the difference is large and
concentrated in the areas it lit; if nothing changed, the difference is sensor
noise and is small and evenly spread.

  ./tools/ppmdiff.py a.ppm b.ppm
"""
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise SystemExit(f"{path}: not a binary PPM (P6)")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = (int(x) for x in line.split())
        maxv = int(f.readline())
        if maxv != 255:
            raise SystemExit(f"{path}: maxval {maxv}, expected 255")
        data = f.read()
    if len(data) != w * h * 3:
        raise SystemExit(f"{path}: short pixel data ({len(data)} of {w * h * 3})")
    return w, h, data


def stats(w, h, d):
    # bytes * int repeats the byte string, so this walks the pixels instead of
    # trying to do it with slices
    n = w * h
    s = 0
    mx = 0
    mn = 255
    for i in range(0, n * 3, 3):
        lum = (77 * d[i] + 150 * d[i + 1] + 29 * d[i + 2]) // 256
        s += lum
        if lum > mx:
            mx = lum
        if lum < mn:
            mn = lum
    return s / n, mx, mn


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip())
    wa, ha, a = read_ppm(sys.argv[1])
    wb, hb, b = read_ppm(sys.argv[2])
    if (wa, ha) != (wb, hb):
        raise SystemExit(f"different sizes: {wa}x{ha} vs {wb}x{hb}")

    n = wa * ha
    # mean |difference| per channel, and how many pixels moved by more than a
    # few code numbers (sensor noise on this sensor is a couple of codes)
    ch = [0, 0, 0]
    moved = 0
    for i in range(n * 3):
        x = a[i]
        y = b[i]
        dd = x - y if x >= y else y - x
        ch[i % 3] += dd
        if dd > 8:
            moved += 1
    for c in range(3):
        ch[c] /= n
    diff = sum(ch) / 3.0

    la = stats(wa, ha, a)
    lb = stats(wb, hb, b)
    print(f"  pixels        {n} ({wa}x{ha})")
    print(f"  mean |diff|   R {ch[0]:6.2f}  G {ch[1]:6.2f}  B {ch[2]:6.2f}   avg {diff:6.2f}")
    print(f"  pixels moved  {moved} ({100.0 * moved / (3 * n):.1f}%) by more than 8 codes")
    print(f"  luminance     a mean {la[0]:6.1f} [{la[2]:.0f}..{la[1]:.0f}]   "
          f"b mean {lb[0]:6.1f} [{lb[2]:.0f}..{lb[1]:.0f}]   delta {lb[0] - la[0]:+6.1f}")
    # A judgement, not just numbers, because the question is always "so did it work?"
    if diff < 1.5:
        print("  verdict: the two frames are the same picture (noise only)")
    elif abs(lb[0] - la[0]) > 20:
        print("  verdict: the scene brightness changed")
    else:
        print("  verdict: the scene changed but brightness held - automatic "
              "exposure/gain is compensating")


if __name__ == "__main__":
    main()
