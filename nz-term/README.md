# NimbleTerm (nz-term)

Terminal for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the M5Stack
Cardputer Zero. Licence: GPL-2.0, like NimbleZero.

Version 0.1.0, 2026-11-21. **It has not run on a device yet** - see "What has been tested".

What it adds to the launcher's "Terminal" (the Linux text console):

- lines that scrolled off the top can be read again (the last 1500),
- three text sizes: 40, 53 or 80 columns,
- battery and clock in a title bar (it can be switched off for two more rows),
- paste of what was copied in Text Editor,
- 256 colours.

## Install

    sudo dpkg -i nz-term_0.1.0_arm64.deb

It appears in the launcher as **NimbleTerm**; the system's "Terminal" entry stays.
NimbleCommander 0.2.0 opens it in the folder of the pane with T. Remove with
`sudo dpkg -r nz-term`. The package needs nothing but the C library.

    nz-term                       a login shell, in the home folder
    nz-term -d FOLDER             ... in FOLDER
    nz-term -e PROGRAM [ARGS]     PROGRAM instead of a shell; the terminal closes with it

## Using it

Every key goes to the program in the terminal. The terminal's own functions are in a menu:

| Key | Does |
|---|---|
| **Fn+H**, or Ctrl+Alt+H | menu: scroll back, paste, text size, title bar, keys, close |
| Shift+Fn+L / Shift+Fn+M | scroll back / forward one page |
| while scrolled back | F / X one line, Z / C one page; any other key returns to the end |

| Text size | Columns x rows (with title bar / without) |
|---|---|
| Large | 40 x 19 / 21 |
| Medium (as the text console) | 53 x 19 / 21 |
| Small | 80 x 26 / 28 |

Keys of the Cardputer in a terminal: Fn + F X Z C are the arrow keys, Fn + K N L M are
Home, End, PgUp, PgDn, Fn + 1...0 are F1...F10, Fn+Backspace is Delete, the key at the top
left is `` ` `` and with Fn it is Esc. Ctrl, Alt and Tab are keys of their own.

Closing the terminal from the menu ends what runs in it (after asking). When the shell
ends (`exit`, Ctrl+D), the terminal closes.

Settings are kept in `~/.config/nz-term/config`.

## For programs

`TERM` is `xterm-256color` (or `xterm` if the system has no such entry); `NZ_TERM` is set
to the version. Understood: cursor movement and the scrolling region, insert and delete of
characters and lines, the second screen that full-screen programs use, colours (16, 256
and direct), bold (shown brighter), underline, reverse, line drawing (both the VT100 set
and Unicode), application cursor keys, bracketed paste, the cursor position report.
Not there: the mouse, blinking, italics, double-width characters (Chinese, Japanese,
Korean and emoji are shown as `?`), window titles (read and dropped).
Characters: ASCII, Western accents, and the symbols and few Greek letters of the classic
PC set.

## What has been tested, and what has not

Tested (2026-11-21), without a device:

- `tests/run-tests.sh`, 17 checks with real programs running in the terminal: screen sizes
  as the program sees them, typing, the bytes sent for arrows, function keys, Ctrl and Alt
  combinations, the cursor position report, paste, closing (the program gets a hang-up),
  scrolling back, changing the text size while a program runs, and nano editing and saving
  a file. They pass for the desktop build under AddressSanitizer and
  UndefinedBehaviorSanitizer, and for **the packaged arm64 program** under user-mode
  emulation (qemu-aarch64).
- nano, vim, less, top and `ls --color` were run in it at the medium text size and looked
  at as screenshots; nano also at the small size, `ls` at all three.
- 3M of random bytes and 60000 random escape sequences in each text size: no error under
  the sanitizers. 300000 lines of output: about one second on the build machine.

**Not tested anywhere:** the program on a Cardputer Zero. In particular:

- whether **Fn+H** arrives as the key code the vendor's key table says (KEY_HELP). If it
  does not, Ctrl+Alt+H opens the menu;
- that Ctrl+C, Ctrl+Z and Ctrl+S only reach the program in the terminal (the terminal
  switches the console underneath to raw keys; this code only runs on the device);
- the Sym layer and Caps Lock; how readable 80 columns are on the real panel;
- htop and mc (not installed on the build machine);
- battery use next to the text console.

## Building

    make sim && ./nz-term-sim           # desktop window, needs libsdl2-dev (letters and digits only)
    make check
    make CROSS_COMPILE=aarch64-linux-gnu-
    make deb

Test hooks, any build: `NZT_HEADLESS=1`, `NZT_KEYS="wait:300,text:ls,enter,wait:300,shot:/tmp/a.ppm"`,
`NZT_CONFIG_DIR` (see `nzui/nzui_app.h`). `src/cp437.h` and `nzui/nzui_fonts.h` are
generated (fonts from the Linux kernel's `lib/fonts`: 8x8 and 6x8 GPL-2.0, 4x6 public
domain).

## Not in this version

Copying text out of the terminal, several terminals at once, searching the scrollback,
re-folding of lines when the text size changes (a full-screen program redraws itself;
plain output stays as it was).
