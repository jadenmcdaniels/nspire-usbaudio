# nspire-usbaudio
#   make            build/usbaudio.tns + build/musicplayer.tns + build/music.wav.tns (test WAV)
#   make test       PC tests natively with ASan+UBSan (alignment included), the
#                   pure tests again as ARMv5 code under qemu-arm, then `check`
#   make check      calculator objects: no ARMv6+ instruction, no floating point,
#                   no Ndless syscall reachable from the OS's USB callbacks
#   make clean
# Needs the Ndless toolchain on PATH.

SHIM     ?= ../nspire-usbshim
QEMU     ?= qemu-arm
HOSTCC   ?= gcc
ARMCC    ?= arm-none-eabi-gcc
NSPIRECC ?= nspire-gcc
GENZEHN  ?= genzehn
B        ?= build
# UA_DEFS: the queue's shape for this build (ua18): e.g. gpSP's
# "-DUA_NF=14 -DUA_NX=4 -DUA_START_ONE"; `make test-gpsp` runs every test with it in B=build-gpsp.
UA_DEFS  ?=

LIB_SRCS  := src/uac.c src/pcm.c src/wav.c src/melody.c src/player.c src/app.c src/ua_os.c src/music.c src/library.c src/screen.c
PURE_SRCS := src/uac.c src/pcm.c src/wav.c src/melody.c
HDRS      := $(wildcard src/*.h tests/*.h tests/fake/*.h) $(SHIM)/usbshim_usb.h
WARN      := -std=c99 -Wall -Wextra -Werror
UA_BUILD  := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)$(shell git diff --quiet HEAD -- src 2>/dev/null || echo +)

CF_asan   := $(WARN) -O1 -g -fsanitize=address,undefined,alignment -fno-sanitize-recover=all \
             -fno-omit-frame-pointer -Isrc -Itests -I$(SHIM) -DUA_HOST_TEST $(UA_DEFS)
CF_qemu   := $(WARN) -O2 -march=armv5te -mtune=arm926ej-s -marm -Isrc -Itests -specs=rdimon.specs $(UA_DEFS)
CF_nspire := $(WARN) -O2 -marm -ffunction-sections -fdata-sections -Isrc -I$(SHIM) -DUA_BUILD='"$(UA_BUILD)"' $(UA_DEFS)

TESTS      := uac_test pcm_test wav_test fuzz_test app_test main_test music_main_test library_test
QEMU_TESTS := uac_test pcm_test wav_test ti_abi_test
# The driver of the 2026-11-14 hardware run, from git, against today's
# simulator (tests/legacy_test.c). Skipped where that commit is not there.
LEGACY     := 3b467c4
ifeq ($(shell git cat-file -e $(LEGACY)^{commit} 2>/dev/null && echo yes),yes)
TESTS      += legacy_test
endif

.PHONY: all test test-gpsp check clean
all: $(B)/usbaudio.tns $(B)/musicplayer.tns $(B)/USBcheck.tns $(B)/music.wav.tns

# ---- calculator ----
NOBJS := $(patsubst src/%.c,$(B)/nspire/%.o,$(LIB_SRCS) src/main.c) $(B)/nspire/usbshim.o
$(B)/nspire/%.o: src/%.c $(HDRS) Makefile | $(B)/nspire
	$(NSPIRECC) $(CF_nspire) -c $< -o $@
$(B)/nspire/usbshim.o: $(SHIM)/usbshim.c $(SHIM)/usbshim.h | $(B)/nspire
	$(NSPIRECC) -std=c99 -Wall -Wextra -Werror -O2 -marm -c $< -o $@
$(B)/usbaudio.elf: $(NOBJS)
	$(NSPIRECC) -Wl,--gc-sections $(NOBJS) -lnspireio -o $@
$(B)/usbaudio.tns: $(B)/usbaudio.elf
	$(GENZEHN) --input $< --output $@ --name "usbaudio" --version 1 --author "nspire-usbaudio" \
	  --color-support=true --clickpad-support=false --240x320-support=true --compress \
	  --ndless-min 31 --ndless-rev-min 2001 --uses-lcd-blit false
$(B)/nspire/music_main.o: src/main.c $(HDRS) Makefile | $(B)/nspire
	$(NSPIRECC) $(CF_nspire) -DUA_MUSIC_PLAYER -c $< -o $@
$(B)/musicplayer.elf: $(filter-out $(B)/nspire/main.o,$(NOBJS)) $(B)/nspire/music_main.o
	$(NSPIRECC) -Wl,--gc-sections $^ -lnspireio -o $@
$(B)/musicplayer.tns: $(B)/musicplayer.elf
	$(GENZEHN) --input $< --output $@ --name "Music" --version 1 --author "nspire-usbaudio" \
	  --color-support=true --clickpad-support=false --240x320-support=true --compress \
	  --ndless-min 31 --ndless-rev-min 2004 --uses-lcd-blit true
# USBcheck: the read-only capture of the OS's USB code (tools/usbinspect.c).
$(B)/USBcheck.elf: tools/usbinspect.c Makefile | $(B)/nspire
	$(NSPIRECC) -std=c99 -Wall -Wextra -Werror -O2 -marm $< -o $@
$(B)/USBcheck.tns: $(B)/USBcheck.elf
	$(GENZEHN) --input $< --output $@ --name "USBcheck" --version 1 --author "nspire-usbaudio" \
	  --color-support=true --clickpad-support=false --240x320-support=true --compress \
	  --ndless-min 31 --ndless-rev-min 2001 --uses-lcd-blit false
$(B)/nspire $(B)/asan $(B)/qemu $(B)/host $(B)/legacy:
	mkdir -p $@

# ---- test WAV (the Twinkle melody, 22.05 kHz mono 16-bit) ----
$(B)/host/mkwav: tools/mkwav.c src/melody.c src/melody.h tests/wavbuild.h | $(B)/host
	$(HOSTCC) $(WARN) -O2 -Isrc -Itests tools/mkwav.c src/melody.c -o $@
$(B)/music.wav.tns: $(B)/host/mkwav
	$< $@

# ---- PC tests ----
$(B)/asan/library_test: tests/library_test.c src/library.c src/wav.c $(HDRS) | $(B)/asan
	$(HOSTCC) $(CF_asan) tests/library_test.c src/library.c src/wav.c -o $@
$(B)/asan/main_test: tests/main_test.c src/main.c $(HDRS) tests/platform/os.h tests/platform/platform_fake.h tests/platform/nspireio/nspireio.h | $(B)/asan
	$(HOSTCC) $(CF_asan) -Itests/platform tests/main_test.c -o $@
$(B)/asan/music_main_test: tests/main_test.c src/main.c src/screen.c src/ui_font.h src/library.c src/wav.c $(HDRS) tests/platform/os.h tests/platform/platform_fake.h tests/platform/nspireio/nspireio.h | $(B)/asan
	$(HOSTCC) $(CF_asan) -Itests/platform -DUA_MUSIC_PLAYER tests/main_test.c src/screen.c src/library.c src/wav.c -o $@
$(B)/asan/app_test: tests/app_test.c tests/fake/fake_os.c $(LIB_SRCS) $(HDRS) | $(B)/asan
	$(HOSTCC) $(CF_asan) tests/app_test.c tests/fake/fake_os.c $(LIB_SRCS) -o $@
LEGACY_SRCS := src/ua_os.c src/ua_os.h src/uac.c src/uac.h src/pcm.c src/pcm.h
$(B)/legacy/src/ua_os.c: | $(B)/legacy
	git archive $(LEGACY) $(LEGACY_SRCS) | tar -x -C $(B)/legacy
$(B)/asan/legacy_test: tests/legacy_test.c tests/fake/fake_os.c $(B)/legacy/src/ua_os.c $(HDRS) | $(B)/asan
	$(HOSTCC) $(WARN) -O1 -g -fsanitize=address,undefined,alignment -fno-sanitize-recover=all \
	  -fno-omit-frame-pointer -I$(B)/legacy/src -Itests -I$(SHIM) -DUA_HOST_TEST tests/legacy_test.c \
	  tests/fake/fake_os.c $(B)/legacy/src/ua_os.c $(B)/legacy/src/uac.c $(B)/legacy/src/pcm.c -o $@
$(B)/asan/%: tests/%.c $(PURE_SRCS) $(HDRS) | $(B)/asan
	$(HOSTCC) $(CF_asan) $< $(PURE_SRCS) -o $@
$(B)/qemu/%: tests/%.c $(PURE_SRCS) $(HDRS) tests/newlib_shim.c | $(B)/qemu
	$(ARMCC) $(CF_qemu) $< $(PURE_SRCS) tests/newlib_shim.c -o $@
$(B)/qemu/ti_abi_test: tests/ti_abi_test.c tests/ti_abi_fixture.S tests/t.h tests/newlib_shim.c | $(B)/qemu
	$(ARMCC) $(CF_qemu) tests/ti_abi_test.c tests/ti_abi_fixture.S tests/newlib_shim.c -o $@

test: $(addprefix $(B)/asan/,$(TESTS)) $(addprefix $(B)/qemu/,$(QEMU_TESTS)) check
	@set -e; for t in $(TESTS); do $(B)/asan/$$t; done
	@set -e; for t in $(QEMU_TESTS); do $(QEMU) -cpu arm926 $(B)/qemu/$$t; done

# gpSP's queue (its Makefile's GPSP_UA_NF / GPSP_UA_NX), every test, in a build dir of its own
test-gpsp:
	$(MAKE) test B=build-gpsp UA_DEFS="-DUA_NF=14 -DUA_NX=4 -DUA_START_ONE"

check: $(B)/usbaudio.tns $(B)/musicplayer.tns
	tests/check_calc.sh $(NOBJS)
	tests/check_calc.sh $(filter-out $(B)/nspire/main.o,$(NOBJS)) $(B)/nspire/music_main.o
	@set -e; for app in usbaudio musicplayer; do \
	if arm-none-eabi-objdump -d --no-show-raw-insn $(B)/$$app.elf | awk -F'\t' 'NF>=2 {print $$2}' | \
	  grep -qE '^(uxt|sxt|rev|ldrex|strex|movw|movt|pkh|usat|ssat|sdiv|udiv|v[a-z]+\.f)'; then \
	  echo "check: $$app.elf has ARMv6+ or VFP instructions"; exit 1; else echo "check: $$app.elf: ARMv5, no VFP"; fi; done

clean:
	rm -rf $(B)
