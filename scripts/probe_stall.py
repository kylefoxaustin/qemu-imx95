#!/usr/bin/env python3
#
# probe_stall.py - diagnose an A55-side Linux HANG on the imx95 QEMU machine.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This is the project's "pillar 5" debugging tool: when a Linux probe or
# initcall *hangs* (rather than faulting) on the A55, there is no fault dump to
# read, and you cannot just `x` a fixed stack address because the stalled
# kworker's kernel stack lives at a non-deterministic address across runs.
#
# The technique it automates:
#   1. Launch QEMU with an HMP monitor on a private unix socket.
#   2. Sleep until execution is sitting in the stall.
#   3. `cpu N; info registers` to read the LIVE frame pointer (X29).
#   4. In the SAME run, `x /<n>gx <FP & ~0xfff>` to dump that exact stack.
#   5. Walk the AArch64 frame chain offline ([fp] = caller fp, [fp+8] = LR).
#   6. addr2line every return address against vmlinux.
#   7. Sample a few times (hangs can be racy) and print each backtrace.
#
# Example - find what a hang on the real-SM boot is stuck in:
#
#   scripts/probe_stall.py --vmlinux ~/linux-imx95-build/vmlinux --cpu 2 \
#       --delay 55 --samples 3 -- \
#       ./build/qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \
#       -icount shift=auto -global fsl-imx95.scmi-stub=false \
#       -kernel Image -dtb imx95-19x19-evk.dtb -initrd initramfs.cpio.gz \
#       -append 'console=ttyLP0,115200 rdinit=/init' \
#       -device loader,file=m33_image.elf,cpu-num=6 \
#       -serial file:/tmp/console.log -serial null
#
# Notes:
#   - The QEMU command (everything after `--`) MUST NOT contain its own
#     `-monitor`; this tool appends `-monitor unix:...`. Route the guest
#     console to a file or null so stdio stays free.
#   - --delay is wall-clock seconds; with `-icount shift=auto` guest time
#     roughly tracks wall time, so pick a value comfortably past the stall.
#   - A consistent leaf across samples is the culprit (the DPU hang that
#     motivated this tool resolved to dpu95_be_read in 3/3 samples).

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time


def recv_until_prompt(sock, timeout=8.0):
    sock.settimeout(timeout)
    buf = b""
    try:
        while b"(qemu)" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf.decode(errors="replace")


def hmp(sock, cmd, settle=0.4):
    sock.sendall((cmd + "\n").encode())
    time.sleep(settle)
    return recv_until_prompt(sock)


def parse_stack_dump(text):
    """Parse `x /Ngx ADDR` output into {address: qword}."""
    mem = {}
    pat = re.compile(r"([0-9a-f]{16}):\s+0x([0-9a-f]{16})\s+0x([0-9a-f]{16})")
    for line in text.splitlines():
        m = pat.search(line)
        if m:
            base = int(m.group(1), 16)
            mem[base] = int(m.group(2), 16)
            mem[base + 8] = int(m.group(3), 16)
    return mem


def unwind(fp, mem, max_frames=48):
    """Walk the AArch64 frame chain: [fp]=next fp, [fp+8]=saved LR."""
    chain = []
    seen = set()
    while fp in mem and fp not in seen and len(chain) < max_frames:
        seen.add(fp)
        chain.append(mem.get(fp + 8, 0))
        fp = mem[fp]
    return chain


def resolve(addr, vmlinux, addr2line):
    # Kernel text is in the 0xffff8000_8....... half-canonical range.
    if (addr >> 40) != 0xffff80:
        return f"{addr:#018x}  (non-text)"
    out = subprocess.run([addr2line, "-f", "-e", vmlinux, hex(addr)],
                         capture_output=True, text=True).stdout.strip()
    return f"{addr:#018x}  " + out.replace("\n", "  |  ")


def main():
    ap = argparse.ArgumentParser(
        description="Diagnose an A55 Linux hang via HMP frame-pointer unwinding.")
    ap.add_argument("--vmlinux", required=True, help="path to vmlinux for addr2line")
    ap.add_argument("--cpu", type=int, default=2, help="CPU index to inspect (default 2)")
    ap.add_argument("--delay", type=float, default=55.0,
                    help="wall-clock seconds to wait before sampling (default 55)")
    ap.add_argument("--samples", type=int, default=3, help="number of samples (default 3)")
    ap.add_argument("--gap", type=float, default=3.0, help="seconds between samples (default 3)")
    ap.add_argument("--dump-qwords", type=int, default=1024,
                    help="qwords to dump from the FP page (default 1024 = 8 KiB)")
    ap.add_argument("--addr2line", default="aarch64-linux-gnu-addr2line",
                    help="addr2line binary (default aarch64-linux-gnu-addr2line)")
    ap.add_argument("qemu", nargs=argparse.REMAINDER,
                    help="-- followed by the full QEMU command (no -monitor)")
    args = ap.parse_args()

    qemu_cmd = args.qemu
    if qemu_cmd and qemu_cmd[0] == "--":
        qemu_cmd = qemu_cmd[1:]
    if not qemu_cmd:
        ap.error("missing QEMU command after `--`")
    if "-monitor" in qemu_cmd:
        ap.error("the QEMU command must not contain -monitor (this tool adds one)")
    if not os.path.exists(args.vmlinux):
        ap.error(f"vmlinux not found: {args.vmlinux}")

    sockpath = tempfile.mktemp(prefix="probe_stall_", suffix=".sock")
    full = list(qemu_cmd) + ["-monitor", f"unix:{sockpath},server,nowait"]

    proc = subprocess.Popen(full)
    try:
        for _ in range(100):
            if os.path.exists(sockpath):
                break
            time.sleep(0.2)
        else:
            print("error: monitor socket never appeared", file=sys.stderr)
            return 1
        time.sleep(2)

        sock = socket.socket(socket.AF_UNIX)
        sock.connect(sockpath)
        recv_until_prompt(sock)

        print(f"# waiting {args.delay:.0f}s for the guest to reach the stall...",
              file=sys.stderr)
        time.sleep(args.delay)

        for i in range(args.samples):
            hmp(sock, f"cpu {args.cpu}")
            reg = hmp(sock, "info registers")
            m = re.search(r"PC=([0-9a-f]{16}).*?X29=([0-9a-f]{16})", reg, re.S)
            if not m:
                print(f"=== sample {i}: could not read registers for cpu {args.cpu}")
                time.sleep(args.gap)
                continue
            pc = int(m.group(1), 16)
            fp = int(m.group(2), 16)
            base = fp & ~0xfff
            dump = hmp(sock, f"x /{args.dump_qwords}gx 0x{base:016x}")
            mem = parse_stack_dump(dump)
            chain = unwind(fp, mem)

            print(f"\n===== cpu {args.cpu} sample {i}")
            print("  PC  " + resolve(pc, args.vmlinux, args.addr2line))
            for lr in chain:
                print("      " + resolve(lr, args.vmlinux, args.addr2line))
            time.sleep(args.gap)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        try:
            os.unlink(sockpath)
        except FileNotFoundError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
