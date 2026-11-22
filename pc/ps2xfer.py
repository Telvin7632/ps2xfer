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
T_DATA, T_ACK = 0x10, 0x11
T_LIST, T_STAT, T_MKDIR, T_DELETE, T_RENAME, T_DF = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
T_OPEN_WRITE, T_CLOSE, T_ABORT, T_BLOCKSUMS = 0x26, 0x27, 0x28, 0x29
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

CTL = struct.Struct("<BBHII")            # type, flags, status, req_id, session
DATA = struct.Struct("<BBQIIHH")         # type, flags, offset, pkt_seq, csum, handle, len
ACK = struct.Struct("<BBHIQQQIIIHH")     # see common/proto.h
STATS = struct.Struct("<4I8s15I44s64s20I48s")
STATS_FIELDS = (
    "version flags uptime_ms ip mac port link_mode speed_rev dma_block iop_free iop_maxfree "
    "nbuf bufsize cpu_load_x10 udma_mode disk_sectors_lo disk_sectors_hi fs_total_mb fs_free_mb "
    "cluster_size disk_model file file_size_lo file_size_hi written_lo written_hi rx_bytes_lo "
    "rx_bytes_hi rx_pkts rx_dups rx_drops rx_retx rx_errors rx_overruns csum_errors ctl_drops "
    "acks_sent disk_errors prof_hdr prof_dma prof_total prof_samples last_error").split()

CD_LIMIT = 700 * 1024 * 1024
OPL_NAME_MAX = 160


class PxError(Exception):
    pass


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
                    if typ == T_HELLO:
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
    def hello(self) -> dict:
        body = struct.pack("<III", PROTO_VERSION, random.getrandbits(32), fattime_now())
        if self.addr is None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            st, r = self.request(T_HELLO, body, addr=("255.255.255.255", self.port), retries=4)
        else:
            st, r = self.request(T_HELLO, body)
        if st != 0 or len(r) < 136:
            raise PxError("bad HELLO response")
        f = struct.unpack_from("<III6sHIIIIIIIIQIII", r)
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

    def stats(self) -> dict:
        r = self.call(T_STATS)
        d = dict(zip(STATS_FIELDS, STATS.unpack_from(r)))
        for k in ("disk_model", "file", "last_error"):
            d[k] = d[k].split(b"\0")[0].decode(errors="replace")
        return d

    @staticmethod
    def path(p: str) -> bytes:
        return p.encode("utf-8") + b"\0"


def connect(args) -> Conn:
    c = Conn(args.host, args.port)
    info = c.hello()
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
        self.last_ack_time = time.monotonic()
        self.rto = 0.1
        self.rx_errors = None
        self.drops = None
        self.last_rate_up = time.monotonic()
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
            self.last_progress = time.monotonic()
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
        # rate adaptation on PS2-side errors / drops
        if self.rx_errors is not None and (rx_err > self.rx_errors or drops > self.drops):
            self.rate = max(1e6, self.rate * 0.9)
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
                continue
            break

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
                    self._poll_acks(0.002)
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
            # rate increase
            if mono - self.last_rate_up >= 1.0:
                self.rate = min(self.rate_max, self.rate + 0.25e6)
                self.last_rate_up = mono
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
        line = (f"\r{self.label} {pct:5.1f}%  {fmt_size(done)}/{fmt_size(self.size)}  "
                f"{cur / 1e6:5.2f} MB/s (avg {avg / 1e6:5.2f})  ETA {int(eta) // 60}:{int(eta) % 60:02d}  "
                f"retx {self.retransmits}")
        sys.stdout.write(line + ("\n" if final else ""))
        sys.stdout.flush()


def fingerprint(path: str) -> bytes:
    st = os.stat(path)
    h = hashlib.sha256()
    h.update(struct.pack("<QQ", st.st_size, int(st.st_mtime)))
    with open(path, "rb") as f:
        h.update(f.read(1 << 20))
    return h.digest()


def transfer(c: Conn, args, src_path: str | None, size: int, dest: str | None, flags: int,
             fp: bytes = b"\0" * 32, label: str = "") -> dict:
    body = struct.pack("<QII", size, flags, 0) + fp + (Conn.path(dest) if dest else b"\0")
    r = c.call(T_OPEN_WRITE, body, timeout=30.0, retries=3)
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
    except KeyboardInterrupt:
        try:
            c.request(T_ABORT, struct.pack("<HB", handle, 0), retries=2)
        except PxError:
            pass
        raise
    finally:
        c.ack_handler = None
        src.close()
    return {"bytes": size - start, "seconds": s.elapsed, "retransmits": s.retransmits,
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
    """(local path, PS2 path, size). With several sources --dest is a folder."""
    if dest and not dest.startswith("/"):
        dest = "/" + dest
    if dest and (len(paths) > 1 or os.path.isdir(paths[0])) and not dest.endswith("/"):
        dest += "/"
    out = []
    for p in paths:
        if os.path.isdir(p):
            root = os.path.basename(os.path.normpath(p))
            base = (dest or "/") + root
            for dirpath, _, files in os.walk(p):
                for fn in sorted(files):
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, p).replace(os.sep, "/")
                    out.append((full, base + "/" + rel, os.path.getsize(full)))
        elif os.path.isfile(p):
            size = os.path.getsize(p)
            out.append((p, opl_dest(p, size, dest), size))
        else:
            raise PxError(f"not found: {p}")
    return out


def cmd_send(args):
    files = plan_files(args.files, args.dest)
    total = sum(f[2] for f in files)
    c = connect(args)
    print(f"{len(files)} file(s), {fmt_size(total)}")
    for i, (src, dst, size) in enumerate(files, 1):
        for w in opl_warnings(dst):
            print(f"WARNING {dst}: {w}")
        if size == 0:
            print(f"skipping empty file {src}")
            continue
        flags = OW_RESUME | (OW_OVERWRITE if args.overwrite else 0)
        label = f"[{i}/{len(files)}] {dst}"
        r = transfer(c, args, src, size, dst, flags, fingerprint(src), label)
        print(f"    {fmt_size(r['bytes'])} in {r['seconds']:.1f} s = "
              f"{r['bytes'] / r['seconds'] / 1e6 if r['seconds'] else 0:.2f} MB/s, "
              f"{r['retransmits']} retransmits")
        if args.verify:
            verify_file(c, src, dst)


def verify_file(c: Conn, src: str, dst: str):
    size = os.path.getsize(src)
    nblocks = (size + (1 << 20) - 1) >> 20
    done = 0
    t0 = time.monotonic()
    while done < nblocks:
        n = min(300, nblocks - done)
        r = c.call(T_BLOCKSUMS, struct.pack("<QI", done << 20, n) + Conn.path(dst), timeout=120.0, retries=2)
        cnt = struct.unpack_from("<I", r)[0]
        remote = list(struct.unpack_from(f"<{cnt}I", r, 4))
        local = fletcher_blocks(src, done << 20, cnt)
        for k, (a, b) in enumerate(zip(local, remote)):
            if a != b:
                raise PxError(f"VERIFY FAILED: {dst} differs in MiB block {done + k}")
        if cnt == 0:
            raise PxError("VERIFY: PS2 returned no data")
        done += cnt
        sys.stdout.write(f"\r    verify {100 * done // nblocks}%")
        sys.stdout.flush()
    print(f"\r    verify OK ({nblocks} MiB blocks, {time.monotonic() - t0:.1f} s)")


def cmd_verify(args):
    c = connect(args)
    verify_file(c, args.local, args.remote)


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
    c = Conn(args.host, args.port)
    if c.addr is None:
        c.hello()
    for _ in range(args.count):
        t = time.perf_counter()
        r = c.call(T_PING, timeout=1.0, retries=1)
        print(f"reply from {c.addr[0]}: {(time.perf_counter() - t) * 1000:.2f} ms, "
              f"uptime {struct.unpack_from('<I', r)[0] / 1000:.1f} s")
        time.sleep(0.5)


def cmd_stats(args):
    c = connect(args)
    for k, v in c.stats().items():
        print(f"  {k:16} {v}")


def cmd_log(args):
    c = connect(args)
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
              f"csum errors +{s1['csum_errors'] - s0['csum_errors']}, CPU {s1['cpu_load_x10'] / 10:.1f}%")
        if s1["prof_samples"]:
            print(f"  profile (ticks/packet): header {s1['prof_hdr']}, payload DMA {s1['prof_dma']}, "
                  f"total {s1['prof_total']} ({s1['prof_samples']} samples)")
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
    s.add_argument("--verify", action="store_true", help="read the file back on the PS2 and compare")
    s.set_defaults(func=cmd_send)

    s = sub.add_parser("verify", help="compare a local file with one on the PS2")
    s.add_argument("local")
    s.add_argument("remote")
    s.set_defaults(func=cmd_verify)

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
                                    "net-sink", "net-sink-nodma", "combined"])
    s.add_argument("--size-mb", type=int, default=512)
    s.add_argument("--chunks", help="comma-separated chunk sizes for disk benches")
    s.add_argument("--count", type=int, default=50, help="selftest: thousands of packets")
    s.set_defaults(func=cmd_bench)

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
