#!/bin/sh
# One camera interruption instead of three: map what the SDK accepts, capture a
# sub-channel and a main-channel stream, sample CPU while encoding, then put the
# vendor stream server back.
#
# This box has exactly one capture pipeline (/dev/isp_char, /dev/video0), so our
# binaries and /mnt/Factory/custom/rtsp cannot hold it at the same time. Every
# test therefore costs a few seconds of RTSP - keep tests short, and never leave
# the camera without a stream server, hence the EXIT trap.
#
#   sh /tmp/run_batch.sh
#
# busybox sh here: no xargs -r, no bash arrays, keep it POSIX.
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

echo '== sizes/fps the SDK accepts =='
timeout -t 25 /tmp/akvi_probe
echo "probe rc=$?"

echo '== capture: sub channel 640x480 @ 20 fps, gop 15, 512 kbps, 8 s =='
timeout -t 30 /tmp/akstream --dump /tmp/sub.h264 --sub -W 640 -H 480 -F 20 -G 15 -B 512 -t 8
echo "akstream(sub) rc=$?"
ls -l /tmp/sub.h264 2>/dev/null
head -c 24 /tmp/sub.h264 2>/dev/null | od -An -tx1

echo '== capture: main channel 1280x720 @ 20 fps, gop 15, 1500 kbps, 10 s =='
( sleep 4; echo '-- top during main capture --'; top -b -n 1 2>/dev/null | head -12 ) &
timeout -t 35 /tmp/akstream --dump /tmp/main.h264 --main -W 1280 -H 720 -F 20 -G 15 -B 1500 -t 10
echo "akstream(main) rc=$?"
wait
ls -l /tmp/main.h264 2>/dev/null

cat /proc/loadavg
free | head -3
