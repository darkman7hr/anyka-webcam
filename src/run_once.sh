#!/bin/sh
# Run one camera-side test binary while the vendor stream server is stopped, and
# put the vendor server back whatever the test does in between.
#
# This box has exactly one capture pipeline (/dev/isp_char, /dev/video0), so our
# binary and /mnt/Factory/custom/rtsp cannot hold it at the same time. That makes
# every test cost a few seconds of RTSP - keep the test short, and never leave
# the camera without a stream server.
#
#   sh /tmp/run_once.sh /tmp/akvi_probe 640 480
BIN=$1
shift
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib

echo "== running: $BIN $*"
V=$(ps | grep 'custom/rtsp' | grep -v grep | awk '{print $1}')
echo "vendor rtsp pid: ${V:-none}"
[ -n "$V" ] && kill $V
sleep 2
[ -n "$V" ] && kill -9 $V 2>/dev/null

timeout -t 30 "$BIN" "$@"
echo "rc=$?"

echo "== restoring vendor rtsp"
setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
sleep 3
ps | grep custom/rtsp | grep -v grep
netstat -tln 2>/dev/null | grep ':554' || echo "PROBLEM: nothing listening on 554"
cat /proc/loadavg
