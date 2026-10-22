CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter
BUILD   := build

COMMON  := src/common/machine.c src/common/mathbox.c src/common/raster.c
CPU     := third_party/mc6809/mc6809.c

.PHONY: all clean roms

all: $(BUILD)/irobot-harness

$(BUILD)/irobot-harness: harness/main.c $(COMMON) $(CPU) src/common/*.h third_party/mc6809/mc6809.h
	@mkdir -p $(BUILD)/snaps
	$(CC) $(CFLAGS) -o $@ harness/main.c $(COMMON) $(CPU) -Wno-bool-compare -Wno-sign-compare

# Unpack a MAME irobot.zip from roms/ into roms/irobot/
roms:
	python3 tools/extract_roms.py roms/irobot.zip roms/irobot

clean:
	rm -rf $(BUILD)
