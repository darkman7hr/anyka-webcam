#!/bin/sh
# Idle-lifetime and leak check: start akrtsp with NO client attached and watch it
# for ~70 s. What this settles: whether the server survives on its own (it does -
# measured 20.22-20.26 fps steady with no client), and whether the SDK or our own
# rings eat memory over time (watch the "Mem:" line every 5 s; it must stay flat).
#
# Measured on the first run of this test: 65 s with no client, 1422 main frames,
# 20.22 fps, and no upward drift in free memory.
#
#   sh /tmp/run_idle_test.sh
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib
chmod +x /tmp/akrtsp 2>/dev/null

# Match processes by /proc/<pid>/comm, not by grepping ps: the vendor server runs
# as /mnt/Factory/custom/rtsp from the init scripts but as "./rtsp" when restarted
# by hand, and a path grep then matches nothing - leaving it holding the capture
# device so every later test fails with "Device or resource busy".
pids_by_comm()
{
	for p in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
		[ "$(cat /proc/$p/comm 2>/dev/null)" = "$1" ] && echo "$p"
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

start_vendor()
{
	echo "== restoring vendor rtsp"
	# Kill ours first and wait for the VI device to actually be released: starting
	# the vendor server too early makes it die with "vi init fail" and the camera
	# is then left with no stream server at all.
	K=$(ak_pids)
	if [ -n "$K" ]; then
		kill -9 $K 2>/dev/null
		i=0
		while [ -n "$(ak_pids)" ]; do
			[ $i -ge 10 ] && break
			sleep 1
			i=$((i + 1))
		done
	fi
	V=$(rtsp_pids)
	[ -n "$V" ] && kill $V 2>/dev/null
	sleep 1
	setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
	sleep 4
	if ! netstat -tln 2>/dev/null | grep -q ':554'; then
		echo "   554 not up yet, retrying"
		setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
		sleep 4
	fi
	ps 2>/dev/null | grep -w rtsp | grep -v grep
	netstat -tln 2>/dev/null | grep ':554' || echo "PROBLEM: nothing listening on 554 - start /mnt/Factory/custom/rtsp by hand"
}
trap start_vendor EXIT

V=$(rtsp_pids)
echo "vendor rtsp pid: ${V:-none}"
[ -n "$V" ] && kill $V 2>/dev/null
i=0
while [ -n "$(rtsp_pids)" ]; do
	[ $i -ge 10 ] && break
	sleep 1
	i=$((i + 1))
done
[ -n "$V" ] && kill -9 $V 2>/dev/null
sleep 1

echo "== starting akrtsp on :8554, no client will connect"
setsid /tmp/akrtsp --port 8554 --verbose > /tmp/akrtsp_idle.log 2>&1 &
sleep 6
netstat -tln 2>/dev/null | grep -q ':8554' || { echo "FAILED: not listening"; exit 1; }
free | grep Mem

i=0
while [ $i -lt 14 ]; do
	sleep 5
	P=$(ak_pids)
	echo "-- t=$((i * 5))s  pid=${P:-GONE}"
	[ -z "$P" ] && break
	free | grep Mem
	i=$((i + 1))
done

K=$(ak_pids)
[ -n "$K" ] && kill $K
sleep 2
echo "== log tail =="
tail -12 /tmp/akrtsp_idle.log
