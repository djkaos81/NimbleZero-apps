/* SPDX-License-Identifier: GPL-2.0-only
 * nz-term (NimbleTerm) - terminal for NimbleZero (M5Stack Cardputer Zero).
 *
 *   nz-term                       a login shell
 *   nz-term -d FOLDER             ... started in FOLDER
 *   nz-term -e PROGRAM [ARGS]     run PROGRAM instead of a shell; the terminal closes with it
 *
 * What it adds to the Linux text console: lines that scrolled away can be read again,
 * three text sizes (40, 53 or 80 columns), battery and clock, paste from the editor.
 * It behaves like an xterm with 256 colours as far as full-screen programs (nano, htop,
 * mc, vi, less) need it: TERM is xterm-256color.
 *
 * Every key goes to the program in the terminal, so the terminal's own functions are on
 * Fn+H (the "help" key code) or Ctrl+Alt+H: a menu.  Shift+Fn+L / Shift+Fn+M (Shift+PgUp /
 * PgDn) scroll back directly.
 *
 * Tests: NZT_HEADLESS, NZT_KEYS, NZT_CONFIG_DIR (see nzui_app.h).
 */
#define _GNU_SOURCE
#define NZUI_IMPLEMENTATION
#define NZUI_KIT_IMPLEMENTATION
#define NZUI_APP_IMPLEMENTATION
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include "nzui_app.h"
#include "nzui_fonts.h"
#include "cp437.h"

#define VERSION "0.1.0"
#define MAXC 80
#define MAXR 28
#define SB   1500                       /* lines kept after they scroll away */

enum { A_BOLD = 1, A_UNDER = 2, A_REV = 4 };
struct cell { unsigned char ch, attr; uint16_t fg, bg; };
static struct cell scr[2][MAXR][MAXC], (*S)[MAXC] = scr[0], sb[SB][MAXC];
static int sb_head, sb_n, sb_off;       /* scrollback ring; how many lines the view is scrolled back */
static int cols, rows, cx, cy, top, bot, wrapnext, alt;
static int m_appcur, m_wrap = 1, m_origin, m_insert, m_cursor = 1, m_paste;
static int cs[2], shift_out;            /* G0, G1: 1 = line drawing; which is in use */
static struct { int x, y, attr, fgi, bgi, cs0, cs1, so; uint16_t fg, bg; } saved[2];
static int attr, fgi = -1, bgi = -1; static uint16_t fg, bg;      /* fgi: palette index 0-15, -1 default, -2 direct */
static struct { int size, title; } cfg = { 1, 1 };
static const struct nzf_font *font; static int X0, Y0, scrolling;
static int master = -1; static pid_t child = -1; static int dirty = 1;

#define DEF_FG NZK_TEXT2
#define DEF_BG NZK_BG

static uint16_t pal(int i)
{
	static const unsigned char base[16][3] = { {0,0,0}, {205,60,60}, {80,200,100}, {210,180,50}, {70,120,230}, {190,90,200}, {60,190,200}, {200,200,200},
		{110,110,110}, {255,100,100}, {120,240,140}, {250,220,90}, {120,170,255}, {230,140,240}, {120,230,240}, {255,255,255} };
	if (i < 16) return NZUI_RGB(base[i][0], base[i][1], base[i][2]);
	if (i < 232) { int c = i - 16, r = c / 36, g = c / 6 % 6, b = c % 6; return NZUI_RGB(r ? 55 + r * 40 : 0, g ? 55 + g * 40 : 0, b ? 55 + b * 40 : 0); }
	{ int v = 8 + (i - 232) * 10; return NZUI_RGB(v, v, v); }
}
static struct cell blank(void) { struct cell c = { ' ', 0, DEF_FG, bgi == -1 ? DEF_BG : bg }; return c; }
static void clear_cells(struct cell *c, int n) { struct cell b = blank(); for (int i = 0; i < n; i++) c[i] = b; }

/* ---------------- geometry ---------------- */
static void set_size(int tell)
{
	int oc = cols, orows = rows, ncols, nrows;
	font = cfg.size == 0 ? &nzf_8x8 : cfg.size == 1 ? &nzf_6x8 : &nzf_4x6;
	Y0 = cfg.title ? NZK_TITLE_H : 0;
	ncols = nzui_w / font->w; nrows = (nzui_h - Y0) / font->h;
	if (ncols > MAXC) ncols = MAXC;
	if (nrows > MAXR) nrows = MAXR;
	X0 = (nzui_w - ncols * font->w) / 2;
	if (oc && (ncols != oc || nrows != orows)) {
		/* fewer rows: the lines above the cursor go to the scrollback so the cursor stays on screen */
		while (cy >= nrows) {
			if (!alt) { memcpy(sb[sb_head], S[0], sizeof(sb[0])); sb_head = (sb_head + 1) % SB; if (sb_n < SB) sb_n++; }
			memmove(S[0], S[1], sizeof(S[0]) * (MAXR - 1)); clear_cells(S[MAXR - 1], MAXC); cy--;
		}
		if (cx >= ncols) cx = ncols - 1;
	}
	cols = ncols; rows = nrows; top = 0; bot = rows - 1; wrapnext = 0; dirty = 1;
	if (tell && master >= 0) { struct winsize ws = { (unsigned short)rows, (unsigned short)cols, (unsigned short)(cols * font->w), (unsigned short)(rows * font->h) }; ioctl(master, TIOCSWINSZ, &ws); }
}

/* ---------------- the screen ---------------- */
static void scroll_up(int t, int b, int n, int keep)     /* keep: lines leaving the top of the main screen are kept */
{
	if (n > b - t + 1) n = b - t + 1;
	if (n <= 0) return;
	if (keep && t == 0 && !alt) for (int i = 0; i < n; i++) { memcpy(sb[sb_head], S[i], sizeof(sb[0])); sb_head = (sb_head + 1) % SB; if (sb_n < SB) sb_n++; if (sb_off && sb_off < sb_n) sb_off++; }
	memmove(S[t], S[t + n], sizeof(S[0]) * (size_t)(b - t + 1 - n));
	for (int i = b - n + 1; i <= b; i++) clear_cells(S[i], MAXC);
}
static void scroll_down(int t, int b, int n)
{
	if (n > b - t + 1) n = b - t + 1;
	if (n <= 0) return;
	memmove(S[t + n], S[t], sizeof(S[0]) * (size_t)(b - t + 1 - n));
	for (int i = t; i < t + n; i++) clear_cells(S[i], MAXC);
}
static void linefeed(void) { if (cy == bot) scroll_up(top, bot, 1, 1); else if (cy < rows - 1) cy++; }
static void moveto(int x, int y)
{
	int lo = m_origin ? top : 0, hi = m_origin ? bot : rows - 1;
	if (m_origin) y += top;
	cx = x < 0 ? 0 : x >= cols ? cols - 1 : x; cy = y < lo ? lo : y > hi ? hi : y; wrapnext = 0;
}
static int glyph_of(unsigned u)
{
	int lo = 0, hi = (int)(sizeof(cp437_map) / sizeof(cp437_map[0])) - 1;
	if (u < 0x80) return (int)u;
	while (lo <= hi) { int m = (lo + hi) / 2; if (cp437_map[m].u == u) return cp437_map[m].g; if (cp437_map[m].u < u) lo = m + 1; else hi = m - 1; }
	if (u >= 0x2800 && u <= 0x28FF) return u == 0x2800 ? ' ' : 0xFA;        /* braille dots (graphs): a dot */
	if (u >= 0x300 && u <= 0x36F) return -1;                                 /* combining accent: nothing */
	if (u == 0x200B || u == 0xFEFF || (u >= 0xFE00 && u <= 0xFE0F)) return -1;
	return '?';
}
static void put(unsigned u)
{
	static const char dec[] = "`afgjklmnopqrstuvwxyz{|}~0"; static const unsigned char to[] = { 0x04, 0xB1, 0xF8, 0xF1, 0xD9, 0xBF, 0xDA, 0xC0, 0xC5,
		0xC4, 0xC4, 0xC4, 0xC4, 0x5F, 0xC3, 0xB4, 0xC1, 0xC2, 0xB3, 0xF3, 0xF2, 0xE3, '=', 0x9C, 0xFA, 0xDB };
	int g; struct cell *c;
	if (cs[shift_out] && u >= 0x5F && u < 0x7F) { const char *p = strchr(dec, (int)u); g = p ? to[p - dec] : (int)u; if (u == 0x5F) g = ' '; }
	else if ((g = glyph_of(u)) < 0) return;
	if (wrapnext && m_wrap) { cx = 0; linefeed(); }
	wrapnext = 0;
	if (m_insert) memmove(&S[cy][cx + 1], &S[cy][cx], sizeof(S[0][0]) * (size_t)(cols - cx - 1));
	c = &S[cy][cx];
	c->ch = (unsigned char)g; c->attr = (unsigned char)attr;
	c->fg = fgi == -1 ? DEF_FG : fgi >= 0 ? pal(fgi < 8 && (attr & A_BOLD) ? fgi + 8 : fgi) : fg;
	c->bg = bgi == -1 ? DEF_BG : bg;
	if (cx == cols - 1) wrapnext = 1; else cx++;
}
static void save_cursor(void) { saved[alt].x = cx; saved[alt].y = cy; saved[alt].attr = attr; saved[alt].fgi = fgi; saved[alt].bgi = bgi; saved[alt].fg = fg; saved[alt].bg = bg; saved[alt].cs0 = cs[0]; saved[alt].cs1 = cs[1]; saved[alt].so = shift_out; }
static void restore_cursor(void)
{
	cx = saved[alt].x < cols ? saved[alt].x : cols - 1; cy = saved[alt].y < rows ? saved[alt].y : rows - 1;
	attr = saved[alt].attr; fgi = saved[alt].fgi; bgi = saved[alt].bgi; fg = saved[alt].fg; bg = saved[alt].bg;
	cs[0] = saved[alt].cs0; cs[1] = saved[alt].cs1; shift_out = saved[alt].so; wrapnext = 0;
}
static void set_alt(int on, int clear)
{
	if (on == alt) return;
	alt = on; S = scr[alt]; top = 0; bot = rows - 1; wrapnext = 0; sb_off = 0;
	if (on && clear) for (int i = 0; i < MAXR; i++) clear_cells(S[i], MAXC);
}
static void reset_term(void)
{
	attr = 0; fgi = bgi = -1; cs[0] = cs[1] = shift_out = 0;
	m_appcur = m_origin = m_insert = m_paste = 0; m_wrap = m_cursor = 1;
	alt = 0; S = scr[0];
	for (int a = 0; a < 2; a++) for (int i = 0; i < MAXR; i++) clear_cells(scr[a][i], MAXC);
	cx = cy = wrapnext = 0; top = 0; bot = rows - 1;
	memset(saved, 0, sizeof(saved)); saved[0].fgi = saved[0].bgi = saved[1].fgi = saved[1].bgi = -1;
}
static void reply(const char *s) { if (master >= 0 && write(master, s, strlen(s)) < 0) {} }

/* ---------------- escape sequences ---------------- */
static int par[16], npar; static char priv, inter;
static int P(int i, int def) { return i < npar && par[i] > 0 ? par[i] : def; }
static void sgr(void)
{
	if (!npar) { par[0] = 0; npar = 1; }
	for (int i = 0; i < npar; i++) {
		int p = par[i];
		if (p == 0) { attr = 0; fgi = bgi = -1; }
		else if (p == 1) attr |= A_BOLD;
		else if (p == 4) attr |= A_UNDER;
		else if (p == 7) attr |= A_REV;
		else if (p == 22) attr &= ~A_BOLD;
		else if (p == 24) attr &= ~A_UNDER;
		else if (p == 27) attr &= ~A_REV;
		else if (p >= 30 && p <= 37) fgi = p - 30;
		else if (p == 39) fgi = -1;
		else if (p >= 40 && p <= 47) { bgi = p - 40; bg = pal(bgi); }
		else if (p == 49) bgi = -1;
		else if (p >= 90 && p <= 97) fgi = p - 90 + 8;
		else if (p >= 100 && p <= 107) { bgi = p - 100 + 8; bg = pal(bgi); }
		else if (p == 38 || p == 48) {
			uint16_t c = 0; int ok = 0, idx = -2;
			if (i + 2 < npar && par[i + 1] == 5) { idx = par[i + 2] & 255; c = pal(idx); if (idx > 15) idx = -2; ok = 1; i += 2; }
			else if (i + 4 < npar && par[i + 1] == 2) { c = NZUI_RGB(par[i + 2] & 255, par[i + 3] & 255, par[i + 4] & 255); ok = 1; i += 4; }
			else break;
			if (ok && p == 38) { fgi = idx; fg = c; } else if (ok) { bgi = idx >= 0 ? idx : -2; bg = c; }
		}
	}
}
static void set_mode(int on)
{
	for (int i = 0; i < npar; i++) {
		int p = par[i];
		if (priv == '?') switch (p) {
			case 1: m_appcur = on; break;
			case 6: m_origin = on; moveto(0, 0); break;
			case 7: m_wrap = on; break;
			case 25: m_cursor = on; break;
			case 47: case 1047: set_alt(on, 1); break;
			case 1048: if (on) save_cursor(); else restore_cursor(); break;
			case 1049: if (on) { save_cursor(); set_alt(1, 1); } else { set_alt(0, 0); restore_cursor(); } break;
			case 2004: m_paste = on; break;
		} else if (p == 4) m_insert = on;
	}
}
static void csi(int f)
{
	int n = P(0, 1); char b[32];
	if (inter && !(inter == '!' && f == 'p')) return;          /* cursor shapes, window titles ...: nothing */
	if (priv && f != 'h' && f != 'l' && !(priv == '>' && f == 'c')) return;
	switch (f) {
	case 'A': { int lim = cy >= top ? top : 0; cy = cy - n < lim ? lim : cy - n; wrapnext = 0; } break;
	case 'B': case 'e': { int lim = cy <= bot ? bot : rows - 1, y = cy + n > lim ? lim : cy + n; cy = y; wrapnext = 0; } break;
	case 'C': case 'a': moveto(cx + n, cy - (m_origin ? top : 0)); break;
	case 'D': moveto(cx - n, cy - (m_origin ? top : 0)); break;
	case 'E': { int lim = cy <= bot ? bot : rows - 1; cy = cy + n > lim ? lim : cy + n; cx = 0; wrapnext = 0; } break;
	case 'F': { int lim = cy >= top ? top : 0; cy = cy - n < lim ? lim : cy - n; cx = 0; wrapnext = 0; } break;
	case 'G': case '`': moveto(n - 1, cy - (m_origin ? top : 0)); break;
	case 'd': moveto(cx, n - 1); break;
	case 'H': case 'f': moveto(P(1, 1) - 1, n - 1); break;
	case 'I': while (n-- > 0 && cx < cols - 1) cx = (cx / 8 + 1) * 8 < cols ? (cx / 8 + 1) * 8 : cols - 1; break;
	case 'Z': while (n-- > 0 && cx > 0) cx = (cx - 1) / 8 * 8; break;
	case 'J':
		if (P(0, 0) == 0) { clear_cells(&S[cy][cx], cols - cx); for (int i = cy + 1; i < rows; i++) clear_cells(S[i], MAXC); }
		else if (P(0, 0) == 1) { for (int i = 0; i < cy; i++) clear_cells(S[i], MAXC); clear_cells(S[cy], cx + 1); }
		else if (P(0, 0) == 2) for (int i = 0; i < rows; i++) clear_cells(S[i], MAXC);
		else if (P(0, 0) == 3) { sb_n = sb_head = sb_off = 0; }
		wrapnext = 0; break;
	case 'K':
		if (P(0, 0) == 0) clear_cells(&S[cy][cx], cols - cx); else if (P(0, 0) == 1) clear_cells(S[cy], cx + 1); else clear_cells(S[cy], MAXC);
		wrapnext = 0; break;
	case 'L': if (cy >= top && cy <= bot) { scroll_down(cy, bot, n); cx = 0; } break;
	case 'M': if (cy >= top && cy <= bot) { scroll_up(cy, bot, n, 0); cx = 0; } break;
	case 'P': if (n > cols - cx) n = cols - cx; memmove(&S[cy][cx], &S[cy][cx + n], sizeof(S[0][0]) * (size_t)(cols - cx - n)); clear_cells(&S[cy][cols - n], n); break;
	case '@': if (n > cols - cx) n = cols - cx; memmove(&S[cy][cx + n], &S[cy][cx], sizeof(S[0][0]) * (size_t)(cols - cx - n)); clear_cells(&S[cy][cx], n); break;
	case 'X': if (n > cols - cx) n = cols - cx; clear_cells(&S[cy][cx], n); break;
	case 'S': scroll_up(top, bot, n, 1); break;
	case 'T': scroll_down(top, bot, n); break;
	case 'b':       /* the character before the cursor again, n times */
		if (cx > 0 || wrapnext) {
			struct cell c = S[cy][wrapnext ? cx : cx - 1];
			for (; n > 0 && n < 2000; n--) { if (wrapnext && m_wrap) { cx = 0; linefeed(); } wrapnext = 0; S[cy][cx] = c; if (cx == cols - 1) wrapnext = 1; else cx++; }
		}
		break;
	case 'm': sgr(); break;
	case 'h': set_mode(1); break;
	case 'l': set_mode(0); break;
	case 'r': { int t = P(0, 1) - 1, bt = P(1, rows) - 1; if (bt >= rows) bt = rows - 1; if (t < bt) { top = t; bot = bt; moveto(0, 0); } } break;
	case 's': save_cursor(); break;
	case 'u': restore_cursor(); break;
	case 'n': if (P(0, 0) == 5) reply("\033[0n"); else if (P(0, 0) == 6) { snprintf(b, sizeof(b), "\033[%d;%dR", cy - (m_origin ? top : 0) + 1, cx + 1); reply(b); } break;
	case 'c': if (priv == '>') reply("\033[>0;10;1c"); else if (!P(0, 0) || !npar) reply("\033[?6c"); break;
	case 'p': if (inter == '!') { attr = 0; fgi = bgi = -1; m_appcur = m_origin = m_insert = 0; m_wrap = m_cursor = 1; top = 0; bot = rows - 1; cs[0] = cs[1] = shift_out = 0; } break;
	case 'g': case 't': case 'q': break;
	}
}
static void feed(const unsigned char *p, int len)
{
	static int st, u8left, osc_esc, digits; static unsigned u8; static char which;
	enum { GROUND, ESC, CSI, OSC, STRING, CHARSET, SKIP1 };
	for (; len > 0; len--, p++) {
		unsigned c = *p;
		if (st == OSC || st == STRING) {                /* a window title and the like: to its end, unseen */
			if (c == 7 && st == OSC) st = GROUND;
			else if (osc_esc && c == '\\') st = GROUND;
			else if (c == 0x18 || c == 0x1A) st = GROUND;
			osc_esc = c == 0x1B;
			continue;
		}
		if (c < 0x20 || c == 0x7F) {                    /* control characters work inside sequences too */
			u8left = 0;
			switch (c) {
			case 8: if (wrapnext) wrapnext = 0; else if (cx > 0) cx--; break;
			case 9: if (cx < cols - 1) cx = (cx / 8 + 1) * 8 < cols ? (cx / 8 + 1) * 8 : cols - 1; break;
			case 10: case 11: case 12: linefeed(); wrapnext = 0; break;
			case 13: cx = 0; wrapnext = 0; break;
			case 14: shift_out = 1; break;
			case 15: shift_out = 0; break;
			case 0x18: case 0x1A: st = GROUND; break;
			case 0x1B: st = ESC; break;
			}
			continue;
		}
		switch (st) {
		case GROUND:
			if (c < 0x80) { u8left = 0; put(c); }
			else if (u8left && (c & 0xC0) == 0x80) { u8 = u8 << 6 | (c & 0x3F); if (!--u8left) put(u8); }
			else if ((c & 0xE0) == 0xC0) { u8 = c & 0x1F; u8left = 1; }
			else if ((c & 0xF0) == 0xE0) { u8 = c & 0x0F; u8left = 2; }
			else if ((c & 0xF8) == 0xF0) { u8 = c & 0x07; u8left = 3; }
			else { u8left = 0; put('?'); }
			break;
		case ESC:
			st = GROUND;
			switch (c) {
			case '[': st = CSI; npar = 0; digits = 0; priv = inter = 0; memset(par, 0, sizeof(par)); break;
			case ']': st = OSC; osc_esc = 0; break;
			case 'P': case '^': case '_': case 'X': st = STRING; osc_esc = 0; break;
			case '(': case ')': st = CHARSET; which = (char)c; break;
			case '*': case '+': case '#': case ' ': case '%': st = SKIP1; break;
			case '7': save_cursor(); break;
			case '8': restore_cursor(); break;
			case 'D': linefeed(); break;
			case 'E': cx = 0; linefeed(); wrapnext = 0; break;
			case 'M': if (cy == top) scroll_down(top, bot, 1); else if (cy > 0) cy--; wrapnext = 0; break;
			case 'c': reset_term(); sb_n = sb_head = sb_off = 0; break;
			case '=': case '>': break;
			}
			break;
		case CSI:
			if (c >= '0' && c <= '9') { if (npar < 16) { if (!digits) npar++; digits = 1; if (par[npar - 1] < 100000) par[npar - 1] = par[npar - 1] * 10 + (int)(c - '0'); } }
			else if (c == ';' || c == ':') { if (!digits && npar < 16) npar++; digits = 0; }
			else if (c >= 0x3C && c <= 0x3F) priv = (char)c;
			else if (c >= 0x20 && c <= 0x2F) inter = (char)c;
			else if (c >= 0x40 && c <= 0x7E) { st = GROUND; csi((int)c); }
			else st = GROUND;
			break;
		case CHARSET: cs[which == ')'] = c == '0'; st = GROUND; break;
		case SKIP1: st = GROUND; break;
		}
	}
	dirty = 1;
}

/* ---------------- drawing ---------------- */
static void draw_cells(void)
{
	int fw = font->w, fh = font->h;
	nzui_rect(0, Y0, nzui_w, nzui_h - Y0, DEF_BG);
	for (int r = 0; r < rows; r++) {
		int k = r - sb_off; const struct cell *line = k >= 0 ? S[k] : sb[((sb_head + k) % SB + SB) % SB];
		for (int c = 0; c < cols; c++) {
			const struct cell *e = &line[c]; uint16_t f = e->fg, b = e->bg;
			if (e->attr & A_REV) { uint16_t t = f; f = b; b = t; }
			if (m_cursor && !sb_off && k == cy && c == cx) { f = NZK_BG; b = NZK_HI; }
			if (e->ch == ' ' && b == DEF_BG && !(e->attr & A_UNDER)) continue;
			nzf_cell(font, X0 + c * fw, Y0 + r * fh, e->ch, f, b);
			if (e->attr & A_UNDER) nzui_rect(X0 + c * fw, Y0 + r * fh + fh - 1, fw, 1, f);
		}
	}
}
static void draw(void)
{
	if (cfg.title) { char t[24] = "Fn+H:menu"; if (sb_off) snprintf(t, sizeof(t), "-%d lines", sb_off); nzk_scrolling = 0; nzk_title("NimbleTerm", t); }
	draw_cells();
	if (scrolling) nzk_footer("F/X:line  Z/C:page  Esc:back");
	nzk_end();
	dirty = 0;
}

/* ---------------- keys ---------------- */
static void send(const char *s, size_t n)
{
	while (n > 0 && master >= 0) {
		ssize_t k = write(master, s, n);
		if (k > 0) { s += k; n -= (size_t)k; }
		else if (k < 0 && errno == EAGAIN) { unsigned char b[4096]; ssize_t r = read(master, b, sizeof(b)); if (r > 0) feed(b, (int)r); else usleep(2000); }   /* it is waiting for us to read */
		else if (k < 0 && errno != EINTR) break;
	}
}
static void key_to_program(const struct nzui_event *ev)
{
	char b[16]; int n = 0, c; const char *seq = NULL; static const char *const fn[] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~" };
	switch (ev->key) {
	case KEY_ENTER: seq = "\r"; break;
	case KEY_BACKSPACE: seq = nza_ctrl ? "\b" : "\177"; break;
	case KEY_TAB: seq = nzk_shift ? "\033[Z" : "\t"; break;
	case KEY_ESC: seq = "\033"; break;
	case KEY_UP: seq = m_appcur ? "\033OA" : "\033[A"; break;
	case KEY_DOWN: seq = m_appcur ? "\033OB" : "\033[B"; break;
	case KEY_RIGHT: seq = m_appcur ? "\033OC" : "\033[C"; break;
	case KEY_LEFT: seq = m_appcur ? "\033OD" : "\033[D"; break;
	case KEY_HOME: seq = m_appcur ? "\033OH" : "\033[H"; break;
	case KEY_END: seq = m_appcur ? "\033OF" : "\033[F"; break;
	case KEY_INSERT: seq = "\033[2~"; break;
	case KEY_DELETE: seq = "\033[3~"; break;
	case KEY_PAGEUP: seq = "\033[5~"; break;
	case KEY_PAGEDOWN: seq = "\033[6~"; break;
	case KEY_F11: seq = "\033[23~"; break;
	case KEY_F12: seq = "\033[24~"; break;
	}
	if (!seq && ev->key >= KEY_F1 && ev->key <= KEY_F10) seq = fn[ev->key - KEY_F1];
	if (seq) { if (nza_alt && seq[0] != '\033') b[n++] = '\033'; n += snprintf(b + n, sizeof(b) - (size_t)n, "%s", seq); send(b, (size_t)n); return; }
	if (!(c = nz_key_char(ev->key, 0))) return;
	if (c >= 'a' && c <= 'z' && (nzk_shift != nza_caps)) c -= 32;
	if (nza_ctrl) {
		if (c >= 'a' && c <= 'z') c -= 96; else if (c >= '@' && c <= '_') c -= 64; else if (c == ' ' || c == '2') c = 0; else if (c == '?') c = 127;
		else if (c >= '3' && c <= '7') c = 27 + c - '3'; else if (c == '8') c = 127;
	}
	if (nza_alt) b[n++] = '\033';
	b[n++] = (char)c;
	send(b, (size_t)n);
}
static void paste(void)
{
	char p[PATH_MAX], *t; const char *r = getenv("XDG_RUNTIME_DIR"); int fd; struct stat st;
	if (r && *r) snprintf(p, sizeof(p), "%s/nz-clipboard", r); else snprintf(p, sizeof(p), "/tmp/nz-clipboard-%u", (unsigned)getuid());
	if ((fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) return;
	if (!fstat(fd, &st) && st.st_size > 0 && st.st_size <= (1 << 20) && (t = malloc((size_t)st.st_size))) {
		ssize_t k = read(fd, t, (size_t)st.st_size);
		for (ssize_t i = 0; i < k; i++) if (t[i] == '\n') t[i] = '\r';
		if (k > 0 && t[k - 1] == '\r' && !memchr(t, '\r', (size_t)k - 1)) k--;     /* one line: do not run it yet */
		if (k > 0) { if (m_paste) send("\033[200~", 6); send(t, (size_t)k); if (m_paste) send("\033[201~", 6); }
		free(t);
	}
	close(fd);
}

/* ---------------- settings, menu ---------------- */
static void cfg_load(void)
{
	char p[PATH_MAX], line[80]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-term"));
	if (!(f = fopen(p, "r"))) return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "size=", 5)) cfg.size = atoi(line + 5) >= 0 && atoi(line + 5) <= 2 ? atoi(line + 5) : 1;
		else if (!strncmp(line, "title=", 6)) cfg.title = !!atoi(line + 6);
	}
	fclose(f);
}
static void cfg_save(void)
{
	char p[PATH_MAX]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-term"));
	if ((f = fopen(p, "w"))) { fprintf(f, "size=%d\ntitle=%d\n", cfg.size, cfg.title); fclose(f); }
}
static int do_help(void)
{
	static const char *const l[] = {
		"Fn+H or Ctrl+Alt+H   this menu",
		"Shift+Fn+L / Shift+Fn+M",
		"        scroll back / forward a page",
		"While scrolled back: F X Z C move,",
		"        any other key returns",
		"Fn + F X Z C   arrow keys",
		"Fn + K N L M   Home End PgUp PgDn",
		"Fn + 1 ... 0   F1 ... F10",
		"Fn+Backspace   Delete",
		"The key at the top left is ` and",
		"        with Fn it is Esc",
	};
	return nzk_help("Keys", "v" VERSION, l, 11);
}
/* 0 go on, 1 close the terminal, -1 leave */
static int do_menu(void)
{
	enum { M_SCROLL, M_PASTE, M_SIZE, M_TITLE, M_KEYS, M_CLOSE };
	static const char *const sizes[] = { "Large, 40 columns", "Medium, 53 columns", "Small, 80 columns" };
	static int sel; char size[48], title[48]; const char *items[6]; int r;
	for (;;) {
		snprintf(size, sizeof(size), "Text size: %s", sizes[cfg.size]);
		snprintf(title, sizeof(title), "Title bar: %s", cfg.title ? "shown" : "not shown");
		items[M_SCROLL] = "Scroll back"; items[M_PASTE] = "Paste"; items[M_SIZE] = size; items[M_TITLE] = title; items[M_KEYS] = "Keys"; items[M_CLOSE] = "Close the terminal";
		if ((r = nzk_pick("NimbleTerm", items, 6, &sel)) <= 0) return r;
		switch (sel) {
		case M_SCROLL: scrolling = 1; if (!sb_off && sb_n) sb_off = 1; return 0;
		case M_PASTE: paste(); return 0;
		case M_SIZE: cfg.size = (cfg.size + 1) % 3; set_size(1); cfg_save(); break;
		case M_TITLE: cfg.title = !cfg.title; set_size(1); cfg_save(); break;
		case M_KEYS: if (do_help() < 0) return -1; break;
		case M_CLOSE:
			if ((r = nzk_confirm("Close the terminal", "Programs running in it are ended.", NULL, "close")) < 0) return -1;
			if (r) return 1;
			break;
		}
	}
}

static int start_child(const char *dir, char **cmd)
{
	struct winsize ws = { (unsigned short)rows, (unsigned short)cols, (unsigned short)(cols * font->w), (unsigned short)(rows * font->h) };
	if ((child = forkpty(&master, NULL, NULL, &ws)) < 0) return -1;
	if (child == 0) {
		static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGPIPE, SIGTERM, SIGHUP, SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGCHLD };
		const char *sh = getenv("SHELL"), *term = "xterm-256color"; struct passwd *pw = getpwuid(getuid()); char a0[64];
		static const char *const tdirs[] = { "/usr/share/terminfo", "/lib/terminfo", "/etc/terminfo", "/usr/lib/terminfo" };
		int have256 = 0, havex = 0;
		for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) signal(sigs[i], SIG_DFL);
		for (unsigned i = 0; i < 4; i++) { char p[96]; snprintf(p, sizeof(p), "%s/x/xterm-256color", tdirs[i]); if (!access(p, R_OK)) have256 = 1; snprintf(p, sizeof(p), "%s/x/xterm", tdirs[i]); if (!access(p, R_OK)) havex = 1; }
		if (!have256 && havex) term = "xterm";
		setenv("TERM", term, 1); setenv("NZ_TERM", VERSION, 1); unsetenv("COLUMNS"); unsetenv("LINES");
		if (dir) { if (chdir(dir)) {} }
		else {          /* started from the launcher, the folder is the program's own: the home folder is more use */
			char cwd[PATH_MAX]; const char *home = getenv("HOME");
			if (home && *home && (!getcwd(cwd, sizeof(cwd)) || !strncmp(cwd, "/opt/", 5))) { if (chdir(home)) {} }
		}
		if (cmd) { execvp(cmd[0], cmd); fprintf(stderr, "%s: %s\n", cmd[0], strerror(errno)); _exit(127); }
		if (!sh || !*sh) sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
		snprintf(a0, sizeof(a0), "-%s", strrchr(sh, '/') ? strrchr(sh, '/') + 1 : sh);      /* a login shell: it reads the profile */
		execl(sh, a0, (char *)NULL);
		execl("/bin/sh", "-sh", (char *)NULL);
		_exit(127);
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	fcntl(master, F_SETFD, FD_CLOEXEC);
	return 0;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
	const char *dir = NULL; char **cmd = NULL; struct nzui_event ev; int r = 0, ready, status = 0, ended = 0; double last_draw = 0, started;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) { printf("nz-term %s\n", VERSION); return 0; }
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
		else if (!strcmp(argv[i], "-e") && i + 1 < argc) { cmd = argv + i + 1; break; }
		else { fprintf(stderr, "usage: nz-term [-d FOLDER] [-e PROGRAM [ARGS]]\n"); return 2; }
	}
	if (nza_start("NimbleTerm", "NZT")) return 1;
	cfg_load(); set_size(0); reset_term();
	if (start_child(dir, cmd)) { nzk_message("Terminal", "The terminal could not be started.", strerror(errno)); nza_stop(); return 1; }
	nza_raw_keys(1);
	started = now_s();
	while (!ended) {
		double since = now_s() - last_draw; int to = dirty ? (since >= 0.03 ? 0 : (int)((0.03 - since) * 1000) + 1) : (cfg.title ? nzk_timeout() : 5000);
		if (dirty && since >= 0.03) { draw(); last_draw = now_s(); to = cfg.title ? nzk_timeout() : 5000; }
		if ((r = nza_wait_fd(&ev, to, master, &ready)) < 0) break;
		if (ready) {
			unsigned char b[8192]; ssize_t k; int total = 0;
			while (total < 65536 && (k = read(master, b, sizeof(b))) > 0) { feed(b, (int)k); total += (int)k; }
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) ended = 1;      /* the program is gone */
		}
		if (r == 0 && !ready && !dirty && cfg.title) dirty = 1;      /* the clock */
		if (r != 1 || !ev.value) continue;
		if (ev.key == KEY_LEFTSHIFT || ev.key == KEY_RIGHTSHIFT || ev.key == KEY_LEFTCTRL || ev.key == KEY_RIGHTCTRL || ev.key == KEY_LEFTALT || ev.key == KEY_RIGHTALT || ev.key == KEY_CAPSLOCK) continue;
		dirty = 1;
		if (ev.value == 1 && (ev.key == KEY_HELP || (nza_ctrl && nza_alt && ev.key == KEY_H))) {
			scrolling = 0; sb_off = 0;
			if ((r = do_menu()) < 0) break;
			if (r == 1) { r = 0; break; }
			continue;
		}
		if (nzk_shift && (ev.key == KEY_PAGEUP || ev.key == KEY_PAGEDOWN)) {
			sb_off += ev.key == KEY_PAGEUP ? rows - 1 : -(rows - 1);
			if (sb_off > sb_n) sb_off = sb_n;
			if (sb_off <= 0) { sb_off = 0; scrolling = 0; } else scrolling = 1;
			continue;
		}
		if (scrolling || sb_off) {
			int nav = nzk_nav(ev.key);
			if (nav == NZK_NAV_UP) sb_off++; else if (nav == NZK_NAV_DOWN) sb_off--; else if (nav == NZK_NAV_PGUP) sb_off += rows - 1; else if (nav == NZK_NAV_PGDN) sb_off -= rows - 1;
			if (sb_off > sb_n) sb_off = sb_n;
			if (!nav || sb_off <= 0) { sb_off = 0; scrolling = 0; }
			if (nav || ev.key == KEY_ESC || ev.key == KEY_ENTER || ev.key == KEY_Q) continue;       /* these only end the scrolling */
		}
		key_to_program(&ev);
	}
	nza_raw_keys(0);
	if (child > 0) {
		/* What still runs in it is told to end, as when a terminal window is closed: a hang-up to the
		 * shell and to the program in front; whatever is still there a second later is ended for good. */
		pid_t front = master >= 0 ? tcgetpgrp(master) : -1;
		if (master >= 0) close(master);
		if (!ended) { if (front > 1 && front != child) kill(-front, SIGHUP); kill(-child, SIGHUP); kill(-child, SIGCONT); }
		for (int i = 0; i < 75 && waitpid(child, &status, WNOHANG) == 0; i++) { if (i == 50) kill(-child, SIGKILL); usleep(20000); }
		if (ended && r >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 127 && now_s() - started < 3) { draw(); nzk_message("Terminal", cmd ? cmd[0] : "The shell", "could not be started"); }
	}
	cfg_save();
	nza_stop();
	return 0;
}
