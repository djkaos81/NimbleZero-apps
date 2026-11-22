/* SPDX-License-Identifier: GPL-2.0-only
 *
 * nzui_kit.h - the shared look and keys of NimbleZero apps, on top of nzui.h.
 *
 * nzui.h gives a back buffer, rectangles, text and key events.  This header adds what
 * every app otherwise copies: the palette, the title bar (title, battery, clock), tabs,
 * the footer, two kinds of list, the scrollbar, message / question / pick / text input /
 * options / help screens, and one meaning for the navigation keys.  docs/STYLE.md is the guide in
 * words; this file is the same guide as code.
 *
 * Usage: in the .c file that has  #define NZUI_IMPLEMENTATION  also
 *     #define NZUI_KIT_IMPLEMENTATION
 *     #include "nzui_kit.h"
 * (nzui_kit.h includes nzui.h).  Other files include it without the defines.
 *
 * A screen is drawn like this:
 *     nzk_begin();                          clear, reset the "something is scrolling" flag
 *     nzk_title("Files", NULL);             or nzk_tabs(names, n, active)
 *     ... nzk_subline(), nzk_row() / nzk_menu_row(), nzk_scrollbar() ...
 *     nzk_footer("Enter:open  O:options");
 *     nzk_end();                            show it
 *     r = nzk_wait(&ev, nzk_timeout());     1 key, 0 nothing, -1 leave
 *
 * An app with its own screen output or key source (nz-player) sets nzk_flush_fn and
 * nzk_poll_fn; everything in here goes through them.
 *
 * Environment (tests, screenshots): NZK_BATTERY="85" or "85+" (level, + charging),
 * NZK_CLOCK="12:34".
 */
#ifndef NZUI_KIT_H
#define NZUI_KIT_H
#include <stddef.h>
#include "nzui.h"

/* ---- palette: an app uses these names, not its own numbers ---- */
#define NZK_BG       NZUI_BLACK
#define NZK_TEXT     NZUI_WHITE                 /* 235,235,235 */
#define NZK_TEXT2    NZUI_RGB(200, 200, 200)    /* long passages of text (help) */
#define NZK_MUTED    NZUI_GREY                  /* 120,120,120: hints, sizes, comments, paths */
#define NZK_BAR      NZUI_RGB(30, 70, 170)      /* title bar */
#define NZK_TAB_OFF  NZUI_RGB(170, 190, 230)    /* name of a tab that is not selected */
#define NZK_HI       NZUI_RGB(250, 200, 60)     /* text of the selected row, selected tab, cursor */
#define NZK_SEL      NZUI_RGB(40, 40, 60)       /* background of the selected row */
#define NZK_FOOT     NZUI_RGB(25, 25, 25)       /* footer, scrollbar track */
#define NZK_LINK     NZUI_RGB(120, 190, 255)    /* folders, values of settings, headings */
#define NZK_RED      NZUI_RGB(230, 60, 60)      /* errors, battery <= 15 % */
#define NZK_GREEN    NZUI_RGB(90, 220, 110)     /* success, battery > 35 % */

/* ---- measures (pixels; the font is 8x8, so 40 columns) ---- */
#define NZK_COLS       40
#define NZK_TITLE_H    12
#define NZK_FOOT_H     11
#define NZK_SUB_Y      14      /* one grey line under the title: a path or a help text */
#define NZK_LIST_Y     24      /* first compact row when there is a sub line */
#define NZK_ROW_H      10      /* compact row: one line */
#define NZK_MENU_Y     15      /* first menu row */
#define NZK_MENU_ROW_H 18      /* menu row: name and comment */
#define NZK_ROWS       ((nzui_h - NZK_LIST_Y - NZK_FOOT_H - 1) / NZK_ROW_H)           /* 13 */
#define NZK_MENU_ROWS  ((nzui_h - NZK_MENU_Y + 1 - NZK_FOOT_H - 1) / NZK_MENU_ROW_H)  /* 8 */

/* ---- output and input hooks ---- */
extern void (*nzk_flush_fn)(void);                              /* default: nzui_flush */
extern int  (*nzk_poll_fn)(struct nzui_event *, int);           /* default: nzui_poll */
extern int nzk_scrolling;       /* set while a too-long selected name is moving sideways */
extern int nzk_shift;           /* a Shift key is held (kept up to date by nzk_wait) */

void nzk_begin(void);
void nzk_end(void);
int  nzk_timeout(void);         /* how long to wait before the screen needs drawing again, ms */
/* Like the poll function, plus: Esc for the ` key (top left of the Cardputer, Esc is its Fn
 * layer), Enter for keypad Enter, and the screen switched off after the time set in the
 * launcher's Options (the first key then only wakes it).  An app that switches the screen
 * off itself calls nzk_idle_allow(0) once. */
int  nzk_wait(struct nzui_event *ev, int timeout_ms);
void nzk_idle_allow(int on);    /* 0: keep the screen on (playing a movie); default 1 */

/* ---- text ---- */
void nzk_ascii(char *dst, size_t n, const char *src);           /* UTF-8 -> what the font can draw */
void nzk_text(int x, int y, uint16_t c, int maxch, const char *s);
/* too long: ends in ~, or moves sideways while selected */
void nzk_text_scroll(int x, int y, uint16_t c, int maxch, const char *s, int selected);

/* ---- bars ---- */
void nzk_title(const char *title, const char *right);           /* right: short extra text or NULL */
void nzk_tabs(const char *const *names, int n, int active);     /* title bar made of tabs (Tab key) */
void nzk_subline(const char *s, int keep_end);                  /* keep_end: show the end of a long path */
void nzk_footer(const char *hints);                             /* "Enter:open  O:options" */
/* Footer of an app with many functions: one label per function, the first character of
 * each is its key and is drawn highlighted ("Help", "2Usr", "5Cp").  Cells touch; 40 columns. */
void nzk_keybar(const char *const *labels, int n);

/* ---- lists ---- */
enum { NZK_NAV_NONE, NZK_NAV_UP, NZK_NAV_DOWN, NZK_NAV_PGUP, NZK_NAV_PGDN };
struct nzk_list { int n, sel, top; };
int  nzk_nav(int key);                                          /* F X Z C, arrows, PgUp PgDn */
void nzk_list_move(struct nzk_list *l, int nav, int rows);      /* stops at both ends */
void nzk_list_fit(struct nzk_list *l, int rows);                /* keep sel visible; call before drawing */
/* Compact row at (x, y), w wide.  selected: 0 no, 1 yes, 2 yes but in a list that does not
 * have the keys (the other pane).  mark: one character before the name ("/" for a folder)
 * or NULL.  right: text at the right edge (a size) or NULL. */
void nzk_row(int x, int y, int w, int selected, uint16_t colour, const char *mark, const char *name, const char *right);
void nzk_menu_row(int y, int selected, const char *name, const char *comment);
void nzk_scrollbar(int x, int y, int h, int top, int n, int rows);   /* nothing if all rows fit */

/* ---- whole screens; each returns -1 when the app should leave ---- */
int nzk_message(const char *title, const char *l1, const char *l2);                        /* 0 */
int nzk_confirm(const char *title, const char *l1, const char *l2, const char *action);    /* 1 yes, 0 no */
/* A question with several answers: returns the key pressed out of keys[], 0 for Esc. */
int nzk_ask(const char *title, const char *l1, const char *l2, const char *hints, const int *keys, int nkeys);
/* Pick one of a short list (compact rows): 1 chosen (*sel), 0 back. */
int nzk_pick(const char *title, const char *const *items, int n, int *sel);
int nzk_input(const char *title, const char *prompt, char *buf, size_t size);              /* 1 Enter, 0 Esc */
int nzk_help(const char *title, const char *right, const char *const *lines, int n);       /* 0 */
struct nzk_option {
	const char *name, *help;
	void (*value)(int index, char *buf, size_t n);          /* the current value as text */
	void (*change)(int index, int dir);                     /* dir: -1 or +1 */
};
int nzk_options(const char *title, const struct nzk_option *o, int n, int *sel);           /* 0 */
/* not a loop: draws once and returns.  permille < 0: no bar. */
void nzk_progress(const char *title, const char *l1, const char *l2, int permille, const char *hints);

/* ---- leaving the top level: Q at once, Esc twice within 2.5 s ---- */
int nzk_leave(int key);         /* 1: leave now.  While waiting for the second Esc, nzk_footer() says so. */

#ifdef NZUI_KIT_IMPLEMENTATION
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include "nz_keymap.h"

void (*nzk_flush_fn)(void) = nzui_flush;
int  (*nzk_poll_fn)(struct nzui_event *, int) = nzui_poll;
int nzk_scrolling, nzk_shift;
static int nzk__idle_ok = 1, nzk__screen_off; static double nzk__last_key, nzk__esc_until;

static double nzk__now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static long nzk__rdl(const char *p) { FILE *f = fopen(p, "r"); long v = -1; if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); } return v; }

void nzk_begin(void) { nzui_clear(NZK_BG); nzk_scrolling = 0; }
void nzk_end(void) { nzk_flush_fn(); }
int nzk_timeout(void)
{
	double left = nzk__esc_until - nzk__now();
	int to = (int)(60 - time(NULL) % 60) * 1000;            /* the clock changes then */
	if (nzk_scrolling) return 250;
	if (left > 0 && left * 1000 < to) to = (int)(left * 1000) + 50;
	return to > 5000 ? 5000 : to;                           /* 5 s: the battery level */
}

/* ---- screen off when no key is pressed (same setting as the launcher) ---- */
#ifndef NZUI_SDL
static void nzk__backlight(int on)
{
	DIR *d = opendir("/sys/class/backlight"); struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[320]; FILE *f; if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof(p), "/sys/class/backlight/%s/bl_power", e->d_name);
		if ((f = fopen(p, "w"))) { fputs(on ? "0\n" : "4\n", f); fclose(f); }
	}
	if (d) closedir(d);
}
static int nzk__idle_s(void)
{
	static double next; static int v = 120; double now = nzk__now();
	if (now >= next) {
		long f = nzk__rdl("/data/etc/nz/screen-off"); const char *e = getenv("NZ_IDLE_S");
		v = e ? atoi(e) : f < 0 ? 120 : (int)f; next = now + 10;
	}
	return v;
}
#endif
void nzk_idle_allow(int on)
{
	nzk__idle_ok = on; nzk__last_key = nzk__now();
#ifndef NZUI_SDL
	if (!on && nzk__screen_off) { nzk__screen_off = 0; nzk__backlight(1); }
#endif
}

int nzk_wait(struct nzui_event *ev, int timeout_ms)
{
	int r;
	if (!nzk__last_key) nzk__last_key = nzk__now();
	if (nzk__screen_off) timeout_ms = 1000;
	r = nzk_poll_fn(ev, timeout_ms);
	if (r != 1) {
#ifndef NZUI_SDL
		if (r == 0 && !nzk__screen_off && nzk__idle_ok && nzk__idle_s() > 0 &&
		    nzk__now() - nzk__last_key >= nzk__idle_s()) { nzk__screen_off = 1; nzk__backlight(0); }
#endif
		return r;
	}
	if (ev->key == KEY_LEFTSHIFT || ev->key == KEY_RIGHTSHIFT) nzk_shift = ev->value != 0;
	if (ev->key == KEY_GRAVE) ev->key = KEY_ESC;
	if (ev->key == KEY_KPENTER) ev->key = KEY_ENTER;
	if (ev->value) {
		nzk__last_key = nzk__now();
#ifndef NZUI_SDL
		if (nzk__screen_off) { nzk__screen_off = 0; nzk__backlight(1); return 0; }   /* the first key only wakes the screen */
#endif
	}
	return 1;
}

/* ---- text ---- */
void nzk_ascii(char *dst, size_t n, const char *src)
{
	static const char lat1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
	const unsigned char *s = (const unsigned char *)src; size_t o = 0;
	if (!n) return;
	while (s && *s && o + 1 < n) {
		unsigned c = *s++; int more = 0;
		if (c < 0x80) { dst[o++] = c < 32 || c == 127 ? ' ' : (char)c; continue; }
		if ((c & 0xE0) == 0xC0) { c &= 0x1F; more = 1; }
		else if ((c & 0xF0) == 0xE0) { c &= 0x0F; more = 2; }
		else if ((c & 0xF8) == 0xF0) { c &= 0x07; more = 3; }
		else { dst[o++] = '?'; continue; }
		while (more-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
		if (c >= 0xC0 && c <= 0xFF) dst[o++] = lat1[c - 0xC0];
		else if (c == 0x2018 || c == 0x2019) dst[o++] = '\'';
		else if (c == 0x201C || c == 0x201D) dst[o++] = '"';
		else if (c == 0x2013 || c == 0x2014) dst[o++] = '-';
		else if (c == 0xA0) dst[o++] = ' ';
		else if (c == 0xFEFF || (c >= 0x300 && c <= 0x36F)) ;   /* byte order mark, combining accents: nothing */
		else dst[o++] = '?';
	}
	dst[o] = 0;
}
void nzk_text(int x, int y, uint16_t c, int maxch, const char *s)
{
	char a[128];
	nzk_ascii(a, sizeof(a), s);
	if (maxch < 0) maxch = 0;
	if (maxch < (int)sizeof(a)) a[maxch] = 0;
	nzui_text(x, y, c, "%s", a);
}
void nzk_text_scroll(int x, int y, uint16_t c, int maxch, const char *s, int selected)
{
	char a[600]; int n;
	nzk_ascii(a, sizeof(a), s); n = (int)strlen(a);
	if (maxch < 1) return;
	if (n > maxch && selected) {
		int span = n - maxch + 8, off = (int)(nzk__now() * 4) % span - 4;
		nzk_scrolling = 1;
		if (off < 0) off = 0;
		if (off > n - maxch) off = n - maxch;
		nzk_text(x, y, c, maxch, a + off);
	} else {
		if (n > maxch) { a[maxch - 1] = '~'; a[maxch] = 0; }
		nzk_text(x, y, c, maxch, a);
	}
}

/* ---- bars ---- */
/* battery level in per cent (-1: no fuel gauge); *chg: 1 charging.  Read every 10 s at most. */
static int nzk__battery(int *chg)
{
	static double next; static int cap = -1, charging; double now = nzk__now();
	const char *env = getenv("NZK_BATTERY");
	if (env) { *chg = strchr(env, '+') != NULL; return atoi(env) > 100 ? 100 : atoi(env); }
	if (now >= next) {
		DIR *d = opendir("/sys/class/power_supply"); struct dirent *e;
		cap = -1; charging = 0; next = now + 10;
		while (d && (e = readdir(d)) && cap < 0) {
			char p[320], st[32] = ""; FILE *f; if (e->d_name[0] == '.') continue;
			snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name); cap = (int)nzk__rdl(p);
			if (cap < 0) continue;
			snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", e->d_name);
			if ((f = fopen(p, "r"))) { if (fgets(st, sizeof(st), f)) charging = !strncmp(st, "Charging", 8); fclose(f); }
		}
		if (d) closedir(d);
		if (cap > 100) cap = 100;
	}
	*chg = charging;
	return cap;
}
/* battery and clock at the right of the title bar; returns the x where they begin */
static int nzk__status(void)
{
	time_t t = time(NULL); struct tm *tm = localtime(&t); int chg, b = nzk__battery(&chg);
	int x = nzui_w - 8 * 5 - 4; const char *clk = getenv("NZK_CLOCK");
	if (clk) nzui_text(x, 2, NZK_TEXT, "%.5s", clk);
	else nzui_text(x, 2, NZK_TEXT, "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
	if (b >= 0) {   /* outline, fill proportional to the level */
		int fill = b * 14 / 100;
		uint16_t lvl = b <= 15 ? NZK_RED : b <= 35 ? NZK_HI : NZK_GREEN;
		x = nzui_w - 8 * 14;
		nzui_rect(x, 2, 18, 8, NZK_TEXT); nzui_rect(x + 18, 4, 2, 4, NZK_TEXT);
		nzui_rect(x + 1, 3, 16, 6, NZK_BAR); nzui_rect(x + 2, 4, fill, 4, lvl);
		if (chg) {  /* charging: a lightning bolt across the battery, white with a dark edge */
			static const char *bolt[8] = { "....##.", "...##..", "..##...", ".#####.", "...##..", "..##...", ".##....", "##....." };
			for (int pass = 0; pass < 2; pass++)
				for (int j = 0; j < 8; j++)
					for (int i = 0; i < 7; i++) {
						if (bolt[j][i] != '#') continue;
						if (pass) nzui_rect(x + 6 + i, 2 + j, 1, 1, NZK_TEXT);
						else nzui_rect(x + 5 + i, 1 + j, 3, 3, NZK_BG);
					}
		}
		nzui_text(x + 23, 2, NZK_TEXT, "%d%%", b);
	}
	return x;
}
void nzk_title(const char *title, const char *right)
{
	int x, rl = right ? (int)strlen(right) : 0;
	nzui_rect(0, 0, nzui_w, NZK_TITLE_H, NZK_BAR);
	x = nzk__status();
	if (rl) { x -= 8 * rl + 8; nzk_text(x, 2, NZK_TEXT, rl, right); }
	nzk_text(4, 2, NZK_TEXT, (x - 4 - 8) / 8, title);
}
void nzk_tabs(const char *const *names, int n, int active)
{
	int x = 6, limit;
	nzui_rect(0, 0, nzui_w, NZK_TITLE_H, NZK_BAR);
	limit = nzk__status() - 8;
	for (int i = 0; i < n; i++) {
		int w = 8 * (int)strlen(names[i]);
		if (x + w > limit) break;
		if (i == active) nzui_rect(x - 4, 1, w + 8, NZK_TITLE_H - 2, NZK_BG);
		nzk_text(x, 2, i == active ? NZK_HI : NZK_TAB_OFF, NZK_COLS, names[i]);
		x += w + 14;
	}
}
void nzk_subline(const char *s, int keep_end)
{
	char a[600]; int n;
	nzk_ascii(a, sizeof(a), s); n = (int)strlen(a);
	nzk_text(4, NZK_SUB_Y, NZK_MUTED, NZK_COLS - 1, keep_end && n > NZK_COLS - 1 ? a + n - (NZK_COLS - 1) : a);
}
void nzk_footer(const char *hints)
{
	nzui_rect(0, nzui_h - NZK_FOOT_H, nzui_w, NZK_FOOT_H, NZK_FOOT);
	nzk_text(4, nzui_h - 9, NZK_MUTED, NZK_COLS - 1, nzk__now() < nzk__esc_until ? "Press Esc again to leave" : hints);
}

void nzk_keybar(const char *const *labels, int n)
{
	int x = 0;
	if (nzk__now() < nzk__esc_until) { nzk_footer(""); return; }
	nzui_rect(0, nzui_h - NZK_FOOT_H, nzui_w, NZK_FOOT_H, NZK_FOOT);
	for (int i = 0; i < n && x < nzui_w; i++) {
		nzk_text(x, nzui_h - 9, NZK_HI, 1, labels[i]);
		nzk_text(x + 8, nzui_h - 9, NZK_TEXT2, NZK_COLS, labels[i] + 1);
		x += 8 * (int)strlen(labels[i]);
	}
}

/* ---- lists ---- */
int nzk_nav(int key)
{
	switch (key) {
	case KEY_UP: case KEY_F: return NZK_NAV_UP;
	case KEY_DOWN: case KEY_X: return NZK_NAV_DOWN;
	case KEY_LEFT: case KEY_Z: case KEY_PAGEUP: return NZK_NAV_PGUP;
	case KEY_RIGHT: case KEY_C: case KEY_PAGEDOWN: return NZK_NAV_PGDN;
	}
	return NZK_NAV_NONE;
}
void nzk_list_move(struct nzk_list *l, int nav, int rows)
{
	if (nav == NZK_NAV_UP) l->sel--;
	else if (nav == NZK_NAV_DOWN) l->sel++;
	else if (nav == NZK_NAV_PGUP) l->sel -= rows;
	else if (nav == NZK_NAV_PGDN) l->sel += rows;
	if (l->sel > l->n - 1) l->sel = l->n - 1;
	if (l->sel < 0) l->sel = 0;
}
void nzk_list_fit(struct nzk_list *l, int rows)
{
	if (l->sel > l->n - 1) l->sel = l->n - 1;
	if (l->sel < 0) l->sel = 0;
	if (l->top > l->n - rows) l->top = l->n - rows;
	if (l->top < 0) l->top = 0;
	if (l->sel < l->top) l->top = l->sel;
	if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
}
void nzk_row(int x, int y, int w, int selected, uint16_t colour, const char *mark, const char *name, const char *right)
{
	int rl = right ? (int)strlen(right) : 0, tx = x + 4, maxch;
	if (selected) nzui_rect(x, y - 1, w - 3, NZK_ROW_H, NZK_SEL);
	if (mark && *mark) { nzk_text(tx, y, colour, 1, mark); tx += 8; }
	maxch = (x + w - 6 - tx) / 8 - (rl ? rl + 1 : 0);
	nzk_text_scroll(tx, y, selected == 1 ? NZK_HI : colour, maxch, name, selected == 1);
	if (rl) nzk_text(x + w - 6 - 8 * rl, y, NZK_MUTED, rl, right);
}
void nzk_menu_row(int y, int selected, const char *name, const char *comment)
{
	if (selected) nzui_rect(0, y - 1, nzui_w - 3, NZK_MENU_ROW_H, NZK_SEL);
	nzk_text(6, y, selected ? NZK_HI : NZK_TEXT, 1, selected ? ">" : " ");
	nzk_text(22, y, selected ? NZK_HI : NZK_TEXT, 36, name);
	if (comment) nzk_text(22, y + 9, NZK_MUTED, 36, comment);
}
void nzk_scrollbar(int x, int y, int h, int top, int n, int rows)
{
	int bh, by;
	if (n <= rows || rows < 1) return;
	bh = h * rows / n < 6 ? 6 : h * rows / n; by = y + (h - bh) * top / (n - rows);
	nzui_rect(x, y, 2, h, NZK_FOOT); nzui_rect(x, by, 2, bh, NZK_MUTED);
}

/* ---- whole screens ---- */
int nzk_leave(int key)
{
	if (key == KEY_Q) return 1;
	if (key != KEY_ESC) { nzk__esc_until = 0; return 0; }
	if (nzk__now() < nzk__esc_until) return 1;
	nzk__esc_until = nzk__now() + 2.5;
	return 0;
}
static void nzk__two_lines(const char *title, const char *l1, const char *l2, const char *foot)
{
	nzk_begin(); nzk_title(title, NULL);
	if (l1) nzk_text(8, 50, NZK_TEXT, 38, l1);
	if (l2) nzk_text(8, 64, NZK_MUTED, 38, l2);
	if (foot) nzk_footer(foot);
}
int nzk_message(const char *title, const char *l1, const char *l2)
{
	struct nzui_event ev; int r;
	for (;;) {
		nzk__two_lines(title, l1, l2, "Press a key"); nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r == 1 && ev.value == 1 && ev.key != KEY_LEFTSHIFT && ev.key != KEY_RIGHTSHIFT) return 0;
	}
}
int nzk_ask(const char *title, const char *l1, const char *l2, const char *hints, const int *keys, int nkeys)
{
	struct nzui_event ev; int r;
	for (;;) {
		nzk__two_lines(title, l1, l2, hints); nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r != 1 || ev.value != 1) continue;          /* a key that is held down never answers */
		if (ev.key == KEY_ESC) return 0;
		for (int i = 0; i < nkeys; i++) if (ev.key == keys[i]) return ev.key;
	}
}
int nzk_confirm(const char *title, const char *l1, const char *l2, const char *action)
{
	static const int k[] = { KEY_ENTER }; char foot[48]; int r;
	snprintf(foot, sizeof(foot), "Enter:%s  Esc:cancel", action ? action : "yes");
	r = nzk_ask(title, l1, l2, foot, k, 1);
	return r < 0 ? -1 : r == KEY_ENTER;
}
int nzk_pick(const char *title, const char *const *items, int n, int *sel)
{
	struct nzk_list l = { n, sel ? *sel : 0, 0 }; struct nzui_event ev; int r; const int rows = NZK_ROWS + 1;
	for (;;) {
		nzk_list_fit(&l, rows);
		nzk_begin(); nzk_title(title, NULL);
		for (int i = 0; i < rows && l.top + i < n; i++)
			nzk_row(0, NZK_SUB_Y + i * NZK_ROW_H, nzui_w, l.top + i == l.sel, NZK_TEXT, NULL, items[l.top + i], NULL);
		nzk_scrollbar(nzui_w - 2, NZK_SUB_Y, rows * NZK_ROW_H, l.top, n, rows);
		nzk_footer("Enter:choose  Esc:back");
		nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r != 1 || !ev.value) continue;
		if (nzk_nav(ev.key)) { nzk_list_move(&l, nzk_nav(ev.key), rows); continue; }
		if (ev.value != 1) continue;
		if (ev.key == KEY_ENTER && n > 0) { if (sel) *sel = l.sel; return 1; }
		if (ev.key == KEY_ESC || ev.key == KEY_BACKSPACE) return 0;
	}
}
void nzk_progress(const char *title, const char *l1, const char *l2, int permille, const char *hints)
{
	nzk__two_lines(title, l1, l2, NULL);
	if (permille >= 0) {
		if (permille > 1000) permille = 1000;
		nzui_rect(8, 82, nzui_w - 16, 8, NZK_FOOT);
		nzui_rect(8, 82, (nzui_w - 16) * permille / 1000, 8, NZK_HI);
	}
	nzk_footer(hints ? hints : "");
	nzk_end();
}
int nzk_input(const char *title, const char *prompt, char *buf, size_t size)
{
	struct nzui_event ev; int r, len = (int)strlen(buf), cur = len, ch; const int vis = 36;
	for (;;) {
		int off = cur > vis - 1 ? cur - (vis - 1) : 0, bx = 8, by = 60;
		nzk_begin(); nzk_title(title, NULL);
		if (prompt) nzk_text(8, 44, NZK_TEXT, 38, prompt);
		nzui_rect(bx - 4, by - 4, vis * 8 + 8, 16, NZK_MUTED); nzui_rect(bx - 3, by - 3, vis * 8 + 6, 14, NZK_BG);
		for (int i = 0; i < vis && off + i < len; i++) {        /* typed text is ASCII: one cell per byte */
			unsigned char c = (unsigned char)buf[off + i];
			nzui_text(bx + i * 8, by, NZK_TEXT, "%c", c >= 32 && c < 127 ? c : '?');
		}
		nzui_rect(bx + (cur - off) * 8, by + 8, 8, 2, NZK_HI);  /* cursor: a bar under the cell */
		nzk_footer("Enter:OK  Esc:cancel");
		nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r != 1 || !ev.value) continue;
		switch (ev.key) {
		case KEY_ENTER: if (ev.value == 1) return 1; break;
		case KEY_ESC: if (ev.value == 1) return 0; break;
		case KEY_LEFT: if (cur > 0) cur--; break;
		case KEY_RIGHT: if (cur < len) cur++; break;
		case KEY_HOME: cur = 0; break;
		case KEY_END: cur = len; break;
		case KEY_BACKSPACE:
			if (cur > 0) { memmove(buf + cur - 1, buf + cur, (size_t)(len - cur) + 1); cur--; len--; }
			break;
		case KEY_DELETE:
			if (cur < len) { memmove(buf + cur, buf + cur + 1, (size_t)(len - cur)); len--; }
			break;
		default:
			if ((ch = nz_key_char(ev.key, nzk_shift)) && (size_t)len + 1 < size) {
				memmove(buf + cur + 1, buf + cur, (size_t)(len - cur) + 1); buf[cur++] = (char)ch; len++;
			}
		}
	}
}
int nzk_help(const char *title, const char *right, const char *const *lines, int n)
{
	struct nzui_event ev; int r;
	for (;;) {
		nzk_begin(); nzk_title(title, right);
		for (int i = 0; i < n && i < 12; i++) nzk_text(4, 16 + i * 11, NZK_TEXT2, NZK_COLS - 1, lines[i]);
		nzk_footer("Press a key");
		nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r == 1 && ev.value == 1 && ev.key != KEY_LEFTSHIFT && ev.key != KEY_RIGHTSHIFT) return 0;
	}
}
int nzk_options(const char *title, const struct nzk_option *o, int n, int *sel)
{
	struct nzk_list l = { n, sel ? *sel : 0, 0 }; struct nzui_event ev; int r, nav;
	for (;;) {
		nzk_list_fit(&l, NZK_ROWS);
		nzk_begin(); nzk_title(title, NULL);
		if (n > 0 && o[l.sel].help) nzk_subline(o[l.sel].help, 0);
		for (int i = 0; i < NZK_ROWS && l.top + i < n; i++) {
			int k = l.top + i, y = NZK_LIST_Y + i * NZK_ROW_H, s = k == l.sel; char v[40] = "";
			if (o[k].value) o[k].value(k, v, sizeof(v));
			if (s) nzui_rect(0, y - 1, nzui_w - 3, NZK_ROW_H, NZK_SEL);
			nzk_text(4, y, s ? NZK_HI : NZK_TEXT, 37 - (int)strlen(v), o[k].name);
			nzk_text(nzui_w - 6 - 8 * (int)strlen(v), y, s ? NZK_HI : NZK_LINK, 37, v);
		}
		nzk_scrollbar(nzui_w - 2, NZK_LIST_Y, NZK_ROWS * NZK_ROW_H, l.top, n, NZK_ROWS);
		nzk_footer("Z/C or Enter:change  Esc:back");
		nzk_end();
		if (sel) *sel = l.sel;
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) return -1;
		if (r != 1 || !ev.value || n < 1) { if (r == 1 && ev.value == 1 && n < 1) return 0; continue; }
		nav = nzk_nav(ev.key);
		if (nav == NZK_NAV_UP || nav == NZK_NAV_DOWN) nzk_list_move(&l, nav, NZK_ROWS);
		else if (nav == NZK_NAV_PGUP) { if (o[l.sel].change) o[l.sel].change(l.sel, -1); }
		else if (nav == NZK_NAV_PGDN || ev.key == KEY_ENTER || ev.key == KEY_SPACE) { if (o[l.sel].change) o[l.sel].change(l.sel, 1); }
		else if (ev.value == 1 && (ev.key == KEY_ESC || ev.key == KEY_BACKSPACE || ev.key == KEY_O || ev.key == KEY_Q)) return 0;
	}
}
#endif /* NZUI_KIT_IMPLEMENTATION */
#endif /* NZUI_KIT_H */
