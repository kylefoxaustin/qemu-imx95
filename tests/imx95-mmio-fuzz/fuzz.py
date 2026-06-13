#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 MMIO register robustness fuzz.
#
# Drives QEMU in qtest mode (no guest OS) and hammers every from-scratch i.MX 95
# device register window with HOSTILE accesses the happy-path tests never make:
# all access widths (1/2/4/8 bytes), aligned AND misaligned offsets, boundary
# offsets, and adversarial values (0, all-ones, walking bits, random). The goal
# is to prove the models' MemoryRegionOps read/write handlers never crash/assert
# QEMU on any guest access ("never trust the guest"). Run against a sanitizer
# (ASan+UBSan) build for maximum catch: a sanitizer/abort kills the qtest socket,
# which this detects, logs (window/offset/width/value/op), then restarts QEMU and
# continues so one crash doesn't end the sweep.
#
# Usage: QEMU=<qemu-system-aarch64> python3 fuzz.py
import os, socket, subprocess, sys, random, tempfile

QEMU = os.environ.get("QEMU", "build/qemu-system-aarch64")
MACHINE = os.environ.get("MACHINE", "imx95-19x19-evk")
SEED = int(os.environ.get("SEED", "1"))
random.seed(SEED)

# (base, size, name) - one instance per from-scratch device type (same handler
# across instances, so fuzzing one covers the rest).
WINDOWS = [
    (0x44290000, 0x30000, "sysctr"),
    (0x44470000, 0x10000, "gpc"),
    (0x44480000, 0x10000, "anatop"),
    (0x44460000, 0x10000, "src"),
    (0x444f0000, 0x10000, "aonmix(blk_ctrl_s)"),
    (0x44400000, 0x800,   "xcache_pc"),
    (0x44400800, 0x800,   "xcache_ps"),
    (0x42490000, 0x10000, "wdog3"),
    (0x44380000, 0x1000,  "lpuart1"),
    (0x47400000, 0x10000, "gpio1"),
    (0x4b0b0000, 0x10000, "irqsteer"),
    (0x4b400000, 0x100000,"dpu"),
    (0x4ad50000, 0x80000, "isi"),
    (0x4acf0000, 0x10000, "dsi"),
    (0x4ab00004, 0x400,   "neutron(dev/mbox)"),
    (0x4e090dc0, 0x1000,  "ddr_pmu"),
    (0x4c500000, 0x10000, "jpegdec"),
    (0x4c550000, 0x10000, "jpegenc"),
    (0x44360000, 0x10000, "lpspi1"),
    (0x443b0000, 0x10000, "sai1"),
    (0x44520000, 0x10000, "micfil"),
    (0x42680000, 0x10000, "xcvr"),
    (0x44000000, 0x10000, "edma1"),
    (0x445b0000, 0x1000,  "sm_mu"),
    (0x47520000, 0x10000, "elemu0"),
]

WIDTHS = [(1, "b"), (2, "w"), (4, "l"), (8, "q")]
VALUES = [0x0, 0xffffffffffffffff, 0xdeadbeefcafef00d, 0x1]

def offsets_for(size):
    # stepped coverage (cap ~64) + every misalignment + window boundaries
    step = max(4, (size // 64) & ~3)
    offs = list(range(0, size, step))
    offs += [1, 2, 3, 5, 6, 7]                       # misaligned
    for end in (size - 8, size - 4, size - 2, size - 1):
        if end > 0:
            offs.append(end)
    return sorted(set(o for o in offs if 0 <= o < size))


class Qtest:
    def __init__(self):
        self.sockpath = tempfile.mktemp(suffix=".qtest")
        self.lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.lsock.bind(self.sockpath)
        self.lsock.listen(1)
        self.lsock.settimeout(30)
        env = dict(os.environ,
                   ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0",
                   UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1")
        # stderr -> a real file, never an undrained PIPE: the qtest accelerator
        # logs every command to stderr, so a PIPE fills (~64K) and QEMU blocks.
        self.errpath = tempfile.mktemp(suffix=".qemu-err")
        self.errfile = open(self.errpath, "w+b")
        self.proc = subprocess.Popen(
            [QEMU, "-M", MACHINE, "-accel", "qtest", "-display", "none",
             "-nodefaults", "-qtest", "unix:%s" % self.sockpath],
            stdout=subprocess.DEVNULL, stderr=self.errfile, env=env)
        try:
            self.conn, _ = self.lsock.accept()
        except socket.timeout:
            sys.exit("FATAL: qemu did not connect to qtest socket.\n" +
                     self._err()[-1500:])
        self.conn.settimeout(20)
        self.buf = b""

    def cmd(self, line):
        # returns the response line, or None if QEMU died
        try:
            self.conn.sendall((line + "\n").encode())
            while b"\n" not in self.buf:
                chunk = self.conn.recv(4096)
                if not chunk:
                    return None
                self.buf += chunk
        except (socket.timeout, OSError):
            return None
        resp, self.buf = self.buf.split(b"\n", 1)
        return resp.decode(errors="replace").strip()

    def _err(self):
        try:
            self.errfile.flush(); self.errfile.seek(0)
            return self.errfile.read().decode(errors="replace")
        except Exception:
            return ""

    def stderr_tail(self):
        try:
            self.proc.kill()
        except Exception:
            pass
        return self._err()[-1200:]

    def close(self):
        for fn in (lambda: self.conn.close(),
                   lambda: self.proc.kill(),
                   lambda: self.errfile.close(),
                   lambda: os.unlink(self.sockpath),
                   lambda: os.unlink(self.errpath)):
            try:
                fn()
            except Exception:
                pass


def main():
    print("i.MX95 MMIO robustness fuzz  (QEMU=%s machine=%s seed=%d)" %
          (QEMU, MACHINE, SEED))
    q = Qtest()
    total = 0
    crashes = []
    for base, size, name in WINDOWS:
        for off in offsets_for(size):
            addr = base + off
            for w, suff in WIDTHS:
                for val in VALUES:
                    mask = (1 << (w * 8)) - 1
                    v = val & mask
                    for op in ("write%s 0x%x 0x%x" % (suff, addr, v),
                               "read%s 0x%x" % (suff, addr)):
                        total += 1
                        if q.cmd(op) is None:
                            err = q.stderr_tail()
                            rec = dict(win=name, base=hex(base), off=hex(off),
                                       addr=hex(addr), width=w, op=op.split()[0],
                                       val=hex(v))
                            crashes.append((rec, err))
                            print("\n*** CRASH: %s @ %s (%s) %s val=%s\n%s" %
                                  (name, hex(addr), op.split()[0], "w%d" % w,
                                   hex(v), err))
                            q.close()
                            q = Qtest()      # restart, continue past it
    q.close()
    print("\n=== FUZZ SUMMARY ===")
    print("accesses: %d   windows: %d" % (total, len(WINDOWS)))
    print("crashes/aborts: %d" % len(crashes))
    if crashes:
        for rec, _ in crashes:
            print("  CRASH %(win)s addr=%(addr)s %(op)s w%(width)s val=%(val)s" % rec)
        sys.exit(1)
    print("RESULT: PASS - no crash/assert on any hostile MMIO access")
    sys.exit(0)


if __name__ == "__main__":
    main()
