#!/bin/sh
# Milestone-1 proof: two profiles that honour goplen >= fps, captured to files
# that stay on the camera so they can be pulled and checked with ffprobe.
#
#   sh /tmp/run_profiles.sh
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib

start_vendor()
{
	echo "== restoring vendor rtsp"
	V=$(ps | grep 'custom/rtsp' | grep -v grep | awk '{print $1}')
	[ -n "$V" ] && kill $V
	sleep 1
	setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
	sleep 3
	ps | grep custom/rtsp | grep -v grep
	netstat -tln 2>/dev/null | grep ':554' || echo "PROBLEM: nothing listening on 554"
}
trap start_vendor EXIT

V=$(ps | grep 'custom/rtsp' | grep -v grep | awk '{print $1}')
echo "vendor rtsp pid: ${V:-none}"
[ -n "$V" ] && kill $V
sleep 2
[ -n "$V" ] && kill -9 $V 2>/dev/null

echo '== preview profile: sub 640x480 @ 20 fps, gop 20, 512 kbps, 10 s =='
timeout -t 35 /tmp/akstream --dump /tmp/sub20.h264 --sub -W 640 -H 480 -F 20 -G 20 -B 512 -t 10
echo "sub rc=$?"

echo '== nvr profile: main 1280x720 @ 20 fps, gop 20, 1500 kbps, 10 s =='
timeout -t 35 /tmp/akstream --dump /tmp/main20.h264 --main -W 1280 -H 720 -F 20 -G 20 -B 1500 -t 10
echo "main rc=$?"

ls -l /tmp/sub20.h264 /tmp/main20.h264
df -h /tmp | tail -1
cat /proc/loadavg
