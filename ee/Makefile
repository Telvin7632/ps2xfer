# ps2xfer.elf - EE program. All IRX drivers are embedded into the ELF.

EE_BIN  = ps2xfer.elf

# Drivers from the SDK ($PS2SDK/iop/irx) + our own module
IRX_SDK = iomanX poweroff ps2dev9 bdm bdmfs_fatfs ata_bd
IRX_C   = $(addsuffix _irx.c,$(IRX_SDK)) ps2xfer_irx.c

EE_OBJS = main.o $(IRX_C:.c=.o)
EE_LIBS = -ldebug -lpad -lpatches -lpoweroff -lelf-loader
EE_CFLAGS += -Wall -I../common
EE_INCS += -I../common

BIN2C = $(PS2SDK)/bin/bin2c

all: $(EE_BIN)

%_irx.c: $(PS2SDK)/iop/irx/%.irx
	$(BIN2C) $< $@ $*_irx

ps2xfer_irx.c: ../iop/ps2xfer.irx
	$(BIN2C) $< $@ ps2xfer_irx

main.o: ../common/px_stats.h ../common/proto.h

clean:
	rm -f $(EE_BIN) $(EE_OBJS) $(IRX_C)

.SECONDARY: $(IRX_C)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
