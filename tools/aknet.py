#!/usr/bin/env python3
"""Measure what the camera's stream server actually costs and delivers.

    ./aknet.py --stats            kernel CPU, capture processes, sockets on :554
    ./aknet.py --watch [N]        CPU and scheduling of one process over N seconds
    ./aknet.py --lan              what the WLAN link is really carrying
"""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy
from cam import Cam

CAM = "192.168.10.46"


def run(cam, cmd):
    return cam.run(cmd, timeout=90)


def pids_by_comm(cam, comm):
    """PIDs whose /proc/<pid>/comm is exactly `comm`.

    Never a ps grep: `grep -w rtsp` matches this script's own command line, and
    a path grep misses a server started as ./rtsp. The camera's shell has no
    pidof in every build either, so this walks /proc itself.
    """
    cmd = ("for d in /proc/[0-9]*; do c=$(cat $d/comm 2>/dev/null); "
           "[ \"$c\" = \"%s\" ] && echo ${d#/proc/}; done" % comm)
    out = run(cam, cmd)
    return [int(p) for p in re.findall(r"\d+", out)]


def stat_field(cam, pid, fields):
    """Selected whitespace fields of /proc/<pid>/stat (1-based as in proc(5))."""
    cmd = "awk '{for (i = 1; i <= NF; i++) ; print %s}' /proc/%d/stat" % (
        ", ".join("$%d" % i for i in fields), pid)
    return run(cam, cmd).split()


def stats(cam):
    print("== kernel threads by CPU (top, one pass) ==")
    print(run(cam, "top -bn1 2>/dev/null | head -14"))
    print("\n== who holds the capture ==")
    for c in ("akrtsp", "rtsp", "ivi", "cmd_serverd"):
        p = pids_by_comm(cam, c)
        if p:
            print("  %-12s pid %s  rss %s kB" % (
                c, " ".join(map(str, p)),
                run(cam, "awk '/VmRSS/{print $2}' /proc/%s/status" % p[0])))
    print("\n== sockets on :554 (Send-Q tells us whether we drain) ==")
    print(run(cam, "netstat -tan 2>/dev/null | grep -E ':554|:1984|:8554'"))
    print("== memory ==")
    print(run(cam, "free; echo -n 'overcommit_memory='; cat /proc/sys/vm/overcommit_memory"))


def watch(cam, comm, n):
    """CPU of `comm` measured from /proc/<pid>/stat, not top's swinging column."""
    print("== CPU of %s over %d s (proc stat, not top) ==" % (comm, n))
    pids = pids_by_comm(cam, comm)
    if not pids:
        print("  no process with comm == %s is running" % comm)
        return 1
    pid = pids[0]
    before = stat_field(cam, pid, [14, 15])          # utime, stime in jiffies
    hz = run(cam, "getconf CLK_TCK 2>/dev/null")
    hz = int(re.sub(r"\D", "", hz) or 100)
    sw0 = run(cam, "grep ctxt_switches /proc/%d/status" % pid)
    t0 = time.time()
    time.sleep(n)
    after = stat_field(cam, pid, [14, 15])
    wall = time.time() - t0
    if len(before) < 2 or len(after) < 2:
        print("  could not read /proc/%d/stat: %r / %r" % (pid, before, after))
        return 1
    db = (int(after[0]) + int(after[1])) - (int(before[0]) + int(before[1]))
    pct = 100.0 * (db / hz) / wall
    print("  pid=%d cpu=%.1f%% (utime %s -> %s jiffies over %.2f s)" % (
        pid, pct, before[0], after[0], wall))
    # /proc/<pid>/sched does not exist on this 3.4 build, but the switch counters
    # in status do - and "never preempted voluntarily while serving" is the point.
    print("  switches: %s" % " ".join(sw0.split()))
    print("  cpu time used in total: %s" % run(cam, "awk '{print $14, $15}' /proc/%d/stat" % pid).strip())
    print("  oom_score_adj=%s rss=%s kB" % (
        run(cam, "cat /proc/%d/oom_score_adj" % pid).strip(),
        run(cam, "awk '/VmRSS/{print $2}' /proc/%d/status" % pid).strip()))
    return 0


def lan(cam):
    """Does the radio carry what the encoder produces, or something else?"""
    print("== wlan0 counters over 12 s ==")
    a = run(cam, "awk '/tx bytes|rx bytes/ {print $1, $2}' /proc/net/dev; "
                 "grep -A1 wlan0 /proc/net/dev")
    tx0 = re.search(r"wlan0\s+(\d+)", run(cam, "grep wlan0 /proc/net/dev"))
    time.sleep(12)
    tx1 = re.search(r"wlan0\s+(\d+)", run(cam, "grep wlan0 /proc/net/dev"))
    if not (tx0 and tx1):
        print("  could not read wlan0 counters")
        return 1
    d = int(tx1.group(1)) - int(tx0.group(1))
    print("  wlan0 tx +%d bytes in 12 s = %.0f kbps" % (d, d * 8 / 12 / 1000))
    print("\n== encoder's own idea of its bitrate ==")
    print(run(cam, "cat /tmp/akrtsp.log 2>/dev/null | tail -2"))
    print("\n== the NVR's side ==")
    print(run(cam, "netstat -tan 2>/dev/null | grep :554"))
    return 0


def main():
    a = sys.argv[1:]
    if not a or "-h" in a or "--help" in a:
        print(__doc__)
        return 2
    cam = Cam(deploy.HOST)
    try:
        if "--stats" in a:
            stats(cam)
            return 0
        if "--watch" in a:
            i = a.index("--watch")
            n = int(a[i + 1]) if i + 1 < len(a) and a[i + 1].isdigit() else 10
            rest = [x for x in a[i + 2:] if not x.startswith("-")] or ["akrtsp"]
            return watch(cam, rest[0], n)
        if "--lan" in a:
            return lan(cam)
        print(__doc__)
        return 2
    finally:
        cam.close()


if __name__ == "__main__":
    sys.exit(main())
