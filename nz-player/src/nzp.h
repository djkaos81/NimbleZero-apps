/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - movie and IPTV player for NimbleZero.  Shared declarations. */
#ifndef NZP_H
#define NZP_H
#include <stddef.h>
#include <stdint.h>
#include "nzui.h"

#define NZP_VERSION "0.1.1"
#define SCR_W 320
#define SCR_H 170

/* ---------------- settings (config.c) ---------------- */
struct config {
	int buffer_ms;     /* how much of a network stream is collected before it plays */
	int reconnect;     /* reopen a live stream that ends or fails */
	int scaler;        /* 0 fast bilinear, 1 bilinear, 2 bicubic, 3 Lanczos */
	int aspect;        /* 0 fit, 1 fill (crop), 2 stretch */
	int speed;         /* 0 auto, 1 full quality, 2 fast, 3 fastest */
	int seek_s;        /* seconds per left/right press */
	int resume;        /* continue movies where they were stopped */
	int av_delay_ms;   /* > 0: sound later against the picture, < 0: earlier */
	int volume;        /* 0..100, applied to this player's sound only */
	int show_info;     /* technical overlay */
	char user_agent[160];
	char dir[2][512];  /* last folder of each mode */
};
extern struct config cfg;
void   cfg_load(void);
void   cfg_save(void);
double resume_get(const char *path);
void   resume_set(const char *path, double pos);   /* pos <= 0 forgets the entry */

/* ---------------- screen and keys (plat.c) ---------------- */
int  plat_init(void);
void plat_quit(void);
void plat_present(void);                                /* nzui_fb -> screen */
void plat_present_stats(double *avg_ms, double *max_ms); /* since the last call */
int  plat_poll(struct nzui_event *ev, int timeout_ms);  /* 1 key, 0 nothing, -1 leave */
const char *plat_output(void);
void plat_allow_idle(int ok);                            /* may the screen switch off when no key is pressed? */
extern int ui_scrolling;                                /* a too-long name is moving: redraw often */

/* ---------------- sound output (audio.c) ---------------- */
int    aout_open(int rate, int channels);               /* signed 16 bit; never fails: falls back to "none" */
void   aout_close(void);
void   aout_write(const int16_t *s, int frames);        /* blocks while the device is full */
double aout_latency(void);                              /* seconds written but not heard yet */
void   aout_flush(void);
const char *aout_name(void);

/* ---------------- playlists (m3u.c) ---------------- */
struct chan { char *name, *group, *url, *ua, *ref; };
struct m3u { struct chan *c; int n; char **groups; int ngroups; };
int  m3u_load(const char *path, struct m3u *out);       /* 0 ok, 1 it is an HLS stream description, -1 unreadable */
void m3u_free(struct m3u *m);
void text_ascii(char *dst, size_t n, const char *src);  /* UTF-8 -> what the 8x8 font can draw */
int  is_url(const char *s);

/* ---------------- playback engine (player.c) ---------------- */
enum pl_state { PL_OPENING, PL_BUFFERING, PL_PLAYING, PL_PAUSED, PL_ENDED, PL_ERROR };
struct pl_info {
	char vcodec[24], acodec[24], alang[8], sink[8];
	int w, h, rate, channels, n_audio, audio_no, level, net, live, seekable, has_video, has_audio;
	double fps, buffered, buffer_need;
	unsigned long shown, dropped, dropped_late, dropped_disp, decoded;   /* late: out of the decoder too late; disp: the screen loop came too late */
};
void   pl_open(const char *url, const char *ua, const char *referer, double start_pos);
void   pl_close(void);
enum pl_state pl_state(void);
const char *pl_error(void);
double pl_position(void);            /* seconds from the start */
double pl_duration(void);            /* 0 if unknown (live) */
int    pl_seek_rel(double seconds);  /* -1 if the stream cannot seek */
int    pl_seek_abs(double seconds);
void   pl_pause(int on);
int    pl_paused(void);
int    pl_cycle_audio(void);         /* number of the sound track now selected, 0 if there is only one */
void   pl_info(struct pl_info *i);
/* Copy the picture that is due into fb (SCR_W x SCR_H, RGB565).  Returns 1 if fb changed;
 * *wait_ms is how long until the next picture is due. */
int    pl_refresh(uint16_t *fb, int *wait_ms);
#endif
