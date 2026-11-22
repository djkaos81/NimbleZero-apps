/* SPDX-License-Identifier: GPL-2.0-only
 * nz-edit - text viewer and editor for NimbleZero (M5Stack Cardputer Zero).
 *
 *   nz-edit                 choose a file, edit it
 *   nz-edit FILE            edit FILE (it is made when saved if it does not exist)
 *   nz-edit --view FILE     read FILE; E changes to editing.  A file that is not text is
 *                           shown as bytes in hexadecimal.
 *   nz-edit +LINE FILE      start at that line
 *
 * Look and keys: nzui_kit.h and docs/STYLE.md of NimbleZero.  While editing, every letter
 * is text, so commands are Ctrl+letter and Esc opens a menu with all of them.
 * Tests: NZE_HEADLESS, NZE_KEYS, NZE_CONFIG_DIR (see nzui_app.h).
 */
#define _GNU_SOURCE
#define NZUI_IMPLEMENTATION
#define NZUI_KIT_IMPLEMENTATION
#define NZUI_APP_IMPLEMENTATION
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>
#include "nzui_app.h"
#include "nzui_fonts.h"

#define VERSION   "0.1.0"
#define FILE_MAX  (8 << 20)             /* larger files: the first 8M, read only */
#define UNDO_MAX  4000
#define HEXW      8

struct line { char *s; int len, cap; };
static struct line *L; static int nl, capl;
static int cy, cx, want = -1;           /* cursor: line, byte in it; column to keep when moving up/down */
static int top, topsub, hscroll;        /* first line on screen, first folded row of it; sideways scroll */
static int sel, my, mx;                 /* a selection runs from (my, mx) to the cursor */
static int modified, viewing, crlf, bom, final_nl = 1, cut_short, is_new;
static char path[PATH_MAX], fname[NAME_MAX + 1], find_text[64];
static unsigned char *hexbuf; static long hexlen; static int hextop;      /* a file that is not text */
static struct { int wrap, small, tab, indent; } cfg = { 1, 0, 4, 1 };
static char *clip; static int clipn;

static int COLS, ROWS, CW, RH, X0;
static void geometry(void)
{
	CW = cfg.small ? 6 : 8; RH = cfg.small ? 9 : 10; X0 = 4;
	COLS = (nzui_w - X0 - 4) / CW;                                  /* 39 or 52 */
	ROWS = (nzui_h - NZK_SUB_Y - NZK_FOOT_H - 1) / RH;              /* 14 or 16 */
}

/* ---------------- settings ---------------- */
static void cfg_load(void)
{
	char p[PATH_MAX], line[80]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-edit"));
	if (!(f = fopen(p, "r"))) return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '='); if (!v) continue;
		*v++ = 0;
		if (!strcmp(line, "wrap")) cfg.wrap = !!atoi(v);
		else if (!strcmp(line, "small")) cfg.small = !!atoi(v);
		else if (!strcmp(line, "tab")) cfg.tab = atoi(v) == 2 || atoi(v) == 8 ? atoi(v) : 4;
		else if (!strcmp(line, "indent")) cfg.indent = !!atoi(v);
	}
	fclose(f);
}
static void cfg_save(void)
{
	char p[PATH_MAX]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", nza_config_dir("nz-edit"));
	if (!(f = fopen(p, "w"))) return;
	fprintf(f, "wrap=%d\nsmall=%d\ntab=%d\nindent=%d\n", cfg.wrap, cfg.small, cfg.tab, cfg.indent);
	fclose(f);
}

/* ---------------- the text: an array of lines ---------------- */
static int line_room(struct line *l, int need)
{
	if (need <= l->cap) return 0;
	int c = l->cap ? l->cap : 32; char *s;
	while (c < need) c *= 2;
	if (!(s = realloc(l->s, (size_t)c))) return -1;
	l->s = s; l->cap = c;
	return 0;
}
static int lines_room(int need)
{
	if (need <= capl) return 0;
	int c = capl ? capl : 256; struct line *n;
	while (c < need) c *= 2;
	if (!(n = realloc(L, (size_t)c * sizeof(*n)))) return -1;
	L = n; capl = c;
	return 0;
}
static void text_free(void)
{
	for (int i = 0; i < nl; i++) free(L[i].s);
	free(L); L = NULL; nl = capl = 0;
	free(hexbuf); hexbuf = NULL; hexlen = 0;
}
/* insert n bytes (may contain line feeds) at (*y, *x); the position moves behind them */
static int raw_insert(int *y, int *x, const char *t, int n)
{
	while (n > 0) {
		const char *lf = memchr(t, '\n', (size_t)n); int k = lf ? (int)(lf - t) : n; struct line *l = &L[*y];
		if (line_room(l, l->len + k + 1)) return -1;
		memmove(l->s + *x + k, l->s + *x, (size_t)(l->len - *x));
		memcpy(l->s + *x, t, (size_t)k); l->len += k; *x += k; t += k; n -= k;
		if (lf) {                               /* split the line here */
			struct line nw = { NULL, 0, 0 };
			if (lines_room(nl + 1) || line_room(&nw, L[*y].len - *x + 1)) return -1;
			l = &L[*y];
			memcpy(nw.s, l->s + *x, (size_t)(l->len - *x)); nw.len = l->len - *x; l->len = *x;
			memmove(&L[*y + 2], &L[*y + 1], (size_t)(nl - *y - 1) * sizeof(L[0]));
			L[*y + 1] = nw; nl++; (*y)++; *x = 0; t++; n--;
		}
	}
	return 0;
}
/* remove n bytes forward from (y, x), a line end counting as one; they go to out if given */
static void raw_delete(int y, int x, int n, char *out)
{
	while (n > 0) {
		struct line *l = &L[y]; int k = l->len - x < n ? l->len - x : n;
		if (k > 0) {
			if (out) { memcpy(out, l->s + x, (size_t)k); out += k; }
			memmove(l->s + x, l->s + x + k, (size_t)(l->len - x - k)); l->len -= k; n -= k;
		} else if (y + 1 < nl) {                /* join with the next line */
			struct line *nx = &L[y + 1];
			if (line_room(l, l->len + nx->len + 1)) return;
			memcpy(l->s + l->len, nx->s, (size_t)nx->len); l->len += nx->len;
			free(nx->s); memmove(&L[y + 1], &L[y + 2], (size_t)(nl - y - 2) * sizeof(L[0])); nl--;
			if (out) *out++ = '\n';
			n--;
		} else break;
	}
}
static long dist(int y1, int x1, int y2, int x2)        /* bytes from the first position to the second */
{
	long d = 0;
	if (y1 == y2) return x2 - x1;
	d = L[y1].len - x1 + 1;
	for (int i = y1 + 1; i < y2; i++) d += L[i].len + 1;
	return d + x2;
}

/* ---------------- undo ---------------- */
struct edit { int del, y, x, n; char *t; };     /* del: the bytes were removed at (y, x); else inserted */
static struct edit undo[UNDO_MAX], redo[UNDO_MAX]; static int nundo, nredo, nomerge;
static void stack_clear(struct edit *s, int *n) { for (int i = 0; i < *n; i++) free(s[i].t); *n = 0; }
static void stack_push(struct edit *s, int *n, struct edit e)
{
	if (*n == UNDO_MAX) { free(s[0].t); memmove(s, s + 1, (UNDO_MAX - 1) * sizeof(s[0])); (*n)--; }
	s[(*n)++] = e;
}
static void record(int del, int y, int x, const char *t, int n)
{
	struct edit e = { del, y, x, n, NULL }, *p = nundo ? &undo[nundo - 1] : NULL;
	stack_clear(redo, &nredo);
	/* typing and backspacing one character after another is one step */
	if (p && !nomerge && n == 1 && *t != '\n' && p->del == del && p->n < 80 && p->y == y && !memchr(p->t, '\n', (size_t)p->n)) {
		char *g;
		if (!del && p->x + p->n == x && (g = realloc(p->t, (size_t)p->n + 1))) { g[p->n++] = *t; p->t = g; return; }
		if (del && p->x == x + 1 && (g = realloc(p->t, (size_t)p->n + 1))) { memmove(g + 1, g, (size_t)p->n); g[0] = *t; p->n++; p->x = x; p->t = g; return; }
		if (del && p->x == x && (g = realloc(p->t, (size_t)p->n + 1))) { g[p->n++] = *t; p->t = g; return; }
	}
	if (!(e.t = malloc((size_t)n + 1))) return;
	memcpy(e.t, t, (size_t)n);
	stack_push(undo, &nundo, e); nomerge = 0;
}
static void do_insert(const char *t, int n)             /* at the cursor */
{
	int y = cy, x = cx;
	if (n <= 0 || raw_insert(&cy, &cx, t, n)) return;
	record(0, y, x, t, n); modified = 1; want = -1;
}
static void do_delete(int y, int x, long n)             /* the cursor goes to (y, x) */
{
	char *t;
	if (n <= 0 || n > INT_MAX / 2 || !(t = malloc((size_t)n))) return;
	raw_delete(y, x, (int)n, t);
	record(1, y, x, t, (int)n); free(t);
	cy = y; cx = x; modified = 1; want = -1;
}
static void apply(struct edit *e, int inverse)          /* inverse: take the edit back */
{
	int y = e->y, x = e->x;
	if (e->del != inverse) { raw_delete(y, x, e->n, NULL); cy = y; cx = x; }
	else { raw_insert(&y, &x, e->t, e->n); cy = y; cx = x; }
	modified = 1; want = -1; sel = 0; nomerge = 1;
}
static void do_undo(void) { if (nundo) { struct edit e = undo[--nundo]; apply(&e, 1); stack_push(redo, &nredo, e); } }
static void do_redo(void) { if (nredo) { struct edit e = redo[--nredo]; apply(&e, 0); stack_push(undo, &nundo, e); } }

/* ---------------- clipboard: a file, so that other apps (the terminal) can paste it ---------------- */
static const char *clip_path(void)
{
	static char p[PATH_MAX]; const char *r = getenv("XDG_RUNTIME_DIR");
	if (r && *r) snprintf(p, sizeof(p), "%s/nz-clipboard", r); else snprintf(p, sizeof(p), "/tmp/nz-clipboard-%u", (unsigned)getuid());
	return p;
}
static void clip_set(const char *t, int n)
{
	int fd; char *c = malloc((size_t)n + 1);
	if (!c) return;
	memcpy(c, t, (size_t)n); free(clip); clip = c; clipn = n;
	if ((fd = open(clip_path(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600)) >= 0) { if (write(fd, t, (size_t)n) < 0) {} close(fd); }
}
static void clip_get(void)
{
	int fd = open(clip_path(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat st; char *c;
	if (fd < 0) return;
	if (!fstat(fd, &st) && st.st_size > 0 && st.st_size <= (1 << 20) && (c = malloc((size_t)st.st_size))) {
		ssize_t k = read(fd, c, (size_t)st.st_size);
		if (k > 0) { free(clip); clip = c; clipn = (int)k; } else free(c);
	}
	close(fd);
}

/* ---------------- columns: a tab is several cells, a UTF-8 character one ---------------- */
static int cells(const struct line *l, int upto)
{
	int c = 0;
	for (int i = 0; i < upto && i < l->len; i++) {
		unsigned char b = (unsigned char)l->s[i];
		if (b == '\t') c += cfg.tab - c % cfg.tab; else if ((b & 0xC0) != 0x80) c++;
	}
	return c;
}
static int byte_at(const struct line *l, int cell)     /* the character that covers this cell */
{
	int c = 0, i = 0;
	while (i < l->len) {
		unsigned char b = (unsigned char)l->s[i]; int w = b == '\t' ? cfg.tab - c % cfg.tab : 1, k = 1;
		if (c + w > cell) break;
		while (i + k < l->len && ((unsigned char)l->s[i + k] & 0xC0) == 0x80) k++;
		c += w; i += k;
	}
	return i;
}
static int rows_of(const struct line *l) { return cfg.wrap ? cells(l, l->len) / COLS + 1 : 1; }
static int next_char(const struct line *l, int x) { if (x < l->len) x++; while (x < l->len && ((unsigned char)l->s[x] & 0xC0) == 0x80) x++; return x; }
static int prev_char(const struct line *l, int x) { if (x > 0) x--; while (x > 0 && ((unsigned char)l->s[x] & 0xC0) == 0x80) x--; return x; }

/* one screen row down or up from (*y, *sub); 0 at the end */
static int row_step(int *y, int *sub, int d)
{
	if (d > 0) { if (*sub + 1 < rows_of(&L[*y])) (*sub)++; else if (*y + 1 < nl) { (*y)++; *sub = 0; } else return 0; }
	else { if (*sub > 0) (*sub)--; else if (*y > 0) { (*y)--; *sub = rows_of(&L[*y]) - 1; } else return 0; }
	return 1;
}
static void scroll_rows(int d) { while (d > 0 && row_step(&top, &topsub, 1)) d--; while (d < 0 && row_step(&top, &topsub, -1)) d++; }
static void clamp_top(void)                             /* no empty space below the last line */
{
	int y = top, s = topsub, below = 0;
	if (top >= nl) { top = nl - 1; topsub = 0; }
	if (topsub >= rows_of(&L[top])) topsub = rows_of(&L[top]) - 1;
	y = top; s = topsub;
	while (below < ROWS - 1 && row_step(&y, &s, 1)) below++;
	while (below < ROWS - 1 && row_step(&top, &topsub, -1)) below++;
}
static void show_cursor(void)                           /* scroll so that the cursor is on screen */
{
	int cc = cells(&L[cy], cx), csub = cfg.wrap ? cc / COLS : 0, y, s, k;
	if (!cfg.wrap) { if (cc < hscroll) hscroll = cc; if (cc >= hscroll + COLS) hscroll = cc - COLS + 1; } else hscroll = 0;
	if (cy < top || (cy == top && csub < topsub)) { top = cy; topsub = csub; return; }
	y = top; s = topsub;
	for (k = 0; k < ROWS; k++) { if (y == cy && s == csub) return; if (!row_step(&y, &s, 1)) return; }
	top = cy; topsub = csub;                        /* below the screen: make it the last row */
	for (k = 0; k < ROWS - 1; k++) if (!row_step(&top, &topsub, -1)) break;
}
static void move_rows(int d)
{
	while (d) {
		int cc = cells(&L[cy], cx), sub = cfg.wrap ? cc / COLS : 0, col = cfg.wrap ? cc % COLS : cc, y = cy, s = sub;
		if (want < 0) want = col;
		if (!cfg.wrap) { if (d > 0 ? cy + 1 >= nl : cy <= 0) break; y = cy + (d > 0 ? 1 : -1); s = 0; }
		else if (!row_step(&y, &s, d > 0 ? 1 : -1)) break;
		cy = y; cx = byte_at(&L[cy], s * (cfg.wrap ? COLS : 0) + want);
		if (cfg.wrap && cells(&L[cy], cx) / COLS != s) cx = prev_char(&L[cy], cx);     /* the end of a full row belongs to it */
		d += d > 0 ? -1 : 1;
	}
}

/* ---------------- files ---------------- */
static void set_path(const char *p)
{
	char tmp[PATH_MAX];
	snprintf(path, sizeof(path), "%s", p);
	snprintf(tmp, sizeof(tmp), "%s", p); snprintf(fname, sizeof(fname), "%s", basename(tmp));
}
static void text_new(void)
{
	text_free();
	lines_room(1); L[0] = (struct line){ NULL, 0, 0 }; line_room(&L[0], 1); nl = 1;
	cy = cx = top = topsub = hscroll = sel = modified = crlf = bom = cut_short = hextop = 0; final_nl = 1; want = -1;
	stack_clear(undo, &nundo); stack_clear(redo, &nredo);
}
/* 0 ok, -1 cannot be read (errno), 1 it is not text (hexbuf holds it) */
static int text_load(const char *p)
{
	int fd = open(p, O_RDONLY | O_CLOEXEC); unsigned char *b; long len = 0, start = 0; struct stat st; int y = 0, bin = 0;
	text_new(); is_new = 0;
	if (fd < 0) { if (errno == ENOENT) { is_new = 1; return 0; } return -1; }
	if (fstat(fd, &st) || S_ISDIR(st.st_mode)) { close(fd); errno = EISDIR; return -1; }
	if (!(b = malloc(FILE_MAX + 1))) { close(fd); errno = ENOMEM; return -1; }
	for (;;) {
		ssize_t n = read(fd, b + len, (size_t)(FILE_MAX + 1 - len));
		if (n <= 0) break;
		len += n;
		if (len > FILE_MAX) { len = FILE_MAX; cut_short = 1; break; }
	}
	close(fd);
	for (long i = 0; i < len && i < 8192; i++) if (!b[i]) bin = 1;
	if (bin) { hexbuf = b; hexlen = len; return 1; }
	if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { bom = 1; start = 3; }
	final_nl = len > start ? b[len - 1] == '\n' : 1;
	if (final_nl && len > start) len--;             /* the last line feed ends the last line, it does not start one */
	{       /* count the lines first: one allocation for the array */
		int n = 1; for (long i = start; i < len; i++) if (b[i] == '\n') n++;
		if (lines_room(n + 1)) { free(b); errno = ENOMEM; return -1; }
	}
	for (long i = start; i <= len; ) {
		unsigned char *lf = i < len ? memchr(b + i, '\n', (size_t)(len - i)) : NULL; long e = lf ? lf - b : len; int k = (int)(e - i);
		if (k > 0 && b[e - 1] == '\r' && (y == 0 || crlf)) { k--; crlf = 1; }
		if (y > 0) { L[y] = (struct line){ NULL, 0, 0 }; nl = y + 1; }
		if (line_room(&L[y], k + 1)) break;
		memcpy(L[y].s, b + i, (size_t)k); L[y].len = k;
		if (!lf) break;
		i = e + 1; y++;
	}
	free(b);
	return 0;
}
static int text_save(const char *p)                     /* 0 ok, -1 errno */
{
	char real[PATH_MAX], tmp[PATH_MAX + 16]; struct stat st; int have = 0, fd, direct = 0; FILE *f;
	if (realpath(p, real)) have = !stat(real, &st); else snprintf(real, sizeof(real), "%s", p);     /* a link: write its target */
	snprintf(tmp, sizeof(tmp), "%s.nz-edit-%d", real, (int)getpid());
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, have ? (st.st_mode & 0777) : 0644)) < 0) {
		/* no new file can be made next to it (a folder we may not write to): write into the file itself */
		if ((fd = open(real, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) return -1;
		direct = 1;
	}
	if (!(f = fdopen(fd, "w"))) { int e = errno; close(fd); if (!direct) unlink(tmp); errno = e; return -1; }
	if (bom) fputs("\xEF\xBB\xBF", f);
	for (int i = 0; i < nl && !(nl == 1 && !L[0].len); i++) {      /* a file with nothing in it stays empty */
		fwrite(L[i].s, 1, (size_t)L[i].len, f);
		if (i + 1 < nl || final_nl) fputs(crlf ? "\r\n" : "\n", f);
	}
	if (fflush(f) || ferror(f) || fsync(fd)) { int e = errno; fclose(f); if (!direct) unlink(tmp); errno = e; return -1; }
	if (!direct && have) { if (fchmod(fd, st.st_mode & 07777)) {} if (fchown(fd, st.st_uid, st.st_gid)) {} }
	if (fclose(f)) { int e = errno; if (!direct) unlink(tmp); errno = e; return -1; }
	if (!direct && rename(tmp, real)) { int e = errno; unlink(tmp); errno = e; return -1; }
	modified = 0; is_new = 0; nomerge = 1;
	return 0;
}

/* ---------------- drawing ---------------- */
static void put(int x, int y, int ch, uint16_t c)
{
	char s[2] = { (char)ch, 0 };
	if (cfg.small) nzf_text(&nzf_6x8, x, y, c, s, 1); else nzui_text(x, y, c, "%s", s);
}
static int in_sel(int y, int x)
{
	int ay = my, ax = mx, by = cy, bx = cx;
	if (!sel) return 0;
	if (ay > by || (ay == by && ax > bx)) { ay = cy; ax = cx; by = my; bx = mx; }
	return (y > ay || (y == ay && x >= ax)) && (y < by || (y == by && x < bx));
}
static void draw_row(int y, int sub, int py)
{
	const struct line *l = &L[y]; int first = cfg.wrap ? sub * COLS : hscroll, c = 0;
	for (int i = 0; i <= l->len; ) {
		unsigned char b = i < l->len ? (unsigned char)l->s[i] : ' '; int w = 1, k = 1, ch = b, s = in_sel(y, i);
		if (i < l->len) {
			if (b == '\t') { w = cfg.tab - c % cfg.tab; ch = ' '; }
			else if (b >= 0x80) {
				char one[8], a[8];
				while (i + k < l->len && k < 4 && ((unsigned char)l->s[i + k] & 0xC0) == 0x80) k++;
				memcpy(one, l->s + i, (size_t)k); one[k] = 0; nzk_ascii(a, sizeof(a), one); ch = a[0] ? a[0] : '?';
			} else if (b < 32 || b == 127) ch = '?';
		}       /* i == len: the line end, shown as selected when the selection goes on behind it */
		for (int j = 0; j < w; j++, c++) {
			int col = c - first;
			if (col < 0 || col >= COLS) continue;
			if (s) nzui_rect(X0 + col * CW, py - 1, CW, RH, NZK_BAR);
			if (ch != ' ' && j == 0) put(X0 + col * CW, py, ch, s ? NZK_TEXT : NZK_TEXT2);
		}
		if (c - first >= COLS) break;
		i += k;
	}
	if (!cfg.wrap && hscroll > 0) nzui_rect(0, py, 2, RH - 2, NZK_MUTED);                   /* more to the left */
	if (!cfg.wrap && cells(l, l->len) > hscroll + COLS) nzui_rect(nzui_w - 5, py, 2, RH - 2, NZK_MUTED);
}
static void draw_hex(void)
{
	long n = (hexlen + HEXW - 1) / HEXW; char right[16]; const int rows = (nzui_h - NZK_SUB_Y - NZK_FOOT_H - 1) / NZK_ROW_H;
	if (hextop > n - rows) hextop = (int)(n - rows);
	if (hextop < 0) hextop = 0;
	snprintf(right, sizeof(right), "%d%%", n > rows ? (int)((long long)hextop * 100 / (n - rows)) : 100);
	nzk_begin(); nzk_title(fname, right);
	for (int i = 0; i < rows && hextop + i < n; i++) {
		long a = (long)(hextop + i) * HEXW; int y = NZK_SUB_Y + i * NZK_ROW_H, o; char t[64], asc[HEXW + 1];
		o = snprintf(t, sizeof(t), "%06lx ", a);
		for (int j = 0; j < HEXW; j++) {
			if (a + j < hexlen) { o += snprintf(t + o, sizeof(t) - (size_t)o, "%02x ", hexbuf[a + j]); asc[j] = hexbuf[a + j] >= 32 && hexbuf[a + j] < 127 ? (char)hexbuf[a + j] : '.'; }
			else { o += snprintf(t + o, sizeof(t) - (size_t)o, "   "); asc[j] = ' '; }
		}
		asc[HEXW] = 0;
		nzui_text(4, y, NZK_MUTED, "%.6s", t); nzui_text(4 + 7 * 8, y, NZK_TEXT, "%s", t + 7); nzui_text(4 + 31 * 8, y, NZK_LINK, "%s", asc);
	}
	nzk_scrollbar(nzui_w - 2, NZK_SUB_Y, rows * NZK_ROW_H, hextop, (int)n, rows);
	nzk_footer("F/X:line  Z/C:page  Esc:back");
	nzk_end();
}
static void draw(void)
{
	char title[NAME_MAX + 4], right[24]; int y, s, total = 0, before = 0;
	if (hexbuf) { draw_hex(); return; }
	geometry();
	if (viewing) clamp_top(); else show_cursor();
	nzk_begin();
	if (viewing) {
		/* how far down, in lines (folded rows would have to be counted over the whole file) */
		snprintf(right, sizeof(right), "%d%%", nl > 1 ? (int)((long long)top * 100 / (nl - 1)) : 100);
		{ int yy = top, ss = topsub, k = 0; while (k < ROWS - 1 && row_step(&yy, &ss, 1)) k++; if (yy == nl - 1 && ss == rows_of(&L[yy]) - 1) snprintf(right, sizeof(right), "100%%"); }
		nzk_title(fname[0] ? fname : "New file", right);
	} else {
		snprintf(title, sizeof(title), "%s%s", modified ? "*" : "", fname[0] ? fname : "New file");
		snprintf(right, sizeof(right), "%d:%d", cy + 1, cells(&L[cy], cx) + 1);
		nzk_title(title, right);
	}
	y = top; s = topsub;
	for (int r = 0; r < ROWS; r++) {
		int py = NZK_SUB_Y + r * RH;
		draw_row(y, s, py);
		if (!viewing && y == cy && (cfg.wrap ? cells(&L[cy], cx) / COLS == s : 1)) {
			int col = cells(&L[cy], cx) - (cfg.wrap ? s * COLS : hscroll);
			if (col >= 0 && col < COLS) nzui_rect(X0 + col * CW, py + 8, CW, cfg.small ? 1 : 2, NZK_HI);
		}
		if (r == ROWS - 1 || !row_step(&y, &s, 1)) break;
	}
	total = nl; before = top;
	if (top || topsub || row_step(&y, &s, 1))       /* not everything fits on the screen */
		nzk_scrollbar(nzui_w - 2, NZK_SUB_Y, ROWS * RH, before, total + ROWS - 1, ROWS);
	if (viewing) nzk_footer(cut_short ? "F/X:line  Z/C:page  S:find" : "F/X:line  Z/C:page  S:find  E:edit");
	else nzk_footer(sel ? "Ctrl+C:copy  Ctrl+X:cut  Esc:menu" : "Ctrl+S:save  Ctrl+F:find  Esc:menu");
	nzk_end();
}

/* ---------------- actions ---------------- */
static int save_as(void);
static int save(void)                                   /* 1 saved, 0 not, -1 leave */
{
	char l2[80];
	if (!path[0]) return save_as();
	if (!text_save(path)) return 1;
	snprintf(l2, sizeof(l2), "%.38s", strerror(errno));
	return nzk_message("Save", "The file could not be saved.", l2) < 0 ? -1 : 0;
}
static int save_as(void)
{
	char name[PATH_MAX], full[PATH_MAX], dir[PATH_MAX]; struct stat st; int r;
	snprintf(name, sizeof(name), "%s", fname);
	if ((r = nzk_input("Save as", "File name:", name, NAME_MAX)) <= 0) return r;
	if (!name[0]) return 0;
	if (name[0] == '/') snprintf(full, sizeof(full), "%s", name);
	else {
		if (path[0]) { char t[PATH_MAX]; snprintf(t, sizeof(t), "%s", path); snprintf(dir, sizeof(dir), "%s", dirname(t)); }
		else if (!getcwd(dir, sizeof(dir))) snprintf(dir, sizeof(dir), "/tmp");
		snprintf(full, sizeof(full), "%s/%s", strcmp(dir, "/") ? dir : "", name);
	}
	if (strcmp(full, path) && !stat(full, &st)) {
		if (S_ISDIR(st.st_mode)) return nzk_message("Save as", name, "That is a folder") < 0 ? -1 : 0;
		if ((r = nzk_confirm("Save as", name, "This file is there already.", "replace")) <= 0) return r;
	}
	set_path(full);
	return save();
}
/* 1 go on (saved or thrown away), 0 stay, -1 leave the app */
static int ask_changes(void)
{
	static const int keys[] = { KEY_ENTER, KEY_D }; char l1[64]; int r;
	if (!modified) return 1;
	snprintf(l1, sizeof(l1), "%.24s has changes.", fname[0] ? fname : "New file");
	r = nzk_ask("Leave", l1, "Save them?", "Enter:save  D:don't save  Esc:back", keys, 2);
	if (r < 0) return -1;
	if (r == KEY_D) return 1;
	if (r == KEY_ENTER) return save();
	return 0;
}
static int find_from(int y, int x, int back)            /* 1 found: selected, cursor behind it */
{
	int n = (int)strlen(find_text);
	if (!n) return 0;
	for (int pass = 0; pass <= nl; pass++) {
		int ly = ((y + (back ? -pass : pass)) % nl + nl) % nl; const struct line *l = &L[ly];
		int from = pass == 0 ? x : back ? l->len : 0;
		if (!back) { for (int i = from; i + n <= l->len; i++) if (!strncasecmp(l->s + i, find_text, (size_t)n)) { my = ly; mx = i; cy = ly; cx = i + n; sel = 1; want = -1; return 1; } }
		else for (int i = (from < l->len - n ? from : l->len - n); i >= 0; i--) if (!strncasecmp(l->s + i, find_text, (size_t)n)) { my = ly; mx = i; cy = ly; cx = i + n; sel = 1; want = -1; return 1; }
	}
	return 0;
}
static void view_goto_cursor(void)                      /* viewing: bring the found text on screen, a few rows from the top */
{
	top = cy; topsub = cfg.wrap ? cells(&L[cy], mx) / COLS : 0;
	if (!cfg.wrap) { int c = cells(&L[cy], mx); hscroll = c >= COLS ? c - COLS / 2 : 0; }
	scroll_rows(-3);
}
static int do_find(int again)
{
	int r, y = cy, x = cx;
	if (!again || !find_text[0]) { if ((r = nzk_input("Find", "Text to find:", find_text, sizeof(find_text))) <= 0) return r; if (sel) { y = my; x = mx; } }
	if (!find_text[0]) return 0;
	if (viewing && !sel) { y = top; x = 0; }
	if (!find_from(y, x, 0)) { sel = 0; return nzk_message("Find", find_text, "This text is not in the file") < 0 ? -1 : 0; }
	if (viewing) view_goto_cursor();
	return 0;
}
static int do_goto(void)
{
	char b[12] = ""; int r, n;
	if ((r = nzk_input("Go to line", "Line number:", b, sizeof(b))) <= 0) return r;
	n = atoi(b); if (n < 1) n = 1; if (n > nl) n = nl;
	cy = n - 1; cx = 0; sel = 0; want = -1;
	if (viewing) { top = cy; topsub = 0; }
	return 0;
}
static void sel_range(int *y, int *x, long *n)
{
	int ay = my, ax = mx, by = cy, bx = cx;
	if (ay > by || (ay == by && ax > bx)) { ay = cy; ax = cx; by = my; bx = mx; }
	*y = ay; *x = ax; *n = dist(ay, ax, by, bx);
}
static void do_copy(int cut)                            /* the selection, or the whole line */
{
	int y, x; long n; char *t;
	if (sel) sel_range(&y, &x, &n);
	else { y = cy; x = 0; n = L[cy].len + (cy + 1 < nl ? 1 : 0); }
	if (n <= 0 || n > (1 << 20) || !(t = malloc((size_t)n + 1))) { sel = 0; return; }
	{       /* copy without changing the text */
		int yy = y, xx = x; long k = 0;
		while (k < n) { if (xx < L[yy].len) t[k++] = L[yy].s[xx++]; else { t[k++] = '\n'; yy++; xx = 0; } }
	}
	if (!sel && cy + 1 >= nl) { t[n++] = '\n'; clip_set(t, (int)n); n--; } else clip_set(t, (int)n);
	free(t);
	if (cut) do_delete(y, x, n);
	sel = 0;
}
static void do_paste(void)
{
	clip_get();
	if (clip && clipn) { nomerge = 1; do_insert(clip, clipn); nomerge = 1; }
	sel = 0;
}
static void opt_value(int i, char *b, size_t n)
{
	if (i == 0) snprintf(b, n, "%s", cfg.wrap ? "Fold" : "Scroll sideways");
	else if (i == 1) snprintf(b, n, "%s", cfg.small ? "Small" : "Normal");
	else if (i == 2) snprintf(b, n, "%d", cfg.tab);
	else snprintf(b, n, "%s", cfg.indent ? "On" : "Off");
}
static void opt_change(int i, int d)
{
	if (i == 0) cfg.wrap = !cfg.wrap; else if (i == 1) cfg.small = !cfg.small;
	else if (i == 2) cfg.tab = d > 0 ? (cfg.tab == 2 ? 4 : cfg.tab == 4 ? 8 : 2) : (cfg.tab == 8 ? 4 : cfg.tab == 4 ? 2 : 8);
	else cfg.indent = !cfg.indent;
}
static int do_options(void)
{
	static const struct nzk_option o[] = {
		{ "Long lines", "Fold: go on in the next row", opt_value, opt_change },
		{ "Text size", "Normal: 39 columns  Small: 52", opt_value, opt_change },
		{ "Tab width", "Columns from one tab stop to the next", opt_value, opt_change },
		{ "Keep the indent", "A new line starts under the one above", opt_value, opt_change },
	};
	static int s; int r = nzk_options("Options", o, 4, &s);
	cfg_save(); geometry(); hscroll = 0; topsub = 0; want = -1;
	return r;
}
static int do_help(void)
{
	static const char *const ed[] = {
		"Arrows (Fn + F X Z C)   move",
		"Fn+K / Fn+N   start / end of line",
		"Fn+L / Fn+M   page up / down",
		"Ctrl+S  save        Ctrl+Q  leave",
		"Ctrl+F  find        Ctrl+G  find next",
		"Ctrl+L  go to line  Ctrl+A  select all",
		"Ctrl+B  select: move, then",
		"Ctrl+C / X / V   copy / cut / paste",
		"   (no selection: the whole line)",
		"Ctrl+Z  undo        Ctrl+Y  redo",
		"Ctrl+O  options     Esc  menu",
		"Fn+Backspace  delete to the right",
	};
	static const char *const vw[] = {
		"F / X   one row up / down",
		"Z / C   one page up / down",
		"T / B   top / bottom of the file",
		"S       find         N  find next",
		"G       go to line",
		"E       edit this file",
		"A / D   sideways (lines not folded)",
		"O       options      H  this page",
		"Esc     back",
	};
	return viewing ? nzk_help("Keys", "v" VERSION, vw, 9) : nzk_help("Keys", "v" VERSION, ed, 12);
}

static int accept_any(const char *n) { (void)n; return 1; }
/* editing one file until the user leaves it.  1: left the file, -1: leave the app */
static int edit_loop(void)
{
	struct nzui_event ev; int r, ch;
	for (;;) {
		draw();
		if ((r = nza_wait(&ev, nzk_timeout())) < 0) {
			/* told to stop (power switch, the launcher's Ctrl+Alt+Backspace) with changes: keep them next to the file */
			if (modified && !viewing && path[0]) { char u[PATH_MAX + 16]; snprintf(u, sizeof(u), "%s.unsaved", path); text_save(u); }
			return -1;
		}
		if (r != 1 || !ev.value) continue;
		r = 0;
		if (hexbuf) {
			const int rows = (nzui_h - NZK_SUB_Y - NZK_FOOT_H - 1) / NZK_ROW_H;
			switch (nzk_nav(ev.key)) {
			case NZK_NAV_UP: hextop--; continue;
			case NZK_NAV_DOWN: hextop++; continue;
			case NZK_NAV_PGUP: hextop -= rows - 1; continue;
			case NZK_NAV_PGDN: hextop += rows - 1; continue;
			}
			if (ev.value == 1 && (ev.key == KEY_ESC || ev.key == KEY_BACKSPACE || ev.key == KEY_Q || ev.key == KEY_ENTER)) return 1;
			if (ev.key == KEY_HOME || ev.key == KEY_T) hextop = 0;
			if (ev.key == KEY_END || ev.key == KEY_B) hextop = INT_MAX / 2;
			continue;
		}
		if (viewing) {
			switch (nzk_nav(ev.key)) {
			case NZK_NAV_UP: scroll_rows(-1); continue;
			case NZK_NAV_DOWN: scroll_rows(1); continue;
			case NZK_NAV_PGUP: scroll_rows(-(ROWS - 1)); continue;
			case NZK_NAV_PGDN: scroll_rows(ROWS - 1); continue;
			}
			if (ev.key == KEY_HOME || ev.key == KEY_T) { top = topsub = 0; continue; }
			if (ev.key == KEY_END || ev.key == KEY_B) { top = nl - 1; topsub = 0; continue; }
			if (!cfg.wrap && ev.key == KEY_A) { hscroll = hscroll > 8 ? hscroll - 8 : 0; continue; }
			if (!cfg.wrap && ev.key == KEY_D) { hscroll += 8; continue; }
			if (ev.value != 1) continue;
			switch (ev.key) {
			case KEY_ESC: case KEY_BACKSPACE: case KEY_Q: case KEY_3: return 1;
			case KEY_S: r = do_find(0); break;
			case KEY_N: r = do_find(1); break;
			case KEY_G: r = do_goto(); break;
			case KEY_O: r = do_options(); break;
			case KEY_H: r = do_help(); break;
			case KEY_E: case KEY_4:
				if (cut_short) r = nzk_message("Edit", "This file is too large to edit.", "The first 8M are shown");
				else { viewing = 0; nza_raw_keys(1); if (!sel) { cy = top; cx = 0; } sel = 0; want = -1; }
				break;
			}
			if (r < 0) return -1;
			continue;
		}
		/* ---- editing ---- */
		if (nza_ctrl && ev.value == 1) {
			switch (ev.key) {
			case KEY_S: r = save(); break;
			case KEY_Q: if ((r = ask_changes()) == 1) return 1; break;
			case KEY_F: r = do_find(0); break;
			case KEY_G: case KEY_N: r = do_find(1); break;
			case KEY_L: r = do_goto(); break;
			case KEY_O: r = do_options(); break;
			case KEY_H: r = do_help(); break;
			case KEY_Z: do_undo(); break;
			case KEY_Y: do_redo(); break;
			case KEY_B: if (sel) sel = 0; else { sel = 1; my = cy; mx = cx; } break;
			case KEY_A: my = mx = 0; cy = nl - 1; cx = L[cy].len; sel = 1; want = -1; break;
			case KEY_C: do_copy(0); break;
			case KEY_X: case KEY_K: do_copy(1); break;
			case KEY_V: case KEY_U: do_paste(); break;
			}
			if (r < 0) return -1;
			continue;
		}
		if (nza_ctrl || nza_alt) continue;
		switch (ev.key) {
		case KEY_UP: move_rows(-1); continue;
		case KEY_DOWN: move_rows(1); continue;
		case KEY_PAGEUP: move_rows(-(ROWS - 1)); continue;
		case KEY_PAGEDOWN: move_rows(ROWS - 1); continue;
		case KEY_LEFT: if (cx > 0) cx = prev_char(&L[cy], cx); else if (cy > 0) { cy--; cx = L[cy].len; } want = -1; nomerge = 1; continue;
		case KEY_RIGHT: if (cx < L[cy].len) cx = next_char(&L[cy], cx); else if (cy + 1 < nl) { cy++; cx = 0; } want = -1; nomerge = 1; continue;
		case KEY_HOME: cx = 0; want = -1; nomerge = 1; continue;
		case KEY_END: cx = L[cy].len; want = -1; nomerge = 1; continue;
		case KEY_BACKSPACE:
			if (sel) { int y, x; long n; sel_range(&y, &x, &n); do_delete(y, x, n); sel = 0; }
			else if (cx > 0) { int p = prev_char(&L[cy], cx); do_delete(cy, p, cx - p); }
			else if (cy > 0) do_delete(cy - 1, L[cy - 1].len, 1);
			continue;
		case KEY_DELETE:
			if (sel) { int y, x; long n; sel_range(&y, &x, &n); do_delete(y, x, n); sel = 0; }
			else if (cx < L[cy].len) do_delete(cy, cx, next_char(&L[cy], cx) - cx);
			else if (cy + 1 < nl) do_delete(cy, cx, 1);
			continue;
		case KEY_ENTER: {
			char t[130] = "\n"; int k = 1;
			if (cfg.indent) for (int i = 0; i < L[cy].len && i < cx && k < 128 && (L[cy].s[i] == ' ' || L[cy].s[i] == '\t'); i++) t[k++] = L[cy].s[i];
			sel = 0; nomerge = 1; do_insert(t, k); nomerge = 1;
			continue; }
		case KEY_TAB: sel = 0; do_insert("\t", 1); continue;
		}
		if (ev.key == KEY_ESC && ev.value == 1) {
			enum { M_SAVE, M_SAVEAS, M_FIND, M_NEXT, M_GOTO, M_SEL, M_COPY, M_CUT, M_PASTE, M_UNDO, M_OPT, M_KEYS, M_LEAVE };
			static const char *const items[] = { "Save", "Save as", "Find", "Find next", "Go to line", "Start or end a selection",
				"Copy (selection or line)", "Cut (selection or line)", "Paste", "Undo", "Options", "Keys", "Leave" };
			static int msel;
			if ((r = nzk_pick(fname[0] ? fname : "New file", items, 13, &msel)) < 0) return -1;
			if (!r) continue;
			r = 0;
			switch (msel) {
			case M_SAVE: r = save(); break;
			case M_SAVEAS: r = save_as(); break;
			case M_FIND: r = do_find(0); break;
			case M_NEXT: r = do_find(1); break;
			case M_GOTO: r = do_goto(); break;
			case M_SEL: if (sel) sel = 0; else { sel = 1; my = cy; mx = cx; } break;
			case M_COPY: do_copy(0); break;
			case M_CUT: do_copy(1); break;
			case M_PASTE: do_paste(); break;
			case M_UNDO: do_undo(); break;
			case M_OPT: r = do_options(); break;
			case M_KEYS: r = do_help(); break;
			case M_LEAVE: if ((r = ask_changes()) == 1) return 1; break;
			}
			if (r < 0) return -1;
			continue;
		}
		if ((ch = nza_char(&ev))) { char c = (char)ch; sel = 0; do_insert(&c, 1); }
	}
}
static int open_file(const char *p, int view, int line)
{
	char l2[80]; int r;
	set_path(p); viewing = view;
	if ((r = text_load(p)) < 0) { snprintf(l2, sizeof(l2), "%.38s", strerror(errno)); text_new(); return nzk_message(view ? "View" : "Edit", fname, l2) < 0 ? -1 : 0; }
	if (r == 1) viewing = 1;
	else if (cut_short) { viewing = 1; if (nzk_message("View", fname, "Large file: the first 8M, read only") < 0) return -1; }
	else if (r == 0 && !view && access(p, W_OK) && !is_new) { if (nzk_message("Edit", fname, "You may not change this file. Read only") < 0) return -1; viewing = 1; }
	geometry();
	if (line > 0 && !hexbuf) { cy = line > nl ? nl - 1 : line - 1; top = cy; }
	nza_raw_keys(!viewing);
	r = edit_loop();
	nza_raw_keys(0);
	return r;
}

int main(int argc, char **argv)
{
	const char *file = NULL; int view = 0, line = 0, r = 0; char dir[PATH_MAX], chosen[PATH_MAX];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) { printf("nz-edit %s\n", VERSION); return 0; }
		else if (!strcmp(argv[i], "--view") || !strcmp(argv[i], "-v")) view = 1;
		else if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) line = atoi(argv[i] + 1);
		else file = argv[i];
	}
	if (nza_start("Text Editor", "NZE")) return 1;
	cfg_load(); geometry(); text_new();
	if (file) r = open_file(file, view, line);
	else {
		const char *home = getenv("HOME");
		if (!getcwd(dir, sizeof(dir)) || !strcmp(dir, "/opt/nz-edit")) snprintf(dir, sizeof(dir), "%s", home && *home ? home : "/");
		while (r >= 0) {
			r = nza_pick_file("Text Editor", dir, sizeof(dir), chosen, sizeof(chosen), accept_any, 1, "Enter:open  N:new file", KEY_N);
			if (r < 0) break;
			if (r == 2) {
				char name[NAME_MAX + 1] = "";
				if ((r = nzk_input("New file", "File name:", name, sizeof(name))) < 0) break;
				if (!r || !name[0] || strchr(name, '/')) continue;
				snprintf(chosen, sizeof(chosen), "%s/%s", strcmp(dir, "/") ? dir : "", name);
				r = 1;
			}
			if (r == 1) r = open_file(chosen, 0, 0);
		}
	}
	cfg_save(); text_free();
	nza_stop();
	return 0;
}
