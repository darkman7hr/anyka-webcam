#!/bin/sh
# Host-side A/B rig: sample the camera's AE state while the room light is switched
# on and off, so the day/night rule can be built from measured numbers.
#   sh tools/ae_ab.sh [mode] ...      modes to test in order (default: day night)
# Runs about 2.5 minutes per mode; read /tmp/ae_ab.log when it prints "done".
cd "$(dirname "$0")/.." || exit 1
LOG=/tmp/ae_ab.log
MODES=${*:-"day night"}
{
	for MODE in $MODES; do
		echo "=============== round mode=$MODE $(date +%T)"
		timeout 20 ./tools/cam.py "echo $MODE > /mnt/camweb/daynight.conf; sleep 6; echo before: \$(grep -m1 ^mode= /tmp/akrtsp.status) \$(grep -m1 ^ldr= /tmp/akrtsp.status)"
		timeout 20 ./tools/cam.py 'setsid /tmp/aewatch.sh 4 22 >/dev/null 2>&1 & echo sampler-started'
		sleep 14
		echo "-- light toggle 1 $(date +%T)"; /scripts/mqtt.light.sh 2>&1 | grep -E 'STATE|turning'
		sleep 30
		echo "-- light toggle 2 $(date +%T)"; /scripts/mqtt.light.sh 2>&1 | grep -E 'STATE|turning'
		sleep 30
		echo "-- light toggle 3 $(date +%T)"; /scripts/mqtt.light.sh 2>&1 | grep -E 'STATE|turning'
		sleep 20
		echo "-- ae log for mode=$MODE"
		timeout 20 ./tools/cam.py 'cat /tmp/ae.log'
		sleep 5
	done
	echo "done $(date +%T)"
} > "$LOG" 2>&1
echo "written $LOG"
