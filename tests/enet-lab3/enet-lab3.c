/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * enet-lab3: the i.MX 95 (Linux/A55) node for the fleet 3-node raw-L2 segment.
 *
 * A model-agnostic raw-Ethernet emitter/sniffer, the Linux counterpart to
 * mcxn947's tests/mcxn-enet-lab3 and rt1180's tools/netc-eth-lab.sh (both
 * bare-metal M-core). It broadcasts a 64-byte L2 frame carrying OUR ethertype
 * on a shared QEMU socket-mcast segment, and watches inbound frames for the
 * OTHER nodes' distinct ethertypes. It declares PASS only after it has OBSERVED
 * EVERY peer ethertype - never on "traffic seen" - so a merely-live wire (or a
 * mcast backend echoing our own frames) cannot manufacture a false green.
 *
 *   usage: enet-lab3 <ifname> <my_ethertype> <peer_ethertype> [more_peers...]
 *   e.g.   enet-lab3 eth0 0x88B7 0x88B5 0x88B6
 *
 * This is a standalone Linux userspace tool (it runs in the guest, not in
 * QEMU), so it uses libc directly - e.g. strtol, not QEMU's qemu_strtol, which
 * is unavailable outside the QEMU build.
 *
 * Design rules adopted from the fleet (mcxn, learned the hard way):
 *   1. IGNORE OUR OWN ethertype on RX - a mcast socket can hand our broadcast
 *      back to us; counting it would "see a peer" that is ourselves.
 *   2. Broadcast forever; silence from a peer = not up yet, not failure. A late
 *      Linux peer (iface up takes seconds) is expected. Only a hard deadline
 *      ends the run, and it ends FAIL with exactly which peers were missing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netpacket/packet.h>

#define MAX_PEERS 8
#define FRAME_LEN 64            /* min Ethernet frame */
#define SEND_EVERY_MS 200
#define POST_PASS_MS 3000       /* keep broadcasting post-PASS for peers */

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    const char *ifname;
    unsigned my_et;
    unsigned peers[MAX_PEERS];
    int seen[MAX_PEERS] = {0};
    int npeers = 0;
    int fd, ifindex, passed = 0;
    unsigned char mymac[6];
    unsigned char frame[FRAME_LEN];
    struct ifreq ifr;
    struct sockaddr_ll to;
    const char *dl_env;
    long dl_ms, deadline, next_send = 0, pass_at = 0;
    long post_pass_ms = POST_PASS_MS;

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <ifname> <my_ethertype> <peer_ethertype>...\n",
                argv[0]);
        return 2;
    }
    ifname = argv[1];
    my_et = (unsigned)strtol(argv[2], NULL, 0);
    for (int i = 3; i < argc && npeers < MAX_PEERS; i++) {
        peers[npeers++] = (unsigned)strtol(argv[i], NULL, 0);
    }

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        return 1;
    }
    ifindex = ifr.ifr_ifindex;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        return 1;
    }
    memcpy(mymac, ifr.ifr_hwaddr.sa_data, 6);
    printf("ENET-LAB3 up: if=%s idx=%d mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "my_et=0x%04x watching %d peer(s)\n", ifname, ifindex,
           mymac[0], mymac[1], mymac[2], mymac[3], mymac[4], mymac[5],
           my_et, npeers);
    fflush(stdout);

    /* Frame we broadcast: dest = ff:ff..., src = our station addr, our et. */
    memset(frame, 0, sizeof(frame));
    memset(frame, 0xff, 6);                 /* dest broadcast */
    memcpy(frame + 6, mymac, 6);            /* src */
    frame[12] = (my_et >> 8) & 0xff;
    frame[13] = my_et & 0xff;
    memcpy(frame + 14, "IMX95-ENET-LAB3", 15);

    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_ifindex = ifindex;
    to.sll_halen = 6;
    memset(to.sll_addr, 0xff, 6);

    dl_env = getenv("LAB_DEADLINE_MS");
    dl_ms = dl_env ? strtol(dl_env, NULL, 0) : 120000;   /* 120s hard cap */
    deadline = now_ms() + dl_ms;

    /*
     * How long to keep broadcasting after PASS. Longer lets late/slow peers
     * (e.g. a node that PASSes fast then exits) stay on the wire long enough
     * for the others to observe it - needed to close a 3-node "all see all".
     */
    {
        const char *pp = getenv("LAB_POST_PASS_MS");

        if (pp) {
            post_pass_ms = strtol(pp, NULL, 0);
        }
    }

    for (;;) {
        long t = now_ms();
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        unsigned char buf[2048];
        struct sockaddr_ll from;
        socklen_t fl = sizeof(from);
        ssize_t n;
        unsigned et;
        int pr, all;

        if (t >= next_send) {
            if (sendto(fd, frame, FRAME_LEN, 0,
                       (struct sockaddr *)&to, sizeof(to)) < 0 &&
                errno != EINTR) {
                perror("sendto");
            }
            next_send = t + SEND_EVERY_MS;
        }
        if (passed && t >= pass_at) {
            printf("ENET-LAB3 done: exiting after post-pass broadcast\n");
            return 0;
        }
        if (!passed && t >= deadline) {
            printf("ENET-LAB3 FAIL: deadline, missing peers:");
            for (int i = 0; i < npeers; i++) {
                if (!seen[i]) {
                    printf(" 0x%04x", peers[i]);
                }
            }
            printf("\n");
            return 1;
        }

        pr = poll(&pfd, 1, SEND_EVERY_MS);
        if (pr <= 0) {
            continue;
        }
        n = recvfrom(fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &fl);
        if (n < 14) {
            continue;
        }
        et = (buf[12] << 8) | buf[13];
        if (et == my_et) {
            continue;                       /* rule 1: ignore our own echo */
        }
        for (int i = 0; i < npeers; i++) {
            if (et == peers[i] && !seen[i]) {
                seen[i] = 1;
                printf("ENET-LAB3 rx: peer ethertype 0x%04x src "
                       "%02x:%02x:%02x:%02x:%02x:%02x\n", et,
                       buf[6], buf[7], buf[8], buf[9], buf[10], buf[11]);
                fflush(stdout);
            }
        }
        if (passed) {
            continue;
        }
        all = 1;
        for (int i = 0; i < npeers; i++) {
            if (!seen[i]) {
                all = 0;
            }
        }
        if (all) {
            printf("ENET-LAB3 PASS: saw BOTH peers on the segment (");
            for (int i = 0; i < npeers; i++) {
                printf("%s0x%04x", i ? "," : "", peers[i]);
            }
            printf(")\n");
            fflush(stdout);
            passed = 1;
            pass_at = t + post_pass_ms;
        }
    }
}
