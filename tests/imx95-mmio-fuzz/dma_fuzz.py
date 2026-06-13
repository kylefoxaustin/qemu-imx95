#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# i.MX 95 eDMA descriptor fuzz.
#
# The MMIO register fuzz (fuzz.py) pokes registers in isolation and so can't
# build a COHERENT-but-hostile transfer descriptor (a TCD that passes the model's
# guards but has adversarial lengths/addresses/offsets). This does: it programs
# edma1 channel 0's TCD with a valid element size + hostile NBYTES/CITER/SADDR/
# DADDR/SOFF/DOFF, kicks the channel (CH_CSR.ERQ, which runs the transfer
# synchronously in the write handler), and watches for:
#   - HANG  : the kick never returns (a huge CITER*NBYTES transfer loops ~forever)
#   - CRASH : ASan/UBSan abort or QEMU exit (OOB on the model side)
# Run against an ASan+UBSan build. edma1 is fsl,imx93-edma3 (v3, 32-bit TCD),
# 0x10000 page stride, so channel 0 is at base + 0x10000.
import os, socket, subprocess, sys, tempfile

QEMU = os.environ.get("QEMU", "build-asan/qemu-system-aarch64")
MACHINE = "imx95-19x19-evk"

CH      = 0x44000000 + 0x10000     # edma1 channel 0 page
CH_CSR  = CH + 0x00
T_SADDR = CH + 0x20                # 32-bit (v3 TCD)
T_SOFF  = CH + 0x24                # 16-bit
T_ATTR  = CH + 0x26                # 16-bit
T_NBYTES= CH + 0x28                # 32-bit
T_DADDR = CH + 0x30                # 32-bit
T_DOFF  = CH + 0x34                # 16-bit
T_CITER = CH + 0x36                # 16-bit
RAM     = 0x90000000               # scratch inside DRAM (launch with -m 2G)


class Qtest:
    def __init__(self):
        self.sockpath = tempfile.mktemp(suffix=".qtest")
        self.lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.lsock.bind(self.sockpath); self.lsock.listen(1); self.lsock.settimeout(30)
        self.errpath = tempfile.mktemp(suffix=".qemu-err")
        self.errfile = open(self.errpath, "w+b")
        env = dict(os.environ,
                   ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0",
                   UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1")
        self.proc = subprocess.Popen(
            [QEMU, "-M", MACHINE, "-m", "2G", "-accel", "qtest", "-display",
             "none", "-nodefaults", "-d", "guest_errors",
             "-qtest", "unix:%s" % self.sockpath],
            stdout=subprocess.DEVNULL, stderr=self.errfile, env=env)
        try:
            self.conn, _ = self.lsock.accept()
        except socket.timeout:
            sys.exit("FATAL: qemu did not connect.\n" + self._err()[-1500:])
        self.conn.settimeout(20); self.buf = b""

    def cmd(self, line, timeout=None):
        if timeout is not None:
            self.conn.settimeout(timeout)
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

    def close(self):
        for fn in (self.conn.close, self.proc.kill, self.errfile.close,
                   lambda: os.unlink(self.sockpath), lambda: os.unlink(self.errpath)):
            try: fn()
            except Exception: pass


def kick(q, saddr, daddr, attr, soff, doff, nbytes, citer, kt=20):
    # program the TCD, then set CH_CSR.ERQ to run it synchronously
    for c in ("writel 0x%x 0x%x" % (T_SADDR, saddr & 0xffffffff),
              "writel 0x%x 0x%x" % (T_DADDR, daddr & 0xffffffff),
              "writew 0x%x 0x%x" % (T_ATTR, attr & 0xffff),
              "writew 0x%x 0x%x" % (T_SOFF, soff & 0xffff),
              "writew 0x%x 0x%x" % (T_DOFF, doff & 0xffff),
              "writel 0x%x 0x%x" % (T_NBYTES, nbytes & 0xffffffff),
              "writew 0x%x 0x%x" % (T_CITER, citer & 0xffff)):
        if q.cmd(c) is None:
            return "CRASH(programming)"
    r = q.cmd("writel 0x%x 0x1" % CH_CSR, timeout=kt)   # the kick
    if r is None:
        # distinguish hang (process alive) from crash (process dead)
        return "HANG" if q.proc.poll() is None else "CRASH"
    return "ok"


# (name, saddr, daddr, attr, soff, doff, nbytes, citer)
# attr=0 -> SSIZE=DSIZE=0 -> esize=1 (passes the esize<=8 guard).
CASES = [
    ("sanity-small",      RAM, RAM + 0x1000, 0, 1, 1, 64,         1),
    ("huge-nbytes",       RAM, RAM + 0x1000, 0, 1, 1, 0x3fffffff, 1),
    ("huge-citer",        RAM, RAM + 0x1000, 0, 1, 1, 0x10000,    0x7fff),
    ("both-huge",         RAM, RAM + 0x1000, 0, 1, 1, 0x3fffffff, 0x7fff),
    ("esize8-both-huge",  RAM, RAM + 0x1000, 0x0303, 8, 8, 0x3fffffff, 0x7fff),
    ("garbage-saddr",     0xffffffff, RAM,   0, 1, 1, 256,        1),
    ("garbage-daddr",     RAM, 0xffffffff,   0, 1, 1, 256,        1),
    ("neg-soff-doff",     RAM + 0x10000, RAM + 0x10000, 0, 0xffff, 0xffff, 0x10000, 0x100),
    ("huge-off-walk",     RAM, RAM + 0x1000, 0, 0x4000, 0x4000, 0x100000, 0x200),
    ("smloe-mloff",       RAM, RAM + 0x1000, 0, 1, 1, 0xbfffffff, 0x7fff),  # SMLOE+offset
]


def main():
    print("i.MX95 eDMA descriptor fuzz (QEMU=%s)" % QEMU)
    q = Qtest()
    findings = []
    for name, sa, da, at, so, do, nb, ci in CASES:
        res = kick(q, sa, da, at, so, do, nb, ci)
        print("  %-18s nbytes=0x%-9x citer=0x%-5x -> %s" % (name, nb, ci, res))
        if res != "ok":
            findings.append((name, res, q._err()[-800:] if res.startswith("CRASH") else ""))
            q.close(); q = Qtest()      # restart, continue
    capped = q._err().count("transfer too large")
    q.close()
    print("\n=== eDMA DMA FUZZ SUMMARY ===")
    print("cases: %d   findings: %d   cap-refused (guest_error): %d"
          % (len(CASES), len(findings), capped))
    for name, res, err in findings:
        print("  %s: %s" % (name, res))
        if err:
            print("    " + err.replace("\n", "\n    ")[:600])
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
