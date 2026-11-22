# NimbleZero-apps

Apps for [NimbleZero](https://github.com/djkaos81/NimbleZero), the Linux system for the
M5Stack Cardputer Zero: NimbleZero's own apps, and apps by others that were written for
NimbleZero.

| App | What it is | Source | Package |
|---|---|---|---|
| **Media Player** (`nz-player`) 0.1.1 | Plays movies and sound files, and IPTV streams from `.m3u` channel lists, with the FFmpeg libraries of the system | [`nz-player/`](nz-player/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-player_0.1.1_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-player_0.1.1_arm64.deb); development images of the next version: in the Store |
| **NimbleCommander** (`nz-commander`) 0.2.0 | File manager with two panes: copy, move, rename, delete; opens files in the apps below | [`nz-commander/`](nz-commander/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-commander_0.2.0_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-commander_0.2.0_arm64.deb); development images of the next version: in the Store |
| **Text Editor** (`nz-edit`) 0.1.0 | Text viewer and editor: find, copy and paste, undo | [`nz-edit/`](nz-edit/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-edit_0.1.0_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-edit_0.1.0_arm64.deb); development images of the next version: in the Store |
| **Image Viewer** (`nz-image`) 0.1.0 | Shows JPEG, PNG, BMP and GIF pictures: zoom, turn, the other pictures of a folder | [`nz-image/`](nz-image/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-image_0.1.0_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-image_0.1.0_arm64.deb); development images of the next version: in the Store |
| **NimbleTerm** (`nz-term`) 0.1.0 | Terminal with scrollback, three text sizes and paste from the editor | [`nz-term/`](nz-term/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-term_0.1.0_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-term_0.1.0_arm64.deb); development images of the next version: in the Store |

Install a package on the device with `sudo dpkg -i <file>`; each app's README has the
details.

NimbleCommander, Text Editor, Image Viewer and NimbleTerm (2026-11-22) are first versions:
they pass their tests on a PC and under emulation, and have not been run on a Cardputer
Zero yet. Each README lists what is and is not tested.

- Branch `main`: source code, one directory per app.
- Branch [`packages`](https://github.com/djkaos81/NimbleZero-apps/tree/packages): the
  built packages, one directory per version of the system.

How to write an app: `docs/APP-DEVELOPMENT.md` in the main repository.

NimbleZero is unofficial firmware: not an M5Stack product, and not endorsed by M5Stack or
Raspberry Pi Ltd. No warranty.
