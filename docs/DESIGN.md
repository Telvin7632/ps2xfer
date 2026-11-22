# ps2xfer — design (Phase 0)

Status: **approved by the owner 2026-11-21** (§15).

Implementation status (Phase 1, 2026-11-22). Implemented: everything in §3–§8
and §11–§14, except:
* `--verify` uses the **blocks** mode (PS2 reads the file and computes a
  word-Fletcher per 1 MiB; the PC compares). The **stream** mode (read-back to
  the PC + SHA-256) is Phase 2;
* on resume the client does not yet re-check the last 32 MiB with BLOCKSUMS
  (§8.2). It trusts the durable watermark, which is written only after a disk
  cache flush;
* `prof_hdr` (header PIO time) is not measured separately; the profile gives
  payload DMA and the whole RX path.

Changes after testing in PCSX2 (2026-11-22): the PS2 broadcasts ANNOUNCE
every 2 s until a client connects; link mode comes from the PHY status
register (PHYSTS); the client's rate control is AIMD on the measured loss
ratio (PROTOCOL.md §5), replacing the fixed ramp of §6.2.

Every statement is tagged as one of:

* **[src]** checked in source code (path given; versions in §1.1);
* **[est]** an estimate or hypothesis, measured in Phase 1 (`docs/BENCHMARKS.md`);
* **[dec]** a design decision. Ones marked **[dec, approved]** change a rule
  from the task and were approved by the owner (§15).

---

## 1. Goal and the hard numbers

Copy files (mainly ISO/ZSO, up to DVD9 size > 4 GiB) from a PC to the exFAT
partition on the internal HDD of a fat PS2 over 100 Mbit Ethernet, as fast as
the hardware allows and with no loss of integrity. Games are then started by an
**OPL 1.2.0 beta build with exFAT support (BDM, ATA device)**. Neutrino is not a target (owner, 2026-11-21); its code is only a reference.

### 1.1 Sources studied

| What | Version | Notes |
|---|---|---|
| Installed PS2SDK | `/usr/local/ps2dev/ps2sdk`, IRX dated 2026-05-17 | used for the build |
| ps2sdk source | `ps2dev/ps2sdk` @ `d4b9943` (2026-11-20) | `atad.h` differs from the installed one only by `ata_set_command_type` (unused here) |
| Neutrino | `rickgaiser/neutrino` @ `1310a9a` (2026-11-12) | `iop/smap`, `iop/udpfs` (UDPRDMA), `iop/udpbd`, `iop/ministack` |
| OPL | `ps2homebrew/Open-PS2-Loader` @ `b3a2199` (2026-10-12) | `src/bdmsupport.c`, `include/supportbase.h` |
| FatFs used by ps2sdk | `fjtrujy/FatFs` branch `iop-r0.16` | `bdmfs_fatfs` config |
| Owner's formatter | `~/ps2-exfat-format` (not `~/ps2`) | Makefiles, IRX embedding, load order, FatFs R0.16 |

### 1.2 Wire ceiling

Per frame on the wire: preamble+SFD 8 + Ethernet 14 + IPv4 20 + UDP 8 +
our header 22 + payload P + FCS 4 + inter-frame gap 12 = **P + 88 bytes**.
At 100 Mbit/s (12 500 000 B/s):

| Payload P | Wire bytes | Frames/s | Payload MB/s (10^6) | DMA fit |
|---:|---:|---:|---:|---|
| 1024 (2×512) | 1112 | 11 241 | 11.51 | 512-byte blocks |
| 1280 (5×256) | 1368 | 9 137 | 11.70 | 256-byte blocks |
| **1408 (11×128)** | **1496** | **8 356** | **11.76** | 128-byte blocks |
| 1448 (max mult. of 4 with 22-byte header) | 1536 | 8 138 | 11.78 | 1408 DMA + 40 B PIO |

Target ≥ 10 MB/s = **7 102 packets/s = 85 % of the wire**.

---

## 2. What the sources say (facts the design rests on)

### 2.1 SMAP (Ethernet)

* RX buffer in SMAP: **16 KiB** (`SMAP_RX_BUFSIZE 16384`), TX 4 KiB,
  **64 RX buffer descriptors** (`SMAP_BD_MAX_ENTRY 64`).
  [src] `common/include/smapregs.h:297-304`
* RX copy: set `SMAP_R_RXFIFO_RD_PTR` to (frame + offset), then PIO-read
  `SMAP_R_RXFIFO_DATA` or DMA from it. The read pointer can be set to any offset
  inside the frame, so header-by-PIO + payload-by-DMA into any 4-byte-aligned
  destination works. [src] Neutrino `iop/smap/src/xfer.c: smap_fifo_read()`
* **DMA block size.** ps2sdk smap now uses **64-byte** blocks
  (`NumBlocks << 16 | 0x10`), not 128 as the task assumed. Neutrino picks
  512/256/128 when the length is a multiple, and limits to 128 on SPEED
  rev ≤ 0x12 (`g_dma_max_block_size`). [src] ps2sdk `iop/network/smap/src/xfer.c`,
  Neutrino `iop/smap/src/smap.c:516-522`, `UDPRDMA.md`
* The EMAC3 strips FCS on RX (`SMAP_E3_RX_STRIP_FCS`), so a 1408-byte payload
  frame takes 14+20+8+22+1408 = **1472 bytes** in the FIFO. That is
  **11 frames = 1.32 ms at line rate**: the IOP must drain the FIFO at least
  that often. [src] Neutrino `smap.c:585`, arithmetic
* Link speed, duplex and negotiated PAUSE come from PHY registers
  (`BMCR`, `ANAR & ANLPAR`): `LinkMode` 8 = 100FD, 4 = 100HD, `|0x40` = flow
  control. [src] Neutrino `smap.c:323-345`
* EMAC3 supports 802.3x PAUSE (`SMAP_E3_FLOWCTRL_ENABLE`, `RX_WATERMARK`).
  [src] `smapregs.h:121-122, 274`
* `smap-modular-none.irx` (`smap_modular.h`) hooks give only
  "allocate a buffer for the **whole** frame, then enqueue it". Header and
  payload cannot be split, so zero-copy into the write buffer is impossible
  through that API. [src] `iop/include/smap_modular.h`, `xfer.c:HandleRxIntr`

### 2.2 DEV9 DMA arbitration

* All DEV9 DMA (SMAP = ctrl 1, ATA = ctrl 0) goes through
  `SpdDmaTransfer()` (alias `dev9DmaTransfer`). It takes `dma_lock_sem`,
  starts the DMA and **busy-waits** on `CHCR.TR` ("thread switching hurts
  throughput greatly"). [src] `iop/dev9/dev9/src/ps2dev9.c:431-489`
* `dma_lock_sem` is created with **`SA_THPRI`** by default, so when it is
  released, the **highest-priority** waiter gets it. [src] `ps2dev9.c:210, 650`
* The CPU spins while the DMA runs. A higher-priority thread made ready by an
  interrupt still preempts the spinning thread. It just cannot start its own
  DMA until the lock is released.

### 2.3 ATA (ata_bd = atad built with BDM)

* `ata_device_sector_io64()` splits a request into ATA commands of ≤ 65 536
  sectors (LBA48). [src] `iop/dev9/atad/src/ps2atad.c:1041-1112`
* Inside one command the data is moved in **pieces of ≤ 31 sectors**: the
  count comes from the SPEED buffer status `SPD_REG16(0x38) & 0x1f`. **Each
  piece is a separate `SpdDmaTransfer()`**, so the DMA lock is released
  between pieces. While the SPEED buffer waits for the drive, the ATA thread
  sleeps in `WaitEventFlag`. [src] `ps2atad.c:730-772`
* Consequence: the longest time an ATA write can hold the DEV9 DMA is one
  piece of ≤ 15.5 KiB. This does **not** depend on the size of our
  `bd->write()` call. [src]+[est]
* UDMA: ata_bd selects the highest supported mode up to **UDMA4**
  (`ATA_ENABLE_MAXUDMA ?= 0`). `ata_devinfo_t` has no UDMA field. We read
  IDENTIFY word 88 ourselves (`sceAtaExecCmd(0xEC)` + `sceAtaWaitResult`).
  [src] `ps2atad.c:1321-1336`, `atad.h`
* **No locking** in `ata_bd_read/write`: two threads must never issue ATA
  commands at the same time. [src] `ps2atad.c:1551-1567`
* `bd->write(bd, u64 sector, buf, u16 count)` returns `count` or `-EIO`.
  `flush` = `sceAtaFlushCache`. [src] `iop/include/bdm.h`, `ps2atad.c`

### 2.4 bdmfs_fatfs, OPL, FatFs

* The FatFs in ps2sdk's `bdmfs_fatfs` is built with **`FF_USE_EXPAND 0`**,
  `FF_FS_EXFAT 1`, `FF_LBA64 1`, `FF_FS_TINY 1`, `FF_FS_REENTRANT 1`.
  [src] `fjtrujy/FatFs@iop-r0.16 source/include/ffconf.h`
* So **`f_expand` cannot be reached through iomanX/fileXio**. `ioctl2` offers
  only `USBMASS_IOCTL_GET_LBA` (start LBA, absolute on the disk),
  `CHECK_CHAIN` and `GET_FRAGLIST`. [src] `iop/fs/bdmfs_fatfs/src/fs_driver.c:706-806`
* OPL (BDM) builds the game's fragment list with
  `fileXioIoctl2(..., USBMASS_IOCTL_GET_FRAGLIST)`, limit `BDM_MAX_FRAGS`.
  A contiguous file = 1 fragment. [src] OPL `src/bdmsupport.c:430-456`
* OPL ISO name rules: `.iso`/`.zso`, name ≤ 160 chars (`ISO_GAME_NAME_MAX`).
  Old `SXXX_YYY.ZZ.Name.iso` format also accepted. [src] OPL `include/supportbase.h`,
  `src/supportbase.c:62`
* FatFs R0.16 `f_expand(fp, fsz, 1)` on exFAT: requires an empty file opened
  for writing, finds one contiguous free run in the bitmap (`find_bitmap`),
  marks it, sets `obj.stat = 2` (contiguous, no FAT chain) and
  `objsize = fsz`. Returns `FR_DENIED` if there is no such run.
  [src] `~/ps2-exfat-format/iop/ff.c:5726-5801`
* `get_frag_list()` in bdmfs_fatfs walks `get_fat()`, and FatFs handles
  `stat == 2` on exFAT. So OPL sees our files as 1 fragment. [src]

### 2.5 Neutrino as proof of concept

* Neutrino README: **UDPBD ≈ 10 MB/s** (PC → IOP RAM, the same direction as
  ours) and **ATA ≈ 30 MB/s** ("older tests, an indication"). These are the
  only published reference numbers. [src] `neutrino/README.md:29-33`
* Its UDPRDMA uses a fixed 1408-byte max payload (11×128), header by PIO and
  payload by DMA, Go-Back-N, and window-ACK every 6 packets with a send window
  of 8. [src] `iop/udpfs/UDPRDMA.md`. We need more: a much larger window
  (disk latency), SACK instead of Go-Back-N, credits and 64-bit offsets.
  So we take the ideas, not the protocol.

### 2.6 Licences

* ps2sdk: AFL-2.0. Neutrino: AFL-3.0. Both permissive: notices must be kept,
  derived code may be distributed under another licence. FatFs: its own
  BSD-style 1-clause licence. The formatter (and this project): MIT.
* **[dec]** The SMAP driver code taken from Neutrino/ps2sdk stays in
  `iop/smap/` with its original notices and an `AFL-3.0` note. The rest is MIT.
  `THIRD_PARTY.md` lists everything.

---

## 3. Architecture

```
            PC (Windows / WSL / Linux)                           PS2
 ┌──────────────────────────────────┐  UDP   ┌────────────────────────────────────────────────┐
 │ ps2xfer.py                        │ ─────► │ IOP: ps2xfer.irx                               │
 │  pacing (perf_counter), window,   │ DATA   │  SMAP RX intr ─► RX thread (prio 10)           │
 │  SACK retransmit, Fletcher,       │        │    PIO 64-byte header                          │
 │  progress, queue, verify (SHA-256)│ ◄───── │    DMA 1408 B ─► write buffer[k] @ offset      │
 └──────────────────────────────────┘ ACK/   │    bitmap, credits, ACK+SACK (TX PIO)           │
                                      replies│  Writer thread (prio 20)                        │
                                             │    full buffer ─► bd->write (ata_bd, UDMA)      │
                                             │  Control thread (prio 30): FatFs, commands      │
                                             │  RPC thread (prio 40): stats for the EE        │
                                             │  Idle thread (prio 126): CPU load meter        │
                                             ├────────────────────────────────────────────────┤
                                             │ EE: ps2xfer.elf - loads IRX, draws status,     │
                                             │     polls stats by SIF RPC every 250 ms, pad   │
                                             └────────────────────────────────────────────────┘
```

Data path: **SMAP RX FIFO → (DEV9 DMA) → IOP RAM buffer → (DEV9 DMA) → ATA**.
Payload is never copied by the CPU and never goes through the EE.

### 3.1 Modules and load order

ELF with all IRX embedded by `bin2c` (as in the formatter). Sequence:
read config (§11) → `SifIopReset("")` → `sbv_patch_enable_lmb` /
`sbv_patch_disable_prefix_check` → load:

| # | Module | Source | Why |
|---|---|---|---|
| 1 | `iomanX.irx` | SDK | dev9 imports `ioman` |
| 2 | `poweroff.irx` | SDK | power-button callback (flush before power-off) |
| 3 | `ps2dev9.irx` | SDK | DEV9/SPEED, DMA arbitration |
| 4 | `bdm.irx` | SDK | block device manager |
| 5 | `ata_bd.irx` | SDK | ATA as BDM block device (UDMA) |
| 6 | `ps2xfer.irx` | ours | SMAP driver + net stack + protocol + FatFs + writer |
| – | `rom0:SIO2MAN`, `rom0:PADMAN` | ROM | gamepad (as in the formatter) |

**Not loaded:** `smap*.irx`, `netman`, `ps2ip`, `udptty`, `ps2link`,
`ps2hdd`/`ps2fs` (APA), `usbd`/`usbmass_bd` (so the only BDM device is ATA),
and in the normal mode also **`bdmfs_fatfs` and `fileXio`**:

**[dec, approved]** `ps2xfer.irx` contains **its own FatFs R0.16**
(`FF_USE_EXPAND 1`, exFAT, LBA64, LFN, UTF-8 API) working on the raw ATA block
device, as the formatter does. Reasons:

1. `f_expand` is not reachable through `bdmfs_fatfs` (§2.4), and contiguous
   preallocation is a hard requirement.
2. Two FatFs instances must never mount the same volume: `bdmfs_fatfs`
   auto-mounts every BDM device it sees. So either ours or theirs, never both.
3. The IOP talks to the disk directly. iomanX adds a layer and fileXio only
   matters for EE-side access, which we do not do.

`bdmfs_fatfs` (+ `iomanX` file API) is loaded **only in the
`bench disk-iomanx` mode** for the comparison the task asks for. In that mode
our FatFs is not mounted.

The device is picked like in the formatter: `bdm_get_bd()` → `name == "ata"`,
`devNr == 0`, `parNr == 0` (whole disk). FatFs finds the exFAT partition
through the MBR itself. USB is impossible since no USB driver is loaded, and
the name is checked anyway.

### 3.2 Why a SMAP driver inside our module

Zero-copy needs "read header, decide, DMA payload to an address chosen from the
header" inside the RX handler. ps2sdk's smap variants do not allow that (§2.1).
Neutrino's `smap` has exactly this (`smap_register_rx_callback` +
`smap_fifo_read`) and builds against ps2sdk's `dev9` (`dev9DmaTransfer` =
`SpdDmaTransfer`, `dev9.h:66-78`). **[dec]** We take Neutrino's
`iop/smap/src/{smap.c,xfer.c}` (≈ 900 lines), trimmed, into `iop/smap/`, and
add: link-mode/PAUSE reporting, RX error counters, optional PAUSE enable,
TX by PIO for small frames and DMA for large ones.

### 3.3 Mini stack (own code, ≈ 600 lines)

* **Ethernet:** accept our MAC and broadcast (the EMAC3 filter already does
  this).
* **ARP:** reply to requests for our IP. We **never send ARP requests**:
  every reply goes to the source MAC of the request frame. That works for the
  same L2 segment and through a router.
* **IPv4:** accept `ver=4, IHL=5`, not fragmented, dst = our IP or broadcast.
  The IP header checksum is checked only on the slow path (ICMP/control).
  **UDP checksum is not checked** (Ethernet FCS covers the frame). It is sent
  as 0 (allowed in IPv4).
* **ICMP echo:** reply, payload ≤ 1472, copied by PIO through a 1.5 KiB
  staging buffer.
* **UDP:** one port, default **7690** **[dec]**, configurable.

---

## 4. Packet size and header layout

**[dec] Payload = 1408 bytes (11 × 128)**:

* the largest multiple of 128 that fits the MTU (1408 + 22 + 8 + 20 = 1458
  ≤ 1500). The next multiple, 1536, does not fit;
* one DMA with 128-byte blocks, which works on **every** SPEED revision
  (§2.1). No PIO tail;
* only 0.15 % below the absolute max (1448), and 2.2 % above 1024. It also
  needs **26 % fewer packets/s** than 1024 (8 356 vs 11 241). Per-packet IOP
  cost is the scarce resource (§10), so this matters more than the bytes.

The only drawback is that 1408 is not a multiple of 512. That is solved by
buffer sizing (§5).

**[dec] DATA header (22 bytes)**, placed so that every field is naturally
aligned **relative to the frame start**. The whole 64-byte header block is
16 PIO word reads. Little-endian (the IOP is LE; no byte swaps):

| Frame offset | Size | Field |
|---:|---:|---|
| 0 | 14 | Ethernet |
| 14 | 20 | IPv4 |
| 34 | 8 | UDP |
| 42 | 1 | `type` (DATA = 0x10, others in PROTOCOL.md) |
| 43 | 1 | `flags` (bit0 LAST, bit1 RETRANSMIT) |
| 44 | 8 | `offset` (u64 LE, file offset of the payload, multiple of 1408) |
| 52 | 4 | `pkt_seq` (u32, +1 per transmission incl. retransmits; RTT/loss detection) |
| 56 | 4 | `csum` (u32 word-Fletcher of the payload, §9) |
| 60 | 2 | `handle` (u16, changes with every OPEN_WRITE; stale packets dropped) |
| 62 | 2 | `len` (u16 payload length; < 1408 only in the last packet, padded to 4) |
| 64 | 1408 | payload → **DMA straight into the write buffer** |

Phase 3 option: read only the words that are needed (≈ 10 instead of 16) and
compare "DMA the 64-byte header" with "PIO the header". Both are bench
switches (`hdr=pio|dma`).

---

## 5. Buffers

* **Unit U = lcm(1408, 4096) = 45 056 bytes = 32 packets = 88 sectors.**
  A buffer of k·U bytes covers file range `[base, base + k·U)` with
  `base` a multiple of U. Then:
  * a packet never straddles two buffers;
  * each buffer is a whole number of **4 KiB** blocks. Its disk LBA is
    4K-aligned because the file starts on a cluster boundary and the formatter
    puts the cluster heap on a 1 MiB boundary of the disk. This avoids
    read-modify-write on 512e drives. Checked at run time: warning if
    `start_lba % 8 != 0`.
  (`lcm(1408, 512) = 5632` would avoid straddling but not the 4K alignment.)
* **[dec] Default buffer = 8 U = 360 448 bytes (352 KiB, 256 packets,
  704 sectors)**, count **B = 3** (1056 KiB). One buffer takes 30.6 ms to fill
  at line rate. While one is written, two receive: that is 61 ms of network
  time covering disk latency.
* B and k are run-time parameters (ini / bench flags). At start:
  `B = min(B_cfg, (QueryMaxFreeMemSize() − 128 KiB) / (k·U))`, B ≥ 2.
  Free memory after all modules is shown on screen and recorded in
  BENCHMARKS.md. **[est]** Expected 1.3–1.6 MiB free. Neutrino's docs treat
  1 MiB as a usable transfer size.
* Buffer states: `FREE → FILLING → FULL → WRITING → FREE`. Each buffer has a
  256-bit received bitmap and a byte count. Buffers map to consecutive file
  ranges, and the writer writes them in file order.

---

## 6. Flow control

### 6.1 Receiver (PS2)

* **Window** = file range covered by FILLING buffers:
  `[W, credit)`, where `W` = base of the oldest unwritten buffer and
  `credit = W + (B − buffers FULL/WRITING) · k·U`. The PS2 gives credit only
  for free buffer space.
* DATA with `offset < W` or already in the bitmap → duplicate, the frame is
  skipped (read pointer advanced, no DMA). `offset ≥ credit` or wrong
  `handle` → dropped and counted. Inside the window: DMA to
  `buf[(offset / (k·U)) mod B] + offset mod (k·U)`, set the bit.
* **ACK** (PS2 → PC), sent from the RX thread:
  * every **32 packets** (1 U) or **2 ms** after the first unacknowledged
    packet;
  * immediately when a new hole appears (at most 1 per ms);
  * when a buffer is freed (credit grows).
  Contents: `cum_offset` (everything below is in RAM), `credit_offset`,
  `disk_offset` (written to disk), `max_pkt_seq_seen`, a **SACK bitmap** of
  packets from `cum_offset` up to the end of the window (≤ B·256 bits = 96
  bytes), and RX counters (FIFO overruns, drops). Typical size ≈ 200 bytes:
  ~250 ACKs/s is negligible.

### 6.2 Sender (PC)

* Never sends `offset ≥ credit_offset`; in flight ≤ `--window`
  (default = credit).
* **Pacing:** a token schedule at rate `--rate` (default: start at 9 MB/s,
  ramp up to 11.5 MB/s), sending **bursts of `--burst` packets**
  (default 2) at precise times from `time.perf_counter()`. Spin-wait for the
  last ≤ 1.5 ms instead of `time.sleep`. When the schedule falls behind by
  more than 2 ms it resets instead of catching up with a burst. A burst must
  stay far below the 11 frames the FIFO holds (§2.1).
* **Loss detection, RACK-style:** a packet is lost when an ACK shows
  `max_pkt_seq_seen` ≥ its `pkt_seq` + 3 and it is not SACKed (LAN has no
  reordering, the margin is for safety). Lost packets are re-sent first,
  inside the same pacing budget, with a new `pkt_seq`.
* **RTO:** no ACK progress for 100 ms (doubling to 1 s) → re-send the oldest
  unacknowledged packet as a probe. If credit is 0 (the disk is slow), the
  sender just waits and sends a small `PING` every 50 ms to get an ACK.
* **Rate adaptation:** if FIFO overruns / losses are reported, rate ×0.9;
  otherwise +0.25 MB/s per second up to `--rate-max`. Statistics shown:
  current/average rate, ETA, retransmits.
* `SO_SNDBUF` = 4 MiB.

---

## 7. DEV9 contention strategy

### 7.1 Analysis

* Worst-case wait of the RX thread for the DMA lock = one ATA piece
  (≤ 31 sectors, §2.3). **[est]** At ~30 MB/s that is ≈ 0.5 ms, i.e. ~4
  frames, while the FIFO holds 11 (1.32 ms). With RX at a higher priority than
  the writer and `SA_THPRI` on the lock, **concurrent** receive + write should
  not overflow the FIFO because of ATA DMA.
* Alternating "receive phase / write phase" (stop the PC while writing) caps
  throughput at `1 / (1/11.76 + 1/R_ata)`. **[est]** That is 8.4 MB/s for
  R_ata = 30 MB/s, **below the target**. So alternation is only a fallback,
  not the plan.
* The real limit is that **the CPU spins during every DMA** (§2.2). Pessimistic
  model where nothing overlaps (§10):
  `pps_max = f_cpu / (c_fixed + 1408·f_cpu·(1/R_smap + 1/R_ata) + c_csum)`.

### 7.2 Plan

1. **[dec]** Concurrent pipeline, thread priorities: RX **10** (highest of
   ours), writer **20**, control **30**, RPC **40**, idle meter **126**
   (IOP: lower number = higher priority).
2. The writer writes a full buffer with `bd->write()` in chunks of
   `ata_chunk` sectors (default = whole buffer, 704). Phase 1 measures loss and
   MB/s for `ata_chunk` ∈ {16, 32, 64, 128, 256, 704, 2048} as the task asks.
   Per §2.3, the expected effect is on command overhead and not on RX latency.
3. Disk mutex: all ATA access (writer, FatFs in the control thread, VERIFY
   reads) goes through one semaphore (§2.3: atad has no locking).
4. Measured fallbacks if loss shows up:
   a. **802.3x PAUSE** (EMAC3 + negotiated with the PC NIC). The SMAP pauses
      the sender when the FIFO is above its watermark. Bench switch
      `pause=on|off`.
   b. Phase alternation per buffer with a credit gap.
   c. A patched `ata_bd` with a smaller cap per DMA piece (the
      `count = min(blkcount, dma_stat)` line), only if (a) and (b) fail.

---

## 8. File system

### 8.1 Writing a file

1. `OPEN_WRITE(path, size, flags)`: `f_open(NAME.part, FA_CREATE_ALWAYS |
   FA_WRITE)` → **`f_expand(fp, size, 1)`** (one contiguous run, or
   `FR_DENIED`) → `f_sync`. **This is the only metadata write until the end.**
2. Extent: `lba0 = clst2sect(sclust)`, `nsect = clusters · csize`.
   Checked: contiguous (`stat == 2`), inside the volume data area,
   `lba0 % 8 == 0` (warning).
3. Data: **raw path** (default): `bd->write(lba0 + off/512, ...)` through one
   function `extent_write()` that **rejects** anything outside
   `[lba0, lba0 + nsect)`. Or **FatFs path** (`f_write`, max one cluster per
   ATA command). Both are bench modes, plus `bdmfs_fatfs`+iomanX (§3.1).
   Default chosen by measurement.
4. The last packet is padded with zeros to the sector. This stays inside the
   last cluster, which belongs to the file.
5. `CLOSE`: all data present → `bd->flush` → `f_close` → delete the state
   file → **`f_rename(NAME.part, NAME)`** → `f_sync` of the directory.
6. No contiguous free space → error "no contiguous free space of N GiB"
   (**[dec, approved]**: no fragmented fallback by default).

The filesystem only changes at OPEN and CLOSE. A cable cut or a crash in
between leaves a valid filesystem with an incomplete `.part`. Power loss
exactly during OPEN/CLOSE has the usual exFAT risk (milliseconds).

### 8.2 Resume

* Sidecar `NAME.part.ps2x`, preallocated to one cluster, updated **by raw
  writes inside its own extent** (no metadata changes). It holds two copies
  (A/B sectors, sequence number + CRC32) of: magic, version, file size,
  source fingerprint (size + mtime + SHA-256 of the first 1 MiB, from the PC),
  **durable watermark**.
* The watermark advances every 32 MiB (configurable): data below it written →
  `bd->flush` → state sector written.
* `OPEN_WRITE(..., RESUME)` with a matching fingerprint returns the watermark.
  The client asks `BLOCKSUMS` (word-Fletcher per 1 MiB, computed by the PS2
  from the disk) for the last 32 MiB below it. It moves the start back to the
  first mismatching block and continues from there.

### 8.3 Other operations

LIST/STAT/MKDIR/DELETE/RENAME/DF run on the control thread under the disk
mutex. DF = `f_getfree`. **[est]** On exFAT the first call scans the bitmap
(2 MiB for a 2 TiB disk), later calls are cached. `f_expand` also scans the
bitmap (`find_bitmap`, bit by bit). Worst case seconds on a full big disk;
done once per file.

---

## 9. Integrity

| Layer | Protection |
|---|---|
| Wire | Ethernet FCS (EMAC3 drops bad frames, counted) |
| PC → IOP RAM | **word-Fletcher per packet** in the header (exact formula in PROTOCOL.md: s1 = Σw, s2 = Σs1 over 32-bit LE words, mod 2^32). The PC computes it in C speed with stdlib (`sum`, `itertools.accumulate` over `memoryview.cast('I')`). |
| Bookkeeping | **manifest sum**: the PS2 adds `mix(offset, csum)` of every packet on first receipt. CLOSE carries the PC's value, so a missed/duplicated offset is caught. Cost: a few cycles per packet. |
| IOP → disk | UDMA CRC (atad returns `ATA_RES_ERR_ICRC` and retries ×3) |
| End to end | `--verify`: **stream** mode = PS2 reads the file back and sends it, the PC compares SHA-256 with the source (default). **blocks** mode = PS2 computes word-Fletcher per 1 MiB from disk, less network traffic. |

**Checking the per-packet csum on the IOP costs about the same as everything
else per packet** (**[est]** 352 loads + 704 adds ≈ 1500–2500 cycles; §10).
**[dec]** So it is a switch `csum=off|sample:N|full`, and the default is chosen
from Phase 1 numbers (expected: `sample:16`). A mismatch clears the bit, so
SACK triggers a re-send before the buffer goes to disk. Phase 3 option:
verify in the idle thread with the CPU time that is left.

---

## 10. IOP budgets

### 10.1 Cycles per packet (36.864 MHz)

Available: **5 191 cycles/packet at 10 MB/s**, 4 412 at line rate.

| Item | **[est]** cycles | Phase 1 measures |
|---|---:|---|
| SMAP payload DMA 1408 B (spin) | 1 000–1 750 | R_smap for blocks 64/128/512 |
| ATA DMA of the same 1408 B (spin) | 1 200–1 750 | R_ata from the disk bench |
| Header PIO 16 words over SSBUS | 160–400 | cost of one SPEED register read |
| BD handling, classification, bitmap, credit | 200–400 | cycle counter around the RX path |
| Interrupt + wake-up (amortized by batching) | 100–300 | |
| ACK TX (amortized, 1 per 32) | ~50 | |
| **Total without csum** | **2 700–4 650** | |
| csum `full` | +1 500–2 500 | Fletcher cycles/KB |

So 10 MB/s fits in the pessimistic serialized model if R_smap and R_ata are
around 30 MB/s or better. Line rate needs both to be better. **This is the
central risk.** Phase 1 measures every term and computes the real ceiling. If
it is below 10 MB/s, BENCHMARKS.md will show which term limits it.

CPU load is measured by an idle thread (prio 126) that counts loop
iterations, compared against an unloaded calibration run. Timestamps come from
an IOP hardware timer (`AllocHardTimer`/`GetTimerCounter`, `timrman.h`).

### 10.2 RAM (2 MiB IOP)

| Item | Size |
|---|---:|
| ROM kernel + ROM modules after reset, SDK IRX (iomanX, poweroff, ps2dev9, bdm, ata_bd ≈ 60 KiB) | measured |
| `ps2xfer.irx` code + data (SMAP ≈ 12 K, stack ≈ 8 K, protocol/writer ≈ 20 K, FatFs exFAT+LFN ≈ 40 K) | **[est]** 80–100 KiB |
| Thread stacks (5 × 2–8 KiB) | ~24 KiB |
| Log ring buffer | 16 KiB |
| FatFs objects, control/ICMP/TX staging buffers | ~8 KiB |
| **Write buffers** B × k·U (default 3 × 352 KiB) | **1 056 KiB** |
| Reserve | ≥ 128 KiB |

---

## 11. EE side and configuration

* **Config** (before the IOP reset, while the launcher's IOP modules can
  still read files):
  1. `ps2xfer.ini` next to the ELF (`argv[0]` dir). Works for `mc0:`,
     `host:` (PCSX2) and anything else the launcher left readable;
  2. `mc0:/SYS-CONF/IPCONFIG.DAT` (wLaunchELF format "IP MASK GW". The exact
     format will be checked in wLaunchELF's source in Phase 2);
  3. default `192.168.0.10`.
  The ini also carries tuning knobs: `port`, `bufs`, `buf_units`, `ata_chunk`,
  `csum`, `pause`, `rx_prio`, `ack_every`, `ack_ms`, `hdr`, `write_path`.
  They are passed to `ps2xfer.irx` as module arguments. `SET` from the PC can
  change them while idle (bench sessions without rebooting).
* **Screen** (`scr_printf`, redrawn 4×/s from one SIF RPC stats call): IP, MAC,
  UDP port, link (**100 Mbit full duplex** or a red warning for half duplex /
  10 Mbit), PAUSE on/off, SPEED revision, disk model + **UDMA mode** (IDENTIFY
  word 88), exFAT size / free / cluster size, IOP free memory, current file,
  progress, MB/s (current/average), retransmits, drops/overruns, IOP CPU load.
* **Exit:** hold START for 2 s, or `EXIT` from the PC. The IOP stops receiving,
  writes the state file of an unfinished `.part`, flushes the disk, and the EE
  returns to the OSD. The power button (poweroff callback) does the same
  flush.
* No `printf` on hot paths. The IOP log goes to a 16 KiB ring buffer, read by
  `LOG` from the PC.

---

## 12. PC client

* **[dec]** `pc/ps2xfer.py`, one file, Python ≥ 3.10, stdlib only. Commands:
  `send <files/dirs> [--dest]`, `ls`, `rm`, `mv`, `mkdir`, `df`, `verify`,
  `ping`, `bench`, `log`.
* Destination defaults: `.iso`/`.zso` < 700 MiB → `CD/`, otherwise `DVD/`,
  overridable with `--dest`. Warnings: non-ASCII names, names > 160 chars
  without extension, other extensions.
* Discovery: HELLO broadcast to `255.255.255.255:7690` when no `--host` is
  given.
* **Python check in Phase 1:** `ps2xfer.py bench --selftest` measures packets/s
  for build + Fletcher + `sendto` on Windows and WSL. Requirement: ≥ 9 000 pps
  with < 50 % of one core. **[est]** ~20–40 µs/packet → OK. If it fails, the
  hot loop is rewritten as a single Go binary (no dependencies), with the same
  CLI.
* README: WSL2 NAT passes unicast but not broadcast, so use the client
  natively on Windows or set `networkingMode=mirrored`. Prefer a direct cable
  or a switch. The PC NIC must not force half duplex.

---

## 13. Testing

1. **Host simulation (`tests/host`)**: the protocol, window, writer, FatFs
   and state-file code of the IOP compiled for Linux. A UDP socket replaces
   SMAP, a sparse disk image replaces ATA (same approach as the formatter's
   host tests). It is the target for the real Python client, with loss,
   duplication and delay injection, cable-cut simulation and resume.
   `fsck.exfat -n` checks the image afterwards. Runs in CI.
2. **PCSX2** (DEV9 Ethernet + HDD image formatted by the formatter): protocol
   and integration on the real IRX. No speed numbers from PCSX2.
3. **Real PS2**: speed and the acceptance criteria (3 ISOs incl. > 4 GiB, hash
   after read-back, contiguous, starts in OPL beta, cable cut +
   resume, ≥ 10 MB/s or a proven ceiling).

## 14. Phase 1 benchmark plan (same ELF, `bench` modes)

| Mode | What | Parameters | Output |
|---|---|---|---|
| `micro` | SPEED register read, DMA rate SMAP (64/128/512-byte blocks), Fletcher cycles/KB, free IOP memory, SPEED rev | – | cycles, MB/s |
| `disk-raw` | RAM → bench file extent via `bd->write` | `ata_chunk` 16…2048 | MB/s write/read, CPU % |
| `disk-fatfs` | same via `f_write` | write size 64 K…1 M | MB/s, CPU % |
| `disk-iomanx` | same via `bdmfs_fatfs` + iomanX `write()` (separate boot mode) | write size | MB/s, CPU % |
| `net-sink` | receive + DMA + drop, ACKs as usual | PC `--rate`, `--burst`, `hdr`, `pause` | pps, MB/s, loss, overruns, CPU % |
| `combined` | real transfer | all knobs | MB/s, loss, CPU %, the before/after table for Phase 3 |

All disk benches write **only inside a preallocated bench file**
(`/.ps2xfer/bench.tmp`, deleted afterwards). No raw disk access outside an
extent, ever.

## 15. Owner decisions (2026-11-21)

All seven approved. The owner chose explicitly: no contiguous space → refuse (3);
full read-back verification only on `--verify`, not by default (4).
Neutrino is not a launch target; only OPL beta with exFAT.

1. **Own FatFs inside `ps2xfer.irx`; `bdmfs_fatfs`/`fileXio` not loaded in
   the normal mode** (only in `bench disk-iomanx`). §3.1
2. **Neutrino's SMAP driver (AFL-3.0) copied into our module**, rest MIT.
   §2.6, §3.2
3. **No contiguous space → refuse** (no fragmented fallback; maybe a later
   `--allow-fragmented`). §8.1
4. **Per-packet csum check on the IOP is a switch**, default from
   measurements (expected `sample:16`). Full guarantee comes from `--verify`
   (SHA-256 on the PC). §9
5. **Resume via the `NAME.part.ps2x` sidecar**, updated by raw writes in its
   own extent. §8.2
6. UDP port **7690**. §3.3
7. Python first, Go only if measurements show Python cannot pace 9 000 pps. §12

## 16. Repository layout (planned)

```
Makefile                 top level: fatfs → iop → ee (+ test, dist)
ee/                      main.c ui.c config.c rpc.c, Makefile (bin2c IRX)
iop/                     ps2xfer.irx
  smap/                  SMAP driver (from Neutrino, AFL-3.0)
  net/                   eth/arp/ipv4/udp/icmp
  proto/                 data RX fast path, window/ACK, control commands
  disk/                  extent writer, FatFs glue (diskio.c), state file
  bench/  log.c  rpc.c  main.c
pc/ps2xfer.py            client
tests/host/              host simulation + unit tests
tools/get_fatfs.sh       FatFs R0.16 download + ffconf (from the formatter, + FF_USE_EXPAND)
docs/DESIGN.md PROTOCOL.md BENCHMARKS.md
.github/workflows/build.yml   ps2dev/ps2dev docker image, artifacts, release zip
```
