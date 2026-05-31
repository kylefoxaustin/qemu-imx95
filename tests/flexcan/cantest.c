// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cantest - a tiny self-contained SocketCAN loopback self-test.
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * Brings up two CAN interfaces (bitrate via rtnetlink, then IFF_UP), then
 * sends a burst of frames each way and verifies they arrive intact on the
 * other interface. Used to validate the QEMU FlexCAN model end-to-end: run
 * two FlexCAN controllers on one emulated can-bus so the guest sees can0 and
 * can1, then "cantest can0 can1" exercises a real Linux flexcan TX -> bus ->
 * RX path in both directions.
 *
 * Self-contained on purpose: no iproute2 / can-utils needed, so it drops into
 * a BusyBox initramfs. Static-link it for aarch64.
 *
 * Usage: cantest <if_a> <if_b> [bitrate] [count]
 * Prints "FLEXCAN SELFTEST: PASS" / "FAIL ..." and exits 0 / 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <linux/rtnetlink.h>

/* Append a netlink attribute to nlmsghdr at *nlh (buffer cap bytes). */
static struct rtattr *nla_put(struct nlmsghdr *nlh, size_t cap, int type,
                              const void *data, int len)
{
    struct rtattr *rta = (void *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));

    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_LENGTH(len) > cap) {
        return NULL;
    }
    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    if (data && len) {
        memcpy(RTA_DATA(rta), data, len);
    }
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

static void nla_end(struct nlmsghdr *nlh, struct rtattr *rta)
{
    rta->rta_len = (char *)nlh + nlh->nlmsg_len - (char *)rta;
}

static int nl_talk(struct nlmsghdr *nlh)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    char buf[4096];
    struct nlmsghdr *r;
    int n, err = 0;

    if (fd < 0) {
        return -errno;
    }
    nlh->nlmsg_flags |= NLM_F_ACK;
    if (send(fd, nlh, nlh->nlmsg_len, 0) < 0) {
        err = -errno;
        goto out;
    }
    n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
        err = -errno;
        goto out;
    }
    for (r = (struct nlmsghdr *)buf; NLMSG_OK(r, n); r = NLMSG_NEXT(r, n)) {
        if (r->nlmsg_type == NLMSG_ERROR) {
            err = ((struct nlmsgerr *)NLMSG_DATA(r))->error;
            break;
        }
    }
out:
    close(fd);
    return err;
}

/* "ip link set <if> type can bitrate <bitrate>" via rtnetlink. */
static int can_set_bitrate(unsigned ifindex, uint32_t bitrate)
{
    char buf[512] = { 0 };
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi;
    struct rtattr *linkinfo, *infodata;
    struct can_bittiming bt = { .bitrate = bitrate };

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    ifi = NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;

    linkinfo = nla_put(nlh, sizeof(buf), IFLA_LINKINFO, NULL, 0);
    nla_put(nlh, sizeof(buf), IFLA_INFO_KIND, "can", 4);
    infodata = nla_put(nlh, sizeof(buf), IFLA_INFO_DATA, NULL, 0);
    nla_put(nlh, sizeof(buf), IFLA_CAN_BITTIMING, &bt, sizeof(bt));
    nla_end(nlh, infodata);
    nla_end(nlh, linkinfo);

    return nl_talk(nlh);
}

static int link_set_up(unsigned ifindex)
{
    char buf[256] = { 0 };
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi;

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    ifi = NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = IFF_UP;
    ifi->ifi_change = IFF_UP;

    return nl_talk(nlh);
}

static int can_open(const char *ifname)
{
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    struct sockaddr_can addr = { 0 };

    if (fd < 0) {
        return -1;
    }
    addr.can_family = AF_CAN;
    addr.can_ifindex = if_nametoindex(ifname);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int bringup(const char *ifname, uint32_t bitrate)
{
    unsigned idx = if_nametoindex(ifname);
    int err;

    if (!idx) {
        fprintf(stderr, "cantest: %s: no such interface\n", ifname);
        return -1;
    }
    err = can_set_bitrate(idx, bitrate);
    if (err) {
        fprintf(stderr, "cantest: %s: set bitrate failed: %s\n", ifname,
                strerror(-err));
        return -1;
    }
    err = link_set_up(idx);
    if (err) {
        fprintf(stderr, "cantest: %s: link up failed: %s\n", ifname,
                strerror(-err));
        return -1;
    }
    return 0;
}

/* Send `count` frames on tx, receive + verify on rx. Returns 0 on success. */
static int run_dir(int tx, int rx, const char *txn, const char *rxn, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        struct can_frame out = { 0 }, in = { 0 };
        struct pollfd pfd = { .fd = rx, .events = POLLIN };
        int n;

        out.can_id = 0x100 + i;
        out.can_dlc = 8;
        for (n = 0; n < 8; n++) {
            out.data[n] = (uint8_t)(i + n);
        }
        if (write(tx, &out, sizeof(out)) != sizeof(out)) {
            fprintf(stderr, "cantest: %s write failed: %s\n", txn,
                    strerror(errno));
            return -1;
        }
        if (poll(&pfd, 1, 2000) <= 0) {
            fprintf(stderr, "cantest: %s -> %s: timeout on frame %d\n",
                    txn, rxn, i);
            return -1;
        }
        if (read(rx, &in, sizeof(in)) != sizeof(in)) {
            fprintf(stderr, "cantest: %s read failed: %s\n", rxn,
                    strerror(errno));
            return -1;
        }
        if (in.can_id != out.can_id || in.can_dlc != out.can_dlc ||
            memcmp(in.data, out.data, 8)) {
            fprintf(stderr, "cantest: %s -> %s: frame %d mismatch "
                    "(id 0x%x/0x%x)\n", txn, rxn, i, out.can_id, in.can_id);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *a, *b;
    uint32_t bitrate = 500000;
    int count = 20, fa, fb;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <if_a> <if_b> [bitrate] [count]\n", argv[0]);
        return 2;
    }
    a = argv[1];
    b = argv[2];
    if (argc > 3) {
        bitrate = strtoul(argv[3], NULL, 0);
    }
    if (argc > 4) {
        count = atoi(argv[4]);
    }

    if (bringup(a, bitrate) || bringup(b, bitrate)) {
        printf("FLEXCAN SELFTEST: FAIL (bring-up)\n");
        return 1;
    }
    fa = can_open(a);
    fb = can_open(b);
    if (fa < 0 || fb < 0) {
        printf("FLEXCAN SELFTEST: FAIL (socket)\n");
        return 1;
    }

    if (run_dir(fa, fb, a, b, count) || run_dir(fb, fa, b, a, count)) {
        printf("FLEXCAN SELFTEST: FAIL\n");
        return 1;
    }

    printf("FLEXCAN SELFTEST: PASS (%d frames each way, %s <-> %s @ %u bps)\n",
           count, a, b, bitrate);
    return 0;
}
