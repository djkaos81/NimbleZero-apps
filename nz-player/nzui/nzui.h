/* SPDX-License-Identifier: GPL-2.0-only
 *
 * nzui.h - tiny single-header UI layer for the Cardputer Zero (320x170 RGB565).
 *
 * One API, two backends:
 *   device (default)  Linux fbdev + evdev: the standard kernel interfaces, nothing
 *                     Cardputer-specific.  Works on any fbdev device.
 *   simulator         -DNZUI_SDL: an SDL2 window on a desktop PC (scaled 3x), or
 *                     headless with SDL_VIDEODRIVER=dummy.
 *
 * Usage: in exactly one .c file  #define NZUI_IMPLEMENTATION  before including.
 * Key codes are Linux input event codes (<linux/input-event-codes.h>) on both backends.
 *
 * Simulator-only environment variables (for automated tests):
 *   NZUI_KEYS="down,down,enter,q"   inject these key presses, one per poll
 *   NZUI_SCREENSHOT=out.ppm         write the last flushed frame on nzui_quit()
 */
#ifndef NZUI_H
#define NZUI_H
#include <stdint.h>

#define NZUI_RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
#define NZUI_BLACK NZUI_RGB(0, 0, 0)
#define NZUI_WHITE NZUI_RGB(235, 235, 235)
#define NZUI_GREY  NZUI_RGB(120, 120, 120)
#define NZUI_FONT_W 8
#define NZUI_FONT_H 8

struct nzui_event { int key; int value; };   /* value: 1 press, 0 release, 2 auto-repeat */

extern int nzui_w, nzui_h;
extern uint16_t *nzui_fb;                    /* back buffer, nzui_w * nzui_h pixels */

int  nzui_init(const char *title);           /* 0 on success */
void nzui_quit(void);
void nzui_flush(void);                       /* present the back buffer */
int  nzui_poll(struct nzui_event *ev, int timeout_ms);   /* 1 event, 0 timeout, -1 quit request */
void nzui_suspend(void);                     /* hand screen + keyboard to a child process */
void nzui_resume(void);
void nzui_clear(uint16_t c);
void nzui_rect(int x, int y, int w, int h, uint16_t c);
void nzui_text(int x, int y, uint16_t c, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

#ifdef NZUI_IMPLEMENTATION
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x8_basic.h"

int nzui_w = 320, nzui_h = 170;
uint16_t *nzui_fb;

void nzui_clear(uint16_t c) { for (int i = 0; i < nzui_w * nzui_h; i++) nzui_fb[i] = c; }
void nzui_rect(int x, int y, int w, int h, uint16_t c)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < nzui_h; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < nzui_w; i++) nzui_fb[j * nzui_w + i] = c;
}
void nzui_text(int x, int y, uint16_t c, const char *fmt, ...)
{
	char buf[128]; va_list ap;
	va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	for (const unsigned char *p = (const unsigned char *)buf; *p; p++, x += NZUI_FONT_W) {
		const unsigned char *g = font8x8_basic[*p < 128 ? *p : '?'];
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++)
				if ((g[j] >> i & 1) && x + i >= 0 && x + i < nzui_w && y + j >= 0 && y + j < nzui_h)
					nzui_fb[(y + j) * nzui_w + x + i] = c;
	}
}

#ifdef NZUI_SDL  /* ------------------------------------------------ simulator */
#include <SDL.h>
#include <linux/input-event-codes.h>
static SDL_Window *nzui__win; static SDL_Renderer *nzui__ren; static SDL_Texture *nzui__tex;
static char *nzui__keys; static const char *nzui__shot;
static const struct { const char *name; SDL_Scancode sc; int key; } nzui__map[] = {
	{"up", SDL_SCANCODE_UP, KEY_UP}, {"down", SDL_SCANCODE_DOWN, KEY_DOWN}, {"left", SDL_SCANCODE_LEFT, KEY_LEFT},
	{"right", SDL_SCANCODE_RIGHT, KEY_RIGHT}, {"enter", SDL_SCANCODE_RETURN, KEY_ENTER}, {"esc", SDL_SCANCODE_ESCAPE, KEY_ESC},
	{"tab", SDL_SCANCODE_TAB, KEY_TAB}, {"space", SDL_SCANCODE_SPACE, KEY_SPACE}, {"backspace", SDL_SCANCODE_BACKSPACE, KEY_BACKSPACE},
	{"a", SDL_SCANCODE_A, KEY_A}, {"b", SDL_SCANCODE_B, KEY_B}, {"c", SDL_SCANCODE_C, KEY_C}, {"d", SDL_SCANCODE_D, KEY_D},
	{"e", SDL_SCANCODE_E, KEY_E}, {"f", SDL_SCANCODE_F, KEY_F}, {"g", SDL_SCANCODE_G, KEY_G}, {"h", SDL_SCANCODE_H, KEY_H},
	{"i", SDL_SCANCODE_I, KEY_I}, {"j", SDL_SCANCODE_J, KEY_J}, {"k", SDL_SCANCODE_K, KEY_K}, {"l", SDL_SCANCODE_L, KEY_L},
	{"m", SDL_SCANCODE_M, KEY_M}, {"n", SDL_SCANCODE_N, KEY_N}, {"o", SDL_SCANCODE_O, KEY_O}, {"p", SDL_SCANCODE_P, KEY_P},
	{"q", SDL_SCANCODE_Q, KEY_Q}, {"r", SDL_SCANCODE_R, KEY_R}, {"s", SDL_SCANCODE_S, KEY_S}, {"t", SDL_SCANCODE_T, KEY_T},
	{"u", SDL_SCANCODE_U, KEY_U}, {"v", SDL_SCANCODE_V, KEY_V}, {"w", SDL_SCANCODE_W, KEY_W}, {"x", SDL_SCANCODE_X, KEY_X},
	{"y", SDL_SCANCODE_Y, KEY_Y}, {"z", SDL_SCANCODE_Z, KEY_Z},
	{"1", SDL_SCANCODE_1, KEY_1}, {"2", SDL_SCANCODE_2, KEY_2}, {"3", SDL_SCANCODE_3, KEY_3}, {"4", SDL_SCANCODE_4, KEY_4},
	{"5", SDL_SCANCODE_5, KEY_5}, {"6", SDL_SCANCODE_6, KEY_6}, {"7", SDL_SCANCODE_7, KEY_7}, {"8", SDL_SCANCODE_8, KEY_8},
	{"9", SDL_SCANCODE_9, KEY_9}, {"0", SDL_SCANCODE_0, KEY_0},
};
int nzui_init(const char *title)
{
	if (SDL_Init(SDL_INIT_VIDEO)) return -1;
	nzui__win = SDL_CreateWindow(title ? title : "nzui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, nzui_w * 3, nzui_h * 3, 0);
	nzui__ren = SDL_CreateRenderer(nzui__win, -1, SDL_RENDERER_SOFTWARE);
	nzui__tex = SDL_CreateTexture(nzui__ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, nzui_w, nzui_h);
	nzui_fb = calloc((size_t)nzui_w * nzui_h, 2);
	if (getenv("NZUI_KEYS")) nzui__keys = strdup(getenv("NZUI_KEYS"));
	nzui__shot = getenv("NZUI_SCREENSHOT");
	return nzui__win && nzui__ren && nzui__tex && nzui_fb ? 0 : -1;
}
void nzui_flush(void)
{
	SDL_UpdateTexture(nzui__tex, NULL, nzui_fb, nzui_w * 2);
	SDL_RenderClear(nzui__ren); SDL_RenderCopy(nzui__ren, nzui__tex, NULL, NULL); SDL_RenderPresent(nzui__ren);
}
int nzui_poll(struct nzui_event *ev, int timeout_ms)
{
	SDL_Event e;
	if (nzui__keys) {                        /* scripted input */
		char *t = nzui__keys, *comma = strchr(t, ',');
		if (!*t) return -1;
		if (comma) *comma = 0;
		nzui__keys = comma ? comma + 1 : t + strlen(t);
		for (unsigned i = 0; i < sizeof(nzui__map) / sizeof(nzui__map[0]); i++)
			if (!strcmp(t, nzui__map[i].name)) { ev->key = nzui__map[i].key; ev->value = 1; return 1; }
		return 0;
	}
	if (!SDL_WaitEventTimeout(&e, timeout_ms)) return 0;
	if (e.type == SDL_QUIT) return -1;
	if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
		for (unsigned i = 0; i < sizeof(nzui__map) / sizeof(nzui__map[0]); i++)
			if (e.key.keysym.scancode == nzui__map[i].sc) {
				ev->key = nzui__map[i].key; ev->value = e.type == SDL_KEYUP ? 0 : e.key.repeat ? 2 : 1; return 1;
			}
	return 0;
}
void nzui_suspend(void) {} void nzui_resume(void) {}
void nzui_quit(void)
{
	if (nzui__shot) {
		FILE *f = fopen(nzui__shot, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", nzui_w, nzui_h);
			for (int i = 0; i < nzui_w * nzui_h; i++) {
				uint16_t c = nzui_fb[i]; fputc(c >> 8 & 0xf8, f); fputc(c >> 3 & 0xfc, f); fputc(c << 3 & 0xf8, f);
			}
			fclose(f);
		}
	}
	SDL_Quit();
}
#else            /* ------------------------------------------------ device: fbdev + evdev */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
static int nzui__fd = -1, nzui__bpp, nzui__stride, nzui__tty = -1, nzui__nin;
static uint8_t *nzui__mem; static struct pollfd nzui__in[16];
static void nzui__inputs(void)
{
	DIR *d = opendir("/dev/input"); struct dirent *e; char p[280];
	for (int i = 0; i < nzui__nin; i++) close(nzui__in[i].fd);
	nzui__nin = 0;
	while (d && (e = readdir(d)) && nzui__nin < 16) {
		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) { nzui__in[nzui__nin].fd = fd; nzui__in[nzui__nin].events = POLLIN; nzui__nin++; }
	}
	if (d) closedir(d);
}
int nzui_init(const char *title)
{
	static const char *cand[] = { "/dev/fb_lcd", "/dev/fb0", NULL };
	const char *env = getenv("NZ_FB"); struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
	(void)title;
	for (int i = env ? -1 : 0; i < 0 || cand[i]; i++) {
		int fd = open(i < 0 ? env : cand[i], O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) || (v.bits_per_pixel != 16 && v.bits_per_pixel != 32)) { close(fd); continue; }
		nzui__fd = fd; nzui_w = v.xres; nzui_h = v.yres; nzui__bpp = v.bits_per_pixel; nzui__stride = f.line_length;
		nzui__mem = mmap(NULL, (size_t)nzui__stride * nzui_h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (nzui__mem == MAP_FAILED) { close(fd); nzui__fd = -1; continue; }
		/* light the panel even if no console ever bound to this framebuffer */
		v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (ioctl(fd, FBIOPUT_VSCREENINFO, &v)) {}
		if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK)) {}
		break;
	}
	if (nzui__fd < 0) return -1;
	nzui_fb = calloc((size_t)nzui_w * nzui_h, 2);
	nzui__tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
	nzui_resume();
	return nzui_fb ? 0 : -1;
}
void nzui_flush(void)
{
	for (int y = 0; y < nzui_h; y++) {
		uint16_t *s = nzui_fb + (size_t)y * nzui_w;
		if (nzui__bpp == 16) memcpy(nzui__mem + (size_t)y * nzui__stride, s, (size_t)nzui_w * 2);
		else {
			uint32_t *d = (uint32_t *)(nzui__mem + (size_t)y * nzui__stride);
			for (int x = 0; x < nzui_w; x++) d[x] = 0xff000000u | ((s[x] >> 8 & 0xf8) << 16) | ((s[x] >> 3 & 0xfc) << 8) | (s[x] << 3 & 0xf8);
		}
	}
}
int nzui_poll(struct nzui_event *ev, int timeout_ms)
{
	static struct input_event q[32]; static int qn, qi; static int rescan;
	for (;;) {
		while (qi < qn) { struct input_event *e = &q[qi++]; if (e->type == EV_KEY) { ev->key = e->code; ev->value = e->value; return 1; } }
		if (poll(nzui__in, nzui__nin, timeout_ms) <= 0) { if (++rescan % 4 == 0 || !nzui__nin) nzui__inputs(); return 0; }
		qn = qi = 0;
		for (int i = 0; i < nzui__nin && !qn; i++)
			if (nzui__in[i].revents & POLLIN) { ssize_t n = read(nzui__in[i].fd, q, sizeof(q)); if (n > 0) qn = (int)(n / sizeof(q[0])); }
			else if (nzui__in[i].revents & (POLLERR | POLLHUP)) { nzui__inputs(); return 0; }
		timeout_ms = 0;
		if (!qn) return 0;
	}
}
void nzui_suspend(void)
{
	/* Keys pressed while this UI was up also reached the tty's line buffer.  Drop them,
	 * or the next console program (a shell script's "read") takes them as its input. */
	if (nzui__tty >= 0) { ioctl(nzui__tty, KDSETMODE, KD_TEXT); tcflush(nzui__tty, TCIFLUSH); }
	for (int i = 0; i < nzui__nin; i++) close(nzui__in[i].fd);
	nzui__nin = 0;
}
void nzui_resume(void)
{
	if (nzui__tty >= 0) ioctl(nzui__tty, KDSETMODE, KD_GRAPHICS);
	nzui__inputs();
}
void nzui_quit(void) { nzui_clear(NZUI_BLACK); nzui_flush(); nzui_suspend(); }
#endif
#endif /* NZUI_IMPLEMENTATION */
#endif /* NZUI_H */
