# Top-level build: FatFs -> IOP module -> EE program
.PHONY: all iop ee fatfs test sim clean distclean dist

all: ee

fatfs: iop/ff.c

iop/ff.c:
	bash tools/get_fatfs.sh

iop: iop/ff.c
	$(MAKE) -C iop

ee: iop
	$(MAKE) -C ee

# host simulator of the IOP core + end-to-end tests with the PC client
sim: iop/ff.c
	$(MAKE) -C tests/host

test: iop/ff.c
	$(MAKE) -C tests/host test

# release archive: ELF, client, example ini, docs
dist: ee
	rm -rf dist && mkdir -p dist/ps2xfer
	cp ee/ps2xfer.elf pc/ps2xfer.py ps2xfer.ini.example README.md LICENSE THIRD_PARTY.md dist/ps2xfer/
	cp -r docs dist/ps2xfer/
	cd dist && (zip -qr ps2xfer.zip ps2xfer 2>/dev/null || python3 -m zipfile -c ps2xfer.zip ps2xfer)

clean:
	$(MAKE) -C iop clean
	$(MAKE) -C ee clean
	$(MAKE) -C tests/host clean
	rm -rf dist

# also removes the downloaded FatFs sources
distclean: clean
	rm -f iop/ff.c iop/ff.h iop/ffconf.h iop/diskio.h iop/ffunicode.c
