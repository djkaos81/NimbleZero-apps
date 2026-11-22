/* SPDX-License-Identifier: GPL-2.0-only
 * NimbleCommander (nz-commander) - file manager for NimbleZero.  Shared declarations. */
#ifndef NZC_H
#define NZC_H
#include <limits.h>
#include <sys/types.h>
#include "nzui_kit.h"

#define NZC_VERSION "0.2.0"

/* ---- main.c ---- */
void fmt_size(char *o, size_t n, long long s);
extern int headless;

/* ---- ops.c: copy, move, delete with a progress screen; Esc cancels ---- */
enum { OP_COPY, OP_MOVE, OP_DELETE };
struct op_result {
	int done, skipped, cancelled, failed, quit;      /* quit: the app was asked to leave meanwhile */
	char err_name[256], err_text[128];              /* the first failure */
};
/* names[]: entries of srcdir.  dstdir: target folder (not for OP_DELETE).  newname: if not
 * NULL there is one name and the target gets this name instead. */
void op_run(int op, const char *srcdir, char *const *names, int n, const char *dstdir, const char *newname, struct op_result *res);
int  tree_size(const char *path, long long *bytes, long *items);   /* 0 ok */

/* ---- view.c ---- */
int view_file(const char *path, const char *name);   /* -1: leave the app */
#endif
