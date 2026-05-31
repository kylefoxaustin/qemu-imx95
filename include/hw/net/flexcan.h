/*
 * NXP/Freescale FlexCAN controller emulation
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A functional model of the FlexCAN IP found on i.MX (6UL/7/8M/93/95),
 * Layerscape and S32 SoCs. Implements the register, message-buffer and
 * interrupt behaviour the Linux flexcan driver exercises (freeze/halt/
 * soft-reset handshake, individual RX mailboxes, TX mailbox, IFLAG/IMASK),
 * connected to QEMU's CAN bus subsystem for real frame TX/RX. Not
 * cycle-accurate; bit-timing registers are accepted and ignored.
 */

#ifndef HW_NET_FLEXCAN_H
#define HW_NET_FLEXCAN_H

#include "hw/core/sysbus.h"
#include "net/can_emu.h"
#include "qom/object.h"

#define TYPE_FLEXCAN "flexcan"
OBJECT_DECLARE_SIMPLE_TYPE(FlexCanState, FLEXCAN)

/* Modelled register/RAM extent (the DT reg window is larger; rest is RAZ/WI) */
#define FLEXCAN_MEM_SIZE   0x10000
#define FLEXCAN_REG_SIZE   0x1000
#define FLEXCAN_NUM_REGS   (FLEXCAN_REG_SIZE / 4)

struct FlexCanState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    CanBusState *canbus;
    CanBusClientState bus_client;

    /* Flat 32-bit backing store for the whole register + mailbox-RAM area. */
    uint32_t regs[FLEXCAN_NUM_REGS];
};

#endif /* HW_NET_FLEXCAN_H */
