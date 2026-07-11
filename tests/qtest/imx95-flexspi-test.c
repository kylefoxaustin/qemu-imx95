/*
 * QTest for the i.MX 95 FlexSPI controller + serial NOR (hw/ssi/imx_fspi.c).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Proves the FlexSPI-backed NOR is HONEST flash - a real m25p80 on the SSI bus,
 * with true NOR physics - and NOT memory_region_init_ram() masquerading as
 * flash at a flash address (mcxn947's fleet-wide bug class; the 95 is immune by
 * construction - init_io AHB window + a real m25p80 - and this test turns that
 * CLAIM into a PROOF).
 *
 * The canonical assertion is rt1180emulator's: a plain erase -> program ->
 * read-back round trip does NOT distinguish honest NOR from RAM-with-a-flash-
 * address (both read back what you wrote). Only a PROGRAM WITHOUT ERASE does:
 * real NOR can only clear bits (1 -> 0), so programming B over A yields A & B,
 * whereas RAM would simply hold B. This test asserts A & B (and != B).
 *
 * It drives the controller's IP command path (LUT -> IPCMD[TRG] -> RX/TX FIFO)
 * exactly as the spi-nxp-fspi driver does. The model always decodes LUT
 * sequence 31, so each opcode is written into LUT[31] before it is triggered.
 */
#include "qemu/osdep.h"
#include "libqtest-single.h"

#define FSPI        0x425e0000ULL

#define FSPI_INTR       0x14
#define FSPI_LUTKEY     0x18
#define FSPI_LCKCR      0x1c
#define FSPI_IPCR0      0xa0
#define FSPI_IPCR1      0xa4
#define FSPI_IPCMD      0xb0
#define FSPI_IPRXFCR    0xb8
#define FSPI_IPTXFCR    0xbc
#define FSPI_RFDR       0x100
#define FSPI_TFDR       0x180
#define FSPI_LUT31      (0x200 + 31 * 16)  /* the sequence the model decodes */

#define INTR_IPCMDDONE  (1u << 0)
#define IPCMD_TRG       (1u << 0)
#define FIFO_CLR        (1u << 0)
#define SEQID31         (31u << 16)

/* LUT: slot = (instruction << 10) | operand; two slots per 32-bit word. */
#define LI_CMD   0x01
#define LI_ADDR  0x02
#define LI_WRITE 0x08
#define LI_READ  0x09
#define SLOT(i, op)   (((unsigned)(i) << 10) | (unsigned)(op))
#define WORD(s0, s1)  ((uint32_t)(s0) | ((uint32_t)(s1) << 16))

/* NOR opcodes. */
#define OP_WREN  0x06
#define OP_RDID  0x9f
#define OP_READ  0x03
#define OP_PP    0x02
#define OP_SE4K  0x20

static void set_lut(QTestState *qts, uint32_t w0, uint32_t w1)
{
    qtest_writel(qts, FSPI + FSPI_LUTKEY, 0x5af05af0);
    qtest_writel(qts, FSPI + FSPI_LCKCR, 0x02);        /* unlock */
    qtest_writel(qts, FSPI + FSPI_LUT31 + 0, w0);
    qtest_writel(qts, FSPI + FSPI_LUT31 + 4, w1);
    qtest_writel(qts, FSPI + FSPI_LUT31 + 8, 0);
    qtest_writel(qts, FSPI + FSPI_LUT31 + 12, 0);
    qtest_writel(qts, FSPI + FSPI_LUTKEY, 0x5af05af0);
    qtest_writel(qts, FSPI + FSPI_LCKCR, 0x01);        /* lock */
}

static void trigger(QTestState *qts, uint32_t addr, uint32_t nbytes)
{
    qtest_writel(qts, FSPI + FSPI_IPCR0, addr);
    qtest_writel(qts, FSPI + FSPI_IPCR1, SEQID31 | nbytes);
    qtest_writel(qts, FSPI + FSPI_IPCMD, IPCMD_TRG);
    /* The model completes synchronously and flags IPCMDDONE. */
    g_assert_cmphex(qtest_readl(qts, FSPI + FSPI_INTR) & INTR_IPCMDDONE, ==,
                    INTR_IPCMDDONE);
}

/* A command-only opcode (WREN, erase): opcode [+ 24-bit address], no data. */
static void cmd_only(QTestState *qts, uint8_t op, int has_addr, uint32_t addr)
{
    uint32_t w0 = has_addr
        ? WORD(SLOT(LI_CMD, op), SLOT(LI_ADDR, 24))
        : WORD(SLOT(LI_CMD, op), 0);
    set_lut(qts, w0, 0);
    trigger(qts, addr, 0);
}

static void nor_wren(QTestState *qts)
{
    cmd_only(qts, OP_WREN, 0, 0);
}

static void nor_read(QTestState *qts, uint32_t addr, uint8_t *out, uint32_t n)
{
    uint32_t i;

    qtest_writel(qts, FSPI + FSPI_IPRXFCR, FIFO_CLR);   /* rx cursor -> 0 */
    set_lut(qts, WORD(SLOT(LI_CMD, OP_READ), SLOT(LI_ADDR, 24)),
            WORD(SLOT(LI_READ, 0), 0));
    trigger(qts, addr, n);
    for (i = 0; i < n; i += 4) {
        uint32_t w = qtest_readl(qts, FSPI + FSPI_RFDR + i);
        int j;

        for (j = 0; j < 4 && i + j < n; j++) {
            out[i + j] = (w >> (8 * j)) & 0xff;
        }
    }
}

static void nor_program(QTestState *qts, uint32_t addr,
                        const uint8_t *data, uint32_t n)
{
    uint32_t i;

    nor_wren(qts);
    qtest_writel(qts, FSPI + FSPI_IPTXFCR, FIFO_CLR);   /* tx cursor -> 0 */
    for (i = 0; i < n; i += 4) {
        uint32_t w = 0;
        int j;

        for (j = 0; j < 4 && i + j < n; j++) {
            w |= (uint32_t)data[i + j] << (8 * j);
        }
        qtest_writel(qts, FSPI + FSPI_TFDR + i, w);
    }
    set_lut(qts, WORD(SLOT(LI_CMD, OP_PP), SLOT(LI_ADDR, 24)),
            WORD(SLOT(LI_WRITE, 0), 0));
    trigger(qts, addr, n);
}

static void nor_erase4k(QTestState *qts, uint32_t addr)
{
    nor_wren(qts);
    cmd_only(qts, OP_SE4K, 1, addr);
}

static void test_flexspi_nor(void)
{
    QTestState *qts = qtest_init("-machine imx95-19x19-evk -accel qtest");
    const uint8_t A[8] = { 0xF0, 0x0F, 0xAA, 0x55, 0xFF, 0x00, 0xC3, 0x3C };
    const uint8_t B[8] = { 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F };
    uint8_t rid[4] = {0}, buf[8];
    int i;

    /*
     * 1. JEDEC ID (RDID 0x9F) - a real m25p80 answers its manufacturer/device
     *    id; a RAM window would read zeros. mt25ql512ab = 0x20 0xBA 0x20.
     */
    qtest_writel(qts, FSPI + FSPI_IPRXFCR, FIFO_CLR);
    set_lut(qts, WORD(SLOT(LI_CMD, OP_RDID), SLOT(LI_READ, 0)), 0);
    trigger(qts, 0, 3);
    {
        uint32_t w = qtest_readl(qts, FSPI + FSPI_RFDR);

        rid[0] = w & 0xff; rid[1] = (w >> 8) & 0xff; rid[2] = (w >> 16) & 0xff;
    }
    g_assert_cmphex(rid[0], ==, 0x20);
    g_assert_cmphex(rid[1], ==, 0xba);
    g_assert_cmphex(rid[2], ==, 0x20);

    /* 2. Erase the 4 KiB sector at 0, then confirm it reads all-ones. */
    nor_erase4k(qts, 0);
    nor_read(qts, 0, buf, 8);
    for (i = 0; i < 8; i++) {
        g_assert_cmphex(buf[i], ==, 0xff);
    }

    /* 3. Program pattern A; read it back byte-exact. */
    nor_program(qts, 0, A, 8);
    nor_read(qts, 0, buf, 8);
    for (i = 0; i < 8; i++) {
        g_assert_cmphex(buf[i], ==, A[i]);
    }

    /*
     * 4. THE ASSERTION THAT CATCHES init_ram: program B over A WITHOUT erasing.
     *    Real NOR only clears bits, so the result must be A & B - never B. A
     *    RAM-backed window would just hold B.
     */
    nor_program(qts, 0, B, 8);
    nor_read(qts, 0, buf, 8);
    for (i = 0; i < 8; i++) {
        g_assert_cmphex(buf[i], ==, (uint8_t)(A[i] & B[i]));
    }
    /* And prove it is genuinely different from a plain overwrite (RAM). */
    g_assert_cmphex(buf[0], !=, B[0]);   /* A&B = 0x00, B = 0x0F */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx95/flexspi/nor-honest", test_flexspi_nor);
    return g_test_run();
}
