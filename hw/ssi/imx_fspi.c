/*
 * NXP i.MX FlexSPI (FSPI) controller ("nxp,imx8mm-fspi")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * FlexSPI is a LUT-driven serial-NOR controller. The spi-nxp-fspi Linux
 * driver builds a single LUT sequence per spi-mem operation (the last entry,
 * seqid = lut_num - 1), programs the address in IPCR0 and the data length in
 * IPCR1, then triggers the command by writing IPCMD[TRG]. It then either reads
 * the result out of the RX FIFO (FSPI_RFDR, IP path) or — for large reads —
 * memcpy()s it straight out of the AHB memory-mapped window. The old UNIMP
 * stub returned zeros, so the driver's JEDEC-ID/SFDP probe never matched a
 * flash and spi-nor failed.
 *
 * This models enough of the controller to make the real driver enumerate and
 * read a flash: it decodes the active LUT (opcode / address bytes / dummy /
 * read-or-write), and replays the sequence byte-for-byte onto a real SSI bus,
 * so an m25p80 attached to it answers. Both datapaths are supported:
 *
 *   - IP command path: IPCMD[TRG] runs the LUT, latches read data into the RX
 *     FIFO and raises FSPI_INTR[IPCMDDONE]; the driver's IRQ handler completes
 *     and drains FSPI_RFDR.
 *   - AHB read path: a read in the 0x2800_0000 window replays the prepared
 *     read LUT at that offset and returns the flash bytes.
 *
 * Status bits the driver polls are answered as "ready/idle/locked" so its
 * reset (MCR0[SWRST], self-clearing), DLL-lock (STS2) and arbiter-idle (STS0)
 * waits all converge. Only the registers the driver touches are modelled.
 *
 * Limitation: the LUT dummy operand is in cycles scaled by the data bus width;
 * the byte count replayed here (operand / 8) is exact for the 1-1-1 commands
 * used during enumeration. Large/octal reads use the AHB path, which the
 * driver prepares with the same LUT, so they replay consistently.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX_FSPI "imx.fspi"
OBJECT_DECLARE_SIMPLE_TYPE(ImxFspiState, IMX_FSPI)

/* Register map (subset the driver uses). */
#define FSPI_MCR0           0x00
#define FSPI_MCR0_SWRST     (1u << 0)
#define FSPI_MCR2           0x08
#define FSPI_AHBCR          0x0c
#define FSPI_INTEN          0x10
#define FSPI_INTR           0x14
#define FSPI_INTR_IPRXWA    (1u << 5)
#define FSPI_INTR_IPTXWE    (1u << 6)
#define FSPI_INTR_IPCMDDONE (1u << 0)
#define FSPI_LUTKEY         0x18
#define FSPI_LCKCR          0x1c
#define FSPI_FLSHA1CR0      0x60
#define FSPI_FLSHB2CR2      0x8c
#define FSPI_IPCR0          0xa0
#define FSPI_IPCR1          0xa4
#define FSPI_IPCR1_SEQID_SHIFT 16
#define FSPI_IPCMD          0xb0
#define FSPI_IPCMD_TRG      (1u << 0)
#define FSPI_IPRXFCR        0xb8
#define FSPI_IPRXFCR_CLR    (1u << 0)
#define FSPI_IPTXFCR        0xbc
#define FSPI_IPTXFCR_CLR    (1u << 0)
#define FSPI_DLLACR         0xc0
#define FSPI_DLLBCR         0xc4
#define FSPI_STS0           0xe0
#define FSPI_STS0_ARB_IDLE  (1u << 1)
#define FSPI_STS0_SEQ_IDLE  (1u << 0)
#define FSPI_STS1           0xe4
#define FSPI_STS2           0xe8
#define FSPI_STS2_AB_LOCK   0x00030003  /* (B)REFLOCK | (B)SLVLOCK | A... */
#define FSPI_IPRXFSTS       0xf0
#define FSPI_IPTXFSTS       0xf4
#define FSPI_RFDR           0x100
#define FSPI_RFDR_END       0x17c
#define FSPI_TFDR           0x180
#define FSPI_TFDR_END       0x1fc
#define FSPI_LUT_BASE       0x200
#define FSPI_LUT_END        0x3fc

/* LUT instruction opcodes (the ones we decode). */
#define LUT_STOP            0x00
#define LUT_CMD             0x01
#define LUT_ADDR            0x02
#define LUT_NXP_WRITE       0x08
#define LUT_NXP_READ        0x09
#define LUT_DUMMY           0x0c
#define LUT_CMD_DDR         0x21
#define LUT_ADDR_DDR        0x22
#define LUT_WRITE_DDR       0x28
#define LUT_READ_DDR        0x29
#define LUT_DUMMY_DDR       0x2c

/* Flash opcodes we care about directly. */
#define SPINOR_OP_READ      0x03    /* slow read, 3-byte address, no dummy */
#define SPINOR_OP_READ_4B   0x13    /* slow read, 4-byte address, no dummy */
#define SPINOR_OP_RDSFDP    0x5a

#define FSPI_LUT_NUM        32   /* imx8mm: 32 sequences of 4 words each */
#define FSPI_NUM_CS         4
#define FSPI_RXFIFO         512
#define FSPI_TXFIFO         1024

enum { FSPI_DIR_NONE, FSPI_DIR_IN, FSPI_DIR_OUT };

struct ImxFspiState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;          /* register window */
    MemoryRegion ahb;           /* memory-mapped flash read window */
    SSIBus *spi;
    qemu_irq irq;
    qemu_irq cs_lines[FSPI_NUM_CS];

    uint32_t mcr0, mcr2, ahbcr, inten, intr;
    uint32_t ipcr0, ipcr1;
    uint32_t flsh[12];          /* FLSHxCR0/1/2, stored only (no behaviour) */
    uint32_t lut[FSPI_LUT_NUM * 4];

    uint8_t rx_buf[FSPI_RXFIFO];
    uint32_t rx_len;
    uint32_t rx_rd;

    uint8_t tx_buf[FSPI_TXFIFO];
    uint32_t tx_wp;
};

/* A decoded LUT sequence. */
typedef struct {
    uint8_t opcode;
    uint8_t addr_bytes;
    uint8_t dummy_bytes;
    uint8_t dir;
} FspiSeq;

static void fspi_update_irq(ImxFspiState *s)
{
    qemu_set_irq(s->irq, !!(s->intr & s->inten & FSPI_INTR_IPCMDDONE));
}

/* Decode the active sequence (the driver always uses seqid lut_num - 1). */
static void fspi_decode_lut(ImxFspiState *s, FspiSeq *seq)
{
    unsigned base = (FSPI_LUT_NUM - 1) * 4;
    int i;

    seq->opcode = 0;
    seq->addr_bytes = 0;
    seq->dummy_bytes = 0;
    seq->dir = FSPI_DIR_NONE;

    for (i = 0; i < 8; i++) {
        uint32_t word = s->lut[base + i / 2];
        uint32_t slot = (i & 1) ? (word >> 16) : (word & 0xffff);
        uint32_t instr = (slot >> 10) & 0x3f;
        uint32_t oprnd = slot & 0xff;

        switch (instr) {
        case LUT_STOP:
            return;
        case LUT_CMD:
        case LUT_CMD_DDR:
            seq->opcode = oprnd;
            break;
        case LUT_ADDR:
        case LUT_ADDR_DDR:
            seq->addr_bytes = oprnd / 8;
            break;
        case LUT_DUMMY:
        case LUT_DUMMY_DDR:
            seq->dummy_bytes = oprnd / 8;
            break;
        case LUT_NXP_READ:
        case LUT_READ_DDR:
            seq->dir = FSPI_DIR_IN;
            break;
        case LUT_NXP_WRITE:
        case LUT_WRITE_DDR:
            seq->dir = FSPI_DIR_OUT;
            break;
        default:
            break;
        }
    }
}

/* Replay a decoded sequence onto the SSI bus; CS0 is the only flash here. */
static void fspi_xfer(ImxFspiState *s, const FspiSeq *seq, uint32_t addr,
                      uint8_t *in, const uint8_t *out, uint32_t nbytes)
{
    uint8_t opcode = seq->opcode;
    int dummy = seq->dummy_bytes;
    int i;

    /*
     * For a memory read, m25p80 counts the fast/multi-lane dummy in its own
     * (byte-oriented numonyx) terms, which don't match the LUT's cycle count;
     * it is also lane-agnostic at the byte level. So issue a plain READ/READ4
     * with no dummy — identical data, handled exactly by m25p80. SFDP (0x5a)
     * and the address-less status/ID reads keep their real opcode and dummy.
     */
    if (seq->dir == FSPI_DIR_IN && seq->addr_bytes >= 3 &&
        seq->opcode != SPINOR_OP_RDSFDP) {
        opcode = (seq->addr_bytes >= 4) ? SPINOR_OP_READ_4B : SPINOR_OP_READ;
        dummy = 0;
    }

    qemu_set_irq(s->cs_lines[0], 0);            /* assert (active low) */

    ssi_transfer(s->spi, opcode);
    for (i = seq->addr_bytes; i > 0; i--) {     /* address, MSB first */
        ssi_transfer(s->spi, (addr >> ((i - 1) * 8)) & 0xff);
    }
    for (i = 0; i < dummy; i++) {
        ssi_transfer(s->spi, 0);
    }
    for (i = 0; i < (int)nbytes; i++) {
        if (seq->dir == FSPI_DIR_OUT) {
            ssi_transfer(s->spi, out ? out[i] : 0);
        } else {
            uint8_t b = ssi_transfer(s->spi, 0);
            if (in) {
                in[i] = b;
            }
        }
    }

    qemu_set_irq(s->cs_lines[0], 1);            /* deassert */
}

/* IPCMD[TRG]: run the LUT via the IP command path. */
static void fspi_run_command(ImxFspiState *s)
{
    FspiSeq seq;
    uint32_t nbytes = s->ipcr1 & 0xffff;        /* IPCR1[IDATSZ] */

    fspi_decode_lut(s, &seq);

    if (seq.dir == FSPI_DIR_IN) {
        if (nbytes > sizeof(s->rx_buf)) {
            nbytes = sizeof(s->rx_buf);
        }
        fspi_xfer(s, &seq, s->ipcr0, s->rx_buf, NULL, nbytes);
        s->rx_len = nbytes;
        s->rx_rd = 0;
    } else if (seq.dir == FSPI_DIR_OUT) {
        if (nbytes > sizeof(s->tx_buf)) {
            nbytes = sizeof(s->tx_buf);
        }
        fspi_xfer(s, &seq, s->ipcr0, NULL, s->tx_buf, nbytes);
    } else {
        /* command-only (e.g. WREN, erase): opcode + address, no data */
        fspi_xfer(s, &seq, s->ipcr0, NULL, NULL, 0);
    }

    s->intr |= FSPI_INTR_IPCMDDONE;
    fspi_update_irq(s);
}

static uint64_t fspi_read(void *opaque, hwaddr addr, unsigned size)
{
    ImxFspiState *s = opaque;

    switch (addr) {
    case FSPI_MCR0:
        return s->mcr0;
    case FSPI_MCR2:
        return s->mcr2;
    case FSPI_AHBCR:
        return s->ahbcr;
    case FSPI_INTEN:
        return s->inten;
    case FSPI_INTR:
        /* TX always ready; RX available while the FIFO holds unread data. */
        return s->intr | FSPI_INTR_IPTXWE |
               (s->rx_rd < s->rx_len ? FSPI_INTR_IPRXWA : 0);
    case FSPI_STS0:
        return FSPI_STS0_ARB_IDLE | FSPI_STS0_SEQ_IDLE;
    case FSPI_STS1:
        return 0;
    case FSPI_STS2:
        return FSPI_STS2_AB_LOCK;       /* DLL always "locked" */
    case FSPI_IPRXFSTS:
        return (s->rx_len - s->rx_rd) / 8;
    case FSPI_IPTXFSTS:
        return 0;
    case FSPI_IPCR0:
        return s->ipcr0;
    case FSPI_IPCR1:
        return s->ipcr1;
    case FSPI_FLSHA1CR0 ... FSPI_FLSHB2CR2:
        return s->flsh[(addr - FSPI_FLSHA1CR0) / 4];
    case FSPI_RFDR ... FSPI_RFDR_END: {
        uint32_t off = s->rx_rd + (addr - FSPI_RFDR);
        uint32_t v = 0;
        int i;
        for (i = 0; i < 4; i++) {
            if (off + i < s->rx_len) {
                v |= (uint32_t)s->rx_buf[off + i] << (8 * i);
            }
        }
        return v;
    }
    case FSPI_LUT_BASE ... FSPI_LUT_END:
        return s->lut[(addr - FSPI_LUT_BASE) / 4];
    default:
        return 0;
    }
}

static void fspi_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ImxFspiState *s = opaque;

    switch (addr) {
    case FSPI_MCR0:
        /* SWRST is write-1-self-clearing: never latch it. */
        s->mcr0 = val & ~FSPI_MCR0_SWRST;
        break;
    case FSPI_MCR2:
        s->mcr2 = val;
        break;
    case FSPI_AHBCR:
        s->ahbcr = val;
        break;
    case FSPI_INTEN:
        s->inten = val;
        fspi_update_irq(s);
        break;
    case FSPI_INTR:
        /* IPRXWA write advances the RX FIFO read pointer by the watermark. */
        if (val & FSPI_INTR_IPRXWA) {
            s->rx_rd += 8;
        }
        s->intr &= ~(uint32_t)val;      /* W1C */
        fspi_update_irq(s);
        break;
    case FSPI_LUTKEY:
    case FSPI_LCKCR:
    case FSPI_DLLACR:
    case FSPI_DLLBCR:
        break;                          /* key/lock/DLL: accepted, no effect */
    case FSPI_FLSHA1CR0 ... FSPI_FLSHB2CR2:
        s->flsh[(addr - FSPI_FLSHA1CR0) / 4] = val;
        break;
    case FSPI_IPCR0:
        s->ipcr0 = val;
        break;
    case FSPI_IPCR1:
        s->ipcr1 = val;
        break;
    case FSPI_IPCMD:
        if (val & FSPI_IPCMD_TRG) {
            fspi_run_command(s);
        }
        break;
    case FSPI_IPRXFCR:
        if (val & FSPI_IPRXFCR_CLR) {
            s->rx_rd = s->rx_len = 0;
        }
        break;
    case FSPI_IPTXFCR:
        if (val & FSPI_IPTXFCR_CLR) {
            s->tx_wp = 0;
        }
        break;
    case FSPI_TFDR ... FSPI_TFDR_END: {
        /* Bytes accumulate at the committed cursor; IPTXWE advances it. */
        uint32_t off = s->tx_wp + (addr - FSPI_TFDR);
        int i;
        for (i = 0; i < 4 && off + i < sizeof(s->tx_buf); i++) {
            s->tx_buf[off + i] = (val >> (8 * i)) & 0xff;
        }
        break;
    }
    case FSPI_LUT_BASE ... FSPI_LUT_END:
        s->lut[(addr - FSPI_LUT_BASE) / 4] = val;
        break;
    default:
        break;
    }

    /*
     * The TX watermark write (INTR[IPTXWE]) lands in the INTR case above and
     * also marks an 8-byte chunk committed; do that here so it sees the new
     * cursor regardless of write order.
     */
    if (addr == FSPI_INTR && (val & FSPI_INTR_IPTXWE)) {
        s->tx_wp += 8;
    }
}

static const MemoryRegionOps fspi_ops = {
    .read = fspi_read,
    .write = fspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * AHB memory-mapped read: replay the prepared read LUT at this flash offset.
 * The driver always programs the read LUT (via prepare_lut) before taking the
 * AHB path, so the active sequence describes exactly this read.
 */
static uint64_t fspi_ahb_read(void *opaque, hwaddr offset, unsigned size)
{
    ImxFspiState *s = opaque;
    FspiSeq seq;
    uint8_t buf[8];
    uint64_t v = 0;
    int i;

    fspi_decode_lut(s, &seq);
    if (!seq.opcode || seq.dir != FSPI_DIR_IN) {
        return 0;
    }

    fspi_xfer(s, &seq, offset, buf, NULL, size);
    for (i = 0; i < (int)size; i++) {
        v |= (uint64_t)buf[i] << (8 * i);
    }
    return v;
}

static void fspi_ahb_write(void *opaque, hwaddr offset, uint64_t val,
                           unsigned size)
{
    /* Read-only in the driver's use; writes go via the IP command path. */
}

static const MemoryRegionOps fspi_ahb_ops = {
    .read = fspi_ahb_read,
    .write = fspi_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};

static void fspi_reset(DeviceState *dev)
{
    ImxFspiState *s = IMX_FSPI(dev);

    s->mcr0 = 0;
    s->mcr2 = 0;
    s->ahbcr = 0;
    s->inten = 0;
    s->intr = 0;
    s->ipcr0 = 0;
    s->ipcr1 = 0;
    s->rx_len = s->rx_rd = 0;
    s->tx_wp = 0;
    memset(s->flsh, 0, sizeof(s->flsh));
    memset(s->lut, 0, sizeof(s->lut));
    qemu_set_irq(s->cs_lines[0], 1);    /* flash deselected */
    fspi_update_irq(s);
}

static void fspi_realize(DeviceState *dev, Error **errp)
{
    ImxFspiState *s = IMX_FSPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &fspi_ops, s,
                          "imx.fspi", 0x10000);
    sysbus_init_mmio(sbd, &s->mmio);

    memory_region_init_io(&s->ahb, OBJECT(s), &fspi_ahb_ops, s,
                          "imx.fspi-ahb", 0x8000000);
    sysbus_init_mmio(sbd, &s->ahb);

    sysbus_init_irq(sbd, &s->irq);

    s->spi = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, s->cs_lines, "cs", FSPI_NUM_CS);
}

static const VMStateDescription vmstate_fspi = {
    .name = "imx.fspi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr0, ImxFspiState),
        VMSTATE_UINT32(mcr2, ImxFspiState),
        VMSTATE_UINT32(ahbcr, ImxFspiState),
        VMSTATE_UINT32(inten, ImxFspiState),
        VMSTATE_UINT32(intr, ImxFspiState),
        VMSTATE_UINT32(ipcr0, ImxFspiState),
        VMSTATE_UINT32(ipcr1, ImxFspiState),
        VMSTATE_UINT32_ARRAY(flsh, ImxFspiState, 12),
        VMSTATE_UINT32_ARRAY(lut, ImxFspiState, FSPI_LUT_NUM * 4),
        VMSTATE_UINT8_ARRAY(rx_buf, ImxFspiState, FSPI_RXFIFO),
        VMSTATE_UINT32(rx_len, ImxFspiState),
        VMSTATE_UINT32(rx_rd, ImxFspiState),
        VMSTATE_UINT8_ARRAY(tx_buf, ImxFspiState, FSPI_TXFIFO),
        VMSTATE_UINT32(tx_wp, ImxFspiState),
        VMSTATE_END_OF_LIST()
    },
};

static void fspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = fspi_realize;
    device_class_set_legacy_reset(dc, fspi_reset);
    dc->vmsd = &vmstate_fspi;
    dc->desc = "NXP i.MX FlexSPI controller";
}

static const TypeInfo fspi_types[] = {
    {
        .name = TYPE_IMX_FSPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ImxFspiState),
        .class_init = fspi_class_init,
    },
};

DEFINE_TYPES(fspi_types)
