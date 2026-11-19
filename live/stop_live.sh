#!/bin/sh
# /mnt/camweb/live/stop_live.sh [--disable|--enable]
#
# Take the preview relay down and leave the camera exactly as it was found.
#
# The supervisor (run_live.sh) restores overcommit_memory and the vendor rtsp
# oom_score_adj on its own exit path, so the correct way to stop the relay is to
# ask the supervisor to stop. This script does that, and then cleans up after it
# in case the supervisor was SIGKILLed (or OOM-killed) and never got to run its
# own teardown. That second half is what keeps a crashed relay from leaving
# :1984 open or overcommit switched on for good.
#
#   --disable  also drop a persistent marker; the UI's Live button then refuses
#              until --enable (or rm /mnt/camweb/live/disable) removes it.

LDIR=/mnt/camweb/live
PIDF=/tmp/camweb_live.pid
LOCKF=/tmp/camweb_live.lock

G2F=/tmp/camweb_g2.pid
STATE=/tmp/live_state
OCF=/tmp/live_oc
STOPF=/tmp/live_stopflag

PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export PATH

case "$1" in
  --disable) touch $LDIR/disable 2>/dev/null; printf 'disabled\n' > $STATE 2>/dev/null ;;
  --enable)  rm -f $LDIR/disable ;;
esac
# Record why the relay is down before anything else, so the status endpoint has
# something honest to show: "stopped" for a plain stop, "disable"/"enable" for
# the persistent switch.
case "$1" in
  --disable) : ;;
  --enable)  printf 'enabled\n' > $STATE 2>/dev/null ;;
  *)         printf 'stopped\n' > $STATE 2>/dev/null ;;
esac


# Ask first: the supervisor's TERM trap does the ordered teardown.
# Snapshot the two pids ONCE and chase only those. This script now runs in the
# background behind the CGI's reply, so a viewer can press Start a second later
# and launch a new supervisor while this loop is still turning; re-reading the
# pidfiles each round would let a stop click kill the relay that was started
# after it - one of the ways the relay looked unstartable.
sp=$(cat $PIDF 2>/dev/null)
gp=$(cat $G2F 2>/dev/null)
if [ -n "$sp" ] && [ -d /proc/$sp ]; then
  printf '1\n' > $STOPF 2>/dev/null
  kill $sp 2>/dev/null
fi

# Then make sure nothing is left holding a port or a memory policy.
i=0
while [ $i -lt 6 ]; do
  if [ -n "$gp" ] && [ -d /proc/$gp ]; then kill $gp 2>/dev/null; fi
  # The supervisor's TERM trap waits for go2rtc to exit (up to 5 s) before it
  # lifts the producer's OOM protection. Escalating too early SIGKILLs it mid
  # teardown and leaves the producer pinned at -500 forever, so this happens on
  # the last round only. (It used to read `[ $i -ge 6 ]` inside `while [ $i -lt 6 ]`,
  # which is unreachable - a wedged supervisor was never killed at all.)
  if [ $i -ge 5 ] && [ -n "$sp" ] && [ -d /proc/$sp ]; then kill -9 $sp 2>/dev/null; fi
  gone=1
  [ -n "$gp" ] && [ -d /proc/$gp ] && gone=0
  [ -n "$sp" ] && [ -d /proc/$sp ] && gone=0
  [ "$gone" = "1" ] && break
  sleep 1
  i=$((i + 1))
done

# Belt and braces: if the supervisor never ran its trap, undo its kernel
# settings here, using the value it saved rather than a hardcoded 0.
if [ -f /tmp/live_oc_on ]; then
  v=$(cat $OCF 2>/dev/null)
  printf '%s\n' "${v:-0}" > /proc/sys/vm/overcommit_memory 2>/dev/null
  rm -f /tmp/live_oc_on
fi
# Match by comm, and match BOTH producers: the relay protects whichever stream
# producer is running (vendor rtsp or our akrtsp), so a fallback that knows only
# the vendor's name leaves our server pinned at -500 after a hard stop.
for d in /proc/[0-9]*; do
  c=$(cat $d/comm 2>/dev/null)
  case "$c" in rtsp|akrtsp) ;; *) continue ;; esac
  cur=$(awk '{print $1}' $d/oom_score_adj 2>/dev/null)
  [ "$cur" = "0" ] && continue
  printf '0\n' > $d/oom_score_adj 2>/dev/null
done

# Only unlink state that still names what this run stopped.
#
# This script now runs in the background, behind the CGI's reply, so a viewer who
# presses Start a moment later has a fresh supervisor writing these very files
# while this cleanup is still turning. Unlinking them blindly took the lock away
# from that supervisor: it decided it had been superseded (by an empty pid),
# exited without cleanup, and left its go2rtc orphaned - holding :1984 and :8554
# with no supervisor and empty pidfiles behind it. The page then reported the
# relay as not running while the relay was in fact up, and nothing could stop it.
own() {  # $1 = file, $2 = pid this run saw there
  [ -f "$1" ] || return 0
  cur=$(cat $1 2>/dev/null)
  if [ -n "$2" ]; then
    [ "$cur" = "$2" ] && rm -f "$1"          # still ours: release it
  elif [ -z "$cur" ] || [ ! -d /proc/$cur ]; then
    rm -f "$1"                               # only ever a leftover of something dead
  fi
  return 0
}
own $PIDF "$sp"
own $G2F "$gp"
if [ -d "$LOCKF" ]; then
  lp=$(cat $LOCKF/pid 2>/dev/null)
  if [ -n "$sp" ]; then
    [ "$lp" = "$sp" ] && rm -rf $LOCKF
  elif [ -z "$lp" ] || [ ! -d /proc/$lp ]; then
    rm -rf $LOCKF
  fi
fi
alive=$(cat $PIDF 2>/dev/null)
if [ -z "$alive" ] || [ ! -d /proc/$alive ]; then
  rm -f $STOPF /tmp/live_ready
  printf '%s\n' "${1#--}" > $STATE 2>/dev/null
fi
echo "live relay stopped"
