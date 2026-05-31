/*
 * NXP/Freescale FlexCAN controller emulation
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional model of the FlexCAN IP (i.MX 6UL/7/8M/93/95, Layerscape,
 * S32). It models what the Linux flexcan driver drives: the MCR
 * freeze/halt/soft-reset handshake, individual RX mailboxes + a TX mailbox
 * in the message-buffer RAM, the IFLAG/IMASK interrupt pair, and CAN-FD
 * geometry. Frames are carried over QEMU's CAN bus subsystem, so a guest
 * can do real `cansend`/`candump` against a host SocketCAN backend or
 * another emulated controller on the same bus. Bit-timing registers
 * (CTRL/CBT/FDCBT/FDCTRL) are accepted and ignored; this is not
 * cycle- or timing-accurate.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/net/flexcan.h"

/* Register byte offsets (subset the driver touches; see flexcan.h spec). */
#define FLEXCAN_MCR        0x00
#define FLEXCAN_CTRL1      0x04
#define FLEXCAN_TIMER      0x08
#define FLEXCAN_ECR        0x1c
#define FLEXCAN_ESR1       0x20
#define FLEXCAN_IMASK2     0x24
#define FLEXCAN_IMASK1     0x28
#define FLEXCAN_IFLAG2     0x2c
#define FLEXCAN_IFLAG1     0x30
#define FLEXCAN_CTRL2      0x34
#define FLEXCAN_MB_BASE    0x80
#define FLEXCAN_MB_BANK1   0x280

/* MCR bits */
#define MCR_MDIS      (1u << 31)
#define MCR_FRZ       (1u << 30)
#define MCR_HALT      (1u << 28)
#define MCR_NOT_RDY   (1u << 27)
#define MCR_SOFTRST   (1u << 25)
#define MCR_FRZ_ACK   (1u << 24)
#define MCR_LPM_ACK   (1u << 20)
#define MCR_FDEN      (1u << 11)
#define MCR_MAXMB     0x7f

/* Message-buffer control/status (CS) word */
#define MB_CODE_MASK    (0xfu << 24)
#define MB_CODE_RX_INACTIVE (0x0u << 24)
#define MB_CODE_RX_EMPTY    (0x4u << 24)
#define MB_CODE_RX_FULL     (0x2u << 24)
#define MB_CODE_RX_BUSY     (0x1u << 24)
#define MB_CODE_TX_INACTIVE (0x8u << 24)
#define MB_CODE_TX_DATA     (0xcu << 24)
#define MB_EDL    (1u << 31)
#define MB_BRS    (1u << 30)
#define MB_ESI    (1u << 29)
#define MB_SRR    (1u << 22)
#define MB_IDE    (1u << 21)
#define MB_RTR    (1u << 20)
#define MB_DLC(x) (((x) >> 16) & 0xf)

/* CAN-FD DLC <-> byte-length table (DLC 9..15 map to 12..64). */
static const uint8_t flexcan_fd_len[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64,
};

static uint8_t flexcan_dlc2len(uint8_t dlc, bool fd)
{
    if (!fd) {
        return dlc > 8 ? 8 : dlc;
    }
    return flexcan_fd_len[dlc & 0xf];
}

static uint8_t flexcan_len2dlc(uint8_t len, bool fd)
{
    int i;

    if (!fd || len <= 8) {
        return len > 8 ? 8 : len;
    }
    for (i = 9; i < 16; i++) {
        if (flexcan_fd_len[i] >= len) {
            return i;
        }
    }
    return 15;
}

/* Mailbox geometry depends on whether CAN-FD is enabled (64B vs 8B MBs). */
static bool flexcan_fd_enabled(FlexCanState *s)
{
    return s->regs[FLEXCAN_MCR / 4] & MCR_FDEN;
}

static unsigned flexcan_mb_size(FlexCanState *s)
{
    return flexcan_fd_enabled(s) ? 72 : 16;   /* 8 + 8 (CS,ID) ; 64 + 8 */
}

static unsigned flexcan_bank_size(FlexCanState *s)
{
    return 512 / flexcan_mb_size(s);          /* 32 classic, 7 FD */
}

static unsigned flexcan_mb_count(FlexCanState *s)
{
    return 2 * flexcan_bank_size(s);          /* 64 classic, 14 FD */
}

/* Byte offset of message buffer @idx (two banks: 0x80 and 0x280). */
static hwaddr flexcan_mb_off(FlexCanState *s, unsigned idx)
{
    unsigned bank = flexcan_bank_size(s);
    unsigned sz = flexcan_mb_size(s);

    if (idx < bank) {
        return FLEXCAN_MB_BASE + idx * sz;
    }
    return FLEXCAN_MB_BANK1 + (idx - bank) * sz;
}

static void flexcan_update_irq(FlexCanState *s)
{
    bool pending =
        (s->regs[FLEXCAN_IFLAG1 / 4] & s->regs[FLEXCAN_IMASK1 / 4]) ||
        (s->regs[FLEXCAN_IFLAG2 / 4] & s->regs[FLEXCAN_IMASK2 / 4]);

    qemu_set_irq(s->irq, pending);
}

static void flexcan_set_iflag(FlexCanState *s, unsigned idx)
{
    if (idx < 32) {
        s->regs[FLEXCAN_IFLAG1 / 4] |= (1u << idx);
    } else {
        s->regs[FLEXCAN_IFLAG2 / 4] |= (1u << (idx - 32));
    }
    flexcan_update_irq(s);
}

/* The driver only re-reads an RX MB; the model owns re-arming it to EMPTY. */
static void flexcan_rearm_rx(FlexCanState *s, uint32_t cleared, unsigned base)
{
    unsigned mb_last = flexcan_mb_count(s) - 2;
    int b;

    for (b = 0; b < 32; b++) {
        unsigned idx = base + b;

        if (!(cleared & (1u << b))) {
            continue;
        }
        if (idx >= 1 && idx <= mb_last) {
            uint32_t *mb = &s->regs[flexcan_mb_off(s, idx) / 4];

            mb[0] = (mb[0] & ~MB_CODE_MASK) | MB_CODE_RX_EMPTY;
        }
    }
}

/* TX mailbox CS write with CODE=TX_DATA: assemble a frame and send it. */
static void flexcan_do_tx(FlexCanState *s)
{
    bool fd = flexcan_fd_enabled(s);
    uint32_t *mb = &s->regs[flexcan_mb_off(s, flexcan_mb_count(s) - 1) / 4];
    uint32_t cs = mb[0], id = mb[1];
    bool edl = fd && (cs & MB_EDL);
    qemu_can_frame f = { 0 };
    uint8_t len, i;

    len = flexcan_dlc2len(MB_DLC(cs), edl);

    if (cs & MB_IDE) {
        f.can_id = (id & QEMU_CAN_EFF_MASK) | QEMU_CAN_EFF_FLAG;
    } else {
        f.can_id = (id >> 18) & QEMU_CAN_SFF_MASK;
    }
    if (cs & MB_RTR) {
        f.can_id |= QEMU_CAN_RTR_FLAG;
    }
    f.can_dlc = len;
    if (edl) {
        f.flags |= QEMU_CAN_FRMF_TYPE_FD;
        if (cs & MB_BRS) {
            f.flags |= QEMU_CAN_FRMF_BRS;
        }
        if (cs & MB_ESI) {
            f.flags |= QEMU_CAN_FRMF_ESI;
        }
    }
    for (i = 0; i < len && i < sizeof(f.data); i++) {
        f.data[i] = (mb[2 + i / 4] >> (24 - 8 * (i % 4))) & 0xff;
    }

    if (s->canbus) {
        can_bus_client_send(&s->bus_client, &f, 1);
    }

    /* TX-complete: latch the TX mailbox's IFLAG bit. */
    flexcan_set_iflag(s, flexcan_mb_count(s) - 1);
}

static bool flexcan_can_receive(CanBusClientState *client)
{
    return true;
}

static ssize_t flexcan_receive(CanBusClientState *client,
                               const qemu_can_frame *frames, size_t frames_cnt)
{
    FlexCanState *s = container_of(client, FlexCanState, bus_client);
    const qemu_can_frame *f = frames;
    unsigned mb_last = flexcan_mb_count(s) - 2;
    bool eff, rtr, fd;
    unsigned idx;
    uint8_t len, dlc, i;

    if (!frames_cnt) {
        return 0;
    }
    /* Drop error frames; this model has no error-state path. */
    if (f->can_id & QEMU_CAN_ERR_FLAG) {
        return 1;
    }

    eff = f->can_id & QEMU_CAN_EFF_FLAG;
    rtr = f->can_id & QEMU_CAN_RTR_FLAG;
    fd = f->flags & QEMU_CAN_FRMF_TYPE_FD;
    len = f->can_dlc;
    dlc = flexcan_len2dlc(len, fd);

    /*
     * Mailbox mode: deliver into the first EMPTY RX mailbox (RXIMR=0 => any
     * ID matches; the guest's CAN stack filters in software).
     */
    for (idx = 1; idx <= mb_last; idx++) {
        uint32_t *mb = &s->regs[flexcan_mb_off(s, idx) / 4];
        uint32_t cs;
        unsigned words = fd ? 16 : 2, w;

        if ((mb[0] & MB_CODE_MASK) != MB_CODE_RX_EMPTY) {
            continue;
        }

        cs = MB_CODE_RX_FULL | (dlc << 16);
        if (eff) {
            cs |= MB_IDE | MB_SRR;
        }
        if (rtr) {
            cs |= MB_RTR;
        }
        if (fd) {
            cs |= MB_EDL;
            if (f->flags & QEMU_CAN_FRMF_BRS) {
                cs |= MB_BRS;
            }
            if (f->flags & QEMU_CAN_FRMF_ESI) {
                cs |= MB_ESI;
            }
        }

        mb[1] = eff ? (f->can_id & QEMU_CAN_EFF_MASK)
                    : ((f->can_id & QEMU_CAN_SFF_MASK) << 18);
        for (w = 0; w < words; w++) {
            mb[2 + w] = 0;
        }
        for (i = 0; i < len && i < sizeof(f->data); i++) {
            mb[2 + i / 4] |= (uint32_t)f->data[i] << (24 - 8 * (i % 4));
        }
        mb[0] = cs;   /* write CODE last */

        flexcan_set_iflag(s, idx);
        return 1;
    }

    /* No free mailbox: drop (a real device would flag overrun). */
    return 1;
}

static CanBusClientInfo flexcan_bus_client_info = {
    .can_receive = flexcan_can_receive,
    .receive = flexcan_receive,
};

/*
 * Soft reset: clear the interrupt/flag state the driver re-establishes next;
 * leave control + bit-timing + mailbox RAM (the "unaffected" set) intact.
 */
static void flexcan_soft_reset(FlexCanState *s)
{
    s->regs[FLEXCAN_IMASK1 / 4] = 0;
    s->regs[FLEXCAN_IMASK2 / 4] = 0;
    s->regs[FLEXCAN_IFLAG1 / 4] = 0;
    s->regs[FLEXCAN_IFLAG2 / 4] = 0;
    s->regs[FLEXCAN_ESR1 / 4] = 0;
}

static uint64_t flexcan_read(void *opaque, hwaddr offset, unsigned size)
{
    FlexCanState *s = opaque;

    if (offset >= FLEXCAN_REG_SIZE) {
        return 0;   /* RAZ above the modelled window */
    }

    switch (offset) {
    case FLEXCAN_TIMER:
        /* Free-running-ish: reading unlocks an MB and yields a timestamp. */
        return s->regs[FLEXCAN_TIMER / 4]++ & 0xffff;
    case FLEXCAN_ECR:
    case FLEXCAN_ESR1:
        return 0;   /* error-active, no counters/faults modelled */
    default:
        return s->regs[offset / 4];
    }
}

static void flexcan_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    FlexCanState *s = opaque;
    uint32_t v = value;

    if (offset >= FLEXCAN_REG_SIZE) {
        return;   /* WI above the modelled window */
    }

    switch (offset) {
    case FLEXCAN_MCR:
        if (v & MCR_SOFTRST) {
            flexcan_soft_reset(s);
            v &= ~MCR_SOFTRST;   /* self-clearing */
        }
        /* Drive the handshake ack bits the driver polls on. */
        v &= ~(MCR_FRZ_ACK | MCR_LPM_ACK | MCR_NOT_RDY);
        if (v & MCR_MDIS) {
            v |= MCR_LPM_ACK;
        }
        if ((v & MCR_FRZ) && (v & MCR_HALT)) {
            v |= MCR_FRZ_ACK;
        }
        if ((v & MCR_MDIS) || (v & MCR_FRZ_ACK)) {
            v |= MCR_NOT_RDY;
        }
        s->regs[FLEXCAN_MCR / 4] = v;
        return;

    case FLEXCAN_IFLAG1: {
        uint32_t cleared = s->regs[FLEXCAN_IFLAG1 / 4] & v;
        s->regs[FLEXCAN_IFLAG1 / 4] &= ~v;       /* W1C */
        flexcan_rearm_rx(s, cleared, 0);
        flexcan_update_irq(s);
        return;
    }
    case FLEXCAN_IFLAG2: {
        uint32_t cleared = s->regs[FLEXCAN_IFLAG2 / 4] & v;
        s->regs[FLEXCAN_IFLAG2 / 4] &= ~v;       /* W1C */
        flexcan_rearm_rx(s, cleared, 32);
        flexcan_update_irq(s);
        return;
    }
    case FLEXCAN_IMASK1:
    case FLEXCAN_IMASK2:
        s->regs[offset / 4] = v;
        flexcan_update_irq(s);
        return;

    case FLEXCAN_ESR1:
        s->regs[FLEXCAN_ESR1 / 4] &= ~v;         /* W1C (stays 0 here) */
        return;

    case FLEXCAN_TIMER:
    case FLEXCAN_ECR:
        return;   /* read-only */

    default:
        s->regs[offset / 4] = v;
        /* A CS write to the TX mailbox with CODE=TX_DATA triggers transmit. */
        if (offset == flexcan_mb_off(s, flexcan_mb_count(s) - 1) &&
            (v & MB_CODE_MASK) == MB_CODE_TX_DATA) {
            flexcan_do_tx(s);
        }
        return;
    }
}

static const MemoryRegionOps flexcan_ops = {
    .read = flexcan_read,
    .write = flexcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Registers are 32-bit (impl = 4), but the Linux driver's
     * flexcan_ram_init() clears the mailbox RAM with memset_io(), which on
     * arm64 issues 8-byte writeq stores; allow 1..8-byte accesses and let
     * QEMU split them into 4-byte device ops.
     */
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void flexcan_reset_hold(Object *obj, ResetType type)
{
    FlexCanState *s = FLEXCAN(obj);

    memset(s->regs, 0, sizeof(s->regs));
    /* Post-reset: disabled + frozen, acks asserted, max mailboxes. */
    s->regs[FLEXCAN_MCR / 4] = MCR_MDIS | MCR_FRZ | MCR_HALT | MCR_NOT_RDY |
                               MCR_FRZ_ACK | MCR_LPM_ACK | MCR_MAXMB;
    flexcan_update_irq(s);
}

static void flexcan_realize(DeviceState *dev, Error **errp)
{
    FlexCanState *s = FLEXCAN(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &flexcan_ops, s,
                          TYPE_FLEXCAN, FLEXCAN_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    if (s->canbus) {
        s->bus_client.info = &flexcan_bus_client_info;
        s->bus_client.fd_mode = true;   /* FlexCAN supports CAN-FD */
        if (can_bus_insert_client(s->canbus, &s->bus_client) < 0) {
            error_setg(errp, "flexcan: failed to connect to CAN bus");
        }
    }
}

static const VMStateDescription vmstate_flexcan = {
    .name = TYPE_FLEXCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, FlexCanState, FLEXCAN_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static const Property flexcan_properties[] = {
    DEFINE_PROP_LINK("canbus", FlexCanState, canbus, TYPE_CAN_BUS,
                     CanBusState *),
};

static void flexcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = flexcan_realize;
    dc->vmsd = &vmstate_flexcan;
    rc->phases.hold = flexcan_reset_hold;
    device_class_set_props(dc, flexcan_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->desc = "NXP FlexCAN controller";
}

static const TypeInfo flexcan_info = {
    .name = TYPE_FLEXCAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FlexCanState),
    .class_init = flexcan_class_init,
};

static void flexcan_register_types(void)
{
    type_register_static(&flexcan_info);
}

type_init(flexcan_register_types)
