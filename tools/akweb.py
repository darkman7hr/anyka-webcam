#!/usr/bin/env python3
"""Who actually uses the camera's web UI - read from the camera's own log.

    ./akweb.py --once             classify the whole CGI action log
    ./akweb.py --watch [N]        poll it and print what changed every N seconds
    ./akweb.py --serve [port]     render one local HTML summary (Ctrl-C to stop)

The camera's CGI records every action it is asked to perform, with the caller's
address:

    149533s a=live_start d= s= p= t= n= from=192.168.10.70

That answers the question a preview button cannot answer by itself: does anyone
press it? Counts here include this workstation, so the summary labels the
address this tool is running from as "operator" - otherwise every test run
inflates the viewer count, which is exactly what happened once already.

One limit worth stating: this log holds CGI actions, not static file requests.
Someone loading the page without pressing anything is invisible to it.
"""
import os
import re
import socket
import subprocess
import sys
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy  # noqa: E402
from cam import Cam  # noqa: E402

LOG = "/tmp/camweb_req.log"
LINE = re.compile(r"^(\d+)s\s+a=(\S*)\s+.*from=(\d+\.\d+\.\d+\.\d+)\s*$")


def operator_ip():
    """The address this machine uses to reach the camera (no traffic sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((deploy.HOST, 9))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def fetch(cam, since=0):
    """Log lines newer than `since` (uptime seconds), oldest first."""
    out = cam.run("awk '$1+0 > %d' %s 2>/dev/null | head -400" % (since, LOG), timeout=60)
    rows = []
    for ln in out.splitlines():
        m = LINE.match(ln.strip())
        if m:
            rows.append((int(m.group(1)), m.group(2), m.group(3)))
    return rows


def summarize(rows, mine):
    acts = Counter(a for _, a, _ in rows)
    ips = Counter(ip for _, _, ip in rows)
    live = Counter(ip for _, a, ip in rows if a.startswith("live"))
    return acts, ips, live


def report(cam):
    mine = operator_ip()
    rows = fetch(cam)
    if not rows:
        print("no CGI actions in %s (log empty or absent)" % LOG)
        return 1
    acts, ips, live = summarize(rows, mine)
    span = rows[-1][0] - rows[0][0]
    print("%d actions in %s, spanning %.1f h" % (len(rows), LOG, span / 3600.0))
    print("first uptime %ds, last uptime %ds (camera clock, not wall time)" % (rows[0][0], rows[-1][0]))
    print("\nactions:")
    for a, n in acts.most_common():
        print("  %-14s %d" % (a, n))
    print("\nwho:")
    for ip, n in ips.most_common():
        who = "  <- this machine (operator/tests)" if ip == mine else ""
        print("  %-16s %3d actions, %2d live-related%s" % (ip, n, live.get(ip, 0), who))
    others = [ip for ip in ips if ip != mine]
    print("\nviewers that are NOT this machine: %d (%s)" % (
        len(others), ", ".join(others) if others else "none"))
    print("last 6 actions:")
    for up, a, ip in rows[-6:]:
        print("  %7ds %-14s %s" % (up, a, ip))
    return 0


def watch(cam, n):
    seen = 0
    print("watching %s every %ds (ctrl-C stops)" % (LOG, n))
    while True:
        rows = fetch(cam, since=seen)
        if rows:
            for up, a, ip in rows:
                print("  %7ds %-14s %s" % (up, a, ip), flush=True)
            seen = rows[-1][0]
        time.sleep(n)


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            body = subprocess.run([sys.executable, os.path.abspath(__file__), "--once"],
                                  capture_output=True, text=True, timeout=60).stdout
        except Exception as e:
            body = "akweb --once failed: %s" % e
        pre = ("<!doctype html><meta charset=utf-8><title>camera web usage</title>"
               "<style>body{background:#14171a;color:#e8eaed;font:14px ui-monospace,monospace;"
               "padding:18px}pre{white-space:pre-wrap}</style><h2>Anyka camera - web UI usage</h2><pre>%s</pre>") % body.replace("<", "&lt;")
        data = pre.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


def main():
    a = sys.argv[1:]
    if not a or "-h" in a or "--help" in a:
        print(__doc__)
        return 2
    if "--serve" in a:
        i = a.index("--serve")
        port = int(a[i + 1]) if i + 1 < len(a) and a[i + 1].isdigit() else 8088
        srv = HTTPServer(("0.0.0.0", port), H)
        print("akweb serving on :%d (ctrl-C stops)" % port, flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass
        return 0
    cam = Cam(deploy.HOST)
    try:
        if "--once" in a:
            return report(cam)
        if "--watch" in a:
            i = a.index("--watch")
            n = int(a[i + 1]) if i + 1 < len(a) and a[i + 1].isdigit() else 10
            watch(cam, n)
            return 0
        print(__doc__)
        return 2
    finally:
        cam.close()


if __name__ == "__main__":
    sys.exit(main())
