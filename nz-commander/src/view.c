/* SPDX-License-Identifier: GPL-2.0-only
 * NimbleCommander - file viewer: text with long lines folded, or bytes in hexadecimal when
 * the file is not text.  Shows the first megabyte of a larger file.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include "nzc.h"

#define VIEW_MAX (1 << 20)
#define VROWS    ((nzui_h - NZK_SUB_Y - NZK_FOOT_H - 1) / NZK_ROW_H)    /* 14 */
#define VCOLS    39
#define HEXW     8

int view_file(const char *path, const char *name)
{
	unsigned char *buf; long len = 0, *line = NULL, nline = 0, alloc = 0; int fd, hex = 0, cut = 0, r;
	struct nzk_list l = { 0, 0, 0 }; struct nzui_event ev;
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return nzk_message("View", name, "The file cannot be read");
	if (!(buf = malloc(VIEW_MAX + 1))) { close(fd); return 0; }
	for (;;) {
		ssize_t n = read(fd, buf + len, (size_t)(VIEW_MAX + 1 - len));
		if (n <= 0) break;
		len += n;
		if (len > VIEW_MAX) { len = VIEW_MAX; cut = 1; break; }
	}
	close(fd);
	for (long i = 0; i < len && i < 4096; i++) if (!buf[i]) hex = 1;
	if (hex) nline = (len + HEXW - 1) / HEXW;
	else {
		/* start of every screen line: after a line feed, or after VCOLS characters */
		for (long i = 0, col = 0, start = 1; i <= len; i++) {
			if (start || (col >= VCOLS && (i == len || (buf[i] & 0xC0) != 0x80))) {
				if (i == len && !start) break;
				if (nline == alloc) { long *nl = realloc(line, (size_t)(alloc = alloc ? alloc * 2 : 256) * sizeof(*nl)); if (!nl) break; line = nl; }
				line[nline++] = i; col = 0; start = 0;
			}
			if (i == len) break;
			if (buf[i] == '\n') start = 1;
			else if ((buf[i] & 0xC0) != 0x80) col += buf[i] == '\t' ? 4 - col % 4 : 1;     /* a UTF-8 character counts once */
		}
		if (nline > 1 && line[nline - 1] == len) nline--;       /* no empty line after the last line feed */
		if (!len) nline = 0;
	}
	l.n = (int)nline;
	if (cut && nzk_message("View", name, "Large file: the first 1M is shown") < 0) { free(buf); free(line); return -1; }
	for (;;) {
		char right[16];
		if (l.top > l.n - VROWS) l.top = l.n - VROWS;
		if (l.top < 0) l.top = 0;
		snprintf(right, sizeof(right), "%d%%", l.n > VROWS ? (int)((long long)l.top * 100 / (l.n - VROWS)) : 100);
		nzk_begin(); nzk_title(name, right);
		for (int i = 0; i < VROWS && l.top + i < l.n; i++) {
			long k = l.top + i; int y = NZK_SUB_Y + i * NZK_ROW_H; char t[160]; int o = 0;
			if (hex) {
				long a = k * HEXW; char asc[HEXW + 1];
				o = snprintf(t, sizeof(t), "%06lx ", a);
				for (int j = 0; j < HEXW; j++) {
					if (a + j < len) { o += snprintf(t + o, sizeof(t) - (size_t)o, "%02x ", buf[a + j]); asc[j] = buf[a + j] >= 32 && buf[a + j] < 127 ? (char)buf[a + j] : '.'; }
					else { o += snprintf(t + o, sizeof(t) - (size_t)o, "   "); asc[j] = ' '; }
				}
				asc[HEXW] = 0;
				nzui_text(4, y, NZK_MUTED, "%.6s", t); nzui_text(4 + 7 * 8, y, NZK_TEXT, "%s", t + 7); nzui_text(4 + 31 * 8, y, NZK_LINK, "%s", asc);
			} else {
				long end = k + 1 < nline ? line[k + 1] : len; int col = 0;
				for (long p = line[k]; p < end && o < (int)sizeof(t) - 5; p++) {
					if (buf[p] == '\n' || buf[p] == '\r') continue;
					if (buf[p] == '\t') { do { t[o++] = ' '; col++; } while (col % 4 && o < (int)sizeof(t) - 5); continue; }
					if ((buf[p] & 0xC0) != 0x80) col++;
					t[o++] = (char)buf[p];
				}
				t[o] = 0;
				nzk_text(4, y, NZK_TEXT2, VCOLS, t);
			}
		}
		if (!l.n) nzk_text(8, 50, NZK_MUTED, 38, "The file is empty.");
		nzk_scrollbar(nzui_w - 2, NZK_SUB_Y, VROWS * NZK_ROW_H, l.top, l.n, VROWS);
		nzk_footer("F/X:line  Z/C:page  Esc:back");
		nzk_end();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) { free(buf); free(line); return -1; }
		if (r != 1 || !ev.value) continue;
		switch (nzk_nav(ev.key)) {
		case NZK_NAV_UP: l.top--; continue;
		case NZK_NAV_DOWN: l.top++; continue;
		case NZK_NAV_PGUP: l.top -= VROWS - 1; continue;
		case NZK_NAV_PGDN: l.top += VROWS - 1; continue;
		}
		if (ev.value == 1 && (ev.key == KEY_ESC || ev.key == KEY_BACKSPACE || ev.key == KEY_Q || ev.key == KEY_ENTER || ev.key == KEY_3)) break;
	}
	free(buf); free(line);
	return 0;
}
