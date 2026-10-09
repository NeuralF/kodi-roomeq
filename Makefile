# kodi-roomeq build
#   make            native build (build/native/)
#   make cross      armhf + aarch64 builds for LibreELEC/Raspberry Pi via zig cc (build/<arch>/)
#   make test       native build + simulated-room test (needs python3, numpy, scipy)
#   make dist       release tarball with prebuilt binaries for all arches
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
ZIG     ?= python3 -m ziglang
ZFLAGS  := -Os -s -w -ffunction-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wl,--gc-sections
VERSION := 1.0.0

.PHONY: all native cross test dist clean
all: native

native: build/native/roomeq-tool build/native/roomeq.so
build/native/roomeq-tool: src/roomeq_tool.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< -lm
build/native/roomeq.so: src/roomeq_ladspa.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -lm

# glibc 2.28 baseline: runs on LibreELEC 10+ and Raspberry Pi OS buster+
cross: build/armhf/roomeq-tool build/armhf/roomeq.so build/aarch64/roomeq-tool build/aarch64/roomeq.so
build/armhf/%: ZT = arm-linux-gnueabihf.2.28 -mcpu=cortex_a7
build/aarch64/%: ZT = aarch64-linux-gnu.2.28
build/armhf/roomeq-tool build/aarch64/roomeq-tool: src/roomeq_tool.c
	@mkdir -p $(@D)
	$(ZIG) cc -target $(ZT) $(ZFLAGS) -o $@ $< -lm
build/armhf/roomeq.so build/aarch64/roomeq.so: src/roomeq_ladspa.c
	@mkdir -p $(@D)
	$(ZIG) cc -target $(ZT) $(ZFLAGS) -shared -fPIC -o $@ $< -lm

test: native
	python3 tests/simulate.py build/native/roomeq-tool

dist: cross
	rm -rf build/dist && mkdir -p build/dist/kodi-roomeq-$(VERSION)/bin
	cp -r build/armhf build/aarch64 build/dist/kodi-roomeq-$(VERSION)/bin/
	cp -r scripts kodi README.md README.ru.md LICENSE docs build/dist/kodi-roomeq-$(VERSION)/
	tar -C build/dist -czf build/kodi-roomeq-$(VERSION).tar.gz kodi-roomeq-$(VERSION)
	@echo build/kodi-roomeq-$(VERSION).tar.gz

clean:
	rm -rf build
