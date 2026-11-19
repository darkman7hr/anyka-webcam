#!/bin/sh
# /mnt/camweb/time/run_time.sh
#
# Keep the camera's clock right, and say out loud whether it is.
#
# Why this exists: this board has no real-time clock at all. There is no
# /sys/class/rtc and no battery - measured on a box that had been up half a day,
# `date` read "Thu Jan 1 13:30 GMT 1970", and the +8 hours it prints under the
# label "GMT" is the vendor's TZ=GMT-08:00 in the environment, not a timezone that
# means anything. The vendor firmware sets its own clock through its cloud bind
# path (its boot log shows "utc_time: 1638933395" and "timezone:8"), and that path
# is not running here. So nothing on this camera knows what time it is.
#
# That stopped being cosmetic the moment the date got burned into the picture:
# akrtsp's OSD overlay puts the clock into the pixels, so a wrong clock is wrong
# in every recording the NVR makes and cannot be corrected afterwards. So the
# overlay refuses to draw a date until this service has confirmed one, and this
# service is what confirms it.
#
# One-shot `ntpd -q` per round rather than a resident ntpd, on purpose: a loop we
# supervise restarts itself when it dies, the status file always says when the
# clock was last confirmed and by whom, and re-stepping every SYNC_SECS keeps the
# correction under a second. That last part is not cosmetic either - akrtsp's RTP
# timestamps count frames and only creep forward against the wall clock, so a
# large step while it is streaming shows up as a hole in the stream. Start this
# BEFORE akrtsp in config.sh so the first (huge) step lands while nothing is
# counting frames yet.
#
# Started by /mnt/Factory/config.sh at boot. Safe to run by hand; it never exits.

CONF=/mnt/camweb/time/time.conf
STATUS=/tmp/camweb_time.status
LASTF=/tmp/camweb_time_last
LOGF=/tmp/camtime.log
PIDF=/tmp/camweb_time.pid
NTPD=/usr/sbin/ntpd

PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export PATH

# Defaults, so the service still does something sensible if the conf is missing.
NTP_PEERS="162.159.200.1 216.239.35.8 pool.ntp.org"
SYNC_SECS=900
BOOT_RETRY_SECS=20
NTP_TIMEOUT=12
TZ_STR="CET-1CEST,M3.5.0,M10.5.0/3"
MIN_VALID_EPOCH=1230768000
[ -f "$CONF" ] && . "$CONF"
TZ="$TZ_STR"
export TZ

# Uptime-stamped, like the other supervisors here: the wall clock is exactly the
# thing in question, so logging it before it is known would be self-reporting a
# lie.
log() {
  echo "$(cut -d. -f1 /proc/uptime)s $*" >> $LOGF 2>/dev/null
}

# Written whole through a temp name: the CGI and akrtsp read this while it is
# being rewritten, and a half-read status is a wrong status.
#
# `now=` is refreshed every minute by nap(), not only on a sync. A status file
# whose "now" is however long ago the last sync was cannot tell anyone whether
# this supervisor is still running, and on a box with no rtc "is the clock being
# looked after?" is the question.
SSTATE=unsynced
SPEER=-
SLAST=0
NOW=0

status() {
  [ $# -ge 1 ] && SSTATE="$1"
  [ $# -ge 2 ] && SPEER="$2"
  [ $# -ge 3 ] && SLAST="$3"
  NOW=$(date +%s)
  printf 'state=%s\npeer=%s\nlast_sync=%s\nnow=%s\ntz=%s\npid=%s\nmin_valid=%s\n' \
    "$SSTATE" "$SPEER" "$SLAST" "$NOW" "$TZ" "$$" "$MIN_VALID_EPOCH" > $STATUS.tmp 2>/dev/null
  mv -f $STATUS.tmp $STATUS 2>/dev/null
}

# One peer, one attempt. ntpd -q exits 0 once it has stepped the clock; the epoch
# check after it is the real test, because a peer that answers with nonsense also
# exits 0.
try_peer() {
  timeout -t $NTP_TIMEOUT $NTPD -n -q -p "$1" >> $LOGF 2>&1 || return 1
  now=$(date +%s)
  [ "$now" -ge "$MIN_VALID_EPOCH" ] || return 2
  return 0
}

# A whole round over the peer list. Sets PEER_OK to the one that worked.
#
# Not run through $(...): a command substitution runs the function in a subshell,
# and then the assignments it makes to SSTATE/SPEER/SLAST die with that subshell
# while its write to the status file does not. Measured on this camera: the log
# said "synced" every 15 minutes and the status file said "unsynced", because the
# next heartbeat - running in the parent, with the parent's untouched defaults -
# wrote those defaults back over the good line.
PEER_OK=
sync_round() {
  PEER_OK=
  for p in $NTP_PEERS; do
    try_peer "$p"
    rc=$?
    if [ $rc -eq 0 ]; then
      now=$(date +%s)
      printf '%s\n' "$now" > $LASTF 2>/dev/null
      status synced "$p" "$now"
      PEER_OK="$p"
      return 0
    fi
    [ $rc -eq 2 ] && log "peer $p answered but the clock still reads $now - not believed"
  done
  log "no peer answered (tried: $NTP_PEERS)"
  return 1
}

echo $$ > $PIDF 2>/dev/null

# `time.py --sync` sends this. Handling it means the clock can be corrected on
# demand - after a reboot, before an experiment, when someone changes the zone -
# without waiting out SYNC_SECS and without a second ntpd racing this one.
NEED_SYNC=0
want_sync() { NEED_SYNC=1; }
trap want_sync USR1

# Sleep in one second slices so USR1 is answered inside a second: ash runs its
# traps once the foreground child returns, and `sleep 900` would be a foreground
# child.
nap() {
  i=0
  while [ $i -lt "$1" ]; do
    [ "$NEED_SYNC" = 1 ] && return 0
    sleep 1
    i=$((i+1))
    # Every minute, same state, fresh "now": a heartbeat, so a reader can tell a
    # watched clock from an abandoned one.
    [ $((i % 60)) -eq 0 ] && status
  done
  return 1
}

log "clock supervisor up: peers=[$NTP_PEERS] every=${SYNC_SECS}s boot_retry=${BOOT_RETRY_SECS}s tz=$TZ"

while :; do
  if sync_round; then
    log "synced from $PEER_OK: date says [$(date)] epoch $(date +%s)"
    nap $SYNC_SECS
  else
    last=$(cat $LASTF 2>/dev/null)
    status unsynced "-" "${last:-0}"
    log "no peer confirmed; retrying in ${BOOT_RETRY_SECS}s"
    nap $BOOT_RETRY_SECS
  fi
  NEED_SYNC=0
done
