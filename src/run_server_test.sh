#!/bin/sh
# Run akrtsp beside the production stream path: it takes the capture pipeline
# (one per board, so the vendor server has to stop for the test) but listens on
# 8554, so a mistake in our RTSP implementation cannot cost the NVR its port.
#
# The trap matters more than anywhere else in this repo: this script is the only
# thing standing between the test and a camera with no stream server at all.
#
#   sh /tmp/run_server_test.sh
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib

# FTP upload leaves the mode at 644; without this the server "starts", fails to
# exec, and the only symptom is a port nobody answers.
chmod +x /tmp/akrtsp 2>/dev/null

# Match processes by /proc/<pid>/comm, not by grepping ps: the vendor server runs
# as /mnt/Factory/custom/rtsp from the init scripts but as "./rtsp" when restarted
# by hand, and a path-based grep silently matches nothing - which leaves it holding
# the capture device and every later test fails with "Device or resource busy".
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
	# Order matters: the vendor server fails with "vi init fail" if it starts while
	# akrtsp still holds the single VI device, and then the camera is left with no
	# stream server at all. Kill ours, wait for it to be really gone, then start.
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
# same race in the other direction: the vendor server's threads take a second or
# two to release /dev/video and the ISP, and akrtsp then fails with
# "VIDIOC_S_FMT, Device or resource busy". Wait for it to be gone.
i=0
while [ -n "$(rtsp_pids)" ]; do
	[ $i -ge 10 ] && break
	sleep 1
	i=$((i + 1))
done
[ -n "$V" ] && kill -9 $V 2>/dev/null
sleep 1

echo "== starting akrtsp on :8554"
setsid /tmp/akrtsp --port 8554 --verbose > /tmp/akrtsp.log 2>&1 &
sleep 6
echo "-- log so far --"
tail -6 /tmp/akrtsp.log
if ! netstat -tln 2>/dev/null | grep -q ':8554'; then
	echo "FAILED: akrtsp is not listening on 8554, giving the pipeline back"
	exit 1
fi

i=0
while [ $i -lt 16 ]; do
	sleep 5
	P=$(ak_pids)
	echo "-- t=$((i * 5))s pid=${P:-GONE} --"
	[ -z "$P" ] && break
	top -b -n 1 2>/dev/null | grep -E 'CPU:|akrtsp'
	free | grep Mem
	i=$((i + 1))
done

K=$(ak_pids)
[ -n "$K" ] && kill $K
sleep 2
echo "== akrtsp log =="
tail -20 /tmp/akrtsp.log
echo "== kernel tail (OOM killer leaves traces here) =="
dmesg 2>/dev/null | tail -6
cat /proc/loadavg
