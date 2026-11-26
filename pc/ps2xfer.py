#!/usr/bin/env python3
"""ps2xfer - send files from a PC to the exFAT HDD of a PS2 running ps2xfer.elf.

Single file, Python >= 3.10, standard library only. Works on Windows, Linux
and WSL (use WSL in mirrored networking mode, or run it natively on Windows:
WSL2 NAT does not pass broadcast discovery).

Protocol: docs/PROTOCOL.md (common/proto.h is the reference).
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import itertools
import mmap
import os
import random
import select
import socket
import struct
import sys
import time

VERSION = "0.1.0"
PROTO_VERSION = 1
DEFAULT_PORT = 7690
PAYLOAD = 1408
WIRE_OVERHEAD = 88          # preamble..IFG + Eth/IP/UDP + our 22-byte header + FCS
M32 = 0xFFFFFFFF

T_HELLO, T_PING, T_SET, T_STATS, T_LOG, T_EXIT = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
T_DATA, T_ACK, T_READ_DATA = 0x10, 0x11, 0x12
T_LIST, T_STAT, T_MKDIR, T_DELETE, T_RENAME, T_DF = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
T_OPEN_WRITE, T_CLOSE, T_ABORT, T_BLOCKSUMS = 0x26, 0x27, 0x28, 0x29
T_READ = 0x2A
T_DISKINFO = 0x2B
T_RESUME_INFO = 0x2C
READ_MAX = 256 * 1024
T_BENCH = 0x30
F_RESPONSE = 0x80

DF_LAST, DF_RETX = 0x01, 0x02
AF_COMPLETE, AF_ERROR = 0x01, 0x02
OW_OVERWRITE, OW_RESUME, OW_SINK, OW_SINK_NODMA, OW_KEEP_PART = 0x01, 0x02, 0x04, 0x08, 0x10
BENCH_MICRO, BENCH_DISK_RAW, BENCH_DISK_FATFS, BENCH_DISK_IOMANX = 1, 2, 3, 4
MANIFEST_MUL = 0x9E3779B1

STATUS = {
    0: "OK", 1: "invalid argument", 2: "no such file or directory", 3: "already exists",
    4: "no space", 5: "no contiguous free space large enough (free space or defragment)",
    6: "I/O error", 7: "busy", 8: "bad session", 9: "bad handle", 10: "directory not empty",
    11: "not supported", 12: "checksum/manifest mismatch", 13: "no disk / no exFAT volume",
    14: "transfer incomplete",
}
EBUSY = 7

CTL = struct.Struct("<BBHII")            # type, flags, status, req_id, session
DATA = struct.Struct("<BBQIIHH")         # type, flags, offset, pkt_seq, csum, handle, len
ACK = struct.Struct("<BBHIQQQIIIHH")     # see common/proto.h
STATS = struct.Struct("<4I8s15I44s64s28I48s")
STATS_FIELDS = (
    "version flags uptime_ms ip mac port link_mode speed_rev dma_block iop_free iop_maxfree "
    "nbuf bufsize cpu_load_x10 udma_mode disk_sectors_lo disk_sectors_hi fs_total_mb fs_free_mb "
    "cluster_size disk_model file file_size_lo file_size_hi written_lo written_hi rx_bytes_lo "
    "rx_bytes_hi rx_pkts rx_dups rx_drops rx_retx rx_errors rx_overruns csum_errors ctl_drops "
    "acks_sent disk_errors prof_hdr prof_dma prof_total prof_samples prof_smap16 prof_smap_wakes "
    "prof_writer16 prof_writer_slots prof_ack16 prof_ack_runs wr_phase wr_lba last_error").split()

CD_LIMIT = 700 * 1024 * 1024
OPL_NAME_MAX = 160


class PxError(Exception):
    pass


# GUI hooks: progress callback (label, done, size, cur_Bps, avg_Bps, eta_s, retx)
# replaces the console progress line; CANCEL aborts the running operation.
PROGRESS_HOOK = None
CANCEL = None   # threading.Event, set by the GUI


def _check_cancel():
    if CANCEL is not None and CANCEL.is_set():
        raise PxError("cancelled")


def status_text(st: int) -> str:
    if st >= 100:
        return f"FatFs error {st - 100}"
    return STATUS.get(st, f"error {st}")


# --------------------------------------------------------------- checksums

def fletcher(buf) -> int:
    """word-Fletcher of common/proto.h; buf length is padded to 4 with zeros."""
    if len(buf) & 3:
        buf = bytes(buf) + b"\0" * (4 - (len(buf) & 3))
    w = memoryview(buf).cast("I")
    s1 = sum(w) & M32
    s2 = sum(itertools.accumulate(w)) & M32
    return s2 ^ (((s1 << 16) | (s1 >> 16)) & M32)


def fletcher_blocks(path: str, offset: int, count: int, block: int = 1 << 20) -> list[int]:
    out = []
    with open(path, "rb") as f:
        f.seek(offset)
        for _ in range(count):
            b = f.read(block)
            if not b:
                break
            out.append(fletcher(b))
    return out


def fattime_now() -> int:
    t = time.localtime()
    return (((t.tm_year - 1980) & 0x7F) << 25 | t.tm_mon << 21 | t.tm_mday << 16 |
            t.tm_hour << 11 | t.tm_min << 5 | (t.tm_sec // 2))


def fmt_size(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return str(n)


def link_text(mode: int) -> str:
    if mode == 0:
        return "no link"
    base = {8: "100 Mbit full duplex", 4: "100 Mbit HALF duplex", 2: "10 Mbit full duplex",
            1: "10 Mbit half duplex"}.get(mode & 0x0F, f"mode {mode:#x}")
    return base + (", PAUSE" if mode & 0x40 else "")


# --------------------------------------------------------------- connection

class Conn:
    def __init__(self, host: str | None, port: int, timeout: float = 0.5, retries: int = 6):
        self.port = port
        self.timeout = timeout
        self.retries = retries
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind(("0.0.0.0", 0))
        self.req_id = random.randint(1, 1 << 30)
        self.session = 0
        self.info: dict = {}
        self.host = host
        self.addr = (host, port) if host else None
        self.ack_handler = None   # called with raw ACK packets that arrive during requests

    # -- low level
    def _next_id(self) -> int:
        self.req_id = (self.req_id + 1) & M32 or 1
        return self.req_id

    def request(self, typ: int, body: bytes = b"", timeout: float | None = None,
                retries: int | None = None, addr=None) -> tuple[int, bytes]:
        rid = self._next_id()
        pkt = CTL.pack(typ, 0, 0, rid, self.session if typ not in (T_HELLO, T_PING) else 0) + body
        timeout = self.timeout if timeout is None else timeout
        retries = self.retries if retries is None else retries
        dest = addr or self.addr
        for _ in range(retries):
            self.sock.sendto(pkt, dest)
            deadline = time.monotonic() + timeout
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                r, _, _ = select.select([self.sock], [], [], left)
                if not r:
                    break
                data, src = self.sock.recvfrom(4096)
                if len(data) < 1:
                    continue
                if data[0] == T_ACK:
                    if self.ack_handler:
                        self.ack_handler(data)
                    continue
                if len(data) < CTL.size:
                    continue
                t, fl, st, r_id, sess = CTL.unpack_from(data)
                if t == typ and fl & F_RESPONSE and r_id == rid:
                    if typ in (T_HELLO, T_PING):
                        self.addr = src
                        self.host = src[0]
                    return st, data[CTL.size:]
        raise PxError(f"no response from {dest[0]}:{dest[1]} (request type {typ:#x})")

    def call(self, typ: int, body: bytes = b"", **kw) -> bytes:
        st, resp = self.request(typ, body, **kw)
        if st != 0:
            raise PxError(status_text(st))
        return resp

    # -- commands
    def discover(self):
        """Find the PS2 by broadcast without starting a session."""
        if self.addr is None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            self.request(T_PING, addr=("255.255.255.255", self.port), retries=4)

    def hello(self) -> dict:
        body = struct.pack("<III", PROTO_VERSION, random.getrandbits(32), fattime_now())
        self.discover()
        st, r = self.request(T_HELLO, body)
        if st != 0 or len(r) < 136:
            raise PxError("bad HELLO response")
        f = struct.unpack_from("<III6sH9IQIII", r)
        info = dict(zip("proto session ip mac port payload unit bufsize nbuf flags link udma "
                        "speed_rev iop_free disk_bytes cluster fs_total_mb fs_free_mb".split(), f))
        info["model"] = r[76:120].split(b"\0")[0].decode(errors="replace")
        info["version"] = r[120:136].split(b"\0")[0].decode(errors="replace")
        self.session = info["session"]
        if info["proto"] != PROTO_VERSION:
            raise PxError(f"protocol version {info['proto']} on the PS2, {PROTO_VERSION} here")
        if info["payload"] != PAYLOAD:
            raise PxError("unexpected payload size")
        self.info = info
        return info

    def call_unless_busy(self, typ: int, body: bytes = b"", wait: float = 8.0, **kw) -> bytes:
        """For requests that take over the write slot (RESUME_INFO, OPEN_WRITE):
        the PS2 answers EBUSY while another client's file is streaming in, and
        for 2 s after our own sender stopped (Ctrl+C and an immediate restart)."""
        deadline = time.monotonic() + wait
        while True:
            st, r = self.request(typ, body, **kw)
            if st != EBUSY or time.monotonic() > deadline:
                break
            time.sleep(0.5)
        if st == EBUSY:
            raise PxError("the PS2 is receiving a file from another client; "
                          "wait until it finishes or stop that client")
        if st != 0:
            raise PxError(status_text(st))
        return r

    def stats(self) -> dict:
        r = self.call(T_STATS)
        d = dict(zip(STATS_FIELDS, STATS.unpack_from(r)))
        for k in ("disk_model", "file", "last_error"):
            d[k] = d[k].split(b"\0")[0].decode(errors="replace")
        return d

    @staticmethod
    def path(p: str) -> bytes:
        return p.encode("utf-8") + b"\0"


MTU_HINT = ("data packets need a 1500-byte MTU on the way to the PS2 (no VPN, no WSL2 "
            "NAT network: run the Windows client 'py ps2xfer.py', or set "
            "networkingMode=mirrored in .wslconfig)")
NEED_MTU = 20 + 8 + DATA.size + PAYLOAD  # IP + UDP + DATA header + payload = 1458


def path_mtu(addr) -> int | None:
    """MTU of the route to addr (Linux/WSL only; None elsewhere)."""
    if not sys.platform.startswith("linux"):
        return None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(addr)
            return s.getsockopt(socket.IPPROTO_IP, getattr(socket, "IP_MTU", 14))
        finally:
            s.close()
    except OSError:
        return None


def connect(args, observe: bool = False) -> Conn:
    """observe: only PING/STATS/LOG follow; no session, a running transfer
    is not disturbed."""
    c = Conn(args.host, args.port)
    if observe:
        c.discover()
        return c
    info = c.hello()
    mtu = path_mtu(c.addr)
    if mtu is not None and mtu < NEED_MTU:
        print(f"WARNING: the route to the PS2 has MTU {mtu}, transfers need {NEED_MTU}: {MTU_HINT}",
              file=sys.stderr)
    if not args.quiet:
        ip = socket.inet_ntoa(struct.pack(">I", info["ip"]))
        print(f"PS2 {ip} ({c.host}) ps2xfer {info['version']}, link: {link_text(info['link'])}")
        if info["link"] & 0x0F not in (8,):
            print("WARNING: the link is not 100 Mbit full duplex; transfers will be slow.")
    return c


# --------------------------------------------------------------- sender

class Source:
    """Payload source: a file (mmap) or synthetic data for benches."""

    def __init__(self, path: str | None, size: int):
        self.size = size
        self.f = None
        self.mm = None
        if path is not None and size > 0:
            self.f = open(path, "rb")
            self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
            self.view = memoryview(self.mm)
        else:
            pat = bytearray(PAYLOAD * 64)
            for i in range(0, len(pat), 4):
                struct.pack_into("<I", pat, i, (i * 2654435761) & M32)
            self.pattern = bytes(pat)

    def chunk(self, idx: int):
        off = idx * PAYLOAD
        n = min(PAYLOAD, self.size - off)
        if self.mm is not None:
            return self.view[off:off + n]
        k = (idx % 64) * PAYLOAD
        return memoryview(self.pattern)[k:k + n]

    def close(self):
        if self.mm is not None:
            self.view.release()
            self.mm.close()
            self.f.close()


class Sender:
    def __init__(self, conn: Conn, handle: int, src: Source, start: int, credit: int,
                 rate_max: float, rate_start: float, burst: int, window: int, quiet=False,
                 label=""):
        self.c = conn
        self.sock = conn.sock
        self.addr = conn.addr
        self.handle = handle
        self.src = src
        self.size = src.size
        self.npkts = (self.size + PAYLOAD - 1) // PAYLOAD
        self.next_new = start // PAYLOAD
        self.base_pkt = start // PAYLOAD
        self.start = start
        self.credit = credit
        self.rate_max = rate_max * 1e6
        self.rate = min(rate_start, rate_max) * 1e6
        self.burst = max(1, burst)
        self.window = window
        self.quiet = quiet
        self.label = label
        self.seq = 0
        self.inflight = collections.deque()     # (seq, idx) in send order
        self.retx_q = collections.deque()
        self.ack_bitmap = 0
        self.ack_base_pkt = self.base_pkt
        self.max_seen = 0
        self.complete = False
        self.error = 0
        self.manifest = 0
        self.retransmits = 0
        self.last_progress = time.monotonic()
        self.last_advance = time.monotonic()  # the PS2 took new data (not reset by timeouts)
        self.last_ack_time = time.monotonic()
        self.rto = 0.1
        self.rx_errors = None
        self.drops = None
        self.last_rate_up = time.monotonic()
        self.sent_win = 0
        self.lost_win = 0
        self.credit_wait = 0.0   # seconds with nothing to send because of credit
        self.acks = 0
        self.buf = bytearray(DATA.size + PAYLOAD + 4)
        self.bufview = memoryview(self.buf)
        self.durable = start
        self.ps2_rx_errors = 0
        self.ps2_csum_errors = 0

    # ---- packets
    def _send(self, idx: int, retx: bool):
        data = self.src.chunk(idx)
        n = len(data)
        wl = (n + 3) & ~3
        csum = fletcher(data)
        self.seq = (self.seq + 1) & M32
        flags = (DF_RETX if retx else 0) | (DF_LAST if idx == self.npkts - 1 else 0)
        DATA.pack_into(self.buf, 0, T_DATA, flags, idx * PAYLOAD, self.seq, csum, self.handle, n)
        self.buf[DATA.size:DATA.size + n] = data
        if wl != n:
            self.buf[DATA.size + n:DATA.size + wl] = b"\0" * (wl - n)
        try:
            self.sock.sendto(self.bufview[:DATA.size + wl], self.addr)
        except (BlockingIOError, OSError):
            pass  # kernel buffer full: treated as a loss, RACK/RTO resend it
        self.inflight.append((self.seq, idx))
        self.sent_win += 1
        if not retx:
            self.manifest = (self.manifest + ((csum + idx * MANIFEST_MUL) & M32)) & 0xFFFFFFFFFFFFFFFF
        else:
            self.retransmits += 1
        return n

    def _acked(self, idx: int) -> bool:
        if idx < self.ack_base_pkt:
            return True
        return (self.ack_bitmap >> (idx - self.ack_base_pkt)) & 1 == 1

    def on_ack(self, data: bytes):
        if len(data) < ACK.size:
            return
        (t, flags, handle, max_seq, base, credit, durable, rx_err, drops, csum_err, bits,
         status) = ACK.unpack_from(data)
        if handle != self.handle:
            return
        self.acks += 1
        self.last_ack_time = time.monotonic()
        nbytes = (bits + 7) // 8
        bm = int.from_bytes(data[ACK.size:ACK.size + nbytes], "little")
        base_pkt = base // PAYLOAD if base < self.size else self.npkts
        if base_pkt > self.ack_base_pkt or bm != self.ack_bitmap:
            self.last_progress = self.last_advance = time.monotonic()
            self.rto = 0.1
        self.ack_base_pkt = base_pkt
        self.ack_bitmap = bm
        self.base_pkt = base_pkt
        self.credit = max(self.credit, credit)
        self.durable = durable
        if (max_seq - self.max_seen) & 0x80000000 == 0:
            self.max_seen = max_seq
        self.ps2_csum_errors = csum_err
        if flags & AF_ERROR:
            self.error = status or 6
        if flags & AF_COMPLETE:
            self.complete = True
        # PS2-side receive errors / drops count as congestion too
        if self.rx_errors is not None and (rx_err > self.rx_errors or drops > self.drops):
            self._on_loss((rx_err - self.rx_errors) + (drops - self.drops))
        self.rx_errors, self.drops = rx_err, drops
        self.ps2_rx_errors = rx_err
        # RACK-style loss detection, walking the send-order queue
        while self.inflight:
            seq, idx = self.inflight[0]
            if self._acked(idx):
                self.inflight.popleft()
                continue
            if ((self.max_seen - seq) & M32) < 0x80000000 and ((self.max_seen - seq) & M32) >= 3:
                self.inflight.popleft()
                self.retx_q.append(idx)
                self._on_loss()
                continue
            break

    # ---- rate control: AIMD on the loss ratio of each 100 ms interval.
    # Heavy loss (queues overflowing: switch, PCSX2, SMAP FIFO) cuts the rate;
    # a few random losses only stop it from growing.
    RATE_MIN = 0.3e6
    LOSS_CUT = 0.10      # > 10 % lost in an interval: rate x 0.8
    LOSS_GROW = 0.02     # < 2 %: rate + 2 %

    def _on_loss(self, n: int = 1):
        self.lost_win += n

    def _rate_tick(self, now: float):
        if now - self.last_rate_up < 0.1:
            return
        self.last_rate_up = now
        sent, lost = self.sent_win, self.lost_win
        self.sent_win = self.lost_win = 0
        if sent == 0:
            return
        ratio = lost / sent
        if ratio > self.LOSS_CUT:
            self.rate = max(self.RATE_MIN, self.rate * 0.8)
        elif ratio < self.LOSS_GROW:
            self.rate = min(self.rate_max, self.rate + max(0.05e6, self.rate * 0.02))

    def _poll_acks(self, timeout: float):
        r, _, _ = select.select([self.sock], [], [], timeout)
        while r:
            try:
                data, _ = self.sock.recvfrom(4096)
            except (BlockingIOError, InterruptedError):
                break
            if data and data[0] == T_ACK:
                self.on_ack(data)
            r, _, _ = select.select([self.sock], [], [], 0)

    def _next_packet(self):
        while self.retx_q:
            idx = self.retx_q.popleft()
            if not self._acked(idx):
                return idx, True
        if self.next_new < self.npkts and self.next_new * PAYLOAD < self.credit and \
                len(self.inflight) < self.window:
            idx = self.next_new
            self.next_new += 1
            return idx, False
        return None, False

    # ---- main loop
    def run(self) -> None:
        t0 = time.perf_counter()
        next_t = t0
        last_print = 0.0
        prev_bytes, prev_t = self.start, t0
        cur_rate = 0.0
        while not self.complete:
            _check_cancel()
            if self.error:
                raise PxError(f"PS2 stopped the transfer: {status_text(self.error)}")
            now = time.perf_counter()
            if now >= next_t:
                sent = 0
                for _ in range(self.burst):
                    idx, retx = self._next_packet()
                    if idx is None:
                        break
                    n = self._send(idx, retx)
                    sent += n + WIRE_OVERHEAD
                if sent:
                    next_t += sent * PAYLOAD / (PAYLOAD + WIRE_OVERHEAD) / self.rate
                    if next_t < now - 0.002:
                        next_t = now
                    self._poll_acks(0)
                else:
                    next_t = now
                    blocked = self.next_new < self.npkts and self.next_new * PAYLOAD >= self.credit
                    t_w = time.perf_counter()
                    self._poll_acks(0.002)
                    if blocked:
                        self.credit_wait += time.perf_counter() - t_w
            else:
                left = next_t - now
                self._poll_acks(left - 0.0015 if left > 0.002 else 0)
            mono = time.monotonic()
            # retransmission timeout: probe with the oldest unacknowledged packet
            if self.inflight and mono - self.last_progress > self.rto:
                seq, idx = self.inflight.popleft()
                if not self._acked(idx):
                    self.retx_q.appendleft(idx)
                self.rto = min(1.0, self.rto * 2)
                self.last_progress = mono
            if mono - self.last_ack_time > 15:
                raise PxError("PS2 does not answer for 15 s (cable? console?); "
                              "run the same send again to resume")
            if mono - self.last_advance > 15 and self.inflight:
                raise PxError("the PS2 answers but takes no data for 15 s. Large UDP packets "
                              "do not reach it: " + MTU_HINT)
            self._rate_tick(mono)
            if not self.quiet and now - last_print >= 0.5:
                done = min(self.size, self.base_pkt * PAYLOAD)
                if now - prev_t >= 1.0:
                    cur_rate = (done - prev_bytes) / (now - prev_t)
                    prev_bytes, prev_t = done, now
                self._print(done, now - t0, cur_rate)
                last_print = now
        self.elapsed = time.perf_counter() - t0
        if not self.quiet:
            self._print(self.size, self.elapsed, cur_rate, final=True)

    def _print(self, done, elapsed, cur, final=False):
        avg = (done - self.start) / elapsed if elapsed > 0 else 0
        pct = 100.0 * done / self.size if self.size else 100.0
        eta = (self.size - done) / avg if avg > 0 else 0
        if PROGRESS_HOOK is not None:
            PROGRESS_HOOK(self.label, done, self.size, cur, avg, eta, self.retransmits)
            return
        line = (f"\r{self.label} {pct:5.1f}%  {fmt_size(done)}/{fmt_size(self.size)}  "
                f"{cur / 1e6:5.2f} MB/s (avg {avg / 1e6:5.2f})  ETA {int(eta) // 60}:{int(eta) % 60:02d}  "
                f"retx {self.retransmits}  rate {self.rate / 1e6:.1f}")
        sys.stdout.write(line + ("\n" if final else ""))
        sys.stdout.flush()


def fingerprint(path: str) -> bytes:
    st = os.stat(path)
    h = hashlib.sha256()
    h.update(struct.pack("<QQ", st.st_size, int(st.st_mtime)))
    with open(path, "rb") as f:
        h.update(f.read(1 << 20))
    return h.digest()


RESUME_CHECK_MB = 32


def check_resume_tail(c: Conn, src: str, size: int, dest: str, fp: bytes, quiet: bool) -> int:
    """Before resuming: compares the last (up to 32) whole MiB below the PS2's
    durable offset with the local file. Returns the resume limit in MiB,
    0 when there is nothing to resume, -1 when the start of the .part is
    already wrong (then the file is sent again from the beginning)."""
    r = c.call_unless_busy(T_RESUME_INFO, struct.pack("<QII", size, 0, 0) + fp + Conn.path(dest), timeout=30.0)
    durable = struct.unpack_from("<Q", r)[0]
    end_mb = durable >> 20
    if end_mb == 0:
        return 0
    first_mb = max(0, end_mb - RESUME_CHECK_MB)
    rr = c.call(T_BLOCKSUMS, struct.pack("<QI", first_mb << 20, end_mb - first_mb) + Conn.path(dest + ".part"),
                timeout=120.0, retries=2)
    cnt = struct.unpack_from("<I", rr)[0]
    remote = struct.unpack_from(f"<{cnt}I", rr, 4)
    local = fletcher_blocks(src, first_mb << 20, cnt)
    for k, (a, b) in enumerate(zip(local, remote)):
        if a != b:
            if not quiet:
                print(f"resume check: MiB block {first_mb + k} differs, resuming from there")
            return first_mb + k if first_mb + k > 0 else -1
    if not quiet:
        print(f"resume check: last {cnt} MiB before {fmt_size(durable)} match")
    return end_mb


def transfer(c: Conn, args, src_path: str | None, size: int, dest: str | None, flags: int,
             fp: bytes = b"\0" * 32, label: str = "") -> dict:
    limit_mb = 0
    if flags & OW_RESUME and src_path and dest:
        limit_mb = check_resume_tail(c, src_path, size, dest, fp, args.quiet)
        if limit_mb < 0:
            flags &= ~OW_RESUME
            limit_mb = 0
    body = struct.pack("<QII", size, flags, limit_mb) + fp + (Conn.path(dest) if dest else b"\0")
    r = c.call_unless_busy(T_OPEN_WRITE, body, timeout=30.0, retries=3)
    handle, _, start, credit, bufsize, nbuf, lba = struct.unpack_from("<HHQQIIQ", r)
    if start and not args.quiet:
        print(f"resuming at {fmt_size(start)}")
    src = Source(src_path, size)
    s = Sender(c, handle, src, start, credit, args.rate, args.rate_start, args.burst,
               args.window, args.quiet, label)
    c.ack_handler = s.on_ack
    try:
        s.run()
        st, r = c.request(T_CLOSE, struct.pack("<HHIQ", handle, 0, 0, s.manifest), timeout=10.0)
        if st != 0:
            extra = ""
            if len(r) >= 8:
                extra = f" (PC manifest {s.manifest:#x}, PS2 {struct.unpack_from('<Q', r)[0]:#x})"
            raise PxError(f"CLOSE failed: {status_text(st)}{extra}")
    except (KeyboardInterrupt, PxError):
        try:   # keep NAME.part: the next send of the same file resumes it
            c.request(T_ABORT, struct.pack("<HB", handle, 0), retries=2)
        except PxError:
            pass
        raise
    finally:
        c.ack_handler = None
        src.close()
    return {"bytes": size - start, "seconds": s.elapsed, "retransmits": s.retransmits,
            "credit_wait": s.credit_wait,
            "csum_errors": s.ps2_csum_errors, "rx_errors": s.ps2_rx_errors}


# --------------------------------------------------------------- commands

def opl_dest(path: str, size: int, dest: str | None) -> str:
    name = os.path.basename(path)
    if dest:
        if dest.endswith("/"):
            return dest + name
        return dest
    ext = os.path.splitext(name)[1].lower()
    if ext in (".iso", ".zso"):
        return ("/CD/" if size < CD_LIMIT else "/DVD/") + name
    return "/" + name


def opl_warnings(dest: str) -> list[str]:
    w = []
    name = dest.rsplit("/", 1)[-1]
    stem, ext = os.path.splitext(name)
    if ext.lower() not in (".iso", ".zso"):
        return w
    if not all(32 <= ord(ch) < 127 for ch in name):
        w.append("non-ASCII characters in the name (OPL may not show it correctly)")
    if len(stem) > OPL_NAME_MAX:
        w.append(f"name longer than {OPL_NAME_MAX} characters: OPL will skip it")
    return w


def plan_files(paths: list[str], dest: str | None) -> list[tuple[str, str, int]]:
    """(local path, PS2 path, size).

    A folder's CONTENTS go straight into --dest (no extra folder level), e.g.
    `send D:\\ps2iso --dest /DVD/` puts the games directly into /DVD, where
    OPL looks. Without --dest, .iso/.zso files of a folder go flat into /CD or
    /DVD by size and other files keep the folder structure under /<folder>."""
    if dest and not dest.startswith("/"):
        dest = "/" + dest
    if dest and (len(paths) > 1 or os.path.isdir(paths[0])) and not dest.endswith("/"):
        dest += "/"
    out = []
    for p in paths:
        if os.path.isdir(p):
            root = os.path.basename(os.path.normpath(p))
            for dirpath, _, files in os.walk(p):
                for fn in sorted(files):
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, p).replace(os.sep, "/")
                    size = os.path.getsize(full)
                    if dest:
                        out.append((full, dest + rel, size))
                    elif os.path.splitext(fn)[1].lower() in (".iso", ".zso"):
                        out.append((full, opl_dest(full, size, None), size))
                    else:
                        out.append((full, "/" + root + "/" + rel, size))
        elif os.path.isfile(p):
            size = os.path.getsize(p)
            out.append((p, opl_dest(p, size, dest), size))
        else:
            raise PxError(f"not found: {p}")
    return out


def send_planned(c: Conn, args, files: list[tuple[str, str, int]], log=print, stage=None) -> None:
    """Sends planned files (see plan_files): OPL names, verify, art.
    stage(src, ps2_path, what): "send", "verify", "art", "done" (for the window)."""
    stage = stage or (lambda *a: None)
    for i, (src, dst, size) in enumerate(files, 1):
        _check_cancel()
        for w in opl_warnings(dst):
            log(f"WARNING {dst}: {w}")
        if size == 0:
            log(f"skipping empty file {src}")
            continue
        if not args.keep_name and dst.lower().endswith(".iso"):
            dst = opl_old_name(dst, local_game_id(src))
        flags = OW_RESUME | (OW_OVERWRITE if args.overwrite else 0)
        label = f"[{i}/{len(files)}] {dst}"
        stage(src, dst, "send")
        r = transfer(c, args, src, size, dst, flags, fingerprint(src), label)
        log(f"    {dst}: {fmt_size(r['bytes'])} in {r['seconds']:.1f} s = "
            f"{r['bytes'] / r['seconds'] / 1e6 if r['seconds'] else 0:.2f} MB/s, "
            f"{r['retransmits']} retransmits")
        if args.verify:
            stage(src, dst, "verify")
            (verify_file_full if args.verify_full else verify_file)(c, src, dst)
        if not args.no_art and dst.lower().endswith((".iso", ".zso")):
            name = dst.rsplit("/", 1)[-1]
            gid = name[:11].upper() if is_opl_old_name(name) else local_game_id(src)
            if gid:
                stage(src, dst, "art")
                got = art_for_game(c, args, gid)
                log(f"    art: {', '.join(got) if got else 'none found / already there'}")
        stage(src, dst, "done")


def cmd_send(args):
    files = plan_files(args.files, args.dest)
    total = sum(f[2] for f in files)
    c = connect(args)
    print(f"{len(files)} file(s), {fmt_size(total)}")
    send_planned(c, args, files)


OLD_NAME = None  # compiled lazily


def is_opl_old_name(name: str) -> bool:
    """OPL's old ISO naming: SLUS_205.52.Name.iso (game ID taken from the name)."""
    import re
    global OLD_NAME
    if OLD_NAME is None:
        OLD_NAME = re.compile(r"^[A-Z]{4}_\d{3}\.\d{2}\..+\.(iso|zso)$", re.IGNORECASE)
    return bool(OLD_NAME.match(name))


def iso_game_id(read) -> str | None:
    """Game ID (e.g. SLUS_205.52) from SYSTEM.CNF of an ISO9660 image.
    read(offset, length) -> bytes."""
    pvd = read(16 * 2048, 2048)
    if pvd[1:6] != b"CD001":
        return None
    lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    data = read(lba * 2048, min(size, 64 * 1024))
    pos = 0
    while pos < len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        nl = data[pos + 32]
        name = data[pos + 33:pos + 33 + nl].upper()
        if name.startswith(b"SYSTEM.CNF"):
            el, es = struct.unpack_from("<I", data, pos + 2)[0], struct.unpack_from("<I", data, pos + 10)[0]
            cnf = read(el * 2048, min(es, 2048)).decode("ascii", "replace")
            for line in cnf.splitlines():
                if line.replace(" ", "").upper().startswith("BOOT2="):
                    exe = line.split("=", 1)[1].strip().split("\\")[-1].split(":")[-1].split(";")[0]
                    if len(exe) == 11 and exe[4] == "_" and exe[8] == ".":
                        return exe.upper()
            return None
        pos += ln
    return None


def local_game_id(path: str) -> str | None:
    if not path.lower().endswith(".iso"):
        return None
    with open(path, "rb") as f:
        def rd(off, n):
            f.seek(off)
            return f.read(n)
        try:
            return iso_game_id(rd)
        except (struct.error, IndexError):
            return None


def remote_game_id(c: Conn, path: str) -> str | None:
    def rd(off, n):
        buf = bytearray()
        Downloader(c, path, buf.extend, start=off, size=off + n, quiet=True).run()
        return bytes(buf)
    try:
        return iso_game_id(rd)
    except (struct.error, IndexError, PxError):
        return None


def opl_old_name(dest: str, gid: str | None) -> str:
    folder, name = dest.rsplit("/", 1)
    if gid is None or is_opl_old_name(name):
        return dest
    return f"{folder}/{gid}.{name}"


def opl_needs_name(name: str) -> bool:
    """An ISO whose name OPL 1.2.0-Beta-2271 does not list from the HDD."""
    return name.lower().endswith(".iso") and not is_opl_old_name(name)


def opl_fix_names(c: Conn, folders=("/CD", "/DVD"), dry_run: bool = False, log=print,
                  skip: set | None = None) -> list[tuple[str, str]]:
    """Renames ISOs to OPL's old format SLUS_205.52.Name.iso (game ID from
    SYSTEM.CNF inside the image). skip: PS2 paths known to have no game ID;
    new ones are added. Returns [(old, new)]."""
    done = []
    for folder in folders:
        for n in ps2_listdir(c, folder):
            path = f"{folder}/{n}"
            if not opl_needs_name(n) or (skip is not None and path in skip):
                continue
            gid = remote_game_id(c, path)
            if gid is None:
                log(f"  {path}: no game ID found, left as is")
                if skip is not None:
                    skip.add(path)
                continue
            new = f"{folder}/{gid}.{n}"
            if dry_run:
                log(f"  would rename {path} -> {new}")
                continue
            try:
                c.call(T_RENAME, Conn.path(path) + Conn.path(new))
            except PxError as e:
                log(f"  {path}: {e}")
                if skip is not None:
                    skip.add(path)
                continue
            log(f"  {path} -> {new}")
            done.append((path, new))
    return done


def cmd_opl_rename(args):
    """Renames ISOs in /CD and /DVD to OPL's old format SLUS_205.52.Name.iso.
    Works around OPL 1.2.0-Beta-2271 not listing new-format ISO names from
    the internal HDD (BDM ATA) while listing them from USB."""
    c = connect(args)
    if not opl_fix_names(c, dry_run=args.dry_run) and not args.dry_run:
        print("  nothing to rename")


# --------------------------------------------------------------- OPL art

ART_DB = "https://archive.org/download/OPLM_ART_2024_09/OPLM_ART_2024_09.zip/PS2/{id}/{id}_{name}.{ext}"
ART_COVERS = "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/{dash}.jpg"
# OPL file suffix -> name in the OPL Manager art backup
ART_TYPES = {"COV": "COV", "COV2": "COV2", "ICO": "ICO", "LAB": "LAB", "LGO": "LGO",
             "BG": "BG_00", "SCR": "SCR_00", "SCR2": "SCR_01"}


def http_get(url: str, tries: int = 3) -> bytes | None:
    import urllib.error
    import urllib.request
    req = urllib.request.Request(url, headers={"User-Agent": "ps2xfer"})
    for i in range(tries):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            time.sleep(2 * (i + 1))           # archive.org answers 503 when busy
        except (urllib.error.URLError, TimeoutError, OSError):
            time.sleep(2 * (i + 1))
    return None


def fetch_art(gid: str, kind: str) -> tuple[bytes, str] | None:
    for ext in ("png", "jpg"):
        data = http_get(ART_DB.format(id=gid, name=ART_TYPES[kind], ext=ext), tries=2 if ext == "jpg" else 3)
        if data:
            return data, ext
    if kind == "COV":
        data = http_get(ART_COVERS.format(dash=gid.replace("_", "-").replace(".", "")))
        if data:
            return data, "jpg"
    return None


def ps2_listdir(c: Conn, folder: str) -> list[str]:
    out, cursor = [], 0
    while True:
        try:
            r = c.call(T_LIST, struct.pack("<I", cursor) + Conn.path(folder))
        except PxError:
            return out
        nxt, count, _ = struct.unpack_from("<IHH", r)
        pos = 8
        for _ in range(count):
            _, _, _, nl = struct.unpack_from("<QIBB", r, pos)
            out.append(r[pos + 14:pos + 14 + nl].decode("utf-8", "replace"))
            pos += 14 + nl
        if nxt == 0:
            return out
        cursor = nxt


def art_for_game(c: Conn, args, gid: str, have: set | None = None, overwrite: bool = False,
                 log=print) -> list[str]:
    """Downloads the OPL art of one game into /ART on the PS2; returns the
    art types stored. `have`: names already in /ART without extension."""
    import tempfile
    import shutil
    if have is None:
        have = {n.rsplit(".", 1)[0] for n in ps2_listdir(c, "/ART")}
    tmpdir = tempfile.mkdtemp(prefix="ps2xfer-art-")
    got = []
    quiet_args = argparse.Namespace(**{**vars(args), "quiet": True})
    try:
        for kind in ART_TYPES:
            if f"{gid}_{kind}" in have and not overwrite:
                continue
            r = fetch_art(gid, kind)
            if r is None:
                continue
            data, ext = r
            tmp = os.path.join(tmpdir, f"{gid}_{kind}.{ext}")
            with open(tmp, "wb") as f:
                f.write(data)
            transfer(c, quiet_args, tmp, len(data), f"/ART/{gid}_{kind}.{ext}", OW_OVERWRITE)
            have.add(f"{gid}_{kind}")
            got.append(kind)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return got


def cmd_art(args):
    """Downloads OPL art (covers, icons, spines, logos, backgrounds,
    screenshots) for the games on the PS2 and stores it in /ART."""
    c = connect(args)
    have = {n.rsplit(".", 1)[0] for n in ps2_listdir(c, "/ART")}
    total = 0
    for folder in ("/CD", "/DVD"):
        for n in ps2_listdir(c, folder):
            if not n.lower().endswith((".iso", ".zso")):
                continue
            gid = n[:11].upper() if is_opl_old_name(n) else (
                remote_game_id(c, f"{folder}/{n}") if n.lower().endswith(".iso") else None)
            if not gid:
                print(f"  {n}: no game ID, skipped")
                continue
            got = art_for_game(c, args, gid, have, args.overwrite)
            total += len(got)
            print(f"  {gid} {n}: {', '.join(got) if got else 'nothing new'}")
    print(f"{total} art file(s) stored in /ART")


class _Req:
    __slots__ = ("off", "length", "buf", "got", "npk", "responded", "t", "parent", "pidx", "children")

    def __init__(self, off, length, buf, parent=None, pidx=0):
        self.off, self.length, self.buf = off, length, buf
        self.npk = (length + PAYLOAD - 1) // PAYLOAD
        self.got = set()
        self.responded = False
        self.t = 0.0
        self.parent, self.pidx = parent, pidx   # single-packet re-request of parent's packet pidx
        self.children = 0


class Downloader:
    """Reads a file from the PS2 with READ requests (several in flight). The
    data arrives as READ_DATA packets; packets still missing after the
    response are asked for again one by one; a request without any answer
    for 1 s is sent again."""

    def __init__(self, conn: Conn, path: str, sink, start: int = 0, size: int | None = None,
                 inflight: int = 3, quiet: bool = False, label: str = ""):
        self.c, self.path, self.sink = conn, path, sink
        self.inflight, self.quiet, self.label = inflight, quiet, label
        self.start, self.size = start, size
        self.pending: dict[int, _Req] = {}     # req_id -> request on the wire
        self.waiting: list[_Req] = []          # big requests waiting for their re-requests
        self.next_off = start
        self.done_upto = start
        self.ready: dict[int, bytes] = {}      # finished chunks not yet in order
        self.rerequests = 0
        self.elapsed = 0.0

    def _send(self, req: _Req):
        rid = self.c._next_id()
        body = struct.pack("<QI", req.off, req.length) + Conn.path(self.path)
        self.c.sock.sendto(CTL.pack(T_READ, 0, 0, rid, self.c.session) + body, self.c.addr)
        req.t = time.monotonic()
        req.responded = False
        self.pending[rid] = req

    def _fill(self):
        big = sum(1 for r in self.pending.values() if r.parent is None) + len(self.waiting)
        while big < self.inflight and self.next_off < self.size:
            big += 1
            n = min(READ_MAX, self.size - self.next_off)
            self._send(_Req(self.next_off, n, bytearray(n)))
            self.next_off += n

    def _finish(self, req: _Req):
        if req.parent is not None:              # a re-request: count it in the parent
            p = req.parent
            p.got.update(range(req.pidx, req.pidx + req.npk))
            p.children -= 1
            if p.children == 0 and len(p.got) == p.npk:
                self.waiting.remove(p)
                self._finish(p)
            return
        self.ready[req.off] = bytes(req.buf)
        while self.done_upto in self.ready:
            b = self.ready.pop(self.done_upto)
            self.sink(b)
            self.done_upto += len(b)

    def _on_data(self, rid, off, data):
        req = self.pending.get(rid)
        if req is None:
            return
        rel = off - req.off
        if rel < 0 or rel + len(data) > req.length or rel % PAYLOAD:
            return
        req.buf[rel:rel + len(data)] = data
        req.got.add(rel // PAYLOAD)
        if len(req.got) == req.npk:
            del self.pending[rid]
            self._finish(req)

    def _on_response(self, rid):
        req = self.pending.get(rid)
        if req is None:
            return
        req.responded = True
        if len(req.got) == req.npk:
            return
        if req.parent is not None:
            # a re-request lost some of its packets: ask for the same run again
            del self.pending[rid]
            req.got.clear()
            self._send(req)
            self.rerequests += 1
            return
        del self.pending[rid]
        missing = [i for i in range(req.npk) if i not in req.got]
        runs = []                                  # contiguous runs of missing packets
        for i in missing:
            if runs and runs[-1][1] == i:
                runs[-1][1] = i + 1
            else:
                runs.append([i, i + 1])
        req.children = len(runs)
        self.waiting.append(req)
        mv = memoryview(req.buf)
        for a, b in runs:
            o = a * PAYLOAD
            n = min(b * PAYLOAD, req.length) - o
            self._send(_Req(req.off + o, n, mv[o:o + n], parent=req, pidx=a))
            self.rerequests += 1

    def run(self) -> None:
        if self.size is None:
            st = self.c.call(T_STAT, Conn.path(self.path))
            self.size = struct.unpack_from("<Q", st)[0]
        t0 = time.perf_counter()
        last_print = 0.0
        self._fill()
        while self.done_upto < self.size:
            r, _, _ = select.select([self.c.sock], [], [], 0.05)
            if r:
                data, _ = self.c.sock.recvfrom(4096)
                if len(data) >= DATA.size and data[0] == T_READ_DATA:
                    _, fl, off, rid, _, _, n = DATA.unpack_from(data)
                    self._on_data(rid, off, data[DATA.size:DATA.size + n])
                elif len(data) >= CTL.size and data[0] == T_READ:
                    t, fl, st, rid, sess = CTL.unpack_from(data)
                    if fl & F_RESPONSE:
                        if st != 0 and rid in self.pending:
                            raise PxError(f"READ failed: {status_text(st)}")
                        self._on_response(rid)
            now = time.monotonic()
            for rid in [k for k, v in self.pending.items() if now - v.t > 0.25]:
                req = self.pending.pop(rid)
                self._send(req)               # no answer at all: same range again
                self.rerequests += 1
            self._fill()
            _check_cancel()
            if not self.quiet and time.perf_counter() - last_print >= 0.5:
                el = time.perf_counter() - t0
                done = self.done_upto - self.start
                rate = done / el if el > 0 else 0
                eta = (self.size - self.done_upto) / rate if rate > 0 else 0
                last_print = time.perf_counter()
                if PROGRESS_HOOK is not None:
                    PROGRESS_HOOK(self.label, self.done_upto, self.size, rate, rate, eta, self.rerequests)
                    continue
                sys.stdout.write(f"\r{self.label} {100 * self.done_upto / max(1, self.size):5.1f}%  "
                                 f"{fmt_size(self.done_upto)}/{fmt_size(self.size)}  {rate / 1e6:5.2f} MB/s  "
                                 f"ETA {int(eta) // 60}:{int(eta) % 60:02d}  re-requests {self.rerequests}")
                sys.stdout.flush()
                last_print = time.perf_counter()
        self.elapsed = time.perf_counter() - t0
        if not self.quiet:
            sys.stdout.write("\n")


def cmd_get(args):
    c = connect(args)
    st = c.call(T_STAT, Conn.path(args.remote))
    size, _, attr, _ = struct.unpack_from("<QIBB", st)
    if attr & 0x10:
        raise PxError("that is a directory")
    local = args.local or os.path.basename(args.remote)
    if os.path.isdir(local):
        local = os.path.join(local, os.path.basename(args.remote))
    with open(local + ".part", "wb") as f:
        d = Downloader(c, args.remote, f.write, size=size, label=os.path.basename(args.remote))
        d.run()
    os.replace(local + ".part", local)
    print(f"    {fmt_size(size)} in {d.elapsed:.1f} s = {size / d.elapsed / 1e6 if d.elapsed else 0:.2f} MB/s, "
          f"{d.rerequests} re-requests -> {local}")


def verify_file_full(c: Conn, src: str, dst: str):
    """reads the whole file back from the PS2 and compares SHA-256"""
    size = os.path.getsize(src)
    h = hashlib.sha256()
    d = Downloader(c, dst, h.update, size=size, label="    verify (read back)")
    d.run()
    local = hashlib.sha256()
    with open(src, "rb") as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            local.update(b)
    if h.digest() != local.digest():
        raise PxError(f"VERIFY FAILED: SHA-256 of {dst} differs from {src}")
    print(f"    verify OK: SHA-256 {local.hexdigest()[:16]}... identical "
          f"({size / d.elapsed / 1e6 if d.elapsed else 0:.2f} MB/s read-back)")


def verify_file(c: Conn, src: str, dst: str):
    size = os.path.getsize(src)
    nblocks = (size + (1 << 20) - 1) >> 20
    done = 0
    t0 = time.monotonic()
    # the PC sums its copy in a thread while the PS2 sums the HDD copy
    pool = concurrent.futures.ThreadPoolExecutor(1)
    while done < nblocks:
        n = min(300, nblocks - done)
        fut = pool.submit(fletcher_blocks, src, done << 20, n)
        r = c.call(T_BLOCKSUMS, struct.pack("<QI", done << 20, n) + Conn.path(dst), timeout=120.0, retries=2)
        cnt = struct.unpack_from("<I", r)[0]
        remote = list(struct.unpack_from(f"<{cnt}I", r, 4))
        local = fut.result()[:cnt]
        for k, (a, b) in enumerate(zip(local, remote)):
            if a != b:
                raise PxError(f"VERIFY FAILED: {dst} differs in MiB block {done + k}")
        if cnt == 0:
            raise PxError("VERIFY: PS2 returned no data")
        done += cnt
        _check_cancel()
        sys.stdout.write(f"\r    verify {100 * done // nblocks}%")
        sys.stdout.flush()
        if PROGRESS_HOOK is not None:
            el = max(time.monotonic() - t0, 1e-6)
            nbytes = min(size, done << 20)
            rate = nbytes / el
            PROGRESS_HOOK(f"verify {dst}", nbytes, size, rate, rate, (size - nbytes) / rate if rate else 0, 0)
    print(f"\r    verify OK ({nblocks} MiB blocks, {time.monotonic() - t0:.1f} s)")


def cmd_verify(args):
    c = connect(args)
    (verify_file_full if args.full else verify_file)(c, args.local, args.remote)


def cmd_info(args):
    c = connect(args)
    i = c.info
    print(f"  MAC         {':'.join(f'{b:02x}' for b in i['mac'])}")
    print(f"  disk        {i['model'] or '-'} ({fmt_size(i['disk_bytes'])}), UDMA "
          f"{i['udma'] if i['udma'] != 0xff else '?'}")
    print(f"  exFAT       {i['fs_total_mb']} MiB, free "
          f"{i['fs_free_mb'] if i['fs_free_mb'] != M32 else '?'} MiB, cluster {i['cluster']}")
    print(f"  buffers     {i['nbuf']} x {i['bufsize']} bytes, IOP free {i['iop_free']} bytes")
    print(f"  SPEED rev   {i['speed_rev']:#x}")


def cmd_ping(args):
    c = connect(args, observe=True)
    for _ in range(args.count):
        t = time.perf_counter()
        r = c.call(T_PING, timeout=1.0, retries=1)
        print(f"reply from {c.addr[0]}: {(time.perf_counter() - t) * 1000:.2f} ms, "
              f"uptime {struct.unpack_from('<I', r)[0] / 1000:.1f} s")
        time.sleep(0.5)


def cmd_stats(args):
    c = connect(args, observe=True)
    for k, v in c.stats().items():
        print(f"  {k:16} {v}")


def cmd_log(args):
    c = connect(args, observe=True)
    cursor = 0
    while True:
        r = c.call(T_LOG, struct.pack("<I", cursor))
        cursor, lost = struct.unpack_from("<II", r)
        text = r[8:].decode(errors="replace")
        if lost:
            print(f"[... {lost} bytes lost ...]")
        sys.stdout.write(text)
        if not text:
            if not args.follow:
                break
            time.sleep(0.5)


def cmd_set(args):
    c = connect(args)
    for kv in args.items:
        c.call(T_SET, kv.encode() + b"\0")
        print(f"set {kv}")


def cmd_exit(args):
    c = connect(args)
    c.call(T_EXIT)
    print("PS2 is leaving ps2xfer")


def cmd_ls(args):
    c = connect(args)
    cursor = 0
    while True:
        r = c.call(T_LIST, struct.pack("<I", cursor) + Conn.path(args.path))
        nxt, count, _ = struct.unpack_from("<IHH", r)
        pos = 8
        for _ in range(count):
            size, ft, attr, nl = struct.unpack_from("<QIBB", r, pos)
            name = r[pos + 14:pos + 14 + nl].decode(errors="replace")
            pos += 14 + nl
            d = ft >> 16
            date = f"{(d >> 9) + 1980:04d}-{(d >> 5) & 15:02d}-{d & 31:02d}"
            kind = "<DIR>" if attr & 0x10 else fmt_size(size)
            print(f"  {date}  {kind:>12}  {name}")
        if nxt == 0:
            break
        cursor = nxt


def cmd_diskinfo(args):
    """read-only look at the partition table of the PS2 disk"""
    c = connect(args)
    r = c.call(T_DISKINFO, timeout=10.0)
    mbr, gpt = r[:512], r[512:1024]
    volbase, fstype = struct.unpack_from("<QI", r, 1024)
    print(f"  sector 0 boot signature: {'55 AA (MBR present)' if mbr[510:512] == bytes([0x55, 0xAA]) else mbr[510:512].hex()}")
    for i in range(4):
        e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
        ptype = e[4]
        start, count = struct.unpack_from("<II", e, 8)
        if ptype:
            name = {0x07: "exFAT/NTFS", 0x0C: "FAT32 LBA", 0xEE: "GPT protective", 0x83: "Linux"}.get(ptype, "?")
            print(f"  MBR partition {i}: type 0x{ptype:02X} ({name}), start LBA {start}, {count} sectors "
                  f"({fmt_size(count * 512)})")
    print(f"  sector 1: {'GPT header signature EFI PART present' if gpt[:8] == b'EFI PART' else 'no GPT header'}")
    if gpt[:8] == b"EFI PART":
        cur, backup, first, last = struct.unpack_from("<QQQQ", gpt, 24)
        ent_lba, nent, esz = struct.unpack_from("<QII", gpt, 72)
        print(f"    GPT: header LBA {cur}, backup LBA {backup}, usable {first}..{last}, {nent} entries at LBA {ent_lba}")
    print(f"  ps2xfer mounted the volume at LBA {volbase} (FatFs type {fstype}: 4 = exFAT)")


def cmd_stat(args):
    c = connect(args)
    size, ft, attr, contig = struct.unpack_from("<QIBB", c.call(T_STAT, Conn.path(args.path)))
    kind = "directory" if attr & 0x10 else ("contiguous" if contig else "FRAGMENTED")
    print(f"  {args.path}: {fmt_size(size)} ({size} bytes), {kind}")


def cmd_df(args):
    c = connect(args)
    total, free, clus = struct.unpack_from("<QQI", c.call(T_DF, timeout=30.0))
    print(f"  total {fmt_size(total)}, free {fmt_size(free)}, cluster {clus} bytes")


def cmd_mkdir(args):
    c = connect(args)
    c.call(T_MKDIR, Conn.path(args.path))


def cmd_rm(args):
    c = connect(args)
    for p in args.paths:
        c.call(T_DELETE, Conn.path(p))
        print(f"deleted {p}")


def cmd_mv(args):
    c = connect(args)
    c.call(T_RENAME, Conn.path(args.src) + Conn.path(args.dst))


# --------------------------------------------------------------- FTP gateway (Explorer)

class FtpGateway:
    """Minimal FTP server on the PC that maps to the PS2: open
    ftp://127.0.0.1:2121 in Windows Explorer to browse, copy, rename and
    delete. Passive mode only. Uploads are received into a temp file first
    (PS2 files are preallocated in one piece, so the size must be known) and
    the 226 reply comes after the file is on the PS2."""

    MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()

    def __init__(self, args):
        import threading
        self.args = args
        self.lock = threading.Lock()
        self.c = connect(args)

    # ---- PS2 calls (one at a time; re-HELLO if another client took the session)
    def ps2(self, typ, body=b"", **kw):
        with self.lock:
            st, r = self.c.request(typ, body, **kw)
            if st == 8:
                self.c.hello()
                st, r = self.c.request(typ, body, **kw)
            if st != 0:
                raise PxError(status_text(st))
            return r

    def listdir(self, path):
        out, cursor = [], 0
        while True:
            r = self.ps2(T_LIST, struct.pack("<I", cursor) + Conn.path(path))
            nxt, count, _ = struct.unpack_from("<IHH", r)
            pos = 8
            for _ in range(count):
                size, ft, attr, nl = struct.unpack_from("<QIBB", r, pos)
                name = r[pos + 14:pos + 14 + nl].decode("utf-8", "replace")
                pos += 14 + nl
                out.append((name, size, ft, attr))
            if nxt == 0:
                return out
            cursor = nxt

    def stat(self, path):
        if path == "/":
            return (0, 0, 0x10)
        size, ft, attr, _ = struct.unpack_from("<QIBB", self.ps2(T_STAT, Conn.path(path)))
        return size, ft, attr

    @staticmethod
    def fat_tuple(ft):
        d, t = ft >> 16, ft & 0xFFFF
        return ((d >> 9) + 1980, max(1, (d >> 5) & 15), max(1, d & 31), t >> 11, (t >> 5) & 63, (t & 31) * 2)

    def list_line(self, name, size, ft, attr):
        y, mo, da, h, mi, _ = self.fat_tuple(ft)
        perm = "drwxr-xr-x" if attr & 0x10 else "-rw-r--r--"
        return f"{perm} 1 ps2 ps2 {size:>13} {self.MONTHS[mo - 1]} {da:2d} {y} {name}"

    # ---- server
    def serve(self, host, port):
        import threading
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((host, port))
        ls.listen(4)
        print(f"FTP gateway to the PS2 on ftp://{host}:{port}/  (Ctrl+C to stop)")
        print("Open that address in Windows Explorer, or add it as a network location.")
        while True:
            cs, addr = ls.accept()
            threading.Thread(target=self.session, args=(cs,), daemon=True).start()

    def session(self, cs):
        f = cs.makefile("rb")
        cwd = "/"
        pasv = None
        rnfr = None
        rest = 0

        def reply(text):
            cs.sendall((text + "\r\n").encode("utf-8"))

        def norm(arg):
            import posixpath
            p = arg if arg.startswith("/") else posixpath.join(cwd, arg)
            p = posixpath.normpath(p)
            return "/" if p in (".", "//") else p

        def data_conn():
            nonlocal pasv
            if pasv is None:
                return None
            pasv.settimeout(15)
            try:
                ds, _ = pasv.accept()
            finally:
                pasv.close()
                pasv = None
            return ds

        reply("220 ps2xfer FTP gateway")
        try:
            while True:
                line = f.readline()
                if not line:
                    return
                line = line.decode("utf-8", "replace").rstrip("\r\n")
                cmd, _, arg = line.partition(" ")
                cmd = cmd.upper()
                try:
                    if cmd == "USER":
                        reply("331 any password")
                    elif cmd == "PASS":
                        reply("230 logged in")
                    elif cmd == "SYST":
                        reply("215 UNIX Type: L8")
                    elif cmd == "FEAT":
                        reply("211-Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n EPSV\r\n PASV\r\n211 End")
                    elif cmd == "OPTS":
                        reply("200 OK")
                    elif cmd in ("TYPE", "MODE", "STRU", "ALLO", "NOOP"):
                        reply("200 OK")
                    elif cmd in ("PWD", "XPWD"):
                        reply(f'257 "{cwd}"')
                    elif cmd in ("CWD", "XCWD", "CDUP"):
                        p = norm(".." if cmd == "CDUP" else arg)
                        if self.stat(p)[2] & 0x10:
                            cwd = p
                            reply("250 OK")
                        else:
                            reply("550 not a directory")
                    elif cmd in ("PASV", "EPSV"):
                        if pasv:
                            pasv.close()
                        pasv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        ip = cs.getsockname()[0]
                        pasv.bind((ip, 0))
                        pasv.listen(1)
                        pp = pasv.getsockname()[1]
                        if cmd == "EPSV":
                            reply(f"229 Entering Extended Passive Mode (|||{pp}|)")
                        else:
                            reply(f"227 Entering Passive Mode ({ip.replace('.', ',')},{pp >> 8},{pp & 255})")
                    elif cmd in ("LIST", "NLST", "MLSD"):
                        args_ = [a for a in arg.split() if not a.startswith("-")]
                        p = norm(args_[0]) if args_ else cwd
                        entries = self.listdir(p)
                        reply("150 listing")
                        ds = data_conn()
                        lines = []
                        for name, size, ft, attr in entries:
                            if cmd == "NLST":
                                lines.append(name)
                            elif cmd == "MLSD":
                                y, mo, da, h, mi, se = self.fat_tuple(ft)
                                typ = "dir" if attr & 0x10 else "file"
                                lines.append(f"type={typ};size={size};modify={y:04d}{mo:02d}{da:02d}{h:02d}{mi:02d}{se:02d}; {name}")
                            else:
                                lines.append(self.list_line(name, size, ft, attr))
                        if ds:
                            ds.sendall(("".join(l + "\r\n" for l in lines)).encode("utf-8"))
                            ds.close()
                        reply("226 done")
                    elif cmd == "SIZE":
                        reply(f"213 {self.stat(norm(arg))[0]}")
                    elif cmd == "MDTM":
                        y, mo, da, h, mi, se = self.fat_tuple(self.stat(norm(arg))[1])
                        reply(f"213 {y:04d}{mo:02d}{da:02d}{h:02d}{mi:02d}{se:02d}")
                    elif cmd == "REST":
                        rest = int(arg)
                        reply(f"350 restarting at {rest}")
                    elif cmd == "RETR":
                        p = norm(arg)
                        size = self.stat(p)[0]
                        reply("150 sending")
                        ds = data_conn()
                        with self.lock:
                            d = Downloader(self.c, p, ds.sendall, start=rest, size=size,
                                           quiet=self.args.quiet, label=f"get {p}")
                            d.run()
                        ds.close()
                        rest = 0
                        reply("226 done")
                    elif cmd == "STOR":
                        import tempfile
                        p = norm(arg)
                        reply("150 receiving")
                        ds = data_conn()
                        tf = tempfile.NamedTemporaryFile(prefix="ps2xfer-", delete=False)
                        try:
                            while True:
                                b = ds.recv(1 << 20)
                                if not b:
                                    break
                                tf.write(b)
                            tf.close()
                            ds.close()
                            size = os.path.getsize(tf.name)
                            if size == 0:
                                reply("553 empty files are not supported")
                            else:
                                with self.lock:
                                    transfer(self.c, self.args, tf.name, size, p, OW_OVERWRITE,
                                             fingerprint(tf.name), label=f"put {p}")
                                reply("226 stored on the PS2")
                        finally:
                            tf.close()
                            os.unlink(tf.name)
                    elif cmd == "DELE" or cmd in ("RMD", "XRMD"):
                        self.ps2(T_DELETE, Conn.path(norm(arg)))
                        reply("250 deleted")
                    elif cmd in ("MKD", "XMKD"):
                        p = norm(arg)
                        self.ps2(T_MKDIR, Conn.path(p))
                        reply(f'257 "{p}" created')
                    elif cmd == "RNFR":
                        rnfr = norm(arg)
                        self.stat(rnfr)
                        reply("350 ready")
                    elif cmd == "RNTO":
                        if rnfr is None:
                            reply("503 RNFR first")
                        else:
                            self.ps2(T_RENAME, Conn.path(rnfr) + Conn.path(norm(arg)))
                            rnfr = None
                            reply("250 renamed")
                    elif cmd == "QUIT":
                        reply("221 bye")
                        return
                    else:
                        reply("502 not implemented")
                except PxError as e:
                    reply(f"550 {e}")
        except (ConnectionError, OSError):
            pass
        finally:
            cs.close()


def cmd_ftp(args):
    host, _, port = args.listen.partition(":")
    FtpGateway(args).serve(host or "127.0.0.1", int(port or 2121))


# --------------------------------------------------------------- benches

def bench_disk(c: Conn, kind: int, size_mb: int, chunk: int, readback: bool) -> dict:
    r = c.call(T_BENCH, struct.pack("<IIII", kind, size_mb, chunk, 1 if readback else 0),
               timeout=30.0, retries=40)
    k, tpm, nbytes, wt, wi, _, rt, ri, nv = struct.unpack_from("<IIQQIIQII", r)
    vals = struct.unpack_from("<16I", r, 48)[:nv]
    out = {"bytes": nbytes, "ticks_per_ms": tpm, "vals": vals}
    if wt:
        out["write_mbs"] = nbytes / (wt / tpm / 1000) / 1e6
        out["write_cpu"] = 100 - wi / 10
    if rt:
        out["read_mbs"] = nbytes / (rt / tpm / 1000) / 1e6
        out["read_cpu"] = 100 - ri / 10
    return out


def cmd_bench(args):
    if args.kind == "selftest":
        return bench_selftest(args)
    c = connect(args)
    if args.kind == "fscheck":
        r = c.call(T_BENCH, struct.pack("<IIII", 5, 0, 0, 0), timeout=60.0, retries=2)
        print(r[112:].split(b"\0")[0].decode(errors="replace"))
        return
    if args.kind == "micro":
        r = bench_disk(c, BENCH_MICRO, 0, 0, False)
        names = ["fletcher ticks/KiB", "memcpy ticks/KiB", "IOP total free", "IOP max free",
                 "SPEED reg read x1000 ticks", "GetSystemTime x1000 ticks", "SPEED rev",
                 "RX DMA block", "SMAP->RAM 1408B DMA ticks (avg of 0 if no frame)"]
        print(f"  ticks/ms = {r['ticks_per_ms']}")
        for n, v in zip(names, r["vals"]):
            print(f"  {n:32} {v}")
        return
    if args.kind in ("disk-raw", "disk-fatfs", "disk-iomanx"):
        kind = {"disk-raw": BENCH_DISK_RAW, "disk-fatfs": BENCH_DISK_FATFS,
                "disk-iomanx": BENCH_DISK_IOMANX}[args.kind]
        chunks = [int(x) for x in args.chunks.split(",")] if args.chunks else [0]
        unit = "sectors" if kind == BENCH_DISK_RAW else "bytes"
        print(f"  {'chunk (' + unit + ')':>18}  {'write MB/s':>10}  {'CPU %':>6}  {'read MB/s':>10}  {'CPU %':>6}")
        for ch in chunks:
            r = bench_disk(c, kind, args.size_mb, ch, True)
            print(f"  {ch or 'buffer':>18}  {r.get('write_mbs', 0):10.2f}  {r.get('write_cpu', 0):6.1f}  "
                  f"{r.get('read_mbs', 0):10.2f}  {r.get('read_cpu', 0):6.1f}")
        return
    if args.kind in ("net-sink", "net-sink-nodma", "combined"):
        size = args.size_mb << 20
        s0 = c.stats()
        if args.kind == "combined":
            flags = OW_OVERWRITE | OW_KEEP_PART
            r = transfer(c, args, None, size, "/.ps2xfer/bench.bin", flags, label=args.kind)
            c.call(T_DELETE, Conn.path("/.ps2xfer/bench.bin.part"))
        else:
            flags = OW_SINK if args.kind == "net-sink" else OW_SINK_NODMA
            r = transfer(c, args, None, size, None, flags, label=args.kind)
        s1 = c.stats()
        mbs = r["bytes"] / r["seconds"] / 1e6
        pps = r["bytes"] / PAYLOAD / r["seconds"]
        print(f"  {mbs:.2f} MB/s, {pps:.0f} pkt/s, retransmits {r['retransmits']}, "
              f"PS2: rx_errors +{s1['rx_errors'] - s0['rx_errors']}, overruns "
              f"+{s1['rx_overruns'] - s0['rx_overruns']}, drops +{s1['rx_drops'] - s0['rx_drops']}, "
              f"csum errors +{s1['csum_errors'] - s0['csum_errors']}, CPU {s1['cpu_load_x10'] / 10:.1f}%, "
              f"waiting for PS2 credit {100 * r['credit_wait'] / r['seconds']:.0f}% of the time")
        if s1["prof_smap_wakes"] > s0["prof_smap_wakes"]:
            npk = max(1, s1["rx_pkts"] - s0["rx_pkts"])
            d = {k: (s1[k] - s0[k]) & M32 for k in ("prof_smap16", "prof_smap_wakes", "prof_writer16",
                                                     "prof_writer_slots", "prof_ack16", "prof_ack_runs")}
            wall = r["seconds"] * 36.864e6
            print(f"  IOP time per packet (ticks; wall {wall / npk:.0f}): SMAP thread {d['prof_smap16'] * 16 / npk:.0f} "
                  f"({d['prof_smap_wakes'] / npk:.2f} wake-ups/packet), writer {d['prof_writer16'] * 16 / npk:.0f}, "
                  f"ack thread {d['prof_ack16'] * 16 / npk:.0f} ({d['prof_ack_runs']} runs)")
        if s1["prof_samples"]:
            print(f"  profile (IOP ticks/packet, 36864 per ms): payload DMA {s1['prof_dma']}, "
                  f"whole RX path {s1['prof_total']} ({s1['prof_samples']} samples; "
                  f"includes ~450 ticks of timer calls)")
        return
    raise PxError(f"unknown bench {args.kind}")


def bench_selftest(args):
    """How many DATA packets/s can this Python build + checksum + send?"""
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
    src = Source(None, PAYLOAD * 100000)
    buf = bytearray(DATA.size + PAYLOAD)
    n = args.count * 1000
    t0 = time.perf_counter()
    c0 = time.process_time()
    for i in range(n):
        data = src.chunk(i)
        DATA.pack_into(buf, 0, T_DATA, 0, i * PAYLOAD, i, fletcher(data), 1, len(data))
        buf[DATA.size:] = data
        try:
            tx.sendto(buf, rx.getsockname())
        except OSError:
            pass
    dt = time.perf_counter() - t0
    cpu = time.process_time() - c0
    print(f"  {n / dt:.0f} packets/s ({n / dt * PAYLOAD / 1e6:.1f} MB/s payload), "
          f"{dt / n * 1e6:.1f} us/packet, CPU {100 * cpu / dt:.0f}% of one core")
    need = 8356
    print(f"  line rate needs {need} packets/s: {'OK' if n / dt > need * 2 else 'TOO SLOW for comfortable pacing'}")


# --------------------------------------------------------------- GUI (tkinter)

def _win_dpi_aware():
    """Sharp text on scaled Windows displays (before the first window)."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass


def _win_enable_drop(root, on_files) -> bool:
    """Accept files dropped from Explorer on the window (Windows, ctypes only).
    on_files(list_of_paths) runs INSIDE the window procedure, where Tk is busy
    dispatching: it must not call tkinter (that is a fatal error), only hand
    the list over, e.g. to a queue the Tk thread polls."""
    if sys.platform != "win32":
        return False
    try:
        import ctypes
        from ctypes import wintypes
        user32, shell32 = ctypes.windll.user32, ctypes.windll.shell32
        LRESULT = ctypes.c_ssize_t
        WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        user32.GetParent.restype = wintypes.HWND
        user32.GetParent.argtypes = [wintypes.HWND]
        user32.GetWindowLongPtrW.restype = ctypes.c_void_p
        user32.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.SetWindowLongPtrW.restype = ctypes.c_void_p
        user32.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
        user32.CallWindowProcW.restype = LRESULT
        user32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                           wintypes.LPARAM]
        shell32.DragQueryFileW.restype = wintypes.UINT
        shell32.DragQueryFileW.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_wchar_p, wintypes.UINT]
        shell32.DragFinish.argtypes = [ctypes.c_void_p]
        shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
        WM_DROPFILES, GWLP_WNDPROC = 0x0233, -4

        root.update_idletasks()
        hwnd = user32.GetParent(root.winfo_id()) or root.winfo_id()
        old = user32.GetWindowLongPtrW(hwnd, GWLP_WNDPROC)

        def proc(h, msg, wp, lp):
            if msg == WM_DROPFILES:
                files = []
                n = shell32.DragQueryFileW(wp, 0xFFFFFFFF, None, 0)
                for i in range(n):
                    ln = shell32.DragQueryFileW(wp, i, None, 0)
                    buf = ctypes.create_unicode_buffer(ln + 1)
                    shell32.DragQueryFileW(wp, i, buf, ln + 1)
                    files.append(buf.value)
                shell32.DragFinish(wp)
                on_files(files)
                return 0
            return user32.CallWindowProcW(old, h, msg, wp, lp)

        root._px_drop_proc = WNDPROC(proc)   # keep it alive
        user32.SetWindowLongPtrW(hwnd, GWLP_WNDPROC, ctypes.cast(root._px_drop_proc, ctypes.c_void_p))
        shell32.DragAcceptFiles(hwnd, True)
        return True
    except Exception:
        return False


def _png_rgba(w: int, h: int, rows: list[bytes]) -> bytes:
    """Minimal PNG encoder (8-bit RGBA rows) for Tk's PhotoImage."""
    import zlib
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


_GDIP_TOKEN = None


def _win_cover_png(data: bytes, max_w: int, max_h: int) -> bytes | None:
    """JPG/PNG cover -> PNG scaled to fit max_w x max_h, decoded by Windows'
    own GDI+ (ctypes; Tk alone cannot read JPG). None if it fails."""
    global _GDIP_TOKEN
    import ctypes
    from ctypes import byref, c_int, c_uint, c_void_p
    gdip, shlwapi = ctypes.windll.gdiplus, ctypes.windll.shlwapi
    if _GDIP_TOKEN is None:
        class StartupInput(ctypes.Structure):
            _fields_ = [("version", c_uint), ("callback", c_void_p), ("no_thread", c_int), ("no_codecs", c_int)]
        tok = ctypes.c_size_t()
        if gdip.GdiplusStartup(byref(tok), byref(StartupInput(1, None, 0, 0)), None) != 0:
            return None
        _GDIP_TOKEN = tok
    shlwapi.SHCreateMemStream.restype = c_void_p
    shlwapi.SHCreateMemStream.argtypes = [ctypes.c_char_p, c_uint]
    stream = shlwapi.SHCreateMemStream(data, len(data))
    if not stream:
        return None
    img, bmp = c_void_p(), c_void_p()

    class Rect(ctypes.Structure):
        _fields_ = [("x", c_int), ("y", c_int), ("w", c_int), ("h", c_int)]

    class BitmapData(ctypes.Structure):
        _fields_ = [("w", c_uint), ("h", c_uint), ("stride", c_int), ("fmt", c_int), ("scan0", c_void_p),
                    ("reserved", ctypes.c_size_t)]
    try:
        if gdip.GdipCreateBitmapFromStream(c_void_p(stream), byref(img)) != 0:
            return None
        w, h = c_uint(), c_uint()
        gdip.GdipGetImageWidth(img, byref(w))
        gdip.GdipGetImageHeight(img, byref(h))
        if not w.value or not h.value:
            return None
        k = min(max_w / w.value, max_h / h.value)
        tw, th = max(1, round(w.value * k)), max(1, round(h.value * k))
        argb = 0x26200A                       # PixelFormat32bppARGB
        if gdip.GdipCreateBitmapFromScan0(tw, th, 0, argb, None, byref(bmp)) != 0:
            return None
        g = c_void_p()
        if gdip.GdipGetImageGraphicsContext(bmp, byref(g)) != 0:
            return None
        gdip.GdipSetInterpolationMode(g, 7)    # high quality bicubic
        gdip.GdipDrawImageRectI(g, img, 0, 0, tw, th)
        gdip.GdipDeleteGraphics(g)
        bd = BitmapData()
        if gdip.GdipBitmapLockBits(bmp, byref(Rect(0, 0, tw, th)), 1, argb, byref(bd)) != 0:
            return None
        rows = []
        for y in range(th):
            bgra = ctypes.string_at(bd.scan0 + y * bd.stride, tw * 4)
            rgba = bytearray(bgra)
            rgba[0::4], rgba[2::4] = bgra[2::4], bgra[0::4]
            rows.append(bytes(rgba))
        gdip.GdipBitmapUnlockBits(bmp, byref(bd))
        return _png_rgba(tw, th, rows)
    except Exception:
        return None
    finally:
        if bmp:
            gdip.GdipDisposeImage(bmp)
        if img:
            gdip.GdipDisposeImage(img)
        vtbl = ctypes.cast(stream, ctypes.POINTER(ctypes.POINTER(c_void_p))).contents
        ctypes.WINFUNCTYPE(ctypes.c_ulong, c_void_p)(vtbl[2])(stream)   # IUnknown::Release


ICON_PNG64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAEFUlEQVR42u2b2U5TURSGfYM+gYLzgFhHELzoG8ggqDjgrKjMOKJS"
    "vVbTCxMo6AOYGElMFHFITqJeeeONRhBBARkVKpRJjFmudaIEWIWz2p69Wwo7+ZKTrH3O/v8v0PSiZ9GihbWwRGvHjREn4kYMxIdA"
    "lOH7l40yOm0rnnp92IUYCMwxKLMrnOIOxDMHi0+HOjiCKp/iHnIg7xCIEaiLTEJKBZavwBsq8MbYgjpZS9h+ze9BIEbxzF7+qt+F"
    "QIwz8wdj8pVBA4EYxwhcvnzQiYBK0m/6obP/D8y0aEZ7VOegrkxA0uUBNwIqqXk5BlaL9qjOQV0DCTDmkQD+b7Dt0k8fAiqpeTFq"
    "LQD3qM5BXbmAizhQjFiAhixMwNYLPlBN9XNrAbRHRxYu4DwOFCMWoCELE7DlXD+opvrZiLUA3KMjCxdQhgPFiAVoyMIEbC7rA9V4"
    "BQJoj44sXEApDhTjrRcIwD06sjABm0p+gGq89cMCAcOgIwsXUIwDxXifCgTgHh1ZmICNRd9BNVUCAbRHRxYuoBAHiqmqEwjAPTqy"
    "MAHOgl5QTVXdkEDAEOjIwgXk40AxVU8EAnCPjixMwIb8HlBNpUAA7dGRhQs4iwPFiAVoyMIEJJ7phlAo9Pqg8rFfxNvGX5YCaI/0"
    "eXR2qLm5gNM4CIHU0h742DYOuhedSWeHmpsJWJ/XBaGSUtKtVQKdRWeGk5kLOIWDMEgp1iPBLI9nhZuXCUg42Qnhsr24S6kEejad"
    "YUdWJQJMCUVqJJjli+wpH1DAuhMdYBfJhZ22SqBn0TPtzMgFHMeBjSQXoITW8CXQM+hZdudjAtYe+wZ2k5TfAbVvhkIuT/fSM1Rk"
    "4wKO4kARoUige1RmYgLWHGkHldS+lkugvarzcAGHcaAYiQSzvIYsTMDqQ22gg9kk0ExXjogJmEmCzvIBBazKbQWdPHzlnyhP17rP"
    "5wIO4kAzVNwsH4GzmYCVB77CfIIL2I+DeQQTsGLfF5hPcAE5OAiCvNs90N77GyK9KANlCTY/E7A8pwWknLrVDdG2KFMwHbiAvS0+"
    "BCS0945HtOy9R30mU/8SxkGan7oyAcv2NBsISIjkct/tgcU7G0zoevKS5qeuXMDuZjcCEiK5sstbJwTQ9RQBwvzUlQlYmv3ZiYCE"
    "SK73zaOQkPPJhK4nL2l+6hrw98JLsz4bCFjxwBiIug9ByiTJTh1n/LV4fFaTK35XE1iRmNsMH1rGoqY8ZaFMkuzUcdZ3BuIzmzwI"
    "SCi70w2e+30RhTJI81I3yzdG4jI/OZB3CMQY1En23lBcBkrIwBsy8MbYgLoE9+bYkvRGB+JBYI5DHYIrP0VEWqMLMRCYY1Dm0F+c"
    "nL4WpzU4ETd+ATEQ3/8vI1GEz8xGGTHrwsvOC0u2/gKAJQgpSncQsQAAAABJRU5ErkJggg==")


def cmd_gui(args):
    """Window for everyday use: games on the PS2, a send queue with progress,
    rename/delete/copy back/verify/art. All PS2 work runs on one worker thread;
    the Tk thread only draws."""
    import json
    import queue
    import threading
    import tkinter as tk
    import tkinter.font as tkfont
    from tkinter import filedialog, messagebox, simpledialog, ttk

    global PROGRESS_HOOK, CANCEL
    CANCEL = threading.Event()
    cfg_path = os.path.join(os.path.expanduser("~"), ".ps2xfer.json")
    try:
        with open(cfg_path) as f:
            cfg = json.load(f)
    except (OSError, ValueError):
        cfg = {}
    sent_map = cfg.setdefault("sent", {})     # PS2 path -> local source (for Verify)

    _win_dpi_aware()
    root = tk.Tk()
    root.title("ps2xfer")
    root.minsize(860, 600)
    k = root.winfo_fpixels("1i") / 96.0       # pixel sizes follow the display scale

    def px(v):
        return int(v * k)

    root.geometry(cfg.get("geometry") or f"{px(1100)}x{px(760)}")
    try:
        root.iconphoto(True, tk.PhotoImage(data=ICON_PNG64))
    except tk.TclError:
        pass

    # ---- look
    BG, CARD, LINE = "#f3f4f6", "#ffffff", "#e2e5ea"
    TEXT, DIM = "#1f2328", "#6b7280"
    ACCENT, ACCENT_D, SEL = "#2563eb", "#1d4ed8", "#dbe7fe"
    OK, WARN, BAD = "#15803d", "#b45309", "#b91c1c"
    family = "Segoe UI" if sys.platform == "win32" else tkfont.nametofont("TkDefaultFont").actual("family")
    F = (family, 10)
    F_B = (family, 10, "bold")
    F_S = (family, 9)
    F_T = (family, 16, "bold")
    root.configure(bg=BG)
    st = ttk.Style(root)
    st.theme_use("clam")
    st.configure(".", background=BG, foreground=TEXT, font=F, bordercolor=LINE, focuscolor=ACCENT)
    st.configure("TFrame", background=BG)
    st.configure("Card.TFrame", background=CARD)
    st.configure("TLabel", background=BG)
    st.configure("Card.TLabel", background=CARD)
    st.configure("Dim.TLabel", background=CARD, foreground=DIM, font=F_S)
    st.configure("Head.TLabel", background=CARD, font=F_B)
    st.configure("Title.TLabel", background=CARD, font=F_T)
    st.configure("Side.TLabel", background=BG, foreground=DIM, font=(family, 8, "bold"))
    st.configure("TButton", background=CARD, padding=(px(10), px(4)),
                 lightcolor=CARD, darkcolor=CARD, bordercolor="#c9cfd8")
    st.map("TButton", background=[("disabled", BG), ("pressed", LINE), ("active", "#eef2ff")],
           foreground=[("disabled", "#a0a6b0")])
    st.configure("Accent.TButton", background=ACCENT, foreground="#ffffff", bordercolor=ACCENT,
                 lightcolor=ACCENT, darkcolor=ACCENT, font=F_B)
    st.map("Accent.TButton", background=[("disabled", "#93b4f5"), ("pressed", ACCENT_D), ("active", ACCENT_D)],
           foreground=[("disabled", "#ffffff")])
    st.configure("TCheckbutton", background=CARD, indicatorbackground=CARD, indicatorforeground="#ffffff",
                 indicatormargin=(0, 0, px(6), 0), upperbordercolor="#9aa3af", lowerbordercolor="#9aa3af")
    st.map("TCheckbutton", background=[("active", CARD)],
           indicatorbackground=[("selected", ACCENT), ("active", "#eef2ff")])
    st.configure("TEntry", fieldbackground="#ffffff", padding=px(3))
    st.configure("Treeview", background=CARD, fieldbackground=CARD, rowheight=px(26), bordercolor=LINE,
                 lightcolor=LINE, darkcolor=LINE)
    st.map("Treeview", background=[("selected", SEL)], foreground=[("selected", TEXT)])
    st.configure("Treeview.Heading", background=BG, foreground=DIM, font=(family, 9, "bold"), relief="flat",
                 bordercolor=LINE, lightcolor=BG, darkcolor=BG, padding=(px(6), px(4)))
    st.map("Treeview.Heading", background=[("active", LINE)])
    st.configure("Side.Treeview", background=BG, fieldbackground=BG, rowheight=px(30), borderwidth=0,
                 lightcolor=BG, darkcolor=BG, bordercolor=BG, font=F)
    st.layout("Side.Treeview", [("Treeview.treearea", {"sticky": "nswe"})])
    st.configure("Bar.Horizontal.TProgressbar", troughcolor="#e5e7eb", background=ACCENT, bordercolor="#e5e7eb",
                 lightcolor=ACCENT, darkcolor=ACCENT, thickness=px(12))
    st.configure("Verify.Horizontal.TProgressbar", troughcolor="#e5e7eb", background=OK, bordercolor="#e5e7eb",
                 lightcolor=OK, darkcolor=OK, thickness=px(12))
    st.configure("Thin.Horizontal.TProgressbar", troughcolor="#e5e7eb", background="#64748b",
                 bordercolor="#e5e7eb", lightcolor="#64748b", darkcolor="#64748b", thickness=px(5))
    st.configure("Disk.Horizontal.TProgressbar", troughcolor="#e5e7eb", background="#94a3b8",
                 bordercolor="#e5e7eb", lightcolor="#94a3b8", darkcolor="#94a3b8", thickness=px(6))

    ui_q: "queue.Queue" = queue.Queue()      # callables for the Tk thread
    work_q: "queue.Queue" = queue.Queue()    # (description, callable) for the worker
    state = {"conn": None, "busy": False, "items": [], "covers": set(), "sort": ("name", False),
             "batch_total": 0, "batch_done": 0, "folder": "/DVD", "plain": 0, "no_id": set(),
             "opl_auto": cfg.get("opl_auto", True)}

    def ui(fn, *a):
        ui_q.put(lambda: fn(*a))

    # ---- menu
    menu = tk.Menu(root)
    m_ps2 = tk.Menu(menu, tearoff=0)
    m_ps2.add_command(label="Connect", command=lambda: do_connect())
    m_ps2.add_command(label="Refresh", accelerator="F5", command=lambda: refresh())
    m_ps2.add_separator()
    m_ps2.add_command(label="Download missing art for all games", command=lambda: download_art())
    m_ps2.add_command(label="Rename games for OPL now", command=lambda: fix_names())
    opl_auto_var = tk.BooleanVar(value=state["opl_auto"])
    m_ps2.add_checkbutton(label="Rename games for OPL automatically", variable=opl_auto_var,
                          command=lambda: set_opl_auto())
    m_ps2.add_separator()
    m_ps2.add_command(label="Exit ps2xfer on the PS2...", command=lambda: exit_ps2())
    menu.add_cascade(label="PS2", menu=m_ps2)
    show_log = tk.BooleanVar(value=cfg.get("show_log", False))
    m_view = tk.Menu(menu, tearoff=0)
    m_view.add_checkbutton(label="Log", variable=show_log, command=lambda: place_log())
    menu.add_cascade(label="View", menu=m_view)
    root.configure(menu=menu)

    # ---- header card
    head = ttk.Frame(root, style="Card.TFrame", padding=(px(16), px(12)))
    head.pack(fill="x")
    ttk.Label(head, text="ps2xfer", style="Title.TLabel").grid(row=0, column=0, rowspan=2, sticky="w")
    ttk.Label(head, text=f"v{VERSION}", style="Dim.TLabel").grid(row=0, column=1, rowspan=2, sticky="sw",
                                                                 padx=(px(4), px(24)), pady=(0, px(5)))
    dot = tk.Canvas(head, width=px(12), height=px(12), bg=CARD, highlightthickness=0)
    dot.grid(row=0, column=2, sticky="w")
    dot_id = dot.create_oval(1, 1, px(11), px(11), fill="#9ca3af", outline="")
    conn_var = tk.StringVar(value="Not connected")
    ttk.Label(head, textvariable=conn_var, style="Head.TLabel").grid(row=0, column=3, sticky="w", padx=px(6))
    disk_var = tk.StringVar(value="Enter the PS2's IP (empty: search the network) and press Connect")
    ttk.Label(head, textvariable=disk_var, style="Dim.TLabel").grid(row=1, column=2, columnspan=2, sticky="w")
    head.columnconfigure(4, weight=1)
    disk_bar = ttk.Progressbar(head, style="Disk.Horizontal.TProgressbar", maximum=1000, length=px(220))
    disk_bar.grid(row=0, column=5, sticky="e", padx=(0, px(16)))
    free_var = tk.StringVar(value="")
    ttk.Label(head, textvariable=free_var, style="Dim.TLabel").grid(row=1, column=5, sticky="e", padx=(0, px(16)))
    ttk.Label(head, text="PS2 IP", style="Dim.TLabel").grid(row=0, column=6, sticky="w")
    host_var = tk.StringVar(value=args.host or cfg.get("host", ""))
    host_entry = ttk.Entry(head, textvariable=host_var, width=15)
    host_entry.grid(row=1, column=6, sticky="w")
    host_entry.bind("<Return>", lambda e: do_connect())
    connect_btn = ttk.Button(head, text="Connect", style="Accent.TButton", command=lambda: do_connect())
    connect_btn.grid(row=0, column=7, rowspan=2, padx=(px(8), 0))
    tk.Frame(root, height=1, bg=LINE).pack(fill="x")

    # ---- log (packed at the bottom when shown)
    log_frame = ttk.Frame(root, style="Card.TFrame", padding=(px(12), px(6)))
    log_box = tk.Text(log_frame, height=7, state="disabled", relief="flat", bg="#f8fafc", fg=TEXT,
                      font=("Consolas" if sys.platform == "win32" else "TkFixedFont", 9),
                      highlightthickness=1, highlightbackground=LINE)
    log_box.pack(fill="both", expand=True)

    # ---- status bar
    status_var = tk.StringVar(value="Ready")
    status = ttk.Frame(root, padding=(px(12), px(3)))
    status.pack(side="bottom", fill="x")
    ttk.Label(status, textvariable=status_var, foreground=DIM, font=F_S).pack(side="left")
    ttk.Label(status, text="Drop .iso files on the window to send them" if sys.platform == "win32" else "",
              foreground=DIM, font=F_S).pack(side="right")

    # ---- transfer card
    xfer = ttk.Frame(root, style="Card.TFrame", padding=(px(16), px(10)))
    xfer.pack(side="bottom", fill="x")
    tk.Frame(root, height=1, bg=LINE).pack(side="bottom", fill="x")
    top_row = ttk.Frame(xfer, style="Card.TFrame")
    top_row.pack(fill="x")
    ttk.Label(top_row, text="Transfers", style="Head.TLabel").pack(side="left")
    verify_var = tk.BooleanVar(value=cfg.get("verify", True))
    art_var = tk.BooleanVar(value=cfg.get("art", True))
    cancel_btn = ttk.Button(top_row, text="Cancel", command=lambda: cancel(), state="disabled")
    cancel_btn.pack(side="right")
    ttk.Button(top_row, text="Clear finished", command=lambda: clear_finished()).pack(side="right", padx=px(6))
    opl_var = tk.BooleanVar(value=cfg.get("opl_names", True))
    ttk.Checkbutton(top_row, text="Download cover art", variable=art_var).pack(side="right", padx=px(10))
    ttk.Checkbutton(top_row, text="Verify after sending", variable=verify_var).pack(side="right")
    ttk.Checkbutton(top_row, text="OPL names (SLUS_xxx.xx.)", variable=opl_var).pack(side="right", padx=(0, px(10)))
    qtree = ttk.Treeview(xfer, columns=("size", "status"), height=3, selectmode="none")
    qtree.heading("#0", text="File", anchor="w")
    qtree.heading("size", text="Size", anchor="e")
    qtree.heading("status", text="Status", anchor="w")
    qtree.column("#0", width=px(560), stretch=True)
    qtree.column("size", width=px(90), anchor="e", stretch=False)
    qtree.column("status", width=px(220), stretch=False)
    for tag, color in (("wait", DIM), ("run", ACCENT), ("ok", OK), ("bad", BAD)):
        qtree.tag_configure(tag, foreground=color)
    qtree.pack(fill="x", pady=(px(6), px(8)))
    cur_var = tk.StringVar(value="Idle")
    row = ttk.Frame(xfer, style="Card.TFrame")
    row.pack(fill="x")
    ttk.Label(row, textvariable=cur_var, style="Card.TLabel").pack(side="left")
    speed_var = tk.StringVar(value="")
    ttk.Label(row, textvariable=speed_var, style="Card.TLabel").pack(side="right")
    pbar = ttk.Progressbar(xfer, style="Bar.Horizontal.TProgressbar", maximum=1000)
    pbar.pack(fill="x", pady=(px(4), px(4)))
    row2 = ttk.Frame(xfer, style="Card.TFrame")
    row2.pack(fill="x")
    total_var = tk.StringVar(value="")
    ttk.Label(row2, textvariable=total_var, style="Dim.TLabel").pack(side="left")
    tbar = ttk.Progressbar(row2, style="Thin.Horizontal.TProgressbar", maximum=1000, length=px(240))
    tbar.pack(side="right", pady=px(2))

    # ---- body: folders + games
    body = ttk.Frame(root, padding=(0, 0))
    body.pack(fill="both", expand=True)
    side = ttk.Frame(body, padding=(px(10), px(10), px(4), px(10)))
    side.pack(side="left", fill="y")
    ttk.Label(side, text="ON THE PS2", style="Side.TLabel").pack(anchor="w", padx=px(6), pady=(0, px(4)))
    folders = ttk.Treeview(side, style="Side.Treeview", show="tree", selectmode="browse", height=6)
    folders.column("#0", width=px(150))
    for iid, text in (("/DVD", "DVD games"), ("/CD", "CD games"), ("/ART", "Cover art"),
                      ("/APPS", "Apps"), ("/", "All files")):
        folders.insert("", "end", iid=iid, text="  " + text)
    folders.pack(fill="y", expand=True)

    main = ttk.Frame(body, padding=(px(6), px(10), px(12), px(8)))
    main.pack(side="left", fill="both", expand=True)
    tools = ttk.Frame(main)
    tools.pack(fill="x", pady=(0, px(8)))
    ttk.Button(tools, text="+  Add games", style="Accent.TButton", command=lambda: add_files()).pack(side="left")
    ttk.Button(tools, text="Add folder", command=lambda: add_folder()).pack(side="left", padx=(px(6), px(14)))
    sel_buttons = []
    for text, cmd in (("Rename", lambda: rename()), ("Delete", lambda: delete()),
                      ("Copy to PC", lambda: copy_to_pc()), ("Verify", lambda: verify_selected()),
                      ("Get art", lambda: art_selected())):
        b = ttk.Button(tools, text=text, command=cmd, state="disabled")
        b.pack(side="left", padx=(0, px(4)))
        sel_buttons.append(b)
    filter_var = tk.StringVar()
    fentry = ttk.Entry(tools, textvariable=filter_var, width=22)
    fentry.pack(side="right")
    hint = tk.Label(fentry, text="Search", fg="#9aa3af", bg="#ffffff", font=F)
    hint.place(x=px(6), rely=0.5, anchor="w")
    hint.bind("<Button-1>", lambda e: fentry.focus_set())

    def on_filter(*_):
        if filter_var.get():
            hint.place_forget()
        else:
            hint.place(x=px(6), rely=0.5, anchor="w")
        fill_table()
    filter_var.trace_add("write", on_filter)
    fentry.bind("<Escape>", lambda e: filter_var.set(""))

    NOTE_BG, NOTE_FG = "#fef3c7", "#92400e"
    notice = tk.Frame(main, bg=NOTE_BG, padx=px(10), pady=px(6))
    notice_var = tk.StringVar()
    tk.Label(notice, textvariable=notice_var, bg=NOTE_BG, fg=NOTE_FG, font=F).pack(side="left")
    ttk.Button(notice, text="Rename for OPL", command=lambda: fix_names()).pack(side="right")
    CW, CH = px(150), px(214)                    # cover preview box (OPL covers are ~0.7:1)
    preview = ttk.Frame(main, style="Card.TFrame", padding=px(10))
    cover = tk.Canvas(preview, width=CW, height=CH, bg="#eef0f3", highlightthickness=0)
    cover.pack()
    cover_txt = cover.create_text(CW // 2, CH // 2, text="", fill=DIM, font=F_S, width=CW - px(20),
                                  justify="center")
    cover_img = cover.create_image(CW // 2, CH // 2, anchor="center")
    p_title = ttk.Label(preview, style="Head.TLabel", wraplength=CW, justify="left")
    p_title.pack(anchor="w", pady=(px(8), 0))
    p_info = ttk.Label(preview, style="Dim.TLabel", wraplength=CW, justify="left")
    p_info.pack(anchor="w")
    preview.pack(side="right", fill="y", padx=(px(8), 0))
    table_frame = ttk.Frame(main)
    table_frame.pack(fill="both", expand=True)
    cols = ("id", "size", "art", "date")
    tree = ttk.Treeview(table_frame, columns=cols, selectmode="extended")
    heads = {"#0": "Name", "id": "Game ID", "size": "Size", "art": "Cover", "date": "Date"}
    keys = {"#0": "name", "id": "id", "size": "size", "art": "art", "date": "date"}
    for c, t in heads.items():
        tree.heading(c, text=t, anchor="e" if c == "size" else "w",
                     command=lambda c=c: sort_by(keys[c]))
    tree.column("#0", width=px(260), minwidth=px(160), stretch=True)
    tree.column("id", width=px(110), stretch=False)
    tree.column("size", width=px(90), anchor="e", stretch=False)
    tree.column("art", width=px(60), anchor="center", stretch=False)
    tree.column("date", width=px(100), stretch=False)
    tree.tag_configure("odd", background="#f9fafb")
    tree.tag_configure("dir", foreground=ACCENT)
    tree.tag_configure("other", foreground=DIM)
    vsb = ttk.Scrollbar(table_frame, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=vsb.set)
    vsb.pack(side="right", fill="y")
    tree.pack(side="left", fill="both", expand=True)
    empty_var = tk.StringVar(value="")
    empty_lbl = tk.Label(table_frame, textvariable=empty_var, bg=CARD, fg=DIM, font=F)
    empty_lbl.place(relx=0.5, rely=0.4, anchor="center")

    ctx = tk.Menu(root, tearoff=0)
    for text, cmd in (("Rename", lambda: rename()), ("Delete", lambda: delete()),
                      ("Copy to PC...", lambda: copy_to_pc()), ("Verify against the PC file...",
                                                                lambda: verify_selected()),
                      ("Download art", lambda: art_selected())):
        ctx.add_command(label=text, command=cmd)

    # ---- log / progress plumbing
    def log(text):
        def f():
            log_box.configure(state="normal")
            log_box.insert("end", text.rstrip("\n") + "\n")
            log_box.see("end")
            log_box.configure(state="disabled")
        ui_q.put(f)

    def place_log():
        if show_log.get():
            log_frame.pack(side="bottom", fill="x", before=status)
        else:
            log_frame.pack_forget()

    place_log()

    def progress(label, done, size, cur, avg, eta, retx):
        def f():
            verifying = label.startswith("verify ")
            name = label.split("/")[-1]
            cur_var.set(("Verifying  " if verifying else "Copying  " if label.startswith("copy ")
                         else "Sending  ") + name)
            pbar.configure(style="Verify.Horizontal.TProgressbar" if verifying else "Bar.Horizontal.TProgressbar")
            pbar["value"] = 1000 * done / size if size else 0
            e = int(eta)
            pct = 100 * done / size if size else 0
            speed_var.set(f"{pct:5.1f} %    {fmt_size(done)} of {fmt_size(size)}    {avg / 1e6:.1f} MB/s    "
                          f"{e // 3600}:{e // 60 % 60:02d}:{e % 60:02d} left")
            if not verifying and not label.startswith("copy ") and state["batch_total"]:
                d = state["batch_done"] + done
                tbar["value"] = 1000 * d / state["batch_total"]
                total_var.set(f"Queue: {fmt_size(d)} of {fmt_size(state['batch_total'])}")
        ui_q.put(f)

    PROGRESS_HOOK = progress

    class LogWriter:
        """console output -> the log box (progress lines with \\r are dropped)"""
        def __init__(self):
            self.buf = ""

        def write(self, text):
            self.buf += text
            while "\n" in self.buf:
                line, self.buf = self.buf.split("\n", 1)
                line = line.split("\r")[-1]
                if line.strip():
                    log(line)
            if "\r" in self.buf:
                self.buf = self.buf.split("\r")[-1]
            return len(text)

        def flush(self):
            pass

    sys.stdout = LogWriter()
    if sys.stderr is None:          # the windowed .exe has no console
        sys.stderr = sys.stdout

    # ---- worker
    def set_busy(b):
        state["busy"] = b
        cancel_btn.configure(state="normal" if b else "disabled")

    def worker():
        while True:
            desc, fn = work_q.get()
            CANCEL.clear()
            ui(set_busy, True)
            ui(status_var.set, desc)
            try:
                fn()
            except PxError as e:
                log(f"{desc}: {e}")
                ui(status_var.set, f"{desc} {e}")
                if str(e) != "cancelled" and not str(e).startswith("not connected"):
                    ui(messagebox.showerror, "ps2xfer", f"{desc}\n\n{e}")
            except Exception as e:     # keep the window alive, show what happened
                log(f"{desc}: {type(e).__name__}: {e}")
                ui(messagebox.showerror, "ps2xfer", f"{desc}\n\n{type(e).__name__}: {e}")
            if work_q.empty():
                ui(set_busy, False)
                ui(idle_view)

    threading.Thread(target=worker, daemon=True).start()

    def job(desc, fn):
        work_q.put((desc, fn))

    def idle_view():
        if not state["busy"]:
            cur_var.set("Idle")
            speed_var.set("")
            pbar["value"] = 0
            if all(qtree.item(i, "tags")[0] in ("ok", "bad") for i in qtree.get_children()):
                state["batch_total"] = state["batch_done"] = 0
                tbar["value"] = 0
                total_var.set("")

    def conn():
        if state["conn"] is None:
            raise PxError("not connected: press Connect")
        return state["conn"]

    def run_args(**kw):
        a = argparse.Namespace(**vars(args))
        a.quiet = False
        a.verify = verify_var.get()
        a.verify_full = False
        a.no_art = not art_var.get()
        a.keep_name = not opl_var.get()
        a.overwrite = False
        for key, v in kw.items():
            setattr(a, key, v)
        return a

    def save_cfg():
        cfg["verify"] = verify_var.get()
        cfg["art"] = art_var.get()
        cfg["show_log"] = show_log.get()
        cfg["opl_names"] = opl_var.get()
        cfg["opl_auto"] = state["opl_auto"]
        cfg["geometry"] = root.geometry()
        if len(sent_map) > 500:
            for key in list(sent_map)[:len(sent_map) - 500]:
                del sent_map[key]
        try:
            with open(cfg_path, "w") as f:
                json.dump(cfg, f)
        except OSError:
            pass

    # ---- connection
    def show_info(c):
        i = c.info
        ip = socket.inet_ntoa(struct.pack(">I", i["ip"]))
        good = i["link"] & 0x0F == 8
        dot.itemconfigure(dot_id, fill=OK if good else WARN)
        conn_var.set(f"PS2 {ip}  ·  {link_text(i['link'])}")
        disk_var.set(f"{i['model'] or 'disk ?'}  ·  ps2xfer {i['version']} on the PS2")
        if i["fs_free_mb"] != M32 and i["fs_total_mb"]:
            used = i["fs_total_mb"] - i["fs_free_mb"]
            disk_bar["value"] = 1000 * used / i["fs_total_mb"]
            free_var.set(f"{fmt_size(i['fs_free_mb'] << 20)} free of {fmt_size(i['fs_total_mb'] << 20)}")
        host_var.set(c.host)
        connect_btn.configure(text="Reconnect", style="TButton")

    def do_connect():
        h = host_var.get().strip() or None

        def fn():
            c = Conn(h, args.port)
            c.hello()
            state["conn"] = c
            cfg["host"] = c.host
            ui(show_info, c)
            log(f"connected to {c.host} (ps2xfer {c.info['version']})")
            list_folder()
        conn_var.set("Connecting...")
        job("Connecting...", fn)

    def update_free():
        try:
            total, free = struct.unpack_from("<QQ", conn().call(T_DF, timeout=30.0))
            conn().info["fs_total_mb"], conn().info["fs_free_mb"] = total >> 20, free >> 20
            ui(show_info, conn())
        except (PxError, struct.error):
            pass

    # ---- the games table
    def folder():
        return state["folder"]      # kept by the Tk thread; the worker must not ask Tk

    def read_items(c, fold):
        items, cursor = [], 0
        while True:
            r = c.call(T_LIST, struct.pack("<I", cursor) + Conn.path(fold))
            nxt, count, _ = struct.unpack_from("<IHH", r)
            pos = 8
            for _ in range(count):
                size, ft, attr, nl = struct.unpack_from("<QIBB", r, pos)
                items.append((r[pos + 14:pos + 14 + nl].decode("utf-8", "replace"), size, attr, ft))
                pos += 14 + nl
            if nxt == 0:
                return items
            cursor = nxt

    def list_folder():
        fold = folder()
        c = conn()
        items = read_items(c, fold)
        games_folder = fold in ("/DVD", "/CD")

        def pending(its):
            return [n for n, *_ in its if opl_needs_name(n) and f"{fold}/{n}" not in state["no_id"]]
        if games_folder and state["opl_auto"] and pending(items):
            log(f"renaming games in {fold} for OPL")
            if opl_fix_names(c, (fold,), log=log, skip=state["no_id"]):
                items = read_items(c, fold)
        covers = {}
        if games_folder:      # game ID -> its cover file in /ART
            covers = {n[:11].upper(): n for n in ps2_listdir(c, "/ART") if n[11:16].upper() == "_COV."}
        plain = len(pending(items)) if games_folder else 0

        def f():
            state["items"], state["covers"], state["listed"], state["plain"] = items, covers, fold, plain
            fill_table()
            show_notice()
        ui_q.put(f)

    def show_notice():
        if state["plain"] and not state["opl_auto"]:
            notice_var.set(f"{state['plain']} game(s) here have names OPL does not list from the HDD "
                           f"(it wants SLUS_xxx.xx.Name.iso).")
            notice.pack(fill="x", pady=(0, px(8)), before=preview)
        else:
            notice.pack_forget()

    def set_opl_auto():
        state["opl_auto"] = opl_auto_var.get()
        if state["opl_auto"] and state["conn"] is not None:
            refresh()
        else:
            show_notice()

    def fix_names():
        def fn():
            done = opl_fix_names(conn(), log=log, skip=state["no_id"])
            log(f"renamed {len(done)} game(s) for OPL")
            ui(status_var.set, f"Renamed {len(done)} game(s) for OPL")
            list_folder()
        job("Renaming games for OPL...", fn)

    def fill_table():
        fold = state.get("listed", folder())
        flt = filter_var.get().strip().lower()
        rows = []
        for name, size, attr, ft in state["items"]:
            isdir = bool(attr & 0x10)
            gid, shown = "", name
            if is_opl_old_name(name):
                gid, shown = name[:11].upper(), name[12:]
            if flt and flt not in name.lower():
                continue
            d = ft >> 16
            date = f"{1980 + (d >> 9)}-{(d >> 5) & 15:02d}-{d & 31:02d}" if d else ""
            game = name.lower().endswith((".iso", ".zso"))
            art = ("✓" if gid in state["covers"] else "–") if gid and fold in ("/DVD", "/CD") else ""
            rows.append(dict(iid=name, name=shown.lower(), shown=shown, id=gid, size=-1 if isdir else size,
                             art=art, date=date, isdir=isdir, game=game))
        key, rev = state["sort"]
        rows.sort(key=lambda r: (not r["isdir"], r[key] if key != "name" else r["name"]), reverse=rev)
        if rev:
            rows.sort(key=lambda r: not r["isdir"])
        tree.delete(*tree.get_children())
        for n, r in enumerate(rows):
            tags = ["odd"] if n % 2 else []
            if r["isdir"]:
                tags.append("dir")
            elif fold in ("/DVD", "/CD") and not r["game"]:
                tags.append("other")
            tree.insert("", "end", iid=r["iid"], text=("▸ " if r["isdir"] else "") + r["shown"],
                        values=(r["id"], "" if r["isdir"] else fmt_size(r["size"]), r["art"], r["date"]),
                        tags=tags)
        games = sum(1 for r in rows if r["game"])
        if state["conn"] is None:
            empty_var.set("Not connected")
        elif not rows:
            empty_var.set("Empty here. Add games with the button above" +
                          (" or drop .iso files on the window." if sys.platform == "win32" else "."))
        else:
            empty_var.set("")
        if empty_var.get():
            empty_lbl.place(relx=0.5, rely=0.4, anchor="center")
        else:
            empty_lbl.place_forget()
        status_var.set(f"{fold}: {len(rows)} item(s)" + (f", {games} game(s)" if games else ""))
        on_select()

    def sort_by(key):
        cur, rev = state["sort"]
        state["sort"] = (key, not rev if cur == key else False)
        fill_table()

    def refresh():
        job("Listing...", list_folder)

    def on_select(*_):
        show_preview()
        n = len(tree.selection())
        for b in sel_buttons:
            b.configure(state="normal" if n else "disabled")
        if n != 1:
            sel_buttons[0].configure(state="disabled")    # Rename: one at a time

    tree.bind("<<TreeviewSelect>>", on_select)

    # ---- cover preview (own thread and own PS2 session; JPG decoded by GDI+)
    cover_dir = os.path.join(os.path.expanduser("~"), ".ps2xfer", "covers")
    cover_imgs: dict = {}            # game ID -> PhotoImage, or None = no cover anywhere
    cover_q: "queue.Queue" = queue.Queue()

    def show_preview():
        fold = state.get("listed", folder())
        if fold not in ("/DVD", "/CD"):
            preview.pack_forget()
            return
        if not preview.winfo_ismapped():
            preview.pack(side="right", fill="y", padx=(px(8), 0), before=table_frame)
        sel = tree.selection()
        row = next((i for i in state["items"] if len(sel) == 1 and i[0] == sel[0]), None)
        cover.itemconfigure(cover_img, image="")
        if row is None:
            cover.itemconfigure(cover_txt, text="Select a game to see its cover" if not sel else "")
            p_title.configure(text=f"{len(sel)} selected" if len(sel) > 1 else "")
            total = sum(i[1] for i in state["items"] if i[0] in sel)
            p_info.configure(text=fmt_size(total) if len(sel) > 1 else "")
            return
        name, size, attr, ft = row
        gid = name[:11].upper() if is_opl_old_name(name) else ""
        p_title.configure(text=name[12:].rsplit(".", 1)[0] if gid else name)
        p_info.configure(text="  ·  ".join(x for x in (gid, fmt_size(size)) if x))
        if not gid:
            cover.itemconfigure(cover_txt, text="No game ID in the name")
            return
        if gid in cover_imgs:
            set_cover(gid, cover_imgs[gid])
            return
        cover.itemconfigure(cover_txt, text="Loading cover...")
        cover_q.put((gid, state["covers"].get(gid), state["busy"]))

    def set_cover(gid, img):
        cover_imgs[gid] = img
        sel = tree.selection()
        if len(sel) != 1 or sel[0][:11].upper() != gid:
            return                       # the selection moved on meanwhile
        if img is None:
            cover.itemconfigure(cover_txt, text="No cover found\n\nGet art downloads it")
            cover.itemconfigure(cover_img, image="")
        else:
            cover.itemconfigure(cover_txt, text="")
            cover.itemconfigure(cover_img, image=img)

    def cover_png(data):
        if sys.platform == "win32":
            return _win_cover_png(data, CW, CH)
        return data if data[:8] == b"\x89PNG\r\n\x1a\n" else None    # Tk reads PNG, not JPG

    def make_image(png):
        import base64
        img = tk.PhotoImage(data=base64.b64encode(png).decode())
        if sys.platform != "win32":      # unscaled PNG: shrink by an integer factor
            f = max(1, -(-img.width() // CW), -(-img.height() // CH))
            if f > 1:
                img = img.subsample(f)
        return img

    def cover_loader():
        c2 = None
        while True:
            gid, fname, busy = cover_q.get()
            if not cover_q.empty():
                continue                 # only the newest selection matters
            data = None
            cached = [f for f in (os.listdir(cover_dir) if os.path.isdir(cover_dir) else [])
                      if f.upper().startswith(gid + "_COV.")]
            if cached:
                with open(os.path.join(cover_dir, cached[0]), "rb") as f:
                    data = f.read()
            if data is None and fname and not busy and state["conn"] is not None:
                try:                     # from /ART on the PS2 (not during a transfer)
                    if c2 is None or c2.host != state["conn"].host:
                        c2 = Conn(state["conn"].host, args.port)
                        c2.hello()
                    buf = bytearray()
                    Downloader(c2, "/ART/" + fname, buf.extend, quiet=True).run()
                    data = bytes(buf)
                except (PxError, OSError):
                    c2, data = None, None
            if data is None:             # the same sources art_for_game uses
                try:
                    r = fetch_art(gid, "COV")
                except Exception:
                    r = None
                if r:
                    data, ext = r
                    fname = f"{gid}_COV.{ext}"
            if data is not None and not cached:
                try:
                    os.makedirs(cover_dir, exist_ok=True)
                    with open(os.path.join(cover_dir, fname or f"{gid}_COV.jpg"), "wb") as f:
                        f.write(data)
                except OSError:
                    pass
            png = cover_png(data) if data else None

            def done(gid=gid, png=png):
                try:
                    set_cover(gid, make_image(png) if png else None)
                except tk.TclError:
                    set_cover(gid, None)
            ui_q.put(done)

    threading.Thread(target=cover_loader, daemon=True).start()

    def popup(e):
        iid = tree.identify_row(e.y)
        if iid:
            if iid not in tree.selection():
                tree.selection_set(iid)
            ctx.tk_popup(e.x_root, e.y_root)

    tree.bind("<Button-3>", popup)
    tree.bind("<Delete>", lambda e: delete())
    tree.bind("<F2>", lambda e: rename())
    tree.bind("<Double-1>", lambda e: open_dir(tree.identify_row(e.y)))
    root.bind("<F5>", lambda e: refresh())
    root.bind("<Control-o>", lambda e: add_files())

    def open_dir(iid):
        if not iid:
            return
        for name, size, attr, ft in state["items"]:
            if name == iid and attr & 0x10:
                path = (folder().rstrip("/") + "/" + name)
                if path not in folders.get_children():
                    folders.insert("", "end", iid=path, text="  " + path)
                folders.selection_set(path)
                return

    # ---- send queue
    def queue_status(qid, text, tag):
        def f():
            if qtree.exists(qid):
                qtree.set(qid, "status", text)
                qtree.item(qid, tags=(tag,))
                qtree.see(qid)
        ui_q.put(f)

    def add_paths(paths):
        try:
            files = plan_files(paths, None)
        except PxError as e:
            messagebox.showerror("ps2xfer", str(e))
            return
        if not files:
            return
        if state["conn"] is None:
            messagebox.showinfo("ps2xfer", "Connect to the PS2 first.")
            return
        for src, dst, size in files:
            qid = qtree.insert("", "end", text=f"{os.path.basename(src)}   →   {dst.rsplit('/', 1)[0]}/",
                               values=(fmt_size(size), "Waiting"), tags=("wait",))
            state["batch_total"] += size
            job_send(qid, src, dst, size)
        total_var.set(f"Queue: {fmt_size(state['batch_done'])} of {fmt_size(state['batch_total'])}")
        log(f"queued {len(files)} file(s), {fmt_size(sum(x[2] for x in files))}")

    def job_send(qid, src, dst, size):
        def fn():
            a = run_args()          # the check boxes as they are now, for this file

            def stage(s, d, what):
                sent_map[d] = s
                text = {"send": "Sending", "verify": "Verifying", "art": "Getting cover art",
                        "done": "Done" + (", verified" if a.verify else "")}[what]
                queue_status(qid, text, "ok" if what == "done" else "run")
            try:
                send_planned(conn(), a, [(src, dst, size)], log, stage)
            except PxError as e:
                queue_status(qid, "Cancelled (resumes next time)" if str(e) == "cancelled" else f"Failed: {e}",
                             "bad")
                raise
            finally:
                state["batch_done"] += size
            if work_q.empty():
                list_folder()
                update_free()
        job(f"Sending {os.path.basename(src)}...", fn)

    def cancel():
        CANCEL.set()
        while True:                    # drop what has not started yet
            try:
                work_q.get_nowait()
            except queue.Empty:
                break
        for i in qtree.get_children():
            if qtree.set(i, "status") == "Waiting":
                qtree.set(i, "status", "Cancelled")
                qtree.item(i, tags=("bad",))

    def clear_finished():
        for i in qtree.get_children():
            if qtree.item(i, "tags")[0] in ("ok", "bad"):
                qtree.delete(i)
        if not qtree.get_children():
            state["batch_total"] = state["batch_done"] = 0
            tbar["value"] = 0
            total_var.set("")

    def add_files():
        paths = filedialog.askopenfilenames(title="Games or files to send to the PS2",
                                            initialdir=cfg.get("last_dir") or None,
                                            filetypes=[("PS2 games", "*.iso *.zso"), ("All files", "*.*")])
        if paths:
            cfg["last_dir"] = os.path.dirname(paths[0])
            add_paths(list(paths))

    def add_folder():
        d = filedialog.askdirectory(title="Folder: its .iso/.zso go to CD/DVD",
                                    initialdir=cfg.get("last_dir") or None)
        if d:
            cfg["last_dir"] = d
            add_paths([d])

    def on_drop(paths):
        if not paths:
            return
        shown = "\n".join(os.path.basename(p) for p in paths[:12]) + ("\n..." if len(paths) > 12 else "")
        if messagebox.askyesno("ps2xfer", f"Send to the PS2?\n\n{shown}"):
            add_paths(paths)

    _win_enable_drop(root, lambda files: ui_q.put(lambda: on_drop(files)))

    # ---- actions on the selection
    def selected():
        fold = folder().rstrip("/")
        return [(n, f"{fold}/{n}") for n in tree.selection()]

    def delete():
        sel = selected()
        if not sel or not messagebox.askyesno("Delete", "Delete on the PS2?\n\n" + "\n".join(n for n, _ in sel)):
            return

        def fn():
            for n, pth in sel:
                conn().call(T_DELETE, Conn.path(pth))
                sent_map.pop(pth, None)
                log(f"deleted {pth}")
            list_folder()
            update_free()
        job("Deleting...", fn)

    def rename():
        sel = selected()
        if len(sel) != 1:
            return
        n, pth = sel[0]
        prefix = n[:12] if is_opl_old_name(n) else ""
        new = simpledialog.askstring("Rename", "New name:", initialvalue=n[len(prefix):], parent=root)
        if not new or new == n[len(prefix):]:
            return
        dst = pth.rsplit("/", 1)[0] + "/" + prefix + new

        def fn():
            conn().call(T_RENAME, Conn.path(pth) + Conn.path(dst))
            if pth in sent_map:
                sent_map[dst] = sent_map.pop(pth)
            log(f"{pth} -> {dst}")
            list_folder()
        job("Renaming...", fn)

    def copy_to_pc():
        sel = [x for x in selected() if x[0] in {i[0] for i in state["items"] if not i[2] & 0x10}]
        if not sel:
            return
        d = filedialog.askdirectory(title="Copy to", initialdir=cfg.get("last_dir") or None)
        if not d:
            return

        def fn():
            for n, pth in sel:
                st_ = conn().call(T_STAT, Conn.path(pth))
                size = struct.unpack_from("<Q", st_)[0]
                local = os.path.join(d, n[12:] if is_opl_old_name(n) else n)
                with open(local + ".part", "wb") as f:
                    Downloader(conn(), pth, f.write, size=size, label=f"copy {pth}").run()
                os.replace(local + ".part", local)
                log(f"copied {pth} -> {local}")
        job("Copying to the PC...", fn)

    def verify_selected():
        todo = []
        for n, pth in selected():
            src = sent_map.get(pth)
            if not src or not os.path.isfile(src):
                src = filedialog.askopenfilename(title=f"The original of {n} on this PC",
                                                 initialdir=cfg.get("last_dir") or None,
                                                 filetypes=[("PS2 games", "*.iso *.zso"), ("All files", "*.*")])
                if not src:
                    continue
                sent_map[pth] = src
            todo.append((n, pth, src))

        def fn():
            for n, pth, src in todo:
                verify_file(conn(), src, pth)
                log(f"{pth}: verify OK")
                ui(messagebox.showinfo, "Verify", f"{n}\n\nidentical to {src}")
        if todo:
            job("Verifying...", fn)

    def art_selected():
        ids = sorted({n[:11].upper() for n, _ in selected() if is_opl_old_name(n)})
        if not ids:
            return

        def fn():
            have = {n.rsplit(".", 1)[0] for n in ps2_listdir(conn(), "/ART")}
            for gid in ids:
                got = art_for_game(conn(), run_args(quiet=True), gid, have)
                log(f"{gid}: {', '.join(got) if got else 'nothing new'}")
            list_folder()
        job("Downloading art...", fn)

    def download_art():
        def fn():
            c = conn()
            have = {n.rsplit(".", 1)[0] for n in ps2_listdir(c, "/ART")}
            for fold in ("/CD", "/DVD"):
                for n in ps2_listdir(c, fold):
                    _check_cancel()
                    if not n.lower().endswith((".iso", ".zso")):
                        continue
                    gid = n[:11].upper() if is_opl_old_name(n) else (
                        remote_game_id(c, f"{fold}/{n}") if n.lower().endswith(".iso") else None)
                    if gid:
                        ui(status_var.set, f"Art for {gid}...")
                        got = art_for_game(c, run_args(quiet=True), gid, have)
                        log(f"{gid}: {', '.join(got) if got else 'nothing new'}")
            log("art done")
            list_folder()
        job("Downloading art...", fn)

    def exit_ps2():
        if messagebox.askyesno("Exit", "Leave ps2xfer on the PS2?\n\nAn unfinished transfer resumes next time."):
            job("Exit", lambda: (conn().call(T_EXIT), log("PS2 is leaving ps2xfer"),
                                 ui(conn_var.set, "PS2 left ps2xfer"),
                                 ui(dot.itemconfigure, dot_id, {"fill": "#9ca3af"})))

    # ---- loop
    def pump():
        try:
            while True:
                ui_q.get_nowait()()
        except queue.Empty:
            pass
        root.after(80, pump)

    def on_close():
        if state["busy"] and not messagebox.askyesno("Quit", "A transfer is running. Quit anyway?\n\n"
                                                     "The interrupted file resumes next time."):
            return
        save_cfg()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    folders.selection_set(cfg.get("folder", "/DVD") if cfg.get("folder") in ("/DVD", "/CD", "/ART", "/APPS", "/")
                          else "/DVD")
    def on_folder(_e=None):
        sel = folders.selection()
        state["folder"] = sel[0] if sel else "/DVD"
        cfg["folder"] = state["folder"]
        if state["conn"]:
            refresh()
    folders.bind("<<TreeviewSelect>>", on_folder)
    state["folder"] = folders.selection()[0] if folders.selection() else "/DVD"
    fill_table()
    pump()
    if host_var.get():
        do_connect()
    root.mainloop()


# --------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(prog="ps2xfer", description="Send files to a PS2 running ps2xfer.elf")
    p.add_argument("--host", help="PS2 IP address (default: broadcast discovery)")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("-q", "--quiet", action="store_true")
    p.add_argument("--rate", type=float, default=11.5, help="max payload rate, MB/s (default 11.5)")
    p.add_argument("--rate-start", type=float, default=9.0, help="initial rate, MB/s (default 9)")
    p.add_argument("--burst", type=int, default=2, help="packets per pacing tick (default 2)")
    p.add_argument("--window", type=int, default=4096, help="max packets in flight")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("send", help="send files/folders")
    s.add_argument("files", nargs="+")
    s.add_argument("--dest", help="destination path on the PS2 (dir ends with /)")
    s.add_argument("--overwrite", action="store_true")
    s.add_argument("--no-art", action="store_true", help="do not download OPL art for sent games")
    s.add_argument("--keep-name", action="store_true",
                   help="do not prefix .iso names with the game ID (OPL old naming)")
    s.add_argument("--verify", action="store_true", help="read the file back on the PS2 and compare")
    s.add_argument("--verify-full", action="store_true",
                   help="with --verify: send the file back to the PC and compare SHA-256")
    s.set_defaults(func=cmd_send)

    s = sub.add_parser("verify", help="compare a local file with one on the PS2")
    s.add_argument("local")
    s.add_argument("remote")
    s.add_argument("--full", action="store_true", help="read the whole file back and compare SHA-256")
    s.set_defaults(func=cmd_verify)

    sub.add_parser("gui", help="open the window (also when started without arguments)").set_defaults(func=cmd_gui)

    s = sub.add_parser("art", help="download OPL art (covers, icons, backgrounds...) for the games on the PS2")
    s.add_argument("--overwrite", action="store_true")
    s.set_defaults(func=cmd_art)

    s = sub.add_parser("opl-rename", help="rename ISOs in /CD and /DVD to SLUS_xxx.xx.Name.iso")
    s.add_argument("--dry-run", action="store_true")
    s.set_defaults(func=cmd_opl_rename)

    s = sub.add_parser("ftp", help="FTP gateway for Windows Explorer (ftp://127.0.0.1:2121)")
    s.add_argument("--listen", default="127.0.0.1:2121")
    s.set_defaults(func=cmd_ftp)

    s = sub.add_parser("get", help="copy a file from the PS2 to the PC")
    s.add_argument("remote")
    s.add_argument("local", nargs="?")
    s.set_defaults(func=cmd_get)

    sub.add_parser("info").set_defaults(func=cmd_info)
    s = sub.add_parser("ping")
    s.add_argument("-c", "--count", type=int, default=4)
    s.set_defaults(func=cmd_ping)
    sub.add_parser("stats").set_defaults(func=cmd_stats)
    s = sub.add_parser("log")
    s.add_argument("-f", "--follow", action="store_true")
    s.set_defaults(func=cmd_log)
    s = sub.add_parser("set", help="change a tuning parameter on the PS2 (key=value)")
    s.add_argument("items", nargs="+")
    s.set_defaults(func=cmd_set)
    sub.add_parser("exit", help="leave ps2xfer on the PS2").set_defaults(func=cmd_exit)
    s = sub.add_parser("ls")
    s.add_argument("path", nargs="?", default="/")
    s.set_defaults(func=cmd_ls)
    sub.add_parser("df").set_defaults(func=cmd_df)
    sub.add_parser("diskinfo", help="read-only: show the partition table of the PS2 disk").set_defaults(func=cmd_diskinfo)
    s = sub.add_parser("stat")
    s.add_argument("path")
    s.set_defaults(func=cmd_stat)
    s = sub.add_parser("mkdir")
    s.add_argument("path")
    s.set_defaults(func=cmd_mkdir)
    s = sub.add_parser("rm")
    s.add_argument("paths", nargs="+")
    s.set_defaults(func=cmd_rm)
    s = sub.add_parser("mv")
    s.add_argument("src")
    s.add_argument("dst")
    s.set_defaults(func=cmd_mv)

    s = sub.add_parser("bench", help="Phase 1 benchmarks")
    s.add_argument("kind", choices=["selftest", "micro", "disk-raw", "disk-fatfs", "disk-iomanx",
                                    "net-sink", "net-sink-nodma", "combined", "fscheck"])
    s.add_argument("--size-mb", type=int, default=512)
    s.add_argument("--chunks", help="comma-separated chunk sizes for disk benches")
    s.add_argument("--count", type=int, default=50, help="selftest: thousands of packets")
    s.set_defaults(func=cmd_bench)

    if argv is None and len(sys.argv) == 1:
        argv = ["gui"]          # double-click / no arguments: the window
    args = p.parse_args(argv)
    try:
        args.func(args)
    except PxError as e:
        print(f"\nerror: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
