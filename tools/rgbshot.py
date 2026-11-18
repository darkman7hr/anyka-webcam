#!/usr/bin/env python3
"""Grab one frame from the camera and report its colour in numbers.

    ./rgbshot.py [rtsp-url] [--out shot.ppm]

"The picture looks purple" is a symptom; R/G/B means over a real frame are a
measurement. Two signatures separate cleanly:

  IR wash (IR-cut open, or ISP in the wrong mode)
      every pixel is tinted - neutrals included - so R-G and B-G are both high
      and the histogram is compressed (low contrast, the sensor is saturated by
      infrared the visible-only optics were tuned for).
  Swapped chroma (U/V planes the wrong way round)
      colours rotate but neutrals stay neutral, because U=V=128 is unchanged by
      a swap. So the neutral fraction stays normal while hues look wrong.
"""
import os
import subprocess
import sys

URL_DEFAULT = "rtsp://192.168.10.46:554/vs0"


def grab(url, out):
    # Joining an H.264 stream mid-GOP makes ffmpeg decode a P-frame with no
    # reference: that frame decodes as near-black/flat grey and looks exactly
    # like "the image went dark". Wait for a real key frame instead, and allow a
    # couple of seconds for one to arrive (GOP here is 1 s).
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-rtsp_transport", "tcp",
           "-i", url, "-t", "6",
           "-vf", "select=eq(pict_type\\,I)", "-fps_mode", "vfr",
           "-frames:v", "1", "-f", "image2", "-c:v", "ppm", "-y", out]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode:
        print("ffmpeg failed: %s" % (r.stderr.strip().splitlines()[-1] if r.stderr.strip() else r.returncode))
        return None
    return out


def read_ppm(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise SystemExit("not a binary PPM")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = (int(x) for x in line.split())
        maxv = int(f.readline().strip())
        if maxv != 255:
            raise SystemExit("only 8-bit PPM supported")
        data = f.read(w * h * 3)
    if len(data) < w * h * 3:
        raise SystemExit("short read: %d of %d bytes - the frame is incomplete" %
                         (len(data), w * h * 3))
    return w, h, data


def report(url, out):
    if not grab(url, out):
        return 1
    w, h, d = read_ppm(out)
    n = w * h
    sr = sg = sb = 0
    neutral = 0
    lo = [255, 255, 255]
    hi = [0, 0, 0]
    for i in range(0, len(d), 3):
        r, g, b = d[i], d[i + 1], d[i + 2]
        sr += r; sg += g; sb += b
        if max(r, g, b) - min(r, g, b) < 12:
            neutral += 1
        lo[0] = min(lo[0], r); hi[0] = max(hi[0], r)
        lo[1] = min(lo[1], g); hi[1] = max(hi[1], g)
        lo[2] = min(lo[2], b); hi[2] = max(hi[2], b)
    mr, mg, mb = sr / n, sg / n, sb / n
    print("%s  %dx%d" % (url, w, h))
    print("  mean R=%.1f G=%.1f B=%.1f   R-G=%+.1f B-G=%+.1f" % (mr, mg, mb, mr - mg, mb - mg))
    print("  range R %d..%d  G %d..%d  B %d..%d" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    print("  near-neutral pixels: %.1f%% (a chroma swap keeps these neutral; an IR wash tints them)" %
          (100.0 * neutral / n))
    tint = "IR/night wash" if (mr - mg > 12 and mb - mg > 12) else "normal-ish"
    print("  verdict: %s" % tint)
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    url = a[0] if a and not a[0].startswith("-") else URL_DEFAULT
    out = "/tmp/rgbshot.ppm"
    if "--out" in a:
        out = a[a.index("--out") + 1]
    sys.exit(report(url, out))
