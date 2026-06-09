/*
 * NXP i.MX 95 Audio Transceiver (XCVR / SPDIF)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from the i.MX93 XCVR model (shared Cadence/NXP IP). Models the XCVR
 * control registers so the fsl_xcvr driver probes and registers its SPDIF card,
 * plus a functional transmit FIFO. Unlike the i.MX93 (SPDIF-only and
 * firmware-free) the i.MX95 soc_data has use_phy + fw_name set, so the driver
 * also (a) writes the xcvr-imx95.bin firmware into the code RAM window (banked
 * through EXT_CTRL.PAGE) and (b) brings up the PHY/PLL through the indirect AI
 * interface - both of which this model accepts/acks (the firmware is not
 * executed; the AI accesses are completed via the DONE bits the driver polls).
 * Once the driver releases the TX datapath (EXT_CTRL.TX_DPTH_RESET clear) with
 * DMA-read enabled, words the eDMA writes to TX_FIFO (0xe00) are clocked out at
 * the audio word rate and a dma-req is pulsed as the FIFO drains past the TX
 * watermark - the same cyclic playback contract the SAI uses. Played samples go
 * to the audio backend. The i.MX95 has two interrupt lines (vs the 93's one).
 */

#ifndef HW_AUDIO_IMX95_XCVR_H
#define HW_AUDIO_IMX95_XCVR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/audio.h"

#define TYPE_IMX95_XCVR "imx95.xcvr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95XcvrState, IMX95_XCVR)

#define IMX95_XCVR_SIZE     0x1000
#define IMX95_XCVR_REG_OFF  0x800            /* control registers start here */
#define IMX95_XCVR_NUM_REGS (0x400 / 4)      /* control register window */
#define IMX95_XCVR_RAM_SIZE 0x800            /* firmware RAM (one page) below */
#define IMX95_XCVR_FIFO_DEPTH 128
#define IMX95_XCVR_CAP_SIZE   16384
#define IMX95_XCVR_NUM_IRQS   2

struct IMX95XcvrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[IMX95_XCVR_NUM_IRQS];
    qemu_irq dma_req;           /* TX FIFO-needs-data request to the eDMA */
    uint32_t regs[IMX95_XCVR_NUM_REGS];
    uint8_t ram[IMX95_XCVR_RAM_SIZE];
    uint32_t ai_sub[256];   /* PHY/PLL sub-registers via the AI interface */

    /* Transmit FIFO (SPDIF playback). */
    QEMUTimer *tx_timer;
    uint32_t tx_fifo[IMX95_XCVR_FIFO_DEPTH];
    uint32_t tx_rptr;
    uint32_t tx_wptr;
    uint32_t tx_count;
    uint64_t tx_words;

    /* Audio backend: clocked-out samples go to an -audiodev (e.g. wav). */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool      voice_active;
    uint8_t   cap[IMX95_XCVR_CAP_SIZE];
    uint32_t  cap_head;
    uint32_t  cap_count;
};

#endif /* HW_AUDIO_IMX95_XCVR_H */
