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
# libmpi_osd.so - the ISP overlay that burns the clock into the picture - is not
# in the vendor's lib directory. It comes from the same rootfs_v1basic tree as the
# other libraries this build links against and is deployed to /mnt/camrtsp/lib, so
# ours is searched first and /mnt/Factory stays untouched. Worth knowing what
# happens if that file is ever missing: the loader refuses to start akrtsp at all,
# the supervisor counts it as a failed start and falls back to the vendor server -
# a camera recording without a clock overlay beats one that records nothing.
LD_LIBRARY_PATH="/mnt/camrtsp/lib:/mnt/Factory/custom/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

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
# The clock that gets burned into the picture is printed in the zone named by the
# same conf the clock supervisor reads, so there is exactly one place that says
# what zone this camera is in. It has to be a POSIX rule (CET-1CEST,M3.5.0,
# M10.5.0/3) because uClibc here carries no zoneinfo database. It overrides the
# vendor's TZ=GMT-08:00 on purpose: that is the vendor's own "timezone:8", not this
# network's.
TIME_CONF=${TIME_CONF:-/mnt/camweb/time/time.conf}
if [ -f "$TIME_CONF" ]; then
	. "$TIME_CONF"
	[ -n "$TZ_STR" ] && TZ="$TZ_STR" && export TZ
fi
# The arguments. A file on the card wins over the environment, because the file is
# what the tools change and what survives a reboot: editing the AK_ARGS in
# config.sh means restarting the supervisor to pick it up, and the supervisor is
# the thing that holds :554. The environment still works when there is no file,
# which is what keeps a bench run (`AK_ARGS="--port 8554 ..." sh run_akrtsp.sh`)
# possible; ARGS_FILE moves the file, so a bench run can use its own.
# Whatever wins, the round says which file it came from: two places that can hold
# the arguments is a trap unless the log names the one that was used.
ARGS_FILE=${ARGS_FILE:-/mnt/camweb/akrtsp.args}
if [ -r "$ARGS_FILE" ]; then
	AK_ARGS=$(cat "$ARGS_FILE" 2>/dev/null)
	ARGS_SRC=$ARGS_FILE
else
	ARGS_SRC=environment
fi
AK_ARGS=${AK_ARGS:---mode-file /mnt/camweb/daynight.conf --force-night night --ir-follow --osd-time --osd-pos br}

pids_by_comm()
{
	# `read' is a shell builtin: `$(cat /proc/$p/comm)' would fork once per
	# process, ~100 forks per round, and this loop runs every round.
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ -r "/proc/$p/comm" ] || continue
		# 2>/dev/null because the test and the read are not one step: a process
		# that dies in between would otherwise print into the supervisor's log
		# every round, and a log full of noise hides the line that matters. The
		# 2>/dev/null goes before the input redirect because ash applies
		# redirections left to right: "read c < f 2>/dev/null" still reports the
		# failed <f on the shell's stderr (measured on this board).
		read -r c 2>/dev/null < "/proc/$p/comm" || continue
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

say "supervisor up: interval=${INTERVAL}s maxfails=${MAXFAILS} args='$AK_ARGS' args_from=$ARGS_SRC bin=$(md5sum $BIN 2>/dev/null | cut -c1-8)"

# The clock, once, before anything starts counting frames against it.
#
# akrtsp's RTP timestamps count frames at 90000/fps and only creep FORWARD
# against the wall clock, so a step under a running stream - tens of seconds, or
# the 56 years that the first sync of every boot is on a board with no rtc - is a
# hole every client has to decode around. Taking the step here, where nothing is
# streaming yet, is what keeps it out of the recording.
#
# Bounded and never fatal: 30 s, and only when the clock service is installed at
# all. A camera that cannot reach an NTP peer still has to stream; what it loses
# is the date, which akrtsp prints as NO TIME rather than as a wrong one.
if [ -r /mnt/camweb/time/run_time.sh ]; then
	i=0
	while [ ! -r /tmp/camweb_time.status ] && [ $i -lt 30 ]; do
		sleep 1
		i=$((i+1))
	done
	if [ -r /tmp/camweb_time.status ]; then
		say "clock: $(grep -h '^state=' /tmp/camweb_time.status 2>/dev/null) " \
			"$(grep -h '^peer=' /tmp/camweb_time.status 2>/dev/null) after ${i}s"
	else
		say "clock: nothing written by /mnt/camweb/time after 30 s; starting anyway"
	fi
fi

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
	# Where the log ended before this start, so the reason for a failed start can
	# be quoted from it below. Line count rather than byte count because busybox
	# tail is not guaranteed to understand "-c +N", but "-n +N" is everywhere.
	MARK=$(wc -l < "$LOG" 2>/dev/null || echo 0)
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
	# Quote what the child actually said. The supervisor can only see "no process,
	# no port"; the reason (an argument this build does not know, a bind error, a
	# vendor library that refused the pipeline) is in the log the child just wrote,
	# and it was the one thing missing when three starts failed over a flag name.
	tail -n +$((MARK + 1)) "$LOG" 2>/dev/null | tail -4 |
		while IFS= read -r line; do say "  child: $line"; done
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
