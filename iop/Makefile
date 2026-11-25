# ps2xfer.irx - IOP module. FatFs sources (ff.c, ff.h, ffconf.h, diskio.h,
# ffunicode.c) are put here by ../tools/get_fatfs.sh ("make fatfs" at the top).

IOP_BIN  = ps2xfer.irx
IOP_OBJS = main.o net.o smap.o smap_xfer.o plat_ps2.o rpc.o \
           util.o log.o rxeng.o writer.o ctrl.o fsops.o disk.o bench.o eesum.o \
           ff.o ffunicode.o imports.o

IOP_OPTFLAGS = -O2
IOP_CFLAGS  += -Wall -I../common -MMD -MP  # .d files: header changes rebuild the users
# 64-bit division helpers (__udivdi3 etc.): FatFs LBA64 and 64-bit offsets
IOP_LIBS    += -lgcc

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN) *.notiopmod*.elf

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
-include $(wildcard obj/*.d)
