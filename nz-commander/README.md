# NimbleCommander (nz-commander)

File manager for [NimbleZero](https://github.com/djkaos81/NimbleZero) on the M5Stack
Cardputer Zero: two folders ("panes") side by side or one at a time, copy, move, rename,
delete, new folder, and the way into the other NimbleZero apps: text files open in Text
Editor, pictures in Image Viewer, movies in Media Player, and T opens a terminal in the
folder you are looking at. Licence: GPL-2.0, like NimbleZero.

Version 0.2.0, 2026-11-21. **It has not run on a device yet** - see "What has been tested".

New in 0.2.0: Text Editor (`nz-edit`), Image Viewer (`nz-image`) and NimbleTerm (`nz-term`)
are used when they are installed; T; "Terminal in this folder" and "Show the picture" in
the 2 menu.

## Install

On the device (Terminal app or SSH), after copying the package there
(`scp nz-commander_0.2.0_arm64.deb zero@<device>:`):

    sudo dpkg -i nz-commander_0.2.0_arm64.deb

It appears in the launcher as **NimbleCommander** (press R there if the list is not
refreshed). Remove with `sudo dpkg -r nz-commander`. The package needs nothing but the C
library, so the same file suits NimbleZero v0.1.0 and the development images.

### With the other apps

Each is a package of its own; NimbleCommander looks for them when a key is pressed, so they
can be installed and removed at any time, in any order.

| | With the app installed | Without it |
|---|---|---|
| 3 (view), Enter on a file | **Text Editor** in reading mode: find, go to line, E to edit; a file that is not text as bytes | the small viewer built into NimbleCommander |
| 4 (edit) | **Text Editor** | nano on the text console |
| Enter or 3 on a picture | **Image Viewer**; Z / C there show the other pictures of the folder | the built-in viewer shows the bytes |
| Enter on a movie or sound file | **Media Player** | the built-in viewer |
| T | **NimbleTerm** in the folder of the pane | a shell on the text console, in that folder |

Leaving the app brings NimbleCommander back where it was, with both folders read again.

## Using it

| Key | Does |
|---|---|
| F / X (or Up / Down) | move |
| Z / C (or Left / Right) | page up / down |
| Tab | the other pane |
| Enter | open: a folder; a picture, movie or sound file in its app (see above); a `.deb` package (asks, then installs); anything else in the viewer |
| Backspace | folder above (also: Enter on `/..`) |
| Space | mark / unmark, and one down |
| A | mark all / none |
| H (or 1) | key overview |
| 2 | more actions for the selected file, and the view settings |
| 3 | view |
| 4 | edit |
| T | terminal in the folder of this pane |
| 5 | copy to the other pane |
| 6 | move to the other pane; for one file: R at the question renames instead |
| 7 | new folder |
| 8 | delete |
| O | options |
| R | read both folders again |
| Esc twice, or Q | leave |

- 5, 6 and 8 apply to the marked entries, or to the selected one if nothing is marked.
  Each asks first. Marked entries are green with a `*`.
- The bottom line above the keys shows the selected entry's full name, size and date, or
  the number and size of the marked entries and the free space on the other side.
- If both panes show the same folder, 5 asks for a name for the copy and 6 renames.
- A file that is already there: Enter replaces it, S skips it, A replaces this and all
  further ones, Esc stops.
- During a long copy, move or delete, Esc stops; what was done so far stays done, and a
  half-written file is removed.
- Folders are copied with everything in them. Links are copied as links. Copies keep
  their permissions and date. Devices, pipes and sockets are left out.
- With **one pane** on screen (Options, or 2), Tab shows the other one; its folder is
  named at the right of the path line, because that is where copies go.

Options: panes on screen (one / two), sort by (name / size / date, folders first), hidden
files, sizes with two panes. Settings and the two folders are kept in
`~/.config/nz-commander/config`.

`nz-commander [LEFT-FOLDER [RIGHT-FOLDER]]` starts in the given folders.

## What has been tested, and what has not

Tested (2026-11-21), without a device:

- `tests/run-tests.sh`, 27 checks: copy (file, folder, link, marked set), dates kept, move
  within and across file systems, rename, new folder, delete, the "already exists"
  answers, a folder into itself, same folder on both sides, Esc during a copy, an
  unreadable folder, settings saved and restored, and which app would be started for 3,
  4, Enter and T with and without the other apps installed. They pass for the desktop build under
  AddressSanitizer and UndefinedBehaviorSanitizer, and for **the packaged arm64 program**
  under user-mode emulation (qemu-aarch64) with a generic glibc 2.39, in both cases
  without screen or keyboard (scripted keys, screenshots).
- The screens were looked at as screenshots from those runs.

**Not tested anywhere:** the program on a Cardputer Zero (screen, keyboard, the Sym layer
in the text fields); **handing the screen to another app and getting it back** (Text
Editor, Image Viewer, NimbleTerm, Media Player, nano, `sudo dpkg -i`) - that code only runs
on the device, and it is what 0.2.0 is about; large copies on the SD card (speed, time); the screen-off timer; file names
that are not ASCII (they are shown with `?`, and can be copied, but not typed).

## Building

    make sim && ./nz-commander-sim          # desktop window, needs libsdl2-dev
    make check
    make CROSS_COMPILE=aarch64-linux-gnu-   # or the NimbleZero SDK: .../bin/aarch64-linux-
    make deb

Test hooks, any build: `NZC_HEADLESS=1`, `NZC_KEYS="down,space,5,enter,shot:/tmp/a.ppm"`,
`NZC_CONFIG_DIR`, `NZC_PROGRESS_NOW=1`, `NZC_APPS_DIR` (where the other apps are looked
for, instead of `/opt`).

`nzui/nzui_kit.h` is the shared look of NimbleZero apps (see `docs/STYLE.md` in the main
repository); this is the first app built on it.

## Not in this version

Archives (zip, tar), search, a quick jump by typing a name, changing permissions, the
size of a folder in the list (2 > Properties counts it), copying in the background,
network places.
