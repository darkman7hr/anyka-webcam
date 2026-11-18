#!/bin/sh
# Keep a stream server listening on :554. Neither the vendor binary nor akrtsp
# restarts itself: with no supervisor, one crash means the NVR stops recording and
# nobody notices until someone looks at a recording gap.
#
#   sh /tmp/run_akrtsp.sh              # check every 10 s, give up after 3 starts
#   sh /tmp/run_akrtsp.sh 5 5          # faster checks, more patience
#
# What it will and will not do, in priority order:
#   1. akrtsp alive                    -> nothing.
#   2. vendor rtsp alive               -> stand down and exit. Someone chose the
#      vendor server (run_takeover.sh --restore); stealing :554 back from a running
#      server would break the rollback path, which is the one thing that has to
#      keep working.
#   3. run_takeover.sh in flight       -> stand down for this round. The handover
#      stops akrtsp for 2 s on purpose while it moves from :8554 to :554; racing it
#      means two processes fighting for one capture pipeline ("vi init fail",
#      "VIDIOC_S_FMT, Device or resource busy").
#   4. nothing owns :554               -> start akrtsp, log it, count it. After
#      MAXFAILS consecutive failed starts, start the vendor server instead and
#      exit: a camera recording on the vendor binary beats a camera with a better
#      server that does not come up.
#
# Processes are matched by /proc/<pid>/comm, never by grepping ps - see the notes
# in run_takeover.sh (the vendor binary is /mnt/Factory/custom/rtsp or ./rtsp, and
# a word grep matches this script's own argv).
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib

INTERVAL=${1:-10}
MAXFAILS=${2:-3}
# Paths come from the environment so the failure paths can be exercised on a bench
# run with an isolated pidfile and log instead of this camera's live ones:
#   BIN=/tmp/nothing PIDF=/tmp/t_sup.pid SUP=/tmp/t_sup.log sh /tmp/run_akrtsp.sh 2 1
BIN=${BIN:-/tmp/akrtsp}
LOG=${LOG:-/tmp/akrtsp.log}
SUP=${SUP:-/tmp/akrtsp_sup.log}
PIDF=${PIDF:-/tmp/akrtsp_sup.pid}
AK_ARGS=${AK_ARGS:---force-night night}

pids_by_comm()
{
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ "$(cat /proc/$p/comm 2>/dev/null)" = "$1" ] && echo "$p"
	done
}

say()
{
	echo "$(date '+%H:%M:%S') $*" >> "$SUP"
}

fails=0

# Single instance. Two supervisors would both notice an empty :554 and start two
# akrtsp at each other, which on this board means the second one dies on
# "VIDIOC_S_FMT, Device or resource busy" while the first one streams - a crash
# loop that looks like akrtsp being unreliable. Take over the old one by pidfile.
if [ -f "$PIDF" ]; then
	OLD=$(cat "$PIDF" 2>/dev/null)
	if [ -n "$OLD" ] && grep -q run_akrtsp /proc/$OLD/cmdline 2>/dev/null; then
		kill "$OLD" 2>/dev/null
		sleep 1
	fi
fi
echo $$ > "$PIDF"

say "supervisor up: interval=${INTERVAL}s maxfails=${MAXFAILS} args='$AK_ARGS' bin=$(md5sum $BIN 2>/dev/null | cut -c1-8)"

while :; do
	if [ -n "$(pids_by_comm akrtsp)" ]; then
		fails=0
		sleep "$INTERVAL"
		continue
	fi

	if [ -n "$(pids_by_comm rtsp)" ]; then
		say "vendor rtsp owns the stream, standing down"
		exit 0
	fi

	# A handover in flight, detected from cmdline rather than comm: a script run
	# as "sh /tmp/run_takeover.sh" has comm "sh", so matching script names in comm
	# finds nothing. And the grep must not match ITSELF: "grep -l run_takeover
	# /proc/*/cmdline" is itself a process whose cmdline contains run_takeover, so
	# it matched, the supervisor believed a handover was always in progress, and it
	# refused to restart a killed akrtsp - :554 sat empty for 20 s until the log
	# line made the reason obvious. The [r] bracket trick is the same one ps|grep
	# needs (tr is not in this shell's PATH, so that is the other option gone).
	if [ -n "$(grep -l '[r]un_takeover' /proc/[0-9]*/cmdline 2>/dev/null)" ]; then
		say "a handover is running, standing down this round"
		sleep "$INTERVAL"
		continue
	fi

	# Nothing owns the port: this is the case the whole script exists for.
	if [ ! -x "$BIN" ]; then
		if [ -n "$NOFALLBACK" ]; then
			say "no $BIN on disk; NOFALLBACK set, not touching the vendor server"
			exit 1
		fi
		say "no $BIN on disk; falling back to the vendor server"
		setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
		exit 1
	fi

	# akrtsp --verbose writes a line per frame on this pipeline; /tmp is tmpfs on
	# a 32 MB box that has been OOM-killed before, so rotate instead of growing.
	if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 400000 ]; then
		mv -f "$LOG" "$LOG.1"
	fi

	fails=$((fails + 1))
	say "nothing on :554; starting akrtsp (attempt $fails/$MAXFAILS)"
	setsid "$BIN" --port 554 --verbose $AK_ARGS >> "$LOG" 2>&1 &
	sleep 6
	# Success means the process this script started is alive AND the port is up.
	# Checking only the port is wrong: something else can be holding :554 (a
	# wedged previous server, or a bench run next to a live one), and the supervisor
	# then reports a start that never happened.
	if [ -n "$(pids_by_comm akrtsp)" ] && netstat -tln 2>/dev/null | grep -q ':554'; then
		say "akrtsp up on :554 (pid $(pids_by_comm akrtsp))"
		continue
	fi

	say "akrtsp failed to bind :554"
	for p in $(pids_by_comm akrtsp); do kill -9 "$p" 2>/dev/null; done
	sleep 2
	if [ "$fails" -ge "$MAXFAILS" ]; then
		if [ -n "$NOFALLBACK" ]; then
			say "$fails failed starts in a row; NOFALLBACK set, giving up without the vendor server"
			exit 1
		fi
		say "$fails failed starts in a row; starting the vendor server and giving up"
		setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
		sleep 4
		exit 1
	fi
done
