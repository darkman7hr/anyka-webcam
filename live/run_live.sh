#!/bin/sh
# /mnt/camweb/live/run_live.sh
#
# On-demand H.264 preview relay beside the vendor stack.
#
# NOT started at boot and NOT in /mnt/Factory/config.sh. The web UI's CGI
# (cgi-bin/api, act=live_start) launches this when a browser asks for the
# in-page stream; it exits by itself once nobody is watching. With no viewer
# open the camera runs exactly the processes it always has, and nothing here
# survives a reboot.
#
# Every guard below is here because of something measured on this board
# (36 MB RAM, no swap, ARM926EJS):
#
#   overcommit_memory=1   go2rtc maps ~53 MB of anonymous address space at
#                         exec (Go's arena reservation). With the vendor default
#                         (0) the kernel refuses the exec before a single
#                         instruction runs, even though the resident set is only
#                         ~7 MB. That is an address-space policy failure, not
#                         out-of-memory, and it is the one reason go2rtc appears
#                         not to run here. Restored on every exit path.
#   GOMEMLIMIT / GOGC     keeps the Go heap inside a budget the vendor stack can
#                         live beside.
#   oom_score_adj         go2rtc is made the kernel's first choice (1000) and
#                         the stream producer - the vendor rtsp server, or our
#                         akrtsp if it owns :554 - is pushed the other way
#                         (-500) while the relay is up: if memory ever does run
#                         out the preview dies and the camera keeps streaming.
#   MemFree guard         on 36 MB without swap, waiting for the OOM killer is
#                         not a policy. The relay removes itself instead.
#   restart cap           go2rtc's MP4 consumer panics reproducibly when a
#                         consumer disconnects (pkg/mp4/consumer.go:82 ->
#                         pkg/core/writebuffer.go:37, nil http.ResponseWriter).
#                         It is restarted, but a bounded number of times.
#
# All mutable state lives in /tmp (tmpfs): the SD card is slow and this logs.

LDIR=/mnt/camweb/live
BIN=$LDIR/go2rtc
CONF=$LDIR/live.conf
CFG=/tmp/live.json
PIDF=/tmp/camweb_live.pid
G2F=/tmp/camweb_g2.pid
SEEN=/tmp/live_seen
STATE=/tmp/live_state
OCF=/tmp/live_oc
STOPF=/tmp/live_stopflag
READYF=/tmp/live_ready
LOG=/tmp/live.log

PATH=/bin:/usr/bin:/sbin:/usr/sbin:$PATH
export PATH

. $CONF 2>/dev/null
[ -n "$LIVE_API_PORT" ]     || LIVE_API_PORT=1984
[ -n "$LIVE_RTSP_PORT" ]    || LIVE_RTSP_PORT=8554
[ -n "$LIVE_IDLE_S" ]       || LIVE_IDLE_S=300
[ -n "$LIVE_MEM_GUARD_KB" ] || LIVE_MEM_GUARD_KB=1200
[ -n "$LIVE_START_MIN_AVAIL_KB" ] || LIVE_START_MIN_AVAIL_KB=12000
[ -n "$LIVE_GOMEMLIMIT" ]   || LIVE_GOMEMLIMIT=12MiB
[ -n "$LIVE_GOGC" ]         || LIVE_GOGC=60
[ -n "$LIVE_MAX_RESTART" ]  || LIVE_MAX_RESTART=5
[ -n "$LIVE_SRC" ]          || LIVE_SRC=rtsp://127.0.0.1:554/vs0

log() {
  printf '%ss %s\n' "$(cut -d. -f1 /proc/uptime 2>/dev/null)" "$*" >> $LOG 2>/dev/null
  n=$(wc -c < $LOG 2>/dev/null)
  case "$n" in ''|*[!0-9]*) n=0 ;; esac
  if [ "$n" -gt 60000 ]; then
    tail -c 30000 $LOG > $LOG.t 2>/dev/null && mv $LOG.t $LOG 2>/dev/null
  fi
}
state() { printf '%s\n' "$*" > $STATE 2>/dev/null; }
freekb() {
  v=$(awk '/MemFree/{print $2}' /proc/meminfo 2>/dev/null)
  case "$v" in ''|*[!0-9]*) v=0 ;; esac
  printf '%s' "$v"
}

# The guard metric is *available* memory, not MemFree. On this camera MemFree
# sits around 2.7 MB while ~19 MB is reclaimable page cache -- that is the
# kernel doing its job on a 36 MB board, not a memory crisis. Guarding on MemFree
# would have the relay kill itself on a perfectly healthy camera.
#
# This kernel may not report MemAvailable (it arrived in Linux 3.14), so the
# same estimate is computed here when it is absent: free + page cache + buffers
# + reclaimable slab, less the watermark the kernel insists on keeping.
availkb() {
  m=$(cat /proc/sys/vm/min_free_kbytes 2>/dev/null)
  case "$m" in ''|*[!0-9]*) m=0 ;; esac
  awk -v m="$m" '
    $1 == "MemAvailable:"  { a = $2 }
    $1 == "MemFree:"       { f = $2 }
    $1 == "Cached:"        { c = $2 }
    $1 == "Buffers:"       { b = $2 }
    $1 == "SReclaimable:"  { s = $2 }
    END {
      if (a > 0) { print a; exit }
      v = f + c + b + s - m
      print (v > 0 ? v : 0)
    }' /proc/meminfo 2>/dev/null
}


# One supervisor at a time. Two of them fight over overcommit_memory and over
# the ports, and the loser fails in a way that looks like a broken relay rather
# than a duplicated one -- which is exactly what happened with a pidfile check:
# two "live_start" requests arriving close together both saw no live pidfile and
# both went ahead. mkdir is atomic on every filesystem, so the lock is taken by
# exactly one of them; a lock left behind by a dead supervisor is taken over.
#
# The lock directory holds the authoritative PID (lock/pid). It is not written
# by the CGI: four concurrent starts each raced to record their own child, and
# the file ended up naming a loser that had already exited.
alive_pid() {
  [ -n "$1" ] || return 1
  [ -d "/proc/$1" ] || return 1
  # A supervisor that exited but was never reaped still has a /proc entry, so
  # "directory exists" is not proof of life. State Z means it is a corpse.
  [ "$(awk '{print $3}' /proc/$1/stat 2>/dev/null)" = "Z" ] && return 1
  return 0
}
LOCK=/tmp/camweb_live.lock
HBF=/tmp/camweb_live.lock/hb
heartbeat() { printf '%s\n' "$(cut -d. -f1 /proc/uptime 2>/dev/null)" > $HBF 2>/dev/null; }
# A lock is only held if its holder is alive AND still beating. The heartbeat is
# what makes a killed or wedged supervisor self-healing: a PID alone can be a
# zombie, can be reused by an unrelated process, or can belong to a shell that
# never got as far as starting anything.
hb_fresh() {
  t=$(cut -d. -f1 /proc/uptime 2>/dev/null)
  h=$(cat $HBF 2>/dev/null)
  case "$t$h" in ''|*[!0-9]*) return 1 ;; esac
  [ $((t - h)) -lt "$1" ]
}
if ! mkdir $LOCK 2>/dev/null; then
  old=$(cat $LOCK/pid 2>/dev/null)
  if alive_pid "$old"; then
    # A live holder keeps the lock, full stop. Reclaiming on a stale heartbeat
    # alone was the mistake: under a load-20 box a supervisor can spend that long
    # in a wait loop, the newcomer deleted the holder's heartbeat file, both kept
    # running, and each started its own go2rtc. Only a dead holder loses the lock.
    log "already running as pid $old, nothing to do"
    exit 0
  fi
  # The holder is gone (killed, crash, or a corpse nobody reaped): reclaim it.
  rm -rf $LOCK 2>/dev/null
  mkdir $LOCK 2>/dev/null || { log "lock $LOCK is stuck, refusing to start"; exit 1; }
  log "reclaimed a stale lock (old pid $old)"
fi
echo $$ > $LOCK/pid
echo $$ > $PIDF
heartbeat
# Logged immediately: a supervisor that dies before this line has been killed,
# and a supervisor that logs this and nothing after it is stuck in a wait loop.
log "lock taken pid=$$ ppid=$(awk '{print $4}' /proc/$$/stat 2>/dev/null) comm=$(cat /proc/$$/comm 2>/dev/null)"
# owns_lock - the second half of single-instance, and the half that was missing.
# Refusing to start only protects the moment of starting; if a newcomer reclaims
# the lock while this supervisor is wedged in a wait loop, two of them run, each
# with its own go2rtc, and the second one's "is the port up?" check passes
# because the *other* one is holding the port. Checking ownership every beat
# makes the superseded one leave instead.
owns_lock() {
  if [ ! -d "$LOCK" ]; then
    # A missing lock directory is not the same as being superseded: whoever
    # supersedes this supervisor replaces the directory, it does not simply
    # remove it, and the removal is what a teardown running in the background
    # does. Treating that as superseded made this supervisor leave without
    # cleanup and orphan its go2rtc - which kept holding :1984 and :8554 behind
    # empty pidfiles, so the page reported the relay as not running and nothing
    # could stop it. Nobody holds the lock now, so take it back and go on.
    mkdir "$LOCK" 2>/dev/null
    if [ -d "$LOCK" ]; then
      echo $$ > $LOCK/pid
      echo $$ > $PIDF
      heartbeat
      log "lock had vanished; retaken as pid $$"
      return 0
    fi
    return 1
  fi
  [ "$(cat $LOCK/pid 2>/dev/null)" = "$$" ]
}





if [ "$LIVE_EN" != "1" ]; then
  state "disabled (LIVE_EN=0)"; log "refused: LIVE_EN=$LIVE_EN"; rm -f $PIDF; rm -rf $LOCK; exit 0
fi
if [ -f $LDIR/disable ]; then
  state "disabled (live/disable)"; log "refused: $LDIR/disable present"; rm -f $PIDF; rm -rf $LOCK; exit 0
fi
if [ ! -x $BIN ]; then
  state "binary missing"; log "refused: $BIN missing"; rm -f $PIDF; rm -rf $LOCK; exit 1
fi
# Never let a stale "ready" marker survive into a new session: the CGI reads it
# to decide whether the relay came up.
rm -f $READYF $STOPF

# The vendor RTSP server's comm is exactly "rtsp". Matching on comm rather than
# grepping cmdline dodges the trap where the grep matches its own command line.

listening() {
  # Straight out of /proc/net/tcp: netstat is not reliably on the CGI's PATH and
  # forking one per poll on this CPU is silly. Ports are uppercase hex there.
  awk -v p=":$(printf '%04X' $1)" '$4 == "0A" && index($2, p) { f = 1 } END { exit !f }' /proc/net/tcp
}
vendor_pids() {
  # The stream producer, whichever binary is serving it: the vendor's rtsp, or
  # our akrtsp if it has taken over :554. Both must be protected while the relay
  # runs - the whole point of this mechanism is that if memory does run out, the
  # preview dies and the camera keeps streaming. Protecting only the name "rtsp"
  # would leave akrtsp as the kernel's most attractive victim.
  for d in /proc/[0-9]*; do
    c=$(cat $d/comm 2>/dev/null)
    case "$c" in
      rtsp|akrtsp) printf '%s ' ${d#/proc/} ;;
    esac
  done
}
oom_vendor() {
  for p in $(vendor_pids); do printf '%s\n' "$1" > /proc/$p/oom_score_adj 2>/dev/null; done
}

# overcommit_memory is saved before it is changed, and the saved value -- not a
# hardcoded 0 -- is what gets restored. rc.local on this camera sets
# min_free_kbytes but leaves overcommit at the kernel default, and a future
# firmware might not.
# The default for THIS boot, not the value a previous supervisor happened to
# leave behind. Whoever starts first since boot records it, nobody overwrites
# it. A superseded supervisor deliberately does not restore overcommit (that
# would pull the setting out from under the relay still running), so the next
# one read 1, saved 1 as 'the original' and later 'restored' 1 forever - that
# is how the kernel setting got stranded after what looked like a clean stop.
# /tmp is RAM: a boot clears the file and the real default is read again.
if [ -r $OCF ]; then
  OC=$(cat $OCF 2>/dev/null)
else
  OC=$(cat /proc/sys/vm/overcommit_memory 2>/dev/null)
  printf '%s\n' "${OC:-0}" > $OCF 2>/dev/null
fi
oc_on() {
  printf '1\n' > /proc/sys/vm/overcommit_memory 2>/dev/null
  printf '1\n' > /tmp/live_oc_on 2>/dev/null
}
oc_off() {
  [ -f /tmp/live_oc_on ] || return 0
  v=$(cat $OCF 2>/dev/null)
  printf '%s\n' "${v:-0}" > /proc/sys/vm/overcommit_memory 2>/dev/null
  rm -f /tmp/live_oc_on
}

G2=""
start_g2() {
  # Config generated from live.conf on every start, so the two cannot drift.
  # No separate webrtc listener: the API on the api port answers the offer and
  # ICE runs over the rtsp udp port, so the page's recvonly WebRTC works
  # without a third listener asking for RAM on this board.
  printf '{"api":{"listen":":%s","origin":"*"},"rtsp":{"listen":":%s"},"webrtc":{"listen":""},"streams":{"cam":"%s"}}\n' \
    "$LIVE_API_PORT" "$LIVE_RTSP_PORT" "$LIVE_SRC" > $CFG 2>/dev/null
  GOMEMLIMIT=$LIVE_GOMEMLIMIT GOGC=$LIVE_GOGC $BIN -c $CFG </dev/null >>$LOG 2>&1 &
  G2=$!
  printf '%s\n' "$G2" > $G2F 2>/dev/null
  printf '1000\n' > /proc/$G2/oom_score_adj 2>/dev/null
  oom_vendor -500
  log "go2rtc started pid=$G2 src=$LIVE_SRC memlimit=$LIVE_GOMEMLIMIT gc=$LIVE_GOGC"
}

stop_g2() {
  [ -n "$G2" ] || G2=$(cat $G2F 2>/dev/null)
  if [ -n "$G2" ] && [ -d /proc/$G2 ]; then
    kill $G2 2>/dev/null
    i=0
    while [ -d /proc/$G2 ] && [ $i -lt 5 ]; do sleep 1; i=$((i + 1)); done
    if [ -d /proc/$G2 ]; then kill -9 $G2 2>/dev/null; log "go2rtc needed SIGKILL pid=$G2"; fi
    log "go2rtc stopped pid=$G2"
  fi
  oom_vendor 0
  rm -f $G2F
}

BYE=""
bye() {
  state "${BYE:-stopped}"
  log "supervisor exit: ${BYE:-stopped} (restarts=${restarts:-0})"
  stop_g2
  oc_off
  rm -f $PIDF $STOPF $READYF
  rm -rf $LOCK
  exit 0
}

trap 'BYE=signalled; bye' INT TERM

# The vendor server must be up before a producer points at it: config.sh starts
# rtsp only after `sleep 30`, so pressing Live early in boot has to wait rather
# than hand go2rtc a stream that refuses to connect.
i=0
until listening 554; do
  heartbeat
  if ! owns_lock; then
    # Someone else owns the lock now. Leave without touching anything: killing
    # go2rtc or restoring overcommit here would take down the relay the new
    # supervisor is responsible for.
    log "superseded by pid $(cat $LOCK/pid 2>/dev/null); leaving without cleanup"
    exit 0
  fi
  i=$((i + 1))
  if [ $i -gt 40 ]; then
    state "vendor rtsp not listening"
    log "no vendor rtsp on :554 after 40 s, giving up"
    BYE="vendor rtsp absent"
    bye
  fi
  sleep 1
done

# Refusing is better than trying: starting a relay on a camera that cannot
# accommodate another 12 MB is how the camera, not the relay, gets killed.
av=$(availkb)
if [ "$av" -lt "$LIVE_START_MIN_AVAIL_KB" ]; then
  state "not enough available memory (${av} kB)"
  log "refused: available=${av}kB below LIVE_START_MIN_AVAIL_KB=${LIVE_START_MIN_AVAIL_KB} (MemFree=$(freekb)kB)"
  BYE="refused: low memory"
  bye
fi


oc_on
# A previous crash can leave a go2rtc orphaned and still holding the ports; the
# bind check below would then see the port listening (held by the stranger) and
# call it ready. Our binary is the only process on this box named "go2rtc", so
# matching on comm is exact and, as with the vendor "rtsp" match, cannot catch
# the grep's own command line.
#
# The scan is gated on a port actually being occupied. Walking /proc costs one
# fork per process, and a fork here is ~0.5 s under load: doing it unconditionally
# stalled the supervisor by close to a minute during a four-way start race, its
# heartbeat went stale, and other starts reclaimed the lock from under it.
stray_pids() {
  for d in /proc/[0-9]*; do
    c=$(cat $d/comm 2>/dev/null)
    [ "$c" = "go2rtc" ] || continue
    printf '%s ' "${d#/proc/}"
  done
}
if listening $LIVE_API_PORT || listening $LIVE_RTSP_PORT; then
  heartbeat
  for p in $(stray_pids); do
    log "reaping orphaned go2rtc pid=$p that still holds the relay ports"
    kill $p 2>/dev/null
  done
  for p in $(stray_pids); do [ -d /proc/$p ] && sleep 2; done
  heartbeat
fi
start_g2



# Report "running" only once the API really answers, so the UI does not chase a
# port that was never bound.
i=0
until listening $LIVE_API_PORT; do
  heartbeat
  if ! owns_lock; then
    # Someone else owns the lock now. Leave without touching anything: killing
    # go2rtc or restoring overcommit here would take down the relay the new
    # supervisor is responsible for.
    log "superseded by pid $(cat $LOCK/pid 2>/dev/null); leaving without cleanup"
    exit 0
  fi
  i=$((i + 1))
  alive=$(if [ -d /proc/$G2 ]; then echo yes; else echo no; fi)
  if [ $i -gt 15 ] || [ "$alive" = "no" ]; then
    state "relay failed to bind :$LIVE_API_PORT"
    log "go2rtc did not bind :$LIVE_API_PORT (alive=$alive)"
    BYE="bind failed"
    bye
  fi
  sleep 1
done
state "running"
# The CGI waits on this file rather than polling /proc/net/tcp through awk: on
# this CPU each awk fork cost ~0.8 s, which dragged "ready" out to 20 s.
touch $READYF 2>/dev/null
log "relay ready on :$LIVE_API_PORT (avail=$(availkb)kB free=$(freekb)kB)"

restarts=0
guard=0
beat=0
while :; do
  sleep 5
  beat=$((beat + 1))
  heartbeat
  if ! owns_lock; then
    log "superseded by pid $(cat $LOCK/pid 2>/dev/null); leaving without touching the new relay"
    exit 0
  fi

  if [ -f $STOPF ]; then BYE="stopped by request"; break; fi

  now=$(date +%s 2>/dev/null)
  last=$(cat $SEEN 2>/dev/null)
  case "$now" in ''|*[!0-9]*) now=0 ;; esac
  case "$last" in ''|*[!0-9]*) last=0 ;; esac
  idle=$((now - last))
  if [ $idle -gt $LIVE_IDLE_S ]; then BYE="idle ${idle}s"; break; fi

  av=$(availkb)
  if [ "$av" -lt "$LIVE_MEM_GUARD_KB" ]; then
    guard=$((guard + 1))
    log "available=${av}kB < guard ${LIVE_MEM_GUARD_KB}kB (MemFree=$(freekb)kB) -> killing relay (guard hit $guard)"

    stop_g2
    if [ $guard -ge 3 ]; then BYE="memory guard"; break; fi
    sleep 15; heartbeat; sleep 15
    oc_on
    start_g2
    continue
  fi

  if [ ! -d /proc/$G2 ]; then
    restarts=$((restarts + 1))
    log "go2rtc pid $G2 is gone (known MP4-consumer panic); restart $restarts/$LIVE_MAX_RESTART"
    if [ $restarts -gt $LIVE_MAX_RESTART ]; then BYE="crash cap reached"; break; fi
    sleep $((restarts * 2))
    oc_on
    start_g2
    continue
  fi

  if [ $((beat % 12)) -eq 0 ]; then
    rss=$(awk '{print $2 * 4}' /proc/$G2/statm 2>/dev/null)
    log "alive: pid=$G2 rss=${rss}kB avail=${av}kB free=$(freekb)kB idle=${idle}s restarts=$restarts"
  fi

done

bye

