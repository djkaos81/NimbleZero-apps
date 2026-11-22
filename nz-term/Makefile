# nz-term - NimbleTerm for NimbleZero
#
#   make sim      desktop build in an SDL window (needs libsdl2-dev)  -> ./nz-term-sim
#   make          cross-compile for the device                        -> ./nz-term
#   make deb      package                                             -> nz-term_<version>_arm64.deb
#   make check    scripted tests (see tests/run-tests.sh)
#
# CROSS_COMPILE: prefix of the cross compiler (Debian/Ubuntu: aarch64-linux-gnu-, the
# NimbleZero SDK: .../bin/aarch64-linux-).  The program needs nothing but the C library.
NAME    = nz-term
VERSION = $(shell sed -n 's/^\#define N*Z*C*_*VERSION *"\(.*\)"/\1/p' src/*.[ch] | head -1)
CROSS_COMPILE ?= aarch64-linux-gnu-
CFLAGS  ?= -O2 -g0
WARN    = -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -std=gnu11
SRC     = src/main.c
HDR     = $(wildcard src/*.h nzui/*.h)
LIBS    = 

$(NAME): $(SRC) $(HDR)
	$(CROSS_COMPILE)gcc $(CFLAGS) $(WARN) -Inzui -Isrc -o $@ $(SRC) $(LIBS)
	$(CROSS_COMPILE)strip $@

sim: $(SRC) $(HDR)
	$(CC) -O1 -g $(WARN) -fsanitize=address,undefined -DNZUI_SDL -Inzui -Isrc $$(sdl2-config --cflags) -o $(NAME)-sim $(SRC) $$(sdl2-config --libs) $(LIBS)

deb: $(NAME)
	NZ_MAINTAINER="NimbleZero <https://github.com/djkaos81/NimbleZero>" ./nz-mkdeb --name $(NAME) --version $(VERSION) --exec $(NAME) \
		--title "NimbleTerm" --comment "Terminal: scrollback, three text sizes" --section utils \
		--description "Terminal with scrollback, three text sizes and paste"

check: sim
	ASAN_OPTIONS=detect_leaks=0 tests/run-tests.sh $$PWD/$(NAME)-sim

clean:
	rm -f $(NAME) $(NAME)-sim *.deb

.PHONY: sim deb check clean
