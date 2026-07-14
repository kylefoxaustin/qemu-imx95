/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spilink - spidev oracle for the i.MX 93 SPI board-to-board link (run-spi.sh).
 * One instance is the SENDER: it clocks a payload out of its LPSPI master,
 * which the spi-link peripheral forwards over the socket to the peer. The other
 * is the RECEIVER: it clocks dummy bytes (so the spi-link shifts the peer's
 * bytes in on MISO) and collects the payload, then verifies it byte-exact. Idle
 * MISO reads back 0xff (the payload has no 0xff), so the receiver skips 0xff
 * while accumulating. Raw spidev ioctls, no libs, so it links -static for a
 * busybox initramfs. Emits SPILINK:PASS / SPILINK:FAIL:<why> / SPILINK:SENT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/spi/spidev.h>

static int xfer(int fd, const unsigned char *tx, unsigned char *rx, int len)
{
    struct spi_ioc_transfer tr;

    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;
    tr.bits_per_word = 8;
    tr.speed_hz = 1000000;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

static int setup(int fd)
{
    unsigned char mode = 0, bits = 8;
    unsigned int speed = 1000000;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        return -1;
    }
    return 0;
}

/*
 * THE FRAME THE PEER ACTUALLY HUNTS. Read out of the peer's own source
 * (mcxn947qemu tests/mcxn-spi-link/main.c: MARKER 0xA5, N 32,
 * expect(i) = (i*3+5) & 0x7F), not out of a description of it.
 *
 * This DEFAULT used to be the ASCII string "IMX95-SPI-LINK-payload-0123456789",
 * and the MCX responder hunts a binary pattern - so the link clocked A PERFECTLY
 * VALID STREAM OF THE WRONG BYTES. The peer resynced forever, printed nothing,
 * and the lab looked like a broken SPI model instead of two programs that never
 * agreed on the bytes. (The same week, and the same bug, as our ENET beacon
 * emitting ASCII where the fleet had agreed a binary body.)
 *
 *   TWO PROGRAMS THAT BOTH "WORK" AND NEVER AGREE ON THE BYTES.  (holobench)
 *
 * The pattern is generated here, once, and ASSERTED - never escaped through a
 * shell, a heredoc, a sed RHS or a printf. Every layer of escaping is a layer
 * that can eat a byte and then blame the hardware.
 */
#define SPI_MARKER  0xA5u
#define SPI_N       32
#define SPI_FRAME_LEN (1 + SPI_N)

static int build_frame(unsigned char *f)
{
    int i;

    f[0] = SPI_MARKER;
    for (i = 0; i < SPI_N; i++) {
        f[1 + i] = (unsigned char)((i * 3 + 5) & 0x7F);
    }
    /*
     * Three real hazards, asserted rather than hoped for: a NUL would end the
     * frame if anyone ever treats it as a C string, 0xFF is the IDLE MISO byte
     * the receiver skips, and a second MARKER would make the peer resync in the
     * middle of a good frame. The 0x7F mask makes all three impossible - but an
     * assertion that holds today is what stops someone "improving" the pattern
     * tomorrow.
     */
    for (i = 1; i < SPI_FRAME_LEN; i++) {
        if (f[i] == 0x00 || f[i] == 0xFF || f[i] == SPI_MARKER) {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *role = argc > 1 ? argv[1] : "";
    const char *dev = argc > 2 ? argv[2] : "/dev/spidev0.0";
    unsigned char frame[SPI_FRAME_LEN];
    const unsigned char *payload;
    int plen, fd, got = 0, i;
    unsigned char tx[64], rx[64], collected[512];
    time_t start;

    if (build_frame(frame) < 0) {
        printf("SPILINK:FAIL:frame contains NUL/0xFF/second-MARKER\n");
        return 1;
    }
    if (argc > 3 && argv[3][0]) {
        payload = (const unsigned char *)argv[3];    /* explicit override */
        plen = strlen(argv[3]);
    } else {
        /*
         * The default is the BUILT-IN frame, never a string handed down through
         * a shell. An empty argv[3] means "use the default" so a harness can
         * pass the slot through without inventing an escaping problem for it.
         */
        payload = frame;
        plen = SPI_FRAME_LEN;
    }

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("SPILINK:FAIL:open %s\n", dev);
        return 1;
    }
    if (setup(fd) < 0) {
        printf("SPILINK:FAIL:spidev setup\n");
        return 1;
    }

    if (!strcmp(role, "send")) {
        if (xfer(fd, payload, rx, plen) < 0) {
            printf("SPILINK:FAIL:send xfer errno=%d\n", errno);
            return 1;
        }
        /*
         * Print the bytes we actually put on the wire, as bytes. A payload
         * echoed back as text cannot show you that it was mangled - and a
         * mangled frame does not look like a mangled frame, it looks like a
         * peer that never PASSes.
         */
        printf("SPILINK:SENT %d bytes:", plen);
        for (i = 0; i < plen; i++) {
            printf(" %02x", payload[i]);
        }
        printf("\n");
        return 0;
    }

    /*
     * receive: clock dummy bytes, collect non-idle (non-0xff) payload bytes
     * into a window and search for the payload as a substring. The sender
     * resends a few copies, so the full payload lands contiguously at least
     * once even if the first (pre-connect) burst was partial - a substring
     * search is alignment-independent, unlike matching the first plen bytes.
     */
    /*
     * Clock 8 bytes/xfer. The RX-FIFO-overflow reason for small chunks is gone -
     * the i.MX95 fsl-lpspi runs the receive over eDMA and the eDMA now
     * interleaves tx-fill and rx-drain byte-by-byte (hw/dma/imx95_edma.c
     * edma_run_txrx), so a receive longer than the 16-deep FIFO no longer drops
     * bytes. Small chunks remain deliberate here for a different reason: this
     * oracle clocks continuously across the window, and every dummy word is
     * forwarded to the peer over the socket; a large chunk floods the peer's
     * 256-deep spi-link FIFO faster than it drains, back-pressures the socket and
     * blocks the clock. 8 bytes/xfer keeps the flow gentle.
     */
    memset(tx, 0, sizeof(tx));
    start = time(NULL);
    while (got < (int)sizeof(collected) && time(NULL) - start < 45) {
        int n = xfer(fd, tx, rx, 8);

        if (n < 0) {
            printf("SPILINK:FAIL:recv xfer errno=%d\n", errno);
            return 1;
        }
        for (i = 0; i < 8 && got < (int)sizeof(collected); i++) {
            if (rx[i] != 0xff) {
                collected[got++] = rx[i];
            }
        }
        /* found the payload contiguously anywhere in the window? */
        for (i = 0; i + plen <= got; i++) {
            if (!memcmp(collected + i, payload, plen)) {
                printf("SPILINK:PASS: %d bytes crossed the SPI link "
                       "byte-exact\n", plen);
                return 0;
            }
        }
        usleep(100000);
    }
    printf("SPILINK:FAIL: got %d/%d\n", got, plen);
    printf("SPILINK:want:");
    for (i = 0; i < plen; i++) printf(" %02x", (unsigned char)payload[i]);
    printf("\nSPILINK:have:");
    for (i = 0; i < got; i++) printf(" %02x", collected[i]);
    printf("\n");
    return 1;
}
