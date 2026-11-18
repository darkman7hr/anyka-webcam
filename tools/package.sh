#!/bin/sh
# Build the tarball that gets shared.
#
#   tools/package.sh            -> dist/anyka-cam-YYYY-MM-DD.tar.gz
#   KEEP_BINARY=0 tools/package.sh   (ship without the 20 MB go2rtc)
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
DIST="$ROOT/dist"
STAGE=$(mktemp -d)
KEEP_BINARY=${KEEP_BINARY:-1}
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/$NAME" "$DIST"
for d in webui live; do
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

# Scrub the token literal out of the docs, whatever the docs say about it.
TOK=""
[ -f "$ROOT/tools/.token" ] && TOK=$(tr -d ' \t\n\r' < "$ROOT/tools/.token")
if [ -n "$TOK" ]; then
    for f in "$STAGE/$NAME"/*.md; do
        [ -f "$f" ] || continue
        sed -i "s/$TOK/<printed by deploy.py>/g" "$f"
    done
fi

# Record what is in here.
BIN="$STAGE/$NAME/live/go2rtc-armv5"
if [ -f "$BIN" ]; then
    BIN_MD5=$(md5sum "$BIN" | cut -d' ' -f1)
    BIN_SZ=$(wc -c < "$BIN")
else
    BIN_MD5="not shipped - build with GOARM=5, see INSTALL.md section 5"
    BIN_SZ=0
fi
{
    echo "anyka-cam web UI + on-demand live preview relay"
    echo "packed      : $STAMP"
    echo "go2rtc      : $BIN_MD5  ($BIN_SZ bytes)  v1.9.15 7c05066 linux/arm GOARM=5"
    echo "tested on   : AK3918E + GC1084, 36 MB RAM, busybox armv5tejl, SD-card hack"
    echo "            : upstream go2rtc_linux_arm asset -> Illegal instruction on this board"
} > "$STAGE/$NAME/VERSION"

chmod 755 "$STAGE/$NAME"/tools/*.py "$STAGE/$NAME"/tools/*.mjs \
          "$STAGE/$NAME"/tools/*.sh "$STAGE/$NAME"/tools/diag.sh 2>/dev/null
chmod 755 "$STAGE/$NAME"/live/*.sh 2>/dev/null
chmod 755 "$STAGE/$NAME"/live/go2rtc-armv5 2>/dev/null
chmod 644 "$STAGE/$NAME"/webui/cgi-bin/api 2>/dev/null

OUT="$DIST/$NAME-$STAMP.tar.gz"
tar czf "$OUT" -C "$STAGE" "$NAME"
echo "wrote $OUT"
ls -l "$OUT" | awk '{print $5" bytes"}'
sha256sum "$OUT"
tar tzf "$OUT" | sort | sed 's/^/  /'
