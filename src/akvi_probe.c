/*
 * akvi_probe - ask the camera's own libplat_vi which channel sizes it accepts.
 *
 * Findings that made this program necessary, all measured on the board:
 *
 * 1. The published ak_vi.h struct video_channel_attr is 48 bytes and carries
 *    max_width/max_height per channel. The validator inside ak_vi_set_channel_attr
 *    (libplat_vi V2.3.01, md5 f924ed82e70147046ff460e6b543fe86) reads main w/h at
 *    offsets 16/20 and sub w/h at 24/28 - two ints per channel. Filling the
 *    documented struct puts main's max pair where the library reads the sub
 *    channel, so every documented attempt fails on a field we never set:
 *      max_* = 0        -> "sub channel argument error, w: 0, h: 0"
 *      max_* = 1280x720 -> "sub channel argument error, w: 1280, h: 720"
 *    src/vi_abi.h encodes the layout this program measured.
 *
 * 2. ak_vi_get_channel_attr() is declared in the header and SEGFAULTS in this
 *    build, so the layout cannot be read back - it has to be probed.
 *
 * 3. ak_vi_set_fps() returns 0 for 15/20/25 but ak_vi_get_fps() reports 30
 *    before and after every call, so get_fps is not a usable confirmation of
 *    frame rate; only counting delivered frames (akstream) is.
 *
 * Run it while nothing else owns the pipeline: sh /tmp/run_once.sh /tmp/akvi_probe
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ak_global.h"
#include "ak_vi.h"
#include "vi_abi.h"

struct cand {
	const char *tag;
	int mw, mh, sw, sh;
};

/* Bounds the validator enforces, plus the profiles we would actually run, plus
 * two deliberately illegal pairs to confirm the limits instead of assuming them. */
static const struct cand cands[] = {
	{"1280x720 / 640x480      ", 1280, 720, 640, 480},
	{"1280x720 / 320x240      ", 1280, 720, 320, 240},
	{"960x540 / 640x480       ", 960, 540, 640, 480},
	{"960x540 / 320x240       ", 960, 540, 320, 240},
	{"704x576 / 352x288       ", 704, 576, 352, 288},
	{"800x600 / 400x300       ", 800, 600, 400, 300},
	{"640x480 / 320x240       ", 640, 480, 320, 240},
	{"1024x576 / 512x288      ", 1024, 576, 512, 288},
	{"1280x960 / 640x480 hi   ", 1280, 960, 640, 480},
	{"640x479 / 320x240 lo    ", 640, 479, 320, 240},
	{"639x480 / 320x240 lo    ", 639, 480, 320, 240},
	{"1280x720 / 641x480 hi   ", 1280, 720, 641, 480},
};

int main(void)
{
	void *vi;
	struct video_resolution res;
	struct vi_channel_attr_real attr;
	unsigned int i;
	int ok_main = 0, ok_sub = 0;

	printf("libplat_vi %s\n", ak_vi_get_version());
	if (ak_vi_match_sensor("/etc/jffs2/")) {
		fprintf(stderr, "match_sensor failed (is the vendor rtsp still running?)\n");
		return 1;
	}
	vi = ak_vi_open(VIDEO_DEV0);
	if (!vi) {
		fprintf(stderr, "ak_vi_open failed\n");
		return 1;
	}

	memset(&res, 0, sizeof(res));
	printf("get_sensor_resolution rc=%d -> w=%d h=%d max_w=%d max_h=%d\n",
		ak_vi_get_sensor_resolution(vi, &res), res.width, res.height,
		res.max_width, res.max_height);
	printf("sizeof published struct = %d, sizeof struct this library reads = %d\n",
		(int)sizeof(struct video_channel_attr), (int)sizeof(struct vi_channel_attr_real));

	for (i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
		const struct cand *c = &cands[i];
		int rc;

		memset(&attr, 0, sizeof(attr));
		attr.crop_left = 0;
		attr.crop_top = 0;
		attr.crop_width = res.width ? res.width : 1280;
		attr.crop_height = res.height ? res.height : 720;
		attr.main_width = c->mw;
		attr.main_height = c->mh;
		attr.sub_width = c->sw;
		attr.sub_height = c->sh;
		rc = ak_vi_set_channel_attr(vi, (struct video_channel_attr *)&attr);
		fflush(stdout);
		printf("set %s -> rc=%d\n", c->tag, rc);
		fflush(stdout);
		if (!rc) {
			ok_main = c->mw;
			ok_sub = c->sw;
		}
	}

	printf("\n-- fps (last accepted: main %d, sub %d) --\n", ok_main, ok_sub);
	printf("get_fps before           -> %d\n", ak_vi_get_fps(vi));
	printf("set_fps(20)              -> %d\n", ak_vi_set_fps(vi, 20));
	fflush(stdout);
	printf("get_fps after set_fps(20)-> %d\n", ak_vi_get_fps(vi));
	printf("set_switch_fps_enable(0) -> %d\n", ak_vi_set_switch_fps_enable(vi, 0));
	fflush(stdout);
	printf("get_fps after that       -> %d\n", ak_vi_get_fps(vi));
	printf("switch_mode(1)           -> %d\n", ak_vi_switch_mode(vi, 1));
	fflush(stdout);

	ak_vi_capture_on(vi);
	ak_vi_close(vi);
	return 0;
}
