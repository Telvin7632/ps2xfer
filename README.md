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

> **Status:** works on a real console. Tested on a fat PS2 with a 320 GB
> disk (exFAT, formatted on the console): **9.5 MB/s sustained** with no lost
> packets (the IOP is the limit, see `docs/BENCHMARKS.md`), games of 8 GiB,
> `--verify` at **~45 MB/s** (checksums on the EE), copying back to the PC at
> 5.2 MB/s, games booting from OPL 1.2.0-Beta-2271. Resume after an
> interrupted transfer works on the console (stalled sends resumed at 7.4 GiB);
> pulling the cable is tested in PCSX2 and the host simulator (`make test`).

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
   and during a transfer the file, progress bar, speed, ETA and error
   counters; during `--verify` a green VERIFYING bar, during a copy to the PC
   a SENDING TO THE PC bar. If the TV cuts an edge, move the picture with the
   **D-pad**; the screen shows the values to keep in `ps2xfer.ini`
   (`screen_x=`, `screen_y=`).
4. To leave, hold **START** for 2 seconds (or run `ps2xfer.py exit`). An
   unfinished transfer is kept for resuming.

## On the PC

### Window

On Windows the simplest way is **`ps2xfer-gui.exe`** from the release archive
(no Python needed); `ps2xfer.exe` is the same program for the command line.
With Python: double-click `ps2xfer.py` (or `py ps2xfer.py gui`).

Enter the PS2's IP (or leave it empty to find it by broadcast) and press
**Connect**. The header shows the link, the disk and the free space.

* Left: folders on the PS2 (DVD games, CD games, cover art, apps, all files).
* Middle: the games with their ID, size, whether a cover is on the PS2 and the
  date. Click a column to sort, type in **Search** to filter. Right-click,
  `Del`, `F2` (rename), `F5` (refresh).
* Right: the **cover** of the selected game (from `ART/` on the PS2, cached
  in `~/.ps2xfer/covers`; during a transfer from the internet source).
* **+ Add games** / **Add folder**, or **drop .iso files on the window**:
  they go to `DVD/` or `CD/`. **OPL names** gives them the
  `SLUS_xxx.xx.Name.iso` form OPL needs on the HDD; **Verify after sending**
  and **Download cover art** apply to each file as it starts.
* **Rename games for OPL automatically** (menu PS2, on by default): ISOs in
  `DVD/` and `CD/` with plain names (copied with Explorer or another tool) get
  the OPL form as soon as the list is shown; the game ID is read from
  `SYSTEM.CNF` inside the image. Switched off, a yellow bar offers
  **Rename for OPL** instead.
* **Transfers**: the queue with the state of each file (waiting, sending,
  verifying, cover art, done, failed), the current file's progress, speed and
  time left, and the whole queue's progress. **Cancel** stops; the file
  resumes when it is sent again.
* **Verify** checks files already on the PS2 against the originals on the PC
  (remembered for files sent from this window, otherwise it asks).
* Menu **PS2**: cover art for all games, exit ps2xfer on the PS2. **View →
  Log** shows the details.

Settings are kept in `~/.ps2xfer.json`.

### Command line

```
python ps2xfer.py --host 192.168.0.10 send "Game (USA).iso"
python ps2xfer.py send game1.iso game2.zso --dest /DVD/
python ps2xfer.py send D:\ps2iso               # every .iso/.zso of the folder -> /CD or /DVD
python ps2xfer.py send D:\ps2iso --dest /DVD/  # the folder's contents straight into /DVD
python ps2xfer.py send game.iso --verify      # PS2 re-reads the file, compares 1 MiB checksums
python ps2xfer.py send game.iso --verify --verify-full   # ... or sends it back: SHA-256 on the PC
python ps2xfer.py get "/DVD/Game.iso" D:\backup\         # copy from the PS2 to the PC
python ps2xfer.py ls /DVD
python ps2xfer.py df
python ps2xfer.py rm /DVD/old.iso
python ps2xfer.py mv /DVD/a.iso /DVD/b.iso
python ps2xfer.py art                         # art for games already on the PS2 (send does it itself)
python ps2xfer.py opl-rename                  # SLUS_xxx.xx.Name.iso names (see below)
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

### OPL and game names

With OPL 1.2.0-Beta-2271 the owner's console lists ISOs on the internal HDD
(BDM, exFAT) only with OPL's old naming `SLUS_205.52.Name.iso`; plain names
(`Name.iso`) work from USB but not from the HDD (cause not found yet; files
and disk check out). So `send` names `.iso` files that way by default (the
game ID comes from SYSTEM.CNF inside the image; OPL shows the name without
the ID). `--keep-name` disables it; `opl-rename` converts games already on
the PS2.

`art` downloads OPL artwork for every game on the PS2 from the OPL Manager
art backup on archive.org (covers from xlenore/ps2-covers as a fallback) and
stores it in `/ART`.

### Windows Explorer (FTP gateway)

```
py ps2xfer.py --host 192.168.0.10 ftp
```

Then open `ftp://127.0.0.1:2121` in Explorer (or *Add a network location*).
You can browse the PS2 disk and copy files in both directions, rename,
delete and create folders. Notes:

* the PS2 must be running ps2xfer, and the `ftp` command must stay open;
* a file copied to the PS2 is first received into a temporary file on the PC
  (the PS2 needs the size in advance to keep the file in one piece); Explorer
  reports the copy as done only when the file is on the PS2;
* empty files cannot be created on the PS2;
* files cannot be edited in place: copy, edit, copy back.

### Several programs at once

The window, the FTP gateway, `stats`, `ls` and other commands can be used
while a file is being sent (each client gets its own session; the PS2 answers
commands within ~0.2 s during a transfer). Only one file is written at a
time: a second `send` gets "the PS2 is receiving a file from another client"
until the first one finishes. If a client crashes or the cable is pulled, the
next `send` of the same file takes over after 2 s and resumes.

### WSL

WSL2 in the default NAT mode passes unicast UDP but **not broadcast**, so
discovery does not work there. Either run the client natively on Windows
(`py ps2xfer.py ...`), pass `--host`, or enable mirrored networking
(`networkingMode=mirrored` in `%UserProfile%\.wslconfig`).

Data packets need a **1500-byte MTU** on the way to the PS2 (1458 bytes of
IP). Some WSL2 NAT setups and VPNs have less (e.g. 1300): then commands work
but no data arrives. On Linux/WSL the client warns about it when it connects,
and a send stops after 15 s without progress. Use the Windows client or
mirrored networking.

## Testing in PCSX2

PCSX2 can run ps2xfer for functional tests (not for speed):

1. Settings → Network & HDD: enable the HDD (an image formatted with
   ps2-exfat-format) and Ethernet with **Sockets** as the API, device
   **Auto**. Enable **Host Filesystem** (Settings → Advanced) so the ELF can
   read `ps2xfer.ini` from its folder.
2. Put `ps2xfer.ini` with `ip=192.0.2.100` next to `ps2xfer.elf`. In Sockets
   mode PCSX2 delivers packets to that address.
3. Start the ELF (System → Start File). PCSX2 binds UDP port 7690 on the PC
   when the PS2 sends its first ANNOUNCE.
4. Run the client with **Windows** Python against localhost:
   `py ps2xfer.py --host 127.0.0.1 info`.

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
