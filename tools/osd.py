#!/usr/bin/env python3
"""Deploy and verify the burned-in date overlay on this camera.

What it is: akrtsp draws the clock through the ISP's own OSD (libmpi_osd), so the
text is in the pixels before the encoder ever runs. That means the NVR's
recordings carry it, and it costs the 400 MHz core nothing measurable - this is
the hardware's overlay path, not software compositing. The vendor firmware used
exactly this on the same board (its boot log shows ak_osd_set_font_file,
ak_osd_init ch:0 fontS:32 res_w:1280 res_h:720 and an ak_osd_set_rect of
1280x64), and the font it used is still on the card at /usr/local/ak_font_16.bin.

What had to be learned first: the ak_osd.h in the SDK dump declares thirteen
functions; the libmpi_osd.so this camera generation ships exports ten. The linker
said so - ak_osd_set_font_size, ak_osd_set_rect_enable and ak_osd_set_edge_color
are undefined at link time - so the font file is mandatory and there is no
"enable this rect" call to lean on. The static libmpi_osd.a in the SDK dump does
have them, and is NOT used: it belongs to a newer SDK generation than this board's
kernel and libraries, and shipping an ABI mismatch to a camera you cannot debug
interactively is the one mistake this project cannot recover from.

Subcommands:
    --install       deploy libmpi_osd.so, akrtsp and run_akrtsp.sh (md5 checked),
                    write the argument file with --osd-time, restart the server
    --status        what the server says about the overlay and the clock
    --on / --off    the flag in /mnt/camweb/akrtsp.args, then restart
    --pos CORNER    tl | tr | bl | br; --size N; --scale N; --rect-h N; --fmt STRFMT
    --shot [FILE]   grab one key frame and measure the ink in the overlay band
    --legible       read the line back off the picture: how far the ink stands
                    off the scene behind it, whether the rect holds one line and
                    not two, and the bright pixels against the glyphs in the
                    vendor font file. That last comparison is the only check
                    that tells a readable date from a plausible mess of strokes.
    --no-matrix     fall back to the library's own text blit, which smears here;
                    --probe draws the calibration pattern instead of the date,
                    --nibble-hi flips which half of a canvas byte is the left
                    pixel (the one thing about this display no header says)
    --proof         grab with the overlay on, turn it off, grab again, and count
                    the pixels that differ - the only check that proves the text
                    is really in the picture rather than in a status file
    --test          deployment checks: files, md5s, flags, status fields

The clock itself is tools/time.py's business. If that service is not saying
"synced", the overlay deliberately draws NO TIME rather than a date it cannot
vouch for, and --status says which of the two it is doing.
"""

import hashlib
import os
import re
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy  # noqa: E402
from cam import Cam  # noqa: E402

# tools/time.py by its real path, under a name that cannot collide with the
# standard library's "time". Importing it as `import time` from this directory
# would shadow the stdlib module for everything else in here, which is a bug that
# only shows up at runtime in the middle of a sleep().
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "camtime", os.path.join(os.path.dirname(os.path.abspath(__file__)), "time.py"))
timeclock = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(timeclock)

# rgbshot.py already solves the one awkward part of grabbing a frame: joining an
# H.264 stream mid-GOP decodes a P-frame with no reference, which looks exactly
# like "the image went dark". It waits for a real key frame. Reusing it beats
# restating that rule here and getting it wrong in a second place.
_rspec = importlib.util.spec_from_file_location(
    "rgbshot_mod", os.path.join(os.path.dirname(os.path.abspath(__file__)), "rgbshot.py"))
rgbshot = importlib.util.module_from_spec(_rspec)
_rspec.loader.exec_module(rgbshot)

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))
SDK_LIB = os.environ.get(
    "SDK_LIB",
    "/home/darkman/opt/anyka_v380ipcam_experiments/rootfs_v1basic/rootfs/usr/lib")

BIN_LOCAL = os.path.join(SRC, "akrtsp.stripped")
SO_LOCAL = os.path.join(SDK_LIB, "libmpi_osd.so")
RUNNER_LOCAL = os.path.join(SRC, "run_akrtsp.sh")

DIR = "/mnt/camrtsp"
BIN = DIR + "/akrtsp"
RUNNER = DIR + "/run_akrtsp.sh"
LIBDIR = DIR + "/lib"
SO = LIBDIR + "/libmpi_osd.so"
ARGS = "/mnt/camweb/akrtsp.args"
STATUS = "/tmp/akrtsp.status"
SUP_LOG = "/tmp/akrtsp_sup.log"
BIN_LOG = "/tmp/akrtsp.log"
FONT = "/usr/local/ak_font_16.bin"
MODE_FILE = "/mnt/camweb/daynight.conf"	# what the web UI writes; akrtsp obeys it
URL = "rtsp://%s:554/vs0" % deploy.HOST
SUB_URL = "rtsp://%s:554/vs1" % deploy.HOST

# The flags this tool owns, as they sit in the argument file. The rest of the line
# (mode file, force-night, ir-follow) belongs to day/night and is carried through
# untouched: two tools rewriting one argument line is how a deployment loses its
# day/night settings overnight.
OSD_DEFAULT = "--osd-time"
OSD_POS_DEFAULT = "--osd-pos br"

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


FIELDS = ("osd", "osd_text", "clock", "clock_state", "clock_peer", "mode", "want",
          "fps", "clients", "ircut", "auto", "pid")


def fields(cam):
    return timeclock.status_fields(cam, STATUS, FIELDS)


def stream_owner(cam):
    """Who answers :554: "akrtsp", "vendor", "unknown", or "" for nobody.

    This matters because of what the supervisor does after three failed starts:
    it launches /mnt/Factory/custom/rtsp and gives up. The NVR keeps seeing
    pictures, so nothing looks wrong from outside, while the day/night control,
    the status file and the overlay are all gone - and the numbers left in the
    status file still look healthy. Reading them without asking who owns the port
    is how that state gets mistaken for a working one (measured: one argument the
    deployed binary did not know cost the camera its whole pipeline this way).
    """
    out = cam.run("netstat -ltnp 2>/dev/null | grep ':554 ' | head -1")
    if ":554" not in out:
        return ""
    if "/akrtsp" in out:
        return "akrtsp"
    if "/rtsp" in out:
        return "vendor"
    return "unknown"


def status_live(cam, f):
    """True only when the process the status file names is still running.

    The file outlives the process, and on this box that gap has already produced
    a false "everything is fine".
    """
    pid = (f.get("pid") or "").strip()
    return bool(pid.isdigit() and cam.run("ls -d /proc/%s 2>/dev/null" % pid).strip())


def deployed_flags(cam):
    """The options the akrtsp on the card knows, from its own --help.

    Cheap and decisive: the usage text is printed before the binary touches the
    capture pipeline, so asking cannot disturb a running server. The explicit
    LD_LIBRARY_PATH is not decoration - akrtsp needs libmpi_osd.so from
    /mnt/camrtsp/lib, and without it the loader refuses to start the binary, the
    probe sees no usage text at all, and this check silently degrades to "cannot
    tell" (measured).
    """
    return set(re.findall(r"--[a-z][a-z-]+",
                          cam.run("LD_LIBRARY_PATH=/mnt/camrtsp/lib:/mnt/Factory/custom/lib "
                                  "%s --help 2>&1 | head -90" % BIN)))


def unknown_flags(cam, line):
    """Flags this argument line asks for that the deployed binary has never
    heard of. None means "could not ask", not "nothing is missing"."""
    known = deployed_flags(cam)
    if not known:
        return None
    return sorted({f for f in re.findall(r"--[a-z][a-z-]+", line) if f not in known})


def put_new(cam, local, remote, label):
    """Upload to /tmp and move it into place, checking the md5 both ends.

    Writing straight over /mnt/camrtsp/akrtsp does not work: ftpd answers 553 for
    a file that is the image of a running process (measured, and the reason the
    takeover scripts do it this way). The move is atomic on the card and the old
    inode keeps serving the process that is still running until it is restarted.
    """
    tmp = "/tmp/osd.new"
    with open(local, "rb") as fh:
        data = fh.read()
    if not deploy.ftp_put(tmp, data):
        log("%s: upload to %s failed" % (label, tmp))
        return None
    cam.run("mkdir -p %s" % os.path.dirname(remote))
    out = cam.run("cp %s %s.new 2>&1 && chmod 755 %s.new && mv -f %s.new %s && "
                  "md5sum %s" % (tmp, remote, remote, remote, remote, remote))
    got = out.split()
    want = hashlib.md5(data).hexdigest()
    if not got or got[0] != want:
        log("%s: on the camera it reads %s, expected %s (%s)"
            % (label, got[0][:8] if got else "nothing", want[:8], out.strip()[:120]))
        return None
    cam.run("rm -f %s" % tmp)
    log("%-22s %6d bytes  md5 %s verified" % (label, len(data), want[:8]))
    return want


def current_args(cam):
    """The argument line the server would start with, and who says so.

    Two places can hold it - /mnt/camweb/akrtsp.args (which run_akrtsp.sh reads and
    which the tools write) and the AK_ARGS in config.sh's boot line (which the
    supervisor's environment carries). The file wins, so this reports the file when
    it exists rather than whatever config.sh happens to remember.
    """
    txt = cam.run("cat %s 2>/dev/null" % ARGS).strip()
    if txt:
        return txt, ARGS
    cfg = (deploy.ftp_get(timeclock.CFG) or b"").decode("utf-8", "replace")
    for line in cfg.splitlines():
        if "run_akrtsp.sh" in line and "AK_ARGS=" in line:
            return line.split('AK_ARGS="', 1)[1].split('"', 1)[0], "config.sh"
    return "", "run_akrtsp.sh default"


def set_args(cam, line, why):
    if not deploy.ftp_put(ARGS, (line.strip() + "\n").encode()):
        log("could not write %s" % ARGS)
        return False
    log("args:    %s (%s)" % (line.strip(), why))
    return True


def log_banners(cam):
    """How many startup lines akrtsp has written to its log, or None.

    A restart is not over when the status file changes: the lines that say where
    the overlay rect went are written by the new process a moment later, and every
    measurement in this tool is taken against those lines. Counting them is how the
    wait below knows the new process has finished talking.
    """
    out = cam.run("grep -c 'akrtsp on :' %s 2>/dev/null" % BIN_LOG).strip()
    try:
        return int(out.splitlines()[-1])
    except (ValueError, IndexError):
        return None


def restart_server(cam, why):
    """Restart the supervisor and its child together.

    The supervisor goes first and on purpose: its whole job is to put akrtsp back
    within its interval, and it holds the argument line in its own environment, so
    leaving it alive would bring akrtsp straight back with the flags from before
    this change. Every NVR and every preview session blinks for a few seconds;
    that is the price of a flag change, and the supervisor's own fallback to the
    vendor server is what covers a change that does not come up.
    """
    log("restart: %s" % why)
    # Who is serving right now. The wait below has to see a *different* pid: a
    # killed akrtsp lingers in /proc for up to its 10 s teardown deadline with the
    # port still bound, and "the status file names a live process" is then true of
    # the corpse - the caller is told the restart worked and the next RTSP connect
    # is refused or half-served (measured twice on 19 Nov).
    before = (fields(cam).get("pid") or "").strip()
    banner0 = log_banners(cam)
    cam.run("for d in /proc/[0-9]*; do if grep -qa run_akrtsp.sh $d/cmdline 2>/dev/null; "
            "then p=${d#/proc/}; kill $p 2>/dev/null; fi; done; "
            "for d in /proc/[0-9]*; do c=$(cat $d/comm 2>/dev/null); "
            "[ \"$c\" = akrtsp ] && kill ${d#/proc/} 2>/dev/null; done; echo killed",
            timeout=20)
    time.sleep(2)
    cam.run("setsid /bin/sh %s 10 3 >/tmp/akrtsp_boot.log 2>&1 & echo started" % RUNNER,
            timeout=10)
    for i in range(35):
        time.sleep(1)
        f = fields(cam)
        pid = (f.get("pid") or "").strip()
        # A status file is not proof of a live server: the file from the process
        # that just died is still sitting there, and reading it as if it were
        # current is how a failed restart gets reported as a successful one.
        if f.get("osd") and pid and pid != before and status_live(cam, f):
            # One more thing has to have happened before this is safe to report:
            # the server has to have logged its own startup line. --status and
            # --shot measure against the rect line in that log, and it appears a
            # moment after the status file does, so returning here early makes the
            # next reader find the PREVIOUS process's rect and measure the wrong
            # corner of the picture (measured: a size change reported the old
            # 208x32 rect while the new one was 800x64).
            if banner0 is not None:
                for _ in range(10):
                    n = log_banners(cam)
                    if n is None or n > banner0:
                        break
                    time.sleep(1)
            return f
        if i % 5 == 4 and stream_owner(cam) == "vendor":
            log("restart: the supervisor gave up and put the vendor server back on "
                ":554 - our server never came up. Read %s for what the child said."
                % SUP_LOG)
            return {}
    log("restart: no status file from a live server after 35 s - check %s" % SUP_LOG)
    return {}


def rect_from_log(cam, channel="main"):
    """The rect the server said it used, parsed out of its own log line.

    Measuring the overlay means knowing where it is. Reading it back from the
    server is honest in a way that recomputing it here is not: it proves the
    server actually placed it, and it survives a change of --osd-pos or --size.
    """
    out = cam.run("(grep -h 'osd: %s rect' %s 2>/dev/null | tail -1;"
                  " grep -h 'osd: %s rect' %s 2>/dev/null | tail -1)"
                  % (channel, BIN_LOG, channel, SUP_LOG))
    lines = [l for l in out.splitlines() if " rect " in l]
    line = lines[0] if lines else ""
    if not line:
        return None
    try:
        rest = line.split(" rect ", 1)[1]
        wh, at = rest.split(" at ")
        w, h = (int(v) for v in wh.lower().split("x"))
        x, y = (int(v) for v in at.split(" of ")[0].split(","))
        return x, y, w, h
    except (ValueError, IndexError):
        log("could not parse the rect from: %r" % line)
        return None


# --------------------------------------------------------------------- pixels

def luma_at(d, i):
    """Integer luma; weights rough enough to tell bright text over a dark scene."""
    return (d[i] * 3 + d[i + 1] * 6 + d[i + 2]) // 10


def rect_pixels(w, h, d, rect):
    """The luma of every pixel inside a rect, clipped to the frame."""
    x, y, rw, rh = rect
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(w, x + rw), min(h, y + rh)
    out = []
    for row in range(y0, y1):
        i = (row * w + x0) * 3
        for _ in range(x0, x1):
            out.append(luma_at(d, i))
            i += 3
    return out


def mirror(rect, w):
    """The same-sized rect on the other side of the frame.

    The control for --proof. If the picture changed because the scene moved, the
    control band moves with it; if it changed because text was drawn, only the one
    rect changes.
    """
    x, y, rw, rh = rect
    return (max(0, w - x - rw), y, rw, rh)


def ink_stats(w, h, d, rect):
    v = rect_pixels(w, h, d, rect)
    if not v:
        return None
    med = statistics.median(v)
    return {"n": len(v), "median": med,
            "ink": sum(1 for p in v if p - med > 40)}


def diff_stats(w, h, da, db, rect):
    a, b = rect_pixels(w, h, da, rect), rect_pixels(w, h, db, rect)
    if not a or len(a) != len(b):
        return None
    return {"n": len(a),
            "changed": sum(1 for p, q in zip(a, b) if abs(p - q) > 12),
            "mean": sum(abs(p - q) for p, q in zip(a, b)) / float(len(a))}


def grab(out):
    """One key frame from the camera's own stream, through rgbshot's grab."""
    return rgbshot.grab(URL, out)


# ------------------------------------------------------------------- status

def do_status(cam):
    f = fields(cam)
    if not f:
        log("status:  no %s - the stream server is not running" % STATUS)
        return 1
    for k in ("osd", "osd_text", "clock_state", "clock_peer", "clock", "mode",
              "want", "fps", "clients"):
        if k in f:
            log("  %-11s %s" % (k, f[k]))
    args, src = current_args(cam)
    log("  args:      %s" % (args or "(default)"))
    log("  args from: %s" % src)
    for ch in ("main", "sub"):
        r = rect_from_log(cam, ch)
        if r:
            log("  %-11s %dx%d at %d,%d" % (ch + " rect", r[2], r[3], r[0], r[1]))
    if f.get("osd") == "on" and f.get("osd_text") == "NO TIME":
        log("  note:      drawing NO TIME - the clock service is not confirming a "
            "date (see tools/time.py --status)")
    live = status_live(cam, f)
    owner = stream_owner(cam)
    if not live:
        log("  WARNING:   %s names pid %s, which is not running - these numbers "
            "come from a server that is gone" % (STATUS, f.get("pid") or "nobody"))
    if owner == "vendor":
        log("  WARNING:   :554 is held by the VENDOR server. Our supervisor gave up "
            "after three failed starts, so the overlay and the day/night control are "
            "not running - read %s for what the child said." % SUP_LOG)
    elif owner == "":
        log("  WARNING:   nothing is listening on :554")
    return 0 if live and owner == "akrtsp" else 1


# ------------------------------------------------------------------ deployment

def strip_osd(line):
    """Remove every --osd-* flag from an argument line, leaving the rest alone."""
    parts, out, i = line.split(), [], 0
    while i < len(parts):
        p = parts[i]
        if p.startswith("--osd"):
            if p in ("--osd-time", "--osd-no-sub", "--osd-probe",
                     "--osd-nibble-hi", "--osd-no-matrix"):
                i += 1
            else:
                i += 2		# the ones that take a value
            continue
        out.append(p)
        i += 1
    return " ".join(out)


def do_install(cam):
    for path, label in ((BIN_LOCAL, "akrtsp build"), (SO_LOCAL, "SDK library"),
                        (RUNNER_LOCAL, "supervisor")):
        if not os.path.exists(path):
            log("no local %s at %s - build it first (make -C src check)" % (label, path))
            return 1
    if not deploy.yes("if [ -r %s ]; then echo YES; fi" % FONT, cam):
        # The overlay needs it and this build's libmpi_osd cannot pick a builtin
        # font: the header's ak_osd_set_font_size is not in this library.
        log("%s is not readable on the camera - the overlay would refuse to start" % FONT)
        return 1
    for path, remote, label in ((SO_LOCAL, SO, "libmpi_osd.so"),
                                (RUNNER_LOCAL, RUNNER, "run_akrtsp.sh"),
                                (BIN_LOCAL, BIN, "akrtsp")):
        if put_new(cam, path, remote, label) is None:
            return 1
    args, src = current_args(cam)
    if "--osd-time" in args:
        log("args:    already carrying --osd-time (read from %s)" % src)
    else:
        args = (args + " " if args else "") + OSD_DEFAULT + " " + OSD_POS_DEFAULT
        if not set_args(cam, args, "--osd-time added"):
            return 1
    f = restart_server(cam, "installed overlay")
    if not f:
        return 1
    log("")
    return do_status(cam)


def do_flag(cam, on, extra=None):
    """Turn the overlay on or off, and restart so it takes effect.

    It is a start-up flag, not a runtime one: ak_osd's rects and pmem are set up
    with the capture pipeline, so "off" means "started without it". There is no
    control-file path for it, and inventing one would mean a thread that can free
    the overlay while frames are being drawn - which is not a thing worth risking
    for a flag that changes once a year.
    """
    args, src = current_args(cam)
    if not args:
        log("the argument line could not be read from %s or from config.sh, and I am "
            "not going to invent one: give it explicitly with --args '...'" % ARGS)
        return 1
    args = strip_osd(args)
    if on:
        args = (args + " --osd-time").strip()
        # The default corner only if the caller did not name one: adding both means
        # an argument line that contradicts itself, and which flag wins then
        # depends on getopt's order rather than on what was asked for.
        if not (extra and "--osd-pos" in extra):
            args += " " + OSD_POS_DEFAULT
    if extra:
        args = (args + " " + extra).strip()
    missing = unknown_flags(cam, args)
    if missing:
        log("the akrtsp on the camera has never heard of: %s" % " ".join(missing))
        log("         deploy it first with --install. Writing a flag the deployed "
            "binary does not know is how a camera loses its stream: the server "
            "exits on the unknown word, the supervisor counts three failed starts "
            "and hands :554 to the vendor server (measured, 19 Nov).")
        return 2
    if missing is None:
        log("note:    the deployed akrtsp printed no usage text, so I cannot check "
            "its flags; writing them anyway")
    if not set_args(cam, args, "overlay %s" % ("on" if on else "off")):
        return 1
    f = restart_server(cam, "overlay %s" % ("on" if on else "off"))
    if not f:
        return 1
    log("")
    return do_status(cam)


def edge_rows(w, h, d, rect):
    """Per row of the rect, how many hard left-to-right luma transitions there are.

    This is the measurement that works on this camera. Text is the only thing in
    the picture that crosses a lot of stroke edges in one row; wall, sky and an
    IR-lit foreground cross almost none. It lives inside a single frame, which
    matters because the on/off comparison does not: starting akrtsp re-initialises
    the ISP, so the exposure of the "off" frame differs from the "on" frame by tens
    of luma everywhere (measured: mean delta 40-47 in the control rect too), and a
    difference of that size buries the text completely.
    """
    x, y, rw, rh = rect
    x0, x1 = max(0, x), min(w, x + rw)
    y0, y1 = max(0, y), min(h, y + rh)
    out = []
    for row in range(y0, y1):
        i = (row * w + x0) * 3
        prev = luma_at(d, i)
        n = 0
        for _ in range(x0 + 1, x1):
            i += 3
            v = luma_at(d, i)
            if abs(v - prev) > 30:
                n += 1
            prev = v
        out.append(n)
    return out


def do_shot(cam, out):
    """Grab a frame and measure the ink where the server said it would draw."""
    rect = rect_from_log(cam)
    if not rect:
        log("shot:    the server has not logged a rect - is osd on? "
            "(grep 'osd:' %s)" % BIN_LOG)
        return 1
    if not grab(out):
        return 1
    w, h, d = rgbshot.read_ppm(out)
    log("shot:    %s  %dx%d, main rect %dx%d at %d,%d" % (out, w, h, rect[2], rect[3],
                                                          rect[0], rect[1]))
    a = ink_stats(w, h, d, rect)
    c = ink_stats(w, h, d, mirror(rect, w))
    if a:
        log("  in the rect:   %d of %d pixels brighter than the local median "
            "(median %d)" % (a["ink"], a["n"], a["median"]))
    if c:
        log("  same rect, opposite corner: %d of %d (the control: a scene with no "
            "text anywhere has similar numbers)" % (c["ink"], c["n"]))
    pa = edge_rows(w, h, d, rect)
    pc = edge_rows(w, h, d, mirror(rect, w))
    if pa and pc:
        scale = max(1, rect[2] // 40)
        log("  stroke edges per row - left the rect, right the same-sized rect on "
            "the other\n  side of the frame. A band on the left with nothing on the "
            "right is painted\ntext; both flat means the text is not reaching the "
            "encoder.")
        for i in range(len(pa)):
            log("    y=%4d %4d %-42s %4d %s"
                % (rect[1] + i, pa[i], "#" * min(40, pa[i] // scale),
                   pc[i], "#" * min(40, pc[i] // scale)))
        hot = 20
        log("  rows with stroke edges: %d of %d in the rect, %d of %d in the control"
            % (sum(1 for v in pa if v > hot), len(pa),
               sum(1 for v in pc if v > hot), len(pc)))
    return 0


# ---------------------------------------------------------------------- proof

def row_diff_profile(w, da, db, rect):
    """How many pixels changed in each row of the rect, top row first.

    This is the only way to answer "how many lines did the library actually paint".
    Its log says where the rect is, not what went inside it, and on this board the
    same 16 px line of glyphs has been seen at two offsets inside one 32 px rect.
    Bands of changed rows are the answer; nothing else separates painted text from a
    scene that happens to be busy.
    """
    x, y, rw, rh = rect
    x0, x1 = max(0, x), min(w, x + rw)
    y0 = max(0, y)
    out = []
    for row in range(y0, min(y0 + rh, len(db) // (w * 3))):
        i = (row * w + x0) * 3
        n = 0
        for _ in range(x0, x1):
            if abs(luma_at(da, i) - luma_at(db, i)) > 24:
                n += 1
            i += 3
        out.append(n)
    return out


# ---------------------------------------------------------------- legibility
#
# Whether the date can be read off the recording is the whole point of the
# feature, and it is the one thing a status file cannot answer. Three numbers
# come off a single grabbed frame: how far the ink sits off the scene behind it,
# whether the rest of the rect is empty (the library's own text blit stamps a
# second, stretched copy), and how well the bright pixels match the glyphs the
# server was asked to draw - the last one from the vendor font file itself, so
# it is a comparison against what the picture should be and not against a
# guess at what text looks like.

FONT_ASCII = 4096		# the ASCII half: 256 cells of 16 bytes, bit 7 leftmost
_font = None


def font_cells(cam):
    """The vendor font's ASCII cells, or None. Cell 1 is the CP437 smiley and
    cell '0' has to carry ink - the same two things osd_load_font() checks on the
    camera, so a file the server will not draw from is not compared against
    either."""
    global _font
    if _font is not None:
        return _font
    data = deploy.ftp_get(FONT) or b""
    if len(data) < FONT_ASCII:
        return None
    if 0x7E not in data[16:32]:
        return None
    zero = data[ord("0") * 16:ord("0") * 16 + 16]
    if sum(bin(b).count("1") for b in zero) < 8:
        return None
    _font = data[:FONT_ASCII]
    return _font


def expect_line(fb, text, scale):
    """The line as osd_render_bits() paints it: one 8x16 cell per character,
    every pixel a scale x scale block, cells 10*scale apart, from x 0 of the
    rect. Kept as a list of rows of 0/1 so it can be laid straight over the
    measured ink."""
    adv = (8 + 2) * scale
    w = len(text) * adv
    rows = [[0] * w for _ in range(16 * scale)]
    for i, ch in enumerate(text):
        cell = fb[ord(ch) * 16:ord(ch) * 16 + 16]
        for r in range(16):
            bits = cell[r]
            if not bits:
                continue
            for c in range(8):
                if not (bits >> (7 - c)) & 1:
                    continue
                for j in range(scale):
                    for k in range(scale):
                        rows[r * scale + k][i * adv + c * scale + j] = 1
    return rows


def template_iou(ink, tpl, span=4):
    """The best overlap of the measured ink with the expected glyphs, allowing a
    few pixels of slack in both axes for the encoder's chroma sit and for the
    rect having been placed with an offset. 1.0 is the same picture twice."""
    th, tw = len(tpl), len(tpl[0])
    best = 0.0
    at = None
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            inter = union = 0
            for r in range(th):
                rr = r + dy
                if rr < 0 or rr >= len(ink):
                    continue
                row = ink[rr]
                tplrow = tpl[r]
                for c in range(tw):
                    cc = c + dx
                    if cc < 0 or cc >= len(row):
                        continue
                    a = tplrow[c]
                    b = row[cc]
                    if a or b:
                        union += 1
                        inter += 1 if (a and b) else 0
            score = inter / float(union) if union else 0.0
            if score > best:
                best = score
                at = (dx, dy)
    return best, at



def legibility(w, h, d, rect, pos="br", line_h=32, text=None, scale=2):
    """Measure the burned-in line in a single frame.

    The scene under the text changes slowly across a row, so the baseline for a
    column is that column's median over a band well above the text; what is left
    is the overlay. Ink is then the pixels that sit well off that baseline, and
    three numbers say whether the line can be read: how strong it is, how many
    rows carry strokes (one line's worth - two would mean the doubling this
    library's own text blit produces), and how many stroke edges there are
    (enough to be glyphs rather than a bar).

    Returns None when the rect is not inside the frame.
    """
    x, y, rw, rh = rect
    if rw < 16 or rh < line_h:
        return None
    y0 = y + rh - line_h if pos.startswith("b") else y
    if y0 < 0 or y0 + line_h > h or x + rw > w:
        return None
    base_rows = range(max(0, y0 - 2 * line_h), min(h, y0 + line_h))
    base = []
    for c in range(rw):
        v = sorted(luma_at(d, ((r * w + x + c) * 3)) for r in base_rows)
        base.append(v[len(v) // 2])
    dev = [[luma_at(d, ((r * w + x + c) * 3)) - base[c] for c in range(rw)]
           for r in range(y0, y0 + line_h)]
    # Which way the ink stands out depends on what is behind it: the palette's
    # foreground over a dark corner reads brighter than its surroundings, over an
    # IR-lit wall it does not. Both polarities are counted and the one carrying
    # more ink wins - the same decision a person squinting at the frame makes.
    lit_up = sum(1 for row in dev for v in row if v > 25)
    lit_dn = sum(1 for row in dev for v in row if v < -25)
    sign = 1 if lit_up >= lit_dn else -1
    ink = [[(sign * v) > 25 for v in row] for row in dev]
    rows = [sum(row) for row in ink]
    lit = [i for i, n in enumerate(rows) if n > rw // 8]
    edges = 0
    for row in ink:
        edges += sum(1 for i in range(1, rw) if row[i] != row[i - 1])
    strength = (sign * sum(dev[r][c] for r in range(line_h) for c in range(rw)
                           if ink[r][c])) / float(max(1, sum(rows)))
    # Every other row of the rect that carries ink is a second line, the ghost of
    # the one just measured, or the library's own text blit leaking through - and
    # the rect is much bigger than the line, so the band around the line says
    # nothing about it. Sampled every fourth column, against a baseline taken over
    # the whole rect (the scene brightens across 500 rows, and the band's baseline
    # does not reach that far), and only counted as text when the row is broken up
    # into many short runs: glyph strokes do that, a window frame or a horizontal
    # scene edge does not.
    step = 4
    cols = range(0, rw, step)
    span = len(list(cols))
    tall = []
    for r in range(y, min(h, y + rh)):
        tall.append([luma_at(d, ((r * w + x + c) * 3)) for c in cols])
    wide = [[tall[i][j] for i in range(len(tall))] for j in range(span)]
    base2 = [sorted(col)[len(col) // 2] for col in wide]
    stray = 0
    for r, row in enumerate(tall):
        if y0 <= y + r < y0 + line_h:
            continue
        lit_n = 0
        trans = 0
        prev = False
        for j, v in enumerate(row):
            on = (sign * (v - base2[j])) > 25
            lit_n += 1 if on else 0
            if on and not prev:
                trans += 1
            prev = on
        if lit_n > span // 10 and trans >= 12:
            stray += 1
    # And the shape itself, against the font the server draws from. This is what
    # separates text from a plausible-looking mess of strokes: the fallback blit
    # paints a horizontally doubled copy of every glyph, which is bright, broken
    # up into strokes, and in the right place - only it is not the date.
    iou = None
    if text and _font:
        tpl = expect_line(_font, text, scale)
        if len(tpl) == len(ink) and tpl[0]:
            iou, at = template_iou(ink, tpl)
    return {"line_h": line_h, "y0": y0, "rows_lit": len(lit),
            "first_row": lit[0] if lit else -1,
            "last_row": lit[-1] if lit else -1,
            "ink": sum(rows), "edges": edges, "strength": strength,
            "stray_rows": stray, "iou": iou, "bright": sign > 0}


def do_legible(cam):
    """Read the line back off the picture and say whether it can be read."""
    rect = rect_from_log(cam, "main")
    sub = rect_from_log(cam, "sub")
    if not rect:
        log("legible: no rect in %s; is the overlay on?" % BIN_LOG)
        return 1
    out = "/tmp/osd-legible.ppm"
    if not grab(out):
        return 1
    w, h, d = rgbshot.read_ppm(out)
    args, _ = current_args(cam)
    toks = args.split() if isinstance(args, str) else list(args)
    pos = toks[toks.index("--osd-pos") + 1] if "--osd-pos" in toks else "bl"
    scale = 2
    if "--osd-scale" in toks:
        scale = int(toks[toks.index("--osd-scale") + 1])
    line_h = 16 * scale
    text = (fields(cam).get("osd_text") or "").strip()
    if not font_cells(cam):
        log("legible: %s did not read as the vendor font, so the glyphs cannot "
            "be compared with anything - brightness and layout only" % FONT)
    rc = 0
    log("legible: main rect %dx%d at %d,%d, pos %s, scale %d, %r" %
        (rect[2], rect[3], rect[0], rect[1], pos, scale, text))
    got = legibility(w, h, d, rect, pos, line_h, text, scale)
    if not got:
        log("  could not measure that rect in the frame")
        return 1
    log("  ink %s over %d rows (rows %d..%d of the %d measured), %d stroke "
        "edges, contrast %+.0f luma"
        % ("bright" if got["bright"] else "dark", got["rows_lit"],
           got["first_row"], got["last_row"], got["line_h"], got["edges"],
           got["strength"]))
    one = got["stray_rows"] <= 2
    strong = abs(got["strength"]) > 20
    shaped = (got["iou"] >= 0.50) if got["iou"] is not None else \
        got["edges"] > 4 * got["line_h"]
    if got["iou"] is not None:
        log("  %.0f%% of the glyph pattern accounted for (measured ink against "
            "the font file, best of a few pixels of slack)" % (100 * got["iou"]))
    log("  %s  the expected glyphs (%s), one line (%s, %d stray rows in the "
        "rect), visible (%s)"
        % ("READABLE" if one and strong and shaped else "NOT READABLE",
           "yes" if shaped else "no",
           "yes" if one else "no", got["stray_rows"],
           "yes" if strong else "no"))
    if not (one and strong and shaped):
        rc = 1
        log("  hint: the display can keep the rect the previous run asked for and")
        log("  slide the new bitmap into it - that looks exactly like this. Restart")
        log("  the server (or power cycle) and measure again before believing it.")
    if sub and rgbshot.grab(SUB_URL, "/tmp/osd-legible-sub.ppm"):
        ws, hs, ds = rgbshot.read_ppm("/tmp/osd-legible-sub.ppm")
        g = legibility(ws, hs, ds, sub, pos, line_h, text, scale)
        if g:
            log("  sub rect %dx%d at %d,%d: ink %s, %d rows, %d edges, "
                "%+.0f luma%s" % (sub[2], sub[3], sub[0], sub[1],
                                  "bright" if g["bright"] else "dark",
                                  g["rows_lit"], g["edges"], g["strength"],
                                  "" if g["iou"] is None else
                                  ", template %.0f%%" % (100 * g["iou"])))
            ok = abs(g["strength"]) > 20 and g["stray_rows"] <= 2 and \
                ((g["iou"] >= 0.50) if g["iou"] is not None
                 else g["edges"] > 4 * g["line_h"])
            if not ok:
                rc = 1
    return rc


def do_proof(cam):
    """Draw it, grab, undraw it, grab, put it back, and count the pixels.

    The only check here that proves the date is in the picture rather than in a
    status file: a real key frame with the overlay against one without it, inside
    the rect the server itself reported, against the same-sized rect on the other
    side of the frame. Two restarts, so every NVR and every preview blinks twice,
    which is why it is a deliberate subcommand and not part of --test.
    """
    args, _ = current_args(cam)
    if "--osd-time" not in args:
        log("proof:   the overlay is off right now; run --on first")
        return 1
    rect = rect_from_log(cam)
    if not rect:
        log("proof:   no rect in %s to measure against" % BIN_LOG)
        return 1
    a, b = "/tmp/osd-on.ppm", "/tmp/osd-off.ppm"
    # Pin the ISP mode for the duration. The first proof here compared a night frame
    # with a day frame - the auto policy switched the ISP between the two grabs and
    # 99.8% of both the measured rect and the control rect changed, which proves
    # nothing about the overlay. Whatever mode the camera is in now is the mode the
    # control file is set to for the next minute, and it is put back after.
    mode = (fields(cam).get("mode") or "").strip()
    saved_mode = cam.run("cat %s 2>/dev/null" % MODE_FILE).strip()
    if mode in ("day", "night"):
        deploy.ftp_put(MODE_FILE, (mode + "\n").encode())
        log("proof:   ISP pinned to %s for the measurement (%s restored after)"
            % (mode, saved_mode or "empty"))
    pinned = mode in ("day", "night")
    got_a = got_b = False
    try:
        log("proof:   grabbing with the overlay on")
        time.sleep(3)			# the encoder has to reach a key frame first
        got_a = bool(grab(a))
        if not got_a:
            return 1
        log("proof:   turning the overlay off (the stream blinks)")
        if do_flag(cam, False) != 0:
            return 1
        # From here the camera has no overlay, and every way out of this function
        # goes through the finally below. A measurement that failed halfway must
        # not be the reason the camera stopped burning its date - that is the whole
        # point of the feature (measured: a refused connection at this step left the
        # overlay off and the next --status described a different camera).
        log("proof:   grabbing without it")
        time.sleep(3)
        got_b = bool(grab(b))
    finally:
        if pinned:
            deploy.ftp_put(MODE_FILE, ((saved_mode or "auto") + "\n").encode())
            log("proof:   ISP control file back to %r" % (saved_mode or "auto"))
        log("proof:   putting it back on (the stream blinks again)")
        # Restore the exact line that was there, not do_flag()'s defaults:
        # strip_osd() removes every --osd-* flag, so the geometry being measured
        # (--osd-rect-h, --osd-yoff, --osd-xoff) would otherwise be gone by the time
        # this returns, and the next --status would describe a camera that is not the
        # one that was just measured.
        set_args(cam, args, "restored after proof")
        restart_server(cam, "proof restored")
    if not got_a or not got_b:
        log("proof:   one of the two frames never arrived; nothing to compare")
        return 1
    wa, ha, da = rgbshot.read_ppm(a)
    wb, hb, db = rgbshot.read_ppm(b)
    if (wa, ha) != (wb, hb):
        log("proof:   frames are %dx%d and %dx%d - not comparable" % (wa, ha, wb, hb))
        return 1
    inside = diff_stats(wa, ha, da, db, rect)
    ctrl = diff_stats(wa, ha, da, db, mirror(rect, wa))
    log("proof:   frames %dx%d, overlay rect %dx%d at %d,%d"
        % (wa, ha, rect[2], rect[3], rect[0], rect[1]))
    if not inside or not ctrl:
        return 1
    log("  where the text is:    %d of %d pixels changed, mean luma delta %.1f"
        % (inside["changed"], inside["n"], inside["mean"]))
    log("  same rect, opposite:  %d of %d pixels changed, mean luma delta %.1f"
        % (ctrl["changed"], ctrl["n"], ctrl["mean"]))
    prof = row_diff_profile(wa, da, db, rect)
    if prof:
        scale = max(1, rect[2] // 40)
        log("  rows inside the rect that changed - each band of rows is one painted "
            "line:")
        for i, n in enumerate(prof):
            log("    y=%4d %4d %s" % (rect[1] + i, n, "#" * (n // scale)))
    moved = inside["changed"] > inside["n"] * 0.10
    quiet = ctrl["changed"] < ctrl["n"] * 0.10
    if moved and quiet:
        log("  verdict: the change is where the date is drawn, and not elsewhere")
        return 0
    log("  verdict: NOT proven. Either the text is not reaching the encoder, or the "
        "scene itself moved enough in the control rect to hide the signal - run it "
        "again with the camera looking at something still.")
    return 1


def do_test(cam):
    """Deployment checks. No restarts, no blinks."""
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    for local, remote, label in ((SO_LOCAL, SO, "libmpi_osd.so"),
                                 (BIN_LOCAL, BIN, "akrtsp"),
                                 (RUNNER_LOCAL, RUNNER, "run_akrtsp.sh")):
        if not os.path.exists(local):
            check("%s built here" % label, False, "missing %s" % local)
            continue
        want = md5_local(local)
        got = md5_remote(cam, remote)
        check("%s on the camera matches this tree" % label,
              bool(got) and got[0] == want,
              "camera %s vs local %s" % (got[0][:8] if got else "absent", want[:8]))

    check("font present", deploy.yes("if [ -r %s ]; then echo YES; fi" % FONT, cam), FONT)

    args, src = current_args(cam)
    check("--osd-time in the argument line", "--osd-time" in args,
          "from %s: %s" % (src, args))

    f = fields(cam)
    check("server running and reporting", bool(f), "" if f else "no %s" % STATUS)
    check("status file is from a live process", status_live(cam, f),
          "pid=%s" % (f.get("pid") or "none"))
    check(":554 is held by akrtsp, not the vendor server",
          stream_owner(cam) == "akrtsp", "owner=%s" % (stream_owner(cam) or "nothing"))
    check("overlay on", f.get("osd") == "on", "osd=%s" % f.get("osd"))
    check("clock confirmed", f.get("clock_state") == "synced",
          "clock_state=%s peer=%s" % (f.get("clock_state"), f.get("clock_peer")))
    if f.get("osd") == "on":
        ref, ref_src = timeclock.reference_epoch()
        base = ref if ref else int(time.time())
        want = time.strftime("%Y-%m-%d %H:%M", time.localtime(base))
        prev = time.strftime("%Y-%m-%d %H:%M", time.localtime(base - 60))
        shown = (f.get("osd_text") or "").strip()
        # A grabbed key frame is up to a GOP old and the server redraws only when
        # the string changes, so a frame drawn at HH:MM:59 is still the picture
        # being made when the reference has ticked on. The minute before is
        # therefore accepted; anything older is a clock that stopped.
        check("drawn text is the reference's local minute",
              shown.startswith(want) or shown.startswith(prev),
              "on screen %r, reference %r%s (%s)"
              % (shown, want,
                 "" if shown.startswith(want) else " (matched the minute before)",
                 ref_src or "workstation clock"))
    if f.get("osd") == "on":
        out = "/tmp/osd-test-legible.ppm"
        rect = rect_from_log(cam)
        toks = (" ".join(args)).split() if not isinstance(args, str) else args.split()
        scale = int(toks[toks.index("--osd-scale") + 1]) if "--osd-scale" in toks else 2
        font_cells(cam)
        if not rgbshot.grab(URL, out):
            check("text in the picture is legible", False, "frame grab failed")
        elif not rect:
            check("text in the picture is legible", False, "no rect in %s" % BIN_LOG)
        else:
            w, h, d = rgbshot.read_ppm(out)
            got = legibility(w, h, d, rect, "br" if "--osd-pos" not in toks
                             else toks[toks.index("--osd-pos") + 1],
                             16 * scale, (f.get("osd_text") or "").strip(), scale)
            ok = bool(got) and got["rows_lit"] > 0 and got["stray_rows"] <= 2 and \
                abs(got["strength"]) > 20 and \
                ((got["iou"] or 0) >= 0.50 if got["iou"] is not None
                 else got["edges"] > 4 * got["line_h"])
            check("text in the picture is legible", ok,
                  "" if ok else ("no measurement" if not got else
                                 "rows %d..%d, strength %+.0f, template %s" %
                                 (got["first_row"], got["last_row"],
                                  got["strength"],
                                  "n/a" if got["iou"] is None else
                                  "%.0f%%" % (100 * got["iou"]))))
    check("server logged where it placed the rect", rect_from_log(cam))

    fails = 0
    for name, ok, detail in checks:
        if not ok:
            fails += 1
        log("  %s  %s%s" % ("PASS" if ok else "FAIL", name,
                            ("  - " + detail) if detail else ""))
    log("\n%d/%d passed" % (len(checks) - fails, len(checks)))
    return 1 if fails else 0


def main(argv):
    args = argv[1:]
    try:
        if "--install" in args:
            return do_install(shell())
        if "--status" in args:
            return do_status(shell())
        if "--on" in args:
            extra = ""
            if "--pos" in args:
                extra += " --osd-pos " + args[args.index("--pos") + 1]
            if "--size" in args:
                extra += " --osd-size " + args[args.index("--size") + 1]
            if "--rect-h" in args:
                extra += " --osd-rect-h " + args[args.index("--rect-h") + 1]
            if "--xoff" in args:
                extra += " --osd-xoff " + args[args.index("--xoff") + 1]
            if "--yoff" in args:
                extra += " --osd-yoff " + args[args.index("--yoff") + 1]
            if "--fmt" in args:
                fmt = args[args.index("--fmt") + 1]
                if " " in fmt:
                    # The supervisor expands the argument line unquoted, so a
                    # format with a space in it arrives as two arguments and the
                    # server reads half of it. Say so rather than shipping a
                    # half-applied setting: use %Y-%m-%dT%H:%M:%S instead.
                    log("--fmt must not contain spaces: the supervisor expands the "
                        "argument line unquoted")
                    return 2
                extra += " --osd-fmt " + fmt
            if "--scale" in args:
                extra += " --osd-scale " + args[args.index("--scale") + 1]
            for name, flag in (("--probe", "--osd-probe"),
                               ("--nibble-hi", "--osd-nibble-hi"),
                               ("--no-matrix", "--osd-no-matrix")):
                if name in args:
                    extra += " " + flag
            return do_flag(shell(), True, extra.strip() or None)
        if "--off" in args:
            return do_flag(shell(), False)
        if "--shot" in args:
            out = "/tmp/osd-shot.ppm"
            if "--out" in args:
                out = args[args.index("--out") + 1]
            return do_shot(shell(), out)
        if "--legible" in args:
            return do_legible(shell())
        if "--proof" in args:
            return do_proof(shell())
        if "--test" in args:
            return do_test(shell())
        print(__doc__)
        return 2
    finally:
        if holder["cam"] is not None:
            holder["cam"].close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))