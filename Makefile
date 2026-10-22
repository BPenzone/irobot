CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter
BUILD   := build
B32     := $(BUILD)/32x

COMMON  := src/common/machine.c src/common/mathbox.c src/common/raster.c \
           src/common/image.c src/common/pokey.c
CPU     := third_party/mc6809/mc6809.c
HEADERS := src/common/*.h third_party/mc6809/mc6809.h

.PHONY: all clean roms 32x recomp-selftest

all: $(BUILD)/irobot-harness

$(BUILD)/irobot-harness: harness/main.c harness/reference.c harness/reference.h $(COMMON) src/common/video_soft.c $(CPU) $(HEADERS) $(BUILD)/recomp.o
	@mkdir -p $(BUILD)/snaps
	$(CC) $(CFLAGS) -DIR_HINTS -o $@ harness/main.c harness/reference.c $(COMMON) src/common/video_soft.c $(CPU) $(BUILD)/recomp.o \
		-Wno-bool-compare -Wno-sign-compare

# The 6809 program translated to C (from the ROMs, so it stays in build/).
RECOMP_HINTS := tools/recomp/hints.txt
RECOMP_HOT   := tools/recomp/hot.txt
$(BUILD)/recomp.c: tools/recomp/recomp.py $(B32)/irobot.img $(wildcard $(RECOMP_HINTS) $(RECOMP_HOT))
	python3 tools/recomp/recomp.py $(B32)/irobot.img $@ --exit-at 8E5E \
		$(if $(wildcard $(RECOMP_HINTS)),--hints $(RECOMP_HINTS)) \
		$(if $(wildcard $(RECOMP_HOT)),--hot $(RECOMP_HOT))

$(BUILD)/recomp.o: $(BUILD)/recomp.c src/common/rc_runtime.h third_party/mc6809/mc6809.h
	$(CC) -O2 -c -o $@ $< -Isrc/common -Wno-bool-compare

# Differential test of the recompiler against the interpreter.
recomp-selftest:
	@mkdir -p $(BUILD)
	python3 tools/recomp/recomp.py --selftest $(BUILD)/rc_selftest.c --count 20000
	$(CC) -O1 -DRC_SELFTEST -Isrc/common -o $(BUILD)/rc-selftest tools/recomp/selftest.c \
		$(BUILD)/rc_selftest.c $(CPU) -Wno-bool-compare
	$(BUILD)/rc-selftest

# Unpack a MAME irobot.zip from roms/ into roms/irobot/
roms:
	python3 tools/extract_roms.py roms/irobot.zip roms/irobot

clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# 32X build: build/irobot.32x
#
# Needs sh-elf-gcc (Ubuntu: gcc-sh-elf, libnewlib-sh-elf-dev) and a 68000
# gcc (Ubuntu: gcc-m68k-linux-gnu), plus the ROMs in roms/irobot/.
# ---------------------------------------------------------------------------

SHCC    := sh-elf-gcc
SHOBJC  := sh-elf-objcopy
MDCC    := m68k-linux-gnu-gcc
MDOBJC  := m68k-linux-gnu-objcopy
MARSDEV := third_party/marsdev

SHFLAGS := -m2 -mb -O2 -std=gnu99 -ffreestanding -fomit-frame-pointer \
           -Wall -Wextra -Wno-unused-parameter -Wno-bool-compare -Wno-sign-compare \
           -DIR_DLIST_EXTERNAL -DIR_DLIST_WORDS=1024 -DIR_PIXEL_BASE=1
MDFLAGS := -m68000 -O2 -std=gnu99 -ffreestanding -fomit-frame-pointer -Wall

SH_C    := $(COMMON) $(CPU) src/32x/video_32x.c src/32x/sound_32x.c src/32x/sh_main.c
SH_GEN  := $(B32)/sh/recomp.o
SH_OBJ  := $(patsubst %.c,$(B32)/sh/%.o,$(SH_C)) $(SH_GEN) $(B32)/sh/mars_start.o $(B32)/sh/image.o
MD_OBJ  := $(B32)/md/md_start.o $(B32)/md/md_main.o

32x: $(BUILD)/irobot.32x

$(BUILD)/irobot.32x: $(B32)/irobot.elf
	$(SHOBJC) -O binary $< $(B32)/irobot.bin
	dd if=$(B32)/irobot.bin of=$@ bs=128k conv=sync status=none

$(B32)/irobot.elf: $(SH_OBJ) $(MARSDEV)/mars.ld
	$(SHCC) -m2 -mb -T $(MARSDEV)/mars.ld -nostartfiles \
		$(B32)/sh/mars_start.o $(filter-out $(B32)/sh/mars_start.o,$(SH_OBJ)) -o $@ -lc -lgcc

$(B32)/sh/%.o: %.c $(HEADERS) src/32x/shared.h
	@mkdir -p $(dir $@)
	$(SHCC) $(SHFLAGS) -c $< -o $@

$(B32)/sh/recomp.o: $(BUILD)/recomp.c src/common/rc_runtime.h
	@mkdir -p $(dir $@)
	$(SHCC) $(filter-out -O2,$(SHFLAGS)) -Os -DRC_HOT_SECTION -DRC_TAILCALL -Isrc/common -c $< -o $@

$(B32)/sh/mars_start.o: $(MARSDEV)/mars_start.s $(B32)/md_start.bin
	@mkdir -p $(dir $@)
	sh-elf-as --small -I $(B32) $< -o $@

$(B32)/sh/image.o: src/32x/image.s $(B32)/irobot.img
	@mkdir -p $(dir $@)
	sh-elf-as -I $(B32) $< -o $@

$(B32)/irobot.img: $(BUILD)/mkimage roms/irobot/136029-101.bin
	@mkdir -p $(dir $@)
	$(BUILD)/mkimage roms/irobot $@

$(BUILD)/mkimage: tools/mkimage.c src/common/image.c src/common/irobot.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/mkimage.c src/common/image.c

# 68000 program, embedded in the SH-2 image at $3F0
$(B32)/md_start.bin: $(MD_OBJ) $(MARSDEV)/md.ld
	$(MDCC) -T $(MARSDEV)/md.ld -nostdlib -Wl,--build-id=none $(MD_OBJ) -o $(B32)/md_start.elf
	$(MDOBJC) -O binary -j .text -j .data $(B32)/md_start.elf $@

$(B32)/md/md_start.o: $(MARSDEV)/md_start.s $(MARSDEV)/common.h
	@mkdir -p $(dir $@)
	$(MDCC) -x assembler-with-cpp -I $(MARSDEV) -m68000 -Wa,--register-prefix-optional -c $< -o $@

$(B32)/md/md_main.o: src/32x/md_main.c
	@mkdir -p $(dir $@)
	$(MDCC) $(MDFLAGS) -c $< -o $@

# Headless 32X runner around a libretro core (PicoDrive), for screenshots.
$(BUILD)/retro-run: tools/retro/retro_run.c tools/retro/libretro.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ $< -ldl
