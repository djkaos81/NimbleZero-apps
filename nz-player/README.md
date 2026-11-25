# nz-player

Movie and IPTV player for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the
M5Stack Cardputer Zero (320x170 screen). It uses the FFmpeg libraries that are already in
the regular NimbleZero image. Licence: GPL-2.0, like NimbleZero.

Version 0.3.8, 2026-11-25, for the development images of the next NimbleZero version
(package set `0.2`). Tried on one Cardputer Zero step by step - see "What has been tested"
for how far that goes. For NimbleZero v0.1.0 the package is still 0.1.1.

## Install

**Development images of the next version** (image dev10 and later, package set `0.2`):
Store > NimbleZero apps > Media Player. The Store first downloads what the player needs
(ffmpeg and its libraries, and the DejaVu fonts for subtitles). For sound through a sound
service install PipeWire or PulseAudio from the Store's Dependencies as well; without one
the player uses the sound card directly.

It appears in the launcher as **Media Player**. GPU decoding needs `gpu_mem=128` in
`config.txt` on the boot partition (that is what it was tried with; smaller values are
untested).

**NimbleZero v0.1.0** (version 0.1.1: no GPU decoding, no subtitles). On the device
(Terminal app or SSH), with Wi-Fi connected:

    wget https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-player_0.1.1_arm64.deb
    sudo dpkg -i nz-player_0.1.1_arm64.deb

Remove with `sudo dpkg -r nz-player`.

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
| | T | next sound track (shows its language) |
| | S | next subtitle track, then off |
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
| GPU decoding | **Automatic**, Off | H.264 up to 1920x1080 (8 bit) is decoded and shrunk by the GPU instead of the processor; see "GPU decoding" below. Takes effect with the next video. |
| Scaling filter | Fast, Bilinear, Bicubic, **Lanczos** | Software decoding only (the GPU has its own scaler). How the picture is shrunk to 320x170. All but "Fast" average over the source pixels that fall on one screen pixel, so fine detail and text do not shimmer; Bicubic and Lanczos are a little sharper and cost a little more. `screenshots/scaling-filters.png` shows the four on a 1080p test picture (top left: Fast). |
| Picture shape | **Fit**, Fill, Stretch | Fit shows the whole picture with black bars; Fill crops the edges; Stretch distorts. Non-square pixels (DVB, DVD) are taken into account. |
| Decoding | **Automatic**, Full quality, Fast, Fastest | Short cuts for when the processor cannot keep up. Automatic leaves out the deblocking filter for video higher than 360 lines (not visible at this screen size), goes from Lanczos to bilinear scaling, then leaves out frames no other frame depends on; short cuts are taken back when the processor keeps up again. If the picture still falls more than 1.5 s behind, nothing is decoded until the next key frame ahead of the sound, and playback continues from there in step. |
| Seek step | 5 - 120 s (10 s) | Per key press. |
| Resume movies | On / Off | Continue a file where it was stopped. |
| Sound timing | -1000 ... +1000 ms | Moves the sound against the picture, if lips and voice do not match. |
| Volume | 0 - 100 % | Of this player only. The system volume is in the launcher's Options. |
| Technical overlay | On / Off | Same as key I. |
| Subtitles at start | On / **Off** | With On, a subtitle file next to the video is shown from the start, else the first track inside it. Key S works either way. |
| Subtitle size | Small, **Normal**, Large | 14, 18 or 23 pixels high on the 170-line screen, for text subtitles of every format. |

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

## GPU decoding (0.2.0)

The Cardputer Zero's chip decodes H.264 (and nothing newer: HEVC stays software) in
hardware, and the same firmware can scale and convert pictures. `src/hwdec.c` drives both
through the kernel driver `bcm2835-codec`: the decoder `/dev/video10` and the scaler
`/dev/video12`. A decoded picture is handed to the scaler by reference; only its
screen-sized result is read by the processor, which converts it to the screen's RGB565
with dithering (the scaler's own RGB565 shows bands in smooth areas).

Measured on a Cardputer Zero with two test programs (in the project's working folder,
not in this repository), 1080p H.264 at 24 pictures a second, 2026-11-21:

| | Processor time at normal speed |
|---|---|
| FFmpeg software decoder (decoding only) | about 175 % of one core |
| GPU decoder, each full picture copied to the processor | 57 % |
| GPU decoder + GPU scaler, small result copied | 9 % |

The first 48 pictures from the GPU were identical to the software decoder's. The scaler's
picture of a 1080p frame is closest to an area-averaging filter (it is filtered, not
point-sampled).

What it needs:

- The kernel module `bcm2835-codec` (NimbleZero v0.1.0 and the development images load it).
- GPU memory: tested with `gpu_mem=128` in `config.txt`. NimbleZero ships `gpu_mem=32`;
  whether that is enough has not been established.
- About 20 MB of the kernel's 64 MB contiguous-memory pool while a 1080p video plays.

FFmpeg's own wrapper for this kind of decoder (`h264_v4l2m2m`) is not used: on this system
it asks for 20 picture buffers (more than the pool holds) and then hangs.

If the GPU decoder cannot be opened or stops answering, the player says so in its log
(`gpu: ...`) and goes on with the software decoder. "Scaling filter" and "Decoding" in the
options apply to the software decoder only.

## Changes from 0.2.0 to 0.3.8

- **Colour and banding.** The GPU scaler delivers full-colour RGB; the player turns it into
  the screen's 16-bit colour with ordered dithering. The scaler's buffer is uncached
  memory: it is copied once before it is touched (reading it byte by byte cost 19 ms a
  picture, the copy costs 1 to 2 ms).
- **1080p at 60 pictures a second** is the limit of the GPU decoder (rated for 1080p30).
  Measured on one live channel: about 3 % short. The player shows every decoded picture;
  when the picture falls behind the sound it leaves out single frames that no other frame
  depends on before decoding (every 0.5 s at 0.04 s behind, every 0.1 s at 0.12 s), and
  all such frames for at least half a second from 0.2 s behind.
- Nothing is sent to the GPU decoder before the first key frame of a stream.
- The next picture goes to the scaler before the previous one is converted.
- **Picture widths the scaler does not take from the decoder** (720, 1440, 848 and others:
  the driver gives the decoder's lines a multiple of 32 bytes and wants a multiple of 64
  for the scaler's input): such pictures are copied into input buffers of the scaler's
  own. Measured for 720x480: 4.1 ms a picture. 1440x1080 is not measured.
- **Scaler buffer sizes.** For an RGB result whose width is not a multiple of 32 the
  driver's buffer was smaller than the firmware demands and the scaler did not start
  (0.2.7 to 0.3.1); the player now asks for the firmware's line length. If a scaler
  set-up is refused, two others are tried (RGB with the colour space left to the driver,
  then YU12).
- **A message on the screen** when the GPU decoder gives up and the software decoder
  takes over ("GPU decoder failed: software"). It used to happen silently.
- Overlay: data rate of the picture stream in the first line.
- **Subtitles.** `S` steps through the subtitle tracks and off. Text formats (SRT,
  ASS/SSA, WebVTT, MP4 text and what else FFmpeg decodes) are drawn by libass with the
  DejaVu Sans font; files with the video's name next to it (`film.srt`, `film.en.srt`;
  .srt .ass .ssa .vtt .smi .sub) are offered after the tracks inside the file, and files
  that are not UTF-8 are read as Windows-1252. Plain text formats use a style of the
  player's own (white, black outline); ASS keeps its styles, scaled so that the file's
  main style has the chosen size. Picture formats (DVB, Blu-ray PGS, DVD) are shrunk with
  the video. libass is loaded at run time; without it or the font, text subtitles are not
  offered. Teletext subtitles are not supported (FFmpeg is built without libzvbi).
  Limitation: after a seek, or after choosing a track inside the file, a subtitle line that
  began before that point is not shown; the next one is.
- **Software decoder that cannot keep up** (HEVC above all; there is no GPU decoder for
  it): see "Decoding" under Options. Before 0.3.8 short cuts only ever increased, ending
  in "key frames only" for good.
- **HEVC above 720 lines** starts with frames nothing depends on left out and bilinear
  scaling. All four cores are in use (FFmpeg frame threads).
- **Defaults**: scaling filter Lanczos. Saved settings are not changed by an update: an
  existing `~/.config/nz-player/config` keeps its values.
- The source builds against FFmpeg 6.1.5 and 7.1.5; a package works only with the set it
  was built for.

## How it works (short)

- `src/player.c` - the engine: a reader thread (demuxing, seeking), a sound thread whose
  progress is the clock, a picture thread (decoding, scaling with libswscale to RGB565),
  and `pl_refresh()` for the user interface to fetch the picture that is due.
- `src/hwdec.c` - H.264 decoding and scaling on the GPU (see above); `src/yuv565.c` - the
  small picture to RGB565 with dithering.
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

**Package for the next version's Store** (set `0.2.0~dev1`, 2026-11-20; emulation only):
built against that set's libraries the program is byte for byte the one tested on the
device. Checked: the Store's own dependency planning finds the 13 packages to download
first; they and the player install in that order with the development image's `dpkg`;
the image's launcher lists the player; `tests/run-tests.sh` passes on the result. Not
checked: the Store's screens with this entry, the package layer on the data partition, and
sound through PipeWire or without any sound service - none of it has run on a device.

**GPU decoding on hardware** (owner's report, 2026-11-21, version 0.2.0, `gpu_mem=128`):
local files played fine. IPTV channels were mixed: several fine, also 1080p at 60 pictures
a second; several showed a black screen until restarted, or dropped to "decoded 60, shown
6" after a while; and smooth areas showed bands, most of all in animation.

**Test builds 0.2.1 to 0.3.8 on hardware** (owner's reports, 2026-11-21 to -25, one
Cardputer Zero, kernel 6.18.60, `gpu_mem=128`; from 0.3.0 on with FFmpeg 7.1.5):

- 0.2.1: banding gone; 0.2.6: late pictures shown, short catch-ups - "a big improvement";
  0.2.7: "only dropping a few frames now" on a 1080p channel at 59.94 pictures a second.
  Whether 0.2.7 was on the GPU in that test is not known: it had the scaler buffer fault
  described above and fell back to software without a sign.
- 0.3.2 (log): 1080p live, 302x170 RGB, 3547 pictures from the GPU, none left out.
- 0.3.5 (log): 720x480 file on the GPU with copied pictures, 718 shown, none left out,
  "the picture looked fine"; 1080p live 320x180, 1300 shown, none left out.
- 0.3.7: text subtitles "looked good at the normal size"; ASS, too large before, "looks
  better".
- 0.3.8: HEVC "much better" (before: 720p stuck at 15 pictures a second after a few
  minutes, 1080p at one a second and then out of step). No log of 0.3.8 was sent.
- Sound went to ALSA directly in these tests (no sound service installed).

Emulation (2026-11-25): the program built against package set `0.2` is byte for byte the
0.3.8 that ran on the device; `tests/run-tests.sh` (11 checks, among them subtitles drawn
with the set's own libass and font) passes on it.

**Not reported from a device yet:** picture subtitles (DVB, PGS, DVD) and 1440x1080 video;
installing 0.3.8 through the Store; GPU decoding with less than `gpu_mem=128`; resume and
track change one by one; which sound output is in use ("out:" in the overlay); a USB keyboard plugged
in while the player runs; HTTPS streams; Wi-Fi drop-outs; long runs; battery use; the
screen-off timer while paused; 1080p at 59.94 frames a second with 0.1.1.

## Building

Desktop simulator (Debian/Ubuntu: `libsdl2-dev libavformat-dev libavcodec-dev
libswscale-dev libswresample-dev libasound2-dev`):

    make sim && ./nz-player-sim            # 3x window, PC keyboard
    make check                             # also builds nz-player-fake (stand-in GPU decoder)

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

`nz-mkdeb` here is the SDK's script with two changes: the package no longer records mode
0700 for `/`, and it writes the fields the next version's Store shows (`NZ-Title`,
`--section`, `--description`).

Test hooks, any build: `NZP_HEADLESS=1` (no screen or keyboard),
`NZP_KEYS="down,enter,wait:2000,shot:/tmp/a.ppm,esc"` (scripted keys, pauses,
screenshots), `NZP_AUDIO=pulse|alsa|none`, `NZP_CONFIG_DIR`, `NZP_DEBUG=1`.

## Not in this version

Hardware decoding of HEVC (the chip decodes H.264 and MPEG-4 only), teletext subtitles,
a way to type in a stream address on the device, favourites, programme guide (EPG),
playlists that repeat or shuffle.
