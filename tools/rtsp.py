#!/usr/bin/env python3
"""Measure and re-tune the vendor RTSP/encoder process on the camera.

Why this tool exists: on this camera /mnt/Factory/custom/rtsp is not just a
stream server, it OWNS the capture pipeline (/dev/isp_char, /dev/video0,
/dev/uio0, /dev/ion, /dev/akpcm_cdev1). Whatever it costs, the camera pays all
day. Measured with --measure on a box with one NVR connected and nobody looking
at the web UI:

    vendor rtsp            ~25% of the single 400MHz core, always
      venc_encode x2        ~2.3%   <- the actual H.264 encode
      venc_capture x2       ~1.5%
      ai_capture            ~1.9%   (audio in; the kernel aec_task adds ~7%)
      rtsp session threads  ~19%, and 8.25 points of it is KERNEL time
    WiFi driver (ssv6xxx)   ~14% while a stream is actually leaving the box

so the protocol side costs about five times what encoding costs, and both the
1280x720 and the 640x480 channel are encoded whether anyone asks for them or
not.

The only control surface this build offers is the binary's own command line (it
prints its table with -h): --main-width/-height/-kbps, --sub-width/-height/-kbps,
--gop, --minqp/--maxqp, --main-chn-name/--sub-chn-name. There is NO fps option:
the SDK runs a "vi_change_fps" thread that drops frame rate under load, which is
why a stream advertising 25 fps really delivers about 10. ONVIF would be another
way in, but this build has [cloud] onvif = 0 and no onvif process running, and
cmd_serverd on 127.0.0.1:8782 is a root shell server, not a media API.

Subcommands:
    --status              what is running, with what arguments, how many clients
    --measure [SECS]      per-thread CPU, WiFi throughput, delivered fps/size
    --set --main-width 1280 --gop 20 ...
                          write rtsp.conf + run_rtsp.sh, point config.sh at
                          them, restart the process
    --restart             restart with whatever rtsp.conf holds
    --restore             back to the stock start line, no arguments

Restarting drops the RTSP server for a few seconds: every NVR and every preview
session blinks. config.sh is copied to /mnt/camweb/backup/ before the first
edit, and a reboot always returns to whatever config.sh says.
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cam  # noqa: E402

BIN = "/mnt/Factory/custom/rtsp"
CONF = "/mnt/camweb/rtsp.conf"
RUNNER = "/mnt/camweb/run_rtsp.sh"
CFG = "/mnt/Factory/config.sh"
BACKUP_DIR = "/mnt/camweb/backup"
LOG = "/tmp/rtsp_run.log"


RUNNER_TEXT = """#!/bin/sh
# Starts the vendor encoder/stream server and keeps it up.
#
# The stock line in /mnt/Factory/config.sh is a bare "/mnt/Factory/custom/rtsp &"
# with no supervisor at all: if it ever dies the camera keeps running with no
# video for anyone until someone reboots it. run_web.sh has used this pattern on
# this box for a while, so the stream server gets the same treatment.
#
# The arguments live in rtsp.conf next to this file, so the encoder can be
# retuned without touching the boot script. See tools/rtsp.py.
CONF=%(CONF)s
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib
echo $$ > /tmp/camweb_rtsp.pid
while :; do
  ARGS=""
  [ -r $CONF ] && ARGS=$(cat $CONF 2>/dev/null)
  echo "$(cut -d. -f1 /proc/uptime)s starting rtsp: $ARGS" >> %(LOG)s 2>/dev/null
  %(BIN)s $ARGS >> %(LOG)s 2>&1
  echo "$(cut -d. -f1 /proc/uptime)s rtsp exited rc=$?, restart in 3s" >> %(LOG)s 2>/dev/null
  sleep 3
done
""" % {"CONF": CONF, "LOG": LOG, "BIN": BIN}


def ftp_put(path, data):
    """Write a file on the camera. Telnet printf is the wrong tool for a script."""
    from ftplib import FTP
    if isinstance(data, str):
        data = data.encode()
    ftp = FTP()
    ftp.connect(cam.HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    try:
        ftp.storbinary("STOR %s" % path, __import__("io").BytesIO(data))
    finally:
        ftp.quit()


def sh(c, cmd, timeout=30):
    return c.run(cmd, timeout=timeout)


def rtsp_pid(c):
    out = sh(c, "ps | grep 'custom/rtsp' | grep -v grep | head -2")
    m = re.search(r"^\s*(\d+)\s+\S+\s+\S+\s+(\S.*)$", out, re.M)
    if not m:
        return None, ""
    return int(m.group(1)), m.group(2).strip()


def grab(text):
    out = {}
    for m in re.finditer(r"^(\d{2,8}) (\d{2,10}) (\d{2,10})\n([a-zA-Z_0-9-]+)", text, re.M):
        out[int(m.group(1))] = (m.group(4), int(m.group(2)), int(m.group(3)))
    return out


LOOP = "for t in /proc/%s/task/*; do cut -d' ' -f1,14,15 $t/stat; cat $t/comm; done"


def threads(c, pid, secs):
    """Per-thread CPU deltas for one process, as a share of one core."""
    a = grab(sh(c, LOOP % pid))
    tx0 = wlan_tx(c)
    time.sleep(secs)
    b = grab(sh(c, LOOP % pid))
    tx1 = wlan_tx(c)
    rows, tot = [], 0.0
    for tid in sorted(a, key=lambda t: -((b.get(t, (0, 0, 0))[1] - a[t][1]) +
                                         (b.get(t, (0, 0, 0))[2] - a[t][2]))):
        u = (b.get(tid, (0, 0, 0))[1] - a[tid][1]) / 100.0 / secs * 100.0
        s = (b.get(tid, (0, 0, 0))[2] - a[tid][2]) / 100.0 / secs * 100.0
        tot += u + s
        rows.append((a[tid][0], u, s, str(tid)))
    txs = (tx1 - tx0) / float(secs) if (tx0 and tx1) else None
    return rows, tot, txs


def wlan_tx(c):
    f = sh(c, "grep wlan0 /proc/net/dev").replace("wlan0:", "wlan0 ").split()
    try:
        return int(f[9])
    except (IndexError, ValueError):
        return None


def probe(ch, secs=6):
    """What the channel actually is. Real fps comes from counting frames."""
    cmd = ["ffprobe", "-v", "error", "-rtsp_transport", "tcp",
           "-read_intervals", "+%d" % secs, "-count_frames",
           "-show_entries", "stream=codec_type,codec_name,width,height,nb_read_frames",
           "-of", "default=nw=1", "rtsp://%s:554/%s" % (cam.HOST, ch)]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=secs + 25).stdout.decode()
    except Exception as e:
        return "ffprobe failed: %s" % e
    keep, kind, frames = [], None, None
    for l in out.splitlines():
        k, _, v = l.partition("=")
        if k == "codec_type":
            kind = v
        elif k == "nb_read_frames" and kind == "video":
            frames = int(v)
        else:
            keep.append(l)
    if frames:
        keep.append("real_fps=%0.1f" % (frames / float(secs)))
    return "  ".join(keep)

OPTIONS = ("--main-width", "--main-height", "--sub-width", "--sub-height",
           "--main-kbps", "--sub-kbps", "--gop", "--minqp", "--maxqp",
           "--main-chn-name", "--sub-chn-name")


def status(c):
    pid, cmdline = rtsp_pid(c)
    print("rtsp pid : %s" % pid)
    print("cmdline  : %s" % cmdline)
    print("in effect: %s" % (c.readfile(CONF).strip() or "(no rtsp.conf: stock defaults)"))
    print(sh(c, "netstat -tnp 2>/dev/null | grep ':554'").strip())
    print("load     :", sh(c, "cat /proc/loadavg").strip())


def measure(c, secs):
    pid, cmdline = rtsp_pid(c)
    if not pid:
        print("rtsp is not running")
        return 1
    print("pid %s (%s), sampling %ds" % (pid, cmdline, secs))
    rows, tot, txs = threads(c, pid, secs)
    print("  thread                 user%   sys%   total%   of one 400MHz core")
    for name, u, s, tid in rows:
        if u + s > 0.05:
            print("  %-8s %-12s %5.2f  %5.2f   %5.2f" % (tid, name, u, s, u + s))
    print("  vendor rtsp total: %0.1f%% of one core" % tot)
    if txs:
        print("  wlan0 TX: %0.0f kB/s (%0.2f Mbps)" % (txs / 1024.0, txs * 8 / 1e6))
    print("  load:", sh(c, "cat /proc/loadavg").strip())
    print("  mem :", " ".join(sh(c, "free | sed -n 3p").split()))
    for ch in ("vs0", "vs1"):
        print("  /%s: %s" % (ch, probe(ch)))
    return 0


def parse_args(argv):
    out = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a not in OPTIONS:
            print("unknown option %r; only these pass through: %s" % (a, " ".join(OPTIONS)))
            return None
        if i + 1 >= len(argv):
            print("%s needs a value" % a)
            return None
        out.append("%s %s" % (a, argv[i + 1]))
        i += 2
    return " ".join(out)


def set_args(c, args):
    sh(c, "mkdir -p %s" % BACKUP_DIR)
    cur = c.readfile(CFG)
    if not cur:
        print("cannot read %s - refusing to touch the boot script" % CFG)
        return 1
    if not c.readfile("%s/config.sh.orig" % BACKUP_DIR):
        ftp_put("%s/config.sh.orig" % BACKUP_DIR, cur)
        print("saved stock config.sh to %s/config.sh.orig" % BACKUP_DIR)
    ftp_put(CONF, args + "\n")
    ftp_put(RUNNER, RUNNER_TEXT)
    sh(c, "chmod +x %s; sh -n %s && echo runner_parses" % (RUNNER, RUNNER))
    print("rtsp.conf : %s" % (c.readfile(CONF).strip()))
    if RUNNER not in cur:
        new, n = re.subn(r"^/mnt/Factory/custom/rtsp(&| .*)$",
                         "/bin/sh %s \\g<1>" % RUNNER, cur, flags=re.M)
        if n == 0:
            print("WARNING: no bare rtsp line in config.sh; not editing it")
        else:
            ftp_put("%s/config.sh.before" % BACKUP_DIR, cur)
            ftp_put(CFG, new)
            print("config.sh: rtsp starts through run_rtsp.sh (%d line changed)" % n)
    return restart(c)


def restart(c):
    pid, _ = rtsp_pid(c)
    if pid:
        print("stopping pid %d" % pid)
        sh(c, "pkill -f run_rtsp.sh 2>/dev/null; kill %d 2>/dev/null; sleep 1; kill -9 %d 2>/dev/null" % (pid, pid))
        time.sleep(1)
    args = c.readfile(CONF).strip()
    print("starting: %s %s" % (BIN, args or "(no arguments)"))
    sh(c, "setsid /bin/sh %s </dev/null >/dev/null 2>&1 & sleep 4" % RUNNER, timeout=25)
    print(sh(c, "netstat -tln 2>/dev/null | grep ':554'; ps | grep custom/rtsp | grep -v grep; tail -2 %s 2>/dev/null" % LOG))
    return 0


def restore(c):
    bak = c.readfile("%s/config.sh.before" % BACKUP_DIR) or c.readfile("%s/config.sh.orig" % BACKUP_DIR)
    if not bak:
        print("no saved copy of config.sh in %s - refusing to guess" % BACKUP_DIR)
        return 1
    ftp_put(CFG, bak)
    sh(c, "rm -f %s" % CONF)
    pid, _ = rtsp_pid(c)
    if pid:
        sh(c, "pkill -f run_rtsp.sh 2>/dev/null; kill -9 %d 2>/dev/null" % pid)
    print("config.sh restored. Starting the stock server with no arguments:")
    sh(c, "setsid env LD_LIBRARY_PATH=/mnt/Factory/custom/lib %s </dev/null >>%s 2>&1 & sleep 4; ps | grep custom/rtsp | grep -v grep" % (BIN, LOG), timeout=25)
    return 0


def main():
    argv = sys.argv[1:]
    mode = argv[0] if argv else "--status"
    c = cam.Cam()
    try:
        if mode == "--status":
            status(c)
        elif mode == "--measure":
            return measure(c, int(argv[1]) if len(argv) > 1 else 12)
        elif mode == "--set":
            args = parse_args(argv[1:])
            return 2 if args is None else set_args(c, args)
        elif mode == "--restart":
            return restart(c)
        elif mode == "--restore":
            return restore(c)
        else:
            print(__doc__)
            return 2
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())

