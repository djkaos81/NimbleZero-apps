/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - sound output.
 *
 * Three outputs, tried in this order (NZP_AUDIO=pulse|alsa|none forces one):
 *   pulse  the sound service of the regular NimbleZero image.  libpulse-simple is loaded
 *          at run time, so the player also starts on a system that has no sound service.
 *   alsa   the sound card directly ("default" device).
 *   none   no device: the sound is thrown away at the speed it would have played, so the
 *          picture still runs at the right pace.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "nzp.h"

#define TARGET_MS 120                        /* how much sound we keep queued in the device */

static enum { A_NONE, A_PULSE, A_ALSA } kind;
static int a_rate = 48000, a_ch = 2;

/* ---- PulseAudio "simple" API, declared here so that no pulse headers are needed ---- */
struct pa_sample_spec { int format; uint32_t rate; uint8_t channels; };
struct pa_buffer_attr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
static void *pa_lib, *pa_s;
static void *(*pa_new)(const char *, const char *, int, const char *, const char *, const struct pa_sample_spec *, const void *, const struct pa_buffer_attr *, int *);
static void (*pa_free_)(void *);
static int (*pa_write)(void *, const void *, size_t, int *);
static int (*pa_flush)(void *, int *);
static uint64_t (*pa_latency)(void *, int *);

static snd_pcm_t *pcm;
static double none_until;                    /* "none": time at which the queued sound ends */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int pulse_open(void)
{
	struct pa_sample_spec ss = { 3 /* PA_SAMPLE_S16LE */, (uint32_t)a_rate, (uint8_t)a_ch };
	struct pa_buffer_attr ba; int err = 0;
	if (!pa_lib) {
		if (!(pa_lib = dlopen("libpulse-simple.so.0", RTLD_NOW))) return -1;
		pa_new = dlsym(pa_lib, "pa_simple_new"); pa_free_ = dlsym(pa_lib, "pa_simple_free");
		pa_write = dlsym(pa_lib, "pa_simple_write"); pa_flush = dlsym(pa_lib, "pa_simple_flush");
		pa_latency = dlsym(pa_lib, "pa_simple_get_latency");
	}
	if (!pa_new || !pa_free_ || !pa_write || !pa_flush || !pa_latency) return -1;
	ba.maxlength = (uint32_t)-1; ba.prebuf = (uint32_t)-1; ba.minreq = (uint32_t)-1; ba.fragsize = (uint32_t)-1;
	ba.tlength = (uint32_t)(a_rate * a_ch * 2 * TARGET_MS / 1000);
	pa_s = pa_new(NULL, "nz-player", 1 /* PA_STREAM_PLAYBACK */, NULL, "playback", &ss, NULL, &ba, &err);
	if (!pa_s) { fprintf(stderr, "nz-player: sound service not usable (error %d)\n", err); return -1; }
	return 0;
}

static int alsa_open(void)
{
	const char *dev = getenv("NZP_ALSA_DEVICE");
	const char *names[] = { dev ? dev : "default", "plughw:0,0" };
	for (int i = 0; i < (dev ? 1 : 2); i++) {    /* plughw converts what the card itself cannot take */
		if (snd_pcm_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0) < 0) { pcm = NULL; continue; }
		if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned)a_ch, (unsigned)a_rate, 1, TARGET_MS * 1000) == 0) return 0;
		snd_pcm_close(pcm); pcm = NULL;
	}
	return -1;
}

int aout_open(int rate, int channels)
{
	const char *force = getenv("NZP_AUDIO");
	aout_close();
	a_rate = rate; a_ch = channels; kind = A_NONE; none_until = 0;
	if ((!force || !strcmp(force, "pulse")) && !pulse_open()) kind = A_PULSE;
	else if ((!force || !strcmp(force, "alsa")) && !alsa_open()) kind = A_ALSA;
	fprintf(stderr, "nz-player: sound output %s, %d Hz, %d channels\n", aout_name(), rate, channels);
	return 0;
}

void aout_close(void)
{
	if (pa_s) { pa_free_(pa_s); pa_s = NULL; }
	if (pcm) { snd_pcm_close(pcm); pcm = NULL; }
	kind = A_NONE;
}

const char *aout_name(void) { return kind == A_PULSE ? "pulse" : kind == A_ALSA ? "alsa" : "none"; }

static void fail_to_none(const char *what)
{
	fprintf(stderr, "nz-player: sound output %s failed (%s); continuing without sound\n", aout_name(), what);
	aout_close(); none_until = 0;
}

void aout_write(const int16_t *s, int frames)
{
	if (kind == A_PULSE) {
		int err = 0;
		if (pa_write(pa_s, s, (size_t)frames * a_ch * 2, &err) < 0) fail_to_none("write");
		else return;
	}
	if (kind == A_ALSA) {
		while (frames > 0) {
			snd_pcm_sframes_t n = snd_pcm_writei(pcm, s, (snd_pcm_uframes_t)frames);
			if (n < 0) n = snd_pcm_recover(pcm, (int)n, 1);
			if (n < 0) { fail_to_none("write"); break; }
			s += n * a_ch; frames -= (int)n;
		}
		if (kind == A_ALSA) return;
	}
	/* none: behave like a device with TARGET_MS of buffer */
	double now = now_s();
	if (none_until < now) none_until = now;
	none_until += (double)frames / a_rate;
	if (none_until - now > TARGET_MS / 1000.0) usleep((useconds_t)((none_until - now - TARGET_MS / 1000.0) * 1e6));
}

double aout_latency(void)
{
	if (kind == A_PULSE) { int err = 0; uint64_t us = pa_latency(pa_s, &err); return err ? TARGET_MS / 1000.0 : us / 1e6; }
	if (kind == A_ALSA) { snd_pcm_sframes_t d = 0; if (snd_pcm_delay(pcm, &d) < 0 || d < 0) d = 0; return (double)d / a_rate; }
	double left = none_until - now_s();
	return left > 0 ? left : 0;
}

void aout_flush(void)
{
	if (kind == A_PULSE) { int err = 0; pa_flush(pa_s, &err); }
	else if (kind == A_ALSA) { snd_pcm_drop(pcm); snd_pcm_prepare(pcm); }
	else none_until = 0;
}
