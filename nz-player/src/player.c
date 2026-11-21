/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - playback engine on the FFmpeg libraries.
 *
 * Threads:
 *   reader   opens the input, reads packets into two queues (sound, picture), seeks
 *   sound    decodes, converts to 16 bit stereo, writes to the output; its progress is
 *            the clock everything else follows
 *   picture  decodes, scales each frame to the screen (RGB565), queues finished pictures
 *   caller   (the user interface) takes the picture that is due with pl_refresh()
 *
 * One mutex and one condition variable guard all shared state.  Every packet and picture
 * carries the "serial" of the seek it belongs to; a seek raises the serial and everything
 * older is thrown away wherever it is met.
 *
 * Network streams are collected for cfg.buffer_ms before they start and again whenever
 * the queues run dry ("buffering"); local files start at once.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "nzp.h"

#define PQ_N        4                        /* finished pictures waiting for their time */
#define MAX_BYTES   (32 << 20)               /* packets held at most */
#define LATE_S      0.06                     /* a frame later than this is not worth converting */
#define MAX_AUDIO   16

struct node { AVPacket *pkt; int serial; double dur; struct node *next; };
struct pktq { struct node *first, *last; int n; long long bytes; double dur; };
struct pic { uint16_t px[SCR_W * SCR_H]; double pts; int serial; };

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int cv_ready;

static struct {
	char url[2048], ua[200], ref[512], err[160];
	double start_pos;
	AVFormatContext *ic;
	int abort, opened, failed, net, live, seekable;
	double duration, start_time;
	int v_idx, a_idx, a_gen, n_audio, audio_ids[MAX_AUDIO];
	struct pktq aq, vq;
	int serial, eof, a_done, v_done;
	int seek_req, seek_gen; double seek_target, pos_hint;
	double skip_until; int skip_pending;   /* after a seek: decode but do not play up to the exact place */
	int buffering, user_paused;
	double clk_pts, clk_wall, nan_since; int clk_run;
	struct pic *pq; int pq_r, pq_n, shown_serial;
	pthread_t th_read, th_audio, th_video; int has_read, has_audio, has_video;
	int level;                               /* decoding short cuts in use, 0..4 */
	unsigned long shown, dropped, dropped_disp, decoded, win_dec, win_late; double win_t0, win_late_sum;
	char vcodec[24], acodec[24], alang[8], sink[8]; int vw, vh, arate, ach; double fps;
} P;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void wait_ms(int ms)                  /* mutex held */
{
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_nsec += (long)ms * 1000000L; t.tv_sec += t.tv_nsec / 1000000000L; t.tv_nsec %= 1000000000L;
	pthread_cond_timedwait(&cv, &mx, &t);
}

/* ---------------- clock (mutex held) ---------------- */
static double clk_get(void)
{
	if (isnan(P.clk_pts)) return NAN;
	return P.clk_run ? P.clk_pts + (now_s() - P.clk_wall) : P.clk_pts;
}
static void clk_set(double pts) { P.clk_pts = pts; P.clk_wall = now_s(); }
static void clk_update_run(void)
{
	int run = !P.buffering && !P.user_paused;
	if (run == P.clk_run) return;
	if (!isnan(P.clk_pts)) clk_set(clk_get());
	P.clk_run = run;
	pthread_cond_broadcast(&cv);
}

/* ---------------- packet queues (mutex held) ---------------- */
static void q_put(struct pktq *q, AVPacket *pkt, double dur)
{
	struct node *n = av_mallocz(sizeof(*n));
	if (!n) { av_packet_free(&pkt); return; }
	n->pkt = pkt; n->serial = P.serial; n->dur = dur;
	if (q->last) q->last->next = n; else q->first = n;
	q->last = n; q->n++; q->dur += dur;
	if (pkt) q->bytes += pkt->size + (long long)sizeof(*n);
}
static int q_get(struct pktq *q, AVPacket **pkt, int *serial)
{
	struct node *n = q->first;
	if (!n) return 0;
	q->first = n->next; if (!q->first) q->last = NULL;
	q->n--; q->dur -= n->dur; if (q->n == 0) q->dur = 0;
	if (n->pkt) q->bytes -= n->pkt->size + (long long)sizeof(*n);
	*pkt = n->pkt; *serial = n->serial; av_free(n);
	return 1;
}
static void q_flush(struct pktq *q)
{
	AVPacket *p; int s;
	while (q_get(q, &p, &s)) av_packet_free(&p);
}

/* seconds of stream waiting in the queues, as far as it matters for starting */
static double buffered(void)
{
	if (P.a_idx >= 0 && P.v_idx >= 0) return P.aq.dur > P.vq.dur - 1.5 ? P.aq.dur : P.vq.dur - 1.5;
	return P.a_idx >= 0 ? P.aq.dur : P.vq.dur;
}
static double buffer_need(void) { return P.net ? cfg.buffer_ms / 1000.0 : 0.15; }

static void check_buffering(void)
{
	if (!P.buffering || (P.skip_pending && !P.eof)) return;
	if (P.eof || P.aq.bytes + P.vq.bytes >= MAX_BYTES / 2 || buffered() >= buffer_need()) { P.buffering = 0; clk_update_run(); }
}

/* a decoder found its queue empty */
static void underrun(void)
{
	if (P.net && !P.eof && !P.buffering && P.opened && cfg.buffer_ms > 0) { P.buffering = 1; clk_update_run(); }
}

static int int_cb(void *opaque)
{
	int a;
	(void)opaque;
	pthread_mutex_lock(&mx); a = P.abort; pthread_mutex_unlock(&mx);
	return a;
}

static void set_err(const char *what, int code)
{
	char e[96] = "";
	if (code) av_strerror(code, e, sizeof(e));
	pthread_mutex_lock(&mx);
	snprintf(P.err, sizeof(P.err), "%s%s%s", what, code ? ": " : "", e);
	P.failed = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mx);
	fprintf(stderr, "nz-player: %s\n", P.err);
}

static AVCodecContext *open_decoder(AVStream *st, int video)
{
	const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
	AVCodecContext *ctx;
	if (!c || !(ctx = avcodec_alloc_context3(c))) return NULL;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) { avcodec_free_context(&ctx); return NULL; }
	ctx->pkt_timebase = st->time_base;
	if (video) { ctx->thread_count = 0; ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE; }
	if (avcodec_open2(ctx, c, NULL) < 0) { avcodec_free_context(&ctx); return NULL; }
	return ctx;
}

/* ================= sound thread ================= */
static void *audio_thread(void *arg)
{
	AVCodecContext *ctx = NULL; SwrContext *swr = NULL; AVFrame *fr = av_frame_alloc();
	int gen = -1, serial = -1, in_rate = 0, in_fmt = -1, out_rate = 0, was_paused = 0;
	AVChannelLayout in_layout = {0}; int16_t *buf = NULL; int buf_frames = 0;
	double pts_end = NAN;
	/* The last half second written, so that what the device had not played yet when a
	 * pause began can be written again afterwards: the clock then goes on without a jump. */
	enum { RING = 24000 }; int16_t *ring = av_mallocz(RING * 4); int ring_w = 0, ring_n = 0, replay = 0;
	(void)arg;
#define PAUSE_SOUND() do { double l_ = aout_latency(); replay = out_rate && ring ? (int)(l_ * out_rate) : 0; if (replay > ring_n) replay = ring_n; aout_flush(); } while (0)
	for (;;) {
		AVPacket *pkt = NULL; int ps = 0, got;
		pthread_mutex_lock(&mx);
		for (;;) {
			if (P.abort) break;
			if (P.buffering || P.user_paused) {
				if (!was_paused) { was_paused = 1; pthread_mutex_unlock(&mx); PAUSE_SOUND(); pthread_mutex_lock(&mx); continue; }
				wait_ms(100); continue;
			}
			was_paused = 0;
			if (P.aq.n) break;
			if (!P.a_done) underrun();
			wait_ms(100);
		}
		if (P.abort) { pthread_mutex_unlock(&mx); break; }
		got = q_get(&P.aq, &pkt, &ps);
		if (gen != P.a_gen) {                /* first run, or another sound track was chosen */
			AVStream *st = P.ic->streams[P.a_idx]; AVDictionaryEntry *l = av_dict_get(st->metadata, "language", NULL, 0);
			gen = P.a_gen; serial = -1;
			avcodec_free_context(&ctx);
			pthread_mutex_unlock(&mx);
			ctx = open_decoder(st, 0);
			pthread_mutex_lock(&mx);
			snprintf(P.acodec, sizeof(P.acodec), "%s", ctx ? avcodec_get_name(ctx->codec_id) : "unsupported");
			snprintf(P.alang, sizeof(P.alang), "%s", l ? l->value : "");
			in_rate = 0;
		}
		pthread_cond_broadcast(&cv);         /* the reader may be waiting for room */
		pthread_mutex_unlock(&mx);
		if (!got) continue;
		if (!ctx) { av_packet_free(&pkt); if (!pkt) { pthread_mutex_lock(&mx); P.a_done = 1; pthread_mutex_unlock(&mx); } continue; }
		if (ps != serial) { avcodec_flush_buffers(ctx); serial = ps; pts_end = NAN; aout_flush(); ring_n = replay = 0; if (swr) swr_close(swr), swr_init(swr); }
		if (replay > 0 && out_rate) {            /* after a pause: first what was cut off */
			int start = (ring_w - replay + RING) % RING, first = replay < RING - start ? replay : RING - start;
			aout_write(ring + start * 2, first);
			if (replay > first) aout_write(ring, replay - first);
		}
		replay = 0;
		if (avcodec_send_packet(ctx, pkt) < 0 && pkt) { av_packet_free(&pkt); continue; }
		int draining = !pkt;
		av_packet_free(&pkt);
		while (avcodec_receive_frame(ctx, fr) >= 0) {
			int n, stale;
			if (fr->sample_rate <= 0 || fr->ch_layout.nb_channels <= 0) { av_frame_unref(fr); continue; }
			if (fr->sample_rate != in_rate || fr->format != in_fmt || av_channel_layout_compare(&fr->ch_layout, &in_layout)) {
				AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, src;
				int rate = fr->sample_rate > 48000 ? 48000 : fr->sample_rate;
				swr_free(&swr);
				if (fr->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&src, fr->ch_layout.nb_channels);
				else av_channel_layout_copy(&src, &fr->ch_layout);
				if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, rate, &src, fr->format, fr->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) swr_free(&swr);
				av_channel_layout_uninit(&src);
				av_channel_layout_uninit(&in_layout); av_channel_layout_copy(&in_layout, &fr->ch_layout);
				in_rate = fr->sample_rate; in_fmt = fr->format;
				if (rate != out_rate) { out_rate = rate; aout_open(out_rate, 2); ring_n = replay = 0; }
				pthread_mutex_lock(&mx); P.arate = fr->sample_rate; P.ach = fr->ch_layout.nb_channels; snprintf(P.sink, sizeof(P.sink), "%s", aout_name()); pthread_mutex_unlock(&mx);
			}
			if (!swr) { av_frame_unref(fr); continue; }
			if (fr->best_effort_timestamp != AV_NOPTS_VALUE) pts_end = fr->best_effort_timestamp * av_q2d(ctx->pkt_timebase);
			if (!isnan(pts_end)) {
				double skip;
				pthread_mutex_lock(&mx); skip = serial == P.serial ? P.skip_until : NAN; pthread_mutex_unlock(&mx);
				if (!isnan(skip) && pts_end + (double)fr->nb_samples / fr->sample_rate < skip) { pts_end += (double)fr->nb_samples / fr->sample_rate; av_frame_unref(fr); continue; }
			}
			n = swr_get_out_samples(swr, fr->nb_samples);
			if (n > buf_frames) { int16_t *nb = av_realloc(buf, (size_t)n * 4); if (!nb) { av_frame_unref(fr); continue; } buf = nb; buf_frames = n; }
			n = swr_convert(swr, (uint8_t **)&buf, buf_frames, (const uint8_t **)fr->extended_data, fr->nb_samples);
			av_frame_unref(fr);
			if (n <= 0) continue;
			if (cfg.volume < 100) {              /* square law: the lower half of the scale stays usable */
				int g = cfg.volume * cfg.volume * 32768 / 10000;
				for (int i = 0; i < n * 2; i++) buf[i] = (int16_t)((buf[i] * g) >> 15);
			}
			for (int off = 0; off < n; ) {       /* in pieces, so that pause and seek take effect at once */
				int piece = n - off > 1024 ? 1024 : n - off;
				pthread_mutex_lock(&mx);
				while (!P.abort && serial == P.serial && gen == P.a_gen && (P.buffering || P.user_paused)) {
					if (!was_paused) { was_paused = 1; pthread_mutex_unlock(&mx); PAUSE_SOUND(); pthread_mutex_lock(&mx); continue; }
					wait_ms(100);
				}
				was_paused = 0;
				stale = P.abort || serial != P.serial || gen != P.a_gen;
				pthread_mutex_unlock(&mx);
				if (stale) break;
				if (replay > 0) {
					int start = (ring_w - replay + RING) % RING, first = replay < RING - start ? replay : RING - start;
					aout_write(ring + start * 2, first);
					if (replay > first) aout_write(ring, replay - first);
					replay = 0;
				}
				aout_write(buf + off * 2, piece);
				for (int i = 0; ring && i < piece; i++) { ring[ring_w * 2] = buf[(off + i) * 2]; ring[ring_w * 2 + 1] = buf[(off + i) * 2 + 1]; ring_w = (ring_w + 1) % RING; }
				if (ring_n < RING) ring_n = ring_n + piece > RING ? RING : ring_n + piece;
				off += piece;
				if (!isnan(pts_end)) {
					double lat = aout_latency();
					pts_end += (double)piece / out_rate;
					if (getenv("NZP_DEBUG")) { static double last; double n_ = now_s(); if (n_ - last > 1) { last = n_; fprintf(stderr, "nz-player: [debug] t=%.2f sound pts %.3f, device holds %.0f ms\n", n_, pts_end, lat * 1000); } }
					pthread_mutex_lock(&mx);
					if (serial == P.serial && P.clk_run) clk_set(pts_end - lat + cfg.av_delay_ms / 1000.0);
					pthread_mutex_unlock(&mx);
				}
			}
		}
		if (draining) {
			double lat = aout_latency();
			if (lat > 0 && lat < 1.0) usleep((useconds_t)(lat * 1e6));   /* let the last sound out */
			pthread_mutex_lock(&mx);
			if (serial == P.serial) P.a_done = 1;
			pthread_cond_broadcast(&cv);
			pthread_mutex_unlock(&mx);
		}
	}
#undef PAUSE_SOUND
	avcodec_free_context(&ctx); swr_free(&swr); av_frame_free(&fr); av_free(buf); av_free(ring); av_channel_layout_uninit(&in_layout);
	aout_close();
	return NULL;
}

/* ================= picture thread ================= */

/* Size of the scaled picture for the chosen aspect mode; it may be larger than the screen
 * in one direction ("fill"), the part outside is cut off afterwards. */
static void target_size(const AVFrame *f, int *dw, int *dh)
{
	AVRational sar = f->sample_aspect_ratio;
	double dar = (double)f->width / f->height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
	double w, h;
	if (cfg.aspect == 2) { *dw = SCR_W; *dh = SCR_H; return; }
	if ((dar > (double)SCR_W / SCR_H) == (cfg.aspect == 0)) { w = SCR_W; h = SCR_W / dar; }
	else { h = SCR_H; w = SCR_H * dar; }
	*dw = (int)(w + 0.5); *dh = (int)(h + 0.5);
	if (cfg.aspect == 0) { if (*dw > SCR_W) *dw = SCR_W; if (*dh > SCR_H) *dh = SCR_H; }
	/* a picture within 2 pixels of the screen size fills it: no hairline bars */
	if (abs(*dw - SCR_W) <= 2) *dw = SCR_W;
	if (abs(*dh - SCR_H) <= 2) *dh = SCR_H;
	if (*dw < 2) *dw = 2;
	if (*dh < 2) *dh = 2;
	if (*dw > 4096) *dw = 4096;
	if (*dh > 4096) *dh = 4096;
}

static void apply_level(AVCodecContext *ctx, int level)
{
	ctx->skip_loop_filter = level >= 2 ? AVDISCARD_ALL : level == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
	ctx->skip_frame = level >= 4 ? AVDISCARD_NONKEY : level == 3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

static void *video_thread(void *arg)
{
	AVCodecContext *ctx = arg; AVFrame *fr = av_frame_alloc();
	struct SwsContext *sws = NULL; int sw = 0, sh = 0, sfmt = -1, sdw = 0, sdh = 0, sflags = -1, scs = -1, srange = -1;
	uint8_t *tmp = NULL; size_t tmp_len = 0;
	int serial = -1; double last_pts = NAN, frame_dur, last_conv = 0;
	AVRational fps = av_guess_frame_rate(P.ic, P.ic->streams[P.v_idx], NULL);
	frame_dur = fps.num > 0 && fps.den > 0 ? av_q2d(av_inv_q(fps)) : 0.04;
	for (;;) {
		AVPacket *pkt = NULL; int ps = 0, level;
		pthread_mutex_lock(&mx);
		while (!P.abort && !P.vq.n) { if (P.a_idx < 0 && !P.v_done) underrun(); wait_ms(100); }
		if (P.abort) { pthread_mutex_unlock(&mx); break; }
		q_get(&P.vq, &pkt, &ps);
		/* Automatic mode: when more than a tenth of the frames of the last 3 s came out of the
		 * decoder too late, take one more short cut; the last one (key frames only) is for
		 * video that keeps falling behind by more than a second. */
		if (cfg.speed == 0) {
			double now = now_s();
			if (now - P.win_t0 > 3.0) {
				if (P.win_dec > 20 && P.win_late * 10 > P.win_dec) {
					if (P.level < 3) P.level++;
					else if (P.level == 3 && P.win_late_sum / P.win_late > 1.0) P.level = 4;
					fprintf(stderr, "nz-player: decoding short cuts level %d\n", P.level);
				}
				P.win_t0 = now; P.win_dec = P.win_late = 0; P.win_late_sum = 0;
			}
		} else P.level = cfg.speed == 1 ? 0 : cfg.speed;
		level = P.level;
		pthread_cond_broadcast(&cv);
		pthread_mutex_unlock(&mx);
		if (ps != serial) {
			avcodec_flush_buffers(ctx); serial = ps; last_pts = NAN;
			pthread_mutex_lock(&mx); P.win_t0 = now_s() + 2.0; P.win_dec = P.win_late = 0; P.win_late_sum = 0;
			if (P.level == 4 && cfg.speed == 0) P.level = 3;   /* give full motion another chance */
			pthread_mutex_unlock(&mx);   /* the first moments after a seek do not count */
		}
		apply_level(ctx, level);
		if (avcodec_send_packet(ctx, pkt) < 0 && pkt) { av_packet_free(&pkt); continue; }
		int draining = !pkt;
		av_packet_free(&pkt);
		while (avcodec_receive_frame(ctx, fr) >= 0) {
			double pts, m, late, now; int dw, dh, slot, flags, cs, range, stale = 0;
			struct pic *p; uint8_t *dst[4] = {0}; int dstride[4] = {0};
			if (fr->best_effort_timestamp != AV_NOPTS_VALUE) pts = fr->best_effort_timestamp * av_q2d(ctx->pkt_timebase);
			else pts = isnan(last_pts) ? 0 : last_pts + frame_dur;
			last_pts = pts;
			pthread_mutex_lock(&mx);
			P.decoded++;
			if (!isnan(P.skip_until) && serial == P.serial && pts < P.skip_until - 0.005) { pthread_mutex_unlock(&mx); av_frame_unref(fr); continue; }
			/* A frame that is already late: scaling it is wasted time, unless the screen has not
			 * changed for a while - a late picture is better than a frozen one. */
			m = clk_get(); now = now_s();
			late = P.clk_run && !isnan(m) && serial == P.serial ? m - pts : 0;
			if (late > 10.0) late = 0;           /* time stamps jumped; that is not lateness */
			if (now >= P.win_t0) { P.win_dec++; if (late > LATE_S) { P.win_late++; P.win_late_sum += late; } }
			if (late > LATE_S && P.shown_serial == serial && now - last_conv < 0.15) { P.dropped++; pthread_mutex_unlock(&mx); av_frame_unref(fr); continue; }
			last_conv = now;
			while (!P.abort && serial == P.serial && P.pq_n == PQ_N) wait_ms(100);
			stale = P.abort || serial != P.serial;
			slot = (P.pq_r + P.pq_n) % PQ_N;
			pthread_mutex_unlock(&mx);
			if (stale || fr->width <= 0 || fr->height <= 0) { av_frame_unref(fr); continue; }

			target_size(fr, &dw, &dh);
			flags = cfg.scaler == 0 ? SWS_FAST_BILINEAR : cfg.scaler == 2 ? SWS_BICUBIC : cfg.scaler == 3 ? SWS_LANCZOS : SWS_BILINEAR;
			cs = fr->colorspace != AVCOL_SPC_UNSPECIFIED ? fr->colorspace : fr->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
			range = fr->color_range == AVCOL_RANGE_JPEG;
			if (!sws || sw != fr->width || sh != fr->height || sfmt != fr->format || sdw != dw || sdh != dh || sflags != flags || scs != cs || srange != range) {
				sws_freeContext(sws);
				sws = sws_getContext(fr->width, fr->height, fr->format, dw, dh, AV_PIX_FMT_RGB565LE, flags, NULL, NULL, NULL);
				if (sws) sws_setColorspaceDetails(sws, sws_getCoefficients(cs == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : cs == AVCOL_SPC_SMPTE240M ? SWS_CS_SMPTE240M : cs == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU601),
					range, sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
				sw = fr->width; sh = fr->height; sfmt = fr->format; sdw = dw; sdh = dh; sflags = flags; scs = cs; srange = range;
				pthread_mutex_lock(&mx); P.vw = fr->width; P.vh = fr->height; pthread_mutex_unlock(&mx);
			}
			p = &P.pq[slot];
			if (!sws) { av_frame_unref(fr); continue; }
			{   /* scale into a scratch buffer (swscale may write a few pixels past the end of a
			     * line), then copy the part that is on the screen to its place, black around it */
				int stride = (dw + 32) * 2; size_t need = (size_t)stride * (dh + 1);
				int cw = dw < SCR_W ? dw : SCR_W, ch = dh < SCR_H ? dh : SCR_H;
				int sx = (dw - cw) / 2, sy = (dh - ch) / 2, ox = (SCR_W - cw) / 2, oy = (SCR_H - ch) / 2;
				if (need > tmp_len) { uint8_t *nt = av_realloc(tmp, need); if (!nt) { av_frame_unref(fr); continue; } tmp = nt; tmp_len = need; }
				dst[0] = tmp; dstride[0] = stride;
				sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dstride);
				if (cw < SCR_W || ch < SCR_H) memset(p->px, 0, sizeof(p->px));
				for (int y = 0; y < ch; y++) memcpy(p->px + (oy + y) * SCR_W + ox, tmp + (size_t)(sy + y) * stride + sx * 2, (size_t)cw * 2);
			}
			av_frame_unref(fr);
			p->pts = pts; p->serial = serial;
			pthread_mutex_lock(&mx);
			if (serial == P.serial && slot == (P.pq_r + P.pq_n) % PQ_N) { P.pq_n++; pthread_cond_broadcast(&cv); }
			pthread_mutex_unlock(&mx);
		}
		if (draining) {
			pthread_mutex_lock(&mx);
			if (serial == P.serial) P.v_done = 1;
			pthread_cond_broadcast(&cv);
			pthread_mutex_unlock(&mx);
		}
	}
	sws_freeContext(sws); av_frame_free(&fr); av_free(tmp);
	avcodec_free_context(&ctx);
	return NULL;
}

/* ================= reader thread ================= */
static int local_hls(const char *url)
{
	size_t n = strlen(url);
	return !is_url(url) && n > 5 && !strcasecmp(url + n - 5, ".m3u8");
}

static void choose_discard(void)
{
	for (unsigned i = 0; i < P.ic->nb_streams; i++)
		P.ic->streams[i]->discard = ((int)i == P.v_idx || (int)i == P.a_idx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

static int open_input(void)
{
	AVDictionary *o = NULL; AVCodecContext *vctx = NULL; int r, v, a;
	P.ic = avformat_alloc_context();
	if (!P.ic) { set_err("out of memory", 0); return -1; }
	P.ic->interrupt_callback.callback = int_cb;
	if (is_url(P.url)) {
		av_dict_set(&o, "user_agent", P.ua[0] ? P.ua : cfg.user_agent, 0);
		if (P.ref[0]) av_dict_set(&o, "referer", P.ref, 0);
		av_dict_set(&o, "reconnect", "1", 0);
		av_dict_set(&o, "reconnect_streamed", "1", 0);
		av_dict_set(&o, "reconnect_on_network_error", "1", 0);
		av_dict_set(&o, "reconnect_delay_max", "4", 0);
		av_dict_set(&o, "rw_timeout", "15000000", 0);
		P.ic->max_analyze_duration = 3 * AV_TIME_BASE;
	} else if (local_hls(P.url)) {
		av_dict_set(&o, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
		av_dict_set(&o, "allowed_extensions", "ALL", 0);
	}
	r = avformat_open_input(&P.ic, P.url, NULL, &o);
	av_dict_free(&o);
	if (r < 0) { P.ic = NULL; set_err("Cannot open", r); return -1; }
	P.ic->flags |= AVFMT_FLAG_GENPTS;
	if ((r = avformat_find_stream_info(P.ic, NULL)) < 0) { set_err("Unknown format", r); return -1; }

	v = av_find_best_stream(P.ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (v >= 0 && (P.ic->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;   /* cover art of a music file */
	a = av_find_best_stream(P.ic, AVMEDIA_TYPE_AUDIO, -1, v, NULL, 0);
	if (v >= 0 && !(vctx = open_decoder(P.ic->streams[v], 1))) {
		fprintf(stderr, "nz-player: no decoder for the picture (%s)\n", avcodec_get_name(P.ic->streams[v]->codecpar->codec_id));
		v = -1;
	}
	if (a >= 0 && !avcodec_find_decoder(P.ic->streams[a]->codecpar->codec_id)) a = -1;
	if (v < 0 && a < 0) { set_err("Nothing playable in this file", 0); avcodec_free_context(&vctx); return -1; }

	pthread_mutex_lock(&mx);
	P.v_idx = v; P.a_idx = a;
	{   /* sound tracks that belong to the chosen programme (all of them if there are no programmes) */
		AVProgram *pr = v >= 0 ? av_find_program_from_stream(P.ic, NULL, v) : NULL;
		for (unsigned i = 0; i < P.ic->nb_streams && P.n_audio < MAX_AUDIO; i++) {
			int in = !pr;
			if (P.ic->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
			for (unsigned k = 0; pr && k < pr->nb_stream_indexes; k++) in |= pr->stream_index[k] == i;
			if (in) P.audio_ids[P.n_audio++] = (int)i;
		}
	}
	P.net = is_url(P.url) || local_hls(P.url);
	P.duration = P.ic->duration != AV_NOPTS_VALUE && P.ic->duration > 0 ? P.ic->duration / (double)AV_TIME_BASE : 0;
	P.live = P.duration <= 0;
	P.seekable = !P.live && !(P.ic->ctx_flags & AVFMTCTX_UNSEEKABLE);
	P.start_time = P.ic->start_time != AV_NOPTS_VALUE ? P.ic->start_time / (double)AV_TIME_BASE : 0;
	if (v >= 0) {
		AVRational fr = av_guess_frame_rate(P.ic, P.ic->streams[v], NULL);
		snprintf(P.vcodec, sizeof(P.vcodec), "%s", avcodec_get_name(vctx->codec_id));
		P.vw = vctx->width; P.vh = vctx->height; P.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
	}
	/* Automatic: a picture shrunk to less than half its height does not show whether the
	 * decoder's deblocking filter ran, so it is left out from the start. */
	P.level = cfg.speed == 0 ? (P.vh > 360 ? 2 : 0) : cfg.speed == 1 ? 0 : cfg.speed;
	P.win_t0 = now_s();
	pthread_mutex_unlock(&mx);
	choose_discard();
	fprintf(stderr, "nz-player: %s: picture %s %dx%d, sound %s, %s, %.0f s\n", P.ic->iformat->name, v >= 0 ? P.vcodec : "none", P.vw, P.vh,
		a >= 0 ? avcodec_get_name(P.ic->streams[a]->codecpar->codec_id) : "none", P.live ? "live" : "file", P.duration);

	if (P.start_pos > 1 && P.seekable && P.start_pos < P.duration - 5)
		if (avformat_seek_file(P.ic, -1, INT64_MIN, (int64_t)((P.start_time + P.start_pos) * AV_TIME_BASE), INT64_MAX, 0) >= 0) {
			pthread_mutex_lock(&mx);
			P.pos_hint = P.start_pos; P.skip_until = P.start_time + P.start_pos; P.skip_pending = v >= 0;
			pthread_mutex_unlock(&mx);
		}

	if (v >= 0) { if (pthread_create(&P.th_video, NULL, video_thread, vctx)) { avcodec_free_context(&vctx); P.v_idx = -1; } else P.has_video = 1; }
	if (a >= 0) { if (pthread_create(&P.th_audio, NULL, audio_thread, NULL)) P.a_idx = -1; else P.has_audio = 1; }
	pthread_mutex_lock(&mx);
	P.opened = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mx);
	return 0;
}

static double pkt_seconds(const AVPacket *pkt, const AVStream *st)
{
	if (pkt->duration > 0) return pkt->duration * av_q2d(st->time_base);
	if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) return st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? av_q2d(av_inv_q(st->avg_frame_rate)) : 0.04;
	if (st->codecpar->frame_size > 0 && st->codecpar->sample_rate > 0) return (double)st->codecpar->frame_size / st->codecpar->sample_rate;
	return 0;
}

static void *read_thread(void *arg)
{
	AVPacket *pkt = NULL; int drained = 0;
	(void)arg;
	if (open_input() < 0) return NULL;
	for (;;) {
		int r, gen = 0, do_seek; double target = 0, cap;
		pthread_mutex_lock(&mx);
		for (;;) {
			long long bytes = P.aq.bytes + P.vq.bytes;
			if (P.abort || P.seek_req) break;
			cap = P.net ? cfg.buffer_ms / 1000.0 + 4.0 : 2.0;
			if (!P.eof && bytes < MAX_BYTES && !((P.a_idx < 0 || P.aq.dur > cap) && (P.v_idx < 0 || P.vq.dur > cap))) break;
			check_buffering();
			wait_ms(P.eof ? 100 : 20);
		}
		if (P.abort) { pthread_mutex_unlock(&mx); break; }
		if ((do_seek = P.seek_req)) { gen = P.seek_gen; target = P.seek_target; }
		pthread_mutex_unlock(&mx);

		if (do_seek) {
			int64_t ts = (int64_t)((P.start_time + target) * AV_TIME_BASE);
			r = avformat_seek_file(P.ic, -1, INT64_MIN, ts, INT64_MAX, 0);
			if (r < 0) r = av_seek_frame(P.ic, -1, ts, AVSEEK_FLAG_BACKWARD);
			pthread_mutex_lock(&mx);
			if (r >= 0) {
				q_flush(&P.aq); q_flush(&P.vq);
				P.serial++; P.eof = 0; P.a_done = P.v_done = 0; drained = 0;
				P.pq_n = 0; P.clk_pts = NAN; P.nan_since = 0; P.pos_hint = target;
				P.skip_until = P.start_time + target; P.skip_pending = P.v_idx >= 0;
				P.buffering = 1; clk_update_run();
			}
			if (gen == P.seek_gen) P.seek_req = 0;
			pthread_cond_broadcast(&cv);
			pthread_mutex_unlock(&mx);
			continue;
		}

		if (!pkt && !(pkt = av_packet_alloc())) { set_err("out of memory", 0); break; }
		r = av_read_frame(P.ic, pkt);
		if (r == AVERROR(EAGAIN)) { usleep(10000); continue; }
		pthread_mutex_lock(&mx);
		if (r < 0) {
			if (!P.abort) {
				if (r != AVERROR_EOF) { char e[96]; av_strerror(r, e, sizeof(e)); fprintf(stderr, "nz-player: reading stopped: %s\n", e); }
				P.eof = 1;
				if (!drained) {                  /* an empty packet tells each decoder to give out what it still holds */
					if (P.a_idx >= 0) q_put(&P.aq, NULL, 0);
					if (P.v_idx >= 0) q_put(&P.vq, NULL, 0);
					drained = 1;
				}
				check_buffering();
				pthread_cond_broadcast(&cv);
			}
		} else {
			int idx = pkt->stream_index;
			if (idx == P.v_idx && P.skip_pending) {
				/* The demuxer went back to a key frame.  If that is close to the place asked for,
				 * the frames in between are decoded without being played; if it is far, play from
				 * the key frame (decoding seconds of video unseen would feel like a hang). */
				int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
				if (ts == AV_NOPTS_VALUE || P.skip_until - ts * av_q2d(P.ic->streams[idx]->time_base) > 3.0) P.skip_until = NAN;
				P.skip_pending = 0;
			}
			if (idx == P.a_idx || idx == P.v_idx) {
				q_put(idx == P.a_idx ? &P.aq : &P.vq, pkt, pkt_seconds(pkt, P.ic->streams[idx]));
				pkt = NULL;
				check_buffering();
				pthread_cond_broadcast(&cv);
			} else av_packet_unref(pkt);
		}
		pthread_mutex_unlock(&mx);
	}
	av_packet_free(&pkt);
	return NULL;
}

/* ================= interface for the caller ================= */
void pl_open(const char *url, const char *ua, const char *referer, double start_pos)
{
	if (!cv_ready) {
		pthread_condattr_t ca; pthread_condattr_init(&ca); pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
		pthread_cond_init(&cv, &ca); cv_ready = 1;
		av_log_set_level(getenv("NZP_DEBUG") ? AV_LOG_INFO : AV_LOG_ERROR);
		avformat_network_init();
	}
	pl_close();
	memset(&P, 0, sizeof(P));
	snprintf(P.url, sizeof(P.url), "%s", url);
	snprintf(P.ua, sizeof(P.ua), "%s", ua ? ua : "");
	snprintf(P.ref, sizeof(P.ref), "%s", referer ? referer : "");
	P.start_pos = start_pos; P.v_idx = P.a_idx = -1; P.clk_pts = NAN; P.skip_until = NAN; P.buffering = 1; P.shown_serial = -1;
	P.pq = av_mallocz(PQ_N * sizeof(*P.pq));
	if (!P.pq || pthread_create(&P.th_read, NULL, read_thread, NULL)) { snprintf(P.err, sizeof(P.err), "out of memory"); P.failed = 1; return; }
	P.has_read = 1;
	if (getenv("NZP_DEBUG")) fprintf(stderr, "nz-player: [debug] t=%.2f open %s\n", now_s(), url);
}

void pl_close(void)
{
	if (!cv_ready) return;
	pthread_mutex_lock(&mx);
	P.abort = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mx);
	if (P.has_read) pthread_join(P.th_read, NULL);
	if (P.has_audio) pthread_join(P.th_audio, NULL);
	if (P.has_video) pthread_join(P.th_video, NULL);
	P.has_read = P.has_audio = P.has_video = 0;
	q_flush(&P.aq); q_flush(&P.vq);
	if (P.ic) avformat_close_input(&P.ic);
	av_freep(&P.pq);
	P.opened = 0; P.pq_n = 0;
}

enum pl_state pl_state(void)
{
	enum pl_state s;
	pthread_mutex_lock(&mx);
	if (P.failed) s = PL_ERROR;
	else if (!P.opened) s = PL_OPENING;
	else if (P.eof && !P.seek_req && !P.aq.n && !P.vq.n && (P.a_idx < 0 || P.a_done) && (P.v_idx < 0 || (P.v_done && !P.pq_n))) s = PL_ENDED;
	else if (P.user_paused) s = PL_PAUSED;
	else if (P.buffering || P.seek_req) s = PL_BUFFERING;
	else s = PL_PLAYING;
	pthread_mutex_unlock(&mx);
	return s;
}

const char *pl_error(void) { return P.err; }
double pl_duration(void)
{
	double d;
	pthread_mutex_lock(&mx); d = P.duration; pthread_mutex_unlock(&mx);
	return d;
}

static double position_locked(void)
{
	double m;
	if (P.seek_req) return P.seek_target;
	m = clk_get();
	if (!isnan(m)) { double p = m - P.start_time; P.pos_hint = p < 0 ? 0 : p; }
	return P.pos_hint;
}

double pl_position(void)
{
	double p;
	pthread_mutex_lock(&mx); p = position_locked(); pthread_mutex_unlock(&mx);
	return p;
}

static int seek_to(double t, int relative)
{
	int ok = -1;
	pthread_mutex_lock(&mx);
	if (P.opened && P.seekable) {
		if (relative) t += position_locked();
		if (t > P.duration - 2) t = P.duration - 2;
		if (t < 0) t = 0;
		P.seek_target = t; P.seek_req = 1; P.seek_gen++; ok = 0;
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&mx);
	return ok;
}
int pl_seek_rel(double s) { return seek_to(s, 1); }
int pl_seek_abs(double s) { return seek_to(s, 0); }

void pl_pause(int on)
{
	pthread_mutex_lock(&mx);
	P.user_paused = on; clk_update_run();
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mx);
}
int pl_paused(void)
{
	int p;
	pthread_mutex_lock(&mx); p = P.user_paused; pthread_mutex_unlock(&mx);
	return p;
}

int pl_cycle_audio(void)
{
	int no = 0, at = 0; double pos;
	pthread_mutex_lock(&mx);
	if (P.opened && P.has_audio && P.n_audio > 1) {
		for (int i = 0; i < P.n_audio; i++) if (P.audio_ids[i] == P.a_idx) at = i;
		at = (at + 1) % P.n_audio; no = at + 1;
		P.a_idx = P.audio_ids[at]; P.a_gen++;
		q_flush(&P.aq);
		choose_discard();
		pos = position_locked();
		if (P.seekable) { P.seek_target = pos; P.seek_req = 1; P.seek_gen++; }   /* start both again from here, in step */
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&mx);
	return no;
}

void pl_info(struct pl_info *i)
{
	memset(i, 0, sizeof(*i));
	pthread_mutex_lock(&mx);
	snprintf(i->vcodec, sizeof(i->vcodec), "%s", P.vcodec); snprintf(i->acodec, sizeof(i->acodec), "%s", P.acodec);
	snprintf(i->alang, sizeof(i->alang), "%s", P.alang); snprintf(i->sink, sizeof(i->sink), "%s", P.sink);
	i->w = P.vw; i->h = P.vh; i->fps = P.fps; i->rate = P.arate; i->channels = P.ach;
	i->n_audio = P.n_audio; i->level = P.level; i->net = P.net; i->live = P.live; i->seekable = P.seekable;
	i->has_video = P.v_idx >= 0; i->has_audio = P.a_idx >= 0;
	for (int k = 0; k < P.n_audio; k++) if (P.audio_ids[k] == P.a_idx) i->audio_no = k + 1;
	i->buffered = P.opened ? (buffered() > 0 ? buffered() : 0) : 0; i->buffer_need = buffer_need();
	i->shown = P.shown; i->dropped = P.dropped; i->dropped_disp = P.dropped_disp; i->dropped_late = P.dropped - P.dropped_disp; i->decoded = P.decoded;
	pthread_mutex_unlock(&mx);
}

int pl_refresh(uint16_t *fb, int *wait_ms_out)
{
	int changed = 0, wait = 50;
	pthread_mutex_lock(&mx);
	if (P.opened && !P.pq_n && P.clk_run && P.v_idx >= 0) wait = 4;   /* a picture may arrive and be due any moment */
	while (P.opened && P.pq_n) {
		struct pic *p = &P.pq[P.pq_r]; double m, diff; int show = 0;
		if (p->serial != P.serial) { P.pq_r = (P.pq_r + 1) % PQ_N; P.pq_n--; continue; }
		m = clk_get();
		if (P.shown_serial != p->serial) {   /* first picture after opening or seeking: show at once */
			show = 1;
			if (P.a_idx < 0 || P.a_done) clk_set(p->pts);
		} else if (!P.clk_run) break;
		else if (isnan(m)) {                 /* sound has not started: wait a moment, then go by the picture */
			double now = now_s();
			if (P.a_idx < 0 || P.a_done) { clk_set(p->pts); show = 1; }
			else if (P.nan_since == 0) { P.nan_since = now; wait = 10; break; }
			else if (now - P.nan_since > 1.0) { clk_set(p->pts); show = 1; }
			else { wait = 10; break; }
		} else {
			P.nan_since = 0;
			diff = p->pts - m;
			if (fabs(diff) > 10.0) {         /* time stamps jumped (live streams do that) */
				show = 1;
				if (P.a_idx < 0 || P.a_done) clk_set(p->pts);
			} else if (diff > 0.002) { wait = (int)(diff * 1000) + 1; break; }
			else if (P.pq_n > 1 && P.pq[(P.pq_r + 1) % PQ_N].serial == P.serial && P.pq[(P.pq_r + 1) % PQ_N].pts <= m) {
				P.pq_r = (P.pq_r + 1) % PQ_N; P.pq_n--; P.dropped++; P.dropped_disp++;   /* the next one is due as well */
				continue;
			} else show = 1;
		}
		if (show) {
			memcpy(fb, p->px, sizeof(p->px));
			P.shown_serial = p->serial; P.shown++; changed = 1;
			P.pq_r = (P.pq_r + 1) % PQ_N; P.pq_n--;
			wait = 4;
			pthread_cond_broadcast(&cv);
			/* how long until the one after it? */
			if (P.pq_n && P.clk_run && !isnan(m = clk_get())) {
				diff = P.pq[P.pq_r].pts - m;
				wait = diff > 0.25 ? 250 : diff > 0.001 ? (int)(diff * 1000) : 1;
			}
		}
		break;
	}
	pthread_mutex_unlock(&mx);
	if (wait > 250) wait = 250;
	*wait_ms_out = wait;
	return changed;
}
