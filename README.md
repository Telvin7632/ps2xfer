# ps2xfer

Copy games and other files from a PC to the **exFAT internal HDD of a PS2**
over the network, as fast as the PS2's 100 Mbit Ethernet allows. Games are
then started with an **OPL 1.2.0 beta build with exFAT support** (BDM, ATA
device). The disk stays in the console.

* `ps2xfer.elf` runs on the PS2 (from wLaunchELF, OPL, FMCB, PS2BBL). It
  receives files with its own minimal network stack on the IOP and writes them
  straight to the disk. The data never passes through the EE and no lwIP/TCP
  is involved.
* `ps2xfer.py` is the PC client: one Python file, standard library only,
  for Windows, Linux and WSL.

> **Status: Phase 1 (benchmark tooling).** It builds, and the protocol,
> resume and file-system logic pass end-to-end tests against a host simulator.
> It has **not been run on a real PS2 or in PCSX2 yet**. Expect bugs on real
> hardware; please report what the screen and `ps2xfer.py log` show.

Every file is written as **one contiguous run** (exFAT "NoFatChain"),
preallocated before the data arrives. That is the best case for OPL. During
the transfer the file is called `NAME.part`; it gets its final name only after
all data is on the disk. An interrupted transfer (cable pulled, PC crash,
power loss) leaves a valid file system and continues where it stopped when you
run the same `send` again.

## Requirements

* A "fat" PS2 with the network adapter and an IDE/SATA disk formatted as
  MBR + exFAT, e.g. with the ps2-exfat-format tool.
* The PS2 and the PC on the same network, ideally a direct cable or a simple
  switch. The PS2 screen must show **100 Mbit full duplex**.
* PC: Python 3.10 or newer.

## On the PS2

1. Copy `ps2xfer.elf` to a memory card or USB stick and start it from
   wLaunchELF (or any launcher).
2. Optional `ps2xfer.ini` next to the ELF (or `mc0:/PS2XFER/ps2xfer.ini`):
   see `ps2xfer.ini.example`. Without it the IP comes from wLaunchELF's
   `mc0:/SYS-CONF/IPCONFIG.DAT`, otherwise **192.168.0.10** is used.
3. The screen shows the IP, link mode, disk model and UDMA mode, free space,
   and during a transfer the file, progress, speed and error counters.
4. To leave, hold **START** for 2 seconds (or run `ps2xfer.py exit`). An
   unfinished transfer is kept for resuming.

## On the PC

```
python ps2xfer.py --host 192.168.0.10 send "Game (USA).iso"
python ps2xfer.py send game1.iso game2.zso folder/ --dest /DVD/
python ps2xfer.py send game.iso --verify      # read back on the PS2 and compare
python ps2xfer.py ls /DVD
python ps2xfer.py df
python ps2xfer.py rm /DVD/old.iso
python ps2xfer.py mv /DVD/a.iso /DVD/b.iso
python ps2xfer.py info | stats | log | ping | exit
```

* Without `--host` the client finds the PS2 by broadcast.
* `.iso`/`.zso` files go to `/CD/` if smaller than 700 MiB, otherwise to
  `/DVD/`, unless `--dest` says otherwise. The client warns about names OPL
  will not like (non-ASCII, longer than 160 characters).
* An existing file is not replaced without `--overwrite`.
* Pressing Ctrl+C keeps `NAME.part` on the PS2; the next `send` of the same
  file resumes it.
* If the disk has no contiguous free area large enough, the PS2 refuses the
  file ("no contiguous free space"). Free some space (or defragment the disk
  on a PC) and try again.

### WSL

WSL2 in the default NAT mode passes unicast UDP but **not broadcast**, so
discovery does not work there. Either run the client natively on Windows
(`py ps2xfer.py ...`), pass `--host`, or enable mirrored networking
(`networkingMode=mirrored` in `%UserProfile%\.wslconfig`).

## Building

Linux or WSL with the PS2DEV toolchain (`PS2DEV`, `PS2SDK` set), `make`,
`curl`, `unzip`, `git`:

```
make            # downloads FatFs R0.16, builds iop/ps2xfer.irx and ee/ps2xfer.elf
make test       # host simulator + end-to-end tests (needs gcc; exfatprogs for fsck)
make dist       # dist/ps2xfer.zip
```

GitHub Actions builds the same with the `ps2dev/ps2dev` docker image
(`.github/workflows/build.yml`).

## Benchmarks (Phase 1)

See `docs/BENCHMARKS.md` for the exact commands and tables:
`bench micro`, `bench disk-raw|disk-fatfs|disk-iomanx`,
`bench net-sink|net-sink-nodma`, `bench combined`, `bench selftest`.
Tuning parameters can be changed without rebooting:
`python ps2xfer.py set ata_chunk=64`.

## Documentation

* `docs/DESIGN.md`: architecture, packet size, buffers, flow control, DEV9
  contention, budgets
* `docs/PROTOCOL.md`: the wire protocol
* `docs/BENCHMARKS.md`: measurements
* `THIRD_PARTY.md`: licences of included code (SMAP driver from Neutrino,
  FatFs, PS2SDK modules)
