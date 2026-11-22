# Host build of the portable IOP core (ps2xfer-sim) + tests.
# IOP sources are copied to build/src first so that FatFs gets a host ffconf.h
# with f_mkfs enabled (the image is formatted by the simulator itself).

ROOT   := ../..
IOP    := $(ROOT)/iop
BUILD  := build
SRC    := $(BUILD)/src

CORE   := util.c log.c rxeng.c writer.c ctrl.c fsops.c disk.c bench.c
FATFS  := ff.c ffunicode.c
HOST   := plat_host.c host_main.c

CC     ?= gcc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(SRC) -I. -I$(ROOT)/common
LDFLAGS := -fsanitize=address,undefined -pthread

OBJS := $(addprefix $(BUILD)/,$(CORE:.c=.o) $(FATFS:.c=.o) $(HOST:.c=.o))

.PHONY: all test clean
all: $(BUILD)/ps2xfer-sim

$(SRC)/.stamp: $(addprefix $(IOP)/,$(CORE) $(FATFS) px.h plat.h ff.h ffconf.h diskio.h)
	mkdir -p $(SRC)
	cp $(addprefix $(IOP)/,$(CORE) $(FATFS) px.h plat.h ff.h diskio.h) $(SRC)/
	sed -E 's/^#define[[:space:]]+FF_USE_MKFS[[:space:]].*/#define FF_USE_MKFS 1/' $(IOP)/ffconf.h > $(SRC)/ffconf.h
	touch $@

$(addprefix $(SRC)/,$(CORE) $(FATFS)): $(SRC)/.stamp
	@true

$(BUILD)/%.o: $(SRC)/%.c $(SRC)/.stamp
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c $(SRC)/.stamp plat_host.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ps2xfer-sim: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

test: $(BUILD)/ps2xfer-sim
	python3 test_e2e.py --sim $(BUILD)/ps2xfer-sim --client $(ROOT)/pc/ps2xfer.py

clean:
	rm -rf $(BUILD)
