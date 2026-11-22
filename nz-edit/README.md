# Text Editor (nz-edit)

Text viewer and editor for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the
M5Stack Cardputer Zero. Licence: GPL-2.0, like NimbleZero.

Version 0.1.0, 2026-11-21. **It has not run on a device yet** - see "What has been tested".

## Install

    sudo dpkg -i nz-edit_0.1.0_arm64.deb

It appears in the launcher as **Text Editor**. NimbleCommander 0.2.0 uses it for View (3)
and Edit (4) once it is installed. Remove with `sudo dpkg -r nz-edit`. The package needs
nothing but the C library.

    nz-edit                 choose a file (N: a new one), edit it
    nz-edit FILE            edit FILE; it is made on saving if it does not exist
    nz-edit --view FILE     read FILE
    nz-edit +LINE FILE      start at that line

## Reading (`--view`, or 3 in NimbleCommander)

| Key | Does |
|---|---|
| F / X | one row up / down |
| Z / C | one page up / down |
| T / B | top / bottom of the file |
| S, N | find, find next (capitals and small letters count as the same) |
| G | go to a line |
| E (or 4) | edit this file |
| A / D | sideways, when long lines are not folded |
| O | options |
| H | key overview |
| Esc, Backspace or Q | back |

A file that is not text is shown as bytes in hexadecimal. Of a file larger than 8M the
first 8M are shown, and it cannot be edited.

## Editing

Every letter is text here, so moving needs the real arrow keys (Fn + F X Z C) and the
commands are Ctrl+letter. **Esc opens a menu with all of them.**

| Key | Does |
|---|---|
| Fn+K / Fn+N | start / end of the line (Home / End) |
| Fn+L / Fn+M | page up / down |
| Fn+Backspace | delete to the right |
| Ctrl+S | save |
| Ctrl+Q | leave; asks if there are changes (Enter: save, D: don't) |
| Ctrl+F, Ctrl+G | find, find next |
| Ctrl+L | go to a line |
| Ctrl+B | start a selection: move, then Ctrl+C / Ctrl+X. Ctrl+B again drops it |
| Ctrl+A | select everything |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste. Without a selection: the whole line |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+O | options |
| Ctrl+H | key overview |

- The title shows the file name (with `*` while there are unsaved changes) and
  line:column.
- Copied text is also written to `$XDG_RUNTIME_DIR/nz-clipboard`; NimbleTerm can paste it.
- Saving writes a new file and puts it in place of the old one, so a power cut in the
  middle leaves the old file whole. Rights and owner are kept; a link stays a link.
- Kept as they are: DOS line ends, a byte order mark, and whether the last line ends with
  a line feed. Characters the 8x8 font does not have (anything but ASCII and Western
  accents) are shown as `?` but stay in the file unchanged.
- If the system tells the editor to stop while there are changes (power switch, the
  launcher's Ctrl+Alt+Backspace), they are written to `FILE.unsaved` next to the file; the
  file itself is not touched.

Options: long lines (fold / scroll sideways), text size (normal: 39 columns, small: 52),
tab width (2 / 4 / 8), keep the indent on a new line. Kept in `~/.config/nz-edit/config`.

## What has been tested, and what has not

Tested (2026-11-21), without a device:

- `tests/run-tests.sh`, 33 checks: typing, saving, leaving with and without saving, line
  ends and byte order mark kept, joining lines, undo and redo, cut / copy / paste, the
  selection, find, go to line, indent, folded long lines, the menu, a new file, rights and
  links kept, view mode, a file that is not text, options saved, changes kept when told to
  stop. They pass for the desktop build under AddressSanitizer and
  UndefinedBehaviorSanitizer, and for **the packaged arm64 program** under user-mode
  emulation (qemu-aarch64), in both cases without screen or keyboard (scripted keys,
  screenshots).
- 300K of random bytes as a text file with a script of edits: no error under the sanitizers.
- The screens were looked at as screenshots from those runs.

**Not tested anywhere:** the program on a Cardputer Zero. In particular: whether Ctrl+S,
Ctrl+Q, Ctrl+C and Ctrl+Z reach the editor (it switches the console to raw keys for that;
this code only runs on the device), the Fn and Sym layers while typing, Caps Lock, speed
with large files, being started from NimbleCommander and handing the screen back.

## Building

    make sim && ./nz-edit-sim FILE      # desktop window, needs libsdl2-dev (letters and digits only)
    make check
    make CROSS_COMPILE=aarch64-linux-gnu-    # or the NimbleZero SDK: .../bin/aarch64-linux-
    make deb

Test hooks, any build: `NZE_HEADLESS=1`, `NZE_KEYS="end,enter,text:hello,C-s,shot:/tmp/a.ppm"`,
`NZE_CONFIG_DIR` (see `nzui/nzui_app.h`).

## Not in this version

Replace, several files at once, colours for program code, word-wise folding (lines fold
at the column, also inside a word), typing characters that are not on the keyboard.
