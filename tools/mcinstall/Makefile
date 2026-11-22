# mcinstall.elf - test helper (see main.c). Not part of the release.
EE_BIN  = mcinstall.elf
EE_OBJS = main.o
EE_LIBS = -ldebug -lelf-loader-nocolour -lpatches
EE_CFLAGS += -Wall

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
