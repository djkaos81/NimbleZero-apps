/* SPDX-License-Identifier: GPL-2.0-only
 *
 * nzui_app.h - what NimbleZero apps with typed text share, on top of nzui_kit.h:
 *
 *   - start and stop (screen, signals, a clean console afterwards)
 *   - the modifier keys (Ctrl, Alt, Caps Lock) and the character a key press types
 *   - "raw keys": Ctrl+C, Ctrl+S, Ctrl+Z ... reach the app instead of the console driver
 *   - waiting for a key and for a file descriptor at the same time (terminal)
 *   - a file chooser
 *   - scripted input, screenshots and running without a screen, for tests
 *
 * Usage: the .c file with NZUI_IMPLEMENTATION and NZUI_KIT_IMPLEMENTATION also defines
 * NZUI_APP_IMPLEMENTATION before including this file.  It must be the same file: the
 * device build looks at nzui.h's list of input devices.
 *
 * Tests (any build).  PFX is the prefix given to nza_start(), e.g. NZE:
 *   PFX_HEADLESS=1     no screen, no keyboard (the device program can run under emulation)
 *   PFX_KEYS="down,enter,text:hello,C-s,shot:/tmp/a.ppm"
 *                      key names (up down left right enter esc tab space backspace delete
 *                      home end pgup pgdn help f1..f12, a letter, a digit), with C- A- S-
 *                      in front for Ctrl, Alt, Shift; text:... types the characters (\c for
 *                      a comma); wait:MS pauses; shot:FILE writes the screen as a PPM.
 *                      The program leaves when the script is finished.
 *   PFX_CONFIG_DIR     settings folder instead of ~/.config/<app>
 */
#ifndef NZUI_APP_H
#define NZUI_APP_H
#include <stddef.h>
#include "nzui_kit.h"

extern int nza_ctrl, nza_alt, nza_caps;         /* kept up to date by nza_wait() */
extern int nza_headless, nza_scripted;

int  nza_start(const char *title, const char *pfx);     /* 0 ok; 1 no display */
void nza_stop(void);
int  nza_wait(struct nzui_event *ev, int timeout_ms);   /* nzk_wait() plus the modifiers */
/* The same, but also returns (0) as soon as fd can be read; *ready says so.  fd < 0: none. */
int  nza_wait_fd(struct nzui_event *ev, int timeout_ms, int fd, int *ready);
int  nza_char(const struct nzui_event *ev);     /* printable character typed, 0 if none */
void nza_raw_keys(int on);                      /* on: Ctrl+C and friends are ordinary keys */
void nza_shot(const char *path);
const char *nza_config_dir(const char *app);
int  nza_quit_requested(void);
/* Choose a file.  dir: where to start, and where the user ended up.  accept: NULL for every
 * file, else which names to list.  top: this is the app's first screen (Esc twice or Q
 * leaves) and not a step back.  footer, extra_key: an app's own key here, or NULL, 0.
 * 1 chosen (path in out), 2 extra_key pressed, 0 back, -1 leave the app. */
int  nza_pick_file(const char *title, char *dir, size_t dirsize, char *out, size_t outsize, int (*accept)(const char *name),
                   int top, const char *footer, int extra_key);

#ifdef NZUI_APP_IMPLEMENTATION
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

int nza_ctrl, nza_alt, nza_caps, nza_headless, nza_scripted;
static volatile sig_atomic_t nza__quit;
static char *nza__script, nza__pfx[16]; static const char *nza__text;
static double nza__wait_until; static int nza__one_ctrl, nza__one_alt, nza__one_shift, nza__one;
static struct termios nza__saved; static int nza__tty = -1, nza__raw;

int nza_quit_requested(void) { return nza__quit; }
static void nza__on_term(int s) { (void)s; nza__quit = 1; }
static void nza__restore_tty(void)
{
	if (nza__raw && nza__tty >= 0) { tcsetattr(nza__tty, TCSANOW, &nza__saved); nza__raw = 0; }
}
static void nza__on_fatal(int s)
{
	nza__restore_tty();
#ifndef NZUI_SDL
	if (!nza_headless) nzui_suspend();          /* text mode again, so the console is usable */
#endif
	signal(s, SIG_DFL); raise(s);
}
void nza_raw_keys(int on)
{
#ifdef NZUI_SDL
	(void)on;
#else
	struct termios t;
	if (nza_headless) return;
	if (!on) { nza__restore_tty(); return; }
	if (nza__raw) return;
	if (nza__tty < 0) nza__tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (nza__tty < 0 || tcgetattr(nza__tty, &nza__saved)) return;
	t = nza__saved;
	t.c_lflag &= ~(tcflag_t)(ISIG | ICANON | ECHO | IEXTEN);
	t.c_iflag &= ~(tcflag_t)(IXON | IXOFF);
	if (!tcsetattr(nza__tty, TCSANOW, &t)) nza__raw = 1;
#endif
}
void nza_shot(const char *path)
{
	FILE *f = fopen(path, "wb"); if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", nzui_w, nzui_h);
	for (int i = 0; i < nzui_w * nzui_h; i++) { uint16_t c = nzui_fb[i]; fputc(c >> 8 & 0xf8, f); fputc(c >> 3 & 0xfc, f); fputc(c << 3 & 0xf8, f); }
	fclose(f);
}
const char *nza_config_dir(const char *app)
{
	static char d[PATH_MAX]; char v[40]; const char *e, *h = getenv("HOME");
	snprintf(v, sizeof(v), "%s_CONFIG_DIR", nza__pfx);
	if ((e = getenv(v))) snprintf(d, sizeof(d), "%s", e); else snprintf(d, sizeof(d), "%s/.config/%s", h && *h ? h : "/tmp", app);
	for (char *s = d + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(d, 0755); *s = '/'; }
	mkdir(d, 0755);
	return d;
}

/* ---- scripted input ---- */
static int nza__code_of(int ch, int *shift)
{
	*shift = 0;
	if (ch >= 'A' && ch <= 'Z') { *shift = 1; ch += 32; }
	for (int code = 1; code < 128; code++) if (nz_key_char(code, 0) == ch) return code;
	return 0;
}
static int nza__script_poll(struct nzui_event *ev, int timeout_ms)
{
	static const struct { const char *n; int k; } map[] = { {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
		{"enter", KEY_ENTER}, {"esc", KEY_ESC}, {"tab", KEY_TAB}, {"space", KEY_SPACE}, {"backspace", KEY_BACKSPACE},
		{"delete", KEY_DELETE}, {"home", KEY_HOME}, {"end", KEY_END}, {"pgup", KEY_PAGEUP}, {"pgdn", KEY_PAGEDOWN}, {"help", KEY_HELP},
		{"insert", KEY_INSERT}, {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4}, {"f5", KEY_F5}, {"f6", KEY_F6},
		{"f7", KEY_F7}, {"f8", KEY_F8}, {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12} };
	char *t, *comma; double now = nzk__now(); int sh;
	if (nza__quit) return -1;
	nza__one = 1; nza__one_ctrl = nza__one_alt = nza__one_shift = 0;
	if (nza__wait_until > now) { if (timeout_ms > 0) usleep(timeout_ms < 10 ? (useconds_t)timeout_ms * 1000 : 10000); return 0; }
	if (nza__text && *nza__text) {                  /* in the middle of text:... */
		int ch = (unsigned char)*nza__text++;
		if (ch == '\\' && *nza__text == 'c') { ch = ','; nza__text++; }
		if (ch == '\\' && *nza__text == 'n') { nza__text++; ev->key = KEY_ENTER; ev->value = 1; return 1; }
		if (!(ev->key = nza__code_of(ch, &sh))) { fprintf(stderr, "script: no key types '%c'\n", ch); return 0; }
		nza__one_shift = sh; ev->value = 1; return 1;
	}
next:
	t = nza__script;
	if (!*t) return timeout_ms ? -1 : 0;            /* finished: leave (but not out of the middle of a job) */
	if ((comma = strchr(t, ','))) *comma = 0;
	nza__script = comma ? comma + 1 : t + strlen(t);
	if (!strncmp(t, "shot:", 5)) { nza_shot(t + 5); goto next; }
	if (!strncmp(t, "wait:", 5)) { nza__wait_until = now + atoi(t + 5) / 1000.0; return 0; }
	if (!strncmp(t, "text:", 5)) { nza__text = t + 5; return 0; }
	for (;;) {
		if (!strncmp(t, "C-", 2) && t[2]) { nza__one_ctrl = 1; t += 2; }
		else if (!strncmp(t, "A-", 2) && t[2]) { nza__one_alt = 1; t += 2; }
		else if (!strncmp(t, "S-", 2) && t[2]) { nza__one_shift = 1; t += 2; }
		else break;
	}
	ev->value = 1;
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcmp(t, map[i].n)) { ev->key = map[i].k; return 1; }
	if (strlen(t) == 1 && (ev->key = nza__code_of(*t, &sh))) return 1;
	fprintf(stderr, "script: unknown key name '%s'\n", t);
	return 0;
}
static int nza__real_poll(struct nzui_event *ev, int timeout_ms) { return nza__quit ? -1 : nzui_poll(ev, timeout_ms); }
static void nza__no_flush(void) {}

int nza_start(const char *title, const char *pfx)
{
	char v[40]; const char *keys;
	snprintf(nza__pfx, sizeof(nza__pfx), "%s", pfx);
	snprintf(v, sizeof(v), "%s_HEADLESS", pfx); nza_headless = getenv(v) != NULL;
	snprintf(v, sizeof(v), "%s_KEYS", pfx); keys = getenv(v);
	signal(SIGTERM, nza__on_term); signal(SIGHUP, nza__on_term);
	signal(SIGINT, SIG_IGN); signal(SIGQUIT, SIG_IGN); signal(SIGTSTP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
	signal(SIGSEGV, nza__on_fatal); signal(SIGBUS, nza__on_fatal); signal(SIGABRT, nza__on_fatal); signal(SIGFPE, nza__on_fatal); signal(SIGILL, nza__on_fatal);
	nzk_poll_fn = nza__real_poll;
	if (keys || nza_headless) { nza__script = strdup(keys ? keys : ""); nzk_poll_fn = nza__script_poll; nza_scripted = 1; }
	if (nza_headless) { nzui_fb = calloc((size_t)nzui_w * nzui_h, 2); nzk_flush_fn = nza__no_flush; if (!nzui_fb) return 1; }
	else if (nzui_init(title)) { fprintf(stderr, "%s: no display\n", title); return 1; }
	if (nza_scripted) nzk_idle_allow(0);
	return 0;
}
void nza_stop(void)
{
	nza__restore_tty();
	if (!nza_headless) nzui_quit();
}

static void nza__mods(struct nzui_event *ev, int r)
{
	if (nza__one) { nza__one = 0; if (r == 1) { nza_ctrl = nza__one_ctrl; nza_alt = nza__one_alt; nzk_shift = nza__one_shift; } return; }
	if (r != 1) return;
	if (ev->key == KEY_LEFTCTRL || ev->key == KEY_RIGHTCTRL) nza_ctrl = ev->value != 0;
	if (ev->key == KEY_LEFTALT || ev->key == KEY_RIGHTALT) nza_alt = ev->value != 0;
	if (ev->key == KEY_CAPSLOCK && ev->value == 1) nza_caps = !nza_caps;
}
int nza_wait(struct nzui_event *ev, int timeout_ms)
{
	int r = nzk_wait(ev, timeout_ms);
	nza__mods(ev, r);
	return r;
}
int nza_wait_fd(struct nzui_event *ev, int timeout_ms, int fd, int *ready)
{
	struct pollfd p = { fd, POLLIN, 0 }; int r;
	*ready = 0;
	if (fd < 0) return nza_wait(ev, timeout_ms);
#ifndef NZUI_SDL
	if (!nza_scripted) {                            /* sleep until a key or the descriptor */
		struct pollfd all[17]; int n = 0, got, key = 0;
		for (int i = 0; i < nzui__nin && n < 16; i++) all[n++] = nzui__in[i];
		all[n] = p;
		got = poll(all, (nfds_t)n + 1, timeout_ms);
		if (got > 0 && (all[n].revents & (POLLIN | POLLHUP | POLLERR))) *ready = 1;
		for (int i = 0; i < n; i++) if (all[i].revents) key = 1;
		/* Ask nzui only when a key is there or the time is up: each empty question makes it look
		 * for new keyboards now and then, which must not happen for every line a program prints. */
		r = key || got <= 0 ? nzk_wait(ev, 0) : 0;
		nza__mods(ev, r);
		return r;
	}
#endif
	for (int waited = 0; ; waited += 10) {          /* simulator, tests: look at both in turn */
		if (poll(&p, 1, 0) > 0) *ready = 1;
		r = nzk_wait(ev, *ready || waited >= timeout_ms ? 0 : 10);
		nza__mods(ev, r);
		if (r || *ready || waited >= timeout_ms) return r;
	}
}
int nza_char(const struct nzui_event *ev)
{
	int c;
	if (!ev->value || nza_ctrl || nza_alt) return 0;
	c = nz_key_char(ev->key, 0);
	if (c >= 'a' && c <= 'z' && (nzk_shift != nza_caps)) c -= 32;
	return c;
}

/* ---- file chooser ---- */
struct nza__ent { char *name; int dir; long long size; };
static int nza__ent_cmp(const void *a, const void *b)
{
	const struct nza__ent *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;
	return strcasecmp(x->name, y->name) ? strcasecmp(x->name, y->name) : strcmp(x->name, y->name);
}
int nza_pick_file(const char *title, char *dir, size_t dirsize, char *out, size_t outsize, int (*accept)(const char *name),
                  int top, const char *footer, int extra_key)
{
	struct nza__ent *e = NULL; struct nzk_list l = { 0, 0, 0 }; struct nzui_event ev; int r, scan = 1, res = 0; char want[NAME_MAX + 1] = "";
	const int rows = NZK_ROWS;
	for (;;) {
		if (scan) {
			DIR *d; struct dirent *de; int alloc = 0, n = 0;
			for (int i = 0; i < l.n; i++) free(e[i].name);
			free(e); e = NULL;
			while (!(d = opendir(dir)) && strcmp(dir, "/")) {       /* gone or closed: the nearest folder that opens */
				char *s = strrchr(dir, '/');
				if (s == dir) s[1] = 0; else if (s) *s = 0; else snprintf(dir, dirsize, "/");
			}
			while (d && (de = readdir(d))) {
				char p[PATH_MAX]; struct stat st; struct nza__ent one;
				if (de->d_name[0] == '.') continue;
				snprintf(p, sizeof(p), "%s/%s", strcmp(dir, "/") ? dir : "", de->d_name);
				if (stat(p, &st)) continue;
				one.dir = S_ISDIR(st.st_mode); one.size = st.st_size;
				if (!one.dir && (!S_ISREG(st.st_mode) || (accept && !accept(de->d_name)))) continue;
				if (n == alloc) { struct nza__ent *ne = realloc(e, (size_t)(alloc = alloc ? alloc * 2 : 64) * sizeof(*ne)); if (!ne) break; e = ne; }
				if (!(one.name = strdup(de->d_name))) break;
				e[n++] = one;
			}
			if (d) closedir(d);
			if (n) qsort(e, (size_t)n, sizeof(e[0]), nza__ent_cmp);
			l.n = n; l.sel = l.top = 0; scan = 0;
			for (int i = 0; i < n; i++) if (!strcmp(e[i].name, want)) l.sel = i;
		}
		nzk_list_fit(&l, rows);
		nzk_begin(); nzk_title(title, NULL); nzk_subline(dir, 1);
		for (int i = 0; i < rows && l.top + i < l.n; i++) {
			struct nza__ent *x = &e[l.top + i]; char sz[12] = "";
			if (!x->dir) { if (x->size >= 1 << 20) snprintf(sz, sizeof(sz), "%lldM", x->size >> 20); else snprintf(sz, sizeof(sz), "%lldK", (x->size + 1023) >> 10); }
			nzk_row(0, NZK_LIST_Y + i * NZK_ROW_H, nzui_w, l.top + i == l.sel, x->dir ? NZK_LINK : NZK_TEXT, x->dir ? "/" : " ", x->name, sz[0] ? sz : NULL);
		}
		if (!l.n) nzk_text(12, NZK_LIST_Y, NZK_MUTED, 36, accept ? "(nothing to open here)" : "(empty)");
		nzk_scrollbar(nzui_w - 2, NZK_LIST_Y, rows * NZK_ROW_H, l.top, l.n, rows);
		nzk_footer(footer ? footer : "Enter:open  Backspace:folder above");
		nzk_end();
		if ((r = nza_wait(&ev, nzk_timeout())) < 0) { res = -1; break; }
		if (r != 1 || !ev.value) continue;
		if (nzk_nav(ev.key)) { nzk_list_move(&l, nzk_nav(ev.key), rows); nzk_leave(0); continue; }
		if (ev.value != 1) continue;
		if (ev.key == KEY_BACKSPACE) {
			char *s = strrchr(dir, '/');
			if (!s || !strcmp(dir, "/")) continue;
			snprintf(want, sizeof(want), "%s", s + 1);
			if (s == dir) s[1] = 0; else *s = 0;
			scan = 1;
		} else if (ev.key == KEY_ENTER && l.n) {
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/%s", strcmp(dir, "/") ? dir : "", e[l.sel].name);
			if (e[l.sel].dir) { if (!access(p, R_OK | X_OK)) { snprintf(dir, dirsize, "%s", p); want[0] = 0; scan = 1; } }
			else { snprintf(out, outsize, "%s", p); res = 1; break; }
		} else if (ev.key == KEY_R) { if (l.n) snprintf(want, sizeof(want), "%s", e[l.sel].name); scan = 1; }
		else if (extra_key && ev.key == extra_key) { res = 2; break; }
		else if (top) { if (nzk_leave(ev.key)) { res = -1; break; } }
		else if (ev.key == KEY_ESC) break;
	}
	for (int i = 0; i < l.n; i++) free(e[i].name);
	free(e);
	return res;
}
#endif /* NZUI_APP_IMPLEMENTATION */
#endif /* NZUI_APP_H */
