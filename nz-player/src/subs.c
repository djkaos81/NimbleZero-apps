/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - subtitles.
 *
 * Text formats (SRT, ASS/SSA, WebVTT, MP4 text, ...): FFmpeg's decoders turn every one of
 * them into ASS events, and libass draws those (fonts, line breaks, styles, any script the
 * font has).  libass is loaded at run time, as libpulse is: without it, or without a font,
 * text subtitles are not offered and the rest of the player is unaffected.
 *
 * Picture formats (DVB, Blu-ray PGS, DVD): the decoder delivers small pictures with a
 * place on the video picture; they are shrunk with the video and laid over it.
 *
 * One track is active at a time.  A track inside the video file gets its packets from the
 * reader as they come by; a file next to the video is read completely when it is chosen.
 */
#include "nzp.h"
#include <dlfcn.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

/* ---- libass, declared here so that its headers are not needed to build ---- */
typedef struct ASS_Image { int w, h, stride; unsigned char *bitmap; uint32_t color; int dst_x, dst_y; struct ASS_Image *next; int type; } ASS_Image;
static void *(*a_lib_init)(void); static void (*a_lib_done)(void *);
static void *(*a_ren_init)(void *); static void (*a_ren_done)(void *);
static void (*a_set_frame)(void *, int, int);
static void (*a_set_fonts)(void *, const char *, const char *, int, const char *, int);
static void (*a_set_scale)(void *, double);
static void (*a_set_hinting)(void *, int);   /* optional */
static void *(*a_new_track)(void *); static void (*a_free_track)(void *);
static void (*a_codec_private)(void *, char *, int);
static void (*a_chunk)(void *, char *, int, long long, long long);
static void (*a_flush)(void *);
static ASS_Image *(*a_render)(void *, void *, long long, int *);
static void (*a_msg_cb)(void *, void (*)(int, const char *, va_list, void *), void *);

#define MAX_EV 24
struct bev {                                 /* one subtitle picture */
	double start, end; int x, y, w, h, cw, ch; unsigned id;
	uint32_t *argb;                          /* w x h, as decoded */
	uint32_t *sc; int sx, sy, sw, sh;        /* shrunk for the screen (made when first drawn) */
	int kx, ky, kw, kh;                      /* the picture rectangle sc was made for */
};
static struct {
	pthread_mutex_t mx; int tried, ass_ok;
	void *lib, *ren, *track;
	AVCodecContext *ctx; int text, external;
	struct bev ev[MAX_EV]; int n_ev; unsigned next_id, drawn_sig;
} S = { .mx = PTHREAD_MUTEX_INITIALIZER };

static void quiet(int level, const char *fmt, va_list va, void *data) { (void)level; (void)fmt; (void)va; (void)data; }

static const char *font_file(void)
{
	static const char *c[] = { "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL };
	const char *e = getenv("NZP_SUB_FONT");
	if (e && !access(e, R_OK)) return e;
	for (int i = 0; c[i]; i++) if (!access(c[i], R_OK)) return c[i];
	return NULL;
}

/* Can text subtitles be drawn here?  (libass and a font) */
int sub_text_ok(void)
{
	void *h; const char *font;
	pthread_mutex_lock(&S.mx);
	if (!S.tried) {
		S.tried = 1;
		font = font_file();
		h = getenv("NZP_NO_ASS") ? NULL : dlopen("libass.so.9", RTLD_NOW | RTLD_GLOBAL);
#define SYM(v, n) (*(void **)&(v) = h ? dlsym(h, n) : NULL)
		if (h && font && SYM(a_lib_init, "ass_library_init") && SYM(a_lib_done, "ass_library_done") && SYM(a_ren_init, "ass_renderer_init")
		    && SYM(a_ren_done, "ass_renderer_done") && SYM(a_set_frame, "ass_set_frame_size") && SYM(a_set_fonts, "ass_set_fonts")
		    && SYM(a_set_scale, "ass_set_font_scale") && SYM(a_new_track, "ass_new_track") && SYM(a_free_track, "ass_free_track")
		    && SYM(a_codec_private, "ass_process_codec_private") && SYM(a_chunk, "ass_process_chunk") && SYM(a_flush, "ass_flush_events")
		    && SYM(a_render, "ass_render_frame") && SYM(a_msg_cb, "ass_set_message_cb")) {
			if ((S.lib = a_lib_init())) {
				a_msg_cb(S.lib, quiet, NULL);
				if ((S.ren = a_ren_init(S.lib))) {
					a_set_frame(S.ren, SCR_W, SCR_H);
					a_set_fonts(S.ren, font, "sans-serif", 0, NULL, 0);   /* 0: this one font for everything */
					*(void **)&a_set_hinting = dlsym(h, "ass_set_hinting");
					if (a_set_hinting) a_set_hinting(S.ren, 1);   /* light hinting: small letters come out crisper */
					S.ass_ok = 1;
				}
			}
		}
#undef SYM
		if (!S.ass_ok) fprintf(stderr, "nz-player: subtitles: text subtitles are not available (%s)\n", !h ? "libass.so.9 not found" : !font ? "no DejaVu Sans font" : "libass did not start");
	}
	pthread_mutex_unlock(&S.mx);
	return S.ass_ok;
}

/* Can a track of this kind be shown? */
int sub_can_show(const AVCodecParameters *par)
{
	const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
	if (!d || !avcodec_find_decoder(par->codec_id)) return 0;
	if (d->props & AV_CODEC_PROP_BITMAP_SUB) return 1;
	return (d->props & AV_CODEC_PROP_TEXT_SUB) && sub_text_ok();
}

static void free_events(void)
{
	for (int i = 0; i < S.n_ev; i++) { av_free(S.ev[i].argb); av_free(S.ev[i].sc); }
	S.n_ev = 0;
}

static void stop_locked(void)
{
	if (S.track) { a_free_track(S.track); S.track = NULL; }
	avcodec_free_context(&S.ctx);
	free_events();
	S.text = S.external = 0;
}

void sub_stop(void) { pthread_mutex_lock(&S.mx); stop_locked(); pthread_mutex_unlock(&S.mx); }

/* Begin a track.  0 ok. */
int sub_start(const AVCodecParameters *par, int tb_num, int tb_den)
{
	AVRational tb = { tb_num, tb_den };
	const AVCodec *c = avcodec_find_decoder(par->codec_id); const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
	static const int px[] = { 14, 18, 23 };  /* letter size on the 170-line screen: small, normal, large */
	int text = d && (d->props & AV_CODEC_PROP_TEXT_SUB);
	if (!c || !d || (text && !sub_text_ok())) return -1;
	pthread_mutex_lock(&S.mx);
	stop_locked();
	if (!(S.ctx = avcodec_alloc_context3(c)) || avcodec_parameters_to_context(S.ctx, par) < 0) goto bad;
	S.ctx->pkt_timebase = tb;
	if (avcodec_open2(S.ctx, c, NULL) < 0) goto bad;
	S.text = text;
	if (text) {
		if (!(S.track = a_new_track(S.lib))) goto bad;
		int size = px[cfg.sub_size < 0 || cfg.sub_size > 2 ? 1 : cfg.sub_size];
		if (par->codec_id == AV_CODEC_ID_ASS || par->codec_id == AV_CODEC_ID_SSA) {
			/* ASS brings its own styles, made for a page of its own size (PlayResY lines) and
			 * usually for a large screen.  They are kept; the letters are scaled so that the
			 * file's main style comes out at the chosen size, whatever page the author used. */
			const char *hd = (const char *)S.ctx->subtitle_header, *q; double resy = 288, fs = 0;
			if (hd) {
				a_codec_private(S.track, (char *)S.ctx->subtitle_header, S.ctx->subtitle_header_size);
				if ((q = strstr(hd, "PlayResY:"))) resy = atof(q + 9);
				for (q = hd; (q = strstr(q, "Style:")); q += 6) {       /* "Style: Name,Fontname,Fontsize,..." */
					const char *nm = q + 6, *c1, *c2; int def;
					while (*nm == ' ') nm++;
					def = !strncmp(nm, "Default,", 8) || !strncmp(nm, "*Default,", 9);
					if ((c1 = strchr(nm, ',')) && (c2 = strchr(c1 + 1, ',')) && (fs == 0 || def)) fs = atof(c2 + 1);
					if (def) break;
				}
			}
			if (resy < 16) resy = 288;
			if (fs < 1) fs = resy / 18;
			{ double k = size / (fs * SCR_H / resy); a_set_scale(S.ren, k < 0.25 ? 0.25 : k > 6 ? 6 : k); }
		} else {
			/* Plain text formats: a style of our own on a page the size of the screen, so that
			 * sizes are screen pixels: white letters, a black outline of 1.4 pixels, no shadow. */
			char hd[1024];
			int n = snprintf(hd, sizeof(hd), "[Script Info]\nScriptType: v4.00+\nPlayResX: %d\nPlayResY: %d\nScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
				"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
				"Style: Default,DejaVu Sans,%d,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1.4,0,2,6,6,5,1\n\n"
				"[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n", SCR_W, SCR_H, size);
			if (n < 0 || n >= (int)sizeof(hd)) goto bad;
			a_codec_private(S.track, hd, n);
			a_set_scale(S.ren, 1.0);
		}
	}
	pthread_mutex_unlock(&S.mx);
	return 0;
bad:
	stop_locked();
	pthread_mutex_unlock(&S.mx);
	return -1;
}

/* One packet of the active track. */
void sub_packet(const AVPacket *pkt, int tb_num, int tb_den)
{
	AVRational tb = { tb_num, tb_den };
	AVSubtitle sub; int got = 0; AVPacket *c; double base, start, end;
	if (!pkt || !(c = av_packet_clone(pkt))) return;
	pthread_mutex_lock(&S.mx);
	if (!S.ctx || avcodec_decode_subtitle2(S.ctx, &sub, &got, c) < 0 || !got) { pthread_mutex_unlock(&S.mx); av_packet_free(&c); return; }
	base = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(tb) : sub.pts != AV_NOPTS_VALUE ? sub.pts / (double)AV_TIME_BASE : 0;
	start = base + sub.start_display_time / 1000.0;
	if (sub.end_display_time && sub.end_display_time != UINT32_MAX) end = base + sub.end_display_time / 1000.0;
	else if (pkt->duration > 0) end = start + pkt->duration * av_q2d(tb);
	else end = start + (S.text ? 5.0 : 30.0);   /* picture formats: until the next one replaces it */
	if (S.text) {
		for (unsigned i = 0; i < sub.num_rects; i++)
			if (sub.rects[i]->type == SUBTITLE_ASS && sub.rects[i]->ass && S.track)
				a_chunk(S.track, sub.rects[i]->ass, (int)strlen(sub.rects[i]->ass), (long long)(start * 1000), (long long)((end - start) * 1000));
	} else {
		/* a new set of pictures replaces what is on the screen (an empty set clears it) */
		for (int i = 0; i < S.n_ev; i++) if (S.ev[i].end > start && S.ev[i].start < start) S.ev[i].end = start;
		for (unsigned i = 0; i < sub.num_rects; i++) {
			AVSubtitleRect *r = sub.rects[i]; struct bev *e; const uint32_t *pal;
			if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1] || r->w > 4096 || r->h > 2304) continue;
			if (S.n_ev == MAX_EV) { av_free(S.ev[0].argb); av_free(S.ev[0].sc); memmove(S.ev, S.ev + 1, sizeof(S.ev[0]) * (MAX_EV - 1)); S.n_ev--; }
			e = &S.ev[S.n_ev]; memset(e, 0, sizeof(*e));
			if (!(e->argb = av_malloc((size_t)r->w * r->h * 4))) continue;
			pal = (const uint32_t *)r->data[1];
			for (int y = 0; y < r->h; y++) for (int x = 0; x < r->w; x++) { unsigned k = r->data[0][y * r->linesize[0] + x]; e->argb[y * r->w + x] = k < (unsigned)r->nb_colors ? pal[k] : 0; }
			e->start = start; e->end = end; e->x = r->x; e->y = r->y; e->w = r->w; e->h = r->h; e->cw = S.ctx->width; e->ch = S.ctx->height; e->id = ++S.next_id;
			S.n_ev++;
		}
	}
	avsubtitle_free(&sub);
	pthread_mutex_unlock(&S.mx);
	av_packet_free(&c);
}

/* After a seek: forget what was read (not for a subtitle file, which is held complete). */
void sub_flush(void)
{
	pthread_mutex_lock(&S.mx);
	if (!S.external) {
		if (S.track) a_flush(S.track);
		if (S.ctx) avcodec_flush_buffers(S.ctx);
		free_events();
	}
	pthread_mutex_unlock(&S.mx);
}

/* Subtitle files from Windows are often not UTF-8.  If this one is not, write a UTF-8 copy
 * (reading it as Windows-1252, the usual case for western languages) and return its path. */
static const char *utf8_copy(const char *path, char *tmp, size_t tn)
{
	static const uint16_t hi[32] = { 0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
	                                 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178 };
	FILE *f = fopen(path, "rb"), *o; unsigned char *b; long n; int bad = 0;
	if (!f) return path;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 8 << 20 || !(b = malloc((size_t)n))) { fclose(f); return path; }
	if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return path; }
	fclose(f);
	if (n >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) { free(b); return path; }   /* UTF-16: left to FFmpeg */
	for (long i = 0; i < n && !bad; ) {
		int k = b[i] < 0x80 ? 0 : (b[i] & 0xE0) == 0xC0 ? 1 : (b[i] & 0xF0) == 0xE0 ? 2 : (b[i] & 0xF8) == 0xF0 ? 3 : -1;
		if (k < 0 || i + k >= n) { bad = 1; break; }
		for (int j = 1; j <= k; j++) if ((b[i + j] & 0xC0) != 0x80) bad = 1;
		i += k + 1;
	}
	if (!bad) { free(b); return path; }
	snprintf(tmp, tn, "/tmp/nz-player-sub-%d%s", (int)getpid(), strrchr(path, '.') ? strrchr(path, '.') : "");
	if (!(o = fopen(tmp, "wb"))) { free(b); return path; }
	for (long i = 0; i < n; i++) {
		unsigned u = b[i] < 0x80 ? b[i] : b[i] < 0xA0 ? hi[b[i] - 0x80] : b[i];
		if (u < 0x80) fputc((int)u, o);
		else if (u < 0x800) { fputc(0xC0 | (u >> 6), o); fputc(0x80 | (u & 63), o); }
		else { fputc(0xE0 | (u >> 12), o); fputc(0x80 | ((u >> 6) & 63), o); fputc(0x80 | (u & 63), o); }
	}
	fclose(o); free(b);
	return tmp;
}

/* Read a subtitle file completely and make it the active track.  0 ok. */
int sub_load_file(const char *path)
{
	AVFormatContext *ic = NULL; AVPacket *pkt; char tmp[128] = ""; const char *use = utf8_copy(path, tmp, sizeof(tmp)); int si = -1, r = -1;
	if (avformat_open_input(&ic, use, NULL, NULL) < 0) goto out;
	if (avformat_find_stream_info(ic, NULL) < 0) goto out;
	for (unsigned i = 0; i < ic->nb_streams; i++) if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) { si = (int)i; break; }
	if (si < 0 || sub_start(ic->streams[si]->codecpar, ic->streams[si]->time_base.num, ic->streams[si]->time_base.den) < 0) goto out;
	if ((pkt = av_packet_alloc())) {
		while (av_read_frame(ic, pkt) >= 0) { if (pkt->stream_index == si) sub_packet(pkt, ic->streams[si]->time_base.num, ic->streams[si]->time_base.den); av_packet_unref(pkt); }
		av_packet_free(&pkt);
	}
	pthread_mutex_lock(&S.mx); S.external = 1; pthread_mutex_unlock(&S.mx);
	r = 0;
out:
	if (ic) avformat_close_input(&ic);
	if (tmp[0]) unlink(tmp);
	return r;
}

static unsigned signature(double t)
{
	unsigned s = 0;
	for (int i = 0; i < S.n_ev; i++) if (t >= S.ev[i].start && t < S.ev[i].end) s = s * 31 + S.ev[i].id;
	return s;
}

/* Would the subtitle on the screen change if it were drawn for time t (seconds)? */
int sub_due(double t)
{
	int r = 0;
	pthread_mutex_lock(&S.mx);
	if (S.ctx && !isnan(t)) {
		if (S.text) { int chg = 0; if (S.track) a_render(S.ren, S.track, (long long)(t * 1000), &chg); r = chg != 0; }
		else r = signature(t) != S.drawn_sig;
	}
	pthread_mutex_unlock(&S.mx);
	return r;
}

static inline void blend(uint16_t *p, int r, int g, int b, int a)   /* a: 0..255 */
{
	int pr = (*p >> 11) << 3, pg = ((*p >> 5) & 63) << 2, pb = (*p & 31) << 3;
	pr += (r - pr) * a / 255; pg += (g - pg) * a / 255; pb += (b - pb) * a / 255;
	*p = (uint16_t)((pr >> 3) << 11 | (pg >> 2) << 5 | (pb >> 3));
}

/* Shrink one subtitle picture for the place the video has on the screen: every screen pixel
 * is the average of the picture pixels it covers (colour weighted by opacity). */
static void shrink(struct bev *e, int dx, int dy, int dw, int dh, int vw, int vh)
{
	int cw = e->cw > 0 ? e->cw : vw, ch = e->ch > 0 ? e->ch : vh;
	if (cw <= 0 || ch <= 0) return;
	if (e->x + e->w > cw || e->y + e->h > ch) { cw = FFMAX(cw, e->x + e->w); ch = FFMAX(ch, e->y + e->h); }
	av_freep(&e->sc);
	e->kx = dx; e->ky = dy; e->kw = dw; e->kh = dh;
	e->sx = dx + (int)((long long)e->x * dw / cw); e->sy = dy + (int)((long long)e->y * dh / ch);
	e->sw = (int)(((long long)(e->x + e->w) * dw + cw - 1) / cw) - (int)((long long)e->x * dw / cw);
	e->sh = (int)(((long long)(e->y + e->h) * dh + ch - 1) / ch) - (int)((long long)e->y * dh / ch);
	if (e->sw <= 0 || e->sh <= 0 || e->sw > 2 * SCR_W || e->sh > 2 * SCR_H || !(e->sc = av_malloc((size_t)e->sw * e->sh * 4))) { e->sw = e->sh = 0; return; }
	for (int y = 0; y < e->sh; y++) for (int x = 0; x < e->sw; x++) {
		int x0 = x * e->w / e->sw, x1 = (x + 1) * e->w / e->sw, y0 = y * e->h / e->sh, y1 = (y + 1) * e->h / e->sh;
		unsigned long a = 0, r = 0, g = 0, b = 0, n = 0;
		if (x1 <= x0) x1 = x0 + 1;
		if (y1 <= y0) y1 = y0 + 1;
		for (int j = y0; j < y1 && j < e->h; j++) for (int i = x0; i < x1 && i < e->w; i++) {
			uint32_t c = e->argb[j * e->w + i]; unsigned ca = c >> 24;
			a += ca; r += ((c >> 16) & 255) * ca; g += ((c >> 8) & 255) * ca; b += (c & 255) * ca; n++;
		}
		e->sc[y * e->sw + x] = a && n ? (uint32_t)((a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a)) : 0;
	}
}

/* Draw the subtitle for time t over the picture in fb.  dx,dy,dw,dh: where the video
 * picture is on the screen (it may be larger than the screen); vw,vh: its size in the file;
 * lift: text in the lower half is raised by this many lines (the playback bar is showing). */
void sub_draw(uint16_t *fb, double t, int dx, int dy, int dw, int dh, int vw, int vh, int lift)
{
	pthread_mutex_lock(&S.mx);
	if (!S.ctx || isnan(t)) { pthread_mutex_unlock(&S.mx); return; }
	if (S.text) {
		int chg = 0; ASS_Image *im = S.track ? a_render(S.ren, S.track, (long long)(t * 1000), &chg) : NULL;
		for (; im; im = im->next) {
			int r = im->color >> 24, g = (im->color >> 16) & 255, b = (im->color >> 8) & 255, op = 255 - (int)(im->color & 255);
			for (int y = 0; y < im->h; y++) {
				int sy = im->dst_y + y - (im->dst_y > SCR_H / 2 ? lift : 0); const unsigned char *row = im->bitmap + (size_t)y * im->stride;
				if (sy < 0 || sy >= SCR_H) continue;
				for (int x = 0; x < im->w; x++) {
					int sx = im->dst_x + x, a = row[x] * op / 255;
					if (a && sx >= 0 && sx < SCR_W) blend(&fb[sy * SCR_W + sx], r, g, b, a);
				}
			}
		}
	} else {
		for (int i = 0; i < S.n_ev; i++) {
			struct bev *e = &S.ev[i];
			if (t < e->start || t >= e->end) continue;
			if (!e->sc || e->kx != dx || e->ky != dy || e->kw != dw || e->kh != dh) shrink(e, dx, dy, dw, dh, vw, vh);
			for (int y = 0; y < e->sh; y++) {
				int sy = e->sy + y - (e->sy > SCR_H / 2 ? lift : 0);
				if (sy < 0 || sy >= SCR_H) continue;
				for (int x = 0; x < e->sw; x++) {
					int sx = e->sx + x; uint32_t c = e->sc[y * e->sw + x];
					if ((c >> 24) && sx >= 0 && sx < SCR_W) blend(&fb[sy * SCR_W + sx], (c >> 16) & 255, (c >> 8) & 255, c & 255, (int)(c >> 24));
				}
			}
		}
		S.drawn_sig = signature(t);
	}
	pthread_mutex_unlock(&S.mx);
}
