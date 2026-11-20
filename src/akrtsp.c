/*
 * akrtsp - capture, encode and serve H.264 on this AK3918 without the vendor's
 * /mnt/Factory/custom/rtsp.
 *
 * Why this exists (all measured on this board, see README section "Replacing the
 * vendor capture/encode path"): the vendor binary owns the capture pipeline AND
 * serves RTSP, and it takes ~33% of the single 400 MHz core with one NVR
 * attached, of which only ~2.3 points are actual encoding. ~19 points are its
 * RTSP session threads and ~9 more are an always-on G.711 audio path that Chrome
 * cannot even decode. Our own producer, encoding one stream, measured 6.5%
 * (640x480@20) to 7.2% (1280x720@20) of the core. So the encoding is cheap and
 * the serving is what has to be rebuilt.
 *
 * Design, and why each piece is the way it is:
 *
 *  - One process, one VI device, two encode channels. The sub channel is the
 *    preview stream the browser consumes through go2rtc; the main channel is
 *    what the NVR records. Both channels are set in a single
 *    ak_vi_set_channel_attr call, because this library takes the two channels in
 *    one struct (and its struct is NOT the one in the published header - see
 *    vi_abi.h for the measured layout).
 *
 *  - GOP is forced to be at least fps. libakstreamenc's VBR rate control divides
 *    by a quotient that truncates to zero when the GOP is shorter than a second,
 *    and then kills the process with SIGFPE at the second keyframe. Measured:
 *    fps 20 dies at gop 5/10/15 (frames 4/9/14), fine at gop 20; fps 10 fine at
 *    gop 10; fps 15 fine at gop 30. Never "optimise" a short GOP back in.
 *
 *  - RTP over TCP interleaved (RFC 2326 section 10.12) only, for now. Measured:
 *    the NVR on this network holds two TCP connections to :554 and there is not
 *    one UDP socket in sight, so interleaved covers both real consumers
 *    (go2rtc pulls RTSP over TCP too). UDP is a deliberate later addition, not
 *    an oversight.
 *
 *  - Never block the encoder. Every session has its own byte ring and its own
 *    writer thread; if a client is too slow for its ring, the rest of its
 *    current GOP is dropped and it resynchronises on the next keyframe. A slow
 *    NVR must not be able to stall capture for everyone else - on a single core
 *    with a loaded WiFi link it otherwise will.
 *
 *  - No audio at all. The camera's audio is PCMA/G.711, WebRTC in Chrome will
 *    not take it, and the vendor's audio path (ai_capture plus the kernel
 *    aec_task) costs ~9 points of core for something nobody hears.
 *
 * Usage (on the camera, while nothing else owns the pipeline):
 *   akrtsp --port 8554                          # test beside the vendor server
 *   akrtsp --port 554                           # replacing it
 * Streams: /main (or /vs0) and /sub (or /vs1), matching the names go2rtc and the
 * vendor firmware already use.
 */

/* strcasestr() is a GNU extension; needed for Transport header parsing, which
 * clients write in mixed case. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "ak_global.h"
#include "ak_vi.h"
#include "ak_venc.h"
#include "ak_drv_ir.h"
#include "ak_vpss.h"
#include "ak_osd.h"
#include "vi_abi.h"

/*
 * Three of the functions this header declares are NOT this library's signature,
 * and calling them the header's way draws garbage instead of returning an error.
 * Read out of the board's own libmpi_osd.so (the one that runs here, "libmpi_osd
 * V2.0.04", md5 ee2c72dc9e01d9774941b4508b572c58):
 *
 *   ak_osd_set_rect(vi, channel, x, y, w, h)          six arguments, not seven
 *       Its entry code moves r2 into what its own error strings call x and r3
 *       into y, reads w and h off the stack, never touches a seventh argument,
 *       and tail-calls the internal osd_set_rect(channel, x, y, w, h). There is
 *       no per-rect index in this generation: one rect per video channel.
 *   ak_osd_draw_str(channel, xoff, yoff, str, len)    five arguments, not six
 *       r0 is range-checked against the two video channels, r1 against the rect
 *       width minus the font, r2 against the rect height minus the font, and r3
 *       is the string.
 *   ak_osd_clean_str(channel, xoff, row, -, -)        five arguments, not six
 *       r0 is the video channel, r1 is compared against the stored rect width and
 *       r2 against its height. What it clears: rows from r2 downwards, a fixed
 *       number of them - its loop counter starts at r1 and runs up to the stored
 *       font height - each (rect_w - r1)/2 bytes wide (4 bpp: two pixels per byte),
 *       inside the canvas that rect was allocated in; it ends by calling the
 *       internal draw_osd() to push the canvas back to the screen. The header's
 *       second argument - a rect index this generation does not have - is not
 *       there, and passing a width where the loop counter wants an x makes the
 *       condition false on the first test: the call returns success having cleared
 *       nothing (measured). One call covers about one line of the rect, so
 *       osd_draw() steps the start row across it. Without any clear at all the
 *       canvas keeps what it had - the last minute's text, the last geometry's
 *       text, and whatever the previous process left, because the DMA canvas comes
 *       back from the pool uninitialised - and all of it is burned into the NVR's
 *       recording beside the current date.
 *
 * What sent me to the disassembly: with the header's arguments, the main channel
 * refused the rect at one corner and the sub channel accepted it at another, and
 * all four outcomes only make sense if the x you pass lands in osd_chn, the width
 * lands in y and the height is thrown away - which is what happens. ak_osd_init,
 * ak_osd_get_max_rect, ak_osd_set_font_file and ak_osd_destroy do match the
 * header; these three do not.
 *
 *   ak_osd_draw_matrix(channel, yoff, xoff, h, w, buf, len)  seven, not eight
 *       r0 is the video channel, r1 the row of the rect the block starts at, r2
 *       the column it starts at, r3 the block height, the first stack slot the
 *       block width, then the pointer and the length; the header's second
 *       argument, a rect index this generation does not have, is not there and
 *       the two offsets come before the sizes. The length has to be at least
 *       width*height/2 or the call returns 1 with "param err". What it does with
 *       the block is one memcpy into the canvas - no pixel maths at all.
 *
 * That last point is why the overlay is drawn with draw_matrix and not draw_str.
 * The canvas is 4 bpp: set_rect allocates rect_w*rect_h/2 bytes, and the display
 * path reads it two pixels to the byte. draw_str does not know that. Its inner
 * loop (0x1a94-0x1b10) takes one bit out of the font cell and writes it to
 * canvas byte (pixel/8) at bit (7 - pixel%8) - one bit per pixel, one byte per
 * eight pixels. On a canvas the display reads two pixels to the byte, so every
 * glyph lands spread across the wrong bytes: what a grabbed frame shows is a
 * comb of 4 px bars every 8 px, ticking over with the clock and unreadable.
 * Measured on 20 Nov, and it is why --osd-time looked broken for as long as it
 * did. draw_matrix writes the bytes the display expects, so the pixels end up
 * exactly where they were computed - one line, where it is asked for.
 *
 * The canvas is column-major: rect_w columns of rect_h/2 bytes, which is what
 * the copy loop steps. The block goes in as one run, so a block shorter than the
 * rect leaves the rest of the canvas as it came out of the pmem pool, and the
 * range checks are cross-wired - the block's width is measured against the
 * rect's HEIGHT and its height against its width ("para err, osd_w: 500,
 * osd_h: 64" for a 500x64 block in a 500x64 rect, measured 20 Nov). osd_open()
 * keeps the rect square for that reason and osd_draw() always sends all of it.
 */
typedef int (*osd_set_rect_fn)(void *vi, int channel, int x, int y, int w, int h);
typedef int (*osd_draw_str_fn)(int channel, int xoff, int yoff,
			       const unsigned short *str, int len);
typedef int (*osd_clean_str_fn)(int channel, int xoff, int row, int unused1,
				int unused2);
typedef int (*osd_draw_matrix_fn)(int channel, int yoff, int xoff, int h, int w,
				  const unsigned char *bits, unsigned int len);

/* The (void *) hop is deliberate: casting one function pointer type to another
 * directly makes gcc warn, and the warning would be the only thing standing
 * between this and a silent mis-call. Held in a variable rather than cast at each
 * call site, because this toolchain warns at the call ("function called through a
 * non-compatible type") and the build here is kept warning-free on purpose. */
static const osd_set_rect_fn osd_set_rect =
	(osd_set_rect_fn)(void *)ak_osd_set_rect;
static const osd_draw_str_fn osd_draw_str =
	(osd_draw_str_fn)(void *)ak_osd_draw_str;
static const osd_clean_str_fn osd_clean_str =
	(osd_clean_str_fn)(void *)ak_osd_clean_str;
static const osd_draw_matrix_fn osd_draw_matrix =
	(osd_draw_matrix_fn)(void *)ak_osd_draw_matrix;

/*
 * The vendor's font bin, as far as the overlay needs it: the ASCII half, which
 * is the first 4096 bytes of /usr/local/ak_font_16.bin. Each cell is 16 bytes,
 * one byte per row, bit 7 the leftmost pixel - an 8 x 16 cell. That is how the
 * file identifies itself: cell 1 is the CP437 smiley (7e 81 a5 81 81 bd 99 81
 * 81 7e) and cell 0 is blank, and cell '0' (0x30) at offset 768 renders as a
 * zero. Everything past the first 4096 bytes is the GB2312 half, which a date
 * never asks for.
 */
#define OSD_FONT_CELL		16
#define OSD_FONT_ASCII_BYTES	(256 * OSD_FONT_CELL)

#define CH_MAIN 0
#define CH_SUB 1
#define N_CH 2

#define MAX_SESS 4
#define RTP_MTU 1400		/* payload bytes per RTP packet; 1400 keeps the
				 * 20-byte IP + 20-byte TCP framing under 1472 */
#define SESS_BUF (256 * 1024)	/* per-session ring: ~1.4 s of the main channel at
				 * 1.5 Mbps. This board has ~26 MB free and go2rtc
				 * already wants its share, so four sessions at
				 * 256 KB is the ceiling, not a starting point. */

static int g_verbose;
static volatile int g_run = 1;

/*
 * Day/night state. The vendor's app drove this and we did not, which is how the
 * preview ended up purple; see light_watch() below for the measurements.
 */
static void *g_vi;			/* the one capture handle, needed to switch mode */
static int g_auto_night;		/* --auto-night: switch the ISP from the light sensor */
static int g_auto_fps = 0;		/* --auto-fps: let libplat_vi adapt light itself */
static int g_ir_follow;			/* --ir-follow: let mode changes move the filter */
static int g_force_night = -1;		/* --force-night day|night, for A/B measuring */
static int g_mode = -1;			/* 1 day, 0 night, -1 not decided yet */
static int g_have_ir;			/* did ak_drv_ir_init() give us the filter? */

/*
 * Control and status files, so the web UI can change the mode without a restart
 * and without a second daemon holding the capture pipeline.
 *
 * The control file is the desired state, one word: "day", "night" or "auto". The
 * web CGI writes it, this thread reads it every few seconds. It is deliberately a
 * plain file rather than a socket or a signal: it can be read and written from
 * ash, from the CGI, from a terminal, and it survives a restart of the server, so
 * the mode a person chose on the card outlives the process that applied it. The
 * file wins over --force-night when it says something valid; that flag is only the
 * fallback for when there is no file, which is what makes an A/B run on /tmp still
 * possible without deleting the deployment's setting.
 *
 * The status file is written here and read by the CGI: current mode, what was
 * asked for, the light sensor reading, fps, clients, frame counters. It is written
 * through a temp name and renamed, so the CGI never reads a half-written file.
 */
static char g_mode_file[160] = "/tmp/akrtsp.cmd";
static char g_status_file[160] = "/tmp/akrtsp.status";
static int g_mode_want = -1;		/* last thing the control file asked for */

/*
 * The clock, and the date burned into the picture.
 *
 * This board has no real-time clock (no /sys/class/rtc, no battery) and the
 * vendor's own way of learning the time - its cloud bind, which logs "utc_time:
 * 1638933395" at boot - is not running here. Measured on a box that had been up
 * half a day, date read "Thu Jan  1 13:30 GMT 1970". So the clock is somebody
 * else's job (/mnt/camweb/time/run_time.sh, which NTPs it and publishes what it
 * last confirmed) and is nobody's job here - except that once the date is in the
 * pixels, a wrong one is wrong in every recording the NVR makes and cannot be
 * corrected afterwards. Hence the rule below: no confirmed sync, no date; the
 * overlay says NO TIME and the status file says why.
 */
static char g_time_status_file[160] = "/tmp/camweb_time.status";
static long g_clock_now;		/* what our own clock says, this tick     */
static long g_clock_last_sync;		/* epoch the time service last confirmed  */
static long g_clock_min_valid = 1230768000L;	/* below this, it is not a date */
static int g_clock_state;		/* 1 synced, 0 unsynced, -1 no time service */
static char g_clock_peer[64] = "-";

static int g_osd;			/* 0 off, 1 on, -1 asked for and failed */
static int g_osd_size = 16;		/* font cell, px; the vendor's bin is 16 */
static char g_osd_font_file[160] = "/usr/local/ak_font_16.bin";
static char g_osd_fmt[64] = "%Y-%m-%d %H:%M:%S";
static char g_osd_pos[8] = "bl";	/* tl tr bl br */
static int g_osd_margin = 8;
static int g_osd_ch[N_CH] = { 1, 1 };	/* which encode channels get it */
static int g_osd_h[N_CH];		/* the rect height each channel accepted */
static int g_osd_w[N_CH];		/* and its width */
static int g_osd_clean_warned[N_CH];	/* say once that the clear is failing */
/*
 * Palette indices for the overlay, not RGB. ak_osd_set_color() stores the front
 * index at osd+0x84 and the background at +0x88 of the library's own struct,
 * and nothing else ever writes them: left alone, they sit in .bss as 0, and
 * index 0 is black - which is why a burned-in date here came out as dark strokes
 * on a dark corner and looked like nothing at all. Index 1 is the bright entry
 * (and the value ak_osd_draw_str() paints with), so white-on-black is the
 * default and --osd-color / --osd-bg can move it.
 */
static int g_osd_color = 1, g_osd_bg = 0;
static int g_osd_alpha = -1;
static int g_osd_rect_h;			/* rect height; 0 = twice the font */
static int g_osd_xoff, g_osd_yoff;
static char g_osd_text[64];		/* exactly what is on the picture now */

/*
 * Drawing the pixels here instead of letting the library do it. --osd-no-matrix
 * goes back to ak_osd_draw_str(), which is the path that smears (see the ABI
 * note above); it stays in the build so the difference can be shown on one
 * camera rather than argued about.
 */
static int g_osd_matrix = 1;
static int g_osd_probe;			/* calibration pattern, not the clock */
static int g_osd_scale = 2;		/* the 8x16 vendor cell, times this */
static int g_osd_nibble_hi;		/* 1 = the left pixel of a pair is the
					 * high nibble; 0 = the low one. Which
					 * way round the display reads a byte is
					 * one bit, so it is a flag and not a
					 * guess baked into the code. */
#define OSD_BITS_MAX		(1280 * 720 / 2 + 64)	/* the whole canvas, 4 bpp */
static unsigned char g_font[256][OSD_FONT_CELL];
static int g_font_ready;
static unsigned char g_osd_bits[OSD_BITS_MAX];
static int g_osd_matrix_warned;

static void osd_draw(int force);
static void clock_poll(void);

/*
 * What the ISP's own auto-exposure is doing right now, as reported by
 * ak_vpss_isp_get_ae_run_info(). This turned out to be the only light measurement
 * on this board that actually responds. The light dependent resistor behind
 * ak_drv_ir_get_input_level() was A/B tested on the live camera - room light
 * switched on, then off, reading taken both times - and it reported 1 (DAY) in
 * both cases with identical thresholds, so nothing sane can be driven from it.
 * The ISP's auto-exposure loop, on the other hand, sees the picture: the struct
 * carries the average luminance it is holding, the analogue/digital gains and the
 * integration time it needed to hold it, and current_darked_flag, which is the
 * ISP's own day/dark determination.
 *
 * The symbol is exported by this board's libplat_vpss.so (152 bytes at 0x1ec0) and
 * declared in ak_vpss.h, so unlike the AWB/histogram stats it does not have to be
 * guessed at. The values below are published in the status file before any day/night
 * rule is built on them - the rule has to come from measurements on this unit.
 */
static struct vpss_isp_ae_run_info g_ae;
static int g_ae_ok;

/*
 * Auto day/night, driven by what the ISP's auto-exposure loop needs.
 *
 * Measured on this unit (room light switched on and off with the camera's own IR
 * illuminator on, sampled every 4 s, both ISP modes):
 *
 *   day mode,   lamp on, room light off : ae_again 12514, ae_lumi 46
 *   day mode,   lamp on, room light on  : ae_again  4187, ae_lumi 48
 *   night mode, lamp on, room light off : ae_again 16384 (pegged), ae_lumi 61
 *   night mode, lamp on, room light on  : ae_again  4669, ae_lumi 56
 *
 * The luminance is not the light meter - AE is holding it at roughly its target
 * (about 50) whatever the room does, which is the whole point of a control loop.
 * What carries the information is the quantity the loop spends to get there: the
 * analogue gain, which moves by a factor of about three and pegs at its ceiling in
 * the dark. So dark and lit are read off the gain, with the integration time as a
 * secondary confirmation.
 *
 * The catch, and the reason for the probe: while the illuminator is lit, the gain
 * only says how much light reaches the sensor including the camera's own lamp, so
 * a bright IR-reflective surface a metre away can look like daytime. A switch back
 * to day is therefore never taken on that reading alone: the lamp goes out for
 * g_auto_probe_secs, the loop is given g_auto_settle_secs to settle (the AE on this
 * board takes a few seconds and the filter has travel time), and only the reading
 * after that decides. That is also why nothing here consults the light dependent
 * resistor: measured with the room light on and off it reports 1 (DAY) both times,
 * and acting on it is what produced a day-mode ISP in a dark IR-lit room - the
 * purple picture.
 */
static int g_auto_dark_gain = 9000;		/* at or above this the scene is dark */
static int g_auto_light_gain = 6500;		/* at or below this it is lit */
static int g_auto_dark_secs = 15;		/* sustained dark before day -> night */
static int g_auto_lit_secs = 30;		/* sustained lit before probing back */
static int g_auto_probe_secs = 16;		/* lamp off for this long; 0 disables */
static int g_auto_settle_secs = 10;		/* of which ignored while AE settles */
static int g_auto_dwell_secs = 45;		/* minimum time in one state */
static int g_auto_rearm_secs = 300;		/* wait after a probe said "still dark" */
static int g_auto_lamp = 1;			/* auto drives the illuminator */
static int g_auto_filter = 1;			/* auto drives the IR-cut filter */
static char g_irled_node[128] = "/sys/user-gpio/IR_LED";
static char g_ircut_node[128] = "/sys/user-gpio/gpio-ircut_a";
static char g_ptz_fifo[128] = "/tmp/ptz.daemon";

/* the machine itself */
static int g_auto_state = -1;			/* -1 unknown, 0 night, 1 day */
static int g_auto_reason;
static long g_auto_gain = -1;
static int g_lamp = -1;				/* last lamp state we wrote/observed */
static int g_filter_pos = -1;			/* ir-cut position as last observed */
static int g_dark_cnt, g_lit_cnt, g_dwell_cnt, g_rearm_cnt;
static int g_probe_left, g_settle_left, g_reason_hold;
static int g_assert_cnt;

#define AR_NONE 0
#define AR_INIT 1
#define AR_DARK 2
#define AR_PROBE_LIT 3
#define AR_PROBE_DARK 4
#define AR_NOAE 5
#define AR_PROBING 6
#define AR_SETTLE 7
#define AR_LIT 8

static const char *auto_reason_name(int r)
{
	switch (r) {
	case AR_INIT: return "first reading";
	case AR_DARK: return "ae gain held high";
	case AR_LIT: return "ae gain held low";
	case AR_PROBE_LIT: return "probe: ambient light";
	case AR_PROBE_DARK: return "probe: still dark with lamp off";
	case AR_NOAE: return "no ae data";
	case AR_PROBING: return "probe running (lamp off)";
	case AR_SETTLE: return "probe settling";
	default: return "-";
	}
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
	exit(1);
}

static long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

/* base64 for the SDP sprop-parameter-sets, which NVRs use to start decoding
 * before the first keyframe arrives. */
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const unsigned char *in, int n, char *out, int cap)
{
	int i = 0, o = 0;

	while (i < n) {
		unsigned v = (unsigned)in[i] << 16;
		int left = n - i;

		if (left > 1)
			v |= (unsigned)in[i + 1] << 8;
		if (left > 2)
			v |= (unsigned)in[i + 2];
		if (o + 4 >= cap)
			return -1;
		out[o++] = b64c[(v >> 18) & 0x3f];
		out[o++] = b64c[(v >> 12) & 0x3f];
		out[o++] = left > 1 ? b64c[(v >> 6) & 0x3f] : '=';
		out[o++] = left > 2 ? b64c[v & 0x3f] : '=';
		i += 3;
	}
	out[o] = 0;
	return o;
}

/*
 * One encoded stream: profile, SDK handles, and the parameter sets pulled out of
 * the elementary stream so the SDP can carry them (NVRs start decoding from the
 * SDP instead of waiting for the next keyframe).
 */
struct profile {
	const char *name;
	int w, h, fps, gop, kbps;
	void *vi;
	void *enc;
	void *stream;
	unsigned char sps[64];
	unsigned char pps[64];
	int sps_len, pps_len;
	unsigned int profile_level;	/* 0x42e028 for this encoder */
	long frames, keyframes, bytes;
	long first_ms, last_ms, last_report_ms;
	unsigned int last_ts;	/* 90 kHz timestamp of the newest access unit */
	unsigned int ts_base;	/* random per run, then advanced by 90000/fps */
	unsigned int ts_cur;	/* timestamp handed to the current access unit */
	long ts_first_ms;		/* wall clock at the first framed frame */
};

/*
 * One RTSP client. buf is a free-running ring: wpos/rpos only ever increase and
 * are masked on use, which keeps "how much is queued" a single subtraction and
 * lets the producer add a whole RTP frame without racing the writer.
 */
struct sess {
	int fd;
	int used;
	int alive;
	int playing;
	int channel;
	unsigned char ic_rtp, ic_rtcp;
	unsigned short seq;
	unsigned int ssrc;
	unsigned int rtp_ts;
	char peer[64];

	unsigned char *buf;
	unsigned wpos, rpos;
	unsigned cap;
	int drop_until_key;
	unsigned long long dropped_pkts, sent_pkts;

	pthread_t th;
	pthread_mutex_t mtx;
	pthread_cond_t cond;
};

static struct sess g_sess[MAX_SESS];
static struct profile g_prof[N_CH];
static pthread_mutex_t g_sess_mtx = PTHREAD_MUTEX_INITIALIZER;

static unsigned ring_used(const struct sess *s)
{
	return s->wpos - s->rpos;
}

static unsigned ring_free(const struct sess *s)
{
	return s->cap - ring_used(s);
}

/* caller must hold s->mtx and must have checked ring_free() first */
static void ring_put(struct sess *s, const unsigned char *p, unsigned n)
{
	unsigned tail = s->cap - (s->wpos % s->cap);

	if (n <= tail) {
		memcpy(s->buf + (s->wpos % s->cap), p, n);
	} else {
		memcpy(s->buf + (s->wpos % s->cap), p, tail);
		memcpy(s->buf, p + tail, n - tail);
	}
	s->wpos += n;
}

/*
 * Append one complete RTP frame (interleaving header + RTP header + payload).
 * A failure here is always "client too slow", and the caller responds by
 * dropping the rest of that GOP, so the client gets a clean resync point
 * instead of a torn picture.
 */
static int ring_put_pkt(struct sess *s, const unsigned char *rtp, int rlen)
{
	unsigned char pre[4];
	unsigned total = (unsigned)rlen + 4;

	if (ring_free(s) < total)
		return -1;
	pre[0] = '$';
	pre[1] = s->ic_rtp;
	pre[2] = (unsigned char)(rlen >> 8);
	pre[3] = (unsigned char)(rlen & 0xff);
	ring_put(s, pre, 4);
	ring_put(s, rtp, (unsigned)rlen);
	pthread_cond_signal(&s->cond);
	return 0;
}

/*
 * Writer thread: hand bytes to the kernel without ever holding the ring for
 * long. It copies a chunk under the mutex, then blocks on the socket with
 * SO_SNDTIMEO set, so a stalled client can delay its own stream but cannot
 * stop the encoder thread from taking the lock.
 */
static void *sess_writer(void *arg)
{
	struct sess *s = (struct sess *)arg;
	unsigned char tmp[8192];

	while (s->alive) {
		unsigned n = 0;

		pthread_mutex_lock(&s->mtx);
		while (s->alive && ring_used(s) == 0)
			pthread_cond_wait(&s->cond, &s->mtx);
		if (!s->alive) {
			pthread_mutex_unlock(&s->mtx);
			break;
		}
		n = ring_used(s);
		if (n > sizeof(tmp))
			n = sizeof(tmp);
		{
			unsigned head = s->rpos % s->cap;
			unsigned tail = s->cap - head;

			if (n <= tail)
				memcpy(tmp, s->buf + head, n);
			else {
				memcpy(tmp, s->buf + head, tail);
				memcpy(tmp + tail, s->buf, n - tail);
			}
			s->rpos += n;
		}
		pthread_mutex_unlock(&s->mtx);

		{
			unsigned off = 0;
			int err = 0;

			while (off < n) {
				int w = (int)send(s->fd, tmp + off, n - off, MSG_NOSIGNAL);

				if (w > 0) {
					off += (unsigned)w;
					continue;
				}
				if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
					continue;
				err = errno;
				break;
			}
			if (err) {
				logmsg("sess %s write failed errno=%d, closing\n", s->peer, err);
				break;
			}
		}
	}


	s->playing = 0;
	s->alive = 0;
	return NULL;
}

/*
 * RTP payload types and packetising. The stream out of the SDK is Annex-B
 * (start codes) with NAL order SPS, PPS, IDR, slice - verified with ffprobe on
 * a captured file - so the packetiser splits NAL units and uses FU-A for the
 * ones that do not fit a packet.
 */
static int nal_off(const unsigned char *b, unsigned n, unsigned i, unsigned *start, unsigned *end)
{
	/* find the next start code at or after i */
	unsigned j = i;

	while (j + 3 <= n) {
		if (b[j] == 0 && b[j + 1] == 0 && b[j + 2] == 1) {
			*start = j + 3;
			j += 3;
			while (j < n) {
				if (b[j] == 0 && b[j + 1] == 0 &&
				    (j + 3 <= n && (b[j + 2] == 1 || (b[j + 2] == 0 && j + 4 <= n && b[j + 3] == 1))))
					break;
				j++;
			}
			*end = j;
			return 1;
		}
		j++;
	}
	return 0;
}

/*
 * Queue one NAL for a session: single-NAL packet when it fits, otherwise FU-A
 * (RFC 6184 5.3). Assumes the caller already reserved enough ring space for the
 * whole NAL, which is what makes a dropped GOP a clean drop instead of a torn
 * frame on the wire.
 */
static void send_nal(struct sess *s, const unsigned char *nal, unsigned n, int marker, unsigned int ts)
{
	static const int HDR = 12;
	unsigned char pkt[HDR + 2 + RTP_MTU];
	unsigned i;

	pkt[0] = 0x80;					/* V=2, no padding, no extension */
	/*
	 * RTP fixed header: V/P/X/CC, M+PT, sequence, timestamp, SSRC - in that
	 * order. Getting these two fields the wrong way round is invisible in a
	 * player until you measure: the client reads the constant SSRC as the
	 * timestamp, reports 0 fps by timestamps and muxes garbage timings.
	 */
	pkt[4] = (unsigned char)(ts >> 24);
	pkt[5] = (unsigned char)(ts >> 16);
	pkt[6] = (unsigned char)(ts >> 8);
	pkt[7] = (unsigned char)(ts);
	pkt[8] = (unsigned char)(s->ssrc >> 24);
	pkt[9] = (unsigned char)(s->ssrc >> 16);
	pkt[10] = (unsigned char)(s->ssrc >> 8);
	pkt[11] = (unsigned char)(s->ssrc);

	if ((int)n <= RTP_MTU) {
		pkt[1] = (unsigned char)(96 | (marker ? 0x80 : 0));
		pkt[2] = (unsigned char)(s->seq >> 8);
		pkt[3] = (unsigned char)(s->seq & 0xff);
		memcpy(pkt + HDR, nal, n);
		s->seq++;
		s->sent_pkts++;
		ring_put_pkt(s, pkt, HDR + (int)n);
		return;
	}

	for (i = 1; i < n; ) {
		unsigned take = n - i;
		int last;

		if (take > (unsigned)RTP_MTU - 2)
			take = (unsigned)RTP_MTU - 2;
		last = (i + take >= n);
		pkt[1] = (unsigned char)(96 | ((last && marker) ? 0x80 : 0));
		pkt[2] = (unsigned char)(s->seq >> 8);
		pkt[3] = (unsigned char)(s->seq & 0xff);
		pkt[HDR] = (unsigned char)((nal[0] & 0xe0) | 28);			/* FU indicator */
		pkt[HDR + 1] = (unsigned char)((i == 1 ? 0x80 : 0) | (last ? 0x40 : 0) | (nal[0] & 0x1f));
		memcpy(pkt + HDR + 2, nal + i, take);
		s->seq++;
		s->sent_pkts++;
		ring_put_pkt(s, pkt, HDR + 2 + (int)take);
		i += take;
	}
}

/* upper bound on the ring space one NAL needs, so the decision to drop is made
 * before any of it is queued */
static unsigned nal_wire_bytes(unsigned n)
{
	unsigned frag = (unsigned)RTP_MTU - 2;
	unsigned overhead = 4 + 12;

	if ((int)n <= RTP_MTU)
		return overhead + n;
	return ((n + frag - 1) / frag) * (overhead + 2) + n - 1;
}

/*
 * Hand one access unit to one session. AUD and SEI are skipped: they cost
 * packets and nothing in this chain uses them.
 */
static void send_au(struct sess *s, const unsigned char *buf, unsigned len, unsigned int ts, int marker_last)
{
	unsigned start = 0, end = 0, need = 0, i = 0;
	unsigned starts[24], ends[24];
	int nnal = 0, k;

	while (nnal < (int)(sizeof(starts) / sizeof(starts[0])) && nal_off(buf, len, i, &start, &end)) {
		unsigned type = buf[start] & 0x1f;

		i = end;
		if (type == 9 || type == 6 || type == 0 || end <= start)
			continue;
		starts[nnal] = start;
		ends[nnal] = end;
		need += nal_wire_bytes(end - start);
		nnal++;
	}
	if (!nnal)
		return;

	pthread_mutex_lock(&s->mtx);
	if (ring_free(s) < need) {
		pthread_mutex_unlock(&s->mtx);
		s->drop_until_key = 1;
		s->dropped_pkts++;
		return;
	}
	for (k = 0; k < nnal; k++)
		send_nal(s, buf + starts[k], ends[k] - starts[k],
			marker_last && k == nnal - 1, ts);
	pthread_mutex_unlock(&s->mtx);
}

/* keep the SDP's parameter sets current; they repeat at every keyframe */
static void note_param_sets(struct profile *p, const unsigned char *buf, unsigned len)
{
	unsigned start = 0, end = 0, i = 0;

	while (nal_off(buf, len, i, &start, &end)) {
		unsigned type = buf[start] & 0x1f;
		unsigned n = end - start;

		i = end;
		if (type == 7 && n <= sizeof(p->sps)) {
			memcpy(p->sps, buf + start, n);
			p->sps_len = (int)n;
			if (n >= 4)
				p->profile_level = ((unsigned)p->sps[1] << 16) |
					((unsigned)p->sps[2] << 8) | (unsigned)p->sps[3];
		} else if (type == 8 && n <= sizeof(p->pps)) {
			memcpy(p->pps, buf + start, n);
			p->pps_len = (int)n;
		}
	}
}

/*
 * One producer thread per channel: take the encoded access unit from the SDK and
 * fan it out to the sessions watching that channel.
 *
 * ak_venc_get_stream is polled rather than blocking (it returns non-zero when
 * there is nothing yet, exactly as the vendor code treats it). The sleep stays
 * short - 4 ms against a 50 ms frame period at 20 fps - because a longer one
 * shows up as jitter on the wire, and it costs nothing next to the encode.
 *
 * A session whose ring overflowed is skipped until the next keyframe
 * (drop_until_key), so what it gets is a gap ending at a decodable frame rather
 * than a half-transmitted GOP.
 */
static void *producer(void *arg)
{
	struct profile *p = (struct profile *)arg;
	int ch = (int)(p - g_prof);
	struct video_stream vs;
	long last_log = 0;

	while (g_run) {
		if (ak_venc_get_stream(p->stream, &vs)) {
			usleep(4000);
			continue;
		}
		if (vs.data && vs.len) {
			int key = (vs.frame_type == FRAME_TYPE_I);
			unsigned int ts;
			long now = now_ms();
			int i;

			/*
			 * RTP timestamps are ours, not the SDK's. The SDK does fill vs.ts with
			 * milliseconds (measured: 144433750 -> 144433800 -> 144433850 at 20 fps),
			 * but it is a boot-relative clock and feeding it through untouched is
			 * still the wrong answer for a stream: what a decoder wants is media
			 * time. So count frames at 90000/fps.
			 *
			 * If the encoder delivers fewer frames than nominal, creep the base
			 * FORWARD to catch the wall clock up; never backwards. A timestamp that
			 * goes backwards is a hard fault for every client (ffmpeg calls it
			 * non-monotonic DTS and muxes garbage), while one that runs slightly
			 * ahead costs nothing here: there is no audio track to keep in step
			 * with and no RTCP sender report to disagree with. Measured drift of the
			 * encoder against nominal is under 1.5% (20.22-20.30 fps for a asked-for
			 * 20), so this stays a small correction.
			 */
			if (!p->frames) {
				p->ts_base = (unsigned)rand();
				p->ts_first_ms = now;
			}
			{
				long wall = (now - p->ts_first_ms) * 90;
				long ours = (long)(p->frames * (90000 / p->fps));
				long fix = wall - ours;

				/*
				 * The correction is bounded, and that is not paranoia:
				 * `now` is CLOCK_REALTIME and this camera now gets stepped
				 * by NTP while it runs. An unbounded creep against a clock
				 * that can jump (boot reads 1970 until the first sync, which
				 * is 56 years) would push the RTP base forward by years in a
				 * single frame - monotonic, and useless. Real encoder lag is
				 * under 1.5% (20.22-20.30 fps for an asked-for 20), so a
				 * correction that big is not lag; when the delta is more than
				 * two seconds of media time, the clock moved, and the honest
				 * answer is to re-anchor the comparison and leave the
				 * timestamps exactly where they were.
				 */
				if (fix > 18000) {
					if (fix > 180000) {
						logmsg("ch %s: wall clock moved %.1f s under the "
						       "stream; timestamps left alone, comparison "
						       "re-anchored\n", p->name,
						       (double)fix / 90000.0);
						p->ts_first_ms =
							now - p->frames * (1000 / p->fps);
					} else {
						p->ts_base += (unsigned)fix;
					}
				}
			}
			p->ts_cur = p->ts_base + (unsigned)(p->frames * (90000 / p->fps));
			ts = p->ts_cur;
			if (g_verbose && p->frames < 3)
				printf("ts: frame %ld ts=%u base=%u sdk_ts=%llu sdk_seq=%lu\n",
					p->frames, ts, p->ts_base, vs.ts, vs.seq_no);

			note_param_sets(p, vs.data, vs.len);
			p->last_ts = ts;
			p->frames++;
			p->bytes += (long)vs.len;
			if (key)
				p->keyframes++;
			if (!p->first_ms)
				p->first_ms = now;
			p->last_ms = now;

			pthread_mutex_lock(&g_sess_mtx);
			for (i = 0; i < MAX_SESS; i++) {
				struct sess *s = &g_sess[i];

				if (!s->used || !s->alive || !s->playing || s->channel != ch)
					continue;
				if (s->drop_until_key && !key) {
					s->dropped_pkts++;
					continue;
				}
				if (key)
					s->drop_until_key = 0;
				send_au(s, vs.data, vs.len, ts, 1);
			}
			pthread_mutex_unlock(&g_sess_mtx);

			if (g_verbose && now - last_log > 10000) {
				last_log = now;
				logmsg("ch %s: %ld frames, %ld key, %ld bytes, %0.2f fps\n",
					p->name, p->frames, p->keyframes, p->bytes,
					p->frames * 1000.0 / (double)(now - p->first_ms + 1));
			}
		}
		ak_venc_release_stream(p->stream, &vs);
	}
	return NULL;
}

/*
 * Bring up capture and both encoders. Order matters and every step is one the
 * vendor binary performs too (its import list is match_sensor, open,
 * get_handle, get_sensor_resolution, set_channel_attr, switch_mode, capture_on,
 * close), plus the two fps calls it never makes.
 *
 * Note ep.bps is in KILOBITS per second on this build: pass 512 and the encoder
 * logs "bps=524288". Easy unit to get wrong.
 */
static void pipeline_open(void)
{
	struct video_resolution res;
	struct vi_channel_attr_real attr;
	int want_fps = g_prof[CH_MAIN].fps > g_prof[CH_SUB].fps ? g_prof[CH_MAIN].fps : g_prof[CH_SUB].fps;
	void *vi;
	int ch;

	if (ak_vi_match_sensor("/etc/jffs2/"))
		die("ak_vi_match_sensor failed - is /mnt/Factory/custom/rtsp still holding "
		    "the capture pipeline? This box has one.\n");

	vi = ak_vi_open(VIDEO_DEV0);
	if (!vi)
		die("ak_vi_open failed\n");
	g_vi = vi;

	memset(&res, 0, sizeof(res));
	if (ak_vi_get_sensor_resolution(vi, &res) || !res.width)
		die("ak_vi_get_sensor_resolution failed\n");
	printf("sensor %dx%d\n", res.width, res.height);

	memset(&attr, 0, sizeof(attr));
	attr.crop_left = 0;
	attr.crop_top = 0;
	attr.crop_width = res.width;
	attr.crop_height = res.height;
	attr.main_width = g_prof[CH_MAIN].w;
	attr.main_height = g_prof[CH_MAIN].h;
	attr.sub_width = g_prof[CH_SUB].w;
	attr.sub_height = g_prof[CH_SUB].h;
	if (ak_vi_set_channel_attr(vi, (struct video_channel_attr *)&attr))
		die("ak_vi_set_channel_attr rejected main %dx%d / sub %dx%d - this build "
		    "wants main 640..1280 x 480..960 and sub at most 640x480\n",
			g_prof[CH_MAIN].w, g_prof[CH_MAIN].h, g_prof[CH_SUB].w, g_prof[CH_SUB].h);

	if (ak_vi_set_fps(vi, want_fps))
		printf("ak_vi_set_fps(%d) failed; encoding rate still set per channel\n", want_fps);
	/*
	 * libplat_vi runs its own light-adaptation thread (the isp conf carries a
	 * "hight light" and a "low light" parameter block, and the vendor's log shows
	 * "[isp_switch:152] switching isp mode -> night" followed by frame_rate=10
	 * max_exp_time=2250). That thread is what the auto-switch fps flag gates.
	 * It is off by default here for an honest reason: our RTP timestamps step by
	 * 90000/fps per frame, so a sensor rate the ISP changes under us puts them off
	 * by exactly that ratio. --auto-fps turns the library's behaviour back on for
	 * measurement; if it is kept, the timestamps have to follow measured frame
	 * intervals instead of a constant.
	 */
	if (ak_vi_set_switch_fps_enable(vi, g_auto_fps))
		printf("ak_vi_set_switch_fps_enable(%d) not honoured on this build\n", g_auto_fps);
	printf("auto light adaptation (libplat_vi): %s\n", g_auto_fps ? "on" : "off");

	if (ak_vi_capture_on(vi))
		die("ak_vi_capture_on failed\n");

	for (ch = 0; ch < N_CH; ch++) {
		struct profile *p = &g_prof[ch];
		struct encode_param ep;

		memset(&ep, 0, sizeof(ep));
		ep.width = (unsigned long)p->w;
		ep.height = (unsigned long)p->h;
		ep.minqp = 28;
		ep.maxqp = 42;
		ep.fps = p->fps;
		ep.goplen = p->gop;
		ep.bps = p->kbps;
		ep.profile = PROFILE_BASE;
		ep.use_chn = (ch == CH_SUB) ? ENCODE_SUB_CHN : ENCODE_MAIN_CHN;
		ep.enc_grp = (ch == CH_SUB) ? ENCODE_SUBCHN_NET : ENCODE_MAINCHN_NET;
		ep.br_mode = BR_MODE_VBR;
		ep.enc_out_type = H264_ENC_TYPE;

		p->enc = ak_venc_open(&ep);
		if (!p->enc)
			die("ak_venc_open failed for %s %dx%d@%d gop=%d %dkbps\n",
				p->name, p->w, p->h, p->fps, p->gop, p->kbps);
		p->vi = vi;
		p->stream = ak_venc_request_stream(vi, p->enc);
		if (!p->stream)
			die("ak_venc_request_stream failed for %s\n", p->name);
		printf("encoding %-5s %dx%d @ %d fps, gop %d, %d kbps\n",
			p->name, p->w, p->h, p->fps, p->gop, p->kbps);
	}
}

/* ---------------------------------------------------------------- day/night */

/*
 * The vendor's app switched the ISP between day and night; ours did not, and
 * that is what made the preview purple. Measured on this camera's own stream,
 * in a room lit only by the camera's IR illuminator (tools/rgbshot.py):
 *
 *   day-mode ISP, IR-cut out   mean R=152 G=76 B=155 -> R-G +76, B-G +79, 1% neutral
 *   day-mode ISP, IR-cut in    mean R=0.2 G=0.2 B=0.2 -> nothing reaches the sensor
 *
 * i.e. all the light is infrared, the filter is correctly out for night, and a
 * day-mode ISP renders that as magenta. The vendor's log says exactly what it
 * does instead:
 *
 *   now set to night
 *   [isp_switch:152] switching isp mode -> night
 *           frame_rate=10 max_exp_time=2250 light_gain=10
 *
 * Two sensors, and they answer different questions:
 *   ak_vpss_isp_get_auto_day_night_level() - what the picture looks like. This
 *     is the one that can tell "the whole frame is infrared", and it carries its
 *     own hysteresis (DAY_TO_NIGHT_LUM_FACTOR vs NIGHT_TO_DAY_LUM_FACTOR plus a
 *     lock time), so polling it every few seconds is safe.
 *   ak_drv_ir_get_input_level()            - the light dependent resistor. Used
 *     as a fallback, and it also drives the mechanical filter, which is what
 *     ptz_daemon does with the same call.
 *
 * Switching the mode can also move the sensor's frame rate to the conf's value
 * for that mode (the vendor's night block says frame_rate=10), which would put
 * our RTP timestamps off by that ratio, so fps is re-asserted after every
 * switch and rtsp_probe.py is what proves it stayed honest.
 */
static int mode_word(const char *w)
{
	if (!strcmp(w, "day"))
		return 1;
	if (!strcmp(w, "night"))
		return 0;
	if (!strcmp(w, "auto"))
		return 2;
	return -1;
}

/*
 * The desired day/night state, as written by the web CGI: 1 day, 0 night, 2 auto,
 * -1 for "no file" or "something I cannot use". Returning -1 without changing
 * anything is deliberate: a half-written or mistyped file must never move the ISP,
 * because the wrong mode here is the difference between a usable picture and a
 * black one.
 */
static int read_mode_file(const char *path, char *word, int cap)
{
	char raw[96];
	FILE *f = fopen(path, "r");
	int got = -1;

	if (!f)
		return -1;
	if (fgets(raw, sizeof raw, f) && sscanf(raw, "%31s", word) == 1) {
		word[cap - 1] = 0;
		got = mode_word(word);
	}
	fclose(f);
	return got;
}

/*
 * What the UI shows. Written to a temp name and renamed, so a CGI reading this
 * between two fprintf() calls cannot see a truncated line.
 */
static void write_status(int want, int ldr)
{
	char path[200];
	FILE *f;
	int i, clients = 0;

	for (i = 0; i < MAX_SESS; i++)
		if (g_sess[i].used)
			clients++;

	snprintf(path, sizeof path, "%s.tmp", g_status_file);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f,
		"pid=%d\nmode=%s\nwant=%s\nldr=%d\nir_init=%d\nfps=%d,%d\ngop=%d\n"
		"clients=%d\nframes=%ld,%ld\nkeys=%ld,%ld\nbytes=%ld,%ld\n"
		"ae=%d\nae_lumi=%d\nae_clumi=%d\nae_dark=%d\n"
		"ae_again=%ld\nae_dgain=%ld\nae_idgain=%ld\nae_exp=%ld\n"
		"auto=%s\nauto_reason=%s\nauto_gain=%ld\nauto_lamp=%d\n"
		"auto_probe=%d\nauto_dark_gain=%d\nauto_light_gain=%d\n"
		"ircut=%d\n"
		/* The clock and what is burned into the picture. Published because
		 * "the camera says 1970" is a thing the page should show rather than
		 * a thing the viewer discovers from a recording six weeks later. */
		"osd=%s\nosd_text=%s\nclock=%ld\nclock_state=%s\nclock_peer=%s\n"
		"clock_last=%ld\n",
		(int)getpid(),
		g_mode < 0 ? "unset" : (g_mode ? "day" : "night"),
		want < 0 ? "none" : (want == 2 ? "auto" : (want ? "day" : "night")),
		ldr, g_have_ir,
		g_prof[CH_MAIN].fps, g_prof[CH_SUB].fps, g_prof[CH_MAIN].gop,
		clients,
		g_prof[CH_MAIN].frames, g_prof[CH_SUB].frames,
		g_prof[CH_MAIN].keyframes, g_prof[CH_SUB].keyframes,
		g_prof[CH_MAIN].bytes, g_prof[CH_SUB].bytes,
		/* The ISP's own view of the light. Published before any rule is built on
		 * it, so the rule can be argued from numbers in a log rather than from a
		 * guess: ae_lumi is the average luminance AE is holding, ae_dark is the
		 * ISP's day/dark flag, and the gains plus the integration time say what it
		 * had to do to get there. */
		g_ae_ok,
		g_ae_ok ? (int)g_ae.current_calc_avg_lumi : -1,
		g_ae_ok ? (int)g_ae.current_calc_avg_compensation_lumi : -1,
		g_ae_ok ? (int)g_ae.current_darked_flag : -1,
		g_ae_ok ? (long)g_ae.current_a_gain : -1,
		g_ae_ok ? (long)g_ae.current_d_gain : -1,
		g_ae_ok ? (long)g_ae.current_isp_d_gain : -1,
		g_ae_ok ? (long)g_ae.current_exp_time : -1,
		/* What the auto policy is doing and why, so the state can be argued
		 * from the log instead of from a guess: which state it settled on, the
		 * reason for the last move, the gain it is reading, the lamp state it
		 * believes it left, and the thresholds it is using. */
		g_auto_state < 0 ? "-" : (g_auto_state ? "day" : "night"),
		auto_reason_name(g_auto_reason),
		g_auto_gain,
		g_lamp,
		g_probe_left,
		g_auto_dark_gain,
		g_auto_light_gain,
		g_filter_pos,
		g_osd == 1 ? "on" : (g_osd < 0 ? "failed" : "off"),
		g_osd_text,
		g_clock_now,
		g_clock_state == 1 ? "synced" :
			(g_clock_state == 0 ? "unsynced" : "no-service"),
		g_clock_peer[0] ? g_clock_peer : "-",
		g_clock_last_sync);
	fclose(f);
	rename(path, g_status_file);
}

static void apply_mode(int mode, const char *why)
{
	int r = ak_vi_switch_mode(g_vi, mode ? VI_MODE_DAY : VI_MODE_NIGHT);

	logmsg("day/night: isp -> %s (%s): %d\n", mode ? "day" : "night", why, r);
	if (r)
		return;
	g_mode = mode;
	/*
	 * Moving the mechanical filter is NOT done unless --ir-follow asks for it. This
	 * board's ptz_daemon already owns that gpio, and the sensor that would drive the
	 * decision cannot be trusted here: with the room dark and the camera's own IR
	 * illuminator on, ak_drv_ir_get_input_level() reports DAY (read 1 against day
	 * min=1000/max=2000, night max=618) because the illuminator lights the resistor
	 * it sits next to. Acting on that reading inserted the IR-cut filter in an IR-lit
	 * room and the frame went black (mean R=G=B=0.2 over 0..255). Wrong sensor beats
	 * no sensor.
	 */
	if (g_ir_follow && g_have_ir)
		ak_drv_ir_set_ircut(mode);	/* 1 = IR-cut in (day), 0 = out (night) */
	if (ak_vi_set_fps(g_vi, g_prof[CH_MAIN].fps))
		logmsg("day/night: re-asserting %d fps after the switch failed\n",
		       g_prof[CH_MAIN].fps);
}

/*
 * The two effectors the auto policy owns that the ISP call does not: the
 * illuminator (a sysfs gpio the web CGI writes the same way) and the mechanical
 * IR-cut filter, which belongs to ptz_daemon and is commanded through its fifo -
 * the same "/tmp/ptz.daemon" the CGI writes, and the reason this does not go
 * through ak_drv_ir_set_ircut(): the daemon already owns that gpio and drives it
 * from the very light sensor that lies on this board, so going around it would
 * leave two owners fighting.
 */
static int node_write(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY);
	size_t len = strlen(text);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

static int node_read(const char *path)
{
	char c = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	if (read(fd, &c, 1) != 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return c == '1' ? 1 : (c == '0' ? 0 : -1);
}

static int lamp_get(void)
{
	return node_read(g_irled_node);
}

static void lamp_set(int on, const char *why)
{
	if (node_write(g_irled_node, on ? "1" : "0")) {
		logmsg("day/night: cannot write %s (%s) - illuminator left alone\n",
		       g_irled_node, strerror(errno));
		return;
	}
	g_lamp = on;
	logmsg("day/night: ir illuminator -> %s (%s)\n", on ? "on" : "off", why);
}

/*
 * One open, one write. The fifo ptz_daemon reads is rebuilt around its own ir
 * commands, and opening it once per command raced that: measured on the camera, a
 * version that wrote "irinit" and then opened again for "irsetircut 1" had the
 * second open fail (the fifo momentarily had no reader) and the filter simply
 * never moved. One payload, one write, and the reopen is waited for.
 */
static int ptz_fifo_send(const char *cmds)
{
	size_t len = strlen(cmds);
	ssize_t n;
	int fd = -1, tries;

	for (tries = 0; tries < 10; tries++) {
		fd = open(g_ptz_fifo, O_WRONLY | O_NONBLOCK);
		if (fd >= 0)
			break;
		if (errno != ENXIO && errno != EAGAIN)
			return -1;
		usleep(200 * 1000);		/* ptz_daemon rebuilding its fifo */
	}
	if (fd < 0)
		return -1;
	n = write(fd, cmds, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

/*
 * Move the mechanical IR-cut filter, then read the position back instead of
 * assuming it: a filter that was commanded and did not move is exactly the failure
 * that leaves the picture black (filter in, illuminator on), and it belongs in the
 * log rather than being inferred from a dead frame afterwards.
 */
static void filter_set(int in, const char *why)
{
	char cmds[64];
	int tries, now = -1;

	if (!g_auto_filter)
		return;
	snprintf(cmds, sizeof cmds, "irinit\nirsetircut %d\n", in ? 1 : 0);
	if (ptz_fifo_send(cmds)) {
		logmsg("day/night: %s not accepting commands - the IR-cut filter stays "
		       "where it is (is ptz_daemon running?)\n", g_ptz_fifo);
		return;
	}
	for (tries = 0; tries < 6; tries++) {		/* mechanical: travel time */
		sleep(1);
		now = node_read(g_ircut_node);
		if (now == in)
			break;
	}
	g_filter_pos = now;
	logmsg("day/night: ir-cut filter -> %s (%s): %s reads %d%s\n",
	       in ? "in" : "out", why, g_ircut_node, now,
	       now == in ? "" : " - IT DID NOT MOVE");
}

/*
 * The reason for the last decision stays visible for a minute. The page and the
 * status file sample every couple of seconds, and a reason the next idle tick
 * overwrites is a decision nobody can see after the fact - which is what made the
 * first round of auto logs useless for telling a probe from a threshold hit.
 */
static void auto_reason_set(int r)
{
	g_auto_reason = r;
	g_reason_hold = 60;
}

/*
 * Put the whole scene into one of the two states: ISP mode, illuminator, filter.
 * Order matters and is the opposite in the two directions, because the frame must
 * never be left with the lamp and the filter disagreeing with the ISP: going to
 * night opens the filter and lights the lamp before the ISP stops doing colour,
 * going to day kills the lamp and closes the filter before the ISP starts again.
 */
static void auto_apply(int state, const char *why)
{
	if (state != g_auto_state) {
		g_dark_cnt = g_lit_cnt = 0;
		g_dwell_cnt = 0;
	}
	g_auto_state = state;
	if (state == 0) {
		filter_set(0, why);
		if (g_auto_lamp && g_lamp != 1)
			lamp_set(1, why);
		if (g_mode != 0)
			apply_mode(0, why);
	} else {
		if (g_auto_lamp && g_lamp != 0)
			lamp_set(0, why);
		filter_set(1, why);
		if (g_mode != 1)
			apply_mode(1, why);
	}
}

/*
 * One second of the auto policy. Called from the day/night thread, which is the
 * only place that touches the ISP mode, so no locking is needed.
 */
static void auto_tick(void)
{
	long gain, exp;

	if (!g_ae_ok) {
		g_auto_reason = AR_NOAE;
		return;
	}
	gain = (long)g_ae.current_a_gain;
	exp = (long)g_ae.current_exp_time;
	if (gain <= 0) {
		g_auto_reason = AR_NOAE;
		return;
	}
	g_auto_gain = gain;
	if (g_reason_hold > 0)
		g_reason_hold--;

	if (g_settle_left > 0) {
		g_settle_left--;
		g_auto_reason = AR_SETTLE;
		return;
	}

	if (g_probe_left > 0) {
		g_probe_left--;
		g_auto_reason = AR_PROBING;
		if (g_probe_left > 0)
			return;
		/*
		 * The lamp has been dark for the settle window, so whatever the loop
		 * needs now is light that is not coming from this camera.
		 */
		if (gain <= g_auto_light_gain) {
			auto_reason_set(AR_PROBE_LIT);
			auto_apply(1, auto_reason_name(AR_PROBE_LIT));
		} else {
			g_rearm_cnt = g_auto_rearm_secs;
			auto_reason_set(AR_PROBE_DARK);
			auto_apply(0, auto_reason_name(AR_PROBE_DARK));
		}
		return;
	}

	if (g_auto_state < 0) {
		/*
		 * Anchor on the mode the server would have used anyway (--force-night,
		 * which for this camera is night), not on the first AE reading. Measured:
		 * the first reading after a restart reported a low gain in a dark room, the
		 * policy adopted DAY on it, and the camera sat with the illuminator off and
		 * the IR-cut filter in - a black picture - for the minute it took the dark
		 * rule to gather its 15 s plus dwell. The rules below move it as soon as
		 * the readings mean something; the point of this branch is that the state
		 * at startup is known-good rather than sampled.
		 */
		g_auto_state = g_mode >= 0 ? g_mode
					  : (g_force_night >= 0 ? g_force_night : 1);
		auto_reason_set(AR_INIT);
		logmsg("auto: starting in %s (%s); the ae rules will move it from there\n",
		       g_auto_state ? "day" : "night", auto_reason_name(AR_INIT));
		auto_apply(g_auto_state, "startup default");
		return;
	}

	g_dwell_cnt++;
	if (g_rearm_cnt > 0)
		g_rearm_cnt--;

	/*
	 * Re-assert the hardware every minute in case it drifted. The filter is
	 * mechanical and ptz_daemon has its own reasons for moving it; a policy that
	 * only acts on transitions leaves a camera sitting for hours with the lamp on
	 * and the filter in - the combination that gives a black picture - because
	 * nothing decided anything since. Observed while debugging: a filter command
	 * that was accepted but not carried out left exactly that state behind.
	 */
	if (++g_assert_cnt >= 60) {
		int want_lamp = g_auto_state == 0 ? 1 : 0;
		int want_filter = g_auto_state == 0 ? 0 : 1;

		g_assert_cnt = 0;
		if (g_auto_lamp && g_lamp != want_lamp)
			lamp_set(want_lamp, "auto re-assert");
		if (g_auto_filter && g_filter_pos != want_filter)
			filter_set(want_filter, "auto re-assert");
	}

	if (g_auto_state == 1) {
		g_lit_cnt = 0;
		if (gain >= g_auto_dark_gain)
			g_dark_cnt++;
		else
			g_dark_cnt = 0;
		if (g_dark_cnt >= g_auto_dark_secs && g_dwell_cnt >= g_auto_dwell_secs) {
			auto_reason_set(AR_DARK);
			auto_apply(0, auto_reason_name(AR_DARK));
		} else if (g_reason_hold <= 0) {
			g_auto_reason = AR_NONE;
		}
		/*
		 * exp is read but not used: on this board the integration time follows
		 * the ISP mode (2250 us in night, 1350 us in day) and did not move when
		 * the room light was switched, so it is a mode signature rather than a
		 * light meter. The gain is what carries the light.
		 */
		(void)exp;
		return;
	}

	g_dark_cnt = 0;
	if (gain <= g_auto_light_gain)
		g_lit_cnt++;
	else
		g_lit_cnt = 0;
	if (g_lit_cnt < g_auto_lit_secs || g_dwell_cnt < g_auto_dwell_secs || g_rearm_cnt > 0) {
		if (g_reason_hold <= 0)
			g_auto_reason = AR_NONE;
		return;
	}
	if (g_auto_probe_secs <= 0 || !g_auto_lamp) {
		/*
		 * No probe, so the reading is taken at face value. Kept because the
		 * probe is a visible blink and some installations would rather not have
		 * one - but the reason then says the switch was made on gain alone,
		 * with the camera's own lamp still lighting the scene.
		 */
		auto_reason_set(AR_LIT);
		auto_apply(1, auto_reason_name(AR_LIT));
		return;
	}
	g_probe_left = g_auto_probe_secs;
	g_settle_left = g_auto_settle_secs < g_auto_probe_secs ? g_auto_settle_secs
							       : g_auto_probe_secs - 1;
	logmsg("auto: ae gain %ld looks lit, probing ambient with the lamp out for "
	       "%d s\n", gain, g_auto_probe_secs);
	lamp_set(0, "ambient probe");
	g_auto_reason = AR_PROBING;
}

/* ---------------------------------------------------------------- clock, OSD
 *
 * Is the clock a real one? The time service (time/run_time.sh) NTPs the box and
 * publishes what it last confirmed; we read that file rather than trusting our
 * own clock, because our own clock is exactly the thing in question. Both halves
 * have to hold: the service has to say "synced", AND the epoch has to look like a
 * date. A peer that answers with nonsense makes ntpd exit 0 too, and the vendor
 * default (1970 plus uptime) would otherwise be drawn as a real date forever.
 */
static void clock_poll(void)
{
	FILE *f;
	char line[128];
	char peer[sizeof g_clock_peer];
	int state = 0, saw_state = 0;
	long last = 0, min_valid = 0;

	f = fopen(g_time_status_file, "r");
	if (!f) {
		g_clock_state = -1;		/* nothing is keeping time here */
		g_clock_last_sync = 0;
		g_clock_peer[0] = 0;
		g_clock_now = (long)time(NULL);
		return;
	}
	peer[0] = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "state=", 6)) {
			saw_state = 1;
			state = !strncmp(line + 6, "synced", 6);
		} else if (!strncmp(line, "last_sync=", 10)) {
			last = atol(line + 10);
		} else if (!strncmp(line, "min_valid=", 10)) {
			min_valid = atol(line + 10);
		} else if (!strncmp(line, "peer=", 5)) {
			sscanf(line + 5, "%63s", peer);
		}
	}
	fclose(f);

	g_clock_now = (long)time(NULL);
	g_clock_last_sync = last;
	if (min_valid > 0)
		g_clock_min_valid = min_valid;
	if (peer[0])
		snprintf(g_clock_peer, sizeof g_clock_peer, "%s", peer);
	g_clock_state = (saw_state && state && g_clock_now >= g_clock_min_valid) ? 1 : 0;
}

/* The line that is going to be drawn. NO TIME rather than a wrong time: a wrong
 * date in a recording is the one OSD fault that cannot be undone afterwards. */
static void osd_text(char *out, size_t n)
{
	time_t now = (time_t)g_clock_now;
	struct tm *tm;
	size_t len;

	if (g_clock_state != 1) {
		snprintf(out, n, "NO TIME");
		return;
	}
	tm = localtime(&now);
	if (!tm || !strftime(out, n, g_osd_fmt, tm)) {
		snprintf(out, n, "NO TIME");
		return;
	}
	len = strlen(out);
	while (len && out[len - 1] == ' ')
		out[--len] = 0;
	if (!len)
		snprintf(out, n, "NO TIME");
}

static void osd_place(int max_w, int max_h, int w, int h, int *x, int *y)
{
	int m = g_osd_margin;

	if (!strcmp(g_osd_pos, "tl")) {
		*x = m;
		*y = m;
	} else if (!strcmp(g_osd_pos, "tr")) {
		*x = max_w - w - m;
		*y = m;
	} else if (!strcmp(g_osd_pos, "br")) {
		*x = max_w - w - m;
		*y = max_h - h - m;
	} else {
		*x = m;
		*y = max_h - h - m;
	}
	if (*x < 0)
		*x = 0;
	if (*y < 0)
		*y = 0;
}

/*
 * The ASCII half of the vendor's font bin: one 16-byte cell per code, one byte
 * per row, bit 7 the leftmost pixel. Read once at start-up, because the one
 * thing this library gets wrong is what a glyph is made of. Checked rather than
 * trusted: a file whose '0' carries no ink is not a font this code can use, and
 * saying so beats burning a blank bar into every recording.
 */
static int osd_load_font(void)
{
	unsigned char *bytes = &g_font[0][0];
	FILE *f;
	size_t got;
	int i, ink = 0;

	if (g_font_ready)
		return 0;
	f = fopen(g_osd_font_file, "rb");
	if (!f)
		return -1;
	got = fread(bytes, 1, OSD_FONT_ASCII_BYTES, f);
	fclose(f);
	if (got < OSD_FONT_ASCII_BYTES) {
		logmsg("osd: %s gave %zu of the %d bytes the ASCII cells need\n",
		       g_osd_font_file, got, OSD_FONT_ASCII_BYTES);
		return -1;
	}
	for (i = 0; i < OSD_FONT_CELL; i++) {
		unsigned char b = g_font['0'][i];
		while (b) {
			ink += b & 1;
			b >>= 1;
		}
	}
	if (ink < 8) {
		logmsg("osd: %s has no usable glyph for '0' (%d lit pixels) - "
		       "not this vendor's font\n", g_osd_font_file, ink);
		return -1;
	}
	g_font_ready = 1;
	return 0;
}

/*
 * Geometry for the matrix path. The rect IS the bitmap, and the bitmap is the
 * whole rect: ak_osd_draw_matrix() memcpy's the block into the canvas, so a
 * block narrower than the rect's stride slides across its own rows, and a block
 * shorter than the rect leaves the rest of the canvas as it came out of the pmem
 * pool - which is whatever the last process left there, burned into the NVR's
 * recording beside the date.
 *
 * The rect has to be at least as tall as it is wide, because this library
 * range-checks the block with the two dimensions crossed over. Its own error
 * says so: for a 500 px block in a 500x64 rect it prints "para err, osd_w: 500,
 * osd_h: 64" - the block's width is measured against the rect's HEIGHT and its
 * height against the rect's width. Measured on 20 Nov, and it is why the rect
 * here is square rather than a band.
 */
static void osd_matrix_dims(int chars, int max_w, int max_h, int *adv, int *gh,
			    int *w, int *h)
{
	int lim = max_w < max_h ? max_w : max_h;	/* what draw_matrix allows */

	*adv = 8 * g_osd_scale + 2 * g_osd_scale;
	*gh = OSD_FONT_CELL * g_osd_scale;
	if (chars * *adv > lim) {
		/* Prefer losing padding to losing glyph size: the rect only has
		 * to hold the text, and "%Y-%m-%d %H:%M:%S" never needs more
		 * than 20 of the 27 slots the worst-case format allows. */
		int fit = lim / *adv;

		if (fit >= 20) {
			chars = fit;
		} else {
			g_osd_scale = 1;
			*adv = 8 * g_osd_scale + 2 * g_osd_scale;
			*gh = OSD_FONT_CELL * g_osd_scale;
			if (chars * *adv > lim)
				chars = lim / *adv;
		}
	}
	*w = chars * *adv;
	if (*w % 2)
		(*w)++;			/* the canvas is two pixels to the byte */
	*h = *gh;
	if (*h < 2 * g_osd_size)
		*h = 2 * g_osd_size;	/* set_rect refuses a shorter rect */
	if (*h < *w)
		*h = *w;		/* what draw_matrix's check wants */
	if (*h > max_h) {
		*h = max_h;
		if (*w > *h)
			*w = *h & ~1;
	}
}

/* Set one pixel of the canvas image: 4 bpp, 0 transparent.
 *
 * Column-major, and that is not a style choice. The canvas is rect_w columns of
 * rect_h/2 bytes each - which is what ak_osd_draw_matrix()'s own copy loop
 * steps (width iterations, height/2 bytes per iteration, advancing height/2
 * bytes), and what the display reads. Written row-major the picture comes back
 * transposed: measured on 20 Nov, where a probe line across the full width of
 * the rect appeared as a line down the full height at x = w/2. */
/* One pixel of the canvas with an explicit 4 bpp value.
 *
 * Row-major, stride rect_w/2 bytes: a byte holds two pixels along the HORIZONTAL
 * axis, low nibble the left one (until --osd-nibble-hi says otherwise). Proven
 * with a comb probe rather than reasoned out: 4 px of ink and 4 px of nothing
 * over a 120 x 120 patch at two corners of the canvas, then correlate the frame
 * with that square wave along each axis. Written this way the comb stays a comb
 * and lands where it was placed; the column-major reading that the memcpy loop
 * in ak_osd_draw_matrix suggests puts it 90 degrees round instead (measured,
 * 20 Nov). The crossed range checks in that function - the block width counted
 * against the rect height - are what a canvas indexed [y][x] with a rotated
 * display would imply, which is why the rect stays square here. */
static void osd_bitv(int w, int h, int x, int y, int val)
{
	size_t at = (size_t)y * (size_t)(w / 2) + (size_t)x / 2;
	unsigned char *b;

	if (x < 0 || y < 0 || x >= w || y >= h || at >= sizeof g_osd_bits)
		return;
	b = &g_osd_bits[at];
	val &= 0x0f;
	if (g_osd_nibble_hi) {
		if (x & 1)
			*b = (unsigned char)((*b & 0x0f) | (val << 4));
		else
			*b = (unsigned char)((*b & 0xf0) | val);
	} else {
		if (x & 1)
			*b = (unsigned char)((*b & 0xf0) | val);
		else
			*b = (unsigned char)((*b & 0x0f) | (val << 4));
	}
}

/* Ink value written into the 4 bpp canvas. Measured, not assumed: a value ramp
 * of fifteen stripes (1..15) burned into the picture and diffed against the
 * same scene without the overlay showed 13, 14 and 15 changing nothing at all,
 * 5..10 painting the scene uniformly DARK, and 1 making pixels brighter. 1 is
 * also the only value ak_osd_draw_str() ever writes - its blit sets a single
 * bit - which is the vendor's own white text. So the nibble is a palette index,
 * not an opacity: 0 transparent, 1 the foreground colour, 8 black. */
#define OSD_INK			1

static void osd_bit(int w, int h, int x, int y, int on)
{
	osd_bitv(w, h, x, y, on ? OSD_INK : 0x00);
}

/*
 * The line as the display wants the bytes. Which of the two pixels packed into
 * a byte is the left one is the single thing here that cannot be read out of
 * the library (draw_str never writes a byte the display reads correctly), so
 * --osd-nibble-hi decides it and the first frame on the camera settles it.
 */
static int osd_render_bits(const char *text, int w, int h, int top)
{
	int n = (int)strlen(text);
	int adv = 8 * g_osd_scale + 2 * g_osd_scale;
	int i, r, c, k, j;

	if ((size_t)w * h / 2 > sizeof g_osd_bits)
		return -1;
	memset(g_osd_bits, 0, (size_t)w * h / 2);
	for (i = 0; i < n; i++) {
		const unsigned char *cell = g_font[(unsigned char)text[i]];
		for (r = 0; r < OSD_FONT_CELL; r++) {
			unsigned char row = cell[r];
			if (!row)
				continue;
			for (c = 0; c < 8; c++) {
				if (!((row >> (7 - c)) & 1))
					continue;
				for (k = 0; k < g_osd_scale; k++)
					for (j = 0; j < g_osd_scale; j++)
						osd_bit(w, h, i * adv + c * g_osd_scale + j,
							top + r * g_osd_scale + k, 1);
			}
		}
	}
	return 0;
}

/*
 * Calibration pattern, for the one question a frame can answer and a
 * disassembly cannot: which way round the display reads a packed byte, and
 * whether row 0 of the rect is its top. A full line at the very top and the
 * very bottom of the rect shows the rows; a column at x=0 for the whole height
 * and a column at x=1 for the lower half only, so the two are one pixel apart
 * and the parity of the packing shows as which side each one lands on.
 */
/*
 * Calibration in one frame. Fifteen stripes of constant 4 bpp value (1..8
 * above, 9..15 below), a one-pixel marker along the top edge and down the left
 * edge, and a full-width line at value 1 - the value the library's own text
 * blit writes - as the control. The stripes say which canvas values the display
 * draws at all and how strongly: 0xf as "ink" is a reading of the canvas format
 * and not a fact from a header, so it gets measured. The markers say where row
 * 0 and column 0 of the rect land in the picture, which is the other thing a
 * column-major canvas can get wrong.
 */
static void osd_probe_bits(int w, int h)
{
	int x, y;

	memset(g_osd_bits, 0, (size_t)w * (size_t)h / 2);
	/* A comb: 4 px of ink, 4 px of nothing, over a 120 x 120 patch. A scene
	 * does not produce that period, so a frame can be searched for it
	 * directly - correlate each window against a square wave of the same
	 * period along x and along y and the answer says both where the canvas
	 * landed and whether the display turned it through 90 degrees. Two combs
	 * at different corners, because a single one cannot tell a transpose
	 * from a mirror. */
	for (y = 20; y < 140 && y < h; y++)
		for (x = 20; x < 140 && x < w; x++)
			osd_bitv(w, h, x, y, ((x / 4) % 2) ? 0 : OSD_INK);
	for (y = h - 140; y < h; y++)
		for (x = w - 140; x < w; x++)
			osd_bitv(w, h, x, y, ((y / 4) % 2) ? OSD_INK : 0);
}


/*
 * Bring the overlay up. The order is the SDK demo's order and it is not
 * cosmetic: the font is chosen before ak_osd_init() because init sizes its
 * buffers from it. The vendor's own boot log on this exact board shows the same
 * sequence and geometry (ak_osd_set_font_file channel=1 font size=16, then
 * ak_osd_init ch:0 fontS:32 res_w:1280 res_h:720, then ak_osd_set_rect xstart:0
 * ystart:0 w:1280 h:64), which is also the proof that this path works on
 * AK3918E + GC1084 and not only in the SDK.
 *
 * The rect is sized once, for the widest the format can produce, so a minute
 * with a short text and a minute with a long one do not need it moved;
 * ak_osd_draw_str() then draws inside it at 0,0.
 */
static int osd_open(void)
{
	int ch, chars, cell_w, cell_h, font_file_ok;

	if (!g_vi) {
		logmsg("osd: no capture handle yet, overlay off\n");
		return -1;
	}
	font_file_ok = (access(g_osd_font_file, R_OK) == 0);
	if (!font_file_ok) {
		/*
		 * There is no fallback here, and that is a property of this board, not
		 * of the code: the ak_osd.h in the SDK dump declares ak_osd_set_font_size
		 * for the "no font file" path, but the libmpi_osd.so this camera
		 * generation actually ships does not export it (nor set_rect_enable, nor
		 * set_edge_color - the linker is what said so, and nm -D on both copies
		 * confirms it: the header belongs to a newer SDK generation than the
		 * library). So the font bin is required. It is on this camera already:
		 * /usr/local/ak_font_16.bin, 265798 bytes, from the vendor firmware.
		 */
		logmsg("osd: %s is not readable, and this build's libmpi_osd cannot "
		       "select a builtin font size - overlay off (that is the vendor's "
		       "own font file, so a missing one means the card changed)\n",
		       g_osd_font_file);
		return -1;
	}
	if (g_osd_matrix && osd_load_font()) {
		logmsg("osd: the glyphs could not be read out of %s, so this run "
		       "falls back to ak_osd_draw_str - which smears on this "
		       "board (see the note by the library calls)\n", g_osd_font_file);
		g_osd_matrix = 0;
	}
	if (ak_osd_set_font_file(g_osd_size, g_osd_font_file)) {
		logmsg("osd: ak_osd_set_font_file(%d, %s) failed\n",
		       g_osd_size, g_osd_font_file);
		return -1;
	}
	if (ak_osd_init(g_vi)) {
		logmsg("osd: ak_osd_init failed - no overlay on this build\n");
		return -1;
	}
	if (g_osd_color >= 0 || g_osd_bg >= 0)
		ak_osd_set_color(g_osd_color < 0 ? 0 : g_osd_color,
				 g_osd_bg < 0 ? 1 : g_osd_bg);
	if (g_osd_alpha > 0)
		ak_osd_set_alpha(g_osd_alpha);

	/* Room for the format at its widest, not for the current text: a format
	 * like "%d %b" is 8 characters one minute and 14 the next. */
	osd_text(g_osd_text, sizeof g_osd_text);
	chars = (int)strlen(g_osd_fmt) + 8;
	if (chars < (int)strlen(g_osd_text))
		chars = (int)strlen(g_osd_text);

	for (ch = 0; ch < N_CH; ch++) {
		int max_w = 0, max_h = 0, w, h, x = 0, y = 0;

		if (!g_osd_ch[ch])
			continue;
		if (ak_osd_get_max_rect(ch, &max_w, &max_h) || max_w <= 0 || max_h <= 0) {
			logmsg("osd: %s reports no drawable area; no overlay there\n",
			       g_prof[ch].name);
			g_osd_ch[ch] = 0;
			continue;
		}
		if (g_osd_matrix) {
			int adv, gh, want = g_osd_scale;

			/* One line, one bitmap, one copy. The rect is sized
			 * from the line: draw_matrix memcpy's the block into
			 * the canvas, so it has to be as wide as the rect and
			 * as tall as the rect is wide (see osd_matrix_dims). */
			osd_matrix_dims(chars, max_w, max_h, &adv, &gh, &w, &h);
			if (g_osd_scale != want)
				logmsg("osd: %s area is %dx%d and the library will "
				       "not take a block wider than the rect is "
				       "tall: glyphs down to %dx%d there\n",
				       g_prof[ch].name, max_w, max_h,
				       8 * g_osd_scale, gh);
			logmsg("osd: %s canvas %dx%d, advance %d, glyphs %dx%d\n",
			       g_prof[ch].name, w, h, adv, 8 * g_osd_scale, gh);
		} else {
		cell_w = g_osd_size > 16 ? g_osd_size : 16;
		/* The glyph advance comes from the font file, not from --osd-size:
		 * this board's ak_font_16.bin draws 16 px wide whatever is handed to
		 * ak_osd_set_font_file() - with --osd-size 8 and with --osd-size 32
		 * the glyphs measured 16 px apart in the frame either way. Sizing the
		 * rect from --osd-size clipped the date to "2026-11-19 1" at size 8,
		 * so the width follows the real advance and --osd-size is left doing
		 * the only thing this library lets it do: set the minimum rect
		 * height, which is twice its value. */
		/* Twice --osd-size: what the SDK demo does (FONT_HEIGHT * 2) and what
		 * the vendor's own boot log does (font 32 in a 1280x64 rect). It is
		 * also the floor this library enforces - a 400x32 rect at --osd-size
		 * 32 was refused with "err ... h: 32, f: 64", f being twice the
		 * size. The height matters more than that: the library stamps the
		 * string once for every 16-row block of the rect (the font file's
		 * real cell height), so a 32-tall rect shows the date twice, 16 px
		 * apart, and a 16-tall rect - only reachable at --osd-size 8, since
		 * the floor is twice the size - shows it once. Measured on 19 Nov;
		 * --osd-rect-h exists to try other heights without a rebuild. */
		cell_h = g_osd_rect_h > 0 ? g_osd_rect_h : g_osd_size * 2;
		w = chars * cell_w;
		h = cell_h;
		if (w > max_w)
			w = max_w;
		if (h > max_h)
			h = max_h;
		}
		osd_place(max_w, max_h, w, h, &x, &y);
		/*
		 * No ak_osd_set_rect_enable() here: the header declares it, this
		 * board's library does not export it, and the SDK's own working example
		 * does not call it either - set_rect is what turns an area on.
		 */
		if (osd_set_rect(g_vi, ch, x, y, w, h)) {
			logmsg("osd: ak_osd_set_rect on %s failed (%dx%d at %d,%d of "
			       "%dx%d)\n", g_prof[ch].name, w, h, x, y, max_w, max_h);
			g_osd_ch[ch] = 0;
			continue;
		}
		g_osd_w[ch] = w;
		g_osd_h[ch] = h;
		logmsg("osd: %s rect %dx%d at %d,%d of max %dx%d, %s, font %s %dpx, "
		       "pos %s\n", g_prof[ch].name, w, h, x, y, max_w, max_h,
		       g_osd_matrix ? "matrix" : "draw_str",
		       g_osd_font_file, g_osd_size, g_osd_pos);
	}
	if (!g_osd_ch[CH_MAIN] && !g_osd_ch[CH_SUB])
		return -1;
	g_osd_text[0] = 0;		/* force the first draw to paint */
	return 0;
}

/*
 * Paint the line. Called every second from the day/night thread, and forced
 * after anything that can wipe the overlay (an ISP mode switch). Redrawing the
 * same text is skipped: it is a copy into the overlay's pmem plus an ioctl,
 * which is cheap, but "cheap" on this box is a claim that has to be measured,
 * and nothing in the picture changes between minutes.
 */
static void osd_draw(int force)
{
	unsigned short wide[64];
	char text[64];
	int ch, i, len;

	if (g_osd != 1)
		return;
	osd_text(text, sizeof text);
	if (!force && g_osd_text[0] && !strcmp(text, g_osd_text))
		return;
	snprintf(g_osd_text, sizeof g_osd_text, "%s", text);
	len = (int)strlen(text);
	if (len > 63)
		len = 63;
	for (i = 0; i < len; i++)
		wide[i] = (unsigned short)(unsigned char)text[i];

	for (ch = 0; ch < N_CH; ch++) {
		int xo = g_osd_xoff, yo = g_osd_yoff;
		int row;

		if (!g_osd_ch[ch])
			continue;
		if (g_osd_matrix) {
			int gh = OSD_FONT_CELL * g_osd_scale;
			int top = 0;

			/* The rect is square, so the line goes to the edge of
			 * it that the chosen corner sits on: the bottom for
			 * "bl"/"br", the top for "tl"/"tr". The rest of the
			 * rect stays 0, which the display draws as nothing. */
			if (g_osd_pos[0] == 'b')
				top = g_osd_h[ch] - gh;
			if (top < 0)
				top = 0;
			if (g_osd_probe)
				osd_probe_bits(g_osd_w[ch], g_osd_h[ch]);
			else if (osd_render_bits(text, g_osd_w[ch], g_osd_h[ch], top)) {
				if (!g_osd_matrix_warned)
					logmsg("osd: the line does not fit in %dx%d "
					       "(canvas holds %d bytes); overlay off\n",
					       g_osd_w[ch], g_osd_h[ch],
					       (int)sizeof g_osd_bits);
				g_osd_matrix_warned = 1;
				g_osd_ch[ch] = 0;
				continue;
			}
			/* Both offsets 0, deliberately: the placement is inside
			 * the bitmap, and the copy has to start at the first
			 * byte of the canvas for its rows to stay level. */
			if (osd_draw_matrix(ch, 0, 0, g_osd_h[ch], g_osd_w[ch],
					    g_osd_bits,
					    (unsigned)(g_osd_w[ch] * g_osd_h[ch] / 2))) {
				if (!g_osd_matrix_warned)
					logmsg("osd: ak_osd_draw_matrix on %s failed "
					       "(%dx%d); no overlay there\n",
					       g_prof[ch].name, g_osd_w[ch], g_osd_h[ch]);
				g_osd_matrix_warned = 1;
				g_osd_ch[ch] = 0;
				continue;
			}
			continue;
		}
		/* The offsets are relative to the rect, and the library range-checks
		 * them against the rect that channel was given. Clamping here means a
		 * --osd-yoff that suits one channel and not the other degrades on the
		 * odd channel instead of failing the overlay on both. */
		if (yo > g_osd_h[ch] - g_osd_size)
			yo = g_osd_h[ch] - g_osd_size;
		if (yo < 0)
			yo = 0;
		if (xo > g_osd_w[ch] - g_osd_size)
			xo = g_osd_w[ch] - g_osd_size;
		if (xo < 0)
			xo = 0;
		/* Clear the whole canvas before painting. Nothing in this library
		 * clears it by itself, and its clean call clears a slice, not the
		 * rect: read out of the board's copy, the loop counter starts at the
		 * SECOND argument and runs up to the stored font height, while the
		 * third argument is the row the clearing starts at - so one call
		 * covers roughly one line of the rect. Walking that start row across
		 * the rect covers all of it. Without this the canvas keeps whatever
		 * was in it: the previous minute's text, the previous geometry's text,
		 * and - the DMA canvas comes back from the pool uninitialised -
		 * whatever the previous process left. All of it gets burned into the
		 * NVR's recording next to the current date, which is what the two
		 * bands in the picture actually were: two different strings, at two
		 * different offsets, one of them left over from an earlier run. */
		for (row = 0; row < g_osd_h[ch]; row += g_osd_size) {
			if (osd_clean_str(ch, 0, row, 0, 0) &&
			    !g_osd_clean_warned[ch]) {
				logmsg("osd: ak_osd_clean_str on %s failed at row %d; "
				       "the overlay piles up instead of being redrawn\n",
				       g_prof[ch].name, row);
				g_osd_clean_warned[ch] = 1;
			}
		}
		if (osd_draw_str(ch, xo, yo, wide, len)) {
			logmsg("osd: ak_osd_draw_str on %s failed; overlay off there\n",
			       g_prof[ch].name);
			g_osd_ch[ch] = 0;
		}
	}
}

/*
 * The day/night thread. It now always runs (unless --mode-file "" is given),
 * because its job is no longer only deciding: it applies what the control file
 * asks for and publishes the status the UI reads.
 *
 * Order of precedence, measured rather than assumed - see the comment above
 * apply_mode() for why the light sensor is the weakest of these:
 *
 *   the control file ("day"/"night")   wins; this is what the web UI writes
 *   --force-night                       only when the file says nothing usable
 *   "auto" in the file, or --auto-night the light sensor, knowing it lies indoors
 *
 * A file that cannot be read, or says something unrecognised, changes nothing:
 * the mode that produced a visible picture stays until someone asks for another.
 */
static void *light_watch(void *unused)
{
	char word[32];
	int tick = 0, want = -1, ldr = -1;
	int last_mode = -2;		/* to notice a switch that can wipe the OSD */

	(void)unused;
	g_have_ir = (ak_drv_ir_init() == 0);
	if (!g_have_ir)
		logmsg("day/night: ak_drv_ir_init failed - the mechanical filter stays "
		       "with ptz_daemon; only the ISP mode will be switched\n");

	while (g_run) {
		word[0] = 0;
		want = read_mode_file(g_mode_file, word, sizeof word);
		g_mode_want = want;
		ldr = g_have_ir ? ak_drv_ir_get_input_level() : -1;
		/* The illuminator is tracked from the node, not from what we last wrote.
		 * ptz_daemon's irinit re-initialises the IR driver and puts its own idea of
		 * the lamp on that gpio: measured on this board, a filter command from the
		 * web page left the lamp OFF while the server still believed it was on, and
		 * the re-assert below then had nothing to correct. Same for the filter, which
		 * moves on its own too - one sysfs open each per second is nothing next to
		 * what the ISP is doing, and a wrong state on the page is worse. */
		g_lamp = lamp_get();
		g_filter_pos = node_read(g_ircut_node);
		if (g_vi)
			g_ae_ok = (ak_vpss_isp_get_ae_run_info(g_vi, &g_ae) == 0);
		/* Read what the clock supervisor last confirmed before anything that
		 * prints a date is consulted. One small file per second: this box
		 * cannot afford /proc walks, but it can afford this. */
		clock_poll();

		if (want == 0 || want == 1) {
			/* Manual beats everything, and the auto machine is re-anchored on
			 * what the operator chose, so coming back to "auto" later does not
			 * start from a state the operator overrode minutes ago. */
			if (want != g_mode)
				apply_mode(want, "control file");
			if (g_auto_state != want) {
				g_auto_state = want;
				g_dwell_cnt = g_dark_cnt = g_lit_cnt = 0;
				g_probe_left = g_settle_left = 0;
				g_auto_reason = AR_NONE;
				g_reason_hold = 0;
			}
		} else if (want == 2) {
			auto_tick();
		} else if (g_mode < 0 && g_force_night >= 0) {
			apply_mode(g_force_night, "--force-night");
		} else if (g_auto_night && ldr >= 0 && ldr != g_mode) {
			/*
			 * Legacy light-sensor path (--auto-night). Left in for boards
			 * where the resistor is wired to something, but on this unit it
			 * was measured sitting at 1 with the room light on and off, which
			 * is exactly the reading that put a day-mode ISP into a dark
			 * IR-lit room. "auto" in the control file no longer means this.
			 */
			logmsg("day/night: ldr says %d (--auto-night; this board's light "
			       "sensor does not respond)\n", ldr);
			apply_mode(ldr, "light sensor");
		}

		/*
		 * The overlay is painted from here rather than from a thread of its
		 * own: it is one more thing that has to agree with the clock, and the
		 * loop already runs every second. A change of mode forces a redraw
		 * even when the text has not changed, because switching the ISP mode
		 * rebuilds what the encoder is fed; that this should ever drop the
		 * overlay has not been measured, and the forced redraw costs one call
		 * per switch, so the precaution stays and is labelled as one.
		 */
		if (g_osd == 1 && g_mode != last_mode) {
			last_mode = g_mode;
			osd_draw(1);
		} else {
			osd_draw(0);
		}

		/* Status every 2 s: fresh enough for the page, and the file is rewritten
		 * whole (temp + rename) so nothing can read it half written. */
		if (++tick >= 2) {
			tick = 0;
			write_status(want, ldr);
		}

		/* One second slices: the auto policy counts in seconds. */
		sleep(1);
	}
	return NULL;
}

/* ---------------------------------------------------------------- RTSP side */

/*
 * Which stream a URL asks for. The names are not ours to choose: go2rtc's
 * LIVE_SRC and the vendor firmware already use /vs0 (the bigger stream) and
 * /vs1 (the smaller one), so those keep working if the server takes over the
 * port, and /main and /sub are the readable aliases.
 */
static int path_channel(const char *path)
{
	if (!strncmp(path, "/vs0", 4) || !strncmp(path, "/main", 5))
		return CH_MAIN;
	if (!strncmp(path, "/vs1", 4) || !strncmp(path, "/sub", 4))
		return CH_SUB;
	return -1;
}

static int build_sdp(int ch, const char *base, char *out, int cap)
{
	struct profile *p = &g_prof[ch];
	char sps[128] = "", pps[128] = "";
	char sprop[300] = "";
	int n;

	if (p->sps_len > 0 && p->pps_len > 0) {
		if (b64_encode(p->sps, p->sps_len, sps, sizeof(sps)) < 0 ||
		    b64_encode(p->pps, p->pps_len, pps, sizeof(pps)) < 0)
			sps[0] = pps[0] = 0;
		else
			snprintf(sprop, sizeof(sprop), ";sprop-parameter-sets=%s,%s", sps, pps);
	}

	n = snprintf(out, cap,
		"v=0\r\n"
		"o=- %ld 1 IN IP4 127.0.0.1\r\n"
		"s=%s\r\n"
		"i=AK3918 %s %dx%d @ %d fps\r\n"
		"t=0 0\r\n"
		"a=tool:akrtsp\r\n"
		"a=type:broadcast\r\n"
		"a=range:npt=now-\r\n"
		"m=video 0 RTP/AVP 96\r\n"
		"a=rtpmap:96 H264/90000\r\n"
		"a=fmtp:96 packetization-mode=1;profile-level-id=%06X%s\r\n"
		"a=control:%s/trackID=0\r\n"
		"a=framerate:%d\r\n",
		(long)time(NULL), p->name, p->name, p->w, p->h, p->fps,
		p->profile_level ? p->profile_level : 0x42e028u,
		sprop, base, p->fps);
	(void)n;
	return 0;
}

static struct sess *sess_alloc(int fd, const char *peer)
{
	int i;

	for (i = 0; i < MAX_SESS; i++) {
		struct sess *s = &g_sess[i];

		if (s->used)
			continue;
		memset(s, 0, sizeof(*s));
		s->fd = fd;
		s->used = 1;
		s->alive = 1;
		s->channel = -1;
		s->cap = SESS_BUF;
		s->buf = malloc(SESS_BUF);
		if (!s->buf) {
			s->used = 0;
			return NULL;
		}
		snprintf(s->peer, sizeof(s->peer), "%s", peer);
		pthread_mutex_init(&s->mtx, NULL);
		pthread_cond_init(&s->cond, NULL);
		s->ssrc = (unsigned)rand();
		s->seq = (unsigned short)rand();
		pthread_mutex_lock(&g_sess_mtx);
		logmsg("sess %d from %s\n", i, peer);
		pthread_mutex_unlock(&g_sess_mtx);
		return s;
	}
	return NULL;
}

static void sess_stop_writer(struct sess *s)
{
	if (s->th) {
		pthread_mutex_lock(&s->mtx);
		s->alive = 0;
		pthread_cond_signal(&s->cond);
		pthread_mutex_unlock(&s->mtx);
		pthread_join(s->th, NULL);
		s->th = 0;
	}
}

static void sess_free(struct sess *s)
{
	sess_stop_writer(s);
	if (s->fd >= 0) {
		shutdown(s->fd, SHUT_RDWR);
		close(s->fd);
		s->fd = -1;
	}
	pthread_mutex_lock(&g_sess_mtx);
	logmsg("sess from %s closed: %llu packets, %llu dropped\n",
		s->peer, s->sent_pkts, s->dropped_pkts);
	if (s->buf) {
		free(s->buf);
		s->buf = NULL;
	}
	s->used = 0;
	pthread_mutex_unlock(&g_sess_mtx);
	pthread_mutex_destroy(&s->mtx);
	pthread_cond_destroy(&s->cond);
}

/*
 * Control-channel write. Blocking, but with SO_SNDTIMEO on the socket, so a
 * client that stops reading cannot wedge the accept thread.
 */
static void ctrl_write(struct sess *s, const char *buf, int n)
{
	int off = 0;

	while (n > 0 && off < n) {
		int w = (int)send(s->fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);

		if (w > 0) {
			off += w;
			continue;
		}
		if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		break;
	}
}

static void reply(struct sess *s, int cseq, const char *extra)
{
	char buf[1024];
	int n = snprintf(buf, sizeof(buf), "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s\r\n", cseq,
		extra ? extra : "");

	ctrl_write(s, buf, n);
}

static void reply_err(struct sess *s, int cseq, int code, const char *why)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf), "RTSP/1.0 %d %s\r\nCSeq: %d\r\n%s\r\n",
		code, why, cseq, why ? "" : "");

	logmsg("  -> %d %s\n", code, why);
	ctrl_write(s, buf, n);
}


static int header_int(const char *req, const char *name)
{
	char pat[32];
	const char *p;

	snprintf(pat, sizeof(pat), "%s:", name);
	p = strstr(req, pat);
	if (!p)
		return -1;
	return atoi(p + strlen(pat));
}

/* copy the path part of rtsp://host[:port]/vs1?x=y into out */
static void url_path(const char *url, char *out, int cap)
{
	const char *p = strstr(url, "://");
	const char *slash;

	if (p)
		p += 3;
	else
		p = url;
	slash = strchr(p, '/');
	if (!slash) {
		snprintf(out, cap, "/");
		return;
	}
	snprintf(out, cap, "%.*s", (int)(strlen(slash)), slash);
	{
		char *q = strchr(out, '?');
		if (q)
			*q = 0;
	}
}

static void handle_request(struct sess *s, const char *req)
{
	char method[16] = "", url[512] = "", path[300] = "", extra[600];
	int cseq = header_int(req, "CSeq");
	int ch;

	if (cseq < 0)
		cseq = 0;
	if (sscanf(req, "%15s %511s", method, url) != 2) {
		reply_err(s, cseq, 400, "Bad Request");
		return;
	}
	url_path(url, path, sizeof(path));
	ch = path_channel(path);
	logmsg("sess %s %s %s\n", s->peer, method, path);

	if (!strcmp(method, "OPTIONS")) {
		snprintf(extra, sizeof(extra),
			"Server: akrtsp\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, "
			"TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
		reply(s, cseq, extra);
		return;
	}

	if (!strcmp(method, "DESCRIBE")) {
		char sdp[1200];
		int slen;

		if (ch < 0) {
			reply_err(s, cseq, 404, "Stream Not Found");
			return;
		}
		build_sdp(ch, url, sdp, sizeof(sdp));
		slen = (int)strlen(sdp);
		/*
		 * One write, not two. Sent separately the SDP lands in its own TCP
		 * segment, which is legal but makes thin clients read the body as if
		 * it were the next response (one of ours did exactly that).
		 */
		{
			static char whole[2048];
			int hn = snprintf(whole, sizeof(whole),
				"RTSP/1.0 200 OK\r\nCSeq: %d\r\nServer: akrtsp\r\n"
				"Content-Type: application/sdp\r\nContent-Base: %s\r\n"
				"Content-Length: %d\r\n\r\n", cseq, url, slen);

			if (hn + slen < (int)sizeof(whole)) {
				memcpy(whole + hn, sdp, (size_t)slen);
				ctrl_write(s, whole, hn + slen);
			} else {
				ctrl_write(s, whole, hn);
				ctrl_write(s, sdp, slen);
			}
		}
		return;
	}

	if (!strcmp(method, "SETUP")) {
		const char *tr = strstr(req, "Transport:");
		unsigned int ic0 = 0, ic1 = 1;

		if (!tr) {
			reply_err(s, cseq, 400, "Bad Request");
			return;
		}
		/*
		 * TCP interleaving only, and said so rather than pretending:
		 * measured on this network the NVR holds two TCP connections and
		 * there is not one UDP socket, so this covers the real consumers.
		 * A UDP client gets 461 instead of a session that silently never
		 * delivers anything.
		 */
		if (!strcasestr(tr, "RTP/AVP/TCP") && !strcasestr(tr, "interleaved")) {
			reply_err(s, cseq, 461, "Unsupported Transport Protocol");
			return;
		}
		{
			const char *il = strcasestr(tr, "interleaved=");

			if (il)
				sscanf(il + 12, "%u-%u", &ic0, &ic1);
		}
		if (ch < 0) {
			reply_err(s, cseq, 404, "Stream Not Found");
			return;
		}
		s->channel = ch;
		s->ic_rtp = (unsigned char)ic0;
		s->ic_rtcp = (unsigned char)ic1;
		if (!s->th && pthread_create(&s->th, NULL, sess_writer, s) != 0) {
			s->th = 0;
			reply_err(s, cseq, 503, "Not Enough Bandwidth");
			return;
		}
		snprintf(extra, sizeof(extra),
			"Session: %d;timeout=60\r\n"
			"Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n"
			"Server: akrtsp\r\n",
			(int)(s - g_sess) + 1, ic0, ic1);
		reply(s, cseq, extra);
		return;
	}

	if (!strcmp(method, "PLAY")) {
		if (s->channel < 0) {
			reply_err(s, cseq, 459, "Incorrect Stream Identifier");
			return;
		}
		s->playing = 1;
		snprintf(extra, sizeof(extra),
			"Session: %d;timeout=60\r\n"
			"RTP-Info: url=%s;seq=%u;rtptime=%u\r\n"
			"Server: akrtsp\r\n",
			(int)(s - g_sess) + 1, url, (unsigned)s->seq,
			g_prof[s->channel].last_ts);
		reply(s, cseq, extra);
		return;
	}

	if (!strcmp(method, "TEARDOWN")) {
		s->playing = 0;
		reply(s, cseq, "Server: akrtsp\r\n");
		return;
	}

	if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
		snprintf(extra, sizeof(extra), "Session: %d;timeout=60\r\nServer: akrtsp\r\n",
			(int)(s - g_sess) + 1);
		reply(s, cseq, extra);
		return;
	}

	if (!strcmp(method, "PAUSE")) {
		s->playing = 0;
		reply(s, cseq, "Server: akrtsp\r\n");
		return;
	}

	reply_err(s, cseq, 501, "Not Implemented");
}

static void *conn_thread(void *arg)
{
	struct sess *s = (struct sess *)arg;
	static const int CAP = 3072;
	char buf[CAP];
	int blen = 0;

	while (g_run && s->alive) {
		fd_set rfds;
		struct timeval tv;
		int r, n;

		FD_ZERO(&rfds);
		FD_SET(s->fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(s->fd + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			continue;
		n = (int)recv(s->fd, buf + blen, (size_t)(CAP - blen - 1), 0);
		if (n <= 0)
			break;
		blen += n;
		buf[blen] = 0;

		for (;;) {
			char *end = strstr(buf, "\r\n\r\n");
			int consumed;

			if (!end) {
				if (blen >= CAP - 1)
					blen = 0;	/* nonsense: reset rather than spin */
				break;
			}
			end[0] = 0;
			handle_request(s, buf);
			consumed = (int)(end - buf) + 4;
			memmove(buf, buf + consumed, (size_t)(blen - consumed) + 1);
			blen -= consumed;
			if (!g_run || !s->alive)
				break;
		}
	}
	sess_free(s);
	return NULL;
}

/* ------------------------------------------------------------------ program */

static void on_stop(int sig)
{
	/*
	 * Print the signal number: an RTSP server that quietly exits minutes after
	 * a client hangs up is a bug, and the only way to tell a real signal from a
	 * corrupted run flag is to be told which one it was.
	 */
	fprintf(stderr, "signal %d received\n", sig);
	fflush(stderr);
	g_run = 0;
}

/*
 * The teardown deadline. Reached only when the vendor close never returns; see
 * the alarm(10) in main's shutdown path.
 */
static void on_teardown_timeout(int sig)
{
	static const char msg[] =
		"teardown stalled: the vendor capture/encoder close never returned; "
		"exiting without it (the kernel releases the capture device, which is "
		"what a crash does and what the next start already handles)\n";

	(void)sig;
	/* write(), not fprintf(): a signal handler must stay async-signal-safe, and
	 * stderr is being taken by the vendor libraries at this very moment. */
	(void)write(2, msg, sizeof msg - 1);
	_exit(0);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--port 8554] [--fps 20] [--gop 20]\n"
		"         [--main 1280x720] [--sub 640x480]\n"
		"         [--main-kbps 1500] [--sub-kbps 600] [--verbose]\n"
		"         [--force-night day|night] [--auto-night] [--ir-follow] [--auto-fps]\n"
		"         [--mode-file /tmp/akrtsp.cmd] [--status-file /tmp/akrtsp.status]\n"
		"         [--auto-dark-gain 9000] [--auto-light-gain 6500]\n"
		"         [--auto-dark-secs 15] [--auto-lit-secs 30]\n"
		"         [--auto-probe-secs 16] [--auto-settle-secs 10]\n"
		"         [--auto-dwell-secs 45] [--auto-rearm-secs 300]\n"
		"         [--no-auto-lamp] [--no-auto-filter] [--ir-led-node PATH]\n"
		"         [--ptz-fifo /tmp/ptz.daemon] [--ircut-node PATH]\n"
		"         [--osd-time] [--osd-pos tl|tr|bl|br] [--osd-size 16]\n"
		"         [--osd-rect-h 32] [--osd-xoff 0] [--osd-yoff 0]\n"
		"         [--osd-fmt \"%%Y-%%m-%%d %%H:%%M:%%S\"]\n"
		"         [--osd-no-sub]\n"
		"         [--osd-font /usr/local/ak_font_16.bin] [--osd-color N]\n"
		"         [--osd-bg N] [--osd-alpha 1..100]\n"
		"         [--osd-scale 2] [--osd-nibble-hi] [--osd-no-matrix]\n"
		"         [--osd-probe]\n"
		"         [--time-status /tmp/camweb_time.status]\n"
		"\n"
		"gop is clamped to at least fps: libakstreamenc dies with SIGFPE at the\n"
		"second keyframe when the GOP is shorter than a second.\n"
		"\n"
		"Day/night. Left in day mode under this camera's own IR illuminator the\n"
		"picture is magenta (mean R-G +76, B-G +79); night mode gives R=G=B across a\n"
		"full 0..255 range, which is what the vendor's own log calls\n"
		"\"switching isp mode -> night\". The control file (--mode-file) holds one\n"
		"word - day, night or auto - and wins over --force-night, which is only the\n"
		"default for when there is no file (this camera: night).\n"
		"\n"
		"\"auto\" reads the ISP's auto-exposure, not the light sensor. The resistor\n"
		"behind ak_drv_ir_get_input_level() measured 1 (DAY) with the room light on\n"
		"AND off, and trusting it is what produced the magenta picture. What does\n"
		"respond is the analogue gain that AE spends to hold its target luminance:\n"
		"about 4.5k when the room is lit, 12.5k when it is dark in day mode, pegged\n"
		"at its 16384 ceiling when it is dark in night mode. So dark means gain at or\n"
		"above --auto-dark-gain for --auto-dark-secs, lit means at or below\n"
		"--auto-light-gain for --auto-lit-secs. A night -> day switch is never taken\n"
		"on the camera's own lamp: the lamp goes out for --auto-probe-secs, the loop\n"
		"is given --auto-settle-secs to settle, and only the reading after that\n"
		"decides; a probe that says \"still dark\" waits --auto-rearm-secs before the\n"
		"next one, and a state is held at least --auto-dwell-secs before any change.\n"
		"auto drives the illuminator (/sys/user-gpio/IR_LED, --ir-led-node, disabled\n"
		"by --no-auto-lamp) and the IR-cut filter through ptz_daemon's fifo\n"
		"(/tmp/ptz.daemon, --ptz-fifo, --no-auto-filter); the position is read\n"
		"back from --ircut-node (/sys/user-gpio/gpio-ircut_a) and published as\n"
		"ircut= so a command that moved nothing is visible in the log and the\n"
		"page. --auto-night is the old\n"
		"light-sensor path, kept for boards where that resistor is wired to\n"
		"something; --ir-follow also moves the filter on a manual mode change;\n"
		"--auto-fps hands light adaptation to libplat_vi's thread, which changes the\n"
		"sensor fps that our fixed-step RTP timestamps do not yet follow.\n"
		"The status file is rewritten every 2 s for the web UI: current and requested\n"
		"mode, the ae readings, what auto decided and why, fps, clients and the\n"
		"frame counters.\n"
		"\n"
		"--osd-time burns the clock into the picture, on both channels, through the\n"
		"ISP's own overlay (ak_osd): the text is in the pixels before the encoder\n"
		"runs, so the NVR's recordings carry it and it costs the core nothing. It\n"
		"refuses to draw a date it cannot vouch for: the clock's state comes from\n"
		"--time-status, written by /mnt/camweb/time/run_time.sh, and unless that\n"
		"says synced and the epoch looks like a date the overlay reads NO TIME - a\n"
		"wrong date in a recording cannot be taken back out afterwards. This board\n"
		"has no rtc at all, so on a box that has never synced, NO TIME is the true\n"
		"answer and not a fault. This library gives each video channel one rect and\n"
		"draws the string inside it, in the 16px font at /usr/local/ak_font_16.bin -\n"
		"the same font and the same overlay path the vendor's own boot log uses on\n"
		"this board.\n", argv0);
}

static int parse_size(const char *s, int *w, int *h)
{
	return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

int main(int argc, char **argv)
{
	int port = 8554, fps = 20, gop = 20;
	int main_kbps = 1500, sub_kbps = 600;
	pthread_t prod_th[N_CH];
	pthread_t light_th;
	int light_on = 0;
	int listen_fd, ch, i, c, ignored = 0;
	struct sockaddr_in sa;
	static const struct option longopts[] = {
		{ "port", 1, 0, 'p' }, { "fps", 1, 0, 'F' }, { "gop", 1, 0, 'G' },
		{ "main", 1, 0, 'M' }, { "sub", 1, 0, 'S' },
		{ "main-kbps", 1, 0, 'b' }, { "sub-kbps", 1, 0, 'k' },
		{ "no-night", 0, 0, 'N' }, { "force-night", 1, 0, 'n' },
		{ "auto-night", 0, 0, 'A' }, { "ir-follow", 0, 0, 'f' },
		{ "auto-dark-gain", 1, 0, 'D' }, { "auto-light-gain", 1, 0, 'L' },
		{ "auto-dark-secs", 1, 0, 'd' }, { "auto-lit-secs", 1, 0, 'l' },
		{ "auto-probe-secs", 1, 0, 'P' }, { "auto-settle-secs", 1, 0, 'e' },
		{ "auto-dwell-secs", 1, 0, 'w' }, { "auto-rearm-secs", 1, 0, 'R' },
		{ "no-auto-lamp", 0, 0, 'x' }, { "no-auto-filter", 0, 0, 'y' },
		{ "ir-led-node", 1, 0, 'I' }, { "ptz-fifo", 1, 0, 'u' },
		{ "ircut-node", 1, 0, 'C' },
		{ "auto-fps", 0, 0, 'a' },
		{ "mode-file", 1, 0, 'm' }, { "status-file", 1, 0, 's' },
		{ "osd-time", 0, 0, 'O' }, { "osd-pos", 1, 0, 'o' },
		{ "osd-size", 1, 0, 'Z' }, { "osd-rect-h", 1, 0, 't' },
		{ "osd-xoff", 1, 0, 'X' }, { "osd-yoff", 1, 0, 'Y' },
		{ "osd-font", 1, 0, 'J' },
		{ "osd-fmt", 1, 0, 'T' }, { "osd-no-sub", 0, 0, 'Q' },
		{ "osd-color", 1, 0, 'W' }, { "osd-bg", 1, 0, 'B' },
		{ "osd-alpha", 1, 0, 'H' },
		{ "osd-no-matrix", 0, 0, 'z' }, { "osd-probe", 0, 0, 'q' },
		{ "osd-scale", 1, 0, 'j' }, { "osd-nibble-hi", 0, 0, 'g' },
		{ "time-status", 1, 0, 'K' },
		{ "verbose", 0, 0, 'v' }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 }
	};

	g_prof[CH_MAIN].name = "main";
	g_prof[CH_SUB].name = "sub";
	g_prof[CH_MAIN].w = 1280;
	g_prof[CH_MAIN].h = 720;
	g_prof[CH_SUB].w = 640;
	g_prof[CH_SUB].h = 480;

	while ((c = getopt_long(argc, argv,
			"p:F:G:M:S:b:k:n:aNAfm:s:D:L:d:l:P:e:w:R:x:y:I:u:C:Oo:Z:t:X:Y:J:T:QW:B:H:K:j:qzgvh",
			longopts, NULL)) != -1) {
		switch (c) {
		case 'p': port = atoi(optarg); break;
		case 'F': fps = atoi(optarg); break;
		case 'G': gop = atoi(optarg); break;
		case 'M': if (!parse_size(optarg, &g_prof[CH_MAIN].w, &g_prof[CH_MAIN].h)) usage(argv[0]), exit(2); break;
		case 'S': if (!parse_size(optarg, &g_prof[CH_SUB].w, &g_prof[CH_SUB].h)) usage(argv[0]), exit(2); break;
		case 'b': main_kbps = atoi(optarg); break;
		case 'k': sub_kbps = atoi(optarg); break;
		case 'N': g_auto_night = 0; break;
		case 'A': g_auto_night = 1; break;
		case 'f': g_ir_follow = 1; break;
		case 'a': g_auto_fps = 1; break;
		case 'D': g_auto_dark_gain = atoi(optarg); break;
		case 'L': g_auto_light_gain = atoi(optarg); break;
		case 'd': g_auto_dark_secs = atoi(optarg); break;
		case 'l': g_auto_lit_secs = atoi(optarg); break;
		case 'P': g_auto_probe_secs = atoi(optarg); break;
		case 'e': g_auto_settle_secs = atoi(optarg); break;
		case 'w': g_auto_dwell_secs = atoi(optarg); break;
		case 'R': g_auto_rearm_secs = atoi(optarg); break;
		case 'x': g_auto_lamp = 0; break;
		case 'y': g_auto_filter = 0; break;
		case 'I':
			snprintf(g_irled_node, sizeof g_irled_node, "%s", optarg);
			break;
		case 'u':
			snprintf(g_ptz_fifo, sizeof g_ptz_fifo, "%s", optarg);
			break;
		case 'C':
			snprintf(g_ircut_node, sizeof g_ircut_node, "%s", optarg);
			break;
		case 'm':
			snprintf(g_mode_file, sizeof g_mode_file, "%s", optarg);
			break;
		case 's':
			snprintf(g_status_file, sizeof g_status_file, "%s", optarg);
			break;
		case 'n':
			if (!strcmp(optarg, "day"))
				g_force_night = 1;
			else if (!strcmp(optarg, "night"))
				g_force_night = 0;
			else
				usage(argv[0]), exit(2);
			break;
		case 'O': g_osd = 1; break;
		case 'o':
			if (strcmp(optarg, "tl") && strcmp(optarg, "tr") &&
			    strcmp(optarg, "bl") && strcmp(optarg, "br")) {
				fprintf(stderr, "--osd-pos must be tl, tr, bl or br\n");
				usage(argv[0]);
				return 2;
			}
			snprintf(g_osd_pos, sizeof g_osd_pos, "%s", optarg);
			break;
		case 'Z':
			g_osd_size = atoi(optarg);
			if (g_osd_size < 8 || g_osd_size > 64)
				die("--osd-size must be 8..64 px (the font this "
				    "board ships is 16)\n");
			break;
		case 't':
			g_osd_rect_h = atoi(optarg);
			if (g_osd_rect_h < 8 || g_osd_rect_h > 128)
				die("--osd-rect-h must be 8..128 px\n");
			break;
		case 'X':
			g_osd_xoff = atoi(optarg);
			if (g_osd_xoff < 0 || g_osd_xoff > 128)
				die("--osd-xoff must be 0..128 px\n");
			break;
		case 'Y':
			g_osd_yoff = atoi(optarg);
			if (g_osd_yoff < 0 || g_osd_yoff > 128)
				die("--osd-yoff must be 0..128 px\n");
			break;
		case 'J':
			snprintf(g_osd_font_file, sizeof g_osd_font_file, "%s", optarg);
			break;
		case 'z': g_osd_matrix = 0; break;
		case 'q': g_osd_probe = 1; break;
		case 'g': g_osd_nibble_hi = 1; break;
		case 'j':
			g_osd_scale = atoi(optarg);
			if (g_osd_scale < 1 || g_osd_scale > 4)
				die("--osd-scale must be 1..4 (the vendor's cell "
				    "is 8x16)\n");
			break;
		case 'T':
			snprintf(g_osd_fmt, sizeof g_osd_fmt, "%s", optarg);
			break;
		case 'Q': g_osd_ch[CH_SUB] = 0; break;
		case 'W': case 'B': {
			int v = atoi(optarg);

			/* 0..15 because that is the size of the library's colour
			 * table, not an arbitrary limit: an id out of it reads
			 * whatever is past the end of the array. There is no
			 * --osd-edge on purpose - ak_osd_set_edge_color is declared
			 * in the header and not exported by this board's libmpi_osd,
			 * so offering it would be offering a flag that does nothing. */
			if (v < 0 || v > 15)
				die("--osd-color/--osd-bg take a colour id 0..15\n");
			if (c == 'W')
				g_osd_color = v;
			else
				g_osd_bg = v;
			break;
		}
		case 'H':
			g_osd_alpha = atoi(optarg);
			if (g_osd_alpha < 1 || g_osd_alpha > 100)
				die("--osd-alpha must be 1..100\n");
			break;
		case 'K':
			snprintf(g_time_status_file, sizeof g_time_status_file, "%s", optarg);
			break;
		case 'v': g_verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		case '?':
		default:
			/*
			 * An option this build has never heard of is a complaint, not a
			 * reason to stop serving. The argument line lives in
			 * /mnt/camweb/akrtsp.args and is read at every start, so a line
			 * written for a newer binary than the one on the card is version
			 * skew waiting to happen - and it happened: one word the deployed
			 * build did not know made this process print its usage and exit,
			 * the supervisor counted three failed starts and handed :554 to the
			 * vendor server, so an overlay flag cost the camera its whole
			 * pipeline. getopt has already named the word and skipped it; count
			 * what was skipped and let the log say so.
			 */
			ignored++;
			continue;
		}
	}
	if (ignored)
		printf("args: %d option(s) this build does not know, ignored - "
		       "check /mnt/camweb/akrtsp.args against --help\n", ignored);
	if (fps < 1 || fps > 30)
		die("fps must be 1..30 (sensor reports 1280x720; the ISP conf runs 15)\n");
	if (gop < fps) {
		printf("gop %d raised to %d: shorter than one second and libakstreamenc "
			"crashes at the 2nd keyframe\n", gop, fps);
		gop = fps;
	}
	for (ch = 0; ch < N_CH; ch++) {
		g_prof[ch].fps = fps;
		g_prof[ch].gop = gop;
	}
	g_prof[CH_MAIN].kbps = main_kbps;
	g_prof[CH_SUB].kbps = sub_kbps;

	/*
	 * The auto band has to be a band: if "lit" starts at or above where "dark"
	 * starts, the policy sits on the same number twice and oscillates the lamp,
	 * the filter and the ISP a few times a minute. That is worth refusing rather
	 * than shipping, because the symptom is a camera that strobes its own
	 * illuminator.
	 */
	if (g_auto_light_gain >= g_auto_dark_gain)
		die("auto: --auto-light-gain (%d) must be below --auto-dark-gain (%d)\n",
		    g_auto_light_gain, g_auto_dark_gain);
	if (g_auto_probe_secs > 0 && g_auto_settle_secs >= g_auto_probe_secs)
		die("auto: --auto-settle-secs (%d) must be shorter than "
		    "--auto-probe-secs (%d): the reading taken after settling is the "
		    "one that decides\n", g_auto_settle_secs, g_auto_probe_secs);
	if (g_auto_dark_secs < 1)
		g_auto_dark_secs = 1;
	if (g_auto_lit_secs < 1)
		g_auto_lit_secs = 1;
	if (g_auto_dwell_secs < 1)
		g_auto_dwell_secs = 1;

	setvbuf(stdout, NULL, _IONBF, 0);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGPIPE, SIG_IGN);

	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		die("socket: %s\n", strerror(errno));
	{
		int one = 1;

		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	}
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons((unsigned short)port);
	if (bind(listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("bind :%d: %s - if the vendor rtsp owns this port, start on 8554 first\n",
			port, strerror(errno));
	if (listen(listen_fd, 4) < 0)
		die("listen: %s\n", strerror(errno));

	pipeline_open();
	/*
	 * The overlay has to exist before the first frames are served, and it has
	 * to know whether the clock is real before it sizes itself, so the clock
	 * state is read once here and then every second in the day/night thread.
	 * An overlay that cannot come up is a broken feature, not a reason not to
	 * stream: say so in the log and the status file and carry on.
	 */
	clock_poll();
	if (g_osd == 1) {
		if (osd_open() != 0)
			g_osd = -1;
		else
			osd_draw(1);
	} else {
		printf("osd: overlay off (--osd-time burns the clock in)\n");
	}
	for (ch = 0; ch < N_CH; ch++) {
		if (pthread_create(&prod_th[ch], NULL, producer, &g_prof[ch]) != 0)
			die("pthread_create producer %s\n", g_prof[ch].name);
	}
	/*
	 * The day/night thread always runs: it applies the control file, falls back to
	 * --force-night when there is none, and writes the status file the web UI reads.
	 * --auto-night is no longer what turns it on, so a mode can be changed from the
	 * browser without restarting the server.
	 */
	if (pthread_create(&light_th, NULL, light_watch, NULL) != 0)
		logmsg("day/night: cannot start the light thread; the ISP will stay "
		       "in the mode it booted into and there will be no status file\n");
	else
		light_on = 1;
	if (g_force_night < 0 && !g_auto_night)
		logmsg("day/night: no mode asked for yet. Left in day mode under this "
		       "camera's own IR illuminator the picture is magenta (mean R-G +76, "
		       "B-G +79); write 'night' to %s, or use --force-night night. The "
		       "light sensor reports DAY here even in the dark, so 'auto' is not "
		       "a safe default indoors.\n", g_mode_file);

	printf("akrtsp on :%d  (rtsp://<camera>:%d/vs1 = %dx%d@%d, /vs0 = %dx%d@%d)\n",
		port, port, g_prof[CH_SUB].w, g_prof[CH_SUB].h, fps,
		g_prof[CH_MAIN].w, g_prof[CH_MAIN].h, fps);

	while (g_run) {
		fd_set rfds;
		struct timeval tv;
		int r, cfd;
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		struct sess *s;
		pthread_t th;
		int one = 1;
		struct timeval tmo;

		FD_ZERO(&rfds);
		FD_SET(listen_fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		r = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
		if (r <= 0) {
			if (r < 0 && errno != EINTR)
				logmsg("select: %s\n", strerror(errno));
			continue;
		}
		cfd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
		if (cfd < 0)
			continue;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
		s = sess_alloc(cfd, inet_ntoa(peer.sin_addr));
		if (!s) {
			fprintf(stderr, "no free session slot for %s\n", inet_ntoa(peer.sin_addr));
			close(cfd);
			continue;
		}
		if (pthread_create(&th, NULL, conn_thread, s) != 0) {
			fprintf(stderr, "cannot serve %s: no thread\n", s->peer);
			sess_free(s);
		}
	}

	printf("stopping\n");
	g_run = 0;
	/*
	 * Bounded teardown. ak_vi_close() waits on a thread the vendor library starts
	 * for its own light adaptation (vi_change_fps), and that thread does not always
	 * come back: measured after a day/night switch, the process then sat in
	 * futex_wait with the encoder already gone and :554 still held, so the
	 * supervisor believed akrtsp was healthy and never restarted it - the camera
	 * streamed nothing until someone noticed and killed it by hand. The cleanup
	 * therefore runs against a 10 s deadline; past it the process exits by itself
	 * and the kernel releases the capture device, which is what a crash does and
	 * what the next start already copes with.
	 */
	signal(SIGALRM, on_teardown_timeout);
	alarm(10);
	for (ch = 0; ch < N_CH; ch++)
		if (g_prof[ch].stream)
			ak_venc_cancel_stream(g_prof[ch].stream);
	for (ch = 0; ch < N_CH; ch++)
		pthread_join(prod_th[ch], NULL);
	if (light_on)
		pthread_join(light_th, NULL);	/* polls in 1 s slices, so this waits at most a second */
	/*
	 * The overlay owns pmem and is registered against the vi handle, so it
	 * goes away between the last drawer stopping and the device closing: after
	 * ak_vi_close() there is nothing left for it to talk to.
	 */
	if (g_osd == 1)
		ak_osd_destroy();
	for (i = 0; i < MAX_SESS; i++)
		if (g_sess[i].used)
			sess_free(&g_sess[i]);
	for (ch = 0; ch < N_CH; ch++) {
		if (g_prof[ch].enc)
			ak_venc_close(g_prof[ch].enc);
		printf("%s: %ld frames, %ld key frames, %ld bytes\n",
			g_prof[ch].name, g_prof[ch].frames, g_prof[ch].keyframes, g_prof[ch].bytes);
	}
	if (g_prof[CH_MAIN].vi) {
		ak_vi_capture_off(g_prof[CH_MAIN].vi);
		ak_vi_close(g_prof[CH_MAIN].vi);
	}
	close(listen_fd);
	alarm(0);					/* teardown finished inside the deadline */
	return 0;
}
