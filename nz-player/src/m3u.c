/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - channel lists (.m3u / .m3u8 as used for IPTV) and text helpers.
 *
 * Understood:
 *   #EXTINF:-1 tvg-name="..." group-title="News",Channel name
 *   #EXTGRP:News
 *   #EXTVLCOPT:http-user-agent=...      #EXTVLCOPT:http-referrer=...
 *   http://host/stream.ts|User-Agent=...&Referer=...
 * An entry that is not a URL is taken as a file, relative to the list's folder, so a list
 * of local movies works too.  A file with #EXT-X- lines is not a channel list but the
 * description of one HLS stream; m3u_load() returns 1 and the caller plays the file itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nzp.h"

int is_url(const char *s)
{
	const char *p = strstr(s, "://");
	if (!p || p == s || p - s > 10) return 0;
	for (const char *c = s; c < p; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '+' || *c == '-' || *c == '.')) return 0;
	return 1;
}

/* The 8x8 font has ASCII only.  Accented Latin letters lose their accent, typographic
 * quotes and dashes become plain ones, anything else becomes '?'. */
void text_ascii(char *dst, size_t n, const char *src)
{
	static const char lat1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
	const unsigned char *s = (const unsigned char *)src; size_t o = 0;
	if (!n) return;
	while (*s && o + 1 < n) {
		unsigned c = *s++; int more = 0;
		if (c < 0x80) { dst[o++] = c < 32 ? ' ' : (char)c; continue; }
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

static char *dup_trim(const char *s, size_t len)
{
	while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
	char *d = malloc(len + 1); if (!d) return NULL;
	memcpy(d, s, len); d[len] = 0;
	return d;
}

/* value of key="value" inside an #EXTINF line */
static char *attr(const char *line, const char *key)
{
	size_t kl = strlen(key);
	for (const char *p = line; (p = strstr(p, key)); p += kl) {
		if (p[kl] != '=' || p[kl + 1] != '"' || (p > line && p[-1] != ' ' && p[-1] != '\t')) continue;
		const char *v = p + kl + 2, *e = strchr(v, '"');
		return e ? dup_trim(v, (size_t)(e - v)) : NULL;
	}
	return NULL;
}

static int group_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

int m3u_load(const char *path, struct m3u *out)
{
	FILE *f = fopen(path, "r"); char *line = NULL; size_t cap = 0; int alloc = 0;
	char *name = NULL, *group = NULL, *ua = NULL, *ref = NULL, dir[1024];
	memset(out, 0, sizeof(*out));
	if (!f) return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	{ char *s = strrchr(dir, '/'); if (s) *s = 0; else strcpy(dir, "."); }
	while (getline(&line, &cap, f) > 0) {
		char *l = line;
		if ((unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l += 3;
		while (*l == ' ' || *l == '\t') l++;
		l[strcspn(l, "\r\n")] = 0;
		if (!*l) continue;
		if (!strncmp(l, "#EXT-X-", 7)) { m3u_free(out); free(line); free(name); free(group); free(ua); free(ref); fclose(f); return 1; }
		if (!strncmp(l, "#EXTINF:", 8)) {
			/* the name follows the first comma that is not inside quotes (names may contain commas) */
			const char *comma = NULL; int q = 0;
			for (const char *p = l + 8; *p && !comma; p++) { if (*p == '"') q = !q; else if (*p == ',' && !q) comma = p; }
			free(name); name = comma ? dup_trim(comma + 1, strlen(comma + 1)) : NULL;
			if (!name || !*name) { free(name); name = attr(l, "tvg-name"); }
			free(group); group = attr(l, "group-title");
		} else if (!strncmp(l, "#EXTGRP:", 8)) { free(group); group = dup_trim(l + 8, strlen(l + 8)); }
		else if (!strncasecmp(l, "#EXTVLCOPT:http-user-agent=", 27)) { free(ua); ua = dup_trim(l + 27, strlen(l + 27)); }
		else if (!strncasecmp(l, "#EXTVLCOPT:http-referrer=", 25)) { free(ref); ref = dup_trim(l + 25, strlen(l + 25)); }
		else if (l[0] != '#') {
			struct chan c = {0}; char *bar;
			if (out->n == alloc) {
				struct chan *nc = realloc(out->c, (size_t)(alloc = alloc ? alloc * 2 : 256) * sizeof(*nc));
				if (!nc) break;
				out->c = nc;
			}
			if (is_url(l) && (bar = strchr(l, '|'))) {      /* url|User-Agent=..&Referer=.. */
				*bar++ = 0;
				for (char *t = strtok(bar, "&"); t; t = strtok(NULL, "&")) {
					if (!strncasecmp(t, "user-agent=", 11)) { free(ua); ua = dup_trim(t + 11, strlen(t + 11)); }
					else if (!strncasecmp(t, "referer=", 8)) { free(ref); ref = dup_trim(t + 8, strlen(t + 8)); }
				}
			}
			if (is_url(l) || l[0] == '/') c.url = dup_trim(l, strlen(l));
			else { size_t n = strlen(dir) + strlen(l) + 2; if ((c.url = malloc(n))) snprintf(c.url, n, "%s/%s", dir, l); }
			if (!c.url) break;
			if (name && *name) { c.name = name; name = NULL; }
			else { const char *b = strrchr(l, '/'); c.name = dup_trim(b ? b + 1 : l, strlen(b ? b + 1 : l)); }
			c.group = group && *group ? group : NULL; if (c.group) group = NULL;
			c.ua = ua; c.ref = ref; ua = ref = NULL;
			free(name); name = NULL; free(group); group = NULL;
			out->c[out->n++] = c;
		}
	}
	free(line); free(name); free(group); free(ua); free(ref); fclose(f);
	/* distinct groups, sorted */
	{
		int ga = 0;
		for (int i = 0; i < out->n; i++) {
			const char *g = out->c[i].group; int k;
			if (!g) continue;
			for (k = out->ngroups - 1; k >= 0; k--) if (!strcmp(out->groups[k], g)) break;   /* lists are grouped: search from the end */
			if (k >= 0) continue;
			if (out->ngroups == ga) { char **ng = realloc(out->groups, (size_t)(ga = ga ? ga * 2 : 32) * sizeof(*ng)); if (!ng) break; out->groups = ng; }
			out->groups[out->ngroups++] = out->c[i].group;
		}
		if (out->ngroups) qsort(out->groups, (size_t)out->ngroups, sizeof(char *), group_cmp);
	}
	return 0;
}

void m3u_free(struct m3u *m)
{
	for (int i = 0; i < m->n; i++) { free(m->c[i].name); free(m->c[i].group); free(m->c[i].url); free(m->c[i].ua); free(m->c[i].ref); }
	free(m->c); free(m->groups);
	memset(m, 0, sizeof(*m));
}
