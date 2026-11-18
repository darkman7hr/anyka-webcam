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
sp=$(cat $PIDF 2>/dev/null)
if [ -n "$sp" ] && [ -d /proc/$sp ]; then
  printf '1\n' > $STOPF 2>/dev/null
  kill $sp 2>/dev/null
fi

# Then make sure nothing is left holding a port or a memory policy.
i=0
while [ $i -lt 6 ]; do
  gp=$(cat $G2F 2>/dev/null)
  sp=$(cat $PIDF 2>/dev/null)
  if [ -n "$gp" ] && [ -d /proc/$gp ]; then kill $gp 2>/dev/null; fi
  # The supervisor's TERM trap waits for go2rtc to exit (up to 5 s) before it
  # lifts the producer's OOM protection. Escalating too early SIGKILLs it mid
  # teardown and leaves the producer pinned at -500 forever.
  if [ $i -ge 6 ] && [ -n "$sp" ] && [ -d /proc/$sp ]; then kill -9 $sp 2>/dev/null; fi
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

rm -f $PIDF $G2F $STOPF /tmp/live_ready
rm -rf $LOCKF
printf '%s\n' "${1#--}" > $STATE 2>/dev/null
echo "live relay stopped"
