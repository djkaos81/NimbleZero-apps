/* SPDX-License-Identifier: GPL-2.0-only
 * NimbleCommander (nz-commander) - file manager for NimbleZero (M5Stack Cardputer Zero).
 *
 * Two folders ("panes"), shown side by side or one at a time; Tab changes the pane.  Copy
 * and move go from the pane that has the keys to the other one.  Look and keys: nzui_kit.h
 * and docs/STYLE.md of NimbleZero.
 *
 * For tests (any build):
 *   NZC_HEADLESS=1      no screen, no keyboard (the device program can run under emulation)
 *   NZC_KEYS="down,space,5,enter,wait:200,shot:/tmp/a.ppm,q"
 *                       scripted input: key names, pauses in ms, screenshots; the program
 *                       leaves when the script is finished
 *   NZC_CONFIG_DIR      settings folder instead of ~/.config/nz-commander
 */
#define _GNU_SOURCE
#define NZUI_IMPLEMENTATION
#define NZUI_KIT_IMPLEMENTATION
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include "nzc.h"

#define ROWS      12                    /* list rows; a status line follows */
#define STATUS_Y  149

/* The other NimbleZero apps this one hands files to, if they are installed:
 * /opt/NAME/NAME (NZC_APPS_DIR instead of /opt for tests). */
static const char *app(const char *name)
{
	static char p[4][PATH_MAX]; static int k; const char *base = getenv("NZC_APPS_DIR");
	k = (k + 1) % 4;
	snprintf(p[k], sizeof(p[k]), "%s/%s/%s", base ? base : "/opt", name, name);
	return p[k];
}
static int have(const char *name) { return !access(app(name), X_OK); }
#define PLAYER app("nz-player")

struct ent { char *name; int dir, mark; long long size; time_t mtime; };
struct pane { char dir[PATH_MAX]; struct ent *e; struct nzk_list l; int nmark; long long markbytes; };
static struct pane pn[2]; static int act;
static struct { int two, sort, hidden, sizes; } cfg = { 1, 0, 0, 1 };
static const char *sort_names[] = { "Name", "Size", "Date" };
int headless;
static volatile sig_atomic_t quit_req;

static void on_signal(int s) { (void)s; quit_req = 1; }
static void join(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s/%s", strcmp(dir, "/") ? dir : "", name); }

void fmt_size(char *o, size_t n, long long s)
{
	if (s >= 1LL << 30) snprintf(o, n, "%.1fG", s / (double)(1LL << 30));
	else if (s >= 1 << 20) snprintf(o, n, "%lldM", s >> 20);
	else snprintf(o, n, "%lldK", (s + 1023) >> 10);
}
static void fmt_date(char *o, size_t n, time_t t)
{
	struct tm *tm = localtime(&t); time_t now = time(NULL);
	if (!tm) { snprintf(o, n, "?"); return; }
	strftime(o, n, t > now - 180 * 86400 && t <= now + 86400 ? "%d %b %H:%M" : "%d %b %Y", tm);
}
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
static int is_media(const char *name)
{
	return has_ext(name, "mp4 m4v mkv webm avi mov mpg mpeg m2v ts m2ts mts wmv flv 3gp ogv vob "
		"mp3 mp2 m4a aac flac ogg oga opus wav wma ac3 mka m3u m3u8");
}
static int is_picture(const char *name) { return has_ext(name, "jpg jpeg jpe png bmp gif ppm pgm pnm"); }

/* ---------------- settings ---------------- */
static const char *cfg_dir(void)
{
	static char d[PATH_MAX]; const char *e = getenv("NZC_CONFIG_DIR"), *h = getenv("HOME");
	if (e) snprintf(d, sizeof(d), "%s", e); else snprintf(d, sizeof(d), "%s/.config/nz-commander", h && *h ? h : "/tmp");
	return d;
}
static void cfg_load(void)
{
	char p[PATH_MAX], line[PATH_MAX + 32]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", cfg_dir());
	if (!(f = fopen(p, "r"))) return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '='); if (!v) continue;
		*v++ = 0; v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "panes")) cfg.two = atoi(v) != 1;
		else if (!strcmp(line, "sort")) cfg.sort = atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 0;
		else if (!strcmp(line, "hidden")) cfg.hidden = !!atoi(v);
		else if (!strcmp(line, "sizes")) cfg.sizes = !!atoi(v);
		else if (!strcmp(line, "active")) act = atoi(v) == 1;
		else if (!strcmp(line, "left")) snprintf(pn[0].dir, sizeof(pn[0].dir), "%s", v);
		else if (!strcmp(line, "right")) snprintf(pn[1].dir, sizeof(pn[1].dir), "%s", v);
	}
	fclose(f);
}
static void cfg_save(void)
{
	char p[PATH_MAX], tmp[PATH_MAX + 8]; FILE *f;
	snprintf(p, sizeof(p), "%s", cfg_dir());
	for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/config", cfg_dir()); snprintf(tmp, sizeof(tmp), "%s.new", p);
	if (!(f = fopen(tmp, "w"))) return;
	fprintf(f, "panes=%d\nsort=%d\nhidden=%d\nsizes=%d\nactive=%d\nleft=%s\nright=%s\n", cfg.two ? 2 : 1, cfg.sort, cfg.hidden, cfg.sizes, act, pn[0].dir, pn[1].dir);
	if (fclose(f) == 0) rename(tmp, p);
}

/* ---------------- reading a folder ---------------- */
static int ent_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b; int up_x = !strcmp(x->name, ".."), up_y = !strcmp(y->name, "..");
	if (up_x != up_y) return up_y - up_x;
	if (x->dir != y->dir) return y->dir - x->dir;
	if (cfg.sort == 1 && !x->dir && x->size != y->size) return x->size < y->size ? 1 : -1;     /* largest first */
	if (cfg.sort == 2 && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;            /* newest first */
	return strcasecmp(x->name, y->name) ? strcasecmp(x->name, y->name) : strcmp(x->name, y->name);
}
static void pane_free(struct pane *p)
{
	for (int i = 0; i < p->l.n; i++) free(p->e[i].name);
	free(p->e); p->e = NULL; p->l.n = 0; p->nmark = 0; p->markbytes = 0;
}
/* read p->dir again; put the selection on `select` if it is there, else keep the row */
static void pane_scan(struct pane *p, const char *select)
{
	DIR *d; struct dirent *de; int alloc = 0, n = 0, keep = p->l.sel; struct ent *e = NULL, one;
	pane_free(p);
	while (!(d = opendir(p->dir)) && strcmp(p->dir, "/")) {     /* the folder is gone: the nearest one that exists */
		char *s = strrchr(p->dir, '/');
		if (s == p->dir) s[1] = 0; else if (s) *s = 0; else snprintf(p->dir, sizeof(p->dir), "/");
	}
	for (int first = 1; d; first = 0) {
		char path[PATH_MAX]; struct stat st, lst; const char *name;
		if (first) { if (!strcmp(p->dir, "/")) continue; name = ".."; }
		else {
			if (!(de = readdir(d))) break;
			name = de->d_name;
			if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !cfg.hidden)) continue;
		}
		join(path, sizeof(path), p->dir, name);
		if (lstat(path, &lst)) continue;
		if (stat(path, &st)) st = lst;                           /* a link that points nowhere */
		one.dir = S_ISDIR(st.st_mode); one.size = st.st_size; one.mtime = st.st_mtime; one.mark = 0;
		if (n == alloc) { struct ent *ne = realloc(e, (size_t)(alloc = alloc ? alloc * 2 : 64) * sizeof(*ne)); if (!ne) break; e = ne; }
		if (!(one.name = strdup(name))) break;
		e[n++] = one;
	}
	if (d) closedir(d);
	if (n) qsort(e, (size_t)n, sizeof(e[0]), ent_cmp);
	p->e = e; p->l.n = n; p->l.sel = keep; p->l.top = 0;
	for (int i = 0; select && i < n; i++) if (!strcmp(e[i].name, select)) p->l.sel = i;
	nzk_list_fit(&p->l, ROWS);
}
static void rescan_both(const char *select)
{
	char *keep[2];
	for (int i = 0; i < 2; i++) keep[i] = pn[i].l.n ? strdup(pn[i].e[pn[i].l.sel].name) : NULL;
	for (int i = 0; i < 2; i++) { pane_scan(&pn[i], i == act && select ? select : keep[i]); free(keep[i]); }
}
static struct ent *cur_ent(void) { struct pane *p = &pn[act]; return p->l.n ? &p->e[p->l.sel] : NULL; }
static int is_up(const struct ent *e) { return !strcmp(e->name, ".."); }
static void go_up(struct pane *p)
{
	char was[NAME_MAX + 1]; char *s = strrchr(p->dir, '/');
	if (!s || !strcmp(p->dir, "/")) return;
	snprintf(was, sizeof(was), "%s", s + 1);
	if (s == p->dir) s[1] = 0; else *s = 0;
	p->l.sel = 0; pane_scan(p, was);
}
static void set_mark(struct pane *p, int i, int on)
{
	struct ent *e = &p->e[i];
	if (is_up(e) || e->mark == on) return;
	e->mark = on; p->nmark += on ? 1 : -1;
	if (!e->dir) p->markbytes += on ? e->size : -e->size;
}

/* ---------------- drawing ---------------- */
static void pane_head(int x, int w, const char *path, int active)
{
	char a[PATH_MAX]; int max = (w - 8) / 8, n;
	nzk_ascii(a, sizeof(a), path); n = (int)strlen(a);
	if (active) nzui_rect(x, NZK_SUB_Y - 1, w - 3, NZK_ROW_H, NZK_SEL);
	nzk_text(x + 4, NZK_SUB_Y, active ? NZK_TEXT : NZK_MUTED, max, n > max ? a + n - max : a);
}
static void pane_draw(int x, int w, struct pane *p, int active, int sizes)
{
	nzk_list_fit(&p->l, ROWS);
	for (int i = 0; i < ROWS && p->l.top + i < p->l.n; i++) {
		int k = p->l.top + i; struct ent *e = &p->e[k]; char sz[12] = "";
		if (sizes && !e->dir) fmt_size(sz, sizeof(sz), e->size);
		nzk_row(x, NZK_LIST_Y + i * NZK_ROW_H, w, k == p->l.sel ? (active ? 1 : 2) : 0, e->mark ? NZK_GREEN : e->dir ? NZK_LINK : NZK_TEXT,
			e->mark ? "*" : e->dir ? "/" : " ", e->name, sz[0] ? sz : NULL);
	}
	if (!p->l.n) nzk_text(x + 12, NZK_LIST_Y, NZK_MUTED, (w - 16) / 8, "(empty)");
	nzk_scrollbar(x + w - 2, NZK_LIST_Y, ROWS * NZK_ROW_H, p->l.top, p->l.n, ROWS);
}
static void draw(void)
{
	static const char *const keys[] = { "Help", "2Usr", "3View", "4Edit", "5Cp", "6Mv", "7Mkdir", "8Del", "Option" };
	struct pane *p = &pn[act]; struct ent *e = cur_ent(); char right[40] = "", a[12], d[24];
	nzk_begin(); nzk_title("NimbleCommander", NULL);
	if (cfg.two) {
		pane_head(0, nzui_w / 2, pn[0].dir, act == 0); pane_head(nzui_w / 2, nzui_w / 2, pn[1].dir, act == 1);
		pane_draw(0, nzui_w / 2, &pn[0], act == 0, cfg.sizes); pane_draw(nzui_w / 2, nzui_w / 2, &pn[1], act == 1, cfg.sizes);
		nzui_rect(nzui_w / 2 - 1, NZK_SUB_Y - 1, 1, STATUS_Y - 3 - (NZK_SUB_Y - 1), NZK_SEL);
	} else {
		char o[PATH_MAX], t[20]; int n;
		nzk_ascii(o, sizeof(o), pn[!act].dir); n = (int)strlen(o);         /* where copies go: the other pane */
		snprintf(t, sizeof(t), "Tab:%s", n > 12 ? o + n - 12 : o);
		pane_head(0, nzui_w - 8 * (int)strlen(t) - 12, p->dir, 1);
		nzui_rect(nzui_w - 8 * (int)strlen(t) - 12, NZK_SUB_Y - 1, 8 * (int)strlen(t) + 9, NZK_ROW_H, NZK_SEL);
		nzk_text(nzui_w - 8 * (int)strlen(t) - 8, NZK_SUB_Y, NZK_MUTED, 16, t);
		pane_draw(0, nzui_w, p, 1, 1);
	}
	nzui_rect(0, STATUS_Y - 3, nzui_w, 1, NZK_SEL);
	if (p->nmark) {
		struct statvfs vfs; char l[40];
		fmt_size(a, sizeof(a), p->markbytes); snprintf(l, sizeof(l), "%d marked, %s", p->nmark, a);
		nzk_text(4, STATUS_Y, NZK_GREEN, 26, l);
		if (!statvfs(pn[!act].dir, &vfs)) { fmt_size(a, sizeof(a), (long long)vfs.f_bavail * (long long)vfs.f_frsize); snprintf(right, sizeof(right), "%s free", a); }
	} else if (e && !is_up(e)) {
		fmt_date(d, sizeof(d), e->mtime);
		if (e->dir) snprintf(right, sizeof(right), "%s", d); else { fmt_size(a, sizeof(a), e->size); snprintf(right, sizeof(right), "%s %s", a, d); }
		nzk_text_scroll(4, STATUS_Y, NZK_TEXT, 38 - (int)strlen(right), e->name, 1);
	} else if (e) nzk_text(4, STATUS_Y, NZK_MUTED, 30, "Folder above");
	nzk_text(nzui_w - 4 - 8 * (int)strlen(right), STATUS_Y, NZK_MUTED, 39, right);
	nzk_keybar(keys, 9);
	nzk_end();
}

/* ---------------- handing the screen to another program ---------------- */
/* terminal: the program gets the text console (nano, sudo); otherwise it draws itself. */
static int run_child(int terminal, char *const argv[])
{
	if (headless) {
		fprintf(stderr, "nz-commander: would run");
		for (int i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
		fprintf(stderr, "\n");
		return 0;
	}
#ifdef NZUI_SDL
	(void)terminal;
	return nzk_message("NimbleCommander", "Other programs are not started", "in the simulator.");
#else
	pid_t pid; int st = 0, tty;
	tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (terminal && tty >= 0) { if (write(tty, "\033[2J\033[H\033[?25h", 13) < 0) {} }
	nzui_suspend();                          /* text mode, typed-ahead keys dropped, input devices closed */
	if (!terminal && tty >= 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);
	/* The program stays in this process group: it can read the terminal, and the launcher's
	 * Ctrl+Alt+Backspace ends both.  Ctrl+C in it must not end the commander. */
	signal(SIGINT, SIG_IGN); signal(SIGQUIT, SIG_IGN); signal(SIGTSTP, SIG_IGN);
	if ((pid = fork()) == 0) {
		signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
		if (terminal && tty >= 0) { dup2(tty, 0); dup2(tty, 1); dup2(tty, 2); }
		execvp(argv[0], argv);
		_exit(127);
	}
	while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	signal(SIGINT, on_signal); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
	if (tty >= 0) { tcflush(tty, TCIFLUSH); if (write(tty, "\033[2J\033[H\033[?25l", 13) < 0) {} close(tty); }
	nzui_resume();
	if (pid < 0 || (WIFEXITED(st) && WEXITSTATUS(st) == 127)) return nzk_message("NimbleCommander", argv[0], "This program could not be started");
	return 0;
#endif
}

/* ---------------- actions ---------------- */
/* what an action applies to: the marked entries, else the selected one.  Free with names_free. */
static char **names_get(int *n)
{
	struct pane *p = &pn[act]; struct ent *e = cur_ent(); char **v; int k = 0;
	*n = 0;
	if (!p->nmark && (!e || is_up(e))) return NULL;
	if (!(v = calloc((size_t)(p->nmark ? p->nmark : 1), sizeof(*v)))) return NULL;
	if (!p->nmark) v[k++] = strdup(e->name);
	else for (int i = 0; i < p->l.n; i++) if (p->e[i].mark) v[k++] = strdup(p->e[i].name);
	*n = k;
	return v;
}
static void names_free(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }

static int valid_name(const char *s) { return *s && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, ".."); }
static int report(const char *title, const struct op_result *r)
{
	char l1[64];
	if (r->quit) return -1;
	if (r->failed) { snprintf(l1, sizeof(l1), "%.37s", r->err_name); return nzk_message(title, l1, r->err_text); }
	if (r->cancelled) return nzk_message(title, "Cancelled.", "What was done so far stays done.");
	if (r->skipped) { snprintf(l1, sizeof(l1), "%d left out.", r->skipped); return nzk_message(title, l1, NULL); }
	return 0;
}
static void tail(char *o, size_t n, const char *path, int max)
{
	char a[PATH_MAX]; int len;
	nzk_ascii(a, sizeof(a), path); len = (int)strlen(a);
	snprintf(o, n, "%s", len > max ? a + len - max : a);
}

static int do_rename(void)
{
	struct pane *p = &pn[act]; struct ent *e = cur_ent(); char name[NAME_MAX + 1], from[PATH_MAX], to[PATH_MAX]; struct stat st; int r;
	if (!e || is_up(e)) return 0;
	snprintf(name, sizeof(name), "%s", e->name);
	if ((r = nzk_input("Rename", "New name:", name, sizeof(name))) <= 0) return r;
	if (!strcmp(name, e->name)) return 0;
	if (!valid_name(name)) return nzk_message("Rename", "That is not a usable name.", "No / and not empty");
	join(from, sizeof(from), p->dir, e->name); join(to, sizeof(to), p->dir, name);
	if (!lstat(to, &st)) return nzk_message("Rename", name, "This name is already in use");
	if (rename(from, to)) return nzk_message("Rename", e->name, strerror(errno));
	sync(); rescan_both(name);
	return 0;
}

static int do_copy_move(int op)
{
	const char *title = op == OP_COPY ? "Copy" : "Move", *verb = op == OP_COPY ? "copy" : "move";
	struct pane *p = &pn[act], *o = &pn[!act]; struct op_result res; struct statvfs vfs; struct stat sa = {0}, sb = {0};
	char l1[64], l2[64], newname[NAME_MAX + 1] = "", **v; int n, r, same;
	long long bytes = 0; long items = 0;
	if (!(v = names_get(&n))) return 0;
	same = !stat(p->dir, &sa) && !stat(o->dir, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	if (same && n > 1) { names_free(v, n); return nzk_message(title, "Both panes show the same folder.", "Change one of them first (Tab)"); }
	if (same && op == OP_MOVE) { names_free(v, n); return do_rename(); }
	if (same) {                              /* a copy next to the original needs another name */
		struct stat st; char to[PATH_MAX];
		snprintf(newname, sizeof(newname), "%s", v[0]);
		if ((r = nzk_input("Copy", "Copy as:", newname, sizeof(newname))) <= 0) { names_free(v, n); return r; }
		join(to, sizeof(to), o->dir, newname);
		if (!valid_name(newname)) r = nzk_message("Copy", "That is not a usable name.", "No / and not empty");
		else if (!lstat(to, &st)) r = nzk_message("Copy", newname, "This name is already in use");
		else r = 1;
		if (r <= 0) { names_free(v, n); return r; }
	} else {
		if (n == 1) snprintf(l1, sizeof(l1), "%s %.28s to", title, v[0]); else snprintf(l1, sizeof(l1), "%s %d items to", title, n);
		tail(l2, sizeof(l2), o->dir, 38);
		if (op == OP_MOVE && n == 1 && !p->nmark) {
			static const int keys[] = { KEY_ENTER, KEY_R };
			r = nzk_ask(title, l1, l2, "Enter:move  R:rename  Esc:cancel", keys, 2);
			if (r == KEY_R) { names_free(v, n); return do_rename(); }
			r = r < 0 ? -1 : r == KEY_ENTER;
		} else r = nzk_confirm(title, l1, l2, verb);
		if (r <= 0) { names_free(v, n); return r; }
	}
	/* room on the other side?  (a move within one file system needs none) */
	for (int i = 0; i < n; i++) { char s[PATH_MAX]; join(s, sizeof(s), p->dir, v[i]); tree_size(s, &bytes, &items); }
	if (!(op == OP_MOVE && sa.st_dev == sb.st_dev) && !statvfs(o->dir, &vfs) && bytes > (long long)vfs.f_bavail * (long long)vfs.f_frsize) {
		char a[12], b[12];
		fmt_size(a, sizeof(a), bytes); fmt_size(b, sizeof(b), (long long)vfs.f_bavail * (long long)vfs.f_frsize);
		snprintf(l1, sizeof(l1), "Not enough room: %s needed,", a); snprintf(l2, sizeof(l2), "%s free.", b);
		names_free(v, n);
		return nzk_message(title, l1, l2);
	}
	op_run(op, p->dir, v, n, o->dir, newname[0] ? newname : NULL, &res);
	names_free(v, n);
	rescan_both(NULL);
	return report(title, &res);
}

static int do_delete(void)
{
	struct pane *p = &pn[act]; struct op_result res; char l1[64], **v; const char *l2 = "This cannot be undone."; int n, r;
	if (!(v = names_get(&n))) return 0;
	if (n > 1) snprintf(l1, sizeof(l1), "Delete %d items?", n);
	else {
		int dir = 0;
		for (int i = 0; i < p->l.n; i++) if (!strcmp(p->e[i].name, v[0])) dir = p->e[i].dir;
		if (dir) { snprintf(l1, sizeof(l1), "Delete folder %.23s?", v[0]); l2 = "With everything in it. No undo."; }
		else snprintf(l1, sizeof(l1), "Delete %.29s?", v[0]);
	}
	if ((r = nzk_confirm("Delete", l1, l2, "delete")) <= 0) { names_free(v, n); return r; }
	op_run(OP_DELETE, p->dir, v, n, NULL, NULL, &res);
	names_free(v, n);
	rescan_both(NULL);
	return report("Delete", &res);
}

static int do_mkdir(void)
{
	struct pane *p = &pn[act]; char name[NAME_MAX + 1] = "", path[PATH_MAX]; int r;
	if ((r = nzk_input("New folder", "Name of the new folder:", name, sizeof(name))) <= 0) return r;
	if (!valid_name(name)) return nzk_message("New folder", "That is not a usable name.", "No / and not empty");
	join(path, sizeof(path), p->dir, name);
	if (mkdir(path, 0755)) return nzk_message("New folder", name, errno == EEXIST ? "This name is already in use" : strerror(errno));
	sync(); rescan_both(name);
	return 0;
}

/* View: a picture in Image Viewer, anything else in Text Editor's viewer (it shows files
 * that are not text as bytes).  Without those apps: the small viewer built in here. */
static int do_view(void)
{
	struct ent *e = cur_ent(); char path[PATH_MAX];
	if (!e || e->dir) return 0;
	join(path, sizeof(path), pn[act].dir, e->name);
	if (is_picture(e->name) && have("nz-image")) return run_child(0, (char *[]){ (char *)app("nz-image"), path, NULL });
	if (have("nz-edit")) return run_child(0, (char *[]){ (char *)app("nz-edit"), "--view", path, NULL });
	return view_file(path, e->name);
}
static int do_edit(void)
{
	struct ent *e = cur_ent(); char path[PATH_MAX]; int r;
	if (!e || e->dir) return 0;
	join(path, sizeof(path), pn[act].dir, e->name);
	if (have("nz-edit")) r = run_child(0, (char *[]){ (char *)app("nz-edit"), path, NULL });
	else r = run_child(1, (char *[]){ "nano", path, NULL });
	rescan_both(NULL);
	return r;
}
/* A shell in the folder of this pane: in Terminal if it is installed, else on the text console. */
static int do_terminal(void)
{
	char *dir = pn[act].dir; int r;
	if (have("nz-term")) r = run_child(0, (char *[]){ (char *)app("nz-term"), "-d", dir, NULL });
	else r = run_child(1, (char *[]){ "sh", "-c", "cd \"$1\" && exec \"${SHELL:-/bin/sh}\" -l", "sh", dir, NULL });
	rescan_both(NULL);
	return r;
}
static int do_play(void)
{
	struct ent *e = cur_ent(); char path[PATH_MAX];
	if (!e || e->dir) return 0;
	if (!have("nz-player")) return nzk_message("Media Player", "Media Player is not installed.", "It is in the Store");
	join(path, sizeof(path), pn[act].dir, e->name);
	return run_child(0, (char *[]){ (char *)PLAYER, path, NULL });
}
static int do_install(void)
{
	struct ent *e = cur_ent(); char path[PATH_MAX], l1[64]; int r;
	if (!e || e->dir) return 0;
	join(path, sizeof(path), pn[act].dir, e->name);
	snprintf(l1, sizeof(l1), "Install %.29s?", e->name);
	if ((r = nzk_confirm("Install package", l1, "You will be asked for your password.", "install")) <= 0) return r;
	return run_child(1, (char *[]){ "sh", "-c", "sudo dpkg -i \"$1\"; echo; printf 'Press Enter to go back '; read x", "sh", path, NULL });
}
static int do_properties(void)
{
	struct ent *e = cur_ent(); char path[PATH_MAX], L[7][48], d[24], perm[11] = "----------"; const char *lines[7]; struct stat st; int n = 0;
	struct passwd *pw; long long bytes = 0; long items = 0;
	if (!e || is_up(e)) return 0;
	join(path, sizeof(path), pn[act].dir, e->name);
	if (lstat(path, &st)) return nzk_message("Properties", e->name, strerror(errno));
	{ char a[64]; nzk_ascii(a, sizeof(a), e->name); snprintf(L[n++], sizeof(L[0]), "%.39s", a); }
	snprintf(L[n++], sizeof(L[0]), "Kind      %s", S_ISLNK(st.st_mode) ? "Link" : S_ISDIR(st.st_mode) ? "Folder" : S_ISREG(st.st_mode) ? "File" : "Special file");
	if (S_ISDIR(st.st_mode)) {
		char a[12];
		nzk_progress("Properties", "Counting...", NULL, -1, "");
		tree_size(path, &bytes, &items); fmt_size(a, sizeof(a), bytes);
		snprintf(L[n++], sizeof(L[0]), "Contains  %ld items, %s", items - 1, a);
	} else { char a[12]; fmt_size(a, sizeof(a), st.st_size); snprintf(L[n++], sizeof(L[0]), "Size      %s (%lld bytes)", a, (long long)st.st_size); }
	{ struct tm *tm = localtime(&st.st_mtime); if (tm) strftime(d, sizeof(d), "%d %b %Y %H:%M", tm); else snprintf(d, sizeof(d), "?"); }
	snprintf(L[n++], sizeof(L[0]), "Changed   %s", d);
	for (int i = 0; i < 9; i++) if (st.st_mode & (0400 >> i)) perm[i + 1] = "rwx"[i % 3];
	perm[0] = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-';
	pw = getpwuid(st.st_uid);
	if (pw) snprintf(L[n++], sizeof(L[0]), "Rights    %s  %.16s", perm, pw->pw_name); else snprintf(L[n++], sizeof(L[0]), "Rights    %s  %u", perm, (unsigned)st.st_uid);
	if (S_ISLNK(st.st_mode)) {
		char t[PATH_MAX], a[40]; ssize_t k = readlink(path, t, sizeof(t) - 1);
		if (k >= 0) { t[k] = 0; tail(a, sizeof(a), t, 29); snprintf(L[n++], sizeof(L[0]), "Points to %s", a); }
	}
	for (int i = 0; i < n; i++) lines[i] = L[i];
	return nzk_help("Properties", NULL, lines, n);
}

static int do_open(void)
{
	struct pane *p = &pn[act]; struct ent *e = cur_ent(); char path[PATH_MAX];
	if (!e) return 0;
	if (is_up(e)) { go_up(p); return 0; }
	if (e->dir) {
		join(path, sizeof(path), p->dir, e->name);
		if (access(path, R_OK | X_OK)) return nzk_message("NimbleCommander", e->name, "This folder cannot be opened");
		snprintf(p->dir, sizeof(p->dir), "%s", path); p->l.sel = 0; pane_scan(p, NULL);
		return 0;
	}
	if (is_media(e->name) && have("nz-player")) return do_play();
	if (has_ext(e->name, "deb")) return do_install();
	return do_view();
}

/* ---------------- options, Usr menu, help ---------------- */
static void opt_value(int i, char *b, size_t n)
{
	snprintf(b, n, "%s", i == 0 ? (cfg.two ? "Two" : "One") : i == 1 ? sort_names[cfg.sort] : i == 2 ? (cfg.hidden ? "On" : "Off") : (cfg.sizes ? "On" : "Off"));
}
static void opt_change(int i, int d)
{
	if (i == 0) cfg.two = !cfg.two; else if (i == 1) cfg.sort = (cfg.sort + 3 + d) % 3; else if (i == 2) cfg.hidden = !cfg.hidden; else cfg.sizes = !cfg.sizes;
}
static int do_options(void)
{
	static const struct nzk_option o[] = {
		{ "Panes on screen", "One: Tab shows the other pane", opt_value, opt_change },
		{ "Sort by", "Folders always come first", opt_value, opt_change },
		{ "Hidden files", "Names that start with a dot", opt_value, opt_change },
		{ "Sizes with two panes", "Off: more room for the names", opt_value, opt_change },
	};
	static int sel; int r = nzk_options("Options", o, 4, &sel);
	cfg_save(); rescan_both(NULL);
	return r;
}
static int do_help(void)
{
	static const char *const l[] = {
		"F/X     up/down       Z/C  page",
		"Tab     other pane    Enter  open",
		"Space   mark/unmark   A  all/none",
		"Backspace  folder above   R  read again",
		"H  this page          2  more actions",
		"3  view               4  edit",
		"5  copy to the other pane",
		"6  move there, or rename",
		"7  new folder         8  delete",
		"T  terminal here      O  options",
		"Esc Esc  leave",
		"5, 6, 8: marked files, else selected",
	};
	return nzk_help("Keys", "v" NZC_VERSION, l, 12);
}
static int do_usr(void)
{
	enum { U_PLAY, U_VIEW, U_EDIT, U_INSTALL, U_RENAME, U_PROPS, U_MARKALL, U_HOME, U_TERM, U_PANES, U_SORT, U_HIDDEN };
	static int sel; struct pane *p = &pn[act]; struct ent *e = cur_ent(); const char *items[14]; int id[14], n = 0, r, file = e && !e->dir, any = e && !is_up(e);
	char panes[40], sort[40], hidden[40]; const char *home = getenv("HOME");
	if (file && is_media(e->name)) { items[n] = "Play with Media Player"; id[n++] = U_PLAY; }
	if (file && has_ext(e->name, "deb")) { items[n] = "Install this package"; id[n++] = U_INSTALL; }
	if (file) { items[n] = is_picture(e->name) && have("nz-image") ? "Show the picture" : "View"; id[n++] = U_VIEW; items[n] = "Edit"; id[n++] = U_EDIT; }
	if (any) { items[n] = "Rename"; id[n++] = U_RENAME; items[n] = "Properties"; id[n++] = U_PROPS; }
	items[n] = p->nmark ? "Unmark all" : "Mark all"; id[n++] = U_MARKALL;
	items[n] = "Go to the home folder"; id[n++] = U_HOME;
	items[n] = "Terminal in this folder"; id[n++] = U_TERM;
	snprintf(panes, sizeof(panes), "Panes on screen: %s", cfg.two ? "two" : "one"); items[n] = panes; id[n++] = U_PANES;
	snprintf(sort, sizeof(sort), "Sort by: %s", sort_names[cfg.sort]); items[n] = sort; id[n++] = U_SORT;
	snprintf(hidden, sizeof(hidden), "Hidden files: %s", cfg.hidden ? "shown" : "not shown"); items[n] = hidden; id[n++] = U_HIDDEN;
	if (sel >= n) sel = 0;
	if ((r = nzk_pick(e && any ? e->name : "Actions", items, n, &sel)) <= 0) return r;
	switch (id[sel]) {
	case U_PLAY: return do_play();
	case U_VIEW: return do_view();
	case U_EDIT: return do_edit();
	case U_INSTALL: return do_install();
	case U_RENAME: return do_rename();
	case U_PROPS: return do_properties();
	case U_MARKALL: { int on = !p->nmark; for (int i = 0; i < p->l.n; i++) set_mark(p, i, on); return 0; }
	case U_TERM: return do_terminal();
	case U_HOME: snprintf(p->dir, sizeof(p->dir), "%s", home && *home ? home : "/"); p->l.sel = 0; pane_scan(p, NULL); return 0;
	case U_PANES: cfg.two = !cfg.two; break;
	case U_SORT: cfg.sort = (cfg.sort + 1) % 3; break;
	case U_HIDDEN: cfg.hidden = !cfg.hidden; break;
	}
	cfg_save(); rescan_both(NULL);
	return 0;
}

/* ---------------- scripted input for tests ---------------- */
static char *script; static double wait_until;
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void screenshot(const char *path)
{
	FILE *f = fopen(path, "wb"); if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", nzui_w, nzui_h);
	for (int i = 0; i < nzui_w * nzui_h; i++) { uint16_t c = nzui_fb[i]; fputc(c >> 8 & 0xf8, f); fputc(c >> 3 & 0xfc, f); fputc(c << 3 & 0xf8, f); }
	fclose(f);
}
static int script_poll(struct nzui_event *ev, int timeout_ms)
{
	static const struct { const char *n; int k; } map[] = { {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
		{"enter", KEY_ENTER}, {"esc", KEY_ESC}, {"tab", KEY_TAB}, {"space", KEY_SPACE}, {"backspace", KEY_BACKSPACE},
		{"dot", 91}, {"minus", 71}, {"underscore", 70}, {"0", KEY_0} };
	static const char rows[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
	char *t = script, *comma; double now = now_s();
	if (quit_req) return -1;
	if (wait_until > now) { if (timeout_ms > 0) usleep(20000); return 0; }
next:
	t = script;
	if (!*t) return timeout_ms ? -1 : 0;     /* the script is finished: leave (but not out of the middle of a copy) */
	if ((comma = strchr(t, ','))) *comma = 0;
	script = comma ? comma + 1 : t + strlen(t);
	if (!strncmp(t, "shot:", 5)) { screenshot(t + 5); goto next; }
	if (!strncmp(t, "wait:", 5)) { wait_until = now + atoi(t + 5) / 1000.0; return 0; }
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcmp(t, map[i].n)) { ev->key = map[i].k; ev->value = 1; return 1; }
	if (strlen(t) == 1 && *t >= '1' && *t <= '9') { ev->key = KEY_1 + *t - '1'; ev->value = 1; return 1; }
	for (int i = 0; strlen(t) == 1 && i < (int)sizeof(rows); i++) if (rows[i] == *t) { ev->key = KEY_Q + i; ev->value = 1; return 1; }
	fprintf(stderr, "nz-commander: unknown key name '%s' in NZC_KEYS\n", t);
	return 0;
}
static int real_poll(struct nzui_event *ev, int timeout_ms) { return quit_req ? -1 : nzui_poll(ev, timeout_ms); }
static void no_flush(void) {}

int main(int argc, char **argv)
{
	struct nzui_event ev; int r = 0; const char *home = getenv("HOME");
	if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) { printf("nz-commander %s\n", NZC_VERSION); return 0; }
	cfg_load();
	for (int i = 0; i < 2; i++) {
		struct stat st;
		if (argc > 1 + i) { char rp[PATH_MAX]; if (realpath(argv[1 + i], rp)) snprintf(pn[i].dir, sizeof(pn[i].dir), "%s", rp); }
		if (!pn[i].dir[0] || stat(pn[i].dir, &st) || !S_ISDIR(st.st_mode)) snprintf(pn[i].dir, sizeof(pn[i].dir), "%s", home && *home ? home : "/");
		if (strlen(pn[i].dir) > 1 && pn[i].dir[strlen(pn[i].dir) - 1] == '/') pn[i].dir[strlen(pn[i].dir) - 1] = 0;
	}
	signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
	headless = getenv("NZC_HEADLESS") != NULL;
	nzk_poll_fn = real_poll;
	if (getenv("NZC_KEYS")) { script = strdup(getenv("NZC_KEYS")); nzk_poll_fn = script_poll; }
	if (headless) { nzui_fb = calloc((size_t)nzui_w * nzui_h, 2); nzk_flush_fn = no_flush; if (!nzui_fb) return 1; if (!script) nzk_poll_fn = script_poll, script = strdup(""); }
	else if (nzui_init("NimbleCommander")) { fprintf(stderr, "nz-commander: no display\n"); return 1; }
	if (headless || script) nzk_idle_allow(0);
	pane_scan(&pn[0], NULL); pane_scan(&pn[1], NULL);
	while (r >= 0) {
		struct pane *p = &pn[act];
		draw();
		if ((r = nzk_wait(&ev, nzk_timeout())) < 0) break;
		if (r != 1 || !ev.value) continue;
		if (nzk_nav(ev.key)) { nzk_list_move(&p->l, nzk_nav(ev.key), ROWS); nzk_leave(0); continue; }
		if (ev.value != 1) continue;
		if (nzk_leave(ev.key)) break;
		switch (ev.key) {
		case KEY_TAB: act = !act; break;
		case KEY_SPACE: if (p->l.n) { set_mark(p, p->l.sel, !p->e[p->l.sel].mark); nzk_list_move(&p->l, NZK_NAV_DOWN, ROWS); } break;
		case KEY_A: { int on = !p->nmark; for (int i = 0; i < p->l.n; i++) set_mark(p, i, on); } break;
		case KEY_ENTER: r = do_open(); break;
		case KEY_BACKSPACE: go_up(p); break;
		case KEY_R: rescan_both(NULL); break;
		case KEY_H: case KEY_1: r = do_help(); break;
		case KEY_2: r = do_usr(); break;
		case KEY_3: r = do_view(); break;
		case KEY_4: r = do_edit(); break;
		case KEY_5: r = do_copy_move(OP_COPY); break;
		case KEY_6: r = do_copy_move(OP_MOVE); break;
		case KEY_7: r = do_mkdir(); break;
		case KEY_8: r = do_delete(); break;
		case KEY_O: r = do_options(); break;
		case KEY_T: r = do_terminal(); break;
		}
	}
	cfg_save();
	if (!headless) nzui_quit();
	return 0;
}
