# ps2xfer protocol (version 1)

UDP, default port **7690**. The PS2 is the server. `common/proto.h` is the
reference; this file describes the same thing in prose. All multi-byte fields
are **little-endian** (the IOP is little-endian; no byte swapping on the hot
path). Strings are UTF-8 and NUL-terminated. Paths are absolute (`/DVD/x.iso`)
and may not contain `\`, `:`, control characters, `.` or `..` components.

## 1. Messages

Byte 0 of every UDP payload is the message type:

| Type | Name | Direction |
|---:|---|---|
| 0x01 | HELLO | PC → PS2 (unicast or broadcast) |
| 0x02 | PING | PC → PS2 |
| 0x03 | SET | PC → PS2 |
| 0x04 | STATS | PC → PS2 |
| 0x05 | LOG | PC → PS2 |
| 0x06 | EXIT | PC → PS2 |
| 0x07 | ANNOUNCE | PS2 → 255.255.255.255:7690, every 2 s while no client is connected |
| 0x10 | DATA | PC → PS2 |
| 0x11 | ACK | PS2 → PC |
| 0x12 | READ_DATA | PS2 → PC (answers READ) |
| 0x20 | LIST | PC → PS2 |
| 0x21 | STAT | PC → PS2 |
| 0x22 | MKDIR | PC → PS2 |
| 0x23 | DELETE | PC → PS2 |
| 0x24 | RENAME | PC → PS2 |
| 0x25 | DF | PC → PS2 |
| 0x26 | OPEN_WRITE | PC → PS2 |
| 0x27 | CLOSE | PC → PS2 |
| 0x28 | ABORT | PC → PS2 |
| 0x29 | BLOCKSUMS | PC → PS2 |
| 0x2A | READ | PC → PS2 |
| 0x30 | BENCH | PC → PS2 |

## 2. Control requests and responses

Every type except DATA and ACK uses a 12-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | type |
| 1 | 1 | flags: 0x80 = response |
| 2 | 2 | status (response; 0 in requests) |
| 4 | 4 | req_id, chosen by the client and echoed in the response |
| 8 | 4 | session (from HELLO; 0 in HELLO and PING) |
| 12 | … | body |

* **Retries.** The client re-sends a request with the **same req_id** if no
  response arrives (0.5 s default). The PS2 keeps its last 4 responses and
  re-sends the cached one instead of running the command again. So DELETE,
  RENAME etc. run at most once.
* **Session.** HELLO creates a new session id; requests with another id get
  status 8 (ESESSION). One client at a time; a new HELLO takes over.
* Requests are processed one by one by the control thread. A slow command
  (BENCH, BLOCKSUMS, DF on a big disk) delays the others; the client uses longer
  timeouts for those.

### Status codes

| Code | Name | Meaning |
|---:|---|---|
| 0 | OK | |
| 1 | EINVAL | bad argument / path |
| 2 | ENOENT | no such file or directory |
| 3 | EEXIST | already exists (OPEN_WRITE without OVERWRITE) |
| 4 | ENOSPC | out of memory/space |
| 5 | ENOCONTIG | no contiguous free run large enough for the file |
| 6 | EIO | disk error |
| 7 | EBUSY | resource used by the active transfer |
| 8 | ESESSION | wrong session (say HELLO again) |
| 9 | EHANDLE | wrong transfer handle |
| 10 | ENOTEMPTY | directory not empty |
| 11 | ENOTSUP | not supported (in this mode) |
| 12 | EMISMATCH | manifest mismatch at CLOSE |
| 13 | ENODISK | no disk / no exFAT volume |
| 14 | EINCOMPLETE | CLOSE before all data is on disk |
| 100+n | – | FatFs `FRESULT` n |

### Bodies

| Type | Request body | Response body |
|---|---|---|
| HELLO | u32 client_proto, u32 nonce, u32 fattime (PC local time, FAT format; used for file timestamps) | see below (136 bytes) |
| PING | – | u32 uptime_ms |
| SET | `key=value\0` (keys of `ps2xfer.ini`; buffer geometry only while no transfer is open) | – |
| STATS | – | the 320-byte `px_stats_t` (`common/px_stats.h`); Python format `<4I8s15I44s64s20I48s` |
| LOG | u32 cursor (0 = oldest) | u32 next_cursor, u32 lost_bytes, text |
| EXIT | – | –; the PS2 closes an open transfer resumably and returns to the browser |
| LIST | u32 cursor, path | u32 next_cursor (0 = end), u16 count, u16 0, entries: u64 size, u32 fattime, u8 attr, u8 name_len, name |
| STAT | path | u64 size, u32 fattime, u8 attr, u8 contiguous (1 = one run), u16 0 |
| MKDIR | path (parents are created) | – |
| DELETE | path (file or empty directory) | – |
| RENAME | from, to (parents of `to` are created) | – |
| DF | – | u64 total_bytes, u64 free_bytes, u32 cluster_bytes |
| OPEN_WRITE | u64 size, u32 flags, u32 0, u8 fingerprint[32], path | u16 handle, u16 0, u64 start_offset, u64 credit, u32 buffer_bytes, u32 buffers, u64 start_lba |
| CLOSE | u16 handle, u16 0, u32 0, u64 manifest | u64 PS2 manifest (also with EMISMATCH) |
| ABORT | u16 handle, u8 delete | – |
| BLOCKSUMS | u64 offset (multiple of 1 MiB), u32 count, path | u32 n, u32 sum[n] (n ≤ 364) |
| READ | u64 offset, u32 length (≤ 256 KiB), path | u64 file size, u32 bytes read; the data was sent before as READ_DATA (§6a) |
| BENCH | u32 kind, u32 size_mb, u32 chunk, u32 flags (bit0: read back) | 112 bytes, see §6 |

HELLO response:

| Offset | Field | Offset | Field |
|---:|---|---:|---|
| 0 | u32 proto (1) | 40 | u32 link_mode (8 = 100FD, 4 = 100HD, \|0x40 PAUSE) |
| 4 | u32 session | 44 | u32 udma_mode (0xff unknown) |
| 8 | u32 ip (a.b.c.d = a<<24…) | 48 | u32 SPEED revision |
| 12 | u8 mac[6] | 52 | u32 IOP free memory |
| 18 | u16 port | 56 | u64 disk bytes |
| 20 | u32 payload (1408) | 64 | u32 cluster bytes |
| 24 | u32 unit (45056) | 68 | u32 fs total MiB |
| 28 | u32 buffer bytes | 72 | u32 fs free MiB (0xffffffff unknown) |
| 32 | u32 buffers | 76 | char model[44] |
| 36 | u32 state flags | 120 | char version[16] |

## 3. Writing a file

1. `OPEN_WRITE(size, flags, fingerprint, path)`. Flags:
   * 0x01 OVERWRITE: replace an existing `path`;
   * 0x02 RESUME: continue `path.part` if its state matches size and fingerprint;
   * 0x04 SINK, 0x08 SINK_NODMA: benches, no file (path empty);
   * 0x10 KEEP_PART: bench, CLOSE does not rename.

   The PS2 creates `path.part`, preallocates it with `f_expand` as **one
   contiguous run** (else ENOCONTIG), commits the metadata, creates
   `path.ps2x` (resume state) and answers with a handle, the start offset
   (0, or the resume point) and the first credit.
   *Fingerprint*: SHA-256 over (u64 size, u64 mtime, first MiB of the file).
2. The PC sends DATA packets for offsets ≥ start, paced, never at or beyond
   the latest credit.
3. The PS2 answers with ACKs. When an ACK has flag COMPLETE (base == size),
   all data is on the disk.
4. `CLOSE(handle, manifest)`. The PS2 compares the manifest, flushes the disk
   cache, deletes `path.ps2x` and renames `path.part` → `path`.

A new OPEN_WRITE while a transfer is open closes the old one resumably first.
ABORT with delete = 0 keeps `.part` + `.ps2x` for a later resume.

## 4. DATA (PC → PS2)

22-byte header, then the payload. Within the Ethernet frame the header starts
at offset 42, so every field is naturally aligned (offset 44 is 4-aligned) and
the payload starts at frame offset 64:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | type 0x10 |
| 1 | 1 | flags: 0x01 LAST, 0x02 RETRANSMIT |
| 2 | 8 | offset: file offset, multiple of 1408 |
| 10 | 4 | pkt_seq: +1 for every transmission (retransmits get a new one) |
| 14 | 4 | csum: word-Fletcher of the payload (§7) |
| 18 | 2 | handle |
| 20 | 2 | len: payload bytes, 1408 except in the last packet |
| 22 | len | payload, zero-padded to a multiple of 4 on the wire |

The PS2 drops DATA with a wrong handle, outside `[base, credit)`, or with an
unexpected length. Duplicates are skipped without reading the payload.

## 5. ACK (PS2 → PC)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | type 0x11 |
| 1 | 1 | flags: 0x01 COMPLETE, 0x02 ERROR |
| 2 | 2 | handle |
| 4 | 4 | max_pkt_seq: highest pkt_seq received |
| 8 | 8 | base: all bytes below are received **and written to disk** |
| 16 | 8 | credit: the PC may send offsets < credit (= base + buffers × buffer_bytes, capped at size) |
| 24 | 8 | durable: flushed and recorded in the resume state |
| 32 | 4 | rx_errors: SMAP receive errors incl. FIFO overruns (cumulative) |
| 36 | 4 | drops: DATA dropped (out of window, stale handle) |
| 40 | 4 | csum_errors |
| 44 | 2 | bitmap_bits |
| 46 | 2 | status: the error that stopped the session (with ERROR) |
| 48 | … | bitmap: bit i (LSB first) = packet at base + i × 1408 is in RAM |

When the PS2 sends an ACK: every `ack_every` (32) packets; when a buffer
completes or is written (credit grows); every `ack_ms` (2) ms while packets
are unacknowledged; at least every 20 ms while a transfer is open.

### Sender rules (PC)

* **Credit:** never send at or beyond `credit`.
* **Pacing:** bursts of `--burst` packets (2) at times from a high-resolution
  clock. The rate starts at `--rate-start` (9 MB/s).
* **Rate control (AIMD on the loss ratio):** every 100 ms the client compares
  packets detected as lost (below; plus growth of the PS2's `rx_errors` and
  `drops`) with packets sent in that interval. More than 10 % lost cuts the
  rate by 20 %. Less than 2 % raises it by 2 % (at least 0.05 MB/s), up to
  `--rate` (11.5 MB/s). In between, the rate is kept. Heavy loss from
  overflowing queues (switch, PCSX2, SMAP FIFO) slows the sender; rare random
  loss does not.
* **Loss detection (RACK-style):** a sent packet that is not acknowledged
  while `max_pkt_seq ≥ its pkt_seq + 3` is lost and is sent again before new
  data, inside the same pacing budget.
* **Timeout:** no progress for 100 ms (doubling to 1 s): re-send the oldest
  unacknowledged packet. No ACK at all for 15 s: give up (the transfer can be
  resumed).

## 6a. Reading files (READ / READ_DATA)

The PC asks for a range with READ. The PS2 reads it from the disk and sends it
as READ_DATA packets, then the READ response. READ_DATA has the DATA layout
(§4): offset = file offset, pkt_seq = the req_id of the READ, csum = 0,
handle = 0, len ≤ 1408, flag LAST on the final packet; the payload goes from
the read buffer to the SMAP TX FIFO by DMA. The client keeps 3 READs in
flight, re-asks contiguous runs of missing packets after the response, and repeats a
READ that got no answer within 0.25 s. READ is refused (EBUSY) while a transfer
to the PS2 is open (it uses the write buffers). The PS2 keeps the last file
open between READs and closes it before DELETE, RENAME and OPEN_WRITE.

## 6. BENCH

Kinds: 1 MICRO, 2 DISK_RAW (chunk = sectors per ATA command), 3 DISK_FATFS
(chunk = bytes per `f_write`), 4 DISK_IOMANX (chunk = bytes per `write()`,
only in `fs_iomanx=1` mode). Disk benches write only into
`/.ps2xfer/bench.bin`, preallocated with `f_expand` and deleted afterwards.

Response: u32 kind, u32 ticks_per_ms (36864 on the IOP), u64 bytes,
u64 write_ticks, u32 write_idle‰, u32 0, u64 read_ticks, u32 read_idle‰,
u32 nvals, u32 vals[16]. MICRO values: Fletcher ticks/KiB, memcpy ticks/KiB,
IOP total free, IOP max free block, 1000 SPEED register reads (ticks),
1000 `GetSystemTime` calls (ticks), SPEED revision, maximum DMA block.

## 7. Checksums

**word-Fletcher** over the payload as 32-bit little-endian words (zero-padded
to 4 bytes):

```
s1 = Σ w[i]                 mod 2^32
s2 = Σ (s1 after w[i])      mod 2^32
csum = s2 XOR rotl32(s1, 16)
```

In Python: `w = memoryview(buf).cast("I"); s1 = sum(w); s2 = sum(itertools.accumulate(w))`.

**Manifest:** the sum (mod 2^64) over all DATA packets of the session, each
counted once, of `(csum + (offset / 1408) × 0x9E3779B1) mod 2^32`. Both sides
compute it. A mismatch at CLOSE means a packet was lost or placed twice by a
bug, not by the network.

**BLOCKSUMS** use the same word-Fletcher over 1 MiB blocks of the file as
stored on the disk (the last block zero-padded to 4 bytes).

## 8. Discovery and other traffic

* A HELLO sent to 255.255.255.255:7690 is answered by unicast. The client then
  talks to that address. WSL2 in NAT mode does not pass this broadcast; use
  `--host` or mirrored networking.
* ANNOUNCE (20 bytes: u8 type, 3 × 0, u32 proto, u8 ip[4], u8 mac[6],
  u16 port) is broadcast from port 7690 every 2 s until a client says HELLO.
  It also makes PCSX2's *Sockets* network mode bind UDP port 7690 on the host,
  because PCSX2 only binds ports the PS2 has sent from.
* The PS2 answers ARP requests for its IP and ICMP echo (ping). It never sends
  ARP requests: replies go to the source MAC of the request.
* UDP checksums are not checked (Ethernet FCS protects the frame) and are sent
  as 0.
