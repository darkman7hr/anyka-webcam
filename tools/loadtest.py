#!/usr/bin/env python3
"""Measure what one open UI tab costs the camera.

The CGI is a shell script: every act=status forks cut/awk/sed/ip/grep children,
so a browser polling every 1.5 s is not free on an armv5 box that already sits
near load 3. This measures jiffies per request against a quiet control period.
"""

import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cam import Cam

HOST = os.environ.get("CAM_HOST", "192.168.10.46")
def _saved_port():
    try:
        return open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 ".port")).read().strip() or "8080"
    except Exception:
        return "8080"


PORT = os.environ.get("CAM_HTTP_PORT") or _saved_port()
ROOT = os.path.dirname(os.path.abspath(__file__))


def cpu_jiffies(cam):
    out = cam.run("head -1 /proc/stat")
    for line in out.splitlines():
        if line.startswith("cpu "):
            return sum(int(x) for x in line.split()[1:6])
    raise RuntimeError("no cpu line: " + out[:80])


def load1(cam):
    return cam.run("cut -d' ' -f1 /proc/loadavg").strip().splitlines()[-1].strip()


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    tok = open(os.path.join(ROOT, ".token")).read().strip()
    url = "http://%s:%s/cgi-bin/api?act=status&token=%s" % (HOST, PORT, tok)
    cam = Cam(HOST)
    try:
        print("control: %d s with no requests" % n)
        a = cpu_jiffies(cam)
        la_a = load1(cam)
        time.sleep(n)
        b = cpu_jiffies(cam)
        la_b = load1(cam)
        print("  jiffies %d -> %d  (delta %d, %.2f jiffies/s idle-of-ui)  load %s -> %s"
              % (a, b, b - a, (b - a) / float(n), la_a, la_b))

        print("test: %d status requests, one per second" % n)
        c = cpu_jiffies(cam)
        lc_a = load1(cam)
        t0 = time.time()
        ok = 0
        for _ in range(n):
            try:
                urllib.request.urlopen(url, timeout=8).read()
                ok += 1
            except Exception as e:
                print("   request failed:", e)
            time.sleep(1.0)
        d = cpu_jiffies(cam)
        lc_b = load1(cam)
        dt = time.time() - t0
        spent = (d - c) - (b - a)          # subtract the control-rate baseline
        print("  jiffies %d -> %d in %.1fs (%d ok), load %s -> %s" % (c, d, dt, ok, lc_a, lc_b))
        print("  attributable to the CGI: %d jiffies over %d requests = %.1f jiffies (~%.0f ms cpu) each"
              % (spent, n, spent / float(n), spent / float(n) * 10))
        print("  one tab polling every 1.5 s therefore costs about %.1f%% of one core"
              % (100.0 * (spent / float(n)) / 100.0 / 1.5))
    finally:
        cam.close()


if __name__ == "__main__":
    main()
