# Benchmarks

Status: **Phase 1 tooling ready and tested in PCSX2; no real-PS2 numbers yet.**
Speed is measured only on a real console (PCSX2 timing is not cycle-accurate).
Fill the tables below from the commands shown; `ps2xfer.py` prints exactly
these values.

## 0. PCSX2 functional test (2026-11-22)

PCSX2 (Qt, Windows), BIOS SCPH-70012 v12, DEV9 network in **Sockets** mode
(`ip=192.0.2.100`), HDD image 40 GiB formatted by ps2-exfat-format, client
`ps2xfer.py` under native Windows Python 3.14 against `127.0.0.1`.

| Check | Result |
|---|---|
| Module load, SMAP init, PHY | OK; link 100 Mbit full duplex (from PHYSTS), SPEED rev 0x11 → 128-byte DMA blocks |
| Disk | IDENTIFY OK (UDMA 4); exFAT mounted, cluster 128 KiB, data at LBA 4096 |
| IOP memory | max free block before the write buffers 1 251 584 bytes; after 3 × 352 KiB: 285 952 free (largest block 170 240) |
| info / ls / stat / df / mkdir / mv / rm / ping / log / set | OK |
| send 50 MiB ISO + `--verify`; send 1 234 567-byte file + `--verify` | OK |
| client killed at 25.1 MiB, `send` again | resumed at 24.8 MiB, verify OK |
| bench micro / disk-raw / disk-fatfs / net-sink / net-sink-nodma / combined | all run; bench file removed afterwards |
| EXIT | PS2 returns to the BIOS browser |
| HDD image read back by an independent exFAT reader (tests/host/test_e2e.py) | both files SHA-256 identical to the source, NoFatChain (contiguous); no `.part`/`.ps2x`/bench leftovers |

Found and fixed by this test: HELLO parse error in the client; link mode
taken from ANLPAR (not emulated by PCSX2) instead of PHYSTS; no rate
reduction on loss that happens before the PS2. PCSX2's Sockets mode drops
most packets at 9 MB/s, so the first run needed 35 727 retransmits for
37 237 packets. With loss-ratio rate control (cut above 10 % loss, grow
below 2 %) the same 50 MiB took 11.8 s (4.45 MB/s) with 1 714 retransmits
(4.6 %). Thresholds of 5 % / 1.25 % gave 0.7 % retransmits at ~4.0 MB/s but
collapsed the rate under 5 % random loss in the host tests.

Known PCSX2 difference: its IDENTIFY model string is not byte-swapped per
the ATA spec, so the screen shows "CPXS-2ED9VA-ATH-DD". Real drives follow
the spec and are decoded correctly.

Setup to record with every run:

| Item | Value |
|---|---|
| Console model | fat PS2 (owner's), started via ps2link from USB |
| Network adapter | SPEED rev 0x11 (ES2) |
| Disk (model, UDMA mode from the PS2 screen) | Seagate ST3320413CS 320 GB, UDMA 4; exFAT by ps2-exfat-format, cluster 128 KiB |
| Link shown on the PS2 screen | 100 Mbit full duplex |
| PC, OS, NIC; direct cable or switch | Windows 11, Realtek 2.5GbE, **direct cable** (PC 192.168.1.1, PS2 192.168.1.10) |
| Client: Python version, native Windows / WSL mirrored / Linux | Python 3.14, native Windows |
| SPEED revision, max DMA block (`bench micro`) | 0x11, 128 bytes |
| IOP free memory after setup (screen / `info`) | 1 251 584 bytes max block before the buffers; 285 952 free after 3 × 352 KiB |

All numbers in §1–§4 are from this setup, 2026-11-22.

## 1. micro: basic costs on the IOP

`python ps2xfer.py bench micro` (ticks = 1/36.864 µs)

| Measurement | Value |
|---|---|
| word-Fletcher, ticks per KiB | 1496 (= 2057 per 1408-byte packet) |
| memcpy, ticks per KiB | 1507 |
| one SPEED register read (1000 reads / 1000) | 14.2 |
| one GetSystemTime() | 150 |
| IOP free / max free block | 285 952 / 170 240 (after the buffers) |

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
| raw (bd->write) | 16 sectors | 22.66 | 100.0 | 24.81 | 100.0 |
| raw | 64 | 44.88 | 99.9 | 48.48 | 100.0 |
| raw | 256 | 55.56 | 98.6 | 59.08 | 100.0 |
| raw | 704 (= 1 buffer) | 56.18 | 95.6 | 62.17 | 99.9 |
| raw | 2048 | 56.28 | 93.7 | 63.46 | 99.9 |
| FatFs f_write | 128 KiB | 55.21 | 99.2 | 58.47 | 100.0 |
| FatFs f_write | 352 KiB | 54.72 | 99.6 | 57.42 | 99.9 |
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
| sink (payload DMA) | 11.8 | 2 | **11.44** | 8125 | 0 | 0 / 0 | 100 |
| sink-nodma (headers only) | 11.8 | 2 | 11.53 | 8192 | 0 | 0 / 0 | 100 |
| sink, hdr_dma=1 | 11.8 | 2 | 11.44 | 8122 | 0 | 0 / 0 | 100 |
| sink, dma_block=64 | 11.8 | 2 | 11.44 | 8124 | 0 | 0 / 0 | 100 |
| sink | 11.8 | 1 | | | | | |
| sink | 11.8 | 4 | | | | | |

Profile (`prof=1`, 381 184 packets): payload DMA **2088** ticks/packet
(SMAP → RAM ≈ 24.9 MB/s), whole RX path **3345** ticks/packet including
~450 ticks of the profiler's own timer calls, so ≈ 2900 without them.

The sink runs at 97 % of the 11.76 MB/s wire ceiling for 1408-byte payloads
with no loss, so the network path is limited by the wire, not by the IOP.
The header/DMA-block switches make no difference. "CPU 100 %" is the idle
thread starving: the SMAP thread spins in DMA waits; the profile shows the
real per-packet cost (2900 × 8125 pkt/s ≈ 64 % of the IOP).

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
