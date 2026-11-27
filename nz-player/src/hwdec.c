/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - H.264 decoding and scaling on the GPU of the Cardputer Zero.
 *
 * The Raspberry Pi firmware decodes H.264 (up to 1080p) and can scale and convert
 * pictures; the kernel driver bcm2835-codec offers both as V4L2 devices: the decoder
 * (/dev/video10, a "stateful decoder") and the scaler ("ISP", /dev/video12).  This file
 * drives them directly: FFmpeg's own wrapper for such decoders (h264_v4l2m2m) hangs with
 * this driver and asks for more buffer memory than the system has.
 *
 * A decoded picture stays in the decoder's memory.  The scaler gets it by reference (a
 * DMABUF) and shrinks it to its size on the screen; only that small picture is read by
 * the processor, which converts it to RGB565 with dithering (yuv565.c).  (The scaler can
 * produce RGB565 itself, but it cuts the colours down without dithering, and smooth areas
 * show bands.)  Measured on a Cardputer Zero with 1080p video at 24 pictures a
 * second: 9 % of one core, against 175 % for decoding in software.
 *
 * Nothing here waits for the hardware for more than a few seconds: every failure is
 * reported to the caller, which goes on with the software decoder.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include "nzp.h"

#define OUT_T V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE    /* V4L2 "output": what goes into a device */
#define CAP_T V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE   /* V4L2 "capture": what comes out of it */
#define MAXB 16
#define N_IN 4                               /* input (bitstream) buffers */
#define EXTRA_PIC 3                          /* picture buffers above the decoder's minimum */
#define TS_BIAS 1000000LL                    /* seconds added to time stamps so that they are never negative */

struct hbuf { void *p; size_t len; int queued; };

struct hwdec {
	int fd, ifd;                             /* decoder, scaler */
	AVBSFContext *bsf; AVPacket *pkt;
	AVRational tb; double frame_dur, last_ts;
	struct hbuf ob[MAXB], cb[MAXB], ib[MAXB]; int n_ob, n_cb, n_ib;
	struct hbuf xb[MAXB]; int n_xb, xb_free[MAXB], isp_copy; unsigned in_stride; double t_relay; long n_relay;   /* scaler input of its own, when the decoder's layout does not suit it */
	int cap_on, isp_on, dfd[MAXB];
	unsigned cap_w, cap_h, cap_stride, cap_size, vis_w, vis_h;
	int isp_w, isp_h, want_w, want_h; unsigned isp_stride, isp_rows;   /* isp_rows: lines of the luma plane, padding included */
	int bt709, full_range, isp_rgb; uint16_t *rgb; size_t rgb_len;
	double t_scaler, t_copy, t_convert; long n_scaled, n_skipped;
	uint8_t *lin; size_t lin_len;             /* ordinary-memory copy of the scaler's picture */   /* for the summary in the log */
	int held; unsigned held_bytes; struct timeval held_ts;   /* picture taken with hw_next(), not yet released */
	int inflight, fl_idx[2];                 /* pictures handed to the scaler and not collected yet (at most 2), and their decoder buffers */
	long isp_done;                           /* pictures the scaler has delivered in its present set-up */
	int draining, ended; long n_in, n_out;
	double first_in, stuck_since, fed_ts, out_ts, out_wall;   /* time stamps of the newest frame sent and the newest picture received */
};

static const char *dec_dev(void) { const char *e = getenv("NZP_HW_DECODER"); return e ? e : "/dev/video10"; }
static const char *isp_dev(void) { const char *e = getenv("NZP_HW_SCALER"); return e ? e : "/dev/video12"; }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define HLOG(...) do { fprintf(stderr, "nz-player: gpu: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define HDBG(...) do { if (getenv("NZP_DEBUG")) HLOG(__VA_ARGS__); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r;
}

static int req_bufs(int fd, int type, int count, struct hbuf *b, const char *what)
{
	struct v4l2_requestbuffers rb = { .count = count, .type = type, .memory = V4L2_MEMORY_MMAP };
	if (xioctl(fd, VIDIOC_REQBUFS, &rb)) { HLOG("no memory for %d %s buffers: %s", count, what, strerror(errno)); return -1; }
	if (rb.count > MAXB) rb.count = MAXB;
	for (unsigned i = 0; i < rb.count; i++) {
		struct v4l2_plane pl[2]; struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 2, .m.planes = pl };
		memset(pl, 0, sizeof(pl));
		b[i].p = MAP_FAILED; b[i].queued = 0;
		if (xioctl(fd, VIDIOC_QUERYBUF, &vb)) { HLOG("QUERYBUF %s: %s", what, strerror(errno)); return -1; }
		b[i].len = pl[0].length;
		b[i].p = mmap(NULL, b[i].len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
		if (b[i].p == MAP_FAILED) { HLOG("mapping a %s buffer: %s", what, strerror(errno)); return -1; }
	}
	return (int)rb.count;
}

static void free_bufs(int fd, int type, struct hbuf *b, int *n)
{
	struct v4l2_requestbuffers rb = { .count = 0, .type = type, .memory = V4L2_MEMORY_MMAP };
	for (int i = 0; i < *n; i++) if (b[i].p && b[i].p != MAP_FAILED) { munmap(b[i].p, b[i].len); b[i].p = NULL; }
	if (*n) xioctl(fd, VIDIOC_REQBUFS, &rb);
	*n = 0;
}

static int qbuf_pic(struct hwdec *h, int i)
{
	struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = pl };
	memset(pl, 0, sizeof(pl));
	if (xioctl(h->fd, VIDIOC_QBUF, &vb)) { HLOG("returning a picture buffer: %s", strerror(errno)); return -1; }
	h->cb[i].queued = 1;
	return 0;
}

/* ---------------- scaler ---------------- */
static void isp_teardown(struct hwdec *h)
{
	int t; struct v4l2_requestbuffers rb = { .count = 0, .type = OUT_T, .memory = V4L2_MEMORY_DMABUF };
	if (h->ifd < 0) return;
	if (h->isp_on) { t = OUT_T; xioctl(h->ifd, VIDIOC_STREAMOFF, &t); t = CAP_T; xioctl(h->ifd, VIDIOC_STREAMOFF, &t); h->isp_on = 0; }
	free_bufs(h->ifd, CAP_T, h->ib, &h->n_ib);
	if (h->n_xb) free_bufs(h->ifd, OUT_T, h->xb, &h->n_xb); else xioctl(h->ifd, VIDIOC_REQBUFS, &rb);
	for (int i = 0; i < MAXB; i++) if (h->dfd[i] >= 0) { close(h->dfd[i]); h->dfd[i] = -1; }
	close(h->ifd); h->ifd = -1; h->inflight = 0;
}

static int isp_good;                         /* the scaler set-up that worked last (see isp_setup) */

static int isp_try(struct hwdec *h, int w, int hgt, int variant)
{
	struct v4l2_format f; struct v4l2_selection sel; struct v4l2_requestbuffers rb; int t;
	if ((h->ifd = open(isp_dev(), O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) { HLOG("cannot open the scaler %s: %s", isp_dev(), strerror(errno)); return -1; }
	memset(&f, 0, sizeof(f)); f.type = OUT_T;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420; f.fmt.pix_mp.width = h->cap_w; f.fmt.pix_mp.height = h->cap_h; f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.plane_fmt[0].bytesperline = h->cap_stride; f.fmt.pix_mp.plane_fmt[0].sizeimage = h->cap_size; f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.colorspace = h->vis_h >= 720 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
	if (xioctl(h->ifd, VIDIOC_S_FMT, &f)) { HLOG("scaler input format: %s", strerror(errno)); return -1; }
	if (f.fmt.pix_mp.height != h->cap_h || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 || f.fmt.pix_mp.plane_fmt[0].bytesperline < h->cap_w) { HLOG("the scaler does not take the decoder's picture size"); return -1; }
	/* The driver (seen in kernel 6.18) gives the decoder's lines a multiple of 32 bytes and
	 * wants a multiple of 64 for the scaler's input.  For widths such as 720, 1440 or 848 the
	 * two differ and a decoded picture cannot be handed over by reference.  (Asking the
	 * decoder for the longer line was tried on the device, 0.3.2/0.3.3: it agrees, but the
	 * scaler then delivers nothing.)  Such pictures are copied, line by line, into input
	 * buffers of the scaler's own with its line length. */
	h->in_stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	h->isp_copy = h->in_stride != h->cap_stride;
	for (int k = 0; k < 2 && (h->vis_w != h->cap_w || h->vis_h != h->cap_h); k++) {   /* leave out the padding below the visible part */
		memset(&sel, 0, sizeof(sel)); sel.type = k ? OUT_T : V4L2_BUF_TYPE_VIDEO_OUTPUT; sel.target = V4L2_SEL_TGT_CROP; sel.r.width = h->vis_w; sel.r.height = h->vis_h;
		if (!xioctl(h->ifd, VIDIOC_S_SELECTION, &sel)) break;
	}
	/* Result: RGB with three bytes a pixel if the scaler offers it (every pixel gets its own
	 * colour), else YU12 (one colour value for each 2x2 pixels).  NZP_HW_YUV=1 forces YU12. */
	h->isp_rgb = 0;
	for (int k = variant == 2 ? 1 : 0; k < 2; k++) {
		unsigned want = k ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_RGB24;
		memset(&f, 0, sizeof(f)); f.type = CAP_T;
		f.fmt.pix_mp.pixelformat = want; f.fmt.pix_mp.width = w; f.fmt.pix_mp.height = hgt; f.fmt.pix_mp.num_planes = 1; f.fmt.pix_mp.field = V4L2_FIELD_NONE;
		/* RGB: the firmware counts a line as the width rounded up to 32 pixels, the driver
		 * rounds the bytes up to 32 and makes the buffer too small for the firmware ("buffer
		 * size mismatch" in the kernel log); the scaler then does not start.  Asking for the
		 * firmware's line fixes it.  Widths that are a multiple of 32 (320, "Fill") never
		 * showed it, which is why 0.2.7 worked on the device and 0.3.0 with "Fit" (302) did not. */
		if (!k) f.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned)((w + 31) & ~31) * 3;
		f.fmt.pix_mp.colorspace = variant == 1 ? V4L2_COLORSPACE_DEFAULT : h->vis_h >= 720 ? V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;
		if (!xioctl(h->ifd, VIDIOC_S_FMT, &f) && f.fmt.pix_mp.pixelformat == want && (k || f.fmt.pix_mp.plane_fmt[0].bytesperline >= 3 * f.fmt.pix_mp.width)) { h->isp_rgb = !k; break; }
		if (k) { HLOG("scaler result format %dx%d: %s", w, hgt, strerror(errno)); return -1; }
	}
	h->isp_w = f.fmt.pix_mp.width; h->isp_h = f.fmt.pix_mp.height; h->isp_stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (h->isp_stride < (unsigned)h->isp_w || !h->isp_stride || h->isp_w < 2 || h->isp_h < 2) { HLOG("scaler: unexpected result %dx%d, line %u bytes", h->isp_w, h->isp_h, h->isp_stride); return -1; }
	h->want_w = w; h->want_h = hgt;

	if (h->isp_copy) {
		if ((h->n_xb = req_bufs(h->ifd, OUT_T, 2, h->xb, "scaler input")) < 2) { if (h->n_xb < 0) h->n_xb = 0; return -1; }
		if (h->xb[0].len < (size_t)h->in_stride * h->cap_h * 3 / 2) { HLOG("scaler: an input buffer of %zu bytes is too small", h->xb[0].len); return -1; }
		for (int i = 0; i < h->n_xb; i++) h->xb_free[i] = 1;
	} else {
		memset(&rb, 0, sizeof(rb)); rb.count = h->n_cb; rb.type = OUT_T; rb.memory = V4L2_MEMORY_DMABUF;
		if (xioctl(h->ifd, VIDIOC_REQBUFS, &rb)) { HLOG("the scaler does not take pictures by reference: %s", strerror(errno)); return -1; }
	}
	if ((h->n_ib = req_bufs(h->ifd, CAP_T, 2, h->ib, "scaler result")) <= 0) { h->n_ib = 0; return -1; }
	/* V4L2's layout for YU12 in one buffer: the brightness plane (line length x height as
	 * reported), then the two colour planes at half the line length and half the height. */
	h->isp_rows = (unsigned)h->isp_h;
	if (h->ib[0].len < (h->isp_rgb ? (size_t)h->isp_stride * h->isp_rows : (size_t)h->isp_stride * h->isp_rows * 3 / 2)) { HLOG("scaler: a buffer of %zu bytes is too small for %dx%d", h->ib[0].len, h->isp_w, h->isp_h); return -1; }
	for (int i = 0; i < h->n_ib; i++) {
		struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = i, .length = 1, .m.planes = pl };
		memset(pl, 0, sizeof(pl));
		if (xioctl(h->ifd, VIDIOC_QBUF, &vb)) { HLOG("scaler result buffer: %s", strerror(errno)); return -1; }
	}
	for (int i = 0; i < h->n_cb && !h->isp_copy; i++) {
		struct v4l2_exportbuffer eb; memset(&eb, 0, sizeof(eb)); eb.type = CAP_T; eb.index = i; eb.flags = O_CLOEXEC | O_RDWR;
		if (xioctl(h->fd, VIDIOC_EXPBUF, &eb)) { HLOG("sharing a picture buffer: %s", strerror(errno)); return -1; }
		h->dfd[i] = eb.fd;
	}
	t = OUT_T; if (xioctl(h->ifd, VIDIOC_STREAMON, &t)) { HLOG("starting the scaler (its input, set-up %d): %s", variant, strerror(errno)); return -1; }
	t = CAP_T; if (xioctl(h->ifd, VIDIOC_STREAMON, &t)) { HLOG("starting the scaler (its result, set-up %d): %s", variant, strerror(errno)); return -1; }
	h->isp_on = 1; h->inflight = 0;
	HLOG("scaler: %ux%u -> %dx%d %s, set-up %d%s (asked %dx%d; line %u bytes, %u lines, buffer %zu bytes)", h->vis_w, h->vis_h, h->isp_w, h->isp_h, h->isp_rgb ? "RGB" : "YU12", variant, h->isp_copy ? ", pictures copied to the scaler" : "", w, hgt, h->isp_stride, h->isp_rows, h->ib[0].len);
	return 0;
}

/* The scaler is set up in the way that worked last; if the firmware refuses it, the other
 * ways are tried in turn:
 *   0  result RGB, colour space named like the video's   (worked on NimbleZero v0.1.0)
 *   1  result RGB, colour space left to the driver
 *   2  result YU12 (one colour value for each 2x2 pixels), converted by the player
 * NZP_HW_YUV=1 allows only the last. */
static int isp_setup(struct hwdec *h, int w, int hgt)
{
	for (int i = 0; i < 3; i++) {
		int v = (isp_good + i) % 3;
		if (getenv("NZP_HW_YUV") && v != 2) continue;
		if (h->ifd >= 0) isp_teardown(h);
		if (!isp_try(h, w, hgt, v)) { isp_good = v; h->isp_done = 0; return 0; }
	}
	return -1;
}

/* ---------------- decoder ---------------- */

/* How much memory the GPU has (the gpu_mem line in config.txt), in MB; 0 if it cannot be
 * told.  The firmware gives the kernel the rest of the board's memory and says so in the
 * device tree; what is missing up to the next power of two is the GPU's.  (512 MB board:
 * 480 for the processor with gpu_mem=32, 448 with 64.)
 * Seen on the device: with 32 the firmware cannot create the video decoder ("failed to
 * create component ril.video_decode" in the kernel log), or creates it and then never
 * answers; with 64, 1080p at 60 pictures a second plays. */
int hw_gpu_mem_mb(void)
{
	static int known = -1;
	unsigned char b[32]; uint32_t ac = 1, sc = 1; FILE *f; size_t n; unsigned long long arm = 0, top = 1;
	if (known >= 0) return known;
	known = 0;
	if (getenv("NZP_GPU_MB")) return known = atoi(getenv("NZP_GPU_MB"));   /* for tests */
	if ((f = fopen("/proc/device-tree/#address-cells", "rb"))) { if (fread(b, 1, 4, f) == 4) ac = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; fclose(f); }
	if ((f = fopen("/proc/device-tree/#size-cells", "rb"))) { if (fread(b, 1, 4, f) == 4) sc = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; fclose(f); }
	if (ac < 1 || ac > 2 || sc < 1 || sc > 2) return 0;
	if (!(f = fopen("/proc/device-tree/memory@0/reg", "rb"))) return 0;
	n = fread(b, 1, sizeof(b), f); fclose(f);
	if (n < 4 * (ac + sc)) return 0;
	for (uint32_t i = 0; i < 4 * sc; i++) arm = arm << 8 | b[4 * ac + i];   /* the size of the first (on these boards: only) block */
	arm >>= 20;
	if (arm < 64 || arm > 16384) return 0;
	while (top < arm) top <<= 1;
	return known = (int)(top - arm);
}

/* (Re)build the picture side for the format the decoder reports. */
static int setup_pictures(struct hwdec *h)
{
	struct v4l2_format f = { .type = CAP_T }; struct v4l2_selection sel; struct v4l2_control c = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
	int type = CAP_T, min;
	isp_teardown(h);                         /* it holds references to the picture buffers */
	if (h->cap_on) { xioctl(h->fd, VIDIOC_STREAMOFF, &type); h->cap_on = 0; }
	free_bufs(h->fd, CAP_T, h->cb, &h->n_cb);
	h->held = -1;
	if (xioctl(h->fd, VIDIOC_G_FMT, &f)) { HLOG("picture format: %s", strerror(errno)); return -1; }
	if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420) {
		f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
		xioctl(h->fd, VIDIOC_S_FMT, &f);
		if (xioctl(h->fd, VIDIOC_G_FMT, &f) || f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420) { HLOG("the decoder gives no YU12 pictures"); return -1; }
	}
	h->cap_w = f.fmt.pix_mp.width; h->cap_h = f.fmt.pix_mp.height; h->cap_stride = f.fmt.pix_mp.plane_fmt[0].bytesperline; h->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	h->vis_w = h->cap_w; h->vis_h = h->cap_h;
	for (int t = 0; t < 2; t++) {            /* the visible part (1920x1080 inside 1920x1088, for example) */
		memset(&sel, 0, sizeof(sel)); sel.type = t ? CAP_T : V4L2_BUF_TYPE_VIDEO_CAPTURE; sel.target = V4L2_SEL_TGT_COMPOSE;
		if (!xioctl(h->fd, VIDIOC_G_SELECTION, &sel) && sel.r.width && sel.r.height) { h->vis_w = sel.r.width; h->vis_h = sel.r.height; break; }
	}
	min = xioctl(h->fd, VIDIOC_G_CTRL, &c) || c.value <= 0 ? 4 : c.value;
	if ((h->n_cb = req_bufs(h->fd, CAP_T, min + EXTRA_PIC, h->cb, "picture")) <= 0) { h->n_cb = 0; return -1; }
	for (int i = 0; i < h->n_cb; i++) if (qbuf_pic(h, i)) return -1;
	if (xioctl(h->fd, VIDIOC_STREAMON, &type)) { HLOG("starting the picture side: %s", strerror(errno)); return -1; }
	h->cap_on = 1;
	HLOG("decoding %ux%u (visible %ux%u, line %u bytes), %d picture buffers of %zu KB", h->cap_w, h->cap_h, h->vis_w, h->vis_h, h->cap_stride, h->n_cb, h->cb[0].len / 1024);
	return 0;
}

int hw_usable(const AVCodecParameters *par)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(par->format);
	if (!cfg.hwdec || getenv("NZP_NO_HW")) return 0;
	if (par->codec_id != AV_CODEC_ID_H264 || par->width < 32 || par->height < 32 || par->width > 1920 || par->height > 1088) return 0;
	if (d && (d->comp[0].depth != 8 || d->log2_chroma_w != 1 || d->log2_chroma_h != 1 || d->nb_components < 3)) return 0;   /* 8 bit 4:2:0 only */
	return !access(dec_dev(), R_OK | W_OK) && !access(isp_dev(), R_OK | W_OK);
}

void hw_close(struct hwdec *h)
{
	int t;
	if (!h) return;
	if (h->fd >= 0) {
		isp_teardown(h);
		t = OUT_T; xioctl(h->fd, VIDIOC_STREAMOFF, &t);
		if (h->cap_on) { t = CAP_T; xioctl(h->fd, VIDIOC_STREAMOFF, &t); }
		free_bufs(h->fd, OUT_T, h->ob, &h->n_ob); free_bufs(h->fd, CAP_T, h->cb, &h->n_cb);
		close(h->fd);
	}
	if (h->n_relay) HLOG("%ld pictures copied to the scaler, %.1f ms each on average", h->n_relay, h->t_relay / h->n_relay * 1000);
	if (h->n_scaled) HLOG("%ld pictures shown from the GPU (waiting for the scaler %.1f ms, copy %.1f ms, conversion %.1f ms each on average), %ld decoded but not scaled", h->n_scaled, h->t_scaler / h->n_scaled * 1000, h->t_copy / h->n_scaled * 1000, h->t_convert / h->n_scaled * 1000, h->n_skipped);
	av_bsf_free(&h->bsf); av_packet_free(&h->pkt);
	av_free(h->rgb); av_free(h->lin);
	free(h);
}

struct hwdec *hw_open(const AVStream *st)
{
	struct hwdec *h = calloc(1, sizeof(*h)); const AVBitStreamFilter *f; struct v4l2_capability cap; struct v4l2_format fmt; struct v4l2_event_subscription sub; int t;
	if (!h) return NULL;
	h->fd = h->ifd = -1; h->held = -1;
	isp_good = 0;                            /* every video starts with the best scaler set-up again */
	for (int i = 0; i < MAXB; i++) h->dfd[i] = -1;
	h->tb = st->time_base;
	h->bt709 = st->codecpar->color_space != AVCOL_SPC_UNSPECIFIED ? st->codecpar->color_space == AVCOL_SPC_BT709 : st->codecpar->height >= 720;
	h->full_range = st->codecpar->color_range == AVCOL_RANGE_JPEG;
	h->frame_dur = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? av_q2d(av_inv_q(st->avg_frame_rate)) : 0.04;
	/* MP4 and MKV keep H.264 without start codes; the decoder wants them (harmless for the rest) */
	if (!(f = av_bsf_get_by_name("h264_mp4toannexb")) || av_bsf_alloc(f, &h->bsf) < 0 || avcodec_parameters_copy(h->bsf->par_in, st->codecpar) < 0) goto bad;
	h->bsf->time_base_in = st->time_base;
	if (av_bsf_init(h->bsf) < 0 || !(h->pkt = av_packet_alloc())) goto bad;

	if ((h->fd = open(dec_dev(), O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) { HLOG("cannot open %s: %s", dec_dev(), strerror(errno)); goto bad; }
	memset(&cap, 0, sizeof(cap));
	if (xioctl(h->fd, VIDIOC_QUERYCAP, &cap) || strcmp((const char *)cap.driver, "bcm2835-codec")) { HLOG("%s is not the Raspberry Pi decoder", dec_dev()); goto bad; }
	memset(&fmt, 0, sizeof(fmt)); fmt.type = OUT_T;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264; fmt.fmt.pix_mp.width = st->codecpar->width; fmt.fmt.pix_mp.height = st->codecpar->height;
	fmt.fmt.pix_mp.num_planes = 1; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = st->codecpar->width > 1280 ? 2 << 20 : 1 << 20;   /* room for the largest single frame */
	if (xioctl(h->fd, VIDIOC_S_FMT, &fmt)) { HLOG("H.264 input: %s", strerror(errno)); goto bad; }
	memset(&sub, 0, sizeof(sub)); sub.type = V4L2_EVENT_SOURCE_CHANGE; xioctl(h->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	memset(&sub, 0, sizeof(sub)); sub.type = V4L2_EVENT_EOS; xioctl(h->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
	if ((h->n_ob = req_bufs(h->fd, OUT_T, N_IN, h->ob, "input")) <= 0) { h->n_ob = 0; goto bad; }
	t = OUT_T;
	if (xioctl(h->fd, VIDIOC_STREAMON, &t)) { HLOG("starting the decoder: %s", strerror(errno)); goto bad; }
	return h;
bad:
	hw_close(h);
	return NULL;
}

int hw_can_send(struct hwdec *h)
{
	if (h->draining) return 0;
	for (int i = 0; i < h->n_ob; i++) if (!h->ob[i].queued) return 1;
	return 0;
}

static int queue_bitstream(struct hwdec *h, const AVPacket *p)
{
	struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = pl, .field = V4L2_FIELD_NONE };
	double ts; long long us; int i;
	for (i = 0; i < h->n_ob && h->ob[i].queued; i++) ;
	if (i == h->n_ob) return 1;              /* no room (should not happen: see hw_can_send) */
	if ((size_t)p->size > h->ob[i].len) { HLOG("a frame of %d KB is larger than the input buffer; left out", p->size / 1024); return 0; }
	ts = p->pts != AV_NOPTS_VALUE ? p->pts * av_q2d(h->tb) : p->dts != AV_NOPTS_VALUE ? p->dts * av_q2d(h->tb) : h->last_ts + h->frame_dur;
	h->last_ts = h->fed_ts = ts;
	us = (long long)((ts + TS_BIAS) * 1e6 + 0.5);
	memset(pl, 0, sizeof(pl));
	memcpy(h->ob[i].p, p->data, p->size);
	pl[0].bytesused = p->size; pl[0].length = h->ob[i].len; vb.index = i;
	vb.timestamp.tv_sec = us / 1000000; vb.timestamp.tv_usec = us % 1000000;
	if (xioctl(h->fd, VIDIOC_QBUF, &vb)) { HLOG("sending a frame to the decoder: %s", strerror(errno)); return -1; }
	h->ob[i].queued = 1;
	if (!h->n_in++) h->first_in = now_s();
	return 0;
}

/* One packet of the stream; NULL = the stream has ended.  0 ok, -1 the hardware failed. */
int hw_send(struct hwdec *h, const AVPacket *pkt)
{
	AVPacket *c; int r;
	if (!pkt) {
		struct v4l2_decoder_cmd dc; memset(&dc, 0, sizeof(dc)); dc.cmd = V4L2_DEC_CMD_STOP;
		if (h->draining) return 0;
		h->draining = 1;
		if (!h->cap_on || xioctl(h->fd, VIDIOC_DECODER_CMD, &dc)) h->ended = 1;   /* nothing to wait for */
		return 0;
	}
	if (!(c = av_packet_clone(pkt))) return 0;
	if (av_bsf_send_packet(h->bsf, c) < 0) { av_packet_free(&c); return 0; }
	av_packet_free(&c);
	while (av_bsf_receive_packet(h->bsf, h->pkt) == 0) {
		r = queue_bitstream(h, h->pkt);
		av_packet_unref(h->pkt);
		if (r < 0) return -1;
	}
	return 0;
}

/* Next decoded picture.  1: one is ready, *pts set; call hw_scale() or hw_skip() for it.
 * 0: none at the moment.  2: the stream has ended.  -1: the hardware failed. */
int hw_next(struct hwdec *h, double *pts)
{
	struct v4l2_event ev; int progress = 0;
	if (h->held >= 0) return -1;             /* the previous one was not released */
	while (!xioctl(h->fd, VIDIOC_DQEVENT, &ev)) {
		progress = 1;
		if (ev.type == V4L2_EVENT_SOURCE_CHANGE && setup_pictures(h)) return -1;
	}
	for (;;) {                               /* used input buffers back */
		struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = OUT_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = pl };
		memset(pl, 0, sizeof(pl));
		if (xioctl(h->fd, VIDIOC_DQBUF, &vb)) break;
		if (vb.index < (unsigned)h->n_ob) h->ob[vb.index].queued = 0;
		progress = 1;
	}
	if (!h->n_out && h->n_in && now_s() - h->first_in > 12.0) {
		HLOG("no picture 12 s after the first frame was sent (%ld frames in)", h->n_in);
		return -1;
	}
	while (h->cap_on) {
		struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = pl };
		memset(pl, 0, sizeof(pl));
		if (xioctl(h->fd, VIDIOC_DQBUF, &vb)) {
			if (errno == EPIPE) { if (h->draining) h->ended = 1; else if (setup_pictures(h)) return -1; }
			else if (errno != EAGAIN) { HLOG("taking a picture: %s", strerror(errno)); return -1; }
			break;
		}
		progress = 1;
		if (vb.index >= (unsigned)h->n_cb) continue;
		h->cb[vb.index].queued = 0;
		if (vb.flags & V4L2_BUF_FLAG_LAST) {
			if (h->draining) h->ended = 1;       /* a picture that comes with the flag is still delivered below */
			else if (!pl[0].bytesused) { if (setup_pictures(h)) return -1; break; }
		}
		if (!pl[0].bytesused || (vb.flags & V4L2_BUF_FLAG_ERROR)) { if (!h->ended && qbuf_pic(h, (int)vb.index)) return -1; if (h->ended) break; continue; }
		h->held = (int)vb.index; h->held_bytes = pl[0].bytesused; h->held_ts = vb.timestamp;
		*pts = h->out_ts = vb.timestamp.tv_sec + vb.timestamp.tv_usec / 1e6 - TS_BIAS;
		h->n_out++; h->stuck_since = 0; h->out_wall = now_s();
		return 1;
	}
	if (h->ended) return 2;
	/* The decoder holds every input buffer and gives nothing back: it is stuck. */
	if (progress || hw_can_send(h)) h->stuck_since = 0;
	else if (h->stuck_since == 0) h->stuck_since = now_s();
	else if (now_s() - h->stuck_since > 3.0) { HLOG("the decoder stopped answering (%ld frames in, %ld pictures out)", h->n_in, h->n_out); return -1; }
	return 0;
}

/* Should the caller send more now?  The frames sent are kept at most 0.3 s of video
 * ahead of the pictures received, so that the stream that is waiting waits in the packet
 * queue (where the buffering logic sees it) and not inside the decoder.  Exceptions: until
 * the first picture (a decoder that starts in the middle of a stream swallows everything
 * up to the next key frame), and when nothing has come out for half a second (a stream
 * with very few pictures a second: the decoder needs more to go on).
 * Counting frames instead does not work: frames the decoder drops, or streams with two
 * packets per picture, make the count drift until the decoder is starved. */
int hw_wants_more(struct hwdec *h)
{
	if (!h->n_out || h->fed_ts - h->out_ts < 0.3) return 1;
	return now_s() - h->out_wall > 0.5;
}
int hw_ended(struct hwdec *h) { return h->ended; }

void hw_skip(struct hwdec *h)
{
	if (h->held >= 0) { if (!h->ended) qbuf_pic(h, h->held); h->held = -1; h->n_skipped++; }
}

/* Size of the visible picture as the decoder reports it (valid after the first hw_next() == 1). */
void hw_size(struct hwdec *h, int *w, int *hgt) { *w = (int)h->vis_w; *hgt = (int)h->vis_h; }

/* Scaling is split in two so that the processor's part (copy and colour conversion, about
 * 3 ms) of one picture runs while the scaler hardware (about 11 ms at 1080p) already works
 * on the next: at 60 pictures a second there are only 16.7 ms for each, and one after the
 * other the two left almost no reserve.
 *
 * hw_scale_begin(): hand the picture from hw_next() to the scaler, for about w x hgt (even
 * numbers).  Does not wait.  0 ok; 1 = collect with hw_scale_end() first (two are under way,
 * or the size changes); -1 the hardware failed. */
int hw_scale_begin(struct hwdec *h, int w, int hgt)
{
	struct v4l2_plane pl[1]; struct v4l2_buffer vb = { .type = OUT_T, .memory = V4L2_MEMORY_DMABUF, .length = 1, .m.planes = pl, .field = V4L2_FIELD_NONE };
	int idx = h->held;
	if (idx < 0) return -1;
	w &= ~1; hgt &= ~1;
	if (w < 32) w = 32;
	if (hgt < 32) hgt = 32;
	if (h->inflight && (h->inflight >= 2 || w != h->want_w || hgt != h->want_h)) return 1;
	if (h->isp_on && (w != h->want_w || hgt != h->want_h)) isp_teardown(h);
	if (!h->isp_on) { if (h->ifd >= 0) isp_teardown(h); if (isp_setup(h, w, hgt)) { isp_teardown(h); return -1; } }
	memset(pl, 0, sizeof(pl));
	if (h->isp_copy) {
		/* YU12 in one buffer: brightness lines, then the two colour planes at half the line
		 * length and half the height; the same in both buffers, only the line length differs */
		int k = -1; double t0 = now_s();
		const uint8_t *sp = h->cb[idx].p; uint8_t *dp; unsigned ss = h->cap_stride, ds = h->in_stride, rows = h->cap_h, n = ss < ds ? ss : ds;
		for (int i = 0; i < h->n_xb && k < 0; i++) if (h->xb_free[i]) k = i;
		if (k < 0) return 1;
		if (h->cb[idx].len < (size_t)ss * rows * 3 / 2) { HLOG("a decoded picture is smaller than expected"); return -1; }
		dp = h->xb[k].p;
		for (unsigned y = 0; y < rows; y++) memcpy(dp + (size_t)y * ds, sp + (size_t)y * ss, n);
		sp += (size_t)ss * rows; dp += (size_t)ds * rows;
		for (unsigned y = 0; y < rows; y++) memcpy(dp + (size_t)y * (ds / 2), sp + (size_t)y * (ss / 2), n / 2);   /* both colour planes, one after the other */
		h->t_relay += now_s() - t0; h->n_relay++;
		vb.memory = V4L2_MEMORY_MMAP; vb.index = k; pl[0].bytesused = ds * rows * 3 / 2; vb.timestamp = h->held_ts;
		if (xioctl(h->ifd, VIDIOC_QBUF, &vb)) { HLOG("handing a picture to the scaler: %s", strerror(errno)); return -1; }
		h->xb_free[k] = 0;
		if (!h->ended && qbuf_pic(h, idx)) return -1;   /* the decoder has its buffer back at once */
		idx = -1;
	} else {
		pl[0].m.fd = h->dfd[idx]; pl[0].bytesused = h->held_bytes; pl[0].length = h->cb[idx].len;
		vb.index = idx; vb.timestamp = h->held_ts;
		if (xioctl(h->ifd, VIDIOC_QBUF, &vb)) { HLOG("handing a picture to the scaler: %s", strerror(errno)); return -1; }
	}
	h->fl_idx[h->inflight] = idx;
	h->held = -1; h->inflight++;
	return 0;
}

/* Collect the oldest picture under way, as RGB565; *px is valid until the next call.
 * 0 ok; 1 = there is none (also after a seek or a change of picture size, which discard
 * what was under way); -1 the hardware failed. */
int hw_scale_end(struct hwdec *h, const uint8_t **px, int *stride, int *got_w, int *got_h)
{
	const uint8_t *res = NULL; int back = 0, ridx = -1; double t1 = now_s();
	if (!h->inflight || !h->isp_on) return 1;
	while ((!res || !back) && now_s() - t1 < 2.0) {
		struct pollfd p = { .fd = h->ifd, .events = POLLIN | POLLOUT };
		if (!res) {
			struct v4l2_plane p2[1]; struct v4l2_buffer v2 = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = p2 };
			memset(p2, 0, sizeof(p2));
			if (!xioctl(h->ifd, VIDIOC_DQBUF, &v2)) {
				if (v2.index >= (unsigned)h->n_ib) break;
				ridx = (int)v2.index;
				if (!p2[0].bytesused || (v2.flags & V4L2_BUF_FLAG_ERROR)) break;
				res = (const uint8_t *)h->ib[v2.index].p + p2[0].data_offset;
			} else if (errno != EAGAIN) break;
		}
		if (!back) {
			struct v4l2_plane p2[1]; struct v4l2_buffer v2 = { .type = OUT_T, .memory = h->isp_copy ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF, .length = 1, .m.planes = p2 };
			memset(p2, 0, sizeof(p2));
			if (!xioctl(h->ifd, VIDIOC_DQBUF, &v2)) {   /* the decoder's buffer (or the scaler's own input buffer) is free again */
				back = 1;
				if (h->isp_copy) { if (v2.index < (unsigned)h->n_xb) h->xb_free[v2.index] = 1; }
				else if (v2.index < (unsigned)h->n_cb && !h->ended && qbuf_pic(h, (int)v2.index)) return -1;
			} else if (errno != EAGAIN) break;
		}
		if (!res || !back) poll(&p, 1, 5);
	}
	if (!res || !back) {
		HLOG("the scaler gave no picture (result %s, its input %s, set-up %d, after %ld good ones)", res ? "came" : ridx >= 0 ? "came empty or marked bad" : "did not come", back ? "came back" : "did not come back", isp_good, h->isp_done);
		/* The very first picture of a set-up: this set-up does not work with this video on
		 * this kernel.  Take the pictures back, and let the next one try the YU12 set-up
		 * (the last in the list); only if that fails too is the GPU path given up. */
		if (!h->isp_done && isp_good != 2) {
			int n = h->inflight, idx[2] = { h->fl_idx[0], h->fl_idx[1] };
			isp_teardown(h); isp_good = 2;
			for (int i = 0; i < n; i++) if (idx[i] >= 0 && !(back && i == 0) && !h->ended && qbuf_pic(h, idx[i])) return -1;
			return 1;
		}
		return -1;
	}
	h->inflight--; h->fl_idx[0] = h->fl_idx[1]; h->isp_done++;
	{   /* the scaler's small YU12 picture -> RGB565, dithered, with the colour rules of the stream.
	     * The scaler's buffer is memory the processor does not cache: reading it byte by byte,
	     * as the conversion does, costs about 100 ns a byte (measured: 19 ms for 320x170).  One
	     * memcpy() in large steps into ordinary memory first is far cheaper. */
		size_t ysz = (size_t)h->isp_stride * h->isp_rows, csz = h->isp_rgb ? 0 : (size_t)(h->isp_stride / 2) * ((h->isp_rows + 1) / 2), src = ysz + 2 * csz;
		size_t need = (size_t)h->isp_w * h->isp_h * 2; double t2 = now_s(), t3; const uint8_t *up, *vp;
		struct v4l2_plane p2[1]; struct v4l2_buffer v2 = { .type = CAP_T, .memory = V4L2_MEMORY_MMAP, .index = ridx, .length = 1, .m.planes = p2 };
		if (src > h->ib[ridx].len) { HLOG("the scaler's picture is smaller than expected"); return -1; }
		if (src > h->lin_len) { uint8_t *n = av_realloc(h->lin, src); if (!n) return -1; h->lin = n; h->lin_len = src; }
		if (need > h->rgb_len) { uint16_t *n = av_realloc(h->rgb, need); if (!n) return -1; h->rgb = n; h->rgb_len = need; }
		memcpy(h->lin, res, src);
		memset(p2, 0, sizeof(p2));
		xioctl(h->ifd, VIDIOC_QBUF, &v2);        /* the result buffer goes straight back: the scaler may need it for the next */
		t3 = now_s();
		up = h->lin + ysz; vp = up + csz;
		if (h->isp_rgb) rgb24_to_rgb565(h->rgb, h->isp_w, h->lin, (int)h->isp_stride, h->isp_w, h->isp_h);
		else yuv420_to_rgb565(h->rgb, h->isp_w, h->lin, (int)h->isp_stride, up, vp, (int)h->isp_stride / 2, h->isp_w, h->isp_h, h->bt709, h->full_range);
		*px = (const uint8_t *)h->rgb; *stride = h->isp_w * 2; *got_w = h->isp_w; *got_h = h->isp_h;
		h->t_scaler += t2 - t1; h->t_copy += t3 - t2; h->t_convert += now_s() - t3; h->n_scaled++;
	}
	return 0;
}

/* After a seek: throw away what is inside the decoder and start again. */
int hw_flush(struct hwdec *h)
{
	int t;
	av_bsf_flush(h->bsf);
	if (h->inflight) isp_teardown(h);        /* it holds pictures of the decoder */
	h->held = -1; h->draining = h->ended = 0; h->stuck_since = 0;
	t = OUT_T; if (xioctl(h->fd, VIDIOC_STREAMOFF, &t)) { HLOG("emptying the decoder: %s", strerror(errno)); return -1; }
	for (int i = 0; i < h->n_ob; i++) h->ob[i].queued = 0;
	if (h->cap_on) {
		t = CAP_T; if (xioctl(h->fd, VIDIOC_STREAMOFF, &t)) { HLOG("emptying the decoder: %s", strerror(errno)); return -1; }
		for (int i = 0; i < h->n_cb; i++) h->cb[i].queued = 0;
	}
	t = OUT_T; if (xioctl(h->fd, VIDIOC_STREAMON, &t)) { HLOG("restarting the decoder: %s", strerror(errno)); return -1; }
	if (h->cap_on) {
		for (int i = 0; i < h->n_cb; i++) if (qbuf_pic(h, i)) return -1;
		t = CAP_T; if (xioctl(h->fd, VIDIOC_STREAMON, &t)) { HLOG("restarting the decoder: %s", strerror(errno)); return -1; }
	}
	h->first_in = now_s(); h->n_in = h->n_out = 0;
	return 0;
}

/* Sleep until the decoder has something for us, at most ms. */
void hw_wait(struct hwdec *h, int ms)
{
	struct pollfd p = { .fd = h->fd, .events = POLLIN | POLLPRI | (hw_can_send(h) ? 0 : POLLOUT) };
	if (poll(&p, 1, ms) > 0 && (p.revents & POLLERR) && !(p.revents & (POLLIN | POLLOUT | POLLPRI))) usleep((useconds_t)(ms > 5 ? 5 : ms) * 1000);
}
