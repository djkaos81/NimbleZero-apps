/* SPDX-License-Identifier: GPL-2.0-only
 * NimbleCommander - copy, move and delete.
 *
 * Folders are handled entry by entry; symbolic links are copied as links, never followed;
 * devices, sockets and pipes are left out (counted as skipped).  A copied file keeps its
 * permissions and its date.  A file that could not be written completely is removed.
 * The progress screen appears after a quarter of a second and is redrawn about seven times
 * a second; Esc there cancels.  NZC_PROGRESS_NOW=1 (tests): show it at once.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input-event-codes.h>
#include "nzc.h"

static struct op_result *R;
static const char *op_title;
static long long total_bytes, done_bytes; static long total_items, done_items;
static int by_items, overwrite_all, replace_once; static double next_draw;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s && s[1] ? s + 1 : p; }
static void fail(const char *path, int err)
{
	if (R->failed++) return;
	snprintf(R->err_name, sizeof(R->err_name), "%s", base(path));
	snprintf(R->err_text, sizeof(R->err_text), "%s", strerror(err));
}
static int stop(void) { return R->cancelled || R->failed || R->quit; }

/* draw the progress screen now and then, and look for Esc */
static void tick(const char *path)
{
	struct nzui_event ev; int r; char l2[48], a[12], b[12]; double now = now_s();
	if (now < next_draw) return;
	next_draw = now + 0.15;
	if (by_items) snprintf(l2, sizeof(l2), "%ld of %ld", done_items, total_items);
	else { fmt_size(a, sizeof(a), done_bytes); fmt_size(b, sizeof(b), total_bytes); snprintf(l2, sizeof(l2), "%s of %s", a, b); }
	nzk_progress(op_title, base(path), l2,
		by_items ? (total_items ? (int)(done_items * 1000 / total_items) : 0) : (total_bytes ? (int)(done_bytes * 1000 / total_bytes) : 0),
		"Esc:cancel");
	while ((r = nzk_wait(&ev, 0)) == 1) if (ev.key == KEY_ESC && ev.value == 1) R->cancelled = 1;
	if (r < 0) R->quit = 1;
}

int tree_size(const char *path, long long *bytes, long *items)
{
	struct stat st; DIR *d; struct dirent *e; int rc = 0;
	if (lstat(path, &st)) return -1;
	(*items)++;
	if (!S_ISDIR(st.st_mode)) { if (S_ISREG(st.st_mode)) *bytes += st.st_size; return 0; }
	if (!(d = opendir(path))) return -1;
	while ((e = readdir(d))) {
		char p[PATH_MAX];
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p)) { rc = -1; continue; }
		if (tree_size(p, bytes, items)) rc = -1;
	}
	closedir(d);
	return rc;
}

static void delete_tree(const char *path)
{
	struct stat st;
	if (stop()) return;
	if (lstat(path, &st)) { fail(path, errno); return; }
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path); struct dirent *e;
		if (!d) { fail(path, errno); return; }
		while (!stop() && (e = readdir(d))) {
			char p[PATH_MAX];
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p)) { fail(path, ENAMETOOLONG); break; }
			delete_tree(p);
		}
		closedir(d);
		if (stop()) return;
		if (rmdir(path)) { fail(path, errno); return; }
	} else if (unlink(path)) { fail(path, errno); return; }
	done_items++;
	if (by_items) tick(path);
}

/* something is in the way at dst: 1 replace it, 0 leave it (skipped) */
static int may_replace(const char *dst)
{
	static const int keys[] = { KEY_ENTER, KEY_S, KEY_A }; char l1[64]; int r;
	if (replace_once) { replace_once = 0; return 1; }
	if (overwrite_all) return 1;
	snprintf(l1, sizeof(l1), "%.28s exists.", base(dst));
	r = nzk_ask(op_title, l1, "Replace it?", "Enter:replace S:skip A:all Esc:cancel", keys, 3);
	next_draw = 0;
	if (r < 0) { R->quit = 1; return 0; }
	if (r == 0) { R->cancelled = 1; return 0; }
	if (r == KEY_S) { R->skipped++; return 0; }
	if (r == KEY_A) overwrite_all = 1;
	return 1;
}

static void copy_file(const char *src, const char *dst, const struct stat *st)
{
	static char buf[256 * 1024]; int in, out; ssize_t n = 0; struct timespec ts[2] = { st->st_atim, st->st_mtim };
	if ((in = open(src, O_RDONLY | O_CLOEXEC)) < 0) { fail(src, errno); return; }
	if ((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st->st_mode & 0777)) < 0) { fail(dst, errno); close(in); return; }
	while (!stop() && (n = read(in, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off < n; ) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) { if (errno == EINTR) continue; fail(dst, errno); break; }
			off += w; done_bytes += w;
		}
		tick(src);
	}
	if (n < 0) fail(src, errno);
	close(in);
	if (!stop()) { if (fchmod(out, st->st_mode & 07777)) {} if (futimens(out, ts)) {} }
	if (close(out) && !stop()) fail(dst, errno);
	if (stop()) unlink(dst);                 /* never leave half a file behind */
}

static void copy_tree(const char *src, const char *dst)
{
	struct stat st, dt; int there;
	if (stop()) return;
	if (lstat(src, &st)) { fail(src, errno); return; }
	there = !lstat(dst, &dt);
	if (there && st.st_dev == dt.st_dev && st.st_ino == dt.st_ino) { fail(src, EEXIST); return; }   /* onto itself */
	if (S_ISDIR(st.st_mode)) {
		DIR *d; struct dirent *e; struct timespec ts[2] = { st.st_atim, st.st_mtim };
		if (there && !S_ISDIR(dt.st_mode)) { fail(dst, ENOTDIR); return; }
		if (!there && mkdir(dst, 0700 | (st.st_mode & 0777))) { fail(dst, errno); return; }
		if (!(d = opendir(src))) { fail(src, errno); return; }
		while (!stop() && (e = readdir(d))) {
			char s[PATH_MAX], t[PATH_MAX];
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			if (snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) || snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) { fail(src, ENAMETOOLONG); break; }
			copy_tree(s, t);
		}
		closedir(d);
		if (!stop() && !there) { if (chmod(dst, st.st_mode & 07777)) {} if (utimensat(AT_FDCWD, dst, ts, 0)) {} }
		return;
	}
	if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) { R->skipped++; return; }
	if (there) {
		if (S_ISDIR(dt.st_mode)) { fail(dst, EISDIR); return; }
		if (!may_replace(dst)) return;
		if (S_ISLNK(dt.st_mode) && unlink(dst)) { fail(dst, errno); return; }   /* replace the link, not what it points to */
		if (S_ISLNK(dt.st_mode)) there = 0;
	}
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX]; ssize_t n = readlink(src, target, sizeof(target) - 1);
		if (n < 0) { fail(src, errno); return; }
		target[n] = 0;
		if (there && unlink(dst)) { fail(dst, errno); return; }
		if (symlink(target, dst)) fail(dst, errno);
		return;
	}
	copy_file(src, dst, &st);
}

/* is `path` the folder `dir` itself or somewhere below it? */
static int inside(const char *path, const char *dir)
{
	char a[PATH_MAX], b[PATH_MAX]; size_t n;
	if (!realpath(path, a) || !realpath(dir, b)) return 0;
	n = strlen(b);
	return !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/' || n == 1);
}

void op_run(int op, const char *srcdir, char *const *names, int n, const char *dstdir, const char *newname, struct op_result *res)
{
	R = res; memset(R, 0, sizeof(*R));
	op_title = op == OP_COPY ? "Copy" : op == OP_MOVE ? "Move" : "Delete";
	total_bytes = done_bytes = 0; total_items = done_items = 0; overwrite_all = replace_once = 0; next_draw = 0;
	by_items = op == OP_DELETE;
	/* something quick should not flash a progress screen: the first one after a quarter second */
	if (!getenv("NZC_PROGRESS_NOW")) next_draw = now_s() + 0.25;
	for (int i = 0; i < n; i++) {
		char s[PATH_MAX];
		snprintf(s, sizeof(s), "%s/%s", strcmp(srcdir, "/") ? srcdir : "", names[i]);
		tree_size(s, &total_bytes, &total_items);
	}
	for (int i = 0; i < n && !stop(); i++) {
		char s[PATH_MAX], t[PATH_MAX]; struct stat st, dt; int skipped = R->skipped;
		if (snprintf(s, sizeof(s), "%s/%s", strcmp(srcdir, "/") ? srcdir : "", names[i]) >= (int)sizeof(s)) { fail(names[i], ENAMETOOLONG); break; }
		if (op == OP_DELETE) { delete_tree(s); if (!stop()) R->done++; continue; }
		if (snprintf(t, sizeof(t), "%s/%s", strcmp(dstdir, "/") ? dstdir : "", newname ? newname : names[i]) >= (int)sizeof(t)) { fail(names[i], ENAMETOOLONG); break; }
		if (lstat(s, &st)) { fail(s, errno); break; }
		if (S_ISDIR(st.st_mode) && inside(dstdir, s)) {
			if (!R->failed++) { snprintf(R->err_name, sizeof(R->err_name), "%s", names[i]); snprintf(R->err_text, sizeof(R->err_text), "A folder cannot go into itself"); }
			break;
		}
		if (op == OP_MOVE) {
			int there = !lstat(t, &dt), merge = there && S_ISDIR(st.st_mode) && S_ISDIR(dt.st_mode);
			if (there && st.st_dev == dt.st_dev && st.st_ino == dt.st_ino) { fail(s, EEXIST); break; }
			if (there && !merge) {
				if (S_ISDIR(st.st_mode) || S_ISDIR(dt.st_mode)) { fail(t, S_ISDIR(dt.st_mode) ? EISDIR : ENOTDIR); break; }
				if (!may_replace(t)) continue;       /* skipped, or cancelled */
				replace_once = 1;                    /* answered: do not ask again if it has to be copied */
			}
			if (!merge) {
				if (!rename(s, t)) { replace_once = 0; R->done++; tick(s); continue; }
				if (errno != EXDEV) { fail(s, errno); break; }
			}
			/* another file system, or a folder merged into an existing one: copy, then delete */
			copy_tree(s, t);
			replace_once = 0;
			if (!stop() && R->skipped == skipped) { delete_tree(s); if (!stop()) R->done++; }
			continue;
		}
		copy_tree(s, t);
		if (!stop() && R->skipped == skipped) R->done++;
	}
	sync();                                  /* the device is switched off with its power switch */
}
