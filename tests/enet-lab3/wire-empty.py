#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Refuse to run a lab on a wire that is not empty.
#
# holobench killed a lab runner, asyncio reaped the parent and ORPHANED ITS QEMU
# CHILDREN, which kept beaconing. Their mcast group is reused across runs, so the
# next run landed on the same wire - not a race, a GUARANTEE. Their "4-node lab"
# was an EIGHT-node segment, half of it ghosts of the previous run still speaking
# the OLD protocol. The scorer then faithfully reported that two peers were
# emitting bugs they had fixed hours earlier, and they were one message from
# posting a false accusation.
#
#   AN ORPHANED PROCESS ON A SHARED BUS IS NOT A LEAK. IT IS A LIAR THAT
#   OUTLIVED THE RUN THAT CREATED IT - AND ITS TESTIMONY IS INDISTINGUISHABLE
#   FROM A PEER'S.
#
# We reuse a FIXED mcast group too, so we have exactly the same exposure - only
# our failure mode is the mirror image and worse: a ghost stand-in from a
# previous run would be counted as a live peer, and OUR GREEN would be the lie.
#
# So: before launching, listen. If anything is already talking, REFUSE - do not
# warn. A warning printed above a green result is a warning nobody reads.
#
#   usage: wire-empty.py <group> <port> [listen_seconds]
#   exit 0 = silent wire (safe to launch)   exit 1 = someone is out there
import socket
import struct
import sys

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: wire-empty.py <group> <port> [seconds]")
    group = sys.argv[1]
    port = int(sys.argv[2])
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", port))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     struct.pack("4sl", socket.inet_aton(group),
                                 socket.INADDR_ANY))
    except OSError as e:
        # A bind failure is NOT "the wire is empty". Fail closed: an error we
        # cannot interpret must never be reported as a clean result.
        print("PREFLIGHT: cannot listen on %s:%d (%s) - refusing to guess"
              % (group, port, e))
        return 1

    s.settimeout(secs)
    seen = 0
    senders = set()
    try:
        while True:
            _, addr = s.recvfrom(2048)
            seen += 1
            senders.add(addr[0])
    except socket.timeout:
        pass

    if seen:
        print("PREFLIGHT: REFUSING TO LAUNCH - %s:%d is NOT EMPTY "
              "(%d frames from %d sender(s) in %.0fs)"
              % (group, port, seen, len(senders), secs))
        print("PREFLIGHT: something from a previous run is still on this wire. "
              "Its frames are indistinguishable from a peer's, so any result "
              "here - green OR red - would be about the wrong segment.")
        print("PREFLIGHT: pkill -x qemu-system-aarch64, or pick another group.")
        return 1

    print("PREFLIGHT: %s:%d silent for %.0fs - wire is empty" %
          (group, port, secs))
    return 0

if __name__ == "__main__":
    sys.exit(main())
