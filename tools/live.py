#!/usr/bin/env python3
"""Install and drive the camera's on-demand preview relay (go2rtc).

The relay is what puts live video inside the web page: the camera repackages its
own H.264 into HLS/fMP4 on demand and the viewer's browser does the
decoding. It runs only while a viewer is asking.

    ./live.py --install        put live.conf, run_live.sh, stop_live.sh and the
                               go2rtc binary into /mnt/camweb/live
    ./live.py --ui             upload the web UI that talks to the relay
    ./live.py --start          start the relay through the CGI (the page's path)
    ./live.py --status         what the CGI reports about the relay
    ./live.py --beat           one idle heartbeat
    ./live.py --stop           stop it now and free the camera's RAM
    ./live.py --off / --on     block/unblock the relay on the card (survives reboot)
    ./live.py --logs           supervisor log (/tmp/live.log)
    ./live.py --test           prove media flows, then prove that stopping puts
                               the camera back exactly as it was
    ./live.py --idle-test      prove the idle reaper shuts the relay down alone
    ./live.py --toggle-test    prove the block/allow switch works both ways and
                               that the relay starts again after unblocking
    ./live.py --boot-check     what the camera runs straight after a reboot
    ./live.py --reboot         reboot, wait for the UI, then --boot-check

Everything installed lives under /mnt/camweb on the SD card. Nothing is written
to /mnt/Factory and the vendor boot script is not touched: the CGI starts the
relay when a browser asks, and it never runs at boot. --test and --idle-test
check the memory settings, the stream producer process (vendor rtsp or akrtsp)
and free memory before and
after, because "the relay stopped" is only worth reporting if the camera is
really back where it started.
"""

import hashlib
import io
import json
import os
import sys
import time
import urllib.parse
import urllib.request
from ftplib import FTP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy  # noqa: E402  HOST, PORT, ftp helpers, token location
from cam import Cam  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "live"))
REMOTE = "/mnt/camweb/live"
TEXT_FILES = ["live.conf", "run_live.sh", "stop_live.sh"]
BIN_LOCAL = [
    # GO2RTC_BIN wins: whoever installs the relay can point at their own binary
    # without editing this file, which is what a shared package needs. Also
    # accepted: drop the binary in live/ next to live.conf named go2rtc-armv5,
    # so the whole install travels as one directory.
    os.environ.get("GO2RTC_BIN", "") or "/nonexistent",
    os.path.normpath(os.path.join(HERE, "..", "live", "go2rtc-armv5")),
    os.path.normpath(os.path.join(HERE, "..", "..", "anyka-sdk", "bin", "go2rtc-armv5")),
    os.path.normpath(os.path.join(HERE, "..", "..", "anyka-sdk", "out", "go2rtc-armv5")),
]
BIN_ON_CARD = "/mnt/ak-sdk/go2rtc-armv5"      # left over from the SDK's experiments
BIN_REMOTE = REMOTE + "/go2rtc"

def log(m):
    print(m, flush=True)


_TOKEN_HINTED = []


def token():
    """The camera's access token, cached by deploy.py in tools/.token.

    A fresh checkout has no such file, and every CGI call then comes back
    {"ok":0,"err":"bad token"} with no clue why. Say it once, loudly.
    """
    p = os.path.join(HERE, ".token")
    if os.path.exists(p):
        return open(p).read().strip()
    if not _TOKEN_HINTED:
        _TOKEN_HINTED.append(1)
        log("note: tools/.token is missing - run ./deploy.py first; it creates the token on the camera")
    return ""


def api(act, timeout=45):
    """Talk to the camera's CGI exactly the way the page does -- same port, same
    token, same cache-buster. Testing anything else would be testing a path the
    browser never takes."""
    url = "http://%s:%s/cgi-bin/api?act=%s&token=%s&_=%d" % (
        deploy.HOST, deploy.PORT, act, urllib.parse.quote(token()), int(time.time() * 1000))
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
    except Exception as e:
        return {"ok": 0, "err": str(e)}
    try:
        return json.loads(body)
    except Exception:
        return {"ok": 0, "err": "not JSON: %s" % body[:140]}


def relay_get(port, path, timeout=25):
    url = "http://%s:%d%s" % (deploy.HOST, port, path)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


# One shell round trip returns everything that matters about the camera's state.
# go2rtc is matched by comm (the process name), never by grepping cmdline: the
# grep would match its own command line, which is how "kill the right process"
# scripts end up killing themselves.
SNAP = (
    "awk '$1==\"MemFree:\"{print \"free_kb=\"$2} $1==\"MemAvailable:\"{print \"avail_kb=\"$2} "
    "$1==\"Cached:\"{print \"cached_kb=\"$2} $1==\"Buffers:\"{print \"buf_kb=\"$2} "
    "$1==\"SReclaimable:\"{print \"slabr_kb=\"$2}' /proc/meminfo; "
    "echo mfk=$(cat /proc/sys/vm/min_free_kbytes 2>/dev/null); "
    "echo oc=$(cat /proc/sys/vm/overcommit_memory 2>/dev/null); "
    "for d in /proc/[0-9]*; do c=$(cat $d/comm 2>/dev/null); "
    "case \"$c\" in rtsp|akrtsp) echo \"rtsp=${d#/proc/}-$c:$(cat $d/oom_score_adj 2>/dev/null)\";; esac; "
    "if [ \"$c\" = \"go2rtc\" ]; then echo \"g2=${d#/proc/}:$(awk '{print $2*4}' $d/statm 2>/dev/null):$(cat $d/oom_score_adj 2>/dev/null)\"; fi; "
    "done; "
    "awk 'NR>1 && $4==\"0A\" {print \"port=\" $2}' /proc/net/tcp"
)


def snap(cam):
    out = cam.run(SNAP, timeout=25)
    st = {"free_kb": 0, "avail_kb": 0, "oc": "?", "g2": [], "rtsp": [], "ports": set(), "raw": out}
    mem = {}
    for line in out.splitlines():
        line = line.strip().replace("\r", "")
        if line.startswith("free_kb=") or line.startswith("avail_kb=") or line.startswith("cached_kb=") \
                or line.startswith("buf_kb=") or line.startswith("slabr_kb=") or line.startswith("mfk="):
            k, v = line.split("=", 1)
            try:
                mem[k] = int(v)
            except ValueError:
                pass
        elif line.startswith("oc="):
            st["oc"] = line[3:].strip()
        elif line.startswith("g2="):
            st["g2"].append(line[3:])
        elif line.startswith("rtsp="):
            st["rtsp"].append(line[5:])
        elif line.startswith("port="):
            hp = line[5:].split(":")
            if len(hp) == 2:
                try:
                    st["ports"].add(int(hp[1], 16))
                except ValueError:
                    pass
    st["free_kb"] = mem.get("free_kb", 0)
    # Same estimate the camera's own guard uses: MemAvailable if this kernel
    # reports it, otherwise free + page cache + buffers + reclaimable slab less
    # the kernel watermark. MemFree on its own is misleading here (~2.7 MB of 36).
    if mem.get("avail_kb", 0) > 0:
        st["avail_kb"] = mem["avail_kb"]
    else:
        st["avail_kb"] = max(0, st["free_kb"] + mem.get("cached_kb", 0) + mem.get("buf_kb", 0)
                             + mem.get("slabr_kb", 0) - mem.get("mfk", 0))
    return st


def show_snap(tag, st):
    log("  %-12s free %5.1f MB  available %5.1f MB  overcommit %s  go2rtc %-6s ports %s" % (
        tag, st["free_kb"] / 1024.0, st["avail_kb"] / 1024.0, st["oc"],
        ",".join(g.split(":")[0] for g in st["g2"]) or "none",
        " ".join(str(p) for p in sorted(st["ports"]) if p in (554, 1984, 8554, 8080)) or "-"))



def set_conf_key(cam, key, value):
    """Change one key in the relay's live.conf without re-uploading the whole
    file, so a test can shorten the idle timeout and put it back."""
    cur = deploy.ftp_get(REMOTE + "/live.conf")
    if cur is None:
        log("cannot read %s/live.conf - run --install first" % REMOTE)
        return None
    lines = cur.decode("utf-8", "replace").splitlines()
    done = False
    for i, ln in enumerate(lines):
        if ln.strip().startswith(key + "="):
            lines[i] = "%s=%s" % (key, value)
            done = True
    if not done:
        lines.append("%s=%s" % (key, value))
    if not deploy.ftp_put(REMOTE + "/live.conf", ("\n".join(lines) + "\n").encode()):
        log("could not write %s/live.conf" % REMOTE)
        return None
    back = deploy.ftp_get(REMOTE + "/live.conf").decode("utf-8", "replace")
    for ln in back.splitlines():
        if ln.strip().startswith(key + "="):
            return ln.strip()
    return None


def md5_of_bytes(b):
    return hashlib.md5(b).hexdigest()


def http_get(path, timeout=15):
    url = "http://%s:%s%s" % (deploy.HOST, deploy.PORT, path)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


def do_ui(cam):
    """Upload index.html, app.js and cgi-bin/api, then verify them the way a
    browser would: fetch them back over HTTP and ask the CGI a question.

    This path deliberately does not need a telnet session. The docroot and the
    CGI's 755 already exist, so FTP is enough; and busybox telnetd keeps
    half-open sessions from killed clients for a long time, which used to make a
    simple page upload fail with 'login failed' on a camera that was otherwise
    perfectly healthy. --install still uses the shell because it has to mkdir,
    chmod and md5sum on the card."""
    c = Checks()
    log("uploading the web UI (index.html, app.js, cgi-bin/api):")
    for rel in ["index.html", "app.js", "cgi-bin/api"]:
        with open(os.path.join(deploy.SRC, rel), "rb") as fh:
            data = fh.read()
        if not deploy.ftp_put("%s/%s" % (deploy.WWW, rel), data):
            c.add(rel, False, "FTP STOR failed")
            continue
        back = deploy.ftp_get("%s/%s" % (deploy.WWW, rel))
        c.add(rel, back is not None and md5_of_bytes(back) == md5_of_bytes(data),
              "%d bytes, md5 %s" % (len(data), md5_of_bytes(data)[:12]))

    page = http_get("/")
    c.add("page served on :%s" % deploy.PORT, page is not None and b"Anyka Cam" in page,
          "" if page else "no response")
    if page is not None:
        ver = ""
        for tok in (b"ui v1", b"ui v2", b"ui v3", b"ui v4", b"ui v5", b"ui v6", b"ui v7", b"ui v8", b"ui v9"):
            if ver:
                break
            for v in (tok + b"0", tok + b"1", tok):
                if v in page:
                    ver = v.decode()
                    break
        c.add("served page carries live view markup", b"lvbtn" in page and b"lv" in page, "version marker %s" % (ver or "?"))
    j = api("live_status", timeout=20)
    c.add("deployed CGI answers live_status", j.get("ok") == 1, json.dumps(j)[:150])
    c.add("deployed CGI has live_start", api("live_start", timeout=30).get("ok") == 1)
    return c.summary()



class Checks:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=""):
        self.rows.append((name, bool(ok)))
        log("  %-40s %s  %s" % (name, "PASS" if ok else "FAIL", str(detail)[:110]))

    def summary(self):
        bad = [r for r in self.rows if not r[1]]
        log("\n%d/%d checks passed" % (len(self.rows) - len(bad), len(self.rows)))
        return 0 if not bad else 1


def remote_md5(cam, path):
    out = cam.run("md5sum %s 2>/dev/null" % path, timeout=60)
    for tok in out.split():
        if len(tok) == 32 and all(c in "0123456789abcdef" for c in tok):
            return tok
    return ""


def do_install(cam):
    """Put the relay under /mnt/camweb on the card. No /mnt/Factory writes, and
    no boot-script edits: the relay is started on demand by the CGI."""
    log("camera %s   UI :%s   relay dir %s" % (deploy.HOST, deploy.PORT, REMOTE))
    cam.run("mkdir -p %s" % REMOTE)
    bad = 0

    ftp = FTP()
    ftp.connect(deploy.HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    for rel in TEXT_FILES:
        with open(os.path.join(SRC, rel), "rb") as fh:
            data = fh.read().replace(b"\r\n", b"\n")   # CRLF would break /bin/sh
        ftp.storbinary("STOR %s/%s" % (REMOTE, rel), io.BytesIO(data))
        got = ftp.size("%s/%s" % (REMOTE, rel))
        ok = got == len(data)
        bad += 0 if ok else 1
        log("  %-13s %7d bytes  %s" % (rel, got or -1, "ok" if ok else "MISMATCH want %d" % len(data)))
    ftp.quit()

    cam.run("chmod 755 %s/*.sh 2>/dev/null" % REMOTE)
    for rel in TEXT_FILES:
        if not rel.endswith(".sh"):
            continue
        out = cam.run("sh -n %s/%s && echo SYNTAX_OK" % (REMOTE, rel), timeout=15).strip()
        good = "SYNTAX_OK" in out
        bad += 0 if good else 1
        log("  %-13s %s" % (rel, "parses" if good else "SYNTAX ERROR " + out[:200]))

    # The binary is 20 MB. If the card already holds an identical copy (the SDK's
    # experiments left one at /mnt/ak-sdk), copy it on the card rather than
    # pushing 20 MB through the camera's FTP server. md5 decides; size only
    # decides whether the md5 has to be spent at all.
    local = next((p for p in BIN_LOCAL if os.path.exists(p)), None)
    want = None
    if local:
        with open(local, "rb") as fh:
            want = fh.read()
    want_md5 = md5_of_bytes(want) if want else None
    have_md5 = remote_md5(cam, BIN_REMOTE)
    card_md5 = remote_md5(cam, BIN_ON_CARD) if "go2rtc-armv5" in cam.run("ls %s 2>/dev/null" % BIN_ON_CARD) else ""

    if want_md5 and have_md5 == want_md5:
        log("  %-13s already on the card, md5 matches - not moved" % "go2rtc")
    elif want_md5 and card_md5 == want_md5:
        log("  %-13s copying from %s on the card (no 20 MB FTP push)" % ("go2rtc", BIN_ON_CARD))
        cam.run("cp %s %s && chmod 755 %s" % (BIN_ON_CARD, BIN_REMOTE, BIN_REMOTE), timeout=120)
        if remote_md5(cam, BIN_REMOTE) != want_md5:
            log("  %-13s copy did not verify" % "go2rtc")
            bad += 1
    elif want:
        log("  %-13s uploading %d bytes over FTP (this takes a while)" % ("go2rtc", len(want)))
        t0 = time.time()
        ftp = FTP()
        ftp.connect(deploy.HOST, 21, timeout=30)
        ftp.login("root", "anyka")
        ftp.storbinary("STOR %s" % BIN_REMOTE, io.BytesIO(want))
        ftp.quit()
        cam.run("chmod 755 %s" % BIN_REMOTE)
        got = remote_md5(cam, BIN_REMOTE)
        log("  %-13s %d s, md5 %s" % ("go2rtc", time.time() - t0, "matches" if got == want_md5 else "DIFFERS"))
        bad += 0 if got == want_md5 else 1
    elif have_md5:
        log("  %-13s no local build; keeping the copy already on the card (md5 %s)" % ("go2rtc", have_md5[:12]))
    else:
        log("  %-13s MISSING - put a linux/arm go2rtc at live/go2rtc-armv5, or set "
            "GO2RTC_BIN=/path/to/go2rtc, and re-run (see INSTALL.md)" % "go2rtc")
        bad += 1

    elf = cam.run("od -An -tx1 -N4 %s 2>/dev/null" % BIN_REMOTE).strip().replace("\r", "")
    log("  ELF header: %s   (want '7f 45 4c 46')" % elf)
    if "7f 45 4c 46" not in elf:
        bad += 1
    log(cam.run("ls -la %s" % REMOTE).strip())
    if cam.run("grep -c 'live_' %s/cgi-bin/api 2>/dev/null" % deploy.WWW).strip() in ("", "0"):
        log("\nthe deployed CGI has no live_* endpoints yet - run:  ./live.py --ui")
    return 0 if bad == 0 else 1


def do_control(act, holder):
    cam = holder["cam"] if isinstance(holder, dict) else holder
    j = api(act, timeout=75 if act == "live_start" else 25)
    if not j.get("ok"):
        log("%s -> %s" % (act, json.dumps(j)[:200]))
        if cam is not None:
            show_snap("device", snap(cam))
        return 1
    log("%s -> running=%s state=%s rss=%s kB avail=%s kB" % (
        act, j.get("running"), j.get("state"), j.get("rss_kb"), j.get("avail_kb")))
    log("     src=%s" % j.get("src"))
    if j.get("page"):
        log("     relay UI   %s" % j.get("page"))
    if j.get("rtsp_out"):
        log("     rtsp out   %s" % j.get("rtsp_out"))
    if cam is not None:
        show_snap("device", snap(cam))
    return 0



def do_boot_check(cam):
    """What the camera should look like with nobody watching: the vendor stack
    and our httpd, and nothing of ours besides files on the card."""
    c = Checks()
    s = snap(cam)
    show_snap("boot", s)
    c.add("vendor rtsp process alive", len(s["rtsp"]) > 0, " ".join(s["rtsp"]))
    c.add("vendor :554 listening", 554 in s["ports"])
    c.add("our UI :8080 listening", 8080 in s["ports"])
    c.add("no relay running at boot", not s["g2"], "go2rtc procs: %d" % len(s["g2"]))
    c.add("no relay ports at boot", 1984 not in s["ports"] and 8554 not in s["ports"], str(sorted(s["ports"])))
    c.add("overcommit at the vendor default", s["oc"] in ("0", "2"), "overcommit_memory=%s" % s["oc"])
    files = cam.run("ls %s 2>/dev/null" % REMOTE)
    c.add("relay installed on the card", all(f in files for f in TEXT_FILES + ["go2rtc"]), " ".join(files.split()))
    j = api("live_status")
    c.add("CGI answers live_status", j.get("ok") == 1, json.dumps(j)[:150])
    c.add("relay idle, not running", bool(j.get("ok")) and not j.get("running"),
          "state=%s off=%s" % (j.get("state"), j.get("off")))
    return c.summary()


def do_test(cam):
    """The whole claim, end to end: press start through the CGI, prove real video
    bytes arrive, then prove the camera is back exactly where it was."""
    c = Checks()
    before = snap(cam)
    show_snap("before", before)
    c.add("nothing of ours running before", not before["g2"], "go2rtc procs: %d" % len(before["g2"]))
    c.add("no relay ports before", 1984 not in before["ports"] and 8554 not in before["ports"])
    c.add("overcommit at vendor default before", before["oc"] in ("0", "2"), "overcommit_memory=%s" % before["oc"])

    t0 = time.time()
    j = api("live_start", timeout=30)
    said = int((time.time() - t0) * 1000)
    if not j.get("ok"):
        c.add("live_start over the CGI", False, json.dumps(j)[:150])
        return c.summary()
    # live_start must answer at once and let the caller poll. A blocking start
    # holds this single-CPU httpd for tens of seconds and starves the page.
    c.add("live_start answered immediately", said < 4000 and j.get("started") == 1,
          "%d ms, started=%s, supervisor pid %s" % (said, j.get("started"), j.get("sup")))

    running, waited = False, 0
    while waited < 45:
        time.sleep(3)
        waited += 3
        api("live_beat", timeout=10)
        s = api("live_status", timeout=20)
        if s.get("running") == 1:
            running = True
            j = s
            break
    c.add("relay came up when polled", running, "running after %d s, state=%s" % (waited, j.get("state")))
    c.add("relay running", j.get("running") == 1, "pid=%s state=%s" % (j.get("pid"), j.get("state")))
    c.add("relay RSS plausible", 3000 < int(j.get("rss_kb") or 0) < 20000, "%s kB" % j.get("rss_kb"))


    s1 = snap(cam)
    show_snap("streaming", s1)
    c.add("overcommit raised while relaying", s1["oc"] == "1", "overcommit_memory=%s" % s1["oc"])
    c.add("go2rtc marked the preferred OOM victim", any(g.endswith(":1000") for g in s1["g2"]), " ".join(s1["g2"]))
    c.add("vendor rtsp oom-protected while relaying", any(a.endswith(":-500") for a in s1["rtsp"]), " ".join(s1["rtsp"]))
    c.add("relay ports :1984 and :8554 open", 1984 in s1["ports"] and 8554 in s1["ports"], str(sorted(s1["ports"])))

    try:
        info = relay_get(1984, "/api/streams").decode("utf-8", "replace")
        c.add("relay has the cam producer", '"cam"' in info, info[:100].replace("\n", " "))
    except Exception as e:
        c.add("relay has the cam producer", False, str(e))
    try:
        m3u = relay_get(1984, "/api/stream.m3u8?src=cam").decode("utf-8", "replace")
        line = [l for l in m3u.splitlines() if "CODEC" in l]
        c.add("HLS advertises a codec", "CODECS=" in m3u, line[:1])
    except Exception as e:
        c.add("HLS advertises a codec", False, str(e))

    got, boxes, beat_at = 0, [], time.time()
    try:
        req = urllib.request.urlopen("http://%s:1984/api/stream.mp4?src=cam" % deploy.HOST, timeout=30)
        end, buf = time.time() + 12, b""
        while time.time() < end:
            chunk = req.read(8192)
            if not chunk:
                break
            got += len(chunk)
            buf += chunk
            if time.time() - beat_at > 5:
                api("live_beat", timeout=10)   # the page beats too; keep it alive
                beat_at = time.time()
        req.close()
        boxes = ["%s:%d" % (b.decode(), buf.count(b)) for b in (b"ftyp", b"moov", b"moof", b"mdat")]
        c.add("fragmented MP4 flows to a viewer", got > 100000, "%d bytes in 12 s, boxes %s" % (got, " ".join(boxes)))
    except Exception as e:
        c.add("fragmented MP4 flows to a viewer", False, str(e))

    mid = snap(cam)
    c.add("vendor rtsp alive while relaying", len(mid["rtsp"]) > 0, " ".join(mid["rtsp"]))
    c.add("available memory kept a margin", mid["avail_kb"] > before["avail_kb"] - 17000,
          "available %.1f MB (was %.1f), free %.1f MB" % (
              mid["avail_kb"] / 1024.0, before["avail_kb"] / 1024.0, mid["free_kb"] / 1024.0))


    j = api("live_stop", timeout=45)
    c.add("live_stop", bool(j.get("ok")), "state=%s" % j.get("state"))
    time.sleep(3)
    after = snap(cam)
    show_snap("after", after)
    c.add("relay process gone", not after["g2"], "go2rtc procs: %d" % len(after["g2"]))
    c.add("relay ports closed", 1984 not in after["ports"] and 8554 not in after["ports"], str(sorted(after["ports"])))
    c.add("overcommit_memory restored", after["oc"] == before["oc"], "overcommit_memory=%s" % after["oc"])
    c.add("vendor rtsp oom_score_adj back to 0", bool(after["rtsp"]) and all(a.endswith(":0") for a in after["rtsp"]),
          " ".join(after["rtsp"]))
    c.add("available memory recovered", after["avail_kb"] > before["avail_kb"] - 4000,
          "available %.1f MB (was %.1f), free %.1f MB" % (
              after["avail_kb"] / 1024.0, before["avail_kb"] / 1024.0, after["free_kb"] / 1024.0))
    return c.summary()


def do_idle_test(cam):
    """Prove the on-demand part: nobody beats the timer, so the relay must go
    away by itself and leave the camera clean."""
    c = Checks()
    cur = deploy.ftp_get(REMOTE + "/live.conf")
    if cur is None:
        log("%s/live.conf missing - run --install first" % REMOTE)
        return 1
    orig = None
    for ln in cur.decode("utf-8", "replace").splitlines():
        if ln.startswith("LIVE_IDLE_S="):
            orig = ln.split("=", 1)[1].strip()
    try:
        log("shortening the idle timeout for this test: " + str(set_conf_key(cam, "LIVE_IDLE_S", "20")))
        api("live_start", timeout=30)
        running = False
        for _ in range(15):
            time.sleep(3)
            api("live_beat", timeout=10)
            if api("live_status", timeout=20).get("running") == 1:
                running = True
                break
        c.add("relay started for the idle test", running, "state=%s" % api("live_status", timeout=20).get("state"))
        api("live_beat", timeout=15)

        log("no further heartbeats; waiting for the camera to shut it down on its own...")
        gone, deadline = False, time.time() + 90
        while time.time() < deadline:
            time.sleep(10)
            s = snap(cam)
            log("   free %5.1f MB  available %5.1f MB  go2rtc %-6s  overcommit %s" % (
                s["free_kb"] / 1024.0, s["avail_kb"] / 1024.0,
                ",".join(g.split(":")[0] for g in s["g2"]) or "none", s["oc"]))
            if not s["g2"]:
                gone = True
                break
        c.add("relay shut itself down when idle", gone)
        s = snap(cam)
        c.add("overcommit restored after idle stop", s["oc"] == "0", "overcommit_memory=%s" % s["oc"])
        c.add("relay ports closed after idle stop", 1984 not in s["ports"] and 8554 not in s["ports"])
        c.add("vendor rtsp untouched", bool(s["rtsp"]) and all(a.endswith(":0") for a in s["rtsp"]), " ".join(s["rtsp"]))
    finally:
        log("restored: " + str(set_conf_key(cam, "LIVE_IDLE_S", orig or "300")))
    return c.summary()


def do_toggle_test(cam):
    """Prove the block/allow switch is a switch.

    This is the contract the page depends on, and it broke in a way that looked
    like a dead button: the CGI ran the whole teardown before answering, holding
    the camera's one-connection web server for ~3 s, so the page's own status
    polls queued behind the click and landed out of order on top of it. The
    switch's state was then read back out of the button's label and used to
    decide what to send next - so one stale reply was enough for every later
    click to ask for the state the camera was already in.

    What must hold, from the API alone (the page is covered by tools/switch-check.mjs):
      - live_off creates the flag and live_on removes it, every time, in both
        directions, and each reply reports the state it just wrote;
      - the reply goes out BEFORE the teardown finishes (it still sees the relay
        running). That ordering is the fix: held up by the teardown, the CGI kept
        the camera's one-connection web server for seconds and the page's own
        status polls came back over the click out of order;
      - the backgrounded teardown does stop the relay a moment later;
      - a blocked relay refuses live_start, and the refusal says how to unblock it;
      - after live_on the relay starts again - the reported bug was that it never
        could;
      - a stop does not kill a relay started after it, and does not take its lock
        or pidfiles away: the teardown now runs in the background, so it may only
        touch what it saw when it started.
    """
    c = Checks()
    was_off = api("live_status", timeout=20).get("off") == 1

    def flag():
        return "PRESENT" in cam.run(
            "ls /mnt/camweb/live/disable >/dev/null 2>&1 && echo PRESENT || echo ABSENT")

    def timed(act):
        t0 = time.time()
        j = api(act, timeout=30)
        return j, int((time.time() - t0) * 1000)

    def up(timeout=40):
        for _ in range(timeout // 4):
            time.sleep(4)
            if api("live_status", timeout=20).get("running") == 1:
                return True
        return False

    try:
        api("live_on", timeout=30)
        time.sleep(2)
        c.add("starts allowed", api("live_status", timeout=20).get("off") == 0 and not flag(),
              "flag=%s" % flag())

        api("live_start", timeout=30)
        c.add("relay starts when allowed", up(), "state=%s" % api("live_status", timeout=20).get("state"))

        # Timing cannot be judged against an absolute budget: this box's baseline
        # swings from ~1 s idle to ~9 s with the relay running at load 15, and the
        # old bug (3.3 s) would hide inside that. What is deterministic is the
        # ORDER: the flag is written and the reply printed before the teardown is
        # forked, so the reply must still see the relay running. When the teardown
        # ran inside the request, the reply always reported running=0 - the relay
        # was already gone by the time the page heard anything.
        j, ms = timed("live_off")
        c.add("live_off writes the flag before answering", j.get("ok") == 1 and j.get("off") == 1,
              "ok=%s off=%s blocked=%s" % (j.get("ok"), j.get("off"), j.get("blocked")))
        c.add("live_off answers while the teardown is still running", j.get("running") == 1,
              "reply said running=%s (0 means the request waited for the teardown), %d ms" % (j.get("running"), ms))
        time.sleep(1)
        c.add("live_off wrote the flag on the card", flag(), "flag=%s" % flag())

        refused = api("live_start", timeout=30)
        c.add("blocked relay refuses to start", refused.get("ok") == 0,
              "err=%s" % str(refused.get("err"))[:70])
        c.add("refusal names the switch, not a shell command",
              "button" in str(refused.get("err", "")), str(refused.get("err"))[:70])

        # The teardown was left to run in the background; it still has to happen.
        gone, waited = False, 0
        while waited < 40:
            time.sleep(4); waited += 4
            if api("live_status", timeout=20).get("running") != 1:
                gone = True
                break
        c.add("the backgrounded teardown really stops the relay", gone, "after %d s" % waited)

        j, ms = timed("live_on")
        c.add("live_on reports allowed in its own reply", j.get("ok") == 1 and j.get("off") == 0,
              "ok=%s off=%s blocked=%s in %d ms" % (j.get("ok"), j.get("off"), j.get("blocked"), ms))
        time.sleep(1)
        c.add("live_on removed the flag", not flag(), "flag=%s" % flag())

        api("live_start", timeout=30)
        c.add("relay starts again after unblocking (the reported bug)", up(),
              "state=%s" % api("live_status", timeout=20).get("state"))

        # The teardown is backgrounded now, so it must not reach past its own
        # start: a viewer who stops and immediately starts must keep the relay.
        # The window is generous because go2rtc on a loaded ARM9 core can take
        # tens of seconds to bind; what is being tested is that the old stopper
        # neither kills the new supervisor nor steals its lock.
        api("live_stop", timeout=30)
        time.sleep(1)
        api("live_start", timeout=30)
        back = up(80)
        s = api("live_status", timeout=20)
        c.add("a stop does not kill a relay started after it",
              back and s.get("running") == 1 and s.get("sup"),
              "running=%s sup=%s state=%s" % (s.get("running"), s.get("sup"), s.get("state")))
        sup = str(s.get("sup") or "")
        c.add("the new supervisor still owns its lock and pidfiles",
              sup.isdigit() and "ALIVE" in cam.run(
                  "[ -d /proc/%s ] && echo ALIVE || echo GONE" % sup),
              "supervisor pid=%s" % sup)

        for act, want in (("live_off", 1), ("live_on", 0), ("live_off", 1), ("live_on", 0)):
            j = api(act, timeout=30)
            time.sleep(1)
            c.add("%s toggles (%s)" % (act, "blocked" if want else "allowed"),
                  j.get("off") == want and flag() == bool(want),
                  "reply off=%s flag=%s" % (j.get("off"), flag()))

        st = api("status", timeout=20)
        c.add("status reports the same field as the live_* replies",
              st.get("live_off") == api("live_status", timeout=20).get("off"),
              "status live_off=%s" % st.get("live_off"))
    finally:
        api("live_stop", timeout=45)
        time.sleep(3)
        if was_off:
            api("live_off", timeout=30)
        log("left as found: off=%s flag=%s" % (api("live_status", timeout=20).get("off"), flag()))
    return c.summary()


def supervisor_pid(cam):
    """The supervisor's recorded PID, from the lock directory that owns it, with
    a zombie check. Walking /proc with a grep per entry would mean ~90 process
    forks on a single-core ARM5 box -- exactly the load this project avoids --
    and a stale pidfile alone can name a corpse: an unreaped process keeps its
    /proc entry, so existence proves nothing."""
    out = cam.run(
        "p=$(cat /tmp/camweb_live.lock/pid 2>/dev/null); "
        "[ -n \"$p\" ] || p=$(cat /tmp/camweb_live.pid 2>/dev/null); "
        "if [ -n \"$p\" ] && [ -d /proc/$p ]; then "
        "t=$(cut -d. -f1 /proc/uptime 2>/dev/null); h=$(cat /tmp/camweb_live.lock/hb 2>/dev/null); "
        "case \"$h\" in ''|*[!0-9]*) h=0 ;; esac; "
        "echo \"pid=$p state=$(awk '{print $3}' /proc/$p/stat 2>/dev/null) hb_age=$((t - h))\"; "
        "tr '\\000' ' ' < /proc/$p/cmdline 2>/dev/null; echo; else echo 'none'; fi", timeout=20)
    out = out.strip().replace("\r", "")
    if "pid=" not in out:
        return 0
    pid = out.split("pid=", 1)[1].split()[0]
    if not pid.isdigit():
        return 0
    if " state=Z" in out:
        return -int(pid)          # a corpse nobody reaped
    age = out.split("hb_age=")[1].split()[0] if "hb_age=" in out else "0"
    if int(age) >= 90:
        return -int(pid)          # alive but not beating: wedged, and its lock lies
    # No cmdline check here, and that is not an oversight: busybox ash leaves
    # /proc/<pid>/cmdline empty, so asking "is this our script?" through cmdline
    # answers "no" for a perfectly healthy supervisor. The proof of identity is
    # that this PID owns the lock and is still beating on it; a reused PID would
    # not be writing our heartbeat file.
    return int(pid)




def do_race_test(cam):
    """Concurrent live_start, the way a page plus a second tab plus a retry
    timer can hit the CGI at once. Two supervisors used to start that way: both
    read an empty pidfile, both went ahead, the second failed to bind, and the
    two of them ate enough memory that the guard killed everything. Afterwards
    there may be exactly one supervisor and one relay -- and the page must still
    get answers while all of this is happening.

    The timing tolerance is wide on purpose. This box idles near load 7 on one
    ARM9 core, and every CGI is a forked shell running awk; four concurrent
    starts genuinely take ~10 s wall clock each. What must not happen is the
    page being unable to ask anything at all."""
    import threading
    c = Checks()
    api("live_stop", timeout=45)
    time.sleep(3)
    log("firing 4 live_start requests at once, and probing the page meanwhile:")
    answers = []
    probe = []

    def fire():
        t0 = time.time()
        j = api("live_start", timeout=60)
        j["_ms"] = int((time.time() - t0) * 1000)
        answers.append(j)

    def ask():
        t0 = time.time()
        j = api("status", timeout=30)
        probe.append((int((time.time() - t0) * 1000), j.get("ok") == 1))

    th = [threading.Thread(target=fire) for _ in range(4)]
    t0 = time.time()
    for t in th:
        t.start()
    time.sleep(2)
    ask()                      # can the page still ask anything during a race?
    for t in th:
        t.join()
    log("  all four answered in %d s" % int(time.time() - t0))
    for a in answers:
        log("   ok=%s started=%s supervisor=%s in %d ms" % (a.get("ok"), a.get("started"), a.get("sup"), a.get("_ms")))
    c.add("all four answered ok", all(a.get("ok") == 1 for a in answers), "ok=%s" % [a.get("ok") for a in answers])
    c.add("page still answered during the race", probe and probe[0][1] and probe[0][0] < 15000,
          "status in %d ms" % (probe[0][0] if probe else -1))
    # "started" is per-request, and three of the four requests answered before
    # the winning supervisor had even written its lock, so an empty supervisor
    # field there is honest, not wrong. What matters is that no two requests
    # name two different supervisors.
    named = set(str(a.get("sup")) for a in answers if a.get("sup"))
    c.add("no two requests name different supervisors", len(named) <= 1, "named=%s" % sorted(named))


    # Then let it come up, the way the page does: poll, do not block.
    running, waited = False, 0
    while waited < 60:
        time.sleep(3)
        waited += 3
        j = api("live_status", timeout=20)
        if j.get("running") == 1:
            running = True
            break
    c.add("relay came up under the race", running, "running after %d s" % waited)

    s = snap(cam)
    show_snap("after race", s)
    c.add("exactly one relay process", len(s["g2"]) == 1, " ".join(s["g2"]))
    pid = supervisor_pid(cam)
    c.add("exactly one live supervisor (by PID)", pid > 0, "pid=%s (negative = zombie or wrong process)" % pid)
    api("live_stop", timeout=45)
    time.sleep(2)
    c.add("supervisor gone after stop", supervisor_pid(cam) == 0, "pid=%s" % supervisor_pid(cam))
    return c.summary()




def do_reboot(cam):

    log("rebooting %s (the camera is offline for about a minute)" % deploy.HOST)
    cam.run("reboot", timeout=10)
    log("waiting for the UI to answer again...")
    t0 = time.time()
    while time.time() - t0 < 240:
        time.sleep(5)
        try:
            with urllib.request.urlopen("http://%s:%s/" % (deploy.HOST, deploy.PORT), timeout=4) as r:
                body = r.read(600).decode("utf-8", "replace")
            if "Anyka Cam" in body:
                log("  UI is back after %d s" % int(time.time() - t0))
                break
        except Exception:
            pass
    else:
        log("the UI never came back within 240 s - check the camera directly")
        return 1
    time.sleep(40)   # config.sh starts the vendor rtsp 30 s into boot
    cam2 = Cam(deploy.HOST)
    try:
        log("boot state:")
        return do_boot_check(cam2)
    finally:
        cam2.close()


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        print(__doc__)
        return 2
    holder = {"cam": None}

    def shell():
        """The telnet shell is created only for commands that need one. busybox
        telnetd holds half-open sessions from killed clients for a long time, so
        a page upload or a status check should never fail because a shell could
        not be opened."""
        if holder["cam"] is None:
            holder["cam"] = Cam(deploy.HOST)
        return holder["cam"]

    try:
        if "--install" in args:
            return do_install(shell())
        if "--ui" in args:
            return do_ui(None)
        for flag, act in (("--start", "live_start"), ("--stop", "live_stop"), ("--status", "live_status"),
                          ("--beat", "live_beat"), ("--off", "live_off"), ("--on", "live_on")):
            if flag in args:
                return do_control(act, holder)
        if "--logs" in args:
            log(shell().run("tail -n 40 /tmp/live.log 2>/dev/null"))
            return 0
        if "--test" in args:
            return do_test(shell())
        if "--idle-test" in args:
            return do_idle_test(shell())
        if "--race-test" in args:
            return do_race_test(shell())
        if "--toggle-test" in args:
            return do_toggle_test(shell())
        if "--boot-check" in args:
            return do_boot_check(shell())
        if "--reboot" in args:
            return do_reboot(shell())
        print(__doc__)
        return 2
    finally:
        if holder["cam"] is not None:
            holder["cam"].close()



if __name__ == "__main__":
    sys.exit(main())






