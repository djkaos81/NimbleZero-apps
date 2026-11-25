/* SPDX-License-Identifier: GPL-2.0-only
 * Stand-in for src/hwdec.c on a PC: the same interface, served by FFmpeg's software
 * decoder.  It lets the tests run the player's GPU code path (feeding, collecting,
 * seeking, end of stream, falling back to software) where there is no GPU decoder.
 *   NZP_FAKE_HW=1            needed, or the player does not use it
 *   NZP_FAKE_FAIL_AFTER=N    report a hardware failure after N pictures
 *   NZP_FAKE_SLOW_MS=N       let scaling a picture take N ms (a decoder too slow for the stream)
 */
#include <stdlib.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "nzp.h"

struct hwdec { AVCodecContext *ctx; AVFrame *fr; struct SwsContext *sws; uint8_t *buf; int bw, bh, have, draining, ended; long n_out, fail_after; double fed, out; };

int hw_usable(const AVCodecParameters *par) { return cfg.hwdec && getenv("NZP_FAKE_HW") && par->codec_id == AV_CODEC_ID_H264; }

struct hwdec *hw_open(const AVStream *st)
{
	struct hwdec *h = calloc(1, sizeof(*h)); const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
	if (!h || !c || !(h->ctx = avcodec_alloc_context3(c))) return NULL;
	avcodec_parameters_to_context(h->ctx, st->codecpar); h->ctx->pkt_timebase = st->time_base; h->ctx->thread_count = 1;
	if (avcodec_open2(h->ctx, c, NULL) < 0 || !(h->fr = av_frame_alloc())) return NULL;
	if (getenv("NZP_FAKE_FAIL_AFTER")) h->fail_after = atol(getenv("NZP_FAKE_FAIL_AFTER"));
	return h;
}
void hw_close(struct hwdec *h) { if (!h) return; avcodec_free_context(&h->ctx); av_frame_free(&h->fr); sws_freeContext(h->sws); free(h->buf); free(h); }
int hw_can_send(struct hwdec *h) { return !h->draining; }
int hw_wants_more(struct hwdec *h) { return !h->n_out || h->fed - h->out < 0.3; }
int hw_ended(struct hwdec *h) { return h->ended; }
int hw_send(struct hwdec *h, const AVPacket *pkt)
{
	if (!pkt) { if (!h->draining) avcodec_send_packet(h->ctx, NULL); h->draining = 1; return 0; }
	if (getenv("NZP_FAKE_SLOW_DEC_MS")) usleep((useconds_t)atoi(getenv("NZP_FAKE_SLOW_DEC_MS")) * 1000);   /* a decoder slower than the stream */
	avcodec_send_packet(h->ctx, pkt); if (pkt->pts != AV_NOPTS_VALUE) h->fed = pkt->pts * av_q2d(h->ctx->pkt_timebase);
	return 0;
}
int hw_next(struct hwdec *h, double *pts)
{
	int r;
	if (h->have) return -1;
	if (h->fail_after && h->n_out >= h->fail_after) return -1;
	if (h->ended) return 2;
	r = avcodec_receive_frame(h->ctx, h->fr);
	if (r == AVERROR_EOF) { h->ended = 1; return 2; }
	if (r < 0) return 0;
	*pts = h->fr->best_effort_timestamp * av_q2d(h->ctx->pkt_timebase);
	h->out = *pts; h->have = 1; h->n_out++;
	return 1;
}
void hw_skip(struct hwdec *h) { if (h->have) av_frame_unref(h->fr); h->have = 0; }
void hw_size(struct hwdec *h, int *w, int *hgt) { *w = h->fr->width; *hgt = h->fr->height; }
/* two pictures can be under way, as with the real scaler */
static uint8_t *fb[2]; static int fw[2], fh[2], f_r, f_n;
int hw_scale_begin(struct hwdec *h, int w, int hgt)
{
	uint8_t *d[4] = {0}; int ds[4] = {0}, k;
	if (!h->have) return -1;
	w &= ~1; hgt &= ~1; if (w < 32) w = 32; if (hgt < 32) hgt = 32;
	if (f_n && (f_n >= 2 || w != h->bw || hgt != h->bh)) return 1;
	if (getenv("NZP_FAKE_SLOW_MS")) usleep((useconds_t)atoi(getenv("NZP_FAKE_SLOW_MS")) * 1000);
	k = (f_r + f_n) % 2;
	free(fb[k]); fb[k] = malloc((size_t)(w + 16) * 2 * (hgt + 1)); fw[k] = w; fh[k] = hgt; h->bw = w; h->bh = hgt;
	h->sws = sws_getCachedContext(h->sws, h->fr->width, h->fr->height, h->fr->format, w, hgt, AV_PIX_FMT_RGB565LE, SWS_BILINEAR, NULL, NULL, NULL);
	if (!h->sws || !fb[k]) return -1;
	d[0] = fb[k]; ds[0] = (w + 16) * 2;
	sws_scale(h->sws, (const uint8_t *const *)h->fr->data, h->fr->linesize, 0, h->fr->height, d, ds);
	av_frame_unref(h->fr); h->have = 0; f_n++;
	return 0;
}
int hw_scale_end(struct hwdec *h, const uint8_t **px, int *stride, int *got_w, int *got_h)
{
	(void)h;
	if (!f_n) return 1;
	*px = fb[f_r]; *stride = (fw[f_r] + 16) * 2; *got_w = fw[f_r]; *got_h = fh[f_r];
	f_r = (f_r + 1) % 2; f_n--;
	return 0;
}
int hw_flush(struct hwdec *h) { f_n = 0; avcodec_flush_buffers(h->ctx); if (h->have) av_frame_unref(h->fr); h->have = h->draining = h->ended = 0; h->n_out = 0; return 0; }
void hw_wait(struct hwdec *h, int ms) { (void)h; usleep((useconds_t)ms * 1000); }
