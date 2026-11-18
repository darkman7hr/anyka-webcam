#!/bin/sh
# One camera session, three questions about the SDK's SIGFPE:
#   does the crash frame track the GOP? (gop 5 vs gop 30)
#   does the rate-control mode matter?  (VBR vs CBR at gop 15)
# The handler in akstream prints a stack scan, so each run also names the SDK
# function that divided by zero.
#
#   sh /tmp/run_variants.sh
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

run()
{
	name=$1
	shift
	echo "=================== $name ==================="
	timeout -t 20 /tmp/akstream --dump /tmp/v.h264 --sub -W 640 -H 480 -F 20 -B 512 -t 5 --trace "$@"
	echo "$name rc=$?"
	rm -f /tmp/v.h264
	sleep 1
}

run "g1 gop 5"  -G 5
run "g2 gop 30" -G 30
run "g3 gop 15 cbr" -G 15 --cbr

cat /proc/loadavg
