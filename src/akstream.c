/*
 * akstream - capture and H.264 encode on the AK3918 without the vendor's RTSP
 * server.
 *
 * Why: /mnt/Factory/custom/rtsp owns the capture pipeline AND serves RTSP, and
 * the serving half is what costs the camera its CPU. Measured on this board
 * (tools/rtsp.py --measure, one NVR connected, nobody watching the web UI): the
 * process takes ~33% of the single 400 MHz core, of which only ~2.3 points are
 * venc_encode; ~19 points are its RTSP session threads, 8.25 points of that in
 * the kernel. It also encodes 1280x720 AND 640x480 all day whether anyone asks
 * for them or not, and its audio path (ai_capture plus the kernel aec_task)
 * costs another ~9 points for G.711 that Chrome cannot even decode.
 *
 * Milestone 1 (this file as it stands) proves the SDK ABI end to end: it drives
 * the same vendor libraries the stock binary uses, with fps set explicitly and
 * the SDK's adaptive frame-rate throttle (the "vi_change_fps" thread, which is
 * what holds this camera to ~10 fps while the SDP advertises 25) switched off,
 * then writes the elementary stream to a file so ffprobe can tell us the truth.
 *
 * The struct layouts are not guessed. ak_venc_open in THIS camera's
 * libmpi_venc.so (md5 19bb0f8046e434894ae490263dc876bf) reads the param block
 * at offsets 0x00 04 08 0c 10 14 18 1c 24 28 2c, which is exactly
 * width/height/minqp/maxqp/fps/goplen/bps/profile/use_chn/enc_grp/br_mode/
 * enc_out_type in the published ak_venc.h - checked with objdump before a line
 * of this code was written. The libraries linked against are md5-identical to
 * the ones on the camera, not a neighbouring build.
 *
 * The VI side is the exception, and it cost the most time: libplat_vi V2.3.01
 * does NOT use the published struct video_channel_attr (48 bytes, with
 * max_width/max_height per channel). Its validator reads main w/h at 16/20 and
 * sub w/h at 24/28 - two ints per channel, 32 bytes total. See src/vi_abi.h.
 * Filling the header struct as documented writes main's max pair over the sub
 * channel, and the library then complains about the values we never meant to
 * put there.
 *
 * Usage (on the camera):
 *   akstream --dump /tmp/out.h264 -W 640 -H 480 -F 20 -G 15 -B 512 -t 6
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <ucontext.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "ak_global.h"
#include "ak_vi.h"
#include "ak_venc.h"
#include "vi_abi.h"

static volatile int g_run = 1;
static volatile int g_frames = 0;

static void on_term(int sig)
{
	(void)sig;
	g_run = 0;
}

/*
 * The SDK encoder dies with SIGFPE a second into capture, on both channels,
 * after delivering real H.264.
 *
 * On ARMv5 there is no hardware divide fault (udiv/sdiv are ARMv7+), so integer
 * division is done by the soft helper and a zero divisor is reported by the
 * helper calling raise(SIGFPE). That is what the addresses said: the first
 * version of this handler reported pc=libpthread+0x6f10 / lr=libpthread+0x6ef0,
 * and nm shows raise() lives at libpthread+0x6ee4 - i.e. the fault context is
 * raise() itself, not the code that divided. So the useful thing to print is the
 * call chain: walk the faulting thread's stack and report every word that looks
 * like a return address inside an executable mapping. Crude, but with no cross
 * gdb in this toolchain and no gdb on the camera it names the SDK function that
 * divided by zero, which is the difference between guessing and knowing.
 *
 * fprintf from a signal handler is not async-signal-safe; accepted because the
 * handler reports and exits immediately. The stack walk is limited to the
 * faulting thread's own stack to keep it from faulting inside the handler.
 */
struct map {
	unsigned long a;
	unsigned long b;
	int exec_;
	char path[64];
};

static struct map g_maps[96];
static int g_nmaps;

static void load_maps(void)
{
	FILE *m = fopen("/proc/self/maps", "r");
	char line[512];

	g_nmaps = 0;
	if (!m)
		return;
	while (fgets(line, sizeof(line), m) && g_nmaps < (int)(sizeof(g_maps) / sizeof(g_maps[0]))) {
		unsigned long a = 0, b = 0;
		char perms[8], path[64];
		path[0] = 0;
		perms[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %63s", &a, &b, perms, path) >= 3) {
			g_maps[g_nmaps].a = a;
			g_maps[g_nmaps].b = b;
			g_maps[g_nmaps].exec_ = (perms[2] == 'x');
			snprintf(g_maps[g_nmaps].path, sizeof(g_maps[g_nmaps].path), "%s", path);
			g_nmaps++;
		}
	}
	fclose(m);
}

static int resolve(unsigned long addr, char *out, int n)
{
	int i;

	for (i = 0; i < g_nmaps; i++) {
		if (addr >= g_maps[i].a && addr < g_maps[i].b) {
			snprintf(out, n, "%s + 0x%lx",
				g_maps[i].path[0] ? g_maps[i].path : "[anon]", addr - g_maps[i].a);
			return g_maps[i].exec_;
		}
	}
	out[0] = 0;
	return 0;
}

static void on_fpe(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = (ucontext_t *)ucv;
	unsigned long pc = uc->uc_mcontext.arm_pc;
	unsigned long lr = uc->uc_mcontext.arm_lr;
	unsigned long sp = uc->uc_mcontext.arm_sp & ~3UL;
	unsigned long fp = uc->uc_mcontext.arm_fp;
	char sym[160];
	unsigned long *p;
	int i;

	(void)sig;
	(void)si;
	load_maps();
	fprintf(stderr, "\nSIGFPE after %d frames\n", g_frames);
	fprintf(stderr, "  r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
		uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1,
		uc->uc_mcontext.arm_r2, uc->uc_mcontext.arm_r3);
	if (resolve(pc, sym, sizeof(sym)))
		fprintf(stderr, "  pc %08lx = %s\n", pc, sym);
	if (resolve(lr, sym, sizeof(sym)))
		fprintf(stderr, "  lr %08lx = %s\n", lr, sym);
	if (fp && resolve(fp, sym, sizeof(sym)))
		fprintf(stderr, "  fp %08lx = %s\n", fp, sym);
	fflush(stderr);

	/* stack scan: return addresses only, own thread stack, bounded */
	for (p = (unsigned long *)sp, i = 0; i < 400; p++, i++) {
		unsigned long v = *p;
		if (resolve(v, sym, sizeof(sym)) == 1) {
			fprintf(stderr, "  [%08lx] %s\n", (unsigned long)p, sym);
			fflush(stderr);
		}
	}
	fflush(stderr);
	_exit(136);
}

static long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static const char *isp_conf(void)
{
	/*
	 * ak_vi_match_sensor accepts a directory and searches it for a matching
	 * sensor config; the vendor binary passes "/etc/jffs2/", whose isp conf is
	 * isp_gc1084.conf. Do the same - it survives a sensor change - but check
	 * the directory exists first so a failure message says something useful.
	 */
	static const char *dirs[] = { "/etc/jffs2/", "/mnt/camweb/", NULL };
	int i;
	for (i = 0; dirs[i]; i++) {
		if (access(dirs[i], R_OK) == 0)
			return dirs[i];
	}
	return "/etc/jffs2/";
}
int main(int argc, char **argv)
{
	int w = 640, h = 480, fps = 20, gop = 15, kbps = 512;
	int secs = 6, sub = 1;
	int fps_ctl = 1;	/* 0: leave VI fps alone (vendor behaviour) */
	int scene = -1;		/* -1: don't touch ak_venc_set_check_scene */
	int trace = 0;		/* 1: mark every frame on stderr */
	int cbr = 0;		/* 1: BR_MODE_CBR instead of VBR */
	const char *dump = NULL;
	const char *vi_cfg = NULL;
	FILE *out = NULL;
	long t0, t_end;
	int ret, frames = 0, keyframes = 0, annexb = -1;
	long bytes = 0;
	struct rusage ru0, ru1;
	void *vi, *enc, *stream;
	struct video_resolution res;
	struct vi_channel_attr_real attr;
	struct encode_param ep;
	struct video_stream vs;
	static const struct option longopts[] = {
		{ "dump", 1, 0, 'd' }, { "width", 1, 0, 'W' }, { "height", 1, 0, 'H' },
		{ "fps", 1, 0, 'F' }, { "gop", 1, 0, 'G' }, { "kbps", 1, 0, 'B' },
		{ "seconds", 1, 0, 't' }, { "sub", 0, 0, 's' }, { "main", 0, 0, 'm' },
		{ "isp", 1, 0, 'i' },
		{ "no-fps-ctl", 0, 0, 200 }, { "scene", 1, 0, 201 }, { "trace", 0, 0, 202 },
		{ "cbr", 0, 0, 203 },
		{ 0, 0, 0, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "d:W:H:F:G:B:t:smi:", longopts, NULL)) != -1) {
		switch (c) {
		case 'd': dump = optarg; break;
		case 'W': w = atoi(optarg); break;
		case 'H': h = atoi(optarg); break;
		case 'F': fps = atoi(optarg); break;
		case 'G': gop = atoi(optarg); break;
		case 'B': kbps = atoi(optarg); break;
		case 't': secs = atoi(optarg); break;
		case 's': sub = 1; break;
		case 'm': sub = 0; break;
		case 'i': vi_cfg = optarg; break;
		case 200: fps_ctl = 0; break;
		case 201: scene = atoi(optarg); break;
		case 202: trace = 1; break;
		case 203: cbr = 1; break;
		default:
			fprintf(stderr, "usage: %s --dump FILE [-W 640 -H 480 -F 20 -G 15"
				" -B 512 -t SECS] [--main|--sub] [--isp DIR]"
				" [--no-fps-ctl] [--scene 0|1] [--trace] [--cbr]\n", argv[0]);
			return 2;
		}
	}
	if (!dump) {
		fprintf(stderr, "--dump FILE is required for this milestone\n");
		return 2;
	}

	/*
	 * libakstreamenc's VBR rate control computes, at every GOP boundary,
	 * a divisor that is itself an integer division (obj->field40 /
	 * (ctx[88] / ctx[92]), see VBR_ratecontrol+0x11c..0x188 in that library).
	 * When the GOP is shorter than one second that inner quotient truncates to
	 * zero and the next division faults: the encoder raises SIGFPE through
	 * libgcc's __aeabi_ldiv0 at the SECOND keyframe, on every run, after
	 * encoding normally for exactly `goplen - 1` frames.
	 *
	 * Measured on this board (frame count at the fault, from the handler):
	 *   fps 20 gop 5  -> dies after 4 frames
	 *   fps 20 gop 10 -> dies after 9 frames
	 *   fps 20 gop 15 -> dies after 14 frames   (VBR and CBR alike)
	 *   fps 20 gop 20 -> fine, 19.86 fps over 8 s
	 *   fps 10 gop 10 -> fine, 9.66 fps over 6 s
	 *   fps 15 gop 30 -> fine, 14.91 fps over 12 s
	 * So goplen >= fps is a hard requirement of this encoder build, not a
	 * tuning preference - and it is why the stock binary runs gop 50 at 25 fps.
	 * Any profile we ship has to honour it; the server will clamp rather than
	 * crash, and say so.
	 */
	if (gop < fps) {
		printf("gop %d < fps %d: libakstreamenc rate control dies with SIGFPE at the "
			"2nd keyframe; raising gop to %d\n", gop, fps, fps);
		gop = fps;
	}

	signal(SIGINT, on_term);
	signal(SIGTERM, on_term);
	signal(SIGPIPE, SIG_IGN);
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_fpe;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGFPE, &sa, NULL);
	}

	printf("libplat_vi  %s\n", ak_vi_get_version());
	printf("libmpi_venc %s\n", ak_venc_get_version());

	if (!vi_cfg)
		vi_cfg = isp_conf();
	printf("matching sensor from %s\n", vi_cfg);
	ret = ak_vi_match_sensor(vi_cfg);
	if (ret) {
		fprintf(stderr, "ak_vi_match_sensor failed (%d) - is the vendor rtsp still\n"
			"holding /dev/video0? Stop it first (tools/rtsp.py); this box has one\n"
			"capture pipeline.\n", ret);
		return 1;
	}

	vi = ak_vi_open(VIDEO_DEV0);
	if (!vi) {
		fprintf(stderr, "ak_vi_open failed\n");
		return 1;
	}
	printf("vi handle %p\n", vi);

	memset(&res, 0, sizeof(res));
	if (ak_vi_get_sensor_resolution(vi, &res)) {
		fprintf(stderr, "ak_vi_get_sensor_resolution failed\n");
		return 1;
	}
	printf("sensor %dx%d (max %dx%d)\n", res.width, res.height, res.max_width, res.max_height);

	memset(&attr, 0, sizeof(attr));
	attr.crop_left = 0;
	attr.crop_top = 0;
	attr.crop_width = res.width;
	attr.crop_height = res.height;
	/*
	 * The size we encode has to be the size the matching VI channel is set to:
	 * ak_venc_open is told w x h, and ak_venc_get_stream then hands back frames
	 * from that channel. So the requested stream drives the channel it will be
	 * encoded from, and the other channel stays at a legal default.
	 *
	 * Bounds enforced by this build's validator (measured with akvi_probe, and
	 * matching the compares in ak_vi_set_channel_attr): main must be
	 * 640..1280 wide and 480..960 high; sub must be at most 640x480. Catching a
	 * violation here beats a failure deep inside capture setup.
	 */
	if (sub) {
		attr.main_width = 1280;
		attr.main_height = 720;
		attr.sub_width = w;
		attr.sub_height = h;
		if (w > 640 || h > 480) {
			fprintf(stderr, "sub channel is capped at 640x480 by this build, "
				"asked for %dx%d (use --main for bigger)\n", w, h);
			return 1;
		}
	} else {
		attr.main_width = w;
		attr.main_height = h;
		attr.sub_width = 320;
		attr.sub_height = 240;
		if (w < 640 || h < 480 || w > 1280 || h > 960) {
			fprintf(stderr, "main channel must be 640..1280 x 480..960, "
				"asked for %dx%d\n", w, h);
			return 1;
		}
	}
	if (ak_vi_set_channel_attr(vi, (struct video_channel_attr *)&attr)) {
		fprintf(stderr, "ak_vi_set_channel_attr failed for %dx%d on %s channel "
			"(main must be 640..1280 x 480..960, sub <= 640x480)\n",
			w, h, sub ? "sub" : "main");
		return 1;
	}

	/*
	 * The calls the vendor binary never makes, and part of the reason for
	 * writing this: sensor frame rate under our control, and the adaptive
	 * fps-switching feature (the vi_change_fps thread) off, so the encoder is
	 * not quietly starved to ~10 fps behind the 25 the SDP advertises.
	 *
	 * Measured on this build: ak_vi_set_fps() returns 0 for 15/20/25 yet
	 * ak_vi_get_fps() reports 30 before and after every call, so it confirms
	 * nothing by itself; and the ISP's own log shows it re-setting the sensor
	 * to 15 (from isp_gc1084.conf) and later to 10 while we run. --no-fps-ctl
	 * skips both calls, because the SDK then dies with SIGFPE either way and
	 * the pair is the first thing worth excluding.
	 */
	if (fps_ctl) {
		if (ak_vi_set_fps(vi, fps))
			printf("ak_vi_set_fps(%d) failed - continuing, venc fps still applies\n", fps);
		if (ak_vi_set_switch_fps_enable(vi, 0))
			printf("ak_vi_set_switch_fps_enable(0) rejected on this build\n");
	} else {
		printf("skipping ak_vi_set_fps / set_switch_fps_enable (vendor behaviour)\n");
	}

	if (ak_vi_capture_on(vi)) {
		fprintf(stderr, "ak_vi_capture_on failed\n");
		return 1;
	}

	memset(&ep, 0, sizeof(ep));
	ep.width = w;
	ep.height = h;
	ep.minqp = 28;
	ep.maxqp = 42;
	ep.fps = fps;
	ep.goplen = gop;
	ep.bps = kbps;
	ep.profile = PROFILE_BASE;
	ep.use_chn = sub ? ENCODE_SUB_CHN : ENCODE_MAIN_CHN;
	ep.enc_grp = sub ? ENCODE_SUBCHN_NET : ENCODE_MAINCHN_NET;
	ep.br_mode = cbr ? BR_MODE_CBR : BR_MODE_VBR;
	ep.enc_out_type = H264_ENC_TYPE;

	enc = ak_venc_open(&ep);
	if (!enc) {
		fprintf(stderr, "ak_venc_open failed for %dx%d@%d gop=%d %dkbps\n", w, h, fps, gop, kbps);
		return 1;
	}
	printf("encoder open: %dx%d@%d gop=%d %dkbps %s\n", w, h, fps, gop, kbps,
		sub ? "sub-chn" : "main-chn");

	/*
	 * ak_venc_set_check_scene is documented as "default switch flag is open",
	 * and the encoder dies of SIGFPE a second into capture on both channels.
	 * Scene checking computes rate statistics, so it is the first suspect for
	 * the divide; --scene 0 turns it off before the stream is requested, as
	 * the header requires.
	 */
	if (scene >= 0)
		printf("ak_venc_set_check_scene(%d) -> %d\n", scene, ak_venc_set_check_scene(enc, scene));

	stream = ak_venc_request_stream(vi, enc);
	if (!stream) {
		fprintf(stderr, "ak_venc_request_stream failed\n");
		return 1;
	}

	out = fopen(dump, "wb");
	if (!out) {
		perror("open dump file");
		return 1;
	}

	t0 = now_ms();
	t_end = t0 + secs * 1000L;
	getrusage(RUSAGE_SELF, &ru0);
	printf("capturing %ds -> %s\n", secs, dump);
	while (g_run && now_ms() < t_end) {
		memset(&vs, 0, sizeof(vs));
		ret = ak_venc_get_stream(stream, &vs);
		if (ret) {
			usleep(5000);
			continue;
		}
		if (vs.data && vs.len) {
			/*
			 * The elementary-stream format decides how the RTP packetiser
			 * has to be written, so report what the SDK actually hands us
			 * instead of assuming Annex-B: FU-A splitting needs NAL units,
			 * and length-prefixed AVCC would need converting first.
			 */
			if (annexb < 0 && vs.len > 5) {
				unsigned char *d = vs.data;
				annexb = (d[0] == 0 && d[1] == 0 && (d[2] == 1 || d[3] == 1));
				printf("first frame: %s, %u bytes, frame_type=%d, ts=%llu seq=%lu\n",
					annexb ? "annex-b (start codes)" : "length-prefixed (avcc)",
					vs.len, (int)vs.frame_type, vs.ts, vs.seq_no);
			}
			if (trace && frames < 3)
				printf("frame %d: ts=%llu seq=%lu len=%u\n",
					frames + 1, vs.ts, vs.seq_no, vs.len);
			if (vs.frame_type == FRAME_TYPE_I)
				keyframes++;
			fwrite(vs.data, 1, vs.len, out);
			bytes += vs.len;
			frames++;
			g_frames = frames;
			/*
			 * Marker per frame on stderr: when the SDK dies, the last
			 * marker plus the SIGFPE report says whether the fault is in
			 * get_stream, in release_stream, or in an SDK thread of its own.
			 */
			if (trace && frames <= 120)
				fprintf(stderr, "f%d(%u)%s", frames, vs.len,
					frames % 10 ? " " : "\n");
		}
		ak_venc_release_stream(stream, &vs);
	}
	fputc('\n', stderr);
	fclose(out);

	{
		long el = now_ms() - t0;
		long cpu;
		getrusage(RUSAGE_SELF, &ru1);
		cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) * 1000L
			+ (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) * 1000L
			+ (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1000L
			+ (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1000L;
		printf("%d frames, %d key frames, %ld bytes in %ld ms -> %0.2f fps, %0.0f kbps\n",
			frames, keyframes, bytes, el, frames * 1000.0 / (el ? el : 1),
			bytes * 8.0 / (el ? el : 1));
		if (keyframes > 1)
			printf("keyframe every %0.2f s (asked for gop %d at %d fps)\n",
				(double)el / 1000.0 / (keyframes - 1), gop, fps);
		/*
		 * RUSAGE_SELF covers every thread in this process, including the SDK's
		 * capture/encode threads, so this is the whole cost of our producer -
		 * the number the vendor's ~33% is being compared against.
		 */
		printf("process cpu: %ld ms user+sys = %0.1f%% of one core\n",
			cpu, el > 0 ? cpu * 100.0 / (double)el : 0.0);
		/* What the SDK itself believes about the running encoder. */
		printf("venc reports fps=%d kbps=%d gop=%d\n",
			ak_venc_get_fps(enc), ak_venc_get_kbps(enc), ak_venc_get_gop_len(enc));
	}

	ak_venc_cancel_stream(stream);
	ak_venc_close(enc);
	ak_vi_capture_off(vi);
	ak_vi_close(vi);
	return 0;
}

