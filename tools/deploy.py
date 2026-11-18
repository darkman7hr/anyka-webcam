#!/usr/bin/env python3
"""Deploy the replacement web UI to the Anyka camera.

By default this runs ours on a side port, beside the stock Factory interface, so
trying it costs nothing. `--promote` is the deliberate step that gives ours the
stock port instead, after backing up both the stock page and config.sh on the
card. `--revert` puts the stock interface back.

    ./deploy.py             upload, and start on the current port if not up
    ./deploy.py --status    listeners + what the API reports
    ./deploy.py --verify    endpoint checks, incl. injection attempts and JSON
    ./deploy.py --preview=URL   point the preview at a JPEG source (see relay.py)
    ./deploy.py --promote   back up the stock UI, take over :8080, start a runner
    ./deploy.py --revert    restore config.sh and put the stock UI back on :8080
    ./deploy.py --stop      stop only OUR httpd (never the stock one)

The port ours answers on is remembered in tools/.port so every command knows
where to look after a promote or revert. CAM_HTTP_PORT overrides it.

busybox httpd daemonises on its own, so it is started without `&`; backgrounding
it makes ash print job-control messages into the shell we share.
"""

import io
import os
import random
import string
import sys
import time
import urllib.parse
import urllib.request
from ftplib import FTP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cam import Cam  # noqa: E402

HOST = os.environ.get("CAM_HOST", "192.168.10.46")
SRC = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "webui"))
ROOT = "/mnt/camweb"
WWW = ROOT + "/www"
BB = "/mnt/Factory/custom/busybox"
CFG = "/mnt/Factory/config.sh"
BACKUP = ROOT + "/backup"
STOCK_DOCROOT = "/mnt/Factory/custom/web_interface/www"
RUNWEB = ROOT + "/run_web.sh"
STOCK_HTTPD_LINE = "%s httpd -p 8080 -h %s &" % (BB, STOCK_DOCROOT)

# Which port ours answers on changes when the UI is promoted onto the stock
# port, and every command below has to know. Kept in a file rather than in the
# source so --promote and --revert leave the tool pointed at the right place.
HERE = os.path.dirname(os.path.abspath(__file__))
PORTFILE = os.path.join(HERE, ".port")


def saved_port():
    try:
        p = open(PORTFILE).read().strip()
        return p or "8090"
    except Exception:
        return "8090"


def set_port(p):
    with open(PORTFILE, "w") as fh:
        fh.write(p)


def guess_port():
    """No tools/.port - a fresh checkout, or a different machine. Ask the camera.

    The stock Factory page answers on 8080 too, so "something answered" proves
    nothing: our own page title decides. Without this, every command from a fresh
    extraction defaults to :8090 and reports connection refused on a camera that
    is working fine on :8080.
    """
    for p in ("8080", "8090"):
        try:
            r = urllib.request.urlopen("http://%s:%s/" % (HOST, p), timeout=4)
            body = r.read(600).decode("utf-8", "replace")
            r.close()
        except Exception:
            continue
        if "Anyka Cam" in body:
            print("port:    no tools/.port, probed the camera - ours answers on %s" % p, flush=True)
            return p
    return "8090"


PORT = os.environ.get("CAM_HTTP_PORT") or (
    saved_port() if os.path.exists(PORTFILE) else guess_port())
BASE = "http://%s:%s" % (HOST, PORT)
TOKENFILE = os.path.join(HERE, ".token")
FILES = ["index.html", "app.js", "cgi-bin/api"]


def log(m):
    print(m, flush=True)


def http(path, token=None, timeout=12):
    url = "http://%s:%s%s" % (HOST, PORT, path)
    if token:
        url += ("&" if "?" in url else "?") + "token=" + urllib.parse.quote(token)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return '{"ok":0,"err":"%s"}' % e


def ftp_get(path):
    """Read a file off the camera byte for byte. Used for config.sh, where a
    shell round trip could mangle quoting or line endings."""
    ftp = FTP()
    ftp.connect(HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    buf = []
    try:
        ftp.retrbinary("RETR %s" % path, buf.append)
    except Exception:
        ftp.quit()
        return None
    ftp.quit()
    return b"".join(buf)


def ftp_put(path, data):
    ftp = FTP()
    ftp.connect(HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    try:
        ftp.storbinary("STOR %s" % path, io.BytesIO(data))
        got = ftp.size(path)
    except Exception:
        ftp.quit()
        return False
    ftp.quit()
    return got == len(data)


# The PID column of netstat comes back empty on this kernel, so processes are
# found under /proc. Two things are checked: the process must be named
# httpd/busybox, and its command line must mention OUR docroot. Docroot rather
# than port, because after promotion ours is on 8080 -- the same port the stock
# one used -- and the only thing that tells them apart is the path. Without the
# name test the grep children of the loop match "httpd" in their own command
# lines and the loop tries to kill them. `killall httpd` is forbidden: it would
# take the stock interface down with it, which is exactly what --revert needs
# to keep alive.
KILL_OURS = (
    "[ -f /tmp/camweb_web.pid ] && kill $(cat /tmp/camweb_web.pid) 2>/dev/null; "
    "for d in /proc/[0-9]*; do "
    "c=$(cat $d/comm 2>/dev/null); "
    "case \"$c\" in *httpd*|*busybox*) "
    "if grep -qa 'camweb/www' $d/cmdline 2>/dev/null; then p=${d#/proc/}; echo killing $p; kill $p; fi ;; "
    "esac; done"
)

# Only ever run deliberately, from --promote/--revert, where replacing or
# restoring the stock interface is the point.
KILL_STOCK = (
    "for d in /proc/[0-9]*; do "
    "c=$(cat $d/comm 2>/dev/null); "
    "case \"$c\" in *httpd*|*busybox*) "
    "if grep -qa 'web_interface' $d/cmdline 2>/dev/null; then p=${d#/proc/}; echo killing_stock $p; kill $p; fi ;; "
    "esac; done"
)

RUNWEB_TEXT = """#!/bin/sh
# Keeps the replacement UI alive on the port this camera's interface has always
# used. Started by /mnt/Factory/config.sh at boot. httpd is run in the
# foreground (-f) inside a loop so that if it ever dies, the interface comes
# back by itself instead of leaving the camera with no UI at all -- which is how
# "I cannot access the page" happens on a box nobody supervises.
BB=/mnt/Factory/custom/busybox
WWW=/mnt/camweb/www
PORT=8080
export PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib
echo $$ > /tmp/camweb_web.pid
while :; do
  echo "$(cut -d. -f1 /proc/uptime)s starting httpd on $PORT" >> /tmp/camweb_web.log 2>/dev/null
  $BB httpd -f -p $PORT -h $WWW >> /tmp/camweb_web.log 2>/dev/null
  sleep 3
done
"""


def yes(cmd, cam):
    return "YES" in cam.run(cmd)


def upload(cam):
    cam.run("mkdir -p %s/cgi-bin %s/presets" % (WWW, ROOT))
    ftp = FTP()
    ftp.connect(HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    bad = 0
    for rel in FILES:
        src = os.path.join(SRC, rel)
        with open(src, "rb") as fh:
            ftp.storbinary("STOR %s/%s" % (WWW, rel), fh)
        got = ftp.size("%s/%s" % (WWW, rel))
        want = os.path.getsize(src)
        ok = got == want
        bad += 0 if ok else 1
        log("  %-16s %6d bytes  %s" % (rel, got or -1, "ok" if ok else "MISMATCH want %d" % want))
    ftp.quit()
    cam.run("chmod 755 %s/cgi-bin/api 2>/dev/null" % WWW)
    return bad == 0


def ensure_files(cam):
    """Create token and config once. Neither is ever overwritten, so a
    re-deploy neither logs an open browser out nor silently rotates a
    credential the user has since edited."""
    if yes("if [ -f %s/token ]; then echo YES; fi" % ROOT, cam):
        tok = cam.readfile(ROOT + "/token").strip()
        log("token:   reused the existing one")
    else:
        al = string.ascii_letters + string.digits
        tok = "".join(random.SystemRandom().choice(al) for _ in range(20))
        cam.run("printf %%s %s > %s/token" % (tok, ROOT))
        log("token:   created")

    if not yes("if [ -f %s/webui.conf ]; then echo YES; fi" % ROOT, cam):
        cam.run(
            "printf 'PAN_MIN=0\\nPAN_MAX=360\\nTILT_MIN=10\\nTILT_MAX=170\\n"
            "STEP_DEF=10\\nAUTH_EN=1\\nPREVIEW_URL=\\n' > %s/webui.conf" % ROOT
        )
        log("config:  written with PREVIEW_URL empty: this unit serves no JPEG on-board, so point it "
            "at tools/relay.py on a PC")
    else:
        log("config:  reused the existing one")
    return tok


def running(cam):
    return yes("netstat -tln 2>/dev/null | grep -q ':%s ' && echo YES" % PORT, cam)


def wait_up(seconds=25):
    """Block until the port actually accepts a connection.

    busybox httpd returns from the command line as soon as it has forked, which
    is before the child has bound the socket, and on a box that is still busy
    that gap is long enough to make every check fail with "connection refused"
    and look like a broken deployment.
    """
    import socket
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            s = socket.create_connection((HOST, int(PORT)), 1.5)
            s.close()
            return True
        except Exception:
            time.sleep(0.7)
    return False


def do_stop(cam):
    """Stop ours, plus the runner that would otherwise bring it straight back."""
    log(cam.run(KILL_OURS).strip() or "nothing to stop")
    # SIGTERM takes a moment and a socket that is still closing keeps answering,
    # so confirm the port is actually free instead of assuming it.
    for _ in range(6):
        time.sleep(1)
        if not yes("netstat -tln 2>/dev/null | grep -q ':%s ' && echo YES" % PORT, cam):
            break
    still = yes("netstat -tln 2>/dev/null | grep -q ':%s ' && echo YES" % PORT, cam)
    log("port %s: %s" % (PORT, "STILL LISTENING (runner restarted it?)" if still else "closed"))
    return 0


def do_status(cam):
    log(cam.run("netstat -tlnp 2>/dev/null | grep -E 'Proto|:(554|80|8080|8090|3000) '"))
    return 0


# Only these characters ever appear in a JPEG URL, so anything else is dropped
# rather than quoted: the value ends up inside a config file the CGI parses.
PREVIEW_OK = set(string.ascii_letters + string.digits + ":/._?=&+-~%")
CONF_KEYS = ["PAN_MIN", "PAN_MAX", "TILT_MIN", "TILT_MAX", "STEP_DEF", "AUTH_EN", "PREVIEW_URL"]


def do_preview(url):
    """Point the UI's preview at a relay. Written over FTP so no shell quoting
    is involved anywhere; the other config keys are preserved."""
    url = "".join(c for c in url.strip() if c in PREVIEW_OK)[:200]
    ftp = FTP()
    ftp.connect(HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    cur = b""
    try:
        buf = []
        ftp.retrbinary("RETR %s/webui.conf" % ROOT, buf.append)
        cur = b"".join(buf)
    except Exception:
        pass
    ftp.quit()
    if not cur.strip():
        log("could not read %s/webui.conf - run ./deploy.py first" % ROOT)
        return 1
    vals = {}
    for line in cur.decode("utf-8", "replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    vals["PREVIEW_URL"] = url
    text = "".join("%s=%s\n" % (k, vals[k]) for k in CONF_KEYS if k in vals)
    import io
    ftp = FTP()
    ftp.connect(HOST, 21, timeout=15)
    ftp.login("root", "anyka")
    ftp.storbinary("STOR %s/webui.conf" % ROOT, io.BytesIO(text.encode()))
    ftp.quit()
    log("PREVIEW_URL -> %s" % (url or "(empty, controls only)"))
    tok = open(TOKENFILE).read().strip() if os.path.exists(TOKENFILE) else ""
    log("api status now: %s" % http("/cgi-bin/api?act=status", tok)[:220])
    return 0


def do_verify(cam):
    tok = open(TOKENFILE).read().strip()
    if not wait_up():
        log("nothing is accepting :%s - run ./deploy.py first" % PORT)
        return 1
    checks = [
        ("index served", "/", None, "Anyka Cam"),
        ("app.js served", "/app.js", None, "preset_goto"),
        ("auth enforced", "/cgi-bin/api?act=status", None, "unauthorized"),
        ("status", "/cgi-bin/api?act=status", "tok", '"ok":1'),
        ("position readable", "/cgi-bin/api?act=status", "tok", '"pan":'),
        ("presets", "/cgi-bin/api?act=presets", "tok", '"presets":['),
        ("injection blocked", "/cgi-bin/api?act=goto&pan=1;reboot&tilt=2", "tok", "bad args"),
        ("daemon quit blocked", "/cgi-bin/api?act=raw&cmd=q", "tok", "not allowed"),
        ("bad token rejected", "/cgi-bin/api?act=reboot", "wrong", '"ok":0'),
    ]
    fails = 0
    for name, path, tk, expect in checks:
        token = tok if tk == "tok" else ("wrong" if tk == "wrong" else None)
        body = http(path, token)
        ok = expect in body
        fails += 0 if ok else 1
        log("  %-22s %s  %s" % (name, "PASS" if ok else "FAIL", body[:70].replace("\n", " ")))
    log("\n%d/%d passed" % (len(checks) - fails, len(checks)))

    # Substring checks cannot catch malformed JSON. An earlier build emitted
    # `...}{...` (a shell `[ 0 ]` test is true), which every browser rejected
    # while the camera kept answering "ok". So parse for real, and re-parse with
    # extra presets present: the bug only appeared once there were two or more.
    import json
    extra = 0

    def parsed(path):
        body = http(path, tok)
        try:
            return json.loads(body), None
        except Exception as e:
            return None, "%s  %s" % (e, body[:90])

    def check(name, cond, detail=""):
        nonlocal fails, extra
        extra += 1
        if cond:
            log("  %-22s PASS  %s" % (name, detail))
        else:
            fails += 1
            log("  %-22s FAIL  %s" % (name, detail))

    for s in ("7", "8"):
        http("/cgi-bin/api?act=preset_del&slot=%s&token=" % s + tok)
    j, err = parsed("/cgi-bin/api?act=presets")
    before = len(j.get("presets", [])) if j else -1
    check("presets parse", j is not None, err or "%d presets" % before)
    for s in ("7", "8"):
        http("/cgi-bin/api?act=preset_save&slot=%s&name=jscheck" % s, tok)
    j3, err = parsed("/cgi-bin/api?act=presets")
    after = len(j3.get("presets", [])) if j3 else -1
    check("presets parse, 3+ rows", j3 is not None and after == before + 2,
          err or "%d -> %d presets" % (before, after))
    for s in ("7", "8"):
        http("/cgi-bin/api?act=preset_del&slot=%s&token=" % s + tok)
    j, err = parsed("/cgi-bin/api?act=status")
    check("status parses", j is not None, err or "")
    j, err = parsed("/cgi-bin/api?act=log")
    check("log parses", j is not None, err or "%d lines" % len(j.get("lines", [])) if j else err)

    log("\n%d/%d passed" % (len(checks) + extra - fails, len(checks) + extra))
    return 1 if fails else 0


def do_promote(cam):
    """Take over :8080, the port this camera's interface has always used.

    Ordered deliberately: back up first, verify the backups exist, only then
    touch config.sh, and syntax-check the edited file before anything is
    restarted. If config.sh stops parsing it is put straight back.
    """
    global PORT
    log("promoting the replacement UI onto :8080 (stock page backed up, not deleted)")
    if not yes("if [ -f %s ]; then echo YES; fi" % CFG, cam):
        log("no %s on this camera - refusing to guess" % CFG)
        return 1

    cam.run("mkdir -p %s" % BACKUP)
    if yes("if [ -s %s/config.sh.orig ]; then echo YES; fi" % BACKUP, cam):
        log("backup:  config.sh.orig already there, left untouched")
    else:
        log("backup:  " + cam.run("cp %s %s/config.sh.orig && echo config.sh copied" % (CFG, BACKUP)).strip())
    if yes("if [ -d %s/stock_www ]; then echo YES; fi" % BACKUP, cam):
        log("backup:  stock_www already there, left untouched")
    else:
        log("backup:  " + cam.run("mkdir -p %s/stock_www && cp -r %s/* %s/stock_www/ && echo stock www copied"
                                  % (BACKUP, STOCK_DOCROOT, BACKUP)).strip())
    chk = cam.run("ls %s 2>/dev/null; echo stock_files=$(ls %s/stock_www 2>/dev/null | wc -l); df -m /mnt 2>/dev/null | tail -1"
                  % (BACKUP, BACKUP))
    log("check:   " + " | ".join(l for l in chk.splitlines() if l.strip()))
    if "config.sh.orig" not in chk:
        log("backup missing - refusing to edit config.sh")
        return 1

    if not ftp_put(RUNWEB, RUNWEB_TEXT.encode()):
        log("could not write %s" % RUNWEB)
        return 1
    cam.run("chmod 755 %s" % RUNWEB)
    log("runner:  %s written (restarts httpd if it dies)" % RUNWEB)

    orig = ftp_get(CFG)
    if orig is None:
        log("could not read %s" % CFG)
        return 1
    text = orig.decode("utf-8", "replace")
    if RUNWEB in text:
        log("config:  already promoted, leaving config.sh alone")
    elif STOCK_HTTPD_LINE in text:
        new = text.replace(
            STOCK_HTTPD_LINE,
            "# stock interface replaced by the camweb UI; original line is in %s/config.sh.orig\n"
            "/bin/sh %s &" % (BACKUP, RUNWEB))
        if not ftp_put(CFG, new.encode()):
            log("could not write %s" % CFG)
            return 1
        log("config:  stock httpd line replaced")
    else:
        log("config:  WARNING - the expected stock line was not found, so ours is appended")
        log("         expected: %s" % STOCK_HTTPD_LINE)
        new = text.rstrip("\n") + "\n\n# added by camweb deploy\n/bin/sh %s &\n" % RUNWEB
        if not ftp_put(CFG, new.encode()):
            log("could not write %s" % CFG)
            return 1

    out = cam.run("sh -n %s && echo SYNTAX_OK" % CFG)
    if "SYNTAX_OK" not in out:
        log("edited config.sh does not parse (%s) - restoring the original" % out.strip()[:120])
        ftp_put(CFG, orig)
        return 1
    log("config:  parses cleanly")

    log(cam.run(KILL_OURS).strip() or "nothing of ours to stop")
    log(cam.run(KILL_STOCK).strip() or "no stock httpd running")
    time.sleep(2)
    cam.run("setsid /bin/sh %s </dev/null >/dev/null 2>&1 &" % RUNWEB, timeout=10)
    PORT = "8080"
    set_port("8080")
    if not wait_up(30):
        log("WARNING: nothing accepting :8080 after promotion.")
        log("         the stock page is still in %s/stock_www; put it back with:" % BACKUP)
        log("         ./deploy.py --revert")
        return 1
    return promote_report()


def promote_report():
    tok = open(TOKENFILE).read().strip() if os.path.exists(TOKENFILE) else ""
    idx = http("/")
    api = http("/cgi-bin/api?act=status", tok)
    stock = http("/cgi-bin/webui")
    ok = "Anyka Cam" in idx and '"ok":1' in api
    log("")
    log("  index on :%s   %s" % (PORT, "ours, serving" if "Anyka Cam" in idx else "PROBLEM"))
    log("  api on :%s     %s" % (PORT, "working" if '"ok":1' in api else api[:90]))
    log("  stock page    %s" % ("still reachable" if "WebUI" in stock or "webui?command" in stock else "gone"))
    log("  to undo       ./deploy.py --revert   (restores config.sh and the stock page)")
    return 0 if ok else 1


def do_revert(cam):
    """Put the stock interface back on :8080 and move ours to :8090."""
    global PORT
    orig = ftp_get(BACKUP + "/config.sh.orig")
    if orig is None:
        log("no %s/config.sh.orig - cannot revert automatically" % BACKUP)
        return 1
    if not ftp_put(CFG, orig):
        log("could not restore %s" % CFG)
        return 1
    log("config.sh restored from backup")
    out = cam.run("sh -n %s && echo SYNTAX_OK" % CFG)
    log("config.sh: " + ("parses" if "SYNTAX_OK" in out else "SYNTAX PROBLEM " + out[:120]))
    log(cam.run(KILL_OURS).strip() or "nothing of ours to stop")
    time.sleep(1)
    cam.run("cd /mnt/Factory/custom && LD_LIBRARY_PATH=/mnt/Factory/custom/lib setsid %s httpd -p 8080 -h %s "
            "</dev/null >/dev/null 2>&1 &" % (BB, STOCK_DOCROOT), timeout=10)
    time.sleep(2)
    cam.run("%s httpd -p 8090 -h %s" % (BB, WWW), timeout=10)
    PORT = "8090"
    set_port("8090")
    url = "http://%s:8080/" % HOST
    try:
        body = urllib.request.urlopen(url, timeout=8).read().decode("utf-8", "replace")
    except Exception as e:
        body = "err %s" % e
    log("  stock on :8080  %s" % ("back" if "cgi-bin/webui" in body or "WebUI" in body else body[:80]))
    log("  ours on  :8090  %s" % ("up" if "Anyka Cam" in http("/") else "not up - run ./deploy.py"))
    return 0


def main():
    args = sys.argv[1:]
    cam = Cam(HOST)
    tok = ""
    try:
        if "--stop" in args:
            return do_stop(cam)
        if "--status" in args:
            return do_status(cam)
        if "--verify" in args:
            return do_verify(cam)
        if "--promote" in args:
            return do_promote(cam)
        if "--revert" in args:
            return do_revert(cam)
        for a in args:
            if a.startswith("--preview="):
                return do_preview(a.split("=", 1)[1])
        log("camera %s   port %s   docroot %s" % (HOST, PORT, WWW))
        log("uploading:")
        if not upload(cam):
            log("upload did not verify; not starting")
            return 1
        tok = ensure_files(cam)
        promoted = yes("grep -q run_web.sh %s 2>/dev/null && echo YES" % CFG, cam)
        if running(cam):
            log("httpd:   already up on %s%s" % (PORT, " (kept alive by the boot runner)" if promoted else ""))
        elif promoted:
            cam.run("setsid /bin/sh %s </dev/null >/dev/null 2>&1 &" % RUNWEB, timeout=10)
            time.sleep(3)
            log("httpd:   started through %s" % RUNWEB)
        else:
            cam.run("%s httpd -p %s -h %s" % (BB, PORT, WWW), timeout=10)
            log("httpd:   started on %s" % PORT)
        wait_up()
    finally:
        cam.close()

    if tok:
        with open(TOKENFILE, "w") as fh:
            fh.write(tok)
    body = http("/", tok or None)
    log("\n  index: %d bytes, %s" % (len(body), "ok" if "Anyka Cam" in body else "PROBLEM"))
    log("  open   %s/" % BASE)
    log("  token  %s" % tok)
    log("  stop   ./deploy.py --stop")
    return 0


if __name__ == "__main__":
    sys.exit(main())

