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

# Started by /mnt/Factory/config.sh at boot, where it inherits the boot
# environment rather than an interactive shell's. Without this the loop can be
# running with a PATH that finds no grep/netstat and silently do nothing - the
# same trap the web CGI documents.
PATH="/bin:/usr/bin:/sbin:/usr/sbin${PATH:+:$PATH}"
export PATH

INTERVAL=${1:-10}
MAXFAILS=${2:-3}
# Paths come from the environment so the failure paths can be exercised on a bench
# run with an isolated pidfile and log instead of this camera's live ones:
#   BIN=/tmp/nothing PIDF=/tmp/t_sup.pid SUP=/tmp/t_sup.log sh /tmp/run_akrtsp.sh 2 1
# BIN defaults to wherever this build is installed, not to one hard-coded path:
# the same script has to be right from /tmp during an experiment and from the card
# at boot. A supervisor that cannot find its binary falls back to the vendor
# server, and if the default were wrong that would happen quietly on every boot.
if [ -z "$BIN" ]; then
  if [ -x /mnt/camrtsp/akrtsp ]; then BIN=/mnt/camrtsp/akrtsp; else BIN=/tmp/akrtsp; fi
fi
LOG=${LOG:-/tmp/akrtsp.log}
SUP=${SUP:-/tmp/akrtsp_sup.log}
PIDF=${PIDF:-/tmp/akrtsp_sup.pid}
AK_ARGS=${AK_ARGS:---mode-file /mnt/camweb/daynight.conf --force-night night --ir-follow}

pids_by_comm()
{
	# `read' is a shell builtin: `$(cat /proc/$p/comm)' would fork once per
	# process, ~100 forks per round, and this loop runs every round.
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ -r "/proc/$p/comm" ] || continue
		read -r c < "/proc/$p/comm"
		[ "$c" = "$1" ] && echo "$p"
	done
}

say()
{
	echo "$(date '+%H:%M:%S') $*" >> "$SUP"
}

fails=0
idle_rounds=0

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
	AP=$(pids_by_comm akrtsp)
	if [ -n "$AP" ]; then
		fails=0
		# Alive is not the same as serving. A process that is stuck in teardown, or
		# wedged with its listening socket gone, looks perfectly healthy to `ps' and
		# is invisible to the NVR until someone notices a recording gap - and this
		# supervisor used to take the process list at its word. Three rounds of
		# "process up, nothing on :554" and it kills it, which drops the loop into the
		# "nothing owns :554" path below. Three rounds, not one, so a start that is
		# still bringing its socket up is never killed mid-bind.
		if netstat -tln 2>/dev/null | grep -q ':554'; then
			idle_rounds=0
		else
			idle_rounds=$((idle_rounds + 1))
			if [ "$idle_rounds" -ge 3 ]; then
				say "akrtsp (pid $AP) is alive but nothing is on :554; killing it"
				idle_rounds=0
				for p in $AP; do kill -9 "$p" 2>/dev/null; done
				sleep 2
				continue
			fi
		fi
		sleep "$INTERVAL"
		continue
	fi
	idle_rounds=0

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
