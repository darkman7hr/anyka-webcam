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
#include "vi_abi.h"

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

				if (wall - ours > 18000)
					p->ts_base += (unsigned)(wall - ours);
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
		"clients=%d\nframes=%ld,%ld\nkeys=%ld,%ld\nbytes=%ld,%ld\n",
		(int)getpid(),
		g_mode < 0 ? "unset" : (g_mode ? "day" : "night"),
		want < 0 ? "none" : (want == 2 ? "auto" : (want ? "day" : "night")),
		ldr, g_have_ir,
		g_prof[CH_MAIN].fps, g_prof[CH_SUB].fps, g_prof[CH_MAIN].gop,
		clients,
		g_prof[CH_MAIN].frames, g_prof[CH_SUB].frames,
		g_prof[CH_MAIN].keyframes, g_prof[CH_SUB].keyframes,
		g_prof[CH_MAIN].bytes, g_prof[CH_SUB].bytes);
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
	int secs, want, ldr, auto_wanted;

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
		auto_wanted = g_auto_night || want == 2;

		if (want == 0 || want == 1) {
			if (want != g_mode)
				apply_mode(want, "control file");
		} else if (g_mode < 0 && g_force_night >= 0) {
			apply_mode(g_force_night, "--force-night");
		} else if (auto_wanted && ldr >= 0 && ldr != g_mode) {
			logmsg("day/night: ldr says %d%s\n", ldr,
			       want == 2 ? " (control file asked for auto)" : "");
			apply_mode(ldr, "light sensor");
		}

		write_status(want, ldr);

		/* Poll in 1 s slices so a stop is honoured promptly. */
		for (secs = 0; secs < 5 && g_run; secs++)
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

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--port 8554] [--fps 20] [--gop 20]\n"
		"         [--main 1280x720] [--sub 640x480]\n"
		"         [--main-kbps 1500] [--sub-kbps 600] [--verbose]\n"
		"         [--force-night day|night] [--auto-night] [--ir-follow] [--auto-fps]\n"
		"         [--mode-file /tmp/akrtsp.cmd] [--status-file /tmp/akrtsp.status]\n"
		"\n"
		"gop is clamped to at least fps: libakstreamenc dies with SIGFPE at the\n"
		"second keyframe when the GOP is shorter than a second.\n"
		"\n"
		"Day/night. Left in day mode under this camera's own IR illuminator the\n"
		"picture is magenta (mean R-G +76, B-G +79); night mode gives R=G=B across a\n"
		"full 0..255 range, which is what the vendor's own log calls\n"
		"\"switching isp mode -> night\". Nothing on this board decides that reliably:\n"
		"libplat_vi's auto path never switched by itself (90 s measured), the light\n"
		"sensor reports DAY in a dark IR-lit room, and the ISP's own statistics are\n"
		"exported by libakispsdk without any header declaring them. So the mode is\n"
		"asked for, not guessed: the control file (--mode-file) holds one word -\n"
		"day, night or auto - and wins over --force-night, which is only the default\n"
		"for when there is no file (this camera: night). --auto-night or \"auto\" in\n"
		"the file trusts the light sensor, --ir-follow also moves the mechanical\n"
		"filter (measured to blind the camera when the sensor lies), --auto-fps hands\n"
		"light adaptation to libplat_vi's thread, which changes the sensor fps that\n"
		"our fixed-step RTP timestamps do not yet follow.\n"
		"The status file is rewritten every 5 s for the web UI: current mode, what\n"
		"was asked for, the light sensor, fps, clients and frame counters.\n", argv0);
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
	int listen_fd, ch, i, c;
	struct sockaddr_in sa;
	static const struct option longopts[] = {
		{ "port", 1, 0, 'p' }, { "fps", 1, 0, 'F' }, { "gop", 1, 0, 'G' },
		{ "main", 1, 0, 'M' }, { "sub", 1, 0, 'S' },
		{ "main-kbps", 1, 0, 'b' }, { "sub-kbps", 1, 0, 'k' },
		{ "no-night", 0, 0, 'N' }, { "force-night", 1, 0, 'n' },
		{ "auto-night", 0, 0, 'A' }, { "ir-follow", 0, 0, 'f' },
		{ "auto-fps", 0, 0, 'a' },
		{ "mode-file", 1, 0, 'm' }, { "status-file", 1, 0, 's' },
		{ "verbose", 0, 0, 'v' }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 }
	};

	g_prof[CH_MAIN].name = "main";
	g_prof[CH_SUB].name = "sub";
	g_prof[CH_MAIN].w = 1280;
	g_prof[CH_MAIN].h = 720;
	g_prof[CH_SUB].w = 640;
	g_prof[CH_SUB].h = 480;

	while ((c = getopt_long(argc, argv, "p:F:G:M:S:b:k:n:aNAfm:s:vh", longopts, NULL)) != -1) {
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
		case 'v': g_verbose = 1; break;
		default: usage(argv[0]); return 2;
		}
	}
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
	for (ch = 0; ch < N_CH; ch++)
		if (g_prof[ch].stream)
			ak_venc_cancel_stream(g_prof[ch].stream);
	for (ch = 0; ch < N_CH; ch++)
		pthread_join(prod_th[ch], NULL);
	if (light_on)
		pthread_join(light_th, NULL);	/* polls in 1 s slices, so this waits at most a second */
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
	return 0;
}
