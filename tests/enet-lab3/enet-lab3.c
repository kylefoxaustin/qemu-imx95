/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * enet-lab3: the i.MX 95 (Linux/A55) node for the fleet raw-L2 segment.
 *
 * A model-agnostic raw-Ethernet beacon/checker, the Linux counterpart to
 * mcxn947's tests/mcxn-enet-lab3 and 91emulator's tests/interconnect-imx91
 * (both bare-metal M-core). It broadcasts a 64-byte L2 frame carrying OUR
 * ethertype on a shared QEMU socket-mcast segment, and watches inbound frames
 * for the OTHER nodes' ethertypes.
 *
 *   usage: enet-lab3 <ifname> <my_ethertype> <peer_ethertype> [more_peers...]
 *   e.g.   enet-lab3 eth0 0x88B7 0x88B5 0x88B6
 *
 * ---------------------------------------------------------------------------
 * THE BODY. This node used to emit the ASCII string "IMX95-ENET-LAB3" as its
 * payload and check nothing but the ethertype. It was WRONG ON THE WIRE and it
 * BLOCKED THE WHOLE SEGMENT: both enforcing peers read our text as their binary
 * fields (mcx logged magic=0x494d5839, which is "IMX9"), correctly judged it
 * corrupt, correctly refused to count us as a peer - and so NEITHER OF THEM
 * COULD EVER PASS. The lab was green for weeks only because nobody was checking.
 *
 *   THE GREEN WAS GREEN BECAUSE THE CHECK DID NOT EXIST.  (holobench)
 *
 * The agreed body, implemented independently by mcx and imx91 - which then
 * interoperated first try on a live wire, so this is a DEMONSTRATED spec and
 * not a proposed one:
 *
 *      bytes  0..5    destination MAC (broadcast)
 *      bytes  6..11   source MAC
 *      bytes 12..13   ethertype                       (big endian)
 *      bytes 14..17   magic 0xB5B6B7C0                (big endian)
 *      bytes 18..19   the sender's OWN ethertype      (big endian)
 *      bytes 20..23   sequence number, monotonic      (big endian)
 *      bytes 24..63   0x5A, repeated
 *
 * ---------------------------------------------------------------------------
 * Design rules, every one of them paid for by somebody on this bus:
 *
 *  1. IGNORE OUR OWN ethertype on RX - a mcast socket can hand our own
 *     broadcast back to us, and counting it would "see a peer" that is us.
 *
 *  2. ASK "IS THIS EVEN MY PROTOCOL?" BEFORE ASKING "IS IT WELL-FORMED?"
 *     Frames whose ethertype is not a beacon ethertype are IGNORED: not
 *     counted, not condemned, not printed. The Linux peers' kernels emit
 *     multicast NDP/MLD (IPv6, ethertype 0x86DD) on this very segment, and
 *     body-checking those against a beacon layout produced a flood of false
 *     CORRUPT reports on the nodes that did it.
 *       A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS
 *       PROTOCOL WILL BE TURNED OFF BY THE PEOPLE IT PROTECTS.  (holobench)
 *
 *  3. PRESENCE IS NOT INTEGRITY. A frame that arrives is not a peer sighting;
 *     a frame that is INTACT is. Magic, self-consistent ethertype and pattern
 *     are checked before a peer is counted.
 *
 *  4. FRESHNESS. A beacon's sequence only ever goes UP. A frame whose seq has
 *     not advanced is a REPLAY - which is exactly what a stalled RX ring, or a
 *     buffer that is never rewritten, looks like from the outside. It is
 *     CORRUPT, not a sighting.
 *       A PEER THAT ONLY EVER REPEATS ITSELF IS SAYING NOTHING NEW, AND A NODE
 *       SAYING NOTHING NEW IS SAYING NOTHING.  (mcxn947)
 *     A GAP in the sequence is a LOSS: logged as a statistic, NEVER a failure.
 *     This is multicast; frames are allowed to go missing.
 *
 *  5. RE-ARM, DO NOT LATCH, AND DO NOT EXIT. Having once seen every peer, we
 *     clear the set and require them all again, forever, printing a PASS
 *     heartbeat each time. A latched PASS is an oracle that has EXPIRED: it
 *     keeps saying yes about a wire it stopped looking at. And we never exit on
 *     our own, because to a peer, LEFT EARLY and CRASHED are the same
 *     observation - so departure must be the coordinator's decision, not ours.
 *     Kill us whenever you like; we will not do it to ourselves.
 *
 * This is a standalone Linux userspace tool (it runs in the guest, not in
 * QEMU), so it uses libc directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
#define FRAME_LEN 64            /* min Ethernet frame - the agreed size */
#define SEND_EVERY_MS 200
#define QUIET_MS 5000           /* a peer silent this long has gone LOST */

/* The fleet's checkable body. Do not "improve" any of these four numbers. */
#define BEACON_MAGIC 0xB5B6B7C0u
#define MAGIC_OFF    14
#define SELF_ET_OFF  18
#define SEQ_OFF      20
#define FILL_OFF     24
#define FILL_BYTE    0x5A

/* Why a frame was rejected. Never counted as a peer sighting. */
enum {
    BAD_OK = 0,
    BAD_SHORT,
    BAD_MAGIC,
    BAD_SELF_ET,
    BAD_PATTERN,
    BAD_STALE,
};

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int main(int argc, char **argv)
{
    const char *ifname;
    unsigned my_et;
    unsigned peers[MAX_PEERS];
    int seen[MAX_PEERS] = {0};          /* seen THIS round (we re-arm) */
    int ever[MAX_PEERS] = {0};          /* has ever shown a valid body */
    uint32_t last_seq[MAX_PEERS] = {0};
    long last_rx[MAX_PEERS] = {0};
    int lost[MAX_PEERS] = {0};
    unsigned long long losses = 0;
    int npeers = 0;
    int fd, ifindex;
    unsigned char mymac[6];
    unsigned char frame[FRAME_LEN];
    struct ifreq ifr;
    struct sockaddr_ll to;
    const char *dl_env;
    long dl_ms, deadline, next_send = 0;
    uint32_t tx_seq = 0;
    unsigned long long beat = 0;
    int passed_once = 0;
    long t0;

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

    /*
     * Deliberately NOT printing the peer ethertypes we are looking for. A node
     * whose banner names the tokens it is hunting can match its OWN banner:
     * rt1180's monitor did exactly that and shouted PASS twelve times at an
     * empty wire. THE OBSERVER PUT ITSELF IN THE SET IT WAS OBSERVING.
     */
    printf("ENET-LAB3 up: if=%s idx=%d mac=%02x:%02x:%02x:%02x:%02x:%02x "
           "my_et=0x%04x watching %d peer(s) body=magic+seq+fill\n",
           ifname, ifindex, mymac[0], mymac[1], mymac[2], mymac[3],
           mymac[4], mymac[5], my_et, npeers);
    fflush(stdout);

    /* The frame we broadcast: header, then the fleet's checkable body. */
    memset(frame, 0, sizeof(frame));
    memset(frame, 0xff, 6);                     /* dest broadcast */
    memcpy(frame + 6, mymac, 6);                /* src */
    frame[12] = (my_et >> 8) & 0xff;
    frame[13] = my_et & 0xff;
    frame[MAGIC_OFF + 0] = (BEACON_MAGIC >> 24) & 0xff;
    frame[MAGIC_OFF + 1] = (BEACON_MAGIC >> 16) & 0xff;
    frame[MAGIC_OFF + 2] = (BEACON_MAGIC >> 8) & 0xff;
    frame[MAGIC_OFF + 3] = BEACON_MAGIC & 0xff;
    frame[SELF_ET_OFF + 0] = (my_et >> 8) & 0xff;   /* must equal bytes 12..13 */
    frame[SELF_ET_OFF + 1] = my_et & 0xff;
    memset(frame + FILL_OFF, FILL_BYTE, FRAME_LEN - FILL_OFF);

    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_ifindex = ifindex;
    to.sll_halen = 6;
    memset(to.sll_addr, 0xff, 6);

    dl_env = getenv("LAB_DEADLINE_MS");
    dl_ms = dl_env ? strtol(dl_env, NULL, 0) : 120000;
    t0 = now_ms();
    deadline = t0 + dl_ms;

    for (;;) {
        long t = now_ms();
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        unsigned char buf[2048];
        struct sockaddr_ll from;
        socklen_t fl = sizeof(from);
        ssize_t n;
        unsigned et;
        int pr, all, idx, bad;
        uint32_t seq;

        if (t >= next_send) {
            /* A fresh, monotonically increasing sequence in every frame. */
            tx_seq++;
            frame[SEQ_OFF + 0] = (tx_seq >> 24) & 0xff;
            frame[SEQ_OFF + 1] = (tx_seq >> 16) & 0xff;
            frame[SEQ_OFF + 2] = (tx_seq >> 8) & 0xff;
            frame[SEQ_OFF + 3] = tx_seq & 0xff;
            if (sendto(fd, frame, FRAME_LEN, 0,
                       (struct sockaddr *)&to, sizeof(to)) < 0 &&
                errno != EINTR) {
                perror("sendto");
            }
            next_send = t + SEND_EVERY_MS;
        }

        /*
         * A peer that has gone quiet is reported ONCE, at the edge. We do NOT
         * exit and we do NOT fail: a departed peer and a stalled one are the
         * same observation from here, and neither is ours to adjudicate.
         */
        for (int i = 0; i < npeers; i++) {
            if (ever[i] && !lost[i] && t - last_rx[i] > QUIET_MS) {
                lost[i] = 1;
                printf("ENET-LAB3 LOST: 0x%04x (silent %ldms)\n",
                       peers[i], t - last_rx[i]);
                fflush(stdout);
            }
        }

        /* Only ever fail if we have NEVER completed a round. */
        if (!passed_once && t >= deadline) {
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
        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 14) {
            continue;
        }
        et = (buf[12] << 8) | buf[13];
        if (et == my_et) {
            continue;                   /* rule 1: our own echo is not a peer */
        }

        /* Rule 2: is this even our protocol? If not, it is none of our business. */
        idx = -1;
        for (int i = 0; i < npeers; i++) {
            if (et == peers[i]) {
                idx = i;
            }
        }
        if (idx < 0) {
            continue;                   /* NOT counted, NOT condemned, NOT printed */
        }

        /*
         * Rule 3: presence is not integrity.
         *
         * The length is part of the contract: FRAME_LEN is EXACTLY 64, not "at
         * least 64". This used to accept anything >= 64, and that leniency is a
         * bug in its own right - rt1180 spent 90 minutes shipping a 1000-byte
         * frame with a valid 64-byte prefix, which every enforcing node on the
         * segment threw away and WE WOULD HAVE COUNTED.
         *
         *   A RECEIVER THAT IS MORE PERMISSIVE THAN THE SEGMENT COUNTS PEERS
         *   THAT EVERYONE ELSE IS REJECTING - and then it is OUR green that is
         *   the lie, because ours is the only one that came back.
         */
        bad = BAD_OK;
        seq = 0;
        if (n != FRAME_LEN) {
            bad = BAD_SHORT;
        } else if (be32(buf + MAGIC_OFF) != BEACON_MAGIC) {
            bad = BAD_MAGIC;
        } else if (((unsigned)buf[SELF_ET_OFF] << 8 | buf[SELF_ET_OFF + 1]) != et) {
            bad = BAD_SELF_ET;          /* the frame contradicts itself */
        } else {
            for (int i = FILL_OFF; i < FRAME_LEN; i++) {
                if (buf[i] != FILL_BYTE) {
                    bad = BAD_PATTERN;
                    break;
                }
            }
        }

        /* Rule 4: freshness. A replayed beacon is stale, not a sighting. */
        if (bad == BAD_OK) {
            seq = be32(buf + SEQ_OFF);
            if (ever[idx] && seq <= last_seq[idx]) {
                bad = BAD_STALE;
            }
        }

        if (bad != BAD_OK) {
            static const char * const why[] = {
                "", "wrong length", "bad magic",
                "payload ethertype disagrees", "pattern broken",
                "STALE (replayed sequence)"
            };
            printf("ENET-LAB3 CORRUPT: et=0x%04x %s len=%zd(want %d) "
                   "magic=0x%08x self-et=0x%04x seq=%u\n",
                   et, why[bad], n, FRAME_LEN,
                   n == FRAME_LEN ? be32(buf + MAGIC_OFF) : 0,
                   n == FRAME_LEN
                       ? ((unsigned)buf[SELF_ET_OFF] << 8 | buf[SELF_ET_OFF + 1])
                       : 0,
                   n == FRAME_LEN ? be32(buf + SEQ_OFF) : 0);
            fflush(stdout);
            continue;                   /* corrupt is NOT a peer sighting */
        }

        /* A gap is a LOSS: a statistic, never a failure. This is multicast. */
        if (ever[idx] && seq > last_seq[idx] + 1) {
            losses += seq - last_seq[idx] - 1;
        }
        if (!ever[idx]) {
            printf("ENET-LAB3 rx: peer 0x%04x body OK src "
                   "%02x:%02x:%02x:%02x:%02x:%02x seq=%u\n", et,
                   buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], seq);
            fflush(stdout);
        }
        ever[idx] = 1;
        lost[idx] = 0;
        last_seq[idx] = seq;
        last_rx[idx] = t;
        seen[idx] = 1;

        /* Rule 5: re-arm. Having seen them all, require them all again. */
        all = 1;
        for (int i = 0; i < npeers; i++) {
            if (!seen[i]) {
                all = 0;
            }
        }
        if (all) {
            beat++;
            passed_once = 1;
            printf("ENET-LAB3 PASS: t=%ld.%03lds peers=%d/%d beat=%llu "
                   "loss=%llu\n", (t - t0) / 1000, (t - t0) % 1000,
                   npeers, npeers, beat, losses);
            fflush(stdout);
            memset(seen, 0, sizeof(seen));      /* re-arm: an oracle that cannot expire */
        }
    }
}
