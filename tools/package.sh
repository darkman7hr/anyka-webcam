#!/bin/sh
# Build the tarball that gets shared.
#
#   tools/package.sh            -> dist/anyka-cam-YYYY-MM-DD.tar.gz
#   TAG=v1.0-final-$(date +%Y%m%d) tools/package.sh    (a named release)
#   KEEP_BINARY=0 tools/package.sh   (ship without the 20 MB go2rtc)
#   WITH_BINARIES=1 tools/package.sh (keep the built akrtsp/akstream/probes)
#
# What it guards against, because a package that leaks a token or ships a stale
# .port is worse than no package:
#
#   * tools/.token, tools/.port, __pycache__, logs and the 15 MB vendored
#     reference tree never go in;
#   * the access token literal is scrubbed out of the markdown as well, not just
#     the dotfile - the README used to quote it;
#   * the go2rtc binary is included by default (the upstream linux/arm build
#     gives "Illegal instruction" on this board, so people need the working one)
#     and its md5 is recorded in VERSION so a corrupted copy is detectable;
#   * everything executable is made executable again after the copy, so the
#     tarball works straight out of the archive.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
NAME=anyka-cam
STAMP=$(date +%Y-%m-%d)
# TAG names the tarball. A release gets a name that says so:
#   TAG=v1.0-final-$(date +%Y%m%d) tools/package.sh
# and the day-to-day build stays anyka-cam-YYYY-MM-DD.tar.gz.
TAG=${TAG:-$NAME-$STAMP}
DIST="$ROOT/dist"
STAGE=$(mktemp -d)
KEEP_BINARY=${KEEP_BINARY:-1}
# src/ holds the C sources and the Makefile. Build products do not belong in a
# source archive unless someone asks for them (they are 30 KB each, and shipping
# a binary nobody can tie to a commit is how an unreproducible release starts):
#   WITH_BINARIES=1 tools/package.sh
WITH_BINARIES=${WITH_BINARIES:-0}
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/$NAME" "$DIST"
for d in webui live src time; do
    cp -R "$ROOT/$d" "$STAGE/$NAME/"
done
mkdir -p "$STAGE/$NAME/tools"
for f in "$ROOT"/tools/*; do
    [ -f "$f" ] || continue
    case "$(basename "$f")" in
        .token|.port) continue ;;          # local state and a live credential
    esac
    cp "$f" "$STAGE/$NAME/tools/"
done
for f in README.md INSTALL.md NOTICE.md; do
    [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$STAGE/$NAME/"
done

if [ "$KEEP_BINARY" = "0" ]; then
    rm -f "$STAGE/$NAME/live/go2rtc-armv5"
fi

# Local state, byte-compiled leftovers, anything editor-ish.
rm -rf "$STAGE/$NAME/tools/__pycache__"
find "$STAGE" -name '*.pyc' -delete -o -name '*.swp' -delete -o -name '.DS_Store' -delete

# Build products. A source archive carries sources: shipping a binary nobody can
# tie back to a source tree is how an unreproducible release starts. The probe
# tools and the two servers are rebuilt with src/Makefile (see INSTALL.md).
if [ "$WITH_BINARIES" != "1" ]; then
    rm -f "$STAGE/$NAME"/src/akrtsp "$STAGE/$NAME"/src/akrtsp.stripped \
          "$STAGE/$NAME"/src/akstream "$STAGE/$NAME"/src/akstream.stripped \
          "$STAGE/$NAME"/src/akvi_probe "$STAGE/$NAME"/src/akir_probe \
          "$STAGE/$NAME"/src/akir_probe.stripped
fi

# Scrub the token literal out of the docs, whatever the docs say about it.
TOK=""
[ -f "$ROOT/tools/.token" ] && TOK=$(tr -d ' \t\n\r' < "$ROOT/tools/.token")
if [ -n "$TOK" ]; then
    for f in "$STAGE/$NAME"/*.md; do
        [ -f "$f" ] || continue
        sed -i "s/$TOK/<printed by deploy.py>/g" "$f"
    done
fi

# Record what is in here. The binary's hash is recorded because the program
# carries no version flag: it is how a copy sitting on a camera gets identified.
BIN="$STAGE/$NAME/live/go2rtc-armv5"
if [ -f "$BIN" ]; then
    BIN_MD5=$(md5sum "$BIN" | cut -d' ' -f1)
    BIN_SZ=$(wc -c < "$BIN")
else
    BIN_MD5="not shipped - build with GOARM=5, see INSTALL.md section 5"
    BIN_SZ=0
fi
AKB="$ROOT/src/akrtsp.stripped"
if [ -f "$AKB" ]; then
    AK_MD5=$(md5sum "$AKB" | cut -d' ' -f1)
    AK_SZ=$(wc -c < "$AKB")
else
    AK_MD5="not built - make -C src akrtsp, see INSTALL.md"
    AK_SZ=0
fi
{
    echo "$TAG"
    echo "anyka-cam: RTSP stream server (akrtsp) + web UI + on-demand preview relay"
    echo "packed      : $STAMP"
    echo "akrtsp      : $AK_MD5  ($AK_SZ bytes)  built from src/akrtsp.c, armv5tejl uClibc"
    echo "go2rtc      : $BIN_MD5  ($BIN_SZ bytes)  v1.9.15 7c05066 linux/arm GOARM=5"
    echo "tested on   : AK3918E + GC1084, 36 MB RAM, busybox armv5tejl, SD-card hack"
    echo "            : upstream go2rtc_linux_arm asset -> Illegal instruction on this board"
    echo "day/night   : ISP mode, IR illuminator and IR-cut filter, manual or auto;"
    echo "            : auto follows the ISP auto-exposure gain - this board's own"
    echo "            : light sensor reads DAY with the room light on and off"
    echo "clock       : NTP service (time/run_time.sh) plus TZ from its config; the"
    echo "            : stream server publishes clock_state and the exact line it"
    echo "            : burns into the picture in /tmp/akrtsp.status"
    echo "osd         : burned-in date/time on both channels (--osd-time). The"
    echo "            : board's libmpi_osd paints one line per 16-row block of the"
    echo "            : rect and refuses a shorter one, so the date is stamped"
    echo "            : twice in a 32-row rect - measured, see README"
} > "$STAGE/$NAME/VERSION"

chmod 755 "$STAGE/$NAME"/tools/*.py "$STAGE/$NAME"/tools/*.mjs \
          "$STAGE/$NAME"/tools/*.sh "$STAGE/$NAME"/tools/diag.sh 2>/dev/null
chmod 755 "$STAGE/$NAME"/live/*.sh 2>/dev/null
chmod 755 "$STAGE/$NAME"/time/*.sh 2>/dev/null
chmod 755 "$STAGE/$NAME"/live/go2rtc-armv5 2>/dev/null
chmod 755 "$STAGE/$NAME"/src/*.sh 2>/dev/null
chmod 644 "$STAGE/$NAME"/src/*.c "$STAGE/$NAME"/src/*.h \
          "$STAGE/$NAME"/src/Makefile 2>/dev/null
chmod 644 "$STAGE/$NAME"/webui/cgi-bin/api 2>/dev/null

OUT="$DIST/$TAG.tar.gz"
tar czf "$OUT" -C "$STAGE" "$NAME"
echo "wrote $OUT"
ls -l "$OUT" | awk '{print $5" bytes"}'
sha256sum "$OUT"
tar tzf "$OUT" | sort | sed 's/^/  /'
