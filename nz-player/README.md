# nz-player

Movie and IPTV player for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the
M5Stack Cardputer Zero (320x170 screen). It uses the FFmpeg libraries that are already in
the regular NimbleZero image. Licence: GPL-2.0, like NimbleZero.

Version 0.1.1, 2026-11-20, for NimbleZero v0.1.0. It runs on a Cardputer Zero - see "What
has been tested" for how far that goes.

## Install

On the device (Terminal app or SSH), with Wi-Fi connected:

    wget https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-player_0.1.1_arm64.deb
    sudo dpkg -i nz-player_0.1.1_arm64.deb

or copy the file from a PC with `scp nz-player_0.1.1_arm64.deb zero@<device>:` first.

It appears in the launcher as **Media Player** (press R there if the list is not
refreshed). Remove with `sudo dpkg -r nz-player`. The package needs the regular v0.1.0
image (FFmpeg 6.1 and alsa-lib; the sound service is used when it is there).

Copy movies and channel lists to the device, for example
`scp film.mp4 channels.m3u zero@<device>:` (that is `/home/zero`, on the data partition,
where the player starts).

## Using it

The player opens with a file selector. **Tab** switches between its two modes:

- **Movies** - folders and video/sound files. Enter plays.
- **IPTV** - folders and `.m3u` / `.m3u8` channel lists. Enter opens the list: its groups
  (if it has `group-title`s), then its channels. Enter plays a channel.

| Where | Key | Does |
|---|---|---|
| Lists | F / X (or Up / Down) | move |
| | Z / C (or Left / Right) | page up / down |
| | Enter | open / play |
| | Backspace | folder above |
| | Tab | Movies / IPTV |
| | O | options |
| | H | key overview |
| | R | read the folder again |
| | Esc twice, or Q | leave the player |
| Playing | Z / C (or Left / Right) | seek back / forward (movies; step in Options) |
| | F / X (or Up / Down) | volume |
| | Space | pause |
| | Esc (or the same key without Fn, Backspace, Q) | back to the list |
| | A | picture shape: fit, fill, stretch |
| | N / P | next / previous channel or file |
| | T | next sound track |
| | M | mute |
| | J / K | sound 25 ms earlier / later against the picture |
| | 1 - 9 | jump to 10 % - 90 %; 0: start |
| | I | technical overlay (see below) |
| | Enter | keep the bar at the bottom on screen / hide it |
| | O | options (playback pauses meanwhile) |

F, X, Z and C carry the arrow legends on the Cardputer keyboard and work without Fn, as in
the NimbleZero launcher.

### Options

| Option | Values | Meaning |
|---|---|---|
| Stream buffer | Off, 0.5 - 30 s (3 s) | How much of a network stream is collected before it starts, and again whenever it runs dry. Larger: fewer stalls on a weak connection, slower channel change. The bar at the bottom shows the level. Local files are not affected. |
| Reconnect live streams | On / Off | Open a live stream again when it ends or fails (up to five times in a row). |
| Scaling filter | Fast, **Bilinear**, Bicubic, Lanczos | How the picture is shrunk to 320x170. All but "Fast" average over the source pixels that fall on one screen pixel, so fine detail and text do not shimmer; Bicubic and Lanczos are a little sharper and cost a little more. `screenshots/scaling-filters.png` shows the four on a 1080p test picture (top left: Fast). |
| Picture shape | **Fit**, Fill, Stretch | Fit shows the whole picture with black bars; Fill crops the edges; Stretch distorts. Non-square pixels (DVB, DVD) are taken into account. |
| Decoding | **Automatic**, Full quality, Fast, Fastest | Short cuts for when the processor cannot keep up. Automatic leaves out the deblocking filter for video higher than 360 lines (not visible at this screen size), then drops B-frames, and as the last resort shows key frames only, so that picture and sound stay together. |
| Seek step | 5 - 120 s (10 s) | Per key press. |
| Resume movies | On / Off | Continue a file where it was stopped. |
| Sound timing | -1000 ... +1000 ms | Moves the sound against the picture, if lips and voice do not match. |
| Volume | 0 - 100 % | Of this player only. The system volume is in the launcher's Options. |
| Technical overlay | On / Off | Same as key I. |

The technical overlay (key I), line by line: picture codec, size and frame rate of the
source; sound codec and output; pictures shown and decoded per second; pictures skipped
so far - "late" came out of the decoder after their time, "screen" were ready but the
screen loop came too late for them; the decoding short cut in use; "draw a/b ms" - average
and longest time to hand a picture to the screen in the last second, and "loop" - the
longest gap between two screen updates in that second; seconds of stream waiting, and the
screen output in use.

Settings are kept in `~/.config/nz-player/config`, resume positions in
`~/.config/nz-player/resume`. One setting has no menu entry: `user_agent=` in the config
file (what the player calls itself towards stream servers; some providers want a specific
one).

### Channel lists

The usual extended M3U:

    #EXTM3U
    #EXTINF:-1 tvg-name="News 1" group-title="News",News 1 HD
    http://example.org/stream/news1.ts
    #EXTINF:-1 group-title="Films",A film
    #EXTVLCOPT:http-user-agent=SomeAgent/1.0
    http://example.org/films/a.mp4

Also understood: `#EXTGRP:`, `#EXTVLCOPT:http-referrer=`, and
`url|User-Agent=...&Referer=...`. An entry that is not a URL is a file name relative to
the list, so a list of local files works too. A `.m3u8` file that describes a single HLS
stream is played directly. Streams: anything the image's FFmpeg reads - MPEG-TS over HTTP,
HLS, HTTP(S) files, RTMP, UDP/RTP. Names are shown in ASCII (accents are dropped; other
scripts appear as `?`: the built-in font has no more).

## How it works (short)

- `src/player.c` - the engine: a reader thread (demuxing, seeking), a sound thread whose
  progress is the clock, a picture thread (decoding, scaling with libswscale to RGB565),
  and `pl_refresh()` for the user interface to fetch the picture that is due.
- `src/plat.c` - screen and keys through `nzui.h`; on the device each finished picture goes
  to the panel's DRM device with one DIRTYFB call (the tear-free path of NimbleZero v0.1.0,
  as in nz-doom), with `/dev/fb0` as fallback.
- `src/audio.c` - sound through the sound service (libpulse-simple, loaded at run time),
  else ALSA directly, else none.
- `src/m3u.c`, `src/config.c`, `src/main.c` - channel lists, settings, screens.

The launcher sends the player's messages to `/tmp/nz-app.log`. An empty file
`~/.config/nz-player/debug` adds a line per second about the sound device.

## What has been tested, and what has not

Tested (2026-11-20) by running **the packaged arm64 program** under user-mode emulation
(qemu-aarch64) against the root file system of `nimblezero-v0.1.0.img`, i.e. with the
image's own FFmpeg 6.1.5, alsa-lib and PulseAudio 17 libraries, without screen or keyboard
(scripted keys, screenshots):

- Files: H.264+AAC (MP4), H.264 720p + AC-3 5.1 and AAC (MKV, two sound tracks), MPEG-2
  576i 16:9 + MP2 (TS), MP3, a file without sound. Seek, pause, resume position, track
  change, picture shapes, the four scaling filters, end of file.
- Streams from a local test server: live MPEG-TS over HTTP (buffering, level display,
  reconnect, channel change), HLS over HTTP, MP4 over HTTP with seeking, a dead server, a
  missing file, a 12 000-channel list with 40 groups.
- Sound through the image's own PulseAudio daemon (run under the same emulation, with a
  sink that goes nowhere): the program connects and the clock follows the device.
- Installing the package with the image's `dpkg` against the image's package database;
  the image's launcher lists it.
- `tests/run-tests.sh`: 9 checks, pass for the desktop build and for the arm64 program.
- Desktop build under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer: no
  findings in the player's code (ThreadSanitizer reports remain inside FFmpeg's own
  threads).

**On hardware** (project owner's reports, 2026-11-20, one Cardputer Zero with NimbleZero
v0.1.0):

- The player starts from the launcher, draws on the panel and takes keys; local files and
  IPTV streams play; sound is in step with the picture.
- Version 0.1.0 showed about 27 pictures a second with H.264 1080p at 29.97 and 59.94
  frames a second and with MPEG-2 720x480 at 29.97, although the overlay said 30 were
  decoded. Cause: keys were read with `nzui_poll()`, which closes and reopens every input
  device on each fourth time-out; a player's waits time out many times a second, and each
  reopening held the screen loop up.
- Version 0.1.1 reads the input devices itself and reopens them only when a keyboard is
  plugged in or removed: "a solid 30" where 0.1.0 gave 27, and 60 a second with a 720p
  video at 60 frames a second.
- A comparable player on the stock firmware reached single-digit frame rates (owner's
  comparison).

**Not reported from a device yet:** seek, pause, resume, track change and picture shapes
one by one; which sound output is in use ("out:" in the overlay); a USB keyboard plugged
in while the player runs; HTTPS streams; Wi-Fi drop-outs; long runs; battery use; the
screen-off timer while paused; 1080p at 59.94 frames a second with 0.1.1.

## Building

Desktop simulator (Debian/Ubuntu: `libsdl2-dev libavformat-dev libavcodec-dev
libswscale-dev libswresample-dev libasound2-dev`):

    make sim && ./nz-player-sim            # 3x window, PC keyboard
    make check

For the device. The v0.1.0 SDK has the compiler but neither the FFmpeg headers nor the
libraries, so the build takes them from elsewhere:

    make CROSS_COMPILE=~/nz-sdk/bin/aarch64-linux- \
         ROOTFS=/path/to/rootfs-of-nimblezero-v0.1.0 \
         FFMPEG_SRC=/path/to/ffmpeg-6.1.5
    make deb CROSS_COMPILE=... ROOTFS=... FFMPEG_SRC=...

- `ROOTFS`: the second partition of the image, unpacked
  (`debugfs -R "rdump / rootfs" root.ext`); only its libraries are used, for linking.
- `FFMPEG_SRC`: `ffmpeg-6.1.5.tar.xz` from the NimbleZero source archive, unpacked; only
  its header files are used.

`nz-mkdeb` here is the SDK's script with one correction (the package no longer records
mode 0700 for `/`).

Test hooks, any build: `NZP_HEADLESS=1` (no screen or keyboard),
`NZP_KEYS="down,enter,wait:2000,shot:/tmp/a.ppm,esc"` (scripted keys, pauses,
screenshots), `NZP_AUDIO=pulse|alsa|none`, `NZP_CONFIG_DIR`, `NZP_DEBUG=1`.

## Not in this version

Subtitles, hardware video decoding (the v0.1.0 kernel has no decoder driver enabled),
a way to type in a stream address on the device, favourites, programme guide (EPG),
playlists that repeat or shuffle.
