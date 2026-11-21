# nz-player - movie and IPTV player for NimbleZero
#
#   make sim      desktop build in an SDL window (needs libsdl2-dev, libav*-dev, libasound2-dev)
#   make          cross-compile for the device   -> ./nz-player
#   make deb      package                        -> nz-player_<version>_arm64.deb
#   make check    scripted tests (see tests/run-tests.sh)
#
# Device build.  The NimbleZero SDK of v0.1.0 has the compiler but not the FFmpeg headers,
# so three things are needed (see README.md, "Building"):
#   CROSS_COMPILE  prefix of the cross compiler            (SDK: .../bin/aarch64-linux-)
#   ROOTFS         root file system of the NimbleZero image the package is for (libraries)
#   FFMPEG_SRC     unpacked FFmpeg source of the version in that image (headers only)
NAME    = nz-player
VERSION = 0.1.1
CROSS_COMPILE ?= aarch64-linux-
ROOTFS  ?= /path/to/rootfs
FFMPEG_SRC ?= /path/to/ffmpeg-6.1.5
CFLAGS  ?= -O2 -g0
WARN    = -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -std=gnu11
SRC     = src/main.c src/player.c src/audio.c src/plat.c src/m3u.c src/config.c
HDR     = src/nzp.h nzui/nzui.h
LIBS    = -lavformat -lavcodec -lswscale -lswresample -lavutil -lasound -lpthread -ldl -lm

$(NAME): $(SRC) $(HDR) build/include/libavutil/avconfig.h
	$(CROSS_COMPILE)gcc $(CFLAGS) $(WARN) -Inzui -Ibuild/include -I$(FFMPEG_SRC) -o $@ $(SRC) \
		-L$(ROOTFS)/usr/lib -Wl,-rpath-link=$(ROOTFS)/usr/lib:$(ROOTFS)/lib $(LIBS)
	$(CROSS_COMPILE)strip $@

# the one public FFmpeg header that its configure script generates
build/include/libavutil/avconfig.h:
	mkdir -p build/include/libavutil
	printf '#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n' > $@

sim: $(SRC) $(HDR)
	$(CC) -O1 -g $(WARN) -DNZUI_SDL -Inzui $$(sdl2-config --cflags) $$(pkg-config --cflags libavformat libavcodec libswscale libswresample libavutil alsa) \
		-o $(NAME)-sim $(SRC) $$(sdl2-config --libs) $$(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil alsa) -lpthread -ldl -lm

deb: $(NAME)
	./nz-mkdeb --name $(NAME) --version $(VERSION) --exec $(NAME) --title "Media Player" \
		--comment "Movies and IPTV" --section video \
		--description "Movie and IPTV player: local files and .m3u channel lists" \
		--depends "libc6 (>= 2.38), ffmpeg, libasound2t64 | libasound2"

check: sim
	tests/run-tests.sh ./$(NAME)-sim

clean:
	rm -rf $(NAME) $(NAME)-sim build *.deb

.PHONY: sim deb check clean
