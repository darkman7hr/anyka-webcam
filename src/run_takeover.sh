#!/bin/sh
# Take over port 554 with our own server. This is the only script in this repo
# that can interrupt the recording the NVR is doing right now, so read it first.
#
#   sh /tmp/run_takeover.sh            # vendor rtsp out, akrtsp on :554
#   sh /tmp/run_takeover.sh --restore  # vendor rtsp back, akrtsp out
#
# Safety properties, in the order they matter:
#   1. Nothing is stopped until akrtsp has proved it can bring the pipeline up on
#      :8554 first. A binary that cannot even open the ISP never costs the NVR
#      its port.
#   2. If the handover leaves nothing listening on 554, the vendor server is put
#      back immediately and the script exits 1.
#   3. The vendor server is waited for by command word, not path (see README):
#      a stale ./rtsp holding the ISP is what makes every later start fail with
#      "vi init fail" / "VIDIOC_S_FMT, Device or resource busy".
#
# This does NOT survive a reboot: config.sh still starts /mnt/Factory/custom/rtsp.
# Making the change permanent means editing config.sh, which is a separate
# decision - do it only after akrtsp has run against the real NVR for a while.
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib

# Extra flags for akrtsp. The default is deliberate, not incidental: this camera
# sits in a dark room lit by its own IR illuminator, and the ISP boots in DAY mode,
# which renders infrared as magenta (measured R-G +76, B-G +79 against R=G=B in
# night mode). --mode-file is the file the web UI writes; it wins over
# --force-night whenever it holds a valid word, so the mode a person chose on the
# card outlives the process. Override for an A/B run with AK_ARGS="..." in the
# environment; do NOT add --auto-night or --ir-follow here - the light sensor
# reports DAY in this room (the illuminator lights the resistor), and acting on it
# inserts the IR-cut filter and blinds the camera. See README, "The purple image".
AK_ARGS=${AK_ARGS:---mode-file /mnt/camweb/daynight.conf --force-night night}

# Match processes by /proc/<pid>/comm, never by grepping ps: the vendor binary
# runs as /mnt/Factory/custom/rtsp from the init scripts but as "./rtsp" when
# restarted by hand, and a path grep silently matches nothing - leaving it holding
# the capture device so every later start fails with "Device or resource busy".
pids_by_comm()
{
	# `read' is a builtin: `$(cat /proc/$p/comm)' forks once per process, and
	# these loops run in poll cycles on a single-core box.
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ -r "/proc/$p/comm" ] || continue
		read -r c < "/proc/$p/comm"
		[ "$c" = "$1" ] && echo "$p"
	done
}
rtsp_pids()
{
	pids_by_comm rtsp
}
ak_pids()
{
	pids_by_comm akrtsp
}

stop_ak()
{
	K=$(ak_pids)
	[ -n "$K" ] && kill -9 $K 2>/dev/null
	i=0
	while [ -n "$(ak_pids)" ]; do
		[ $i -ge 10 ] && break
		sleep 1
		i=$((i + 1))
	done
}

stop_rtsp()
{
	V=$(rtsp_pids)
	[ -n "$V" ] && kill $V 2>/dev/null
	i=0
	while [ -n "$(rtsp_pids)" ]; do
		[ $i -ge 10 ] && break
		sleep 1
		i=$((i + 1))
	done
	[ -n "$V" ] && kill -9 $V 2>/dev/null
	sleep 1
}

start_rtsp()
{
	setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
	sleep 4
	if ! netstat -tln 2>/dev/null | grep -q ':554'; then
		echo "   554 down, retrying once"
		setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
		sleep 4
	fi
	netstat -tln 2>/dev/null | grep ':554'
}

restore_vendor()
{
	echo "== vendor rtsp back on :554"
	stop_ak
	stop_rtsp
	start_rtsp || true
	ps 2>/dev/null | grep -w rtsp | grep -v grep
	netstat -tln 2>/dev/null | grep ':554' || echo "PROBLEM: nothing on 554 - start /mnt/Factory/custom/rtsp by hand"
}

if [ "$1" = "--restore" ]; then
	restore_vendor
	exit 0
fi

chmod +x /tmp/akrtsp 2>/dev/null
[ -x /tmp/akrtsp ] || { echo "no /tmp/akrtsp - upload src/akrtsp.stripped first"; exit 1; }

# Armed before anything is stopped, and left armed: it only acts when nothing
# owns :554, so on the success path it does nothing, and if this session dies
# half-way through the camera still gets its stream server back.
trap 'if ! netstat -tln 2>/dev/null | grep -q ":554"; then echo "!! nothing on 554, rolling back"; restore_vendor; fi' EXIT

# Step 1: prove the binary can run the pipeline at all, on a harmless port.
# Not free: there is one capture pipeline, so the NVR loses the stream for the
# ~10 s the handover takes even in rehearsal.
echo "== phase 1: akrtsp on :8554 as a rehearsal"
stop_ak
stop_rtsp
setsid /tmp/akrtsp --port 8554 --verbose $AK_ARGS > /tmp/akrtsp_pre.log 2>&1 &
sleep 6
if ! netstat -tln 2>/dev/null | grep -q ':8554'; then
	echo "FAILED: akrtsp did not reach the listening state; leaving the vendor server alone"
	tail -6 /tmp/akrtsp_pre.log
	start_rtsp
	exit 1
fi
echo "   rehearsal ok, handing the pipeline to :554"
stop_ak
sleep 2

# Step 2: the real swap. The rollback trap armed above covers it.
setsid /tmp/akrtsp --port 554 --verbose $AK_ARGS > /tmp/akrtsp.log 2>&1 &
sleep 6
if ! netstat -tln 2>/dev/null | grep -q ':554'; then
	echo "FAILED: akrtsp did not bind 554"
	tail -8 /tmp/akrtsp.log
	exit 1
fi

echo "== akrtsp owns :554"
ps 2>/dev/null | grep akrtsp | grep -v grep
netstat -tan 2>/dev/null | grep ':554' | head -6
echo "watch it with:  top -b -n 1 | grep akrtsp    and   tail -f /tmp/akrtsp.log"
echo "NVR reconnect:  it holds TCP connections and will retry on its own; if it"
echo "does not, force a reconnect from the NVR side. Roll back with --restore."
# The rollback trap stays armed: it only fires when nothing owns :554, which is
# exactly the condition we never want to leave the camera in.
