/*
 * ps2xfer wire protocol (UDP). Shared by the IOP module and the host simulator.
 * docs/PROTOCOL.md describes the same thing in prose; pc/ps2xfer.py mirrors it.
 *
 * All multi-byte fields are little-endian. Every message starts with a 1-byte
 * type; bit 7 of `flags` marks a response.
 */
#ifndef PS2XFER_PROTO_H
#define PS2XFER_PROTO_H

#define PX_PROTO_VERSION 1
#define PX_DEFAULT_PORT  7690

/* Payload bytes per DATA packet: 11 x 128 (one SMAP DMA, 128-byte blocks). */
#define PX_PAYLOAD 1408
/* lcm(1408, 4096): buffer sizes are multiples of this (32 packets, 88 sectors). */
#define PX_UNIT    45056
#define PX_UNIT_PKTS 32

/* ---------------------------------------------------------------- types */
#define PX_T_HELLO      0x01
#define PX_T_PING       0x02
#define PX_T_SET        0x03
#define PX_T_STATS      0x04
#define PX_T_LOG        0x05
#define PX_T_EXIT       0x06
#define PX_T_ANNOUNCE   0x07 /* PS2 -> broadcast, every 2 s while no client is connected */

#define PX_T_DATA       0x10 /* PC -> PS2 */
#define PX_T_ACK        0x11 /* PS2 -> PC */
#define PX_T_READ_DATA  0x12 /* PS2 -> PC: file data answering READ (DATA layout) */

#define PX_T_LIST       0x20
#define PX_T_STAT       0x21
#define PX_T_MKDIR      0x22
#define PX_T_DELETE     0x23
#define PX_T_RENAME     0x24
#define PX_T_DF         0x25
#define PX_T_OPEN_WRITE 0x26
#define PX_T_CLOSE      0x27
#define PX_T_ABORT      0x28
#define PX_T_BLOCKSUMS  0x29
#define PX_T_DISKINFO   0x2B
#define PX_T_RESUME_INFO 0x2C /* closes an open transfer resumably; returns its durable offset */ /* read-only: raw sectors 0 and 1 + mounted volume start */
#define PX_T_READ       0x2A /* read a file range; data comes as READ_DATA */
#define PX_READ_MAX     (256 * 1024)

#define PX_T_BENCH      0x30

#define PX_F_RESPONSE 0x80

/* ---------------------------------------------------------------- status */
#define PX_OK          0
#define PX_EINVAL      1
#define PX_ENOENT      2
#define PX_EEXIST      3
#define PX_ENOSPC      4
#define PX_ENOCONTIG   5  /* no contiguous free run large enough */
#define PX_EIO         6
#define PX_EBUSY       7
#define PX_ESESSION    8
#define PX_EHANDLE     9
#define PX_ENOTEMPTY   10
#define PX_ENOTSUP     11
#define PX_EMISMATCH   12 /* manifest / checksum mismatch */
#define PX_ENODISK     13
#define PX_EINCOMPLETE 14 /* CLOSE before all data was written */
#define PX_EFS_BASE    100 /* + FatFs FRESULT */

/* ---------------------------------------------------------------- control header
 * 0  u8  type
 * 1  u8  flags        (PX_F_RESPONSE in responses)
 * 2  u16 status       (responses; 0 in requests)
 * 4  u32 req_id       (chosen by the client, echoed; retries reuse it)
 * 8  u32 session      (from HELLO; 0 for HELLO and PING)
 * 12 ... body
 */
#define PX_CTL_HDR 12

/* ---------------------------------------------------------------- DATA (22 bytes)
 * 0  u8  type = PX_T_DATA
 * 1  u8  flags        (PX_DF_*)
 * 2  u64 offset       (file offset, multiple of PX_PAYLOAD)
 * 10 u32 pkt_seq      (+1 per transmission, including retransmits)
 * 14 u32 csum         (px_fletcher over the payload, zero-padded to 4)
 * 18 u16 handle
 * 20 u16 len          (payload bytes; < PX_PAYLOAD only in the last packet)
 * 22 payload, padded with zeros to a multiple of 4 bytes on the wire
 *
 * In the Ethernet frame the header starts at offset 42, so every field is
 * naturally aligned relative to the frame start (offset 44 = 4-aligned).
 */
#define PX_DATA_HDR 22
#define PX_DF_LAST  0x01
#define PX_DF_RETX  0x02

/* ---------------------------------------------------------------- ACK
 * 0  u8  type = PX_T_ACK
 * 1  u8  flags        (PX_AF_*)
 * 2  u16 handle
 * 4  u32 max_pkt_seq  (highest pkt_seq seen in this session)
 * 8  u64 base         (everything below is received AND written to disk)
 * 16 u64 credit       (the PC may send offsets < credit)
 * 24 u64 durable      (flushed to disk and recorded in the resume state)
 * 32 u32 rx_errors    (SMAP RX errors / FIFO overruns, cumulative)
 * 36 u32 drops        (DATA dropped: stale handle, out of window)
 * 40 u32 csum_errors
 * 44 u16 bitmap_bits  (packets covered, starting at `base`)
 * 46 u16 status       (PX_OK, or the disk error that stopped the session)
 * 48 bitmap, bit i (LSB first) = packet at base + i * PX_PAYLOAD received
 */
#define PX_ACK_HDR 48
#define PX_AF_COMPLETE 0x01 /* base == file size */
#define PX_AF_ERROR    0x02 /* session stopped, see status */

/* OPEN_WRITE flags */
#define PX_OW_OVERWRITE 0x01 /* replace an existing final file */
#define PX_OW_RESUME    0x02 /* continue NAME.part if fingerprints match */
#define PX_OW_SINK      0x04 /* bench: receive, do not write (no file) */
#define PX_OW_SINK_NODMA 0x08 /* bench: receive headers only, skip payload */
#define PX_OW_KEEP_PART 0x10 /* bench: CLOSE does not rename */

/* BENCH kinds */
#define PX_BENCH_MICRO      1
#define PX_BENCH_DISK_RAW   2
#define PX_BENCH_DISK_FATFS 3
#define PX_BENCH_DISK_IOMANX 4
#define PX_BENCH_FSCHECK    5 /* fs_iomanx mode: list mass0:DVD through bdmfs_fatfs, read sector 16 of each ISO */

/* word-Fletcher over 32-bit little-endian words:
 *   s1 = sum(w) mod 2^32;  s2 = sum of running s1 values mod 2^32
 *   csum = s2 ^ rotl32(s1, 16)
 * Python: w = memoryview(buf).cast('I'); s1 = sum(w); s2 = sum(accumulate(w)) */

/* Manifest term of one packet: csum + (offset / PX_PAYLOAD) * 0x9E3779B1, mod 2^32;
 * the manifest is the u64 sum of the terms of all packets of the file. */
#define PX_MANIFEST_MUL 0x9E3779B1u

#endif
