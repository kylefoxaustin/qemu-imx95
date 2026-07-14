#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Read the segment ourselves, and require that the frame we claim to have sent
# ACTUALLY CROSSED IT.
#
# The impostor test asserts that our receiver refuses a 1000-byte frame whose
# 64-byte prefix is perfectly valid. But the first time I ran it, the impostor
# flag never reached the guest - a shell edit that silently did not apply - so it
# emitted ordinary 64-byte frames, our judge correctly counted them, and the
# harness reported that OUR MODEL was broken. I was one commit from "fixing" a
# receiver that was already right.
#
#   A NEGATIVE TEST THAT DID NOT PRODUCE THE CONDITION IT NAMES DOES NOT MERELY
#   MISS A BUG - IT MANUFACTURES ONE. AND THE FIX YOU THEN APPLY IS DAMAGE.
#
# Grepping the sender's own log for "I am sending evil frames" only proves the
# sender INTENDED to. rt1180emulator's correction, and it is the right one:
#
#   NOT "WE MEANT TO SEND IT" - "IT IS ON THE WIRE."
#
# So this joins the multicast group as an independent observer and reports what
# genuinely crossed. It answers to nobody's intentions, including our own.
#
#   usage: wire-sniff.py <group> <port> <seconds> [--require-len N] [--require-et 0xNNNN]
#   exit 0 = the required frame was observed (or nothing was required)
#   exit 1 = it was NOT - so any verdict downstream would be about a wire that
#            never carried the condition under test
import socket
import struct
import sys

BEACON_MAGIC = b"\xb5\xb6\xb7\xc0"
MAGIC_OFF = 14


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: wire-sniff.py <group> <port> <secs> "
                 "[--require-len N] [--require-et 0xNNNN]")
    group, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    need_len = need_et = None
    argv = sys.argv[4:]
    for i, a in enumerate(argv):
        if a == "--require-len" and i + 1 < len(argv):
            need_len = int(argv[i + 1])
        if a == "--require-et" and i + 1 < len(argv):
            need_et = int(argv[i + 1], 0)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", port))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     struct.pack("4sl", socket.inet_aton(group),
                                 socket.INADDR_ANY))
    except OSError as e:
        # Fail CLOSED. An observer that cannot observe must not report "clean".
        print("SNIFF: cannot listen on %s:%d (%s)" % (group, port, e))
        return 1
    s.settimeout(secs)

    census = {}          # (ethertype, len, has_magic) -> count
    hit = 0
    deadline = None
    try:
        import time
        deadline = time.time() + secs
        while time.time() < deadline:
            s.settimeout(max(0.1, deadline - time.time()))
            d, _ = s.recvfrom(4096)
            if len(d) < 14:
                continue
            et = (d[12] << 8) | d[13]
            magic = len(d) >= MAGIC_OFF + 4 and \
                d[MAGIC_OFF:MAGIC_OFF + 4] == BEACON_MAGIC
            key = (et, len(d), magic)
            census[key] = census.get(key, 0) + 1
            if (need_len is None or len(d) == need_len) and \
               (need_et is None or et == need_et) and magic:
                hit += 1
    except socket.timeout:
        pass

    print("SNIFF: %s:%d for %.0fs - what actually crossed the wire:" %
          (group, port, secs))
    for (et, ln, magic), n in sorted(census.items()):
        print("   et=0x%04x len=%-5d magic=%-3s x%d" %
              (et, ln, "yes" if magic else "no", n))
    if not census:
        print("   (nothing)")

    if need_len is None and need_et is None:
        return 0
    if hit:
        print("SNIFF: OK - saw %d frame(s) et=0x%04x len=%d WITH a valid magic "
              "prefix. The condition under test really was on the wire." %
              (hit, need_et or 0, need_len or 0))
        return 0
    print("SNIFF: REQUIRED FRAME NEVER CROSSED (wanted et=0x%04x len=%s with a "
          "valid magic prefix)." % (need_et or 0, need_len))
    print("SNIFF: the impostor did not produce the condition it names, so any "
          "verdict about the receiver would be about a wire that never carried "
          "the test. Refusing to let a downstream result mean anything.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
