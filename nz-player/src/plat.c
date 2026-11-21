/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - screen and keyboard.
 *
 * Drawing and keys come from nzui.h (the NimbleZero SDK header).  On the device the
 * finished picture is not written to /dev/fb0 but handed to the panel's DRM device with
 * one DIRTYFB call per frame, the same way nz-doom does it: that path is synchronised
 * with the panel (no tearing, up to 60 pictures a second, see docs/DISPLAY.md of
 * NimbleZero).  If the DRM device cannot be used, nzui's /dev/fb0 output is used.
 *
 * For tests (any build):
 *   NZP_HEADLESS=1     no screen, no keyboard
 *   NZP_KEYS="down,enter,wait:1500,shot:/tmp/a.ppm,esc"
 *                      scripted input: key names, pauses in ms, screenshots; the program
 *                      leaves when the script is finished
 */
#define NZUI_IMPLEMENTATION
#include "nzp.h"
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>

static volatile sig_atomic_t quit_req;
static int idle_ok, idle_s = 120, screen_off; static double last_key;
static int headless;
static char *script; static double wait_until;
static uint16_t shadow[SCR_W * SCR_H];
static const char *out_name = "none";

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void on_signal(int s) { (void)s; quit_req = 1; }

#if !defined(NZUI_SDL)
static unsigned dev_sig; static double dev_next_scan;
static unsigned dev_signature(void);
#include <errno.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
static int drm_fd = -1, drm_w, drm_h, drm_pitch; static uint32_t drm_fb_id;
static uint8_t *drm_px = MAP_FAILED;

static int drm_try(const char *path)
{
	struct drm_mode_card_res res = {0}; struct drm_mode_get_connector conn = {0};
	struct drm_mode_create_dumb cd = {0}; struct drm_mode_fb_cmd fbc = {0};
	struct drm_mode_map_dumb md = {0}; struct drm_mode_crtc crtc = {0};
	struct drm_mode_modeinfo modes[8]; uint32_t crtcs[4], conns[4], encs[4];
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;
	res.crtc_id_ptr = (uintptr_t)crtcs; res.count_crtcs = 4;
	res.connector_id_ptr = (uintptr_t)conns; res.count_connectors = 4;
	res.encoder_id_ptr = (uintptr_t)encs; res.count_encoders = 4;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors) goto bad;
	conn.connector_id = conns[0]; conn.modes_ptr = (uintptr_t)modes; conn.count_modes = 8;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes) goto bad;
	if (modes[0].hdisplay < SCR_W || modes[0].vdisplay < SCR_H) goto bad;
	cd.width = modes[0].hdisplay; cd.height = modes[0].vdisplay; cd.bpp = 16;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto bad;
	fbc.width = cd.width; fbc.height = cd.height; fbc.pitch = cd.pitch; fbc.bpp = 16; fbc.depth = 16; fbc.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fbc)) goto bad;
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) goto bad;
	drm_px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (drm_px == MAP_FAILED) goto bad;
	memset(drm_px, 0, cd.size);
	crtc.crtc_id = crtcs[0]; crtc.fb_id = fbc.fb_id; crtc.set_connectors_ptr = (uintptr_t)conns;
	crtc.count_connectors = 1; crtc.mode = modes[0]; crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) { munmap(drm_px, cd.size); drm_px = MAP_FAILED; goto bad; }
	drm_fd = fd; drm_fb_id = fbc.fb_id; drm_w = cd.width; drm_h = cd.height; drm_pitch = cd.pitch;
	return 0;
bad:
	fprintf(stderr, "nz-player: %s: not usable as DRM output (%s)\n", path, strerror(errno));
	close(fd);
	return -1;
}
#endif

int plat_init(void)
{
	const char *k = getenv("NZP_KEYS");
	headless = getenv("NZP_HEADLESS") != NULL;
	if (k) script = strdup(k);
	signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
	if (headless) {
		nzui_fb = calloc(SCR_W * SCR_H, 2); out_name = "headless";
		return nzui_fb ? 0 : -1;
	}
	{
		FILE *f = fopen("/data/etc/nz/screen-off", "r"); int v;
		if (f) { if (fscanf(f, "%d", &v) == 1 && v >= 0) idle_s = v; fclose(f); }
		if (getenv("NZ_IDLE_S")) idle_s = atoi(getenv("NZ_IDLE_S"));
		last_key = now_s();
	}
	if (nzui_init("nz-player")) return -1;
	if (nzui_w != SCR_W || nzui_h != SCR_H) { fprintf(stderr, "nz-player: the screen is %dx%d, expected %dx%d\n", nzui_w, nzui_h, SCR_W, SCR_H); nzui_quit(); return -1; }
#if defined(NZUI_SDL)
	out_name = "SDL";
#else
	out_name = "fbdev";
	dev_sig = dev_signature(); dev_next_scan = now_s() + 2.0;   /* nzui_init() has just opened the devices */
	if (!getenv("NZP_NO_DRM") && (!drm_try("/dev/dri/cardputer-zero-internal") || !drm_try("/dev/dri/card0") ||
	    !drm_try("/dev/dri/card1") || !drm_try("/dev/dri/card2"))) out_name = "DRM";
#endif
	fprintf(stderr, "nz-player: screen output %s\n", out_name);
	return 0;
}

const char *plat_output(void) { return out_name; }

/* ---- screen off when nothing happens, as in the launcher (same setting: Options > Screen off) ---- */
#if !defined(NZUI_SDL)
static void backlight_power(int on)
{
	DIR *d = opendir("/sys/class/backlight"); struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[320]; FILE *f; if (e->d_name[0] == '.') continue;
		snprintf(p, sizeof(p), "/sys/class/backlight/%s/bl_power", e->d_name);
		if ((f = fopen(p, "w"))) { fputs(on ? "0\n" : "4\n", f); fclose(f); }
	}
	if (d) closedir(d);
}
#else
static void backlight_power(int on) { (void)on; }
#endif
void plat_allow_idle(int ok) { idle_ok = ok; if (!ok) last_key = now_s(); }

/* how long handing a picture to the screen takes (shown in the technical overlay) */
static double pres_sum, pres_max; static unsigned pres_n;
void plat_present_stats(double *avg_ms, double *max_ms)
{
	*avg_ms = pres_n ? pres_sum / pres_n * 1000 : 0; *max_ms = pres_max * 1000;
	pres_sum = pres_max = 0; pres_n = 0;
}

#if !defined(NZUI_SDL)
/* Keys.  nzui_poll() closes and reopens every input device on each fourth time-out; that
 * suits a launcher that waits for minutes, but a player's waits end many times a second,
 * and each reopening holds the picture up.  Here the devices are reopened only when the
 * set of /dev/input/event* names has changed (a USB keyboard came or went). */
static unsigned dev_signature(void)
{
	DIR *d = opendir("/dev/input"); struct dirent *e; unsigned sig = 1;
	while (d && (e = readdir(d))) {
		unsigned h = 5381;
		if (strncmp(e->d_name, "event", 5)) continue;
		for (const char *c = e->d_name; *c; c++) h = h * 33 + (unsigned char)*c;
		sig += h;
	}
	if (d) closedir(d);
	return sig;
}
static int dev_poll(struct nzui_event *ev, int timeout_ms)
{
	static struct input_event q[64]; static int qn, qi;
	for (;;) {
		double now;
		while (qi < qn) { struct input_event *e = &q[qi++]; if (e->type == EV_KEY) { ev->key = e->code; ev->value = e->value; return 1; } }
		now = now_s();
		if (now >= dev_next_scan) {
			unsigned s = dev_signature();
			if (s != dev_sig || !nzui__nin) { nzui__inputs(); dev_sig = s; }
			dev_next_scan = now + 2.0;
		}
		if (poll(nzui__in, nzui__nin, timeout_ms) <= 0) return 0;
		qn = qi = 0;
		for (int i = 0; i < nzui__nin && !qn; i++)
			if (nzui__in[i].revents & POLLIN) { ssize_t n = read(nzui__in[i].fd, q, sizeof(q)); if (n > 0) qn = (int)(n / sizeof(q[0])); }
			else if (nzui__in[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { nzui__inputs(); dev_sig = dev_signature(); return 0; }
		timeout_ms = 0;
		if (!qn) return 0;
	}
}
#else
#define dev_poll nzui_poll
#endif

static void present_now(void);
void plat_present(void)
{
	double t0 = now_s(), dt;
	present_now();
	dt = now_s() - t0; pres_sum += dt; pres_n++; if (dt > pres_max) pres_max = dt;
}
static void present_now(void)
{
	if (script) memcpy(shadow, nzui_fb, sizeof(shadow));
	if (headless) return;
#if !defined(NZUI_SDL)
	if (drm_fd >= 0) {
		struct drm_mode_fb_dirty_cmd d = { .fb_id = drm_fb_id };
		int ox = (drm_w - SCR_W) / 2, oy = (drm_h - SCR_H) / 2;
		for (int y = 0; y < SCR_H; y++) memcpy(drm_px + (size_t)(y + oy) * drm_pitch + ox * 2, nzui_fb + y * SCR_W, SCR_W * 2);
		ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &d);
		return;
	}
#endif
	nzui_flush();
}

static void screenshot(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
	for (int i = 0; i < SCR_W * SCR_H; i++) { uint16_t c = shadow[i]; fputc(c >> 8 & 0xf8, f); fputc(c >> 3 & 0xfc, f); fputc(c << 3 & 0xf8, f); }
	fclose(f);
}

static int key_by_name(const char *t)
{
	static const struct { const char *name; int key; } map[] = {
		{"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"enter", KEY_ENTER},
		{"esc", KEY_ESC}, {"tab", KEY_TAB}, {"space", KEY_SPACE}, {"backspace", KEY_BACKSPACE},
		{"comma", KEY_COMMA}, {"dot", KEY_DOT},
	};
	static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm"; const char *p;
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (!strcmp(t, map[i].name)) return map[i].key;
	if (strlen(t) != 1) return 0;
	if (*t >= '1' && *t <= '9') return KEY_1 + *t - '1';
	if (*t == '0') return KEY_0;
	if ((p = strchr(row1, *t))) return KEY_Q + (int)(p - row1);
	if ((p = strchr(row2, *t))) return KEY_A + (int)(p - row2);
	if ((p = strchr(row3, *t))) return KEY_Z + (int)(p - row3);
	return 0;
}

int plat_poll(struct nzui_event *ev, int timeout_ms)
{
	if (quit_req) return -1;
	if (timeout_ms < 0) timeout_ms = 1000;
	if (script) {
		double now = now_s();
		if (wait_until > now) {
			double left = (wait_until - now) * 1000;
			usleep((useconds_t)((left < timeout_ms ? left : timeout_ms) * 1000));
			return 0;
		}
		if (!*script) return -1;
		char *t = script, *comma = strchr(t, ',');
		if (comma) *comma = 0;
		script = comma ? comma + 1 : t + strlen(t);
		if (!strncmp(t, "wait:", 5)) { wait_until = now + atoi(t + 5) / 1000.0; return 0; }
		if (!strncmp(t, "shot:", 5)) { screenshot(t + 5); return 0; }
		if ((ev->key = key_by_name(t))) { ev->value = 1; return 1; }
		fprintf(stderr, "nz-player: unknown key name '%s' in NZP_KEYS\n", t);
		return 0;
	}
	if (headless) { usleep((useconds_t)timeout_ms * 1000); return 0; }
	int r;
	if (screen_off) timeout_ms = 1000;       /* nothing is drawn while the screen is off */
	r = dev_poll(ev, timeout_ms);
	if (r == 1 && ev->value) {
		last_key = now_s();
		if (screen_off) { screen_off = 0; backlight_power(1); return 0; }   /* the first key only wakes the screen */
	} else if (!screen_off && idle_ok && idle_s > 0 && now_s() - last_key >= idle_s) { screen_off = 1; backlight_power(0); }
	/* The key at the top left of the Cardputer is ` without Fn and Esc with it: both leave. */
	if (r == 1 && ev->key == KEY_GRAVE) ev->key = KEY_ESC;
	if (r == 1 && ev->key == KEY_KPENTER) ev->key = KEY_ENTER;
	return r;
}

void plat_quit(void)
{
	if (headless) return;
	if (screen_off) backlight_power(1);
#if !defined(NZUI_SDL)
	if (drm_fd >= 0) { close(drm_fd); drm_fd = -1; }       /* the kernel puts /dev/fb0 back on the panel */
#endif
	nzui_quit();
}
