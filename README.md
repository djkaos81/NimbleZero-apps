# NimbleZero-apps: package files

This branch holds the apps that the NimbleZero Store offers under "NimbleZero apps": one
directory per package set, each with a `Packages` index and `.deb` files. The Store of a
system reads the set named in its `/etc/nz-release`.

| Directory | For |
|---|---|
| `0.1.0/` | NimbleZero v0.1.0. Its Store does not read this directory: download the `.deb` and install it with `sudo dpkg -i`. Media Player 0.1.1 and, since 2026-11-22, the four apps named below |
| `0.2.0~dev1/` | development image 1 of the next version (test builds, not a release) |
| `0.2.0~dev2/` | development image 2 (FFmpeg 7.1). Media Player here is the same source, version 0.1.1, built against FFmpeg 7.1; the files of the other sets do not run on this image and this one not on theirs |
| `0.2.0~dev3/` | development image 5: the same apps as `0.2.0~dev2/` (Media Player 0.1.1 built against FFmpeg 7.1, Snake). Added 2026-11-22: NimbleCommander 0.2.0, Text Editor 0.1.0, Image Viewer 0.1.0, NimbleTerm 0.1.0 |
| `0.2/` | development image 10 and later. Every package has its own version from here on, so a new app version is simply added here. Contents on 2026-11-22: the apps of `0.2.0~dev3/` (same files), Snake 0.2.1. Since 2026-11-25: Media Player 0.3.8 (GPU decoding of H.264, subtitles), built against this set's FFmpeg 7.1.5; it also needs the DejaVu fonts package, which the Store downloads with it |

Media Player (`nz-player`): GPL-2.0, source and instructions in `nz-player/` on the `main`
branch of this repository. Version 0.1.1 is in the sets up to `0.2.0~dev3/` (one build for FFmpeg 6.1, one for 7.1), version 0.3.8 in `0.2/`. It links to the FFmpeg and ALSA libraries of the system and
contains none of them.

NimbleCommander (`nz-commander`), Text Editor (`nz-edit`), Image Viewer (`nz-image`) and
NimbleTerm (`nz-term`): GPL-2.0, source and instructions in the directories of those names on
the `main` branch. They need nothing but the C library, so the same four files are in
`0.1.0/` and `0.2.0~dev3/`. First versions: tested on a PC and under emulation, not yet on
a Cardputer Zero.

Snake: GPL-2.0, source `sdk/examples/snake.c` in https://github.com/djkaos81/NimbleZero.
How the set was built: the `packages` branch of NimbleZero-dependencies, `source-info/`.

Unofficial software for the M5Stack Cardputer Zero; not an M5Stack product. No warranty.
