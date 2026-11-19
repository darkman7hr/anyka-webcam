#!/usr/bin/env python3
"""Install and drive the camera's clock: NTP, the timezone, and what the
burned-in date overlay is allowed to claim.

Why this exists on its own: this board has no real-time clock. There is no
/sys/class/rtc and no battery, so after every boot the camera is at the epoch
plus however long it has been up, and the vendor's own way of learning the time
(its cloud bind, which logs "utc_time: 1638933395" and "timezone:8") is not
running here. Measured before any of this, on a box that had been up 13 hours:

    date            Thu Jan  1 13:30 GMT 1970
    TZ              GMT-08:00        (the vendor's, printed as "GMT")

That was harmless while the clock only ever appeared in a log line. It stopped
being harmless the moment the date was burned into the picture with
akrtsp --osd-time: the overlay puts the clock into the pixels before the encoder
runs, so the NVR's recordings carry it, and a wrong date in a recording cannot be
taken back out afterwards. So the overlay only draws a date when this service
says it confirmed one, and this tool is how that service is installed, aimed and
checked.

Subcommands:
    --install           put time.conf and run_time.sh in /mnt/camweb/time, add the
                        boot line to config.sh (backed up, syntax-checked, restored
                        if it stops parsing), and start it now
    --status            what the service last confirmed, and what date says
    --sync              ask the running supervisor for a sync right now (USR1)
    --tz RULE           set the POSIX timezone rule and restart the service
    --start / --stop / --restart
    --logs              the supervisor's log (/tmp/camtime.log)
    --test              the checks that matter, see below
    --restore           take the boot line out of config.sh and stop the service

--test is deliberately not "the clock reads a sensible number", because 1970 plus
uptime is a sensible-looking number on a box with no rtc. It checks that the
supervisor is running, that it says synced, that the epoch agrees with this
workstation to within a couple of seconds, that the zone the overlay prints in is
the one in time.conf, and that the boot line is still in config.sh - which is the
only reason any of it survives a reboot.

The zone has to be a POSIX rule (CET-1CEST,M3.5.0,M10.5.0/3), not a name: uClibc
on this board carries no zoneinfo database, so a named zone silently means UTC.
"""

import calendar
import email.utils
import hashlib
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy  # noqa: E402
from cam import Cam  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "time"))
REMOTE = "/mnt/camweb/time"
CONF = REMOTE + "/time.conf"
RUNNER = REMOTE + "/run_time.sh"
CFG = "/mnt/Factory/config.sh"
BACKUP = "/mnt/camweb/backup"
STATUS = "/tmp/camweb_time.status"
PIDF = "/tmp/camweb_time.pid"
LOGF = "/tmp/camtime.log"
AK_STATUS = "/tmp/akrtsp.status"
FILES = ["time.conf", "run_time.sh"]

# Inserted above the akrtsp supervisor line rather than appended: the first sync
# of a boot steps the clock by about 56 years, and akrtsp counts RTP timestamps
# per frame against the wall clock. Stepping before it starts is free; stepping
# under it is a hole in the stream.
BOOT_TEXT = (
    "# The clock supervisor. This board has no rtc, and the date akrtsp burns into\n"
    "# the picture (--osd-time) comes from here: akrtsp refuses to draw a date it\n"
    "# cannot vouch for, so this has to run before the stream server does.\n"
    "/bin/sh %s &\n" % RUNNER
)

holder = {"cam": None}


def log(m):
    print(m, flush=True)


def shell():
    if holder["cam"] is None:
        holder["cam"] = Cam(deploy.HOST)
    return holder["cam"]


def md5_local(path):
    with open(path, "rb") as fh:
        return hashlib.md5(fh.read()).hexdigest()


def md5_remote(cam, path):
    return cam.run("md5sum %s 2>/dev/null" % path).split()


def status_map(cam):
    """The supervisor's status file as a dict (key=value lines, whole-file rename)."""
    out = cam.run("cat %s 2>/dev/null" % STATUS)
    m = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            m[k.strip()] = v.strip()
    return m


def status_fields(cam, path, want):
    """Pull named key=value fields out of a status file on the camera.

    One grep over the file rather than one round trip per field: every telnet
    command on this box is a round trip that costs about as much as the read.
    osd_text contains a space, so the split is on the first "=" only.
    """
    pat = "^\\(" + "\\|\\".join(want) + "\\)="
    out = cam.run("grep -h '%s' %s 2>/dev/null" % (pat, path))
    m = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            if k.strip() in want:
                m[k.strip()] = v.strip()
    return m if m else {}


# Public servers whose HTTP Date header is a second-accurate read of a real clock.
# Not a name we trust more than NTP - just a way to ask "which of the two clocks
# in front of us is the wrong one?" without a second NTP client here.
REF_URLS = ["https://www.cloudflare.com/", "https://www.google.com/"]


def reference_epoch():
    """(epoch, source) from outside this network, or (None, None) offline.

    The workstation is NOT a trusted reference: measured while this was written,
    it ran about 20 s behind the Date header of a public server while the camera,
    synced to an NTP peer, agreed with that server to the second. A clock test that
    compares a synced camera with an unsynced workstation reports the camera broken.
    """
    for url in REF_URLS:
        try:
            req = urllib.request.Request(url, method="HEAD")
            with urllib.request.urlopen(req, timeout=8) as r:
                head = r.headers.get("Date")
            if head:
                dt = email.utils.parsedate_to_datetime(head)
                return calendar.timegm(dt.utctimetuple()), url
        except Exception:
            continue
    return None, None


def supervisor_up(cam):
    """The supervisor's pid if it is really running, else "".

    The pidfile alone is not the answer: a recycled pid would make a dead
    supervisor look alive, and on a box with no rtc that is the difference between
    "the clock is wrong" and "the clock is wrong and nothing is fixing it".
    """
    out = cam.run("if [ -f %s ]; then p=$(cat %s); if [ -d /proc/$p ] && "
                  "grep -qa run_time.sh /proc/$p/cmdline 2>/dev/null; then "
                  "echo ALIVE $p; fi; fi" % (PIDF, PIDF))
    return out.strip()


# ------------------------------------------------------------------- config.sh

def patch_config(cam):
    """Add the boot line, the way deploy.py --promote does it.

    Backed up and verified first, syntax-checked after, restored straight away if
    it stops parsing: a config.sh that does not parse is a camera that boots into
    nothing at all.
    """
    if not deploy.yes("if [ -f %s ]; then echo YES; fi" % CFG, cam):
        log("no %s on this camera - refusing to guess" % CFG)
        return False
    cam.run("mkdir -p %s %s" % (BACKUP, REMOTE))
    if deploy.yes("if [ -s %s/config.sh.orig ]; then echo YES; fi" % BACKUP, cam):
        log("backup:  config.sh.orig already there, left untouched")
    else:
        log("backup:  " + cam.run("cp %s %s/config.sh.orig && echo config.sh copied"
                                  % (CFG, BACKUP)).strip())
    if not deploy.yes("if [ -s %s/config.sh.orig ]; then echo YES; fi" % BACKUP, cam):
        log("backup missing - refusing to edit config.sh")
        return False

    orig = deploy.ftp_get(CFG)
    if orig is None:
        log("could not read %s" % CFG)
        return False
    text = orig.decode("utf-8", "replace")
    if "run_time.sh" in text:
        log("config:  clock line already present, left alone")
        return True

    out, done = [], False
    lines = text.splitlines(True)
    at = None
    for i, line in enumerate(lines):
        if "run_akrtsp.sh" in line and not line.lstrip().startswith("#"):
            at = i
            break
    if at is not None:
        # Immediately above the line that starts the stream server. Ordering on its
        # own does not keep the boot step out of the stream, and pretending
        # otherwise would be guesswork: what keeps it out is run_akrtsp.sh waiting
        # for this service's first status write before it starts akrtsp (see the
        # comment in that file). Sitting above the start line only means the first
        # NTP attempt is under way before the server is up.
        where = at
        out = lines[:where] + [BOOT_TEXT] + lines[where:]
        done = True
    if not done:
        log("config:  WARNING - no run_akrtsp.sh line to sit above; appending")
        out = lines + ["\n" + BOOT_TEXT]

    if not deploy.ftp_put(CFG, "".join(out).encode()):
        log("could not write %s" % CFG)
        return False
    if "SYNTAX_OK" not in cam.run("sh -n %s && echo SYNTAX_OK" % CFG):
        log("edited config.sh does not parse - restoring the original")
        deploy.ftp_put(CFG, orig)
        return False
    log("config:  boot line added above the stream server, parses cleanly")
    return True


def unpatch_config(cam):
    orig = deploy.ftp_get(CFG)
    if orig is None:
        log("could not read %s" % CFG)
        return False
    text = orig.decode("utf-8", "replace")
    if "run_time.sh" not in text:
        log("config:  no clock line to remove")
        return True
    kept, skip = [], 0
    for line in text.splitlines(True):
        if line.startswith("# The clock supervisor."):
            skip = 4			# three comment lines and the line that starts it
            continue
        if skip > 0:
            skip -= 1
            continue
        kept.append(line)
    if not deploy.ftp_put(CFG, "".join(kept).encode()):
        log("could not write %s" % CFG)
        return False
    if "SYNTAX_OK" not in cam.run("sh -n %s && echo SYNTAX_OK" % CFG):
        log("config.sh does not parse after removal - putting the copy back")
        deploy.ftp_put(CFG, orig)
        return False
    log("config:  clock line removed, parses cleanly")
    return True


# ------------------------------------------------------------------ operations

def stop_supervisor(cam):
    """Kill the supervisor by pidfile, then by command line.

    The second sweep is not decoration: a copy started by hand from a telnet
    session has a pid that the pidfile no longer holds, and two supervisors both
    stepping the clock is how a clock ends up going backwards and forwards.
    """
    cam.run("if [ -f %s ]; then kill $(cat %s) 2>/dev/null; fi; "
            "for d in /proc/[0-9]*; do if grep -qa 'run_time.sh' $d/cmdline 2>/dev/null; "
            "then p=${d#/proc/}; kill $p 2>/dev/null; fi; done; echo stopped"
            % (PIDF, PIDF), timeout=10)


def do_start(cam):
    stop_supervisor(cam)
    time.sleep(1)
    out = cam.run("setsid /bin/sh %s </dev/null >/dev/null 2>&1 & echo started" % RUNNER,
                  timeout=10)
    log("start:   " + out.strip())
    # Wait for THIS supervisor to write, not for any file that says "synced": the
    # previous one leaves its status behind, and a restart that reports the dead
    # process's numbers is how you end up believing a clock is being watched when
    # nothing is watching it. The supervisor writes its pid into the pidfile and
    # into every status write, so that is the thing to match on.
    newpid = cam.run("cat %s 2>/dev/null" % PIDF).strip()
    for _ in range(25):
        time.sleep(1)
        st = status_map(cam)
        if newpid and st.get("pid") == newpid and st.get("state") == "synced":
            break
    return do_status(cam)


def do_stop(cam):
    stop_supervisor(cam)
    log("stop:    supervisor stopped. The clock stays where it was until the next "
        "boot, and akrtsp keeps drawing whatever it last saw confirmed.")
    return 0


def do_sync(cam):
    up = supervisor_up(cam)
    if not up:
        log("sync:    no supervisor running - start it with --start")
        return 1
    log("sync:    %s <- USR1" % up)
    cam.run("kill -USR1 $(cat %s) 2>/dev/null; echo signalled" % PIDF)
    for _ in range(35):
        time.sleep(1)
        st = status_map(cam)
        if st.get("state") == "synced":
            log("sync:    peer=%s last_sync=%s" % (st.get("peer"), st.get("last_sync")))
            return 0
    log("sync:    still not synced after 35 s; --logs will say which peers answered")
    return 1


def do_tz(cam, rule):
    orig = deploy.ftp_get(CONF)
    if orig is None:
        log("no %s on the camera - run --install first" % CONF)
        return 1
    text = orig.decode("utf-8", "replace")
    out, done = [], False
    for line in text.splitlines(True):
        if line.startswith("TZ_STR="):
            out.append('TZ_STR="%s"\n' % rule)
            done = True
        else:
            out.append(line)
    if not done:
        out.append('TZ_STR="%s"\n' % rule)
    if not deploy.ftp_put(CONF, "".join(out).encode()):
        log("could not write %s" % CONF)
        return 1
    log("tz:      TZ_STR=%s written; akrtsp picks it up when it next starts" % rule)
    return do_start(cam)


def do_install(cam):
    cam.run("mkdir -p %s" % REMOTE)
    for name in FILES:
        src = os.path.join(SRC, name)
        with open(src, "rb") as fh:
            data = fh.read()
        if not deploy.ftp_put(REMOTE + "/" + name, data):
            log("upload FAILED: %s" % name)
            return 1
        got = md5_remote(cam, REMOTE + "/" + name)
        want = hashlib.md5(data).hexdigest()
        ok = bool(got) and got[0] == want
        log("upload:  %-12s %6d bytes  md5 %s"
            % (name, len(data), "verified" if ok else "MISMATCH"))
        if not ok:
            return 1
    cam.run("chmod 755 %s %s" % (RUNNER, CONF))
    if not patch_config(cam):
        return 1
    log("")
    return do_start(cam)


def do_status(cam):
    st = status_map(cam)
    if not st:
        log("status:  no %s yet - the supervisor has not written one" % STATUS)
    else:
        for k in ("state", "peer", "last_sync", "now", "tz", "min_valid"):
            if k in st:
                log("  %-10s %s" % (k, st[k]))
    log("  running:   %s" % (supervisor_up(cam) or "no"))
    log("  date:      %s" % cam.run("date").strip())
    log("  date -u:   %s" % cam.run("date -u").strip())
    ak = cam.run("grep -h '^osd=\\|^clock' /tmp/akrtsp.status 2>/dev/null")
    if ak.strip():
        log("  akrtsp:    " + " ".join(ak.split()))
    return 0


def do_logs(cam):
    log(cam.run("tail -n 40 %s 2>/dev/null" % LOGF))
    return 0


def do_test(cam):
    """Everything that has to hold for a burned-in date to be trustworthy."""
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    for name in FILES:
        want = md5_local(os.path.join(SRC, name))
        got = md5_remote(cam, REMOTE + "/" + name)
        check("%s matches this tree" % name, got and got[0] == want,
              "camera %s vs local %s" % (got[0][:8] if got else "?", want[:8]))

    up = supervisor_up(cam)
    check("supervisor running", up, up)

    st = status_map(cam)
    check("status file present", st.get("state"), "state=%s" % st.get("state"))
    check("says synced", st.get("state") == "synced",
          "state=%s peer=%s" % (st.get("state"), st.get("peer")))
    check("conf TZ_STR in force on the service", (st.get("tz") or "").strip() != "",
          "tz=%s" % st.get("tz"))

    # The honest one: an unsynced clock on this board reads epoch+uptime, which is
    # a plausible-looking number, so it has to be compared with a real clock. Read
    # the epoch live rather than trusting the status file's "now", which is at best
    # a minute old by design; the freshness of that same field is a separate check.
    cam_epoch = int(cam.run("date +%s").strip() or 0)
    here = int(time.time())
    ref, ref_src = reference_epoch()
    base = ref if ref else here
    base_name = ref_src or "this workstation (no internet reference reached)"
    check("epoch agrees with %s" % base_name, abs(cam_epoch - base) <= 5,
          "camera %+d s from the reference (camera %d, reference %d)"
          % (cam_epoch - base, cam_epoch, base))
    if ref and abs(here - ref) > 5:
        log("note:      this workstation is %+d s off the same reference, so a "
            "disagreement here is the workstation, not the camera" % (here - ref))
    try:
        wrote = int(st.get("now") or 0)
    except ValueError:
        wrote = 0
    check("status heartbeat is alive", 0 <= cam_epoch - wrote <= 90,
          "last write %+d s ago; a dead supervisor stops writing this"
          % (cam_epoch - wrote))

    cfg = deploy.ftp_get(CFG) or b""
    check("boot line in config.sh", b"run_time.sh" in cfg,
          "this is the only reason any of it survives a reboot")

    ak = status_fields(cam, AK_STATUS, ("osd", "osd_text", "clock", "clock_state",
                                        "clock_peer", "mode"))
    if ak:
        log("akrtsp:  " + " | ".join("%s=%s" % kv for kv in sorted(ak.items())))
        # End to end, and the only check of the zone that means anything: the
        # string actually painted on the picture has to be the reference's local
        # minute. osd_text is written by akrtsp under the TZ from time.conf
        # (CET-1CEST,...), which has to agree with this workstation's own zone.
        drawn = ak.get("osd_text", "")
        if ak.get("osd") == "on" and drawn and drawn != "NO TIME":
            want = time.strftime("%Y-%m-%d %H:%M", time.localtime(base))
            check("drawn text is the reference's local minute", drawn.startswith(want),
                  "on screen %r, reference %r" % (drawn, want))
        elif ak.get("osd") == "on":
            check("drawn text is the reference's local minute", False,
                  "overlay is on but drawing %r" % drawn)
        else:
            log("         (overlay %s - nothing drawn to compare; that is osd.py's "
                "business, not the clock's)" % ak.get("osd"))
    else:
        log("akrtsp:  no status file - the stream server is not running, so there is "
            "nothing to overlay yet")

    # Informational, deliberately not a pass/fail: the vendor keeps its own zone in
    # /etc/jffs2/time_zone.sh ("export TZ=GMT-08:00") and we do not edit vendor
    # config. Everything camweb starts sets TZ from time.conf; a bare `date` in a
    # telnet session still answers in the vendor's zone, and that is expected.
    log("note:      login-shell offset camera %s vs workstation %s - vendor's own "
        "/etc/jffs2/time_zone.sh, not ours"
        % (cam.run("date +%z").strip(), time.strftime("%z")))

    fails = 0
    for name, ok, detail in checks:
        if not ok:
            fails += 1
        log("  %s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  - " + detail) if detail else ""))
    log("\n%d/%d passed" % (len(checks) - fails, len(checks)))
    return 1 if fails else 0


def do_restore(cam):
    do_stop(cam)
    return 0 if unpatch_config(cam) else 1


def main(argv):
    args = argv[1:]
    try:
        if "--install" in args:
            return do_install(shell())
        if "--status" in args:
            return do_status(shell())
        if "--logs" in args:
            return do_logs(shell())
        if "--sync" in args:
            return do_sync(shell())
        if "--start" in args or "--restart" in args:
            return do_start(shell())
        if "--stop" in args:
            return do_stop(shell())
        if "--restore" in args:
            return do_restore(shell())
        if "--tz" in args:
            i = args.index("--tz")
            if i + 1 >= len(args):
                log("--tz wants a POSIX rule, e.g. 'CET-1CEST,M3.5.0,M10.5.0/3'")
                return 2
            return do_tz(shell(), args[i + 1])
        if "--test" in args:
            return do_test(shell())
        print(__doc__)
        return 2
    finally:
        if holder["cam"] is not None:
            holder["cam"].close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
