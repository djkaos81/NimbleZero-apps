/* SPDX-License-Identifier: GPL-2.0-only
 * nz-image - picture viewer for NimbleZero (M5Stack Cardputer Zero).
 *
 *   nz-image            choose a picture
 *   nz-image FILE       show FILE; Z / C go to the other pictures of its folder
 *
 * JPEG, PNG, BMP, GIF (first picture of an animation), PPM/PGM; decoded by stb_image
 * (public domain, src/stb_image.h).  A picture larger than the screen is reduced by
 * averaging all its pixels, not by leaving pixels out, and the 16-bit screen gets an
 * ordered dither, so that photos and gradients look right on 320x170.
 *
 * Look and keys: nzui_kit.h and docs/STYLE.md of NimbleZero.
 * Tests: NZI_HEADLESS, NZI_KEYS, NZI_CONFIG_DIR (see nzui_app.h).
 */
#define _GNU_SOURCE
#define NZUI_IMPLEMENTATION
#define NZUI_KIT_IMPLEMENTATION
#define NZUI_APP_IMPLEMENTATION
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>
#include "nzui_app.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_PNM
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define VERSION    "0.1.0"
#define MAX_PIXELS (32L * 1000 * 1000)
#define EXTS       "jpg jpeg jpe png bmp gif ppm pgm pnm"

static unsigned char *img; static int iw, ih;           /* the picture as RGB, already turned upright */
static double zoom, cxp, cyp;                           /* 0: whole picture; else screen pixels per picture pixel, and the centre */
static int bars = 1, src_w, src_h; static long long file_size; static time_t file_time;
static char dir[PATH_MAX], **names; static int nnames, cur;
static char why[64];

static int has_ext(const char *name)
{
	const char *dot = strrchr(name, '.'); size_t n;
	if (!dot || !dot[1]) return 0;
	n = strlen(dot + 1);
	for (const char *p = EXTS; *p; ) {
		const char *e = strchr(p, ' '); size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == n && !strncasecmp(p, dot + 1, n)) return 1;
		p += l; while (*p == ' ') p++;
	}
	return 0;
}
static int name_cmp(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	return strcasecmp(x, y) ? strcasecmp(x, y) : strcmp(x, y);
}
static void names_free(void) { for (int i = 0; i < nnames; i++) free(names[i]); free(names); names = NULL; nnames = 0; }
static void names_scan(const char *select)
{
	DIR *d = opendir(dir); struct dirent *e; int alloc = 0;
	names_free(); cur = 0;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || !has_ext(e->d_name)) continue;
		if (nnames == alloc) { char **n = realloc(names, (size_t)(alloc = alloc ? alloc * 2 : 64) * sizeof(*n)); if (!n) break; names = n; }
		if (!(names[nnames] = strdup(e->d_name))) break;
		nnames++;
	}
	if (d) closedir(d);
	if (nnames) qsort(names, (size_t)nnames, sizeof(names[0]), name_cmp);
	for (int i = 0; select && i < nnames; i++) if (!strcmp(names[i], select)) cur = i;
}

/* ---------------- loading ---------------- */
/* How a JPEG from a camera wants to be turned: the Orientation entry of its Exif block, 1-8; 1 if none. */
static int exif_orientation(const char *path)
{
	FILE *f = fopen(path, "rb"); unsigned char b[65536]; int o = 1; size_t n;
	if (!f) return 1;
	n = fread(b, 1, sizeof(b), f); fclose(f);
	if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return 1;
	for (size_t p = 2; p + 4 <= n; ) {
		unsigned len;
		if (b[p] != 0xFF) break;
		len = (unsigned)b[p + 2] << 8 | b[p + 3];
		if (b[p + 1] == 0xE1 && p + 4 + 14 <= n && !memcmp(b + p + 4, "Exif\0\0", 6)) {
			const unsigned char *t = b + p + 10; size_t tn = p + 2 + len <= n ? len - 8 : n - p - 10; int le = t[0] == 'I';
#define RD16(q) (le ? (unsigned)(q)[0] | (unsigned)(q)[1] << 8 : (unsigned)(q)[1] | (unsigned)(q)[0] << 8)
#define RD32(q) (le ? RD16(q) | RD16((q) + 2) << 16 : RD16((q) + 2) | RD16(q) << 16)
			if (tn >= 8) {
				unsigned ifd = RD32(t + 4);
				if (ifd + 2 <= tn) {
					unsigned cnt = RD16(t + ifd);
					for (unsigned i = 0; i < cnt && ifd + 2 + (i + 1) * 12 <= tn; i++) {
						const unsigned char *e = t + ifd + 2 + i * 12;
						if (RD16(e) == 0x0112) { unsigned v = RD16(e + 8); if (v >= 1 && v <= 8) o = (int)v; }
					}
				}
			}
			break;
		}
		if (b[p + 1] == 0xDA || len < 2) break;                 /* picture data starts: no Exif */
		p += 2 + len;
	}
	return o;
}
static void flip_h(void)
{
	for (int y = 0; y < ih; y++) for (int x = 0; x < iw / 2; x++) {
		unsigned char *a = img + ((size_t)y * iw + x) * 3, *b = img + ((size_t)y * iw + iw - 1 - x) * 3, t[3];
		memcpy(t, a, 3); memcpy(a, b, 3); memcpy(b, t, 3);
	}
}
static void turn_180(void)
{
	size_t n = (size_t)iw * ih;
	for (size_t i = 0; i < n / 2; i++) { unsigned char *a = img + i * 3, *b = img + (n - 1 - i) * 3, t[3]; memcpy(t, a, 3); memcpy(a, b, 3); memcpy(b, t, 3); }
}
static int turn_90(void)                /* clockwise; 0 ok, -1 no memory (the picture stays as it is) */
{
	unsigned char *n = malloc((size_t)iw * ih * 3); int t;
	if (!n) return -1;
	for (int y = 0; y < ih; y++) {
		const unsigned char *s = img + (size_t)y * iw * 3;
		for (int x = 0; x < iw; x++, s += 3) memcpy(n + ((size_t)x * ih + (ih - 1 - y)) * 3, s, 3);
	}
	free(img); img = n; t = iw; iw = ih; ih = t;
	return 0;
}
static int load(const char *path)       /* 0 ok; else `why` says what is wrong */
{
	int w, h, comp, o; struct stat st; unsigned char *p;
	free(img); img = NULL; iw = ih = 0; zoom = 0; why[0] = 0;
	if (stat(path, &st)) { snprintf(why, sizeof(why), "%.60s", strerror(errno)); return -1; }
	file_size = st.st_size; file_time = st.st_mtime;
	if (!stbi_info(path, &w, &h, &comp)) { snprintf(why, sizeof(why), "The file is damaged or of another kind"); return -1; }
	if ((long)w * h > MAX_PIXELS) { snprintf(why, sizeof(why), "Too large: %d x %d (%ld megapixels)", w, h, (long)w * h / 1000000); return -1; }
	src_w = w; src_h = h;
	if (comp == 2 || comp == 4) {           /* see-through parts: on a grey chequerboard */
		if (!(p = stbi_load(path, &w, &h, &comp, 4))) { snprintf(why, sizeof(why), "%.60s", stbi_failure_reason()); return -1; }
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			unsigned char *s = p + ((size_t)y * w + x) * 4, *d = p + ((size_t)y * w + x) * 3; int a = s[3], bg = ((x >> 3) ^ (y >> 3)) & 1 ? 70 : 45;
			int r = (s[0] * a + bg * (255 - a)) / 255, g = (s[1] * a + bg * (255 - a)) / 255, b = (s[2] * a + bg * (255 - a)) / 255;
			d[0] = (unsigned char)r; d[1] = (unsigned char)g; d[2] = (unsigned char)b;
		}
		img = p;                                /* the fourth byte of each pixel is no longer used */
	} else if (!(img = stbi_load(path, &w, &h, &comp, 3))) { snprintf(why, sizeof(why), "%.60s", stbi_failure_reason()); return -1; }
	iw = w; ih = h;
	o = exif_orientation(path);
	if (o == 2 || o == 4 || o == 5 || o == 7) flip_h();
	if (o == 3 || o == 4) turn_180();
	if (o == 6 || o == 7) turn_90();
	if (o == 8 || o == 5) { if (!turn_90()) turn_180(); }
	return 0;
}

/* ---------------- drawing ---------------- */
static double fit_zoom(int dw, int dh)
{
	double z = (double)dw / iw < (double)dh / ih ? (double)dw / iw : (double)dh / ih;
	return z > 1 ? 1 : z;                   /* a small picture is not blown up until asked */
}
struct span { int i0, n; unsigned short w[2]; };        /* picture pixels i0 .. i0+n-1; weight of the first and the last, /256 */
/* which picture pixels screen pixel d covers, for a reduction (z < 1) */
static void span_of(struct span *s, double z, double centre, int d, int dsize, int isize)
{
	double a = centre + (d - dsize / 2.0) / z, b = a + 1 / z; int i0 = (int)floor(a), i1 = (int)ceil(b) - 1;
	s->w[0] = (unsigned short)(256 * (i0 + 1 - a)); s->w[1] = (unsigned short)(256 * (b - i1));
	if (i0 < 0) { i0 = 0; s->w[0] = 256; }
	if (i1 >= isize) { i1 = isize - 1; s->w[1] = 256; }
	s->i0 = i0; s->n = i1 - i0 + 1;
	if (s->n == 1) s->w[0] = s->w[1] = 256;
	if (a >= isize || b <= 0) s->n = 0;     /* beside the picture */
}
static inline uint16_t dither565(int r, int g, int b, int x, int y)
{
	static const unsigned char bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	int t = bayer[y & 3][x & 3];
	r += t >> 1; g += t >> 2; b += t >> 1;                  /* steps of 8, 4, 8: add up to one step less one */
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (uint16_t)((r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3);
}
static void render(int x0, int y0, int dw, int dh)
{
	double z = zoom > 0 ? zoom : fit_zoom(dw, dh), halfw = dw / 2.0 / z, halfh = dh / 2.0 / z;
	if (zoom <= 0) { cxp = iw / 2.0; cyp = ih / 2.0; }
	/* keep the picture on screen: no empty edge while there is more picture on the other side */
	if (iw * z <= dw) cxp = iw / 2.0; else { if (cxp < halfw) cxp = halfw; if (cxp > iw - halfw) cxp = iw - halfw; }
	if (ih * z <= dh) cyp = ih / 2.0; else { if (cyp < halfh) cyp = halfh; if (cyp > ih - halfh) cyp = ih - halfh; }
	if (z < 1) {
		struct span *sx = malloc((size_t)dw * sizeof(*sx)), sy;
		if (!sx) return;
		for (int x = 0; x < dw; x++) span_of(&sx[x], z, cxp, x, dw, iw);
		for (int y = 0; y < dh; y++) {
			uint16_t *out = nzui_fb + (size_t)(y0 + y) * nzui_w + x0;
			span_of(&sy, z, cyp, y, dh, ih);
			for (int x = 0; x < dw; x++) {
				const struct span *s = &sx[x]; unsigned long long r = 0, g = 0, b = 0, wsum = 0;
				if (!s->n || !sy.n) { out[x] = NZK_BG; continue; }
				for (int j = 0; j < sy.n; j++) {
					const unsigned char *p = img + ((size_t)(sy.i0 + j) * iw + s->i0) * 3;
					unsigned wy = j == 0 ? sy.w[0] : j == sy.n - 1 ? sy.w[1] : 256, rr = 0, gg = 0, bb = 0, ww = 0;
					for (int i = 0; i < s->n; i++, p += 3) {
						unsigned wx = i == 0 ? s->w[0] : i == s->n - 1 ? s->w[1] : 256;
						rr += p[0] * wx; gg += p[1] * wx; bb += p[2] * wx; ww += wx;
					}
					r += (unsigned long long)rr * wy; g += (unsigned long long)gg * wy; b += (unsigned long long)bb * wy; wsum += (unsigned long long)ww * wy;
				}
				if (!wsum) { out[x] = NZK_BG; continue; }
				out[x] = dither565((int)(r / wsum), (int)(g / wsum), (int)(b / wsum), x, y);
			}
		}
		free(sx);
		return;
	}
	for (int y = 0; y < dh; y++) {
		uint16_t *out = nzui_fb + (size_t)(y0 + y) * nzui_w + x0;
		double v = cyp + (y + 0.5 - dh / 2.0) / z - 0.5;
		for (int x = 0; x < dw; x++) {
			double u = cxp + (x + 0.5 - dw / 2.0) / z - 0.5;
			if (u < -0.5 || v < -0.5 || u > iw - 0.5 || v > ih - 0.5) { out[x] = NZK_BG; continue; }
			if (z >= 3) {                           /* enlarged a lot: the pixels themselves */
				int xi = (int)(u + 0.5), yi = (int)(v + 0.5); const unsigned char *p;
				if (xi >= iw) xi = iw - 1;
				if (yi >= ih) yi = ih - 1;
				p = img + ((size_t)yi * iw + xi) * 3;
				out[x] = dither565(p[0], p[1], p[2], x, y);
			} else {
				int xi, yi, x1, y1, fx, fy, c[3];
				if (u < 0) u = 0;
				if (v < 0) v = 0;
				xi = (int)u; yi = (int)v; fx = (int)((u - xi) * 256); fy = (int)((v - yi) * 256);
				x1 = xi + 1 < iw ? xi + 1 : xi; y1 = yi + 1 < ih ? yi + 1 : yi;
				for (int k = 0; k < 3; k++) {
					int a = img[((size_t)yi * iw + xi) * 3 + k], b = img[((size_t)yi * iw + x1) * 3 + k];
					int cc = img[((size_t)y1 * iw + xi) * 3 + k], d = img[((size_t)y1 * iw + x1) * 3 + k];
					int t0 = a * (256 - fx) + b * fx, t1 = cc * (256 - fx) + d * fx;
					c[k] = (t0 * (256 - fy) + t1 * fy) >> 16;
				}
				out[x] = dither565(c[0], c[1], c[2], x, y);
			}
		}
	}
}
static void draw(void)
{
	char right[24];
	nzk_begin();
	if (bars) {
		if (zoom > 0) snprintf(right, sizeof(right), "%d%%", (int)(zoom * 100 + 0.5)); else snprintf(right, sizeof(right), "%d/%d", cur + 1, nnames);
		nzk_title(nnames ? names[cur] : "Image Viewer", right);
		if (img) render(0, NZK_TITLE_H, nzui_w, nzui_h - NZK_TITLE_H - NZK_FOOT_H);
		nzk_footer(zoom > 0 ? "F/X/Z/C:move  Enter:in  Backspace:out" : "Z/C:other  Enter:zoom  T:turn  H:keys");
	} else if (img) render(0, 0, nzui_w, nzui_h);
	if (!img) { nzk_text(8, 50, NZK_TEXT, 38, "This picture cannot be shown."); nzk_text(8, 64, NZK_MUTED, 38, why); }
	nzk_end();
}

/* ---------------- one folder of pictures ---------------- */
static void show(int i)
{
	char path[PATH_MAX];
	if (i < 0 || i >= nnames) return;
	cur = i;
	snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", names[cur]);
	if (!nza_headless) { nzk_begin(); if (bars) { nzk_title(names[cur], NULL); nzk_footer(""); } nzk_text(8, 50, NZK_MUTED, 38, "Loading..."); nzk_end(); }
	load(path);
}
static void zoom_step(int in)
{
	int dw = nzui_w, dh = bars ? nzui_h - NZK_TITLE_H - NZK_FOOT_H : nzui_h; double fit, z;
	if (!img) return;
	fit = fit_zoom(dw, dh); z = zoom > 0 ? zoom : fit;
	if (in) {
		double n = z * 2;
		if (z < 1 && n > 1) n = 1;              /* stop at 100 % on the way */
		if (n > 8) n = 8;
		if (zoom <= 0) { cxp = iw / 2.0; cyp = ih / 2.0; }
		zoom = n;
	} else {
		double n = z / 2;
		if (z > 1 && n < 1) n = 1;
		zoom = n <= fit * 1.01 ? 0 : n;
	}
}
static int do_info(void)
{
	char L[6][48], d[24]; const char *lines[6]; int n = 0; struct tm *tm = localtime(&file_time);
	if (!nnames) return 0;
	{ char a[64]; nzk_ascii(a, sizeof(a), names[cur]); snprintf(L[n++], sizeof(L[0]), "%.39s", a); }
	if (img) snprintf(L[n++], sizeof(L[0]), "Size      %d x %d pixels", src_w, src_h);
	if (file_size >= 1 << 20) snprintf(L[n++], sizeof(L[0]), "File      %.1fM", file_size / 1048576.0); else snprintf(L[n++], sizeof(L[0]), "File      %lldK", (file_size + 1023) >> 10);
	if (tm) { strftime(d, sizeof(d), "%d %b %Y %H:%M", tm); snprintf(L[n++], sizeof(L[0]), "Changed   %s", d); }
	if (img) snprintf(L[n++], sizeof(L[0]), "Shown at  %d %%", (int)((zoom > 0 ? zoom : fit_zoom(nzui_w, bars ? nzui_h - NZK_TITLE_H - NZK_FOOT_H : nzui_h)) * 100 + 0.5));
	for (int i = 0; i < n; i++) lines[i] = L[i];
	return nzk_help("About this picture", NULL, lines, n);
}
static int do_help(void)
{
	static const char *const l[] = {
		"Z / C   previous / next picture",
		"Enter   closer    Backspace  back out",
		"F X Z C   move around when closer",
		"Esc     whole picture; then: back",
		"T       turn a quarter to the right",
		"Space   picture only / with the bars",
		"I       about this picture",
		"H       this page",
		"Shown: JPEG, PNG, BMP, GIF, PPM/PGM",
	};
	return nzk_help("Keys", "v" VERSION, l, 9);
}
/* 1: back (to the list, or out), -1: leave the app */
static int view_loop(void)
{
	struct nzui_event ev; int r, nav;
	for (;;) {
		draw();
		if ((r = nza_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r != 1 || !ev.value) continue;
		if ((nav = nzk_nav(ev.key))) {
			if (zoom > 0) {
				int dw = nzui_w, dh = bars ? nzui_h - NZK_TITLE_H - NZK_FOOT_H : nzui_h;
				if (nav == NZK_NAV_UP) cyp -= dh / 4.0 / zoom; else if (nav == NZK_NAV_DOWN) cyp += dh / 4.0 / zoom;
				else if (nav == NZK_NAV_PGUP) cxp -= dw / 4.0 / zoom; else cxp += dw / 4.0 / zoom;
			} else if (ev.value == 1) {
				if (nav == NZK_NAV_UP || nav == NZK_NAV_PGUP) { if (cur > 0) show(cur - 1); }
				else if (cur + 1 < nnames) show(cur + 1);
			}
			continue;
		}
		if (ev.value != 1) continue;
		r = 0;
		switch (ev.key) {
		case KEY_ENTER: zoom_step(1); break;
		case KEY_BACKSPACE: if (zoom > 0) zoom_step(0); else return 1; break;
		case KEY_ESC: if (zoom > 0) zoom = 0; else return 1; break;
		case KEY_Q: return 1;
		case KEY_SPACE: bars = !bars; break;
		case KEY_T: if (img && turn_90()) r = nzk_message("Turn", "Not enough memory to turn", "this picture."); zoom = 0; break;
		case KEY_I: r = do_info(); break;
		case KEY_H: r = do_help(); break;
		}
		if (r < 0) return -1;
	}
}
static void cfg_load(void)
{
	char p[PATH_MAX], line[80]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-image"));
	if (!(f = fopen(p, "r"))) return;
	while (fgets(line, sizeof(line), f)) if (!strncmp(line, "bars=", 5)) bars = !!atoi(line + 5);
	fclose(f);
}
static void cfg_save(void)
{
	char p[PATH_MAX]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-image"));
	if ((f = fopen(p, "w"))) { fprintf(f, "bars=%d\n", bars); fclose(f); }
}

int main(int argc, char **argv)
{
	const char *file = NULL; int r = 0; char chosen[PATH_MAX], tmp[PATH_MAX], real[PATH_MAX];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) { printf("nz-image %s\n", VERSION); return 0; }
		file = argv[i];
	}
	if (nza_start("Image Viewer", "NZI")) return 1;
	cfg_load();
	if (file) {
		if (!realpath(file, real)) snprintf(real, sizeof(real), "%s", file);
		snprintf(tmp, sizeof(tmp), "%s", real); snprintf(dir, sizeof(dir), "%s", dirname(tmp));
		snprintf(tmp, sizeof(tmp), "%s", real); snprintf(chosen, sizeof(chosen), "%s", basename(tmp));
		names_scan(chosen);
		if (!nnames || strcmp(names[cur], chosen)) {    /* a picture without one of the usual endings: only this one */
			names_free();
			if ((names = malloc(sizeof(*names))) && (names[0] = strdup(chosen))) nnames = 1;
		}
		if (nnames) { show(cur); r = view_loop(); }
	} else {
		const char *home = getenv("HOME");
		if (!getcwd(dir, sizeof(dir)) || !strcmp(dir, "/opt/nz-image")) snprintf(dir, sizeof(dir), "%s", home && *home ? home : "/");
		while (r >= 0) {
			if ((r = nza_pick_file("Image Viewer", dir, sizeof(dir), chosen, sizeof(chosen), has_ext, 1, NULL, 0)) < 0) break;
			if (r != 1) continue;
			snprintf(tmp, sizeof(tmp), "%s", chosen);
			names_scan(basename(tmp));
			if (nnames) { show(cur); r = view_loop(); }
		}
	}
	cfg_save(); free(img); names_free();
	nza_stop();
	return 0;
}
