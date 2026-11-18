#!/bin/sh
# Round trip test of the auto day/night policy on the live camera.
#
#   sh tools/auto_check.sh            run the whole cycle (~6 min, in background)
#
# Three phases, each sampled every 8 s into /tmp/auto_check.log plus a colour
# signature of the real frame at the end of each phase:
#
#   A  dark room, control file "auto"  -> must settle on night, lamp on, filter out,
#                                         and the frame must stay neutral (R=G=B).
#                                         This is the case the light sensor got wrong
#                                         and the one that produced the purple picture.
#   B  room light on                   -> gain drops, probe runs with the lamp out,
#                                         probe agrees, so: day, lamp off, filter in.
#   C  room light off again            -> gain pegs, so back to night, lamp on.
#   D  control file back to night      -> production default restored.
#
# The room light is driven through /scripts/mqtt.light.sh, which toggles rather than
# sets, so each phase records the state it saw. It ends with the light off.
cd "$(dirname "$0")/.." || exit 1
LOG=/tmp/auto_check.log
ST='grep -E "^(mode|auto|auto_reason|auto_lamp|auto_probe|ae_again)=" /tmp/akrtsp.status'

sample() {
	echo "-- $1 $(date +%T)"
	timeout 20 ./tools/cam.py "$ST; echo ir=\$(head -c1 /sys/user-gpio/IR_LED) cut=\$(head -c1 /sys/user-gpio/gpio-ircut_a)" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
	timeout 40 ./tools/rgbshot.py 2>&1 | grep -iE 'mean|R-G|B-G|neutral' | head -4
}

{
	echo "=== auto round trip $(date +%T) ==="
	timeout 20 ./tools/cam.py 'echo auto > /mnt/camweb/daynight.conf; echo control=$(cat /mnt/camweb/daynight.conf)'
	timeout 20 ./tools/cam.py 'setsid /tmp/aewatch.sh 8 60 >/dev/null 2>&1 & echo sampler'
	sleep 70
	sample "A dark, auto (expect night, lamp on, neutral frame)"
	echo "-- light ON $(date +%T)"; /scripts/mqtt.light.sh 2>&1 | grep -E 'STATE|turning'
	# Long enough for the whole night -> day path: --auto-lit-secs of low gain,
	# --auto-dwell-secs of holding night, then the probe (lamp out --auto-probe-secs
	# with a settle window) and its verdict. Shorten this and phase B merely catches
	# the probe mid-flight and looks like a failure.
	sleep 130
	sample "B lit (expect probe then day, lamp off, filter in)"
	echo "-- light OFF $(date +%T)"; /scripts/mqtt.light.sh 2>&1 | grep -E 'STATE|turning'
	sleep 90
	sample "C dark again (expect night, lamp on)"
	timeout 20 ./tools/cam.py 'echo night > /mnt/camweb/daynight.conf; echo control=$(cat /mnt/camweb/daynight.conf)'
	sleep 12
	sample "D restored to manual night"
	echo "-- camera log tail"
	timeout 20 ./tools/cam.py 'tail -25 /tmp/akrtsp_sup.log' 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
	echo "-- day/night decisions (akrtsp.log)"
	timeout 20 ./tools/cam.py 'grep -iE "auto|day/night" /tmp/akrtsp.log | tail -45' 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
	echo "-- sampler log"
	timeout 20 ./tools/cam.py 'cat /tmp/ae.log' 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
	echo "done $(date +%T)"
} > "$LOG" 2>&1
echo "written $LOG"
