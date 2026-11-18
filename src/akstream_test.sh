#!/bin/sh
# Milestone-1 hardware test for akstream: own the capture pipeline for six
# seconds, dump the elementary stream, and put the vendor server back no matter
# what our binary does in between. The camera has one capture pipeline, so the
# two cannot run at the same time - this is the shortest window that still lets
# ffprobe count frames and key frames.
#
# Run as: sh /tmp/akstream_test.sh   (from tools/rtsp.py or by hand)
BIN=/tmp/akstream
LIB=/mnt/Factory/custom/lib
OUT=/tmp/out.h264
export LD_LIBRARY_PATH=$LIB

echo "== binary"
md5sum $BIN
ls -l $BIN

echo "== stopping vendor rtsp"
V=$(ps | grep 'custom/rtsp' | grep -v grep | awk '{print $1}')
echo "vendor rtsp pid: ${V:-none}"
[ -n "$V" ] && kill $V
sleep 2
[ -n "$V" ] && kill -9 $V 2>/dev/null

echo "== akstream (640x480 asked for 20 fps, gop 15, 512 kbps)"
rm -f $OUT
timeout -t 25 $BIN --dump $OUT -W 640 -H 480 -F 20 -G 15 -B 512 -t 6
echo "rc=$?"
ls -l $OUT 2>/dev/null

echo "== restoring vendor rtsp"
setsid /mnt/Factory/custom/rtsp >>/tmp/rtsp_run.log 2>&1 &
sleep 3
ps | grep custom/rtsp | grep -v grep
netstat -tln 2>/dev/null | grep ':554' || echo "PROBLEM: nothing listening on 554"
echo "== load"
cat /proc/loadavg
free | sed -n 3p
