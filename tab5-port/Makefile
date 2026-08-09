# tab5-port build & test runner.
# Isolated PlatformIO core dir avoids cache thrash with concurrent sessions.
CORE  := $(CURDIR)/.pio-core
PIO   := PLATFORMIO_CORE_DIR=$(CORE) pio
EMU   := ./tools/emulate.sh
CXX   := g++
CXXFLAGS := -std=c++11 -O2 -fsanitize=address,undefined -Wall -Wextra -Idivlink

.PHONY: test test-native emu smoke host companion clean core

## Run everything that can be validated headlessly.
test: test-native emu

## Host-native unit tests (protocol + end-to-end host<->companion).
test-native:
	$(CXX) $(CXXFLAGS) test/test_divlink_native.cpp divlink/divlink.cpp -o /tmp/divlink_test && /tmp/divlink_test
	$(CXX) $(CXXFLAGS) test/test_linkserver_native.cpp divlink/divlink.cpp divlink/link_server.cpp -o /tmp/linkserver_test && /tmp/linkserver_test

## Build the P4 smoke image and assert it passes under esp-emu.
smoke:
	cd tab5-host-smoke && $(PIO) run -e tab5-smoke
emu: smoke
	$(EMU) tab5-host-smoke/.pio/build/tab5-smoke "SMOKE_RESULT:PASS"

## Compile the full host firmware (M5Unified). Real target is esp32p4_es
## (Josh's Tab5); note it boot-loops under esp-emu — see the smoke note.
host:
	cd tab5-host && $(PIO) run -e tab5-host

## End-to-end emulated link handshake: LinkClient (r3) <-> companion_sim over UART1.
link-emu:
	cd link-emu && $(PIO) run -e link-emu
emu-link: link-emu
	./tools/emulate_link.sh

## One-time: hardlink-snapshot ~/.platformio into an isolated core dir.
core:
	[ -d $(CORE) ] || cp -al $(HOME)/.platformio $(CORE)

clean:
	rm -rf tab5-host/.pio tab5-host-smoke/.pio /tmp/divlink_test /tmp/linkserver_test
