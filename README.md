# NimbleZero-apps

Apps for [NimbleZero](https://github.com/djkaos81/NimbleZero), the Linux system for the
M5Stack Cardputer Zero: NimbleZero's own apps, and apps by others that were written for
NimbleZero.

| App | What it is | Source | Package |
|---|---|---|---|
| **Media Player** (`nz-player`) 0.1.1 | Plays movies and sound files, and IPTV streams from `.m3u` channel lists, with the FFmpeg libraries of the system | [`nz-player/`](nz-player/) (GPL-2.0) | for NimbleZero v0.1.0: [`nz-player_0.1.1_arm64.deb`](https://github.com/djkaos81/NimbleZero-apps/raw/packages/0.1.0/nz-player_0.1.1_arm64.deb); development images of the next version: in the Store |

Install a package on the device with `sudo dpkg -i <file>`; each app's README has the
details.

- Branch `main`: source code, one directory per app.
- Branch [`packages`](https://github.com/djkaos81/NimbleZero-apps/tree/packages): the
  built packages, one directory per version of the system.

How to write an app: `docs/APP-DEVELOPMENT.md` in the main repository.

NimbleZero is unofficial firmware: not an M5Stack product, and not endorsed by M5Stack or
Raspberry Pi Ltd. No warranty.
