/*
 * Minimal SCMI server stub for the qemu-imx95 emulator
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Stands in for the M33 System Manager firmware. Answers SCMI
 * messages that U-Boot SPL exchanges over MU2 / sram0 (the SMT
 * shared-memory transport per ARM DEN0056). v0.1 implements only
 * the base, clock, and pinctrl protocols at PROTOCOL_VERSION /
 * PROTOCOL_ATTRIBUTES / a small handful of message-attribute
 * queries - just enough to get U-Boot SPL past imx9_probe_mu()
 * and into preloader_console_init(). All other messages return
 * SCMI_NOT_SUPPORTED and log to LOG_GUEST_ERROR so option-C
 * iteration can extend coverage based on observed behaviour.
 */

#ifndef IMX95_SCMI_SERVER_H
#define IMX95_SCMI_SERVER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/misc/imx_mu.h"

#define TYPE_IMX95_SCMI_SERVER "imx95.scmi-server"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95SCMIServerState, IMX95_SCMI_SERVER)

/* Protocol IDs (per imx95.dtsi:378-420 and ARM DEN0056). */
#define SCMI_PROTOCOL_BASE              0x10
#define SCMI_PROTOCOL_POWER_DOMAIN      0x11
#define SCMI_PROTOCOL_SYSTEM_POWER      0x12
#define SCMI_PROTOCOL_PERFORMANCE       0x13
#define SCMI_PROTOCOL_CLOCK             0x14
#define SCMI_PROTOCOL_SENSOR            0x15
#define SCMI_PROTOCOL_RESET             0x16
#define SCMI_PROTOCOL_VOLTAGE           0x17
#define SCMI_PROTOCOL_PINCTRL           0x19

/*
 * NXP-vendor SCMI protocols (per references/uboot-imx/include/
 * scmi_protocols.h:31). Linux device-tree exposes the ones at
 * dtsi:406-420 (lmm 0x80, bbm 0x81, cpu 0x82, misc 0x84). v0.2 adds
 * misc because U-Boot SPL queries ROM_PASSOVER_GET on it before
 * trying to load the next boot stage.
 */
#define SCMI_PROTOCOL_IMX_LMM           0x80
#define SCMI_PROTOCOL_IMX_BBM           0x81
#define SCMI_PROTOCOL_IMX_CPU           0x82
#define SCMI_PROTOCOL_IMX_MISC          0x84

/* SCMI imx-misc message IDs. */
#define SCMI_MSG_IMX_MISC_RESET_REASON      0x0A
#define SCMI_MSG_IMX_MISC_ROM_PASSOVER_GET  0x07

/* Return status codes. */
#define SCMI_SUCCESS                     0
#define SCMI_NOT_SUPPORTED              -1
#define SCMI_INVALID_PARAMETERS         -2

/* Messages shared across all protocols. */
#define SCMI_MSG_PROTOCOL_VERSION                   0x0
#define SCMI_MSG_PROTOCOL_ATTRIBUTES                0x1
#define SCMI_MSG_PROTOCOL_MESSAGE_ATTRIBUTES        0x2

/* Base protocol message IDs. */
#define SCMI_MSG_BASE_DISCOVER_VENDOR               0x3
#define SCMI_MSG_BASE_DISCOVER_SUB_VENDOR           0x4
#define SCMI_MSG_BASE_DISCOVER_IMPLEMENTATION_VERSION 0x5
#define SCMI_MSG_BASE_DISCOVER_LIST_PROTOCOLS       0x6

struct IMX95SCMIServerState {
    SysBusDevice    parent_obj;

    /* Wiring (set via properties at realize). */
    IMXMUState     *mu;             /* MU2: the SCMI channel transport */
    uint64_t        shmem_base;     /* sram0 base, default 0x445b1000 */
    uint64_t        shmem_size;     /* sram0 size, default 0x400 */

    /*
     * Which doorbell channel the SCMI server uses on the MU. The BSP
     * DTS firmware/scmi node uses two pairs (idx 0 and 1); the agent
     * picks one per message. We watch both as inbound and assert the
     * matching idx on the response side.
     */
    unsigned int    inbound_channels;   /* default: 0x3 (channels 0,1) */
};

#endif /* IMX95_SCMI_SERVER_H */
