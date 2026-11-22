#!/usr/bin/env python3
"""End-to-end tests: pc/ps2xfer.py against the host simulator (ps2xfer-sim).

Checks every transferred file with an independent exFAT reader (no FatFs):
content SHA-256 and the NoFatChain (contiguous) flag. Also covers packet loss,
client interruption + resume, simulator "power loss" + resume, benches and
fsck.exfat of the final image.
"""
import argparse
import hashlib
import os
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

PORT = 17700


# ------------------------------------------------------------------ independent exFAT reader

class ExFat:
    def __init__(self, path):
        self.f = open(path, "rb")
        s0 = self.read(0, 512)
        self.base = 0
        if s0[3:11] != b"EXFAT   ":
            assert s0[510:512] == b"\x55\xaa", "no MBR"
            self.base = struct.unpack_from("<I", s0, 446 + 8)[0] * 512
        vbr = self.read(self.base, 512)
        assert vbr[3:11] == b"EXFAT   ", "no exFAT VBR"
        (self.fat_off, self.fat_len, self.heap_off, self.nclus, self.root) = struct.unpack_from("<IIIII", vbr, 80)
        self.bps = 1 << vbr[108]
        self.cbytes = self.bps << vbr[109]

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def clus_off(self, c):
        return self.base + self.heap_off * self.bps + (c - 2) * self.cbytes

    def fat(self, c):
        return struct.unpack("<I", self.read(self.base + self.fat_off * self.bps + c * 4, 4))[0]

    def chain(self, first, nofat, length):
        n = max(1, (length + self.cbytes - 1) // self.cbytes)
        if nofat:
            return [first + i for i in range(n)]
        out, c = [], first
        while 2 <= c < 0xFFFFFFF7 and len(out) < n:
            out.append(c)
            c = self.fat(c)
        return out

    def dir_entries(self, first, nofat, length):
        data = b"".join(self.read(self.clus_off(c), self.cbytes) for c in self.chain(first, nofat, length))
        i, out = 0, []
        while i + 32 <= len(data):
            t = data[i]
            if t == 0:
                break
            if t == 0x85:
                sec = data[i + 1]
                attr = struct.unpack_from("<H", data, i + 4)[0]
                st = data[i + 32:i + 64]
                flags, nlen = st[1], st[3]
                vdl, fc, dlen = struct.unpack_from("<Q4xIQ", st, 8)
                name = b""
                for k in range(2, sec + 1):
                    e = data[i + 32 * k:i + 32 * (k + 1)]
                    name += e[2:32]
                name = name[:nlen * 2].decode("utf-16-le")
                out.append((name, attr, flags & 2 != 0, fc, dlen, vdl))
                i += 32 * (sec + 1)
            else:
                i += 32
        return out

    def lookup(self, path):
        first, nofat, length = self.root, False, 1 << 40
        ent = None
        for part in [p for p in path.split("/") if p]:
            for e in self.dir_entries(first, nofat, length):
                if e[0].lower() == part.lower():
                    ent = e
                    break
            else:
                return None
            first, nofat, length = ent[3], ent[2], ent[4]
        return ent

    def sha256(self, path):
        e = self.lookup(path)
        assert e is not None, f"{path} not found in the image"
        name, attr, nofat, fc, dlen, vdl = e
        h = hashlib.sha256()
        left = dlen
        for c in self.chain(fc, nofat, dlen):
            b = self.read(self.clus_off(c), min(self.cbytes, left))
            h.update(b)
            left -= len(b)
        assert vdl == dlen, f"{path}: ValidDataLength {vdl} != DataLength {dlen}"
        return h.hexdigest(), nofat, dlen


# ------------------------------------------------------------------ harness

class Env:
    def __init__(self, sim, client, tmp):
        self.sim_bin, self.client, self.tmp = sim, client, tmp
        self.image = os.path.join(tmp, "disk.img")
        self.proc = None

    def start(self, *extra, mkfs=None):
        args = [self.sim_bin, "--image", self.image, "--port", str(PORT)]
        if mkfs:
            args += ["--mkfs", str(mkfs)]
        args += list(extra)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        self.log = open(os.path.join(self.tmp, "sim.log"), "a")
        self.proc = subprocess.Popen(args, stdout=self.log, stderr=self.log, env=env)
        time.sleep(0.7)

    def kill(self, sig=signal.SIGKILL):
        if self.proc:
            self.proc.send_signal(sig)
            self.proc.wait(10)
            self.proc = None

    def run(self, *args, timeout=300, check=True, rate=200):
        cmd = [sys.executable, self.client, "--host", "127.0.0.1", "--port", str(PORT),
               "--rate", str(rate), "--rate-start", str(rate)] + list(args)
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = (p.stdout + p.stderr).replace("\r", "\n")
        if check and p.returncode != 0:
            raise AssertionError(f"client failed ({p.returncode}): {' '.join(args)}\n{out[-2000:]}")
        return out

    def popen(self, *args, rate=200):
        cmd = [sys.executable, self.client, "--host", "127.0.0.1", "--port", str(PORT),
               "--rate", str(rate), "--rate-start", str(rate)] + list(args)
        return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def mkfile(path, size, seed):
    rnd = hashlib.sha256(str(seed).encode()).digest()
    with open(path, "wb") as f:
        left = size
        block = (rnd * (1 << 15))[: 1 << 20]
        i = 0
        while left > 0:
            b = block[i % 997:] + block[:i % 997]
            f.write(b[:min(left, len(b))])
            left -= min(left, len(b))
            i += 1
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def check_image(env, files):
    fs = ExFat(env.image)
    for remote, digest in files.items():
        h, contig, size = fs.sha256(remote)
        assert h == digest, f"{remote}: content mismatch"
        assert contig or size <= fs.cbytes, f"{remote}: not contiguous"
        print(f"    image check {remote}: sha256 OK, contiguous")


def fsck(env):
    if not shutil.which("fsck.exfat"):
        print("    fsck.exfat not installed: skipped")
        return
    fs = ExFat(env.image)
    part = os.path.join(env.tmp, "part.img")
    with open(env.image, "rb") as src, open(part, "wb") as dst:
        src.seek(fs.base)
        while True:
            b = src.read(1 << 20)
            if not b:
                break
            if b.count(0) == len(b):
                dst.seek(len(b), 1)
            else:
                dst.write(b)
        dst.truncate()
    p = subprocess.run(["fsck.exfat", "-n", part], capture_output=True, text=True)
    os.unlink(part)
    assert p.returncode == 0, f"fsck.exfat failed:\n{p.stdout}{p.stderr}"
    print("    fsck.exfat -n: clean")


# ------------------------------------------------------------------ tests

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True)
    ap.add_argument("--client", required=True)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="ps2xfer-test-")
    env = Env(os.path.abspath(a.sim), os.path.abspath(a.client), tmp)
    files = {}
    try:
        print("1. basic sends (odd sizes, folder, every packet checksummed)")
        env.start("csum=1", mkfs=1024)
        d = os.path.join(tmp, "src")
        os.makedirs(os.path.join(d, "folder", "sub"))
        cases = {"odd.iso": 5 * 1048576 + 123, "tiny.bin": 100, "exact.bin": 1408 * 1000,
                 "folder/a.bin": 3000, "folder/sub/b.bin": 777777}
        for i, (n, size) in enumerate(cases.items()):
            files[n] = mkfile(os.path.join(d, n), size, i)
        out = env.run("info")
        assert "exFAT" in out and "buffers" in out, out
        env.run("send", os.path.join(d, "odd.iso"), "--verify")
        env.run("send", os.path.join(d, "tiny.bin"), os.path.join(d, "exact.bin"), "--dest", "/misc", "--verify")
        env.run("send", os.path.join(d, "folder"), "--dest", "/stuff")
        remote = {"/CD/odd.iso": files["odd.iso"], "/misc/tiny.bin": files["tiny.bin"],
                  "/misc/exact.bin": files["exact.bin"], "/stuff/folder/a.bin": files["folder/a.bin"],
                  "/stuff/folder/sub/b.bin": files["folder/sub/b.bin"]}
        out = env.run("stats")
        assert "csum_errors      0" in out, out
        out = env.run("stat", "/CD/odd.iso")
        assert "contiguous" in out, out
        out = env.run("send", os.path.join(d, "odd.iso"), check=False)
        assert "already exists" in out, out
        env.run("send", os.path.join(d, "odd.iso"), "--overwrite")
        env.kill(signal.SIGTERM)
        check_image(env, remote)

        print("2. 5% packet loss")
        env.start("--drop", "5")
        files["lossy.iso"] = mkfile(os.path.join(d, "lossy.iso"), 40 << 20, 10)
        out = env.run("send", os.path.join(d, "lossy.iso"), "--dest", "/DVD/", "--verify")
        assert "retransmits" in out
        remote["/DVD/lossy.iso"] = files["lossy.iso"]

        print("3. client interrupted, then resumed")
        env.kill(signal.SIGTERM)
        env.start()
        files["big.iso"] = mkfile(os.path.join(d, "big.iso"), 120 << 20, 11)
        p = env.popen("send", os.path.join(d, "big.iso"), "--dest", "/DVD/big.iso", rate=15)
        time.sleep(5)
        p.kill()
        p.wait()
        out = env.run("send", os.path.join(d, "big.iso"), "--dest", "/DVD/big.iso", "--verify")
        assert "resuming at" in out, out
        print("    " + [l for l in out.splitlines() if "resuming" in l][0])
        remote["/DVD/big.iso"] = files["big.iso"]
        env.kill(signal.SIGTERM)

        print("4. PS2 'power loss' (simulator killed), then resumed")
        env.start()
        files["pl.iso"] = mkfile(os.path.join(d, "pl.iso"), 120 << 20, 12)
        p = env.popen("send", os.path.join(d, "pl.iso"), "--dest", "/DVD/pl.iso", rate=15)
        time.sleep(5)
        env.kill(signal.SIGKILL)
        p.kill()
        p.wait()
        fsck(env)  # the file system must be consistent right after the "power loss"
        env.start()
        out = env.run("send", os.path.join(d, "pl.iso"), "--dest", "/DVD/pl.iso", "--verify")
        assert "resuming at" in out, out
        print("    " + [l for l in out.splitlines() if "resuming" in l][0])
        remote["/DVD/pl.iso"] = files["pl.iso"]

        print("5. file commands")
        env.run("mkdir", "/tmpdir/x")
        env.run("mv", "/misc/tiny.bin", "/tmpdir/x/t.bin")
        out = env.run("ls", "/tmpdir/x")
        assert "t.bin" in out, out
        env.run("rm", "/tmpdir/x/t.bin", "/tmpdir/x", "/tmpdir")
        out = env.run("ls", "/")
        assert "tmpdir" not in out, out
        del remote["/misc/tiny.bin"]
        out = env.run("df")
        assert "free" in out

        print("6. benches")
        print("    " + env.run("bench", "net-sink", "--size-mb", "30").strip().splitlines()[-1])
        print("    " + env.run("bench", "combined", "--size-mb", "30").strip().splitlines()[-1])
        print("    " + env.run("bench", "disk-raw", "--size-mb", "32", "--chunks", "64,704").strip().splitlines()[-1])
        print("    " + env.run("bench", "disk-fatfs", "--size-mb", "32", "--chunks", "131072").strip().splitlines()[-1])
        out = env.run("ls", "/.ps2xfer")
        assert "bench" not in out, out
        env.run("exit")
        time.sleep(0.5)
        env.proc.wait(5)
        env.proc = None

        print("7. final image")
        check_image(env, remote)
        fsck(env)
        print("ALL TESTS PASSED")
    finally:
        env.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
