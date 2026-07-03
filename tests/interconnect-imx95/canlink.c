// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * canlink - a two-role SocketCAN oracle for the i.MX 95 board-to-board CAN
 * link test (tests/interconnect-imx95/run-can.sh).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * Unlike tests/flexcan/cantest (a one-instance loopback self-test), this splits
 * into send/recv roles so two QEMU i.MX 95 instances - each a FlexCAN on a local
 * can-bus joined to the other by a can-host-chardev socket bridge - can prove a
 * CAN frame crosses byte-exact between guests:
 *
 *   canlink send <if> [count]   bring <if> up, transmit a known frame `count`x
 *   canlink recv <if>           bring <if> up, wait for that frame, verify it
 *
 * The receiver boots at a different time than the sender, so the sender resends
 * across a window and the receiver watches a window - a match anywhere is a PASS
 * (CANLINK:PASS), like the SPI-link oracle. Self-contained (raw rtnetlink bring-
 * up, no iproute2/can-utils); static-link for aarch64.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <linux/rtnetlink.h>

/* The known frame that crosses the link. */
#define LINK_CAN_ID   0x321
#define LINK_DLC      8
static const uint8_t LINK_DATA[8] = { 'C', 'A', 'N', 'L', 'i', 'n', 'k', '!' };

/* ---- rtnetlink bring-up helpers (same shape as tests/flexcan/cantest) ------ */

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

static int bringup(const char *ifname, uint32_t bitrate)
{
    unsigned idx = if_nametoindex(ifname);
    int err;

    if (!idx) {
        fprintf(stderr, "canlink: %s: no such interface\n", ifname);
        return -1;
    }
    err = can_set_bitrate(idx, bitrate);
    if (err) {
        fprintf(stderr, "canlink: %s: set bitrate failed: %s\n", ifname,
                strerror(-err));
        return -1;
    }
    err = link_set_up(idx);
    if (err) {
        fprintf(stderr, "canlink: %s: link up failed: %s\n", ifname,
                strerror(-err));
        return -1;
    }
    return 0;
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

/* ---- roles ----------------------------------------------------------------- */

static int do_send(const char *ifn, int count)
{
    struct can_frame f = { 0 };
    int fd, i;

    if (bringup(ifn, 500000)) {
        printf("CANLINK:FAIL:send bring-up\n");
        return 1;
    }
    fd = can_open(ifn);
    if (fd < 0) {
        printf("CANLINK:FAIL:send socket\n");
        return 1;
    }
    f.can_id = LINK_CAN_ID;
    f.can_dlc = LINK_DLC;
    memcpy(f.data, LINK_DATA, LINK_DLC);

    for (i = 0; i < count; i++) {
        if (write(fd, &f, sizeof(f)) == sizeof(f)) {
            printf("CANLINK:SENT id=0x%03x [%.8s]\n", LINK_CAN_ID, LINK_DATA);
        } else {
            printf("CANLINK:FAIL:send write %s\n", strerror(errno));
        }
        fflush(stdout);
        sleep(2);
    }
    return 0;
}

static int do_recv(const char *ifn)
{
    time_t start;
    int fd;

    if (bringup(ifn, 500000)) {
        printf("CANLINK:FAIL:recv bring-up\n");
        return 1;
    }
    fd = can_open(ifn);
    if (fd < 0) {
        printf("CANLINK:FAIL:recv socket\n");
        return 1;
    }

    start = time(NULL);
    while (time(NULL) - start < 45) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        struct can_frame in = { 0 };

        if (poll(&pfd, 1, 1000) <= 0) {
            continue;
        }
        if (read(fd, &in, sizeof(in)) != sizeof(in)) {
            continue;
        }
        if (in.can_id == LINK_CAN_ID && in.can_dlc == LINK_DLC &&
            !memcmp(in.data, LINK_DATA, LINK_DLC)) {
            printf("CANLINK:PASS: frame id=0x%03x [%.8s] crossed the CAN link "
                   "byte-exact\n", LINK_CAN_ID, LINK_DATA);
            return 0;
        }
        printf("CANLINK:recv other id=0x%03x dlc=%d\n", in.can_id, in.can_dlc);
    }
    printf("CANLINK:FAIL: no matching frame in window\n");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s send|recv <if> [count]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "send")) {
        return do_send(argv[2], argc > 3 ? atoi(argv[3]) : 15);
    }
    if (!strcmp(argv[1], "recv")) {
        return do_recv(argv[2]);
    }
    fprintf(stderr, "canlink: unknown role '%s'\n", argv[1]);
    return 2;
}
