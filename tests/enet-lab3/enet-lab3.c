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
 *     Frames outside the fleet's ethertype block are IGNORED: not condemned,
 *     not printed - but they ARE COUNTED and reported on the PASS line, because
 *     "we never false-CORRUPTed at IPv6" and "there was no IPv6" are the same
 *     log, and only one of them is a result. The Linux peers' kernels emit
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
 *  4. FRESHNESS, SCOPED TO AN INCARNATION. Within one boot of a peer, its
 *     sequence only ever goes UP; a seq that has not advanced is a REPLAY - what
 *     a stalled RX ring, or a buffer that is never rewritten, looks like from the
 *     outside. It is CORRUPT, not a sighting.
 *       A PEER THAT ONLY EVER REPEATS ITSELF IS SAYING NOTHING NEW, AND A NODE
 *       SAYING NOTHING NEW IS SAYING NOTHING.  (mcxn947)
 *     But "within one boot" is load-bearing: a peer that REBOOTS restarts its
 *     counter at 1, and that is NOT a replay (see INCARN_OFF below). A seq that
 *     goes backwards under a NEW incarnation is a reboot - re-baselined and
 *     counted, never condemned.
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
#include <fcntl.h>
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

/* The fleet's checkable body. Do not "improve" any of these numbers. */
#define BEACON_MAGIC 0xB5B6B7C0u
#define MAGIC_OFF    14
#define SELF_ET_OFF  18
#define SEQ_OFF      20
#define INCARN_OFF   24         /* per-BOOT nonce - see below. v2 of the body. */
#define FILL_OFF     28
#define FILL_BYTE    0x5A

/*
 * A v1 (pre-incarnation) node fills [24..27] with 0x5A, so its "incarnation"
 * reads 0x5A5A5A5A on every node and every boot. That is not a nonce; it is the
 * ABSENCE of one, and it is legible AS such. rt1180's call, and it is the right
 * one: DO NOT condemn a legacy peer and DO NOT treat the sentinel as a real
 * incarnation - count the peer, check everything else, and declare its freshness
 * UNVERIFIABLE. A red we cannot stand behind is worse than no red, and the seq
 * rule fired 8,982 times at an honest peer once already.
 */
#define INCARN_LEGACY 0x5A5A5A5Au

/*
 * THE INCARNATION, AND WHY A SEQUENCE NUMBER ALONE IS A BUG.
 *
 * Rule 4 said: a beacon's sequence only ever goes UP, so a seq that has not
 * advanced is a REPLAY - a stalled RX ring handing us the same buffer forever.
 * Every node in the fleet adopted that check, and it caught real replays.
 *
 * It is also WRONG, and holobench proved it by doing the one thing no unit test
 * does: they KILLED a peer mid-run and BROUGHT IT BACK. mcx departed at t+420
 * and rejoined at t+480 - a fresh QEMU, fresh firmware, and a beacon counter
 * that starts over at 1. Our node, and rt1180's, independently condemned it
 * ~9,000 times each:
 *
 *   ENET-LAB3 CORRUPT: PAYLOAD-REPLAY peer 0x88b5 seq 1 <= last 13485
 *
 *   A PEER THAT RESTARTED IS NOT A PEER THAT REPLAYED. The freshness check
 *   cannot tell "my RX path handed me an old frame" from "my peer REBOOTED and
 *   started counting again" - and it condemns an honest, healthy, freshly-booted
 *   peer, forever, as a stale buffer.
 *
 * No stale buffer can produce a monotonically INCREASING run starting at 1. The
 * information needed to tell the two apart was never on the wire.
 *
 * So the body carries an INCARNATION: 4 bytes chosen once at boot, never again.
 *
 *   seq went BACKWARDS + SAME incarnation  ->  REPLAY. A stale buffer. CONDEMN.
 *   seq went BACKWARDS + NEW  incarnation  ->  A REBOOT. Re-baseline. COUNT IT.
 *
 * This is TCP's ISN, DTLS's epoch, a Lamport clock's epoch. Every protocol that
 * survives a peer restart has one, for exactly this reason - and a board farm
 * restarts boards for a living.
 *
 *   WITHOUT AN INCARNATION, "MONOTONIC" IS A CLAIM ABOUT A PROCESS, NOT ABOUT A
 *   PEER.
 *
 * ⚠ THIS IS A FLAG DAY. The nonce lands at [24..27], which the v1 body filled
 * with 0x5A - so a v1 receiver reads a v2 frame as BAD_PATTERN, and a v2
 * receiver reads a v1 frame the same way. There is no compatible half-step: the
 * frame is 64 bytes EXACTLY and every spare byte was already spoken for. All
 * four nodes move together or the segment is red, and a red segment during the
 * cutover is CORRECT - it is the contract being enforced, not a regression.
 */
#define INCARN_LEN   4

/*
 * The fleet's ALLOCATED ETHERTYPE BLOCK, and the distinction that goes with it.
 *
 *   VALIDATING A PEER IS NOT THE SAME AS DEPENDING ON ONE.   (rt1180emulator)
 *
 * We validate the body of ANY node in the block - read its magic, its
 * self-ethertype, its fill and its sequence, and say so. We only REQUIRE the
 * peers we were actually given on the command line. Those are different acts and
 * only one of them is worth anything to the peer being checked.
 *
 * This started as a bug of mine: I added imx91's 0x88B8 to the REQUIRED set,
 * which meant a lab with imx91 absent would fail on my side for no reason - I
 * conflated "I check your bytes" with "I cannot pass without you". And watching
 * a fixed list is itself the smaller mistake:
 *
 *   IF A PEER SET IS A CONSTANT, EVERY FUTURE NODE IS A FIRMWARE RELEASE.
 *
 * So a fifth node joins this segment by picking an ethertype in the block, not
 * by making four other teams rebuild.
 */
#define BEACON_ET_LO   0x88B5u
#define BEACON_ET_HI   0x88BFu
#define BEACON_ET_SLOTS (BEACON_ET_HI - BEACON_ET_LO + 1)
#define IS_BEACON_ET(et) ((et) >= BEACON_ET_LO && (et) <= BEACON_ET_HI)

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

/*
 * Wall-clock Unix epoch in ms, for the PASS line's t= field ONLY. Internal
 * pacing and quiet-detection use now_ms() (CLOCK_MONOTONIC) so a stepped clock
 * cannot disturb them; but holobench cross-aligns the four nodes' departure
 * windows by their guest-emitted t=, which must be a COMMON absolute base, not
 * each node's elapsed-since-start. 91 and rt1180 emit gettimeofday/SYS_TIME
 * epochs; this matches them.
 *
 * The caveat travels with the number (91's, taken verbatim): without -icount
 * this tracks HOST wall-clock, so absolute stamps drift under load. Trust a GAP
 * in t= to bracket a departure to ~beat/100 ms resolution; do NOT build a
 * sub-100 ms absolute-timing claim on it. A timestamp trusted blindly is the
 * next bug.
 */
static long now_epoch_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
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
    uint32_t my_incarn = 0;    /* THIS boot of THIS node. Chosen once. */
    /* REQUIRED peers: these, and only these, gate PASS. */
    unsigned peers[MAX_PEERS];
    int seen[MAX_PEERS] = {0};          /* seen THIS round (we re-arm) */
    int npeers = 0;
    /* VALIDATED peers: every node in the allocated block, required or not. */
    int blk_ever[BEACON_ET_SLOTS] = {0};
    uint32_t blk_last_seq[BEACON_ET_SLOTS] = {0};
    uint32_t blk_incarn[BEACON_ET_SLOTS] = {0};   /* which BOOT of that peer */
    int blk_legacy_warned[BEACON_ET_SLOTS] = {0}; /* said "v1, unverifiable" once */
    unsigned long long reboots = 0;               /* a statistic, NEVER a failure */
    unsigned long long legacy_seen = 0;           /* frames from pre-incarnation peers */
    long blk_last_rx[BEACON_ET_SLOTS] = {0};
    int blk_lost[BEACON_ET_SLOTS] = {0};
    unsigned long long losses = 0;
    /*
     * Count what we IGNORE. "We never fired a false CORRUPT at IPv6" and "there
     * was no IPv6" are the same log, and the second one is not a result. A
     * negative test also ROTS GREEN when the model improves under it, so the
     * only durable form of this claim is the POSITIVE fact: how many foreign
     * frames were actually on the wire that we declined to condemn.
     */
    unsigned long long foreign_ignored = 0;
    unsigned long long self_ignored = 0;
    int fd, ifindex;
    unsigned char mymac[6];
    unsigned char frame[FRAME_LEN + 936];   /* impostor tail lives here */
    int impostor_len = 0;
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

    /*
     * Our incarnation: chosen ONCE, here, and never again. It says "this is a
     * different boot of the same node" - the one fact a sequence number cannot
     * carry across a restart.
     *
     * rt1180 paid for the subtlety and I will not repeat it: /dev/urandom is NOT
     * automatically per-boot. In a TCG guest with no seeded CRNG - or under
     * -icount/-seed for reproducibility - it can return the SAME bytes every
     * boot, and then the "nonce" is a constant wearing a nonce's name, every peer
     * keeps condemning our restart, and NO SINGLE-BOOT TEST CAN SEE IT. So we do
     * not TRUST any one source: we MIX urandom with values that vary across boots
     * by construction (wall clock, pid, and this run's stack address under ASLR),
     * UNCONDITIONALLY - not only on the failure path. If urandom is real entropy
     * the mix is still real entropy; if urandom is a constant, the clock still
     * moves. The only way the result repeats is if EVERY source is pinned, which
     * is the deterministic-replay case the lab opts into deliberately with -seed.
     *
     * Two values are then forbidden: 0 (our "not yet seen" marker on the RX side)
     * and INCARN_LEGACY (a real node must never accidentally emit the sentinel
     * that means "I have no incarnation"). Nudge off either.
     */
    {
        uint32_t ur = 0;
        int rf = open("/dev/urandom", O_RDONLY);

        if (rf >= 0) {
            if (read(rf, &ur, sizeof(ur)) != sizeof(ur)) {
                ur = 0;
            }
            close(rf);
        }
        my_incarn = ur
                  ^ ((uint32_t)time(NULL) * 2654435761u)
                  ^ ((uint32_t)getpid() * 2246822519u)
                  ^ (uint32_t)(uintptr_t)&frame;
        if (my_incarn == 0 || my_incarn == INCARN_LEGACY) {
            my_incarn ^= 0x9e3779b9u;    /* off the two reserved values */
        }
        frame[INCARN_OFF + 0] = (my_incarn >> 24) & 0xff;
        frame[INCARN_OFF + 1] = (my_incarn >> 16) & 0xff;
        frame[INCARN_OFF + 2] = (my_incarn >> 8) & 0xff;
        frame[INCARN_OFF + 3] = my_incarn & 0xff;
    }
    frame[SELF_ET_OFF + 0] = (my_et >> 8) & 0xff;   /* must equal bytes 12..13 */
    frame[SELF_ET_OFF + 1] = my_et & 0xff;
    memset(frame + FILL_OFF, FILL_BYTE, FRAME_LEN - FILL_OFF);

    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_ifindex = ifindex;
    to.sll_halen = 6;
    memset(to.sll_addr, 0xff, 6);

    /*
     * THE IMPOSTOR: a frame with a PERFECTLY VALID 64-byte prefix and 936 bytes
     * of tail. Every field a receiver reads at a fixed offset is correct - magic,
     * self-ethertype, sequence, fill - and the frame is still not the contract,
     * because FRAME_LEN is 64 EXACTLY.
     *
     * This exists because tightening the length check is not the same as HAVING
     * tightened it. 91emulator shipped exactly that fix, sent a 1000-byte frame
     * at it, and their honest node COUNTED THE LIAR 268 TIMES ANYWAY - the
     * over-long frame carried no valid body, so their self-arming latch filed it
     * as "a peer that has not upgraded yet" and counted it. Their leniency was
     * never in the length check at all.
     *
     *   IF YOUR PERMISSIVENESS LIVES IN A LATCH OR A FALLBACK, YOUR LENGTH FIX
     *   IS A NO-OP AND YOUR SUITE WILL STILL BE GREEN.       (91emulator)
     *
     * So we do not reason about our own receiver; we send it the frame and
     * require it to go to ZERO.
     */
    if (getenv("LAB_IMPOSTOR")) {
        impostor_len = FRAME_LEN + 936;     /* the shape rt1180 actually shipped */
        printf("ENET-LAB3 EVIL: sending %d-byte frames with a valid 64-byte "
               "prefix (a beacon that is speaking the protocol WRONG)\n",
               impostor_len);
        fflush(stdout);
    }

    dl_env = getenv("LAB_DEADLINE_MS");
    dl_ms = dl_env ? strtol(dl_env, NULL, 0) : 120000;

    /*
     * DECLARE OUR CONTRACT ON THE WIRE, ONCE. holobench cannot score a node's
     * CORRUPT verdicts until it knows what that node PROMISED - "mcx + imx95
     * print no ENET-LAB3 UP: line, so their contract is UNDECLARED and their
     * CORRUPT is unscoreable". So a red we emit is un-auditable until we say, up
     * front and in the format the scorer greps, exactly what we are: which
     * ethertype, how many required peers gate our PASS, that we EMIT the body
     * (not just check it), and that we self-arm (re-arm every round, never
     * latch). rt1180 and imx91 already declare; this closes the gap on our side.
     */
    printf("ENET-LAB3 UP: ethertype=0x%04x peers=%d body=emit "
           "enforce=self-arming\n", my_et, npeers);
    fflush(stdout);

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
        int pr, all, idx, bad, slot, is_legacy;
        uint32_t seq;

        if (t >= next_send) {
            /* A fresh, monotonically increasing sequence in every frame. */
            tx_seq++;
            frame[SEQ_OFF + 0] = (tx_seq >> 24) & 0xff;
            frame[SEQ_OFF + 1] = (tx_seq >> 16) & 0xff;
            frame[SEQ_OFF + 2] = (tx_seq >> 8) & 0xff;
            frame[SEQ_OFF + 3] = tx_seq & 0xff;
            if (sendto(fd, frame, impostor_len ? impostor_len : FRAME_LEN, 0,
                       (struct sockaddr *)&to, sizeof(to)) < 0 &&
                errno != EINTR) {
                perror("sendto");
            }
            next_send = t + SEND_EVERY_MS;
        }

        /*
         * A node that has gone quiet is reported ONCE, at the edge - for ANY
         * node in the block, not just the ones we depend on. We do NOT exit and
         * we do NOT fail: a departed peer and a stalled one are the same
         * observation from here, and neither is ours to adjudicate.
         */
        for (unsigned i = 0; i < BEACON_ET_SLOTS; i++) {
            if (blk_ever[i] && !blk_lost[i] &&
                t - blk_last_rx[i] > QUIET_MS) {
                blk_lost[i] = 1;
                printf("ENET-LAB3 LOST: 0x%04x (silent %ldms)\n",
                       BEACON_ET_LO + i, t - blk_last_rx[i]);
                fflush(stdout);
            }
        }

        /*
         * Only ever fail if we have NEVER completed a round.
         *
         * A NODE MAY ONLY REPORT WHAT ITS OWN VANTAGE CAN SEE. We can see that
         * no frames of a required ethertype arrived before our deadline. We
         * CANNOT see whether that peer was silent or never existed at all -
         * and those are different results. holobench's coordinator hit exactly
         * this: a run where the silicon nodes were never launched, and another
         * where a peer's beacon was blocked by sudo needing a tty, both of
         * which a node reporting "missing peers" would have laundered into a
         * WIRE failure. So the token and the verdict stay (our gate genuinely
         * did not close - that IS a true statement about our own state) and
         * only the implied CAUSE goes. Whether the run is INCONCLUSIVE or FAIL
         * belongs to the coordinator, which can prove peer-start; we must not
         * pre-empt it. Same shape as a scorer calling a crashed writer "nothing
         * arrived": A CLAIM THAT OUTRUNS THE VANTAGE THAT PRODUCED IT.
         */
        if (!passed_once && t >= deadline) {
            printf("ENET-LAB3 FAIL: deadline, no frames from required peer(s):");
            for (int i = 0; i < npeers; i++) {
                if (!seen[i]) {
                    printf(" 0x%04x", peers[i]);
                }
            }
            printf(" - ABSENT or SILENT, this node cannot tell which; "
                   "coordinator decides INCONCLUSIVE vs FAIL\n");
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
            self_ignored++;             /* rule 1: our own echo is not a peer */
            continue;
        }

        /*
         * Rule 2: is this even our protocol? If not, it is none of our business
         * - NOT condemned, NOT printed. But it IS counted, because otherwise
         * "we never false-CORRUPTed at IPv6" and "there was no IPv6" are the
         * same log, and only one of them is a result.
         *
         * We validate ANY node in the fleet's allocated block, not a hardcoded
         * pair: a fifth node joins by picking an ethertype, not by making four
         * other teams rebuild.
         */
        if (!IS_BEACON_ET(et)) {
            foreign_ignored++;
            continue;
        }
        slot = et - BEACON_ET_LO;

        /* Is this one of the peers we actually DEPEND on? (-1 = observed only) */
        idx = -1;
        for (int i = 0; i < npeers; i++) {
            if (et == peers[i]) {
                idx = i;
            }
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
        is_legacy = 0;
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

        /*
         * Rule 4: freshness - and it is only meaningful WITHIN ONE INCARNATION.
         *
         * A sequence that goes backwards means one of two completely different
         * things, and the seq alone cannot tell you which:
         *
         *   SAME incarnation -> the peer never restarted, so a seq we have
         *                       already seen can only be OUR RX path handing us
         *                       a stale buffer. That is the bug this rule exists
         *                       to catch. CONDEMN.
         *   NEW  incarnation -> the peer REBOOTED. Its counter legitimately
         *                       starts over. Re-baseline and count it. Condemning
         *                       this is condemning a healthy node for being
         *                       restarted, which is what a board farm DOES.
         */
        if (bad == BAD_OK) {
            uint32_t incarn = be32(buf + INCARN_OFF);

            seq = be32(buf + SEQ_OFF);

            if (incarn == INCARN_LEGACY) {
                /*
                 * A v1 peer, which had no incarnation and filled these 4 bytes
                 * with 0x5A. We can still check its magic, self-ethertype, length
                 * and fill - and we DO, and it is COUNTED as a sighting. But we
                 * cannot check its freshness: with no incarnation, a restart and
                 * a stalled ring are indistinguishable, which is the whole reason
                 * v2 exists. So we decline to render that one verdict rather than
                 * render a false one. A red we cannot stand behind is worse than
                 * no red - the seq rule fired 8,982 times at an honest peer once.
                 * A legacy node interoperates; it just gets a weaker, honestly
                 * labelled guarantee until it upgrades. (rt1180's call.)
                 */
                if (!blk_legacy_warned[slot]) {
                    printf("ENET-LAB3 LEGACY: peer 0x%04x has no incarnation "
                           "(v1 body); freshness UNVERIFIABLE, so it does NOT "
                           "satisfy a required-peer gate - segment stays red "
                           "until it carries a real incarnation\n",
                           BEACON_ET_LO + slot);
                    fflush(stdout);
                    blk_legacy_warned[slot] = 1;
                }
                is_legacy = 1;
                /*
                 * Record that this slot is IN the legacy state, so that when the
                 * peer later UPGRADES and sends a real nonce, the reboot/replay
                 * branches (which both exclude INCARN_LEGACY) correctly treat it
                 * as first-real-contact and not as a restart. Without this,
                 * blk_incarn stays 0 through the legacy phase and the first real
                 * frame reads as a reboot - the exact first-frame degenerate case
                 * rt1180 flagged, one transition over.
                 */
                blk_incarn[slot] = INCARN_LEGACY;
                legacy_seen++;
            } else if (blk_ever[slot] && blk_incarn[slot] != INCARN_LEGACY &&
                       incarn != blk_incarn[slot]) {
                printf("ENET-LAB3 REBOOT: peer 0x%04x incarnation %08x -> %08x "
                       "(seq restarts at %u; NOT a replay)\n",
                       BEACON_ET_LO + slot, blk_incarn[slot], incarn, seq);
                fflush(stdout);
                reboots++;
                blk_incarn[slot] = incarn;
                blk_last_seq[slot] = 0;     /* re-baseline: a new boot, a new run */
            } else if (blk_ever[slot] && blk_incarn[slot] != INCARN_LEGACY &&
                       seq <= blk_last_seq[slot]) {
                bad = BAD_STALE;            /* same boot, old seq: a REAL replay */
            } else {
                /* First real incarnation seen (or first after a legacy phase). */
                blk_incarn[slot] = incarn;
            }
        }

        if (bad != BAD_OK) {
            static const char * const why[] = {
                "", "wrong length", "bad magic",
                "payload ethertype disagrees", "pattern broken",
                "STALE (replayed sequence)"
            };
            /*
             * Report the magic whenever the bytes are THERE to read, not only
             * when the frame is well-formed. A frame carrying 0xB5B6B7C0 IS
             * speaking the protocol - it is just speaking it WRONG - and that
             * is a different peer from one that has never spoken it at all.
             * "Hasn't shipped the emitter" and "shipped a BROKEN emitter" are
             * not the same peer, and the magic is what tells them apart.
             */
            uint32_t mg = n >= MAGIC_OFF + 4 ? be32(buf + MAGIC_OFF) : 0;

            printf("ENET-LAB3 CORRUPT: et=0x%04x %s len=%zd(want %d) "
                   "magic=0x%08x%s\n",
                   et, why[bad], n, FRAME_LEN, mg,
                   mg == BEACON_MAGIC ? " [a BROKEN beacon, not a stranger]" : "");
            fflush(stdout);
            continue;                   /* corrupt is NOT a peer sighting */
        }

        /* A gap is a LOSS: a statistic, never a failure. This is multicast. */
        if (blk_ever[slot] && seq > blk_last_seq[slot] + 1) {
            losses += seq - blk_last_seq[slot] - 1;
        }
        /*
         * We say so the FIRST time we accept a body - for every node in the
         * block, whether we depend on it or not. Reading someone's bytes and
         * refusing to say so are different acts, and only one of them is worth
         * anything to them: 91emulator asked who was validating their beacon and
         * the honest answer from three of four nodes was "nobody".
         */
        if (!blk_ever[slot]) {
            printf("ENET-LAB3 rx: peer 0x%04x body OK src "
                   "%02x:%02x:%02x:%02x:%02x:%02x seq=%u%s\n", et,
                   buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], seq,
                   idx < 0 ? " (observed, not required)" : "");
            fflush(stdout);
        }
        blk_ever[slot] = 1;
        blk_lost[slot] = 0;
        blk_last_seq[slot] = seq;
        blk_last_rx[slot] = t;

        /*
         * Only the peers we CONTRACTED with gate PASS. Validating is not
         * depending: a lab without imx91 is not a lab we should fail.
         *
         * A LEGACY (pre-incarnation) peer does NOT satisfy that gate, even if it
         * is one we contracted with. It was rt1180 who caught this, in their tree
         * and by implication in mine: counting a peer whose freshness you cannot
         * verify produces "a green that means a node quietly stayed on the old
         * body" - the exact masking-green I had warned the fleet about, and then
         * shipped one commit later. During a ratified cutover a not-yet-upgraded
         * required peer MUST hold the segment red; that red is the forcing
         * function, not a regression. So we track and announce the legacy peer
         * but refuse to let it close the gate.
         */
        if (idx >= 0 && !is_legacy) {
            seen[idx] = 1;
        }

        /* Rule 5: re-arm. Having seen them all, require them all again. */
        all = 1;
        for (int i = 0; i < npeers; i++) {
            if (!seen[i]) {
                all = 0;
            }
        }
        if (all) {
            int validated = 0;

            for (unsigned i = 0; i < BEACON_ET_SLOTS; i++) {
                validated += blk_ever[i];
            }
            long ep = now_epoch_ms();

            beat++;
            passed_once = 1;
            /*
             * t= is an ABSOLUTE Unix epoch (now_epoch_ms), not elapsed-since-
             * start, so holobench can cross-align this node's beat timeline with
             * the other three and bracket a survivor's departure directly off the
             * wire - the one measurement its arrival-stamping cannot make. Format
             * matches 91/rt1180 (seconds.milliseconds). See now_epoch_ms() for
             * the ~beat-resolution caveat that ships with it.
             *
             * foreign= is the POSITIVE fact behind "we do not condemn traffic
             * that is not our protocol". Without it, that claim cannot testify
             * that the condition was ever present - and a negative test rots
             * green when nothing is left to trigger it.
             */
            printf("ENET-LAB3 PASS: t=%ld.%03lds peers=%d/%d validated=%d "
                   "beat=%llu loss=%llu foreign=%llu self=%llu "
                   "reboots=%llu legacy=%llu\n",
                   ep / 1000, ep % 1000, npeers, npeers,
                   validated, beat, losses, foreign_ignored, self_ignored,
                   reboots, legacy_seen);
            fflush(stdout);
            memset(seen, 0, sizeof(seen));      /* re-arm: an oracle that cannot expire */
        }
    }
}
