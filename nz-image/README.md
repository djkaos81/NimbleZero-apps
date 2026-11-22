# Image Viewer (nz-image)

Picture viewer for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the M5Stack
Cardputer Zero. Licence: GPL-2.0, like NimbleZero; the decoder `src/stb_image.h` (Sean
Barrett and others, v2.34) is public domain.

Version 0.1.0, 2026-11-21. **It has not run on a device yet** - see "What has been tested".

## Install

    sudo dpkg -i nz-image_0.1.0_arm64.deb

It appears in the launcher as **Image Viewer**. NimbleCommander 0.2.0 opens pictures with
it once it is installed. Remove with `sudo dpkg -r nz-image`. The package needs nothing
but the C library.

    nz-image            choose a picture
    nz-image FILE       show FILE

Shown: JPEG (also progressive), PNG, BMP, GIF (the first picture of an animation),
PPM / PGM. Not shown: WebP, HEIC, TIFF, SVG.

## Using it

| Key | Does |
|---|---|
| Z / C (or F / X) | previous / next picture of the same folder |
| Enter | closer: twice as large each time, stopping at 100 % on the way, up to 800 % |
| Backspace | back out one step; from the whole picture: back |
| F X Z C | move around while closer |
| Esc | the whole picture; pressed again: back |
| T | turn a quarter to the right (not saved in the file) |
| Space | the picture alone on the whole screen / with title and key line |
| I | name, size in pixels, file size, date |
| H | key overview |

- A picture larger than the screen is reduced by averaging all its pixels, and the
  16-bit screen gets a fine dither pattern, so photos keep their detail and skies do not
  show bands. A picture smaller than the screen is shown in its own size.
- Photos from a camera or phone that are stored on their side are turned upright (Exif).
- See-through parts of a PNG are shown on grey squares.
- The title shows the name and "3/12" (which picture of the folder), or the size in per
  cent while closer.
- The largest picture is 32 megapixels (it needs 3 bytes of memory per pixel).

## What has been tested, and what has not

Tested (2026-11-21), without a device:

- `tests/run-tests.sh`, 15 checks that look at the colours of screen pixels: each kind of
  file, centring, the whole screen, Exif turning, T, small pictures, 800 %, see-through
  PNG, a damaged file, the chooser, the saved setting. They pass for the desktop build
  under AddressSanitizer and UndefinedBehaviorSanitizer, and for **the packaged arm64
  program** under user-mode emulation (qemu-aarch64).
- 48 deliberately damaged JPEG, PNG, GIF and BMP files opened one after another, with
  zooming and turning: no error under the sanitizers.
- A 4000x3000 JPEG: looked at as screenshots at several sizes.

**Not tested anywhere:** the program on a Cardputer Zero. In particular how long a large
photo takes: the decoder is plain C without the processor's vector instructions, so a
12-megapixel JPEG may take several seconds ("Loading..." is on screen meanwhile). That
number is needed before deciding whether a faster decoder is worth it.

## Building

    make sim && ./nz-image-sim FILE     # desktop window, needs libsdl2-dev
    make check                          # needs python3 with Pillow
    make CROSS_COMPILE=aarch64-linux-gnu-
    make deb

Test hooks, any build: `NZI_HEADLESS=1`, `NZI_KEYS="enter,enter,shot:/tmp/a.ppm"`,
`NZI_CONFIG_DIR` (see `nzui/nzui_app.h`).

## Not in this version

Slide show, animated GIF, deleting or renaming from the viewer, saving a turned picture,
WebP.
