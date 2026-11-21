# NimbleZero-apps: package files

This branch holds the apps that the NimbleZero Store offers under "NimbleZero apps": one
directory per package set, each with a `Packages` index and `.deb` files. A package set
belongs to one version of the system.

| Directory | For |
|---|---|
| `0.1.0/` | NimbleZero v0.1.0. Its Store does not read this directory: download the `.deb` and install it with `sudo dpkg -i` |
| `0.2.0~dev1/` | development images of the next version (test builds, not a release) |

Media Player (`nz-player`): GPL-2.0, source and instructions in `nz-player/` on the `main`
branch of this repository. It links to the FFmpeg and ALSA libraries of the system and
contains none of them.

Snake: GPL-2.0, source `sdk/examples/snake.c` in https://github.com/djkaos81/NimbleZero.
How the set was built: the `packages` branch of NimbleZero-dependencies, `source-info/`.

Unofficial software for the M5Stack Cardputer Zero; not an M5Stack product. No warranty.
