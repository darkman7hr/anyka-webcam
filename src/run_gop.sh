#!/bin/sh
# Is the encoder's div-by-zero at the GOP boundary frame-count based or time
# based? Four runs separate the two, because the crash frame number is the
# measurement: it prints "SIGFPE after N frames" from akstream's handler.
#
#   sh /tmp/run_gop.sh
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
	fps=$2
	gop=$3
	secs=$4
	echo "=================== $name (fps $fps gop $gop, ${secs}s) ==================="
	timeout -t $((secs + 15)) /tmp/akstream --dump /tmp/g.h264 --sub -W 640 -H 480 -F "$fps" -G "$gop" -B 512 -t "$secs" --trace
	echo "$name rc=$?"
	rm -f /tmp/g.h264
	sleep 1
}

# crash at 9 frames? (frame-count rule)
run "r1" 20 10 6
# same gop, half the fps: if it still dies at frame 9 the divisor is frame
# counted, if it lives the divisor is time based
run "r2" 10 10 6
# does the crash frame simply follow goplen?
run "r3" 20 20 8
# gop 30 at 15 fps: GOP period 2 s, boundary reached at ~frame 30 inside 12 s
run "r4" 15 30 12

cat /proc/loadavg
