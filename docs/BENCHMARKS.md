# Benchmarks

Status: **Phase 1 tooling ready; no real-PS2 numbers yet.** Speed is measured
only on a real console (PCSX2 numbers are meaningless). Fill the tables below
from the commands shown; `ps2xfer.py` prints exactly these values.

Setup to record with every run:

| Item | Value |
|---|---|
| Console model | |
| Network adapter | |
| Disk (model, UDMA mode from the PS2 screen) | |
| Link shown on the PS2 screen | |
| PC, OS, NIC; direct cable or switch | |
| Client: Python version, native Windows / WSL mirrored / Linux | |
| SPEED revision, max DMA block (`bench micro`) | |
| IOP free memory after setup (screen / `info`) | |

## 1. micro: basic costs on the IOP

`python ps2xfer.py bench micro` (ticks = 1/36.864 µs)

| Measurement | Value |
|---|---|
| word-Fletcher, ticks per KiB | |
| memcpy, ticks per KiB | |
| one SPEED register read (1000 reads / 1000) | |
| one GetSystemTime() | |
| IOP free / max free block | |

From these: cost of `csum=full` per packet = 1.375 × Fletcher ticks/KiB.

## 2. disk: RAM → ATA

```
python ps2xfer.py bench disk-raw   --size-mb 1024 --chunks 16,32,64,128,256,704,2048
python ps2xfer.py bench disk-fatfs --size-mb 1024 --chunks 65536,131072,360448
# reboot ps2xfer.elf with fs_iomanx=1 in ps2xfer.ini, then:
python ps2xfer.py bench disk-iomanx --size-mb 1024 --chunks 65536,131072,360448
```

| Path | Chunk | Write MB/s | IOP CPU % | Read MB/s | IOP CPU % |
|---|---|---:|---:|---:|---:|
| raw (bd->write) | 16 sectors | | | | |
| raw | 64 | | | | |
| raw | 256 | | | | |
| raw | 704 (= 1 buffer) | | | | |
| raw | 2048 | | | | |
| FatFs f_write | 128 KiB | | | | |
| FatFs f_write | 352 KiB | | | | |
| bdmfs_fatfs + iomanX | 128 KiB | | | | |
| bdmfs_fatfs + iomanX | 352 KiB | | | | |

## 3. net-sink: receive and discard

```
python ps2xfer.py --rate 11.8 --rate-start 11.8 bench net-sink --size-mb 1024
python ps2xfer.py --rate 11.8 --rate-start 11.8 bench net-sink-nodma --size-mb 1024
python ps2xfer.py set prof=1      # then repeat: adds per-packet ticks
python ps2xfer.py set hdr_dma=1   # header by DMA instead of PIO
python ps2xfer.py set dma_block=64
python ps2xfer.py --burst 1 ... / --burst 4 ...
```

| Mode | Rate set | Burst | MB/s | pkt/s | Retransmits | RX errors / overruns | IOP CPU % |
|---|---:|---:|---:|---:|---:|---:|---:|
| sink (payload DMA) | 11.8 | 2 | | | | | |
| sink-nodma (headers only) | 11.8 | 2 | | | | | |
| sink, hdr_dma=1 | 11.8 | 2 | | | | | |
| sink, dma_block=64 | 11.8 | 2 | | | | | |
| sink | 11.8 | 1 | | | | | |
| sink | 11.8 | 4 | | | | | |

Profile (`prof=1`): payload DMA ticks/packet ___, whole RX path ticks/packet ___.

## 4. combined: network + real disk writes

```
python ps2xfer.py bench combined --size-mb 4096
python ps2xfer.py set ata_chunk=64      # etc.
```

| ata_chunk | csum | bufs × units | MB/s | Retransmits | RX errors / overruns | IOP CPU % |
|---:|---:|---|---:|---:|---:|---:|
| 0 (704) | 16 | 3 × 8 | | | | |
| 64 | 16 | 3 × 8 | | | | |
| 256 | 16 | 3 × 8 | | | | |
| 0 | 0 | 3 × 8 | | | | |
| 0 | 1 | 3 × 8 | | | | |

## 5. Python client

`python ps2xfer.py bench selftest` (build + checksum + sendto to localhost):

| Machine / OS | packets/s | µs/packet | CPU % of one core |
|---|---:|---:|---:|
| WSL2 Ubuntu, Python 3.12.3, Ryzen 7 9700X (2026-11-22) | 67 900 | 14.7 | 96 (busy loop) |

Line rate needs 8 356 packets/s, so the Python sender uses about 12 % of one
core: **no Go/C rewrite needed** (decision rule of DESIGN.md §12). Native
Windows numbers: to be added (`py ps2xfer.py bench selftest`).

## 6. Ceiling analysis (to be filled from 1–4)

Pessimistic serialized model (docs/DESIGN.md §10):

```
pps_max = 36.864e6 / (c_rx + 1408 * 36.864e6 * (1/R_smap + 1/R_ata) + c_csum)
```

| Term | Value | Source |
|---|---|---|
| R_ata (raw write, MB/s) | | §2 |
| R_smap (1408 B DMA) = 1408 / payload DMA ticks × 36.864e6 | | §3 profile |
| c_rx (RX path without DMA, ticks) | | §3 profile |
| c_csum | | §1 |
| **pps_max / MB/s predicted** | | |
| **measured (combined)** | | §4 |

## Before / after (Phase 3)

| Change | MB/s before | MB/s after |
|---|---:|---:|
