/*
 * vi_abi.h - the channel attribute struct libplat_vi V2.3.01 on this board
 * actually reads, which is NOT the struct the published ak_vi.h declares.
 *
 * Published header:
 *   struct video_resolution   { int width, height, max_width, max_height; };
 *   struct video_channel_attr { struct crop_info crop;
 *                               struct video_resolution res[VIDEO_CHN_NUM]; };
 *   -> sizeof == 48
 *
 * What the validator inside ak_vi_set_channel_attr (libplat_vi V2.3.01, md5
 * f924ed82e70147046ff460e6b543fe86) reads, from objdump of that function:
 *   crop left / top / width / height   at 0 / 4 / 8 / 12
 *   main width / height                at 16 / 20   (needs 640<=w<=1280,
 *                                                    480<=h<=960)
 *   sub  width / height                at 24 / 28   (needs w<=640, h<=480)
 * i.e. each channel entry is two ints, not four: the max_width/max_height the
 * header declares do not exist in the struct the library sees (32 bytes real,
 * 48 bytes published).
 *
 * Filling the published struct as documented therefore writes main's max pair
 * to offsets 24/28, which is where the library reads the SUB channel. That is
 * the whole explanation for the failures measured on the board:
 *   max_* = 0          -> "sub channel argument error, w: 0, h: 0"
 *   max_* = 1280x720   -> "sub channel argument error, w: 1280, h: 720"
 * and the only documented-layout candidate that "worked" did so by accident:
 * main max 640x480 became sub 640x480, which the validator accepts.
 *
 * Measured on the camera 2026-11-18 with src/akvi_probe.c, not inferred from
 * the header. If a firmware update ships a different libplat_vi, re-run
 * akvi_probe before trusting these offsets.
 */
#ifndef VI_ABI_H
#define VI_ABI_H

struct vi_channel_attr_real {
	int crop_left;
	int crop_top;
	int crop_width;
	int crop_height;
	int main_width;
	int main_height;
	int sub_width;
	int sub_height;
};

/* The published header is 48 bytes (four ints per channel); the library consumes
 * 32 (two ints per channel: crop 16 + main 8 + sub 8). Anything else and the
 * offsets above are wrong, so fail the build rather than hand the library a
 * malformed struct. */
typedef char vi_abi_size_check[(sizeof(struct vi_channel_attr_real) == 32) ? 1 : -1];

#endif /* VI_ABI_H */
