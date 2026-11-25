/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - movie and IPTV player for NimbleZero (M5Stack Cardputer Zero, 320x170).
 *
 * Screens: file selector (two modes, Tab switches) -> channel list (IPTV) -> playback;
 * Options and Help from the selector, Options also during playback.
 *
 * Keys follow the NimbleZero launcher: F/X/Z/C are up/down/left/right without Fn; real
 * arrow keys work as well.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>
#include "nzp.h"

#define C_BAR   NZUI_RGB(30, 70, 170)
#define C_HI    NZUI_RGB(250, 200, 60)
#define C_SEL   NZUI_RGB(40, 40, 60)
#define C_FOOT  NZUI_RGB(25, 25, 25)
#define C_DIR   NZUI_RGB(120, 190, 255)
#define C_RED   NZUI_RGB(230, 60, 60)
#define C_GREEN NZUI_RGB(90, 220, 110)
#define COLS    40
#define LIST_Y  24
#define ROW_H   10
#define ROWS    ((SCR_H - LIST_Y - 12) / ROW_H)

enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_PGUP, NAV_PGDN };
static int nav_key(int k)
{
	switch (k) {
	case KEY_UP: case KEY_F: case KEY_K: return NAV_UP;
	case KEY_DOWN: case KEY_X: case KEY_J: return NAV_DOWN;
	case KEY_LEFT: case KEY_Z: case KEY_PAGEUP: return NAV_PGUP;
	case KEY_RIGHT: case KEY_C: case KEY_PAGEDOWN: return NAV_PGDN;
	}
	return NAV_NONE;
}
static void nav_move(int nav, int *sel, int n)
{
	if (nav == NAV_UP) *sel = *sel > 0 ? *sel - 1 : n - 1;
	else if (nav == NAV_DOWN) *sel = *sel < n - 1 ? *sel + 1 : 0;
	else if (nav == NAV_PGUP) *sel = *sel > ROWS ? *sel - ROWS : 0;
	else if (nav == NAV_PGDN) *sel = *sel + ROWS < n ? *sel + ROWS : n - 1;
	if (*sel < 0) *sel = 0;
}

int ui_scrolling;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---------------- drawing helpers ---------------- */
static void text(int x, int y, uint16_t c, int maxch, const char *s)
{
	char a[160];
	text_ascii(a, sizeof(a), s);
	if (maxch < (int)sizeof(a) && maxch >= 0) a[maxch] = 0;
	nzui_text(x, y, c, "%s", a);
}
/* text that does not fit moves sideways while it is selected */
static void text_scroll(int x, int y, uint16_t c, int maxch, const char *s, int selected)
{
	char a[600]; int n;
	text_ascii(a, sizeof(a), s); n = (int)strlen(a);
	if (n > maxch && selected) {
		ui_scrolling = 1;
		int span = n - maxch + 8, off = (int)(now_s() * 4) % span - 4;
		if (off < 0) off = 0;
		if (off > n - maxch) off = n - maxch;
		text(x, y, c, maxch, a + off);
	} else {
		if (n > maxch) { a[maxch - 1] = '~'; a[maxch] = 0; }
		text(x, y, c, maxch, a);
	}
}
static void dim(int x, int y, int w, int h)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < SCR_H; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < SCR_W; i++) nzui_fb[j * SCR_W + i] = (nzui_fb[j * SCR_W + i] >> 2) & 0x39E7;
}
static void title_bar(const char *left, const char *right)
{
	time_t t = time(NULL); struct tm *tm = localtime(&t);
	nzui_rect(0, 0, SCR_W, 12, C_BAR);
	text(4, 2, NZUI_WHITE, 26, left);
	if (right) text(SCR_W - 8 * 6 - 8 * (int)strlen(right) - 8, 2, NZUI_WHITE, 12, right);
	nzui_text(SCR_W - 8 * 5 - 4, 2, NZUI_WHITE, "%02d:%02d", tm->tm_hour, tm->tm_min);
}
static void footer(const char *s)
{
	nzui_rect(0, SCR_H - 11, SCR_W, 11, C_FOOT);
	text(4, SCR_H - 9, NZUI_GREY, 39, s);
}
static void scrollbar(int top, int n)
{
	if (n <= ROWS) return;
	int h = ROWS * ROW_H, bh = h * ROWS / n < 6 ? 6 : h * ROWS / n, by = LIST_Y + (h - bh) * top / (n - ROWS);
	nzui_rect(SCR_W - 2, LIST_Y, 2, h, C_FOOT); nzui_rect(SCR_W - 2, by, 2, bh, NZUI_GREY);
}
static void fmt_time(char *b, size_t n, double s, int hours)
{
	long t = s < 0 ? 0 : (long)s;
	if (hours) snprintf(b, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	else snprintf(b, n, "%02ld:%02ld", t / 60, t % 60);
}
static void message(const char *title, const char *l1, const char *l2)
{
	struct nzui_event ev; int r;
	nzui_clear(NZUI_BLACK); title_bar(title, NULL);
	text(8, 50, NZUI_WHITE, 38, l1);
	if (l2) text(8, 64, NZUI_GREY, 38, l2);
	footer("Press a key");
	plat_present();
	while ((r = plat_poll(&ev, 500)) >= 0) if (r == 1 && ev.value == 1) break;
}

/* ---------------- options ---------------- */
static const int buf_steps[] = { 0, 500, 1000, 2000, 3000, 5000, 8000, 10000, 15000, 20000, 30000 };
static const int seek_steps[] = { 5, 10, 15, 30, 60, 120 };
static const char *scaler_names[] = { "Fast (rough)", "Bilinear", "Bicubic (sharper)", "Lanczos (sharpest)" };
static const char *aspect_names[] = { "Fit (whole picture)", "Fill (crop edges)", "Stretch" };
static const char *speed_names[] = { "Automatic", "Full quality", "Fast", "Fastest" };

static int step_in(const int *t, int n, int cur, int d)
{
	int i, best = 0;
	for (i = 0; i < n; i++) if (abs(t[i] - cur) < abs(t[best] - cur)) best = i;
	best += d; if (best < 0) best = 0; if (best >= n) best = n - 1;
	return t[best];
}

enum { O_BUFFER, O_RECONNECT, O_HWDEC, O_SCALER, O_ASPECT, O_SPEED, O_SEEK, O_RESUME, O_AVDELAY, O_VOLUME, O_INFO, O_SUBS, O_SUBSIZE, O_COUNT };
static const char *opt_names[O_COUNT] = { "Stream buffer", "Reconnect live streams", "GPU decoding", "Scaling filter", "Picture shape", "Decoding",
	"Seek step", "Resume movies", "Sound timing", "Volume", "Technical overlay", "Subtitles at start", "Subtitle size" };

static void opt_value(int o, char *b, size_t n)
{
	switch (o) {
	case O_BUFFER: if (cfg.buffer_ms) snprintf(b, n, "%.1f s", cfg.buffer_ms / 1000.0); else snprintf(b, n, "Off"); break;
	case O_RECONNECT: snprintf(b, n, "%s", cfg.reconnect ? "On" : "Off"); break;
	case O_HWDEC: snprintf(b, n, "%s", cfg.hwdec ? "Automatic" : "Off"); break;
	case O_SUBS: snprintf(b, n, "%s", cfg.subs ? "On" : "Off"); break;
	case O_SUBSIZE: snprintf(b, n, "%s", cfg.sub_size == 0 ? "Small" : cfg.sub_size == 2 ? "Large" : "Normal"); break;
	case O_SCALER: snprintf(b, n, "%s", scaler_names[cfg.scaler]); break;
	case O_ASPECT: snprintf(b, n, "%s", aspect_names[cfg.aspect]); break;
	case O_SPEED: snprintf(b, n, "%s", speed_names[cfg.speed]); break;
	case O_SEEK: snprintf(b, n, "%d s", cfg.seek_s); break;
	case O_RESUME: snprintf(b, n, "%s", cfg.resume ? "On" : "Off"); break;
	case O_AVDELAY: if (cfg.av_delay_ms) snprintf(b, n, "%+d ms (%s)", cfg.av_delay_ms, cfg.av_delay_ms > 0 ? "later" : "earlier"); else snprintf(b, n, "0 ms"); break;
	case O_VOLUME: snprintf(b, n, "%d %%", cfg.volume); break;
	case O_INFO: snprintf(b, n, "%s", cfg.show_info ? "On" : "Off"); break;
	}
}
static const char *opt_help(int o)
{
	switch (o) {
	case O_BUFFER: return "Network streams: collected before play";
	case O_RECONNECT: return "Reopen a live stream that stops";
	case O_HWDEC: return "H.264 up to 1080p; from the next video";
	case O_SCALER: return "Software decoding only: how it shrinks";
	case O_ASPECT: return "Key A changes this during playback";
	case O_SPEED: return "Software decoding only: short cuts";
	case O_SEEK: return "Seconds per left/right press";
	case O_RESUME: return "Continue a movie where it was stopped";
	case O_AVDELAY: return "Sound against picture; keys J and K";
	case O_VOLUME: return "This player only; system: Options menu";
	case O_INFO: return "Codec, frames, buffer; key I";
	}
	return "";
}
static void opt_change(int o, int d)
{
	switch (o) {
	case O_BUFFER: cfg.buffer_ms = step_in(buf_steps, sizeof(buf_steps) / sizeof(int), cfg.buffer_ms, d); break;
	case O_RECONNECT: cfg.reconnect = !cfg.reconnect; break;
	case O_HWDEC: cfg.hwdec = !cfg.hwdec; break;
	case O_SCALER: cfg.scaler = (cfg.scaler + 4 + d) % 4; break;
	case O_ASPECT: cfg.aspect = (cfg.aspect + 3 + d) % 3; break;
	case O_SPEED: cfg.speed = (cfg.speed + 4 + d) % 4; break;
	case O_SEEK: cfg.seek_s = step_in(seek_steps, sizeof(seek_steps) / sizeof(int), cfg.seek_s, d); break;
	case O_RESUME: cfg.resume = !cfg.resume; break;
	case O_AVDELAY: cfg.av_delay_ms += d * 25; if (cfg.av_delay_ms > 1000) cfg.av_delay_ms = 1000; if (cfg.av_delay_ms < -1000) cfg.av_delay_ms = -1000; break;
	case O_VOLUME: cfg.volume += d * 5; if (cfg.volume > 100) cfg.volume = 100; if (cfg.volume < 0) cfg.volume = 0; break;
	case O_INFO: cfg.show_info = !cfg.show_info; break;
	case O_SUBS: cfg.subs = !cfg.subs; break;
	case O_SUBSIZE: cfg.sub_size = (cfg.sub_size + 3 + d) % 3; break;
	}
}

/* returns -1 if the program should leave */
static int options_screen(void)
{
	static int sel; struct nzui_event ev; int r, top = 0;
	for (;;) {
		char v[40];
		if (sel < top) top = sel;
		if (sel >= top + ROWS) top = sel - ROWS + 1;
		nzui_clear(NZUI_BLACK); title_bar("Options", NULL);
		text(4, 14, NZUI_GREY, 39, opt_help(sel));
		for (int i = 0; i < ROWS && top + i < O_COUNT; i++) {
			int o = top + i, y = LIST_Y + i * ROW_H, s = o == sel;
			if (s) nzui_rect(0, y - 1, SCR_W - 3, ROW_H, C_SEL);
			text(4, y, s ? C_HI : NZUI_WHITE, 22, opt_names[o]);
			opt_value(o, v, sizeof(v));
			text(SCR_W - 6 - 8 * (int)strlen(v), y, s ? C_HI : C_DIR, 20, v);
		}
		scrollbar(top, O_COUNT);
		footer("Z/C or Enter:change  Esc:back");
		plat_present();
		if ((r = plat_poll(&ev, 5000)) < 0) return -1;
		if (r != 1 || !ev.value) continue;
		switch (ev.key) {
		case KEY_UP: case KEY_F: case KEY_K: sel = sel > 0 ? sel - 1 : O_COUNT - 1; break;
		case KEY_DOWN: case KEY_X: case KEY_J: sel = sel < O_COUNT - 1 ? sel + 1 : 0; break;
		case KEY_LEFT: case KEY_Z: opt_change(sel, -1); break;
		case KEY_RIGHT: case KEY_C: case KEY_ENTER: case KEY_SPACE: opt_change(sel, 1); break;
		case KEY_ESC: case KEY_BACKSPACE: case KEY_O: case KEY_Q: if (ev.value == 1) { cfg_save(); return 0; } break;
		}
	}
}

static int help_screen(void)
{
	static const char *l[] = {
		"Lists    F/X up/down    Z/C page",
		"         Enter open     Backspace up",
		"         Tab Movies/IPTV   O options",
		"Playing  Z/C seek       F/X volume",
		"         Space pause    Esc back",
		"         A picture shape   M mute",
		"         N/P next/previous  I details",
		"         T sound track  S subtitles",
		"         J/K sound earlier/later",
		"         1-9 jump to 10%-90%",
		"         Enter show/hide the bar",
		"Arrow keys (Fn) work like F/X/Z/C.",
	};
	struct nzui_event ev; int r;
	nzui_clear(NZUI_BLACK); title_bar("Keys", "v" NZP_VERSION);
	for (unsigned i = 0; i < sizeof(l) / sizeof(l[0]); i++) text(4, 16 + (int)i * 11, i < 3 || i == 11 ? NZUI_WHITE : NZUI_RGB(200, 200, 200), 39, l[i]);
	footer("Press a key");
	plat_present();
	while ((r = plat_poll(&ev, 1000)) >= 0) if (r == 1 && ev.value == 1) return 0;
	return -1;
}

/* ---------------- playback ---------------- */
enum { PLAY_BACK, PLAY_NEXT, PLAY_PREV, PLAY_QUIT };

static void draw_icon(int x, int y, enum pl_state st)
{
	if (st == PL_PAUSED) { nzui_rect(x, y, 3, 8, NZUI_WHITE); nzui_rect(x + 5, y, 3, 8, NZUI_WHITE); }
	else if (st == PL_PLAYING) for (int i = 0; i < 4; i++) nzui_rect(x + i * 2, y + i, 2, 8 - i * 2, NZUI_WHITE);
	else nzui_rect(x + 1, y + 1, 6, 6, C_HI);
}

static void draw_osd(const char *title, enum pl_state st, const struct pl_info *in, double pos, double dur, double live_since, int reconnects)
{
	char a[24], b[24];
	dim(0, SCR_H - 30, SCR_W, 30);
	draw_icon(4, SCR_H - 27, st);
	text_scroll(18, SCR_H - 27, NZUI_WHITE, 37, title, 1);
	if (dur > 0) {
		int hours = dur >= 3600, x0, x1, fill;
		fmt_time(a, sizeof(a), pos, hours); fmt_time(b, sizeof(b), dur, hours);
		x0 = 4 + 8 * (int)strlen(a) + 6; x1 = SCR_W - 4 - 8 * (int)strlen(b) - 6;
		nzui_text(4, SCR_H - 13, NZUI_WHITE, "%s", a); nzui_text(x1 + 6, SCR_H - 13, NZUI_WHITE, "%s", b);
		nzui_rect(x0, SCR_H - 11, x1 - x0, 4, NZUI_RGB(70, 70, 70));
		fill = (int)((x1 - x0) * (pos / dur)); if (fill > x1 - x0) fill = x1 - x0;
		nzui_rect(x0, SCR_H - 11, fill, 4, C_HI); nzui_rect(x0 + fill - 1, SCR_H - 13, 3, 8, NZUI_WHITE);
	} else {
		int bw = 80, x0 = SCR_W - 4 - bw, fill;
		nzui_rect(4, SCR_H - 14, 36, 10, C_RED); nzui_text(6, SCR_H - 13, NZUI_WHITE, "LIVE");
		fmt_time(a, sizeof(a), now_s() - live_since, now_s() - live_since >= 3600);
		nzui_text(46, SCR_H - 13, NZUI_WHITE, "%s", a);
		if (reconnects) nzui_text(46 + 8 * ((int)strlen(a) + 1), SCR_H - 13, NZUI_GREY, "r%d", reconnects);
		if (in->net && in->buffer_need > 0) {      /* how full the stream buffer is */
			double f = in->buffered / in->buffer_need; if (f > 1) f = 1;
			fill = (int)(bw * f);
			nzui_text(x0 - 8 * 9, SCR_H - 13, NZUI_GREY, "buf %3.1fs", in->buffered > 99 ? 99.0 : in->buffered);
			nzui_rect(x0, SCR_H - 11, bw, 4, NZUI_RGB(70, 70, 70)); nzui_rect(x0, SCR_H - 11, fill, 4, f < 0.3 ? C_RED : C_GREEN);
		}
	}
}

static void draw_box(const char *l1, const char *l2)
{
	int w = 8 * 30, x = (SCR_W - w) / 2, y = 60;
	dim(x, y, w, l2 ? 36 : 24);
	text(x + (w - 8 * (int)(strlen(l1) > 28 ? 28 : strlen(l1))) / 2, y + 8, NZUI_WHITE, 28, l1);
	if (l2) text(x + (w - 8 * (int)(strlen(l2) > 28 ? 28 : strlen(l2))) / 2, y + 20, NZUI_GREY, 28, l2);
}

static void draw_info(const struct pl_info *in)
{
	static unsigned long last_shown, last_dec; static double last_t, fps_out, fps_dec, gap_max, gap_show, pres_avg, pres_max, last_call;
	static const char *lv[] = { "full quality", "less deblocking", "no deblocking", "no deblock, B-frames", "key frames only" };
	double t = now_s(); int y = 2;
	/* longest time between two passes of the screen loop: a stall here costs pictures */
	if (last_call > 0 && t - last_call > gap_max && t - last_call < 5) gap_max = t - last_call;
	last_call = t;
	if (t - last_t >= 1.0 || in->shown < last_shown) {
		if (last_t > 0 && in->shown >= last_shown) { fps_out = (in->shown - last_shown) / (t - last_t); fps_dec = (in->decoded - last_dec) / (t - last_t); }
		last_shown = in->shown; last_dec = in->decoded; last_t = t;
		plat_present_stats(&pres_avg, &pres_max); gap_show = gap_max * 1000; gap_max = 0;
	}
	dim(0, 0, 8 * 35, 6 + 9 * 7);
	if (in->has_video) {
		if (in->v_kbps >= 1000) nzui_text(4, y, NZUI_WHITE, "%s %dx%d %.4g fps %.1f Mb/s", in->vcodec, in->w, in->h, in->fps, in->v_kbps / 1000);
		else if (in->v_kbps > 0) nzui_text(4, y, NZUI_WHITE, "%s %dx%d %.4g fps %.0f kb/s", in->vcodec, in->w, in->h, in->fps, in->v_kbps);
		else nzui_text(4, y, NZUI_WHITE, "%s %dx%d %.4g fps", in->vcodec, in->w, in->h, in->fps);
		y += 9;
	}
	if (in->has_audio) nzui_text(4, y, NZUI_WHITE, "%s %d Hz %d ch  out:%s", in->acodec, in->rate, in->channels, in->sink), y += 9;
	if (in->has_video) {
		nzui_text(4, y, NZUI_WHITE, "shown %.0f/s decoded %.0f/s", fps_out, fps_dec), y += 9;
		nzui_text(4, y, in->dropped ? C_HI : NZUI_WHITE, "skipped: late %lu, screen %lu", in->dropped_late, in->dropped_disp), y += 9;
		nzui_text(4, y, NZUI_WHITE, "decoding: %s", in->hw ? "GPU (and scaling)" : lv[in->level > 4 ? 4 : in->level]), y += 9;
		nzui_text(4, y, NZUI_WHITE, "draw %.1f/%.1f ms  loop %.0f ms", pres_avg, pres_max, gap_show), y += 9;
	}
	nzui_text(4, y, NZUI_WHITE, "buffer %.1f s  screen:%s", in->buffered, plat_output());
}

/* Play one item.  local_path is set for a file of the user (resume applies). */
static int play(const char *url, const char *title, const char *ua, const char *ref, const char *local_path)
{
	static uint16_t vframe[SCR_W * SCR_H];
	struct nzui_event ev; struct pl_info in; enum pl_state st;
	double osd_until, toast_until = 0, last_draw = 0, opened_at = 0, live_since = now_s(), retry_at = 0, start = 0;
	char toast[40] = ""; int result = PLAY_BACK, muted_vol = -1, osd_pinned = 0, reconnects = 0, played = 0, fails = 0, wait = 20, resumed = 0;

	if (local_path && cfg.resume) start = resume_get(local_path);
	memset(vframe, 0, sizeof(vframe));
	plat_allow_idle(0);
	pl_open(url, ua, ref, start);
	osd_until = now_s() + 4;
	if (start > 1) { char t[16]; fmt_time(t, sizeof(t), start, start >= 3600); snprintf(toast, sizeof(toast), "Resumed at %s  0:restart", t); toast_until = now_s() + 4; resumed = 1; }
#define TOAST(...) do { snprintf(toast, sizeof(toast), __VA_ARGS__); toast_until = now_s() + 1.8; } while (0)

	int hw_fails_seen = 0;
	for (;;) {
		int changed, r; double t, pos, dur;
		st = pl_state();
		changed = pl_refresh(vframe, &wait);
		t = now_s();
		pl_info(&in);
		if (in.hw_fails > hw_fails_seen) { TOAST("GPU decoder failed: software"); toast_until = now_s() + 5.0; }
		hw_fails_seen = in.hw_fails;
		if (st == PL_PLAYING) { if (!opened_at) opened_at = t; if (t - opened_at > 20) { played = 1; fails = 0; } }

		/* a live stream that stopped: open it again (a few times) */
		if ((st == PL_ERROR || st == PL_ENDED) && !retry_at) {
			int live = st == PL_ERROR ? is_url(url) && !local_path : in.live && in.net;
			if (live && cfg.reconnect && fails < (played ? 5 : 1)) { fails++; retry_at = t + 1.5; }
			else if (st == PL_ERROR) { char e[160]; snprintf(e, sizeof(e), "%s", pl_error()); pl_close(); plat_allow_idle(1); message("Cannot play", title, e); return PLAY_BACK; }
			else { if (local_path) resume_set(local_path, 0); result = PLAY_BACK; break; }
		}
		if (retry_at && t >= retry_at) { retry_at = 0; opened_at = 0; reconnects++; pl_close(); pl_open(url, ua, ref, 0); continue; }

		pos = pl_position(); dur = pl_duration();
		if (changed || t - last_draw > 0.2 || pl_sub_due()) {
			int bar = osd_pinned || t < osd_until || st != PL_PLAYING || !in.has_video;
			memcpy(nzui_fb, vframe, sizeof(vframe));
			pl_sub_draw(nzui_fb, bar && in.has_video ? 30 : 0);   /* above the bar while it is shown */
			if (!in.has_video && in.has_audio && st != PL_OPENING && st != PL_ERROR) {
				nzui_clear(NZUI_BLACK);
				for (int i = 0; i < 5; i++) {      /* a few bars so the screen is not empty */
					int h = 8 + (int)((i * 37 + (long)(t * 6) * (i + 3)) % 30) * (st == PL_PLAYING);
					nzui_rect(130 + i * 13, 90 - h, 9, h, C_BAR);
				}
				text(SCR_W / 2 - 8 * 5, 100, NZUI_GREY, 12, "Sound only");
			}
			if (retry_at) draw_box(played ? "Stream stopped" : "No answer yet", played ? "Reconnecting..." : "Trying once more...");
			else if (st == PL_OPENING) draw_box("Opening...", "Esc: cancel");
			else if (st == PL_BUFFERING && in.net && in.buffer_need > 0.2) {
				char b[32]; double f = in.buffered / in.buffer_need;
				snprintf(b, sizeof(b), "Buffering %d%%", f > 1 ? 100 : (int)(f * 100));
				draw_box(b, NULL);
			}
			if (cfg.show_info && st != PL_OPENING) draw_info(&in);
			if (t < toast_until) { int w = 8 * (int)strlen(toast) + 8; dim(SCR_W - w - 2, cfg.show_info ? 72 : 2, w, 12); text(SCR_W - w + 2, cfg.show_info ? 74 : 4, C_HI, 39, toast); }
			if (bar) draw_osd(title, retry_at ? PL_BUFFERING : st, &in, pos, dur, live_since, reconnects);
			plat_present();
			last_draw = t;
		}

		plat_allow_idle(st == PL_PAUSED);
		r = plat_poll(&ev, wait < 1 ? 1 : wait > 100 ? 100 : wait);
		if (r < 0) { result = PLAY_QUIT; break; }
		if (r != 1 || !ev.value) continue;
		osd_until = now_s() + 3; last_draw = 0;
		switch (ev.key) {
		case KEY_ESC: case KEY_BACKSPACE: case KEY_Q: if (ev.value == 1) goto leave; break;
		case KEY_SPACE: if (ev.value == 1 && st != PL_OPENING) pl_pause(!pl_paused()); break;
		case KEY_LEFT: case KEY_Z: case KEY_RIGHT: case KEY_C:
			if (pl_seek_rel((ev.key == KEY_LEFT || ev.key == KEY_Z) ? -cfg.seek_s : cfg.seek_s) < 0 && st != PL_OPENING) TOAST("Live: cannot seek");
			break;
		case KEY_UP: case KEY_F: case KEY_DOWN: case KEY_X:
			cfg.volume += (ev.key == KEY_UP || ev.key == KEY_F) ? 5 : -5;
			if (cfg.volume > 100) cfg.volume = 100;
			if (cfg.volume < 0) cfg.volume = 0;
			muted_vol = -1; TOAST("Volume %d%%", cfg.volume);
			break;
		case KEY_M: if (ev.value == 1) { if (muted_vol >= 0) { cfg.volume = muted_vol; muted_vol = -1; TOAST("Volume %d%%", cfg.volume); } else { muted_vol = cfg.volume; cfg.volume = 0; TOAST("Muted"); } } break;
		case KEY_A: if (ev.value == 1) { cfg.aspect = (cfg.aspect + 1) % 3; TOAST("%s", aspect_names[cfg.aspect]); } break;
		case KEY_I: if (ev.value == 1) cfg.show_info = !cfg.show_info; break;
		case KEY_T: if (ev.value == 1) { int n = pl_cycle_audio(); if (n) { struct pl_info i2; pl_info(&i2); if (i2.alang[0]) TOAST("Sound track %d of %d (%s)", n, in.n_audio, i2.alang); else TOAST("Sound track %d of %d", n, in.n_audio); } else TOAST("One sound track"); } break;
		case KEY_S: if (ev.value == 1) { char nm[40]; int n = pl_cycle_subs(nm, sizeof(nm)); if (n < 0) TOAST("No subtitles"); else if (!n) TOAST("Subtitles off"); else if (nm[0]) TOAST("Subtitles %d: %s", n, nm); else TOAST("Subtitles %d", n); } break;
		case KEY_J: case KEY_K: case KEY_COMMA: case KEY_DOT:
			cfg.av_delay_ms += ev.key == KEY_DOT || ev.key == KEY_K ? 25 : -25;
			if (cfg.av_delay_ms > 1000) cfg.av_delay_ms = 1000;
			if (cfg.av_delay_ms < -1000) cfg.av_delay_ms = -1000;
			TOAST("Sound %+d ms", cfg.av_delay_ms);
			break;
		case KEY_N: if (ev.value == 1) { result = PLAY_NEXT; goto leave; } break;
		case KEY_P: if (ev.value == 1) { result = PLAY_PREV; goto leave; } break;
		case KEY_ENTER: if (ev.value == 1) { osd_pinned = !osd_pinned; if (!osd_pinned) osd_until = 0; } break;
		case KEY_O:
			if (ev.value == 1) {
				int was = pl_paused();
				pl_pause(1);
				if (options_screen() < 0) { result = PLAY_QUIT; goto leave; }
				pl_pause(was);
			}
			break;
		case KEY_0: if (ev.value == 1 && (resumed || in.seekable)) { pl_seek_abs(0); resumed = 0; } break;
		default:
			if (ev.value == 1 && ev.key >= KEY_1 && ev.key <= KEY_9 && dur > 0) pl_seek_abs(dur * (ev.key - KEY_1 + 1) / 10.0);
			break;
		}
	}
leave:
	if (local_path && cfg.resume) {
		double pos = pl_position(), dur = pl_duration();
		st = pl_state();
		if (st != PL_OPENING && st != PL_ERROR && dur > 60) resume_set(local_path, st == PL_ENDED || pos > dur - 15 || pos < 5 ? 0 : pos - 2);
	}
	pl_close();
	cfg_save();
	plat_allow_idle(1);
	return result;
#undef TOAST
}

/* ---------------- channel list ---------------- */
static int channels_screen(const char *path)
{
	struct m3u m; struct nzui_event ev; int r, *view, nview = 0, group = -1 /* -1 group list, -2 all, >=0 one group */, gsel = 0, sel = 0, top = 0;
	const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
	r = m3u_load(path, &m);
	if (r == 1) { r = play(path, base, NULL, NULL, NULL); return r == PLAY_QUIT ? -1 : 0; }   /* one HLS stream */
	if (r < 0 || !m.n) { m3u_free(&m); message("IPTV", base, r < 0 ? "The file cannot be read." : "No channels found in this file."); return 0; }
	if (!(view = malloc((size_t)m.n * sizeof(int)))) { m3u_free(&m); return 0; }
	if (m.ngroups < 2) group = -2;
	for (;;) {
		int n, in_groups = group == -1, *cur = in_groups ? &gsel : &sel; char hdr[80];
		if (in_groups) n = m.ngroups + 1;
		else {
			nview = 0;
			for (int i = 0; i < m.n; i++) if (group == -2 || (m.c[i].group && !strcmp(m.c[i].group, m.groups[group]))) view[nview++] = i;
			n = nview;
		}
		if (*cur >= n) *cur = n ? n - 1 : 0;
		if (*cur < top) top = *cur;
		if (*cur >= top + ROWS) top = *cur - ROWS + 1;
		ui_scrolling = 0;
		nzui_clear(NZUI_BLACK); title_bar(base, NULL);
		if (in_groups) snprintf(hdr, sizeof(hdr), "%d groups, %d channels", m.ngroups, m.n);
		else snprintf(hdr, sizeof(hdr), "%.28s (%d)", group == -2 ? "All channels" : m.groups[group], n);
		text(4, 14, NZUI_GREY, 39, hdr);
		for (int i = 0; i < ROWS && top + i < n; i++) {
			int k = top + i, y = LIST_Y + i * ROW_H, s = k == *cur; char num[16];
			if (s) nzui_rect(0, y - 1, SCR_W - 3, ROW_H, C_SEL);
			if (in_groups) text_scroll(4, y, s ? C_HI : C_DIR, 38, k == 0 ? "All channels" : m.groups[k - 1], s);
			else {
				snprintf(num, sizeof(num), "%4d", k + 1);
				nzui_text(0, y, NZUI_GREY, "%s", num);
				text_scroll(38, y, s ? C_HI : NZUI_WHITE, 34, m.c[view[k]].name, s);
			}
		}
		scrollbar(top, n);
		footer(in_groups ? "Enter:open  Esc:back" : "Enter:play  Esc:back  Z/C:page");
		plat_present();
		if ((r = plat_poll(&ev, ui_scrolling ? 250 : 5000)) < 0) { free(view); m3u_free(&m); return -1; }
		if (r != 1 || !ev.value) continue;
		if (nav_key(ev.key)) { nav_move(nav_key(ev.key), cur, n); continue; }
		if (ev.value != 1) continue;
		if (ev.key == KEY_ENTER && n) {
			if (in_groups) { group = gsel == 0 ? -2 : gsel - 1; sel = 0; top = 0; }
			else for (;;) {                      /* N / P during playback move through the list shown */
				struct chan *c = &m.c[view[sel]];
				r = play(c->url, c->name, c->ua, c->ref, NULL);
				if (r == PLAY_QUIT) { free(view); m3u_free(&m); return -1; }
				if (r == PLAY_NEXT) sel = (sel + 1) % nview;
				else if (r == PLAY_PREV) sel = (sel + nview - 1) % nview;
				else break;
			}
		} else if (ev.key == KEY_ESC || ev.key == KEY_BACKSPACE) {
			if (!in_groups && m.ngroups >= 2) { group = -1; top = 0; }
			else break;
		} else if (ev.key == KEY_O) { if (options_screen() < 0) { free(view); m3u_free(&m); return -1; } }
		else if (ev.key == KEY_H) { if (help_screen() < 0) { free(view); m3u_free(&m); return -1; } }
	}
	free(view); m3u_free(&m);
	return 0;
}

/* ---------------- file selector ---------------- */
struct ent { char *name; int dir; long long size; };
static struct browser { char dir[512]; struct ent *e; int n, sel, top; } br[2];
static int mode;                             /* 0 movies, 1 IPTV */

static int has_ext(const char *name, const char *list)
{
	const char *dot = strrchr(name, '.'); size_t n;
	if (!dot || !dot[1]) return 0;
	n = strlen(dot + 1);
	for (const char *p = list; *p; ) {
		const char *e = strchr(p, ' '); size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == n && !strncasecmp(p, dot + 1, n)) return 1;
		p += l; while (*p == ' ') p++;
	}
	return 0;
}
static int wanted(const char *name, int m)
{
	return has_ext(name, m ? "m3u m3u8" :
		"mp4 m4v mkv webm avi mov mpg mpeg m2v ts m2ts mts wmv flv f4v 3gp ogv vob divx asf rm rmvb mxf dv nut "
		"mp3 mp2 m4a m4b aac flac ogg oga opus wav aif aiff wma ac3 mka amr ape wv");
}
static int ent_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}
static void br_free(struct browser *b) { for (int i = 0; i < b->n; i++) free(b->e[i].name); free(b->e); b->e = NULL; b->n = 0; }

static void br_scan(struct browser *b, int m, const char *select)
{
	DIR *d; struct dirent *de; int alloc = 0;
	br_free(b);
	if (!(d = opendir(b->dir))) { snprintf(b->dir, sizeof(b->dir), "/"); d = opendir(b->dir); }
	while (d && (de = readdir(d))) {
		char p[1024]; struct stat st; struct ent e;
		if (de->d_name[0] == '.') continue;
		snprintf(p, sizeof(p), "%s/%s", b->dir, de->d_name);
		if (stat(p, &st)) continue;
		e.dir = S_ISDIR(st.st_mode); e.size = st.st_size;
		if (!e.dir && (!S_ISREG(st.st_mode) || !wanted(de->d_name, m))) continue;
		if (b->n == alloc) { struct ent *ne = realloc(b->e, (size_t)(alloc = alloc ? alloc * 2 : 64) * sizeof(*ne)); if (!ne) break; b->e = ne; }
		if (!(e.name = strdup(de->d_name))) break;
		b->e[b->n++] = e;
	}
	if (d) closedir(d);
	if (b->n) qsort(b->e, (size_t)b->n, sizeof(b->e[0]), ent_cmp);
	b->sel = b->top = 0;
	for (int i = 0; select && i < b->n; i++) if (!strcmp(b->e[i].name, select)) b->sel = i;
}

static void br_up(struct browser *b, int m)
{
	char was[256]; char *s = strrchr(b->dir, '/');
	if (!s || !strcmp(b->dir, "/")) return;
	snprintf(was, sizeof(was), "%s", s + 1);
	if (s == b->dir) s[1] = 0; else *s = 0;
	br_scan(b, m, was);
}

static void fmt_size(char *o, size_t n, long long s)
{
	if (s >= 1LL << 30) snprintf(o, n, "%.1fG", s / (double)(1LL << 30));
	else if (s >= 1 << 20) snprintf(o, n, "%lldM", s >> 20);
	else snprintf(o, n, "%lldK", (s + 1023) >> 10);
}

static void draw_browser(double quit_hint_until)
{
	struct browser *b = &br[mode]; char p[600]; int n;
	ui_scrolling = 0;
	nzui_clear(NZUI_BLACK);
	nzui_rect(0, 0, SCR_W, 12, C_BAR);
	{   /* two tabs */
		time_t t = time(NULL); struct tm *tm = localtime(&t);
		nzui_rect(mode ? 62 : 2, 1, mode ? 44 : 56, 10, NZUI_BLACK);
		nzui_text(6, 2, mode ? NZUI_RGB(170, 190, 230) : C_HI, "Movies"); nzui_text(68, 2, mode ? C_HI : NZUI_RGB(170, 190, 230), "IPTV");
		nzui_text(SCR_W - 8 * 5 - 4, 2, NZUI_WHITE, "%02d:%02d", tm->tm_hour, tm->tm_min);
	}
	text_ascii(p, sizeof(p), b->dir); n = (int)strlen(p);
	text(4, 14, NZUI_GREY, 39, n > 39 ? p + n - 39 : p);
	if (b->sel < b->top) b->top = b->sel;
	if (b->sel >= b->top + ROWS) b->top = b->sel - ROWS + 1;
	for (int i = 0; i < ROWS && b->top + i < b->n; i++) {
		struct ent *e = &b->e[b->top + i]; int y = LIST_Y + i * ROW_H, s = b->top + i == b->sel; char sz[12] = "";
		if (s) nzui_rect(0, y - 1, SCR_W - 3, ROW_H, C_SEL);
		if (e->dir) { nzui_text(4, y, C_DIR, "/"); text_scroll(12, y, s ? C_HI : C_DIR, 37, e->name, s); }
		else {
			fmt_size(sz, sizeof(sz), e->size);
			text_scroll(4, y, s ? C_HI : NZUI_WHITE, 38 - (int)strlen(sz) - 1, e->name, s);
			nzui_text(SCR_W - 6 - 8 * (int)strlen(sz), y, NZUI_GREY, "%s", sz);
		}
	}
	if (!b->n) {
		text(8, 50, NZUI_GREY, 38, mode ? "No .m3u channel lists in this folder." : "No movies in this folder.");
		text(8, 64, NZUI_GREY, 38, "Copy files here, e.g. with scp, or");
		text(8, 76, NZUI_GREY, 38, "press Backspace for the folder above.");
	}
	scrollbar(b->top, b->n);
	footer(now_s() < quit_hint_until ? "Press Esc again to leave" : mode ? "Enter:open Tab:Movies O:options H:keys" : "Enter:play Tab:IPTV O:options H:keys");
	plat_present();
}

int main(int argc, char **argv)
{
	struct nzui_event ev; int r; double quit_hint = 0; const char *home = getenv("HOME");
	if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) { printf("nz-player %s\n", NZP_VERSION); return 0; }
	cfg_load();
	{   /* a file named "debug" in the settings folder turns on extra lines in the log */
		char p[700]; const char *d = getenv("NZP_CONFIG_DIR");
		if (d) snprintf(p, sizeof(p), "%s/debug", d); else snprintf(p, sizeof(p), "%s/.config/nz-player/debug", home ? home : "/tmp");
		if (!access(p, F_OK)) setenv("NZP_DEBUG", "1", 0);
	}
	fprintf(stderr, "nz-player %s\n", NZP_VERSION);
	for (int m = 0; m < 2; m++) {
		struct stat st;
		snprintf(br[m].dir, sizeof(br[m].dir), "%s", cfg.dir[m]);
		if (!br[m].dir[0] || stat(br[m].dir, &st) || !S_ISDIR(st.st_mode)) snprintf(br[m].dir, sizeof(br[m].dir), "%s", home && *home ? home : "/");
		if (strlen(br[m].dir) > 1 && br[m].dir[strlen(br[m].dir) - 1] == '/') br[m].dir[strlen(br[m].dir) - 1] = 0;
	}
	if (plat_init()) { fprintf(stderr, "nz-player: no display\n"); return 1; }
	plat_allow_idle(1);
	if (argc > 1) {                          /* nz-player FILE-or-URL: play it, then leave */
		const char *b = strrchr(argv[1], '/');
		if (has_ext(argv[1], "m3u") || (has_ext(argv[1], "m3u8") && !is_url(argv[1]))) channels_screen(argv[1]);
		else play(argv[1], b && b[1] ? b + 1 : argv[1], NULL, NULL, is_url(argv[1]) ? NULL : argv[1]);
		plat_quit();
		return 0;
	}
	br_scan(&br[0], 0, NULL); br_scan(&br[1], 1, NULL);
	for (;;) {
		struct browser *b = &br[mode];
		draw_browser(quit_hint);
		if ((r = plat_poll(&ev, ui_scrolling ? 250 : now_s() < quit_hint ? 2600 : 5000)) < 0) break;
		if (r != 1 || !ev.value) continue;
		if (ev.key != KEY_ESC) quit_hint = 0;
		if (nav_key(ev.key)) { if (b->n) nav_move(nav_key(ev.key), &b->sel, b->n); continue; }
		if (ev.value != 1) continue;
		if (ev.key == KEY_TAB) mode = !mode;
		else if (ev.key == KEY_BACKSPACE) br_up(b, mode);
		else if (ev.key == KEY_R) { char *keep = b->n ? strdup(b->e[b->sel].name) : NULL; br_scan(b, mode, keep); free(keep); }
		else if (ev.key == KEY_O) { if (options_screen() < 0) break; }
		else if (ev.key == KEY_H) { if (help_screen() < 0) break; }
		else if (ev.key == KEY_Q) break;
		else if (ev.key == KEY_ESC) { if (now_s() < quit_hint) break; quit_hint = now_s() + 2.5; }
		else if (ev.key == KEY_ENTER && b->n) {
			struct ent *e = &b->e[b->sel]; char p[1024];
			snprintf(p, sizeof(p), "%s%s%s", b->dir, strcmp(b->dir, "/") ? "/" : "", e->name);
			if (e->dir) { snprintf(b->dir, sizeof(b->dir), "%s", p); br_scan(b, mode, NULL); }
			else {
				snprintf(cfg.dir[mode], sizeof(cfg.dir[mode]), "%s", b->dir);
				if (mode) { if (channels_screen(p) < 0) break; }
				else for (;;) {                  /* N / P during playback: the next / previous file of the folder */
					int k = b->sel, res;
					e = &b->e[b->sel];
					snprintf(p, sizeof(p), "%s%s%s", b->dir, strcmp(b->dir, "/") ? "/" : "", e->name);
					res = play(p, e->name, NULL, NULL, p);
					if (res == PLAY_QUIT) goto out;
					if (res != PLAY_NEXT && res != PLAY_PREV) break;
					do k = (k + (res == PLAY_NEXT ? 1 : b->n - 1)) % b->n; while (b->e[k].dir && k != b->sel);
					if (k == b->sel) break;
					b->sel = k;
				}
				cfg_save();
			}
		}
	}
out:
	cfg_save();
	plat_quit();
	return 0;
}
