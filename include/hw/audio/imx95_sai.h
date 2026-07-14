/*
 * NXP i.MX 95 SAI (Synchronous Audio Interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Each SAI is an I2S transmit/receive front-end whose FIFOs are drained/filled
 * by eDMA. This model carries the register file the fsl-sai driver needs to
 * probe and the ASoC card to register (VERID/PARAM, self-clearing reset bits)
 * plus a functional transmit FIFO: words pushed to TDR0 (by the eDMA) are
 * clocked out at the audio word rate, maintaining the request/warning/error
 * flags and FIFO-request interrupt the driver relies on, requesting eDMA bursts
 * as the FIFO drains, and handing the played samples to the audio backend (so
 * -audio captures the playback). A receive FIFO synthesises a capture stream
 * the same way.
 */

#ifndef IMX95_SAI_H
#define IMX95_SAI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/audio.h"

#define TYPE_IMX95_SAI "imx95.sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95SaiState, IMX95_SAI)

#define IMX95_SAI_SIZE   0x10000
/* Registers run from VERID (0x00) to MDIV (0x104); cover a little past that. */
#define IMX95_SAI_REGS   (0x108 / 4)

/* PARAM reports WPF=7 -> a 128-word transmit FIFO per data line. */
#define IMX95_SAI_FIFO_DEPTH 128

/* Capture ring decoupling the FIFO drain from the audio backend's callback. */
#define IMX95_SAI_CAP_SIZE 16384

struct IMX95SaiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req;           /* TX FIFO-needs-data request to the eDMA */
    uint32_t regs[IMX95_SAI_REGS];

    /* RM VERID/PARAM reset values - per instance (datalines/FIFO differ). */
    uint32_t verid;
    uint32_t param;

    /*
     * The sample rate the CODEC told us it is clocking at.
     *
     * This model used to pace its FIFO - and open its audio voice - at a
     * hardcoded 48 kHz. Every audio test asks for 48 kHz, which is the one
     * rate at which a SAI that ignores the requested rate and a SAI that
     * honours it are indistinguishable. A 16 kHz stream was clocked out three
     * times too fast and everything looked healthy.
     *
     * And it CANNOT be derived from this device. On a wm8962 board the SAI
     * is a bit-clock SLAVE (TCR2.BCD_MSTR = 0): the codec drives BCLK/LRCLK,
     * the rate is programmed into the codec over I2C, and the SAI's own
     * divider registers are never touched - at 48 kHz and 16 kHz they are
     * byte-identical. A
     * divider computation here would be correct for a bit-clock MASTER, would
     * agree with itself at the only rate anyone tests, and would be a
     * fabrication with an RM citation attached. (93emulator paid for that one.)
     *
     * So the rate arrives the way it does on the board: FROM THE CODEC, on the
     * "codec-rate" input.
     */
    uint32_t rate;
    bool     rate_announced;    /* the CODEC told us; not a guess */

    /* Transmit FIFO (data line 0). */
    QEMUTimer *tx_timer;
    uint32_t tx_fifo[IMX95_SAI_FIFO_DEPTH];
    uint32_t tx_rptr;           /* read (transmit) pointer */
    uint32_t tx_wptr;           /* write (TDR0) pointer */
    uint32_t tx_count;          /* words currently in the FIFO */
    uint64_t tx_words;          /* total words clocked out (bookkeeping) */

    /*
     * Receive FIFO (data line 0) - capture. The receiver synthesises a sample
     * stream into this FIFO at the audio word rate; as it fills past the
     * watermark it requests an eDMA burst, which drains RDR0 into memory.
     */
    QEMUTimer *rx_timer;
    uint32_t rx_fifo[IMX95_SAI_FIFO_DEPTH];
    uint32_t rx_rptr;           /* read (RDR0 pop) pointer */
    uint32_t rx_wptr;           /* write (synthesis) pointer */
    uint32_t rx_count;          /* words currently in the FIFO */
    uint64_t rx_words;          /* total words captured (bookkeeping) */
    uint16_t rx_phase;          /* sawtooth-synthesis phase */

    /* Audio backend: clocked-out samples go to an -audiodev (e.g. wav). */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool      voice_active;
    uint8_t   cap[IMX95_SAI_CAP_SIZE];   /* played PCM awaiting the backend */
    uint32_t  cap_head;
    uint32_t  cap_count;
};

#endif /* IMX95_SAI_H */
