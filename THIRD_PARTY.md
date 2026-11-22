# Third-party code

| Component | Files | Origin | Licence |
|---|---|---|---|
| SMAP driver | `iop/smap.c`, `iop/smap_xfer.c`, `iop/smap.h` (struct + prototypes) | Neutrino, `iop/smap` @ 1310a9af7c73 (github.com/rickgaiser/neutrino), itself derived from the ps2sdk smap driver | Academic Free License 3.0 (Neutrino), Academic Free License 2.0 (ps2sdk). Changes for ps2xfer are marked `ps2xfer:` in the source. |
| FatFs R0.16 | `iop/ff.c`, `ff.h`, `ffconf.h`, `diskio.h`, `ffunicode.c` (downloaded by `tools/get_fatfs.sh`, not in git) | ChaN, elm-chan.org/fsw/ff | FatFs licence (BSD-style, 1 clause) |
| PS2SDK IRX modules embedded in `ps2xfer.elf` | iomanX, poweroff, ps2dev9, bdm, bdmfs_fatfs, ata_bd | ps2sdk (github.com/ps2dev/ps2sdk) | Academic Free License 2.0 |

Everything else: MIT (see LICENSE).
