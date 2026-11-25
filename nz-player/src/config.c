/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - settings and resume positions, kept as text below $HOME/.config/nz-player. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nzp.h"

struct config cfg = {
	.buffer_ms = 3000, .reconnect = 1, .scaler = 3, .aspect = 0, .speed = 0, .seek_s = 10,
	.resume = 1, .av_delay_ms = 0, .volume = 80, .show_info = 0, .hwdec = 1, .subs = 0, .sub_size = 1,
	.user_agent = "nz-player/" NZP_VERSION,
};

#define RESUME_MAX 64
static struct { char path[768]; double pos; } res[RESUME_MAX];
static int nres;

static const char *cfg_dir(void)
{
	static char d[600]; const char *e = getenv("NZP_CONFIG_DIR"), *h = getenv("HOME");
	if (e) snprintf(d, sizeof(d), "%s", e);
	else snprintf(d, sizeof(d), "%s/.config/nz-player", h ? h : "/tmp");
	return d;
}

static void mkdirs(const char *path)
{
	char p[600]; snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
	mkdir(p, 0755);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cfg_load(void)
{
	char p[700], line[1024]; FILE *f;
	snprintf(p, sizeof(p), "%s/config", cfg_dir());
	if ((f = fopen(p, "r"))) {
		while (fgets(line, sizeof(line), f)) {
			char *v = strchr(line, '='); if (!v) continue;
			*v++ = 0; v[strcspn(v, "\r\n")] = 0;
			if (!strcmp(line, "buffer_ms")) cfg.buffer_ms = clampi(atoi(v), 0, 60000);
			else if (!strcmp(line, "reconnect")) cfg.reconnect = !!atoi(v);
			else if (!strcmp(line, "scaler")) cfg.scaler = clampi(atoi(v), 0, 3);
			else if (!strcmp(line, "aspect")) cfg.aspect = clampi(atoi(v), 0, 2);
			else if (!strcmp(line, "speed")) cfg.speed = clampi(atoi(v), 0, 3);
			else if (!strcmp(line, "seek_s")) cfg.seek_s = clampi(atoi(v), 1, 600);
			else if (!strcmp(line, "resume")) cfg.resume = !!atoi(v);
			else if (!strcmp(line, "av_delay_ms")) cfg.av_delay_ms = clampi(atoi(v), -2000, 2000);
			else if (!strcmp(line, "volume")) cfg.volume = clampi(atoi(v), 0, 100);
			else if (!strcmp(line, "show_info")) cfg.show_info = !!atoi(v);
			else if (!strcmp(line, "subs")) cfg.subs = !!atoi(v);
			else if (!strcmp(line, "sub_size")) cfg.sub_size = clampi(atoi(v), 0, 2);
			else if (!strcmp(line, "hwdec")) cfg.hwdec = !!atoi(v);
			else if (!strcmp(line, "user_agent") && *v) snprintf(cfg.user_agent, sizeof(cfg.user_agent), "%s", v);
			else if (!strcmp(line, "dir_movies")) snprintf(cfg.dir[0], sizeof(cfg.dir[0]), "%s", v);
			else if (!strcmp(line, "dir_iptv")) snprintf(cfg.dir[1], sizeof(cfg.dir[1]), "%s", v);
		}
		fclose(f);
	}
	snprintf(p, sizeof(p), "%s/resume", cfg_dir());
	if ((f = fopen(p, "r"))) {
		while (nres < RESUME_MAX && fgets(line, sizeof(line), f)) {
			char *t = strchr(line, '\t'); if (!t) continue;
			*t++ = 0; t[strcspn(t, "\r\n")] = 0;
			res[nres].pos = atof(line); snprintf(res[nres].path, sizeof(res[nres].path), "%s", t); nres++;
		}
		fclose(f);
	}
}

void cfg_save(void)
{
	char p[700], tmp[720]; FILE *f;
	mkdirs(cfg_dir());
	snprintf(p, sizeof(p), "%s/config", cfg_dir()); snprintf(tmp, sizeof(tmp), "%s.new", p);
	if (!(f = fopen(tmp, "w"))) return;
	fprintf(f, "buffer_ms=%d\nreconnect=%d\nscaler=%d\naspect=%d\nspeed=%d\nseek_s=%d\nresume=%d\n"
		"av_delay_ms=%d\nvolume=%d\nshow_info=%d\nhwdec=%d\nsubs=%d\nsub_size=%d\nuser_agent=%s\ndir_movies=%s\ndir_iptv=%s\n",
		cfg.buffer_ms, cfg.reconnect, cfg.scaler, cfg.aspect, cfg.speed, cfg.seek_s, cfg.resume,
		cfg.av_delay_ms, cfg.volume, cfg.show_info, cfg.hwdec, cfg.subs, cfg.sub_size, cfg.user_agent, cfg.dir[0], cfg.dir[1]);
	if (fclose(f) == 0) rename(tmp, p);
}

static void resume_save(void)
{
	char p[700], tmp[720]; FILE *f;
	mkdirs(cfg_dir());
	snprintf(p, sizeof(p), "%s/resume", cfg_dir()); snprintf(tmp, sizeof(tmp), "%s.new", p);
	if (!(f = fopen(tmp, "w"))) return;
	for (int i = 0; i < nres; i++) fprintf(f, "%.1f\t%s\n", res[i].pos, res[i].path);
	if (fclose(f) == 0) rename(tmp, p);
}

double resume_get(const char *path)
{
	for (int i = 0; i < nres; i++) if (!strcmp(res[i].path, path)) return res[i].pos;
	return 0;
}

void resume_set(const char *path, double pos)
{
	int i;
	if (strlen(path) >= sizeof(res[0].path) || strchr(path, '\n') || strchr(path, '\t')) return;
	for (i = 0; i < nres; i++) if (!strcmp(res[i].path, path)) break;
	if (i < nres) { memmove(&res[i], &res[i + 1], (size_t)(nres - i - 1) * sizeof(res[0])); nres--; }
	else if (pos <= 0) return;
	if (pos > 0) {                           /* newest first; the oldest falls off the end */
		if (nres == RESUME_MAX) nres--;
		memmove(&res[1], &res[0], (size_t)nres * sizeof(res[0]));
		snprintf(res[0].path, sizeof(res[0].path), "%s", path); res[0].pos = pos; nres++;
	}
	resume_save();
}
