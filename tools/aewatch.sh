#!/bin/sh
# Sample the ISP's auto-exposure state into a log, for the day/night rule design.
#
#   ./aewatch.sh [seconds] [samples]        default: every 4 s, 20 samples
#
# Written because the light dependent resistor on this camera does not respond
# (A/B: room light on and off both read 1 = DAY), so the only usable signal is
# what the ISP's AE loop reports: the luminance it is holding, its own day/dark
# flag, and the gain and integration time it needed to get there. Toggle the room
# light while this runs and read the log back.

N=${1:-4}
M=${2:-20}
LOG=${LOG:-/tmp/ae.log}

: > "$LOG"
i=0
while [ "$i" -lt "$M" ]; do
	i=$((i + 1))
	{
		printf '%s ' "$(date +%H:%M:%S)"
		# busybox here has no `tr' applet, so the compact one-line form is done
		# inside awk instead of piping grep through tr.
		awk -F= 'BEGIN{ORS=" "} \
			$1=="mode"||$1=="ldr"||$1=="ae_lumi"||$1=="ae_again"||$1=="ae_exp" \
			||$1=="auto"||$1=="auto_reason"||$1=="auto_lamp"||$1=="auto_probe" \
			{printf "%s=%s ", $1, $2}' /tmp/akrtsp.status 2>/dev/null
		printf 'ir=%s cut=%s' "$(head -c1 /sys/user-gpio/IR_LED 2>/dev/null)" \
			"$(head -c1 /sys/user-gpio/gpio-ircut_a 2>/dev/null)"
		echo ""
	} >> "$LOG"
	sleep "$N"
done
cat "$LOG"
