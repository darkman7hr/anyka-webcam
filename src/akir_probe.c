/*
 * akir_probe - read the light sensor the vendor app uses for day/night.
 *
 * The vendor's rtsp app polls ak_drv_ir_get_input_level() and switches the ISP
 * (its log shows "now set to night" / "[isp_switch:152] switching isp mode ->
 * night"). Our server never did either, which is how an image ends up with the
 * colour cast of one mode while the optics sit in the other. This reads the
 * same sensor so the question "day or night, and what does the camera think?"
 * can be answered with a number instead of a screenshot.
 *
 * It deliberately does NOT open the capture device, so it is safe to run while
 * akrtsp is streaming. Whether ak_drv_ir_init() succeeds here also tells us who
 * owns the IR hardware - ptz_daemon calls the same functions.
 *
 *   ./akir_probe              read level + thresholds
 *   ./akir_probe --ircut N    ask the filter to move (0 or 1), then re-read
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ak_drv_ir.h"

static const char *modename(enum ak_ain_check_mode m)
{
	switch (m) {
	case AIN_MODE_DAY_MIN:   return "day-min";
	case AIN_MODE_DAY_MAX:   return "day-max";
	case AIN_MODE_NIGTH_MIN: return "night-min";
	case AIN_MODE_NIGTH_MAX: return "night-max";
	case AIN_MODE_DAY_NIGTH: return "day+night";
	default:                 return "?";
	}
}

int main(int argc, char **argv)
{
	struct ak_ir_threshold day, night;
	int ircut_set = -1, i;
	int r, level;
	const char *ver;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--ircut") && i + 1 < argc)
			ircut_set = atoi(argv[++i]);
		else {
			printf("usage: %s [--ircut 0|1]\n", argv[0]);
			return 2;
		}
	}

	ver = ak_drv_ir_get_version();
	printf("ir driver version : %s\n", ver ? ver : "(none)");

	r = ak_drv_ir_init();
	printf("ak_drv_ir_init    : %d %s\n", r,
	       r ? "(failed - another process owns it, or no IR hardware)" : "(ok)");
	if (r)
		return 1;

	level = ak_drv_ir_get_input_level();
	printf("light input level : %d -> %s\n", level,
	       level == 1 ? "DAY" : level == 0 ? "NIGHT" : "read failed");

	memset(&day, 0, sizeof day);
	memset(&night, 0, sizeof night);
	r = ak_drv_ir_get_threshold(&day, &night);
	printf("ak_ir_get_threshold: %d\n", r);
	if (!r) {
		printf("  day   min=%d max=%d feature=%d\n", day.min, day.max, day.feature);
		printf("  night min=%d max=%d feature=%d\n", night.min, night.max, night.feature);
	}

	if (ircut_set >= 0) {
		r = ak_drv_ir_set_ircut(ircut_set);
		printf("ak_drv_ir_set_ircut(%d): %d\n", ircut_set, r);
		level = ak_drv_ir_get_input_level();
		printf("light input level after : %d\n", level);
	}

	printf("(check mode is a driver-internal setting; not touched here)\n");
	(void)modename(AIN_MODE_DAY_NIGTH);
	return 0;
}
