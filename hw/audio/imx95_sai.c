/*
 * NXP i.MX 95 SAI (Synchronous Audio Interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Transmit- and receive-FIFO datapath model for the fsl-sai driver. VERID/PARAM
 * identify the block so the driver probes and the ASoC card registers; on top
 * of that both FIFOs are functional.
 *
 * TRANSMIT (playback): a word written to TDR0 is enqueued, and once the
 * transmitter is enabled (TCSR.TE) the SAI clocks one word out of the FIFO per
 * audio word period. FRF/FWF/FEF track the FIFO level exactly as hardware does,
 * the FIFO-request interrupt fires when enabled, and as the FIFO drains past
 * the watermark a dma-req burst is requested - the contract the eDMA cyclic
 * playback path and the driver's IRQ handler depend on. Clocked-out samples are
 * handed to the audio backend so "-audio" captures the playback.
 *
 * RECEIVE (capture): once the receiver is enabled (RCSR.RE) the SAI synthesises
 * a sample stream into the RX FIFO at the audio word rate; as the level rises
 * past the watermark it requests an eDMA burst, which drains RDR0 into the ring
 * buffer (the eDMA's device->memory minor loop). Because the eDMA bursts
 * several words per request and out-runs the per-word fill tick, RDR0 makes the
 * next sample on demand when read on an empty FIFO, so a burst never starves.
 */

#include "qemu/osdep.h"
#include "hw/audio/imx95_sai.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/module.h"

/* Register map (reg_offset = 8: VERID/PARAM precede the control regs). */
#define SAI_VERID       0x00    /* Version ID (read-only)            */
#define SAI_PARAM       0x04    /* Parameter   (read-only)           */
#define SAI_TCSR        0x08    /* Transmit Control/Status           */
#define SAI_TCR1        0x0c    /* Transmit Config 1 (watermark)     */
#define SAI_TDR0        0x20    /* Transmit Data 0 (FIFO push)       */
#define SAI_TFR0        0x40    /* Transmit FIFO 0 (R/W pointers)    */
#define SAI_RCSR        0x88    /* Receive Control/Status            */
#define SAI_RCR1        0x8c    /* Receive Config 1 (watermark)      */
#define SAI_RDR0        0xa0    /* Receive Data 0 (FIFO pop)         */
#define SAI_RFR0        0xc0    /* Receive FIFO 0 (R/W pointers)     */

/* TCSR bits. */
#define TCSR_TE         (1u << 31)  /* transmitter enable             */
#define TCSR_FR         (1u << 25)  /* FIFO reset (self-clearing)     */
#define TCSR_SR         (1u << 24)  /* software reset (self-clearing) */
#define TCSR_WSF        (1u << 20)  /* word-start flag    (W1C)       */
#define TCSR_SEF        (1u << 19)  /* sync-error flag    (W1C)       */
#define TCSR_FEF        (1u << 18)  /* FIFO error/underrun (W1C)      */
#define TCSR_FWF        (1u << 17)  /* FIFO warning  (read-only)      */
#define TCSR_FRF        (1u << 16)  /* FIFO request  (read-only)      */
#define TCSR_FEIE       (1u << 10)  /* FIFO error interrupt enable    */
#define TCSR_FWIE       (1u << 9)   /* FIFO warning interrupt enable  */
#define TCSR_FRIE       (1u << 8)   /* FIFO request interrupt enable  */
#define TCSR_FRDE       (1u << 0)   /* FIFO request DMA enable        */

/* RCSR has the same bit layout; only the enable bit is named differently. */
#define RCSR_RE         (1u << 31)  /* receiver enable                */

#define TCSR_STATUS     (TCSR_WSF | TCSR_SEF | TCSR_FEF | TCSR_FWF | TCSR_FRF)
#define TCSR_W1C        (TCSR_WSF | TCSR_SEF | TCSR_FEF)
#define TCSR_RO         (TCSR_FWF | TCSR_FRF)

#define R(s, off)       ((s)->regs[(off) / 4])

/*
 * VERID: major 3, minor 3, feature 0. A zero feature word keeps the timestamp
 * (TSTMP_EN) path out of the driver's probe, which is all we need here.
 */
/*
 * VERID/PARAM (the verid/param properties). fsl_sai READS PARAM at probe
 * (fsl_sai.c: sai->param.dataline = PARAM & DLN_MASK; spf/wpf likewise).
 * VERID is an identity register, so it gets the RM's value: 0x0302_0002.
 *
 * PARAM IS A CAPABILITY REGISTER, AND THOSE DO NOT GET THE RM'S VALUE JUST
 * BECAUSE THE RM SAYS SO. mcxn947qemu paid for this rule: they set a uSDHC
 * capability register to its RM value, thereby advertising an SDR104 tuning
 * engine their model does not contain, and a driver would have switched the
 * card to it and tuned into nothing. ON A CAPABILITY REGISTER, MATCHING THE
 * MANUAL IS THE BUG UNLESS YOU ALSO IMPLEMENT THE CHIP BEHIND IT.
 *
 * PARAM fields: FRAME[19:16] (2^FRAME slots), FIFO[11:8] (2^FIFO deep),
 * DATALINE[3:0] (number of data lines).
 *
 * DATALINE: silicon has 1/2/4/8 depending on the instance (RM Table 491). THIS
 * MODEL IMPLEMENTS EXACTLY ONE DATA LINE - only TDR0/RDR0/TFR0 exist, and there
 * is a single tx_fifo/rx_fifo. Advertising the silicon count would let a TDM
 * configuration push audio at lines 1..7 that this model silently drops. So we
 * report what we DELIVER: one. Under-reporting is the safe direction; the
 * failure mode of over-reporting is a promise made on the chip's behalf.
 *
 * FIFO: we implement a 128-deep FIFO, so we may advertise up to 128. SAI1's
 * silicon FIFO is only 32 deep, and we report 32 for it - both because it is
 * the truth about the chip AND because it under-reports our own capacity, so
 * guest code sized against this model still fits the real part.
 */
#define SAI_VERID_DEFAULT 0x03020002

#define SAI_PARAM_FRAME     5           /* 2^5 = 32 slots/frame */
#define SAI_PARAM_DATALINES 1           /* == the data lines we implement */
#define SAI_PARAM_FIFO_EXP  7           /* 2^7 = 128 == IMX95_SAI_FIFO_DEPTH */
#define SAI_PARAM_VAL(fifo_exp, lines) \
    (((uint32_t)SAI_PARAM_FRAME << 16) | ((uint32_t)(fifo_exp) << 8) | (lines))
#define SAI_PARAM_DEFAULT \
    SAI_PARAM_VAL(SAI_PARAM_FIFO_EXP, SAI_PARAM_DATALINES)

/* One word of a 48 kHz stereo stream: 96000 words/s. */
#define SAI_TX_WORD_NS  (NANOSECONDS_PER_SECOND / 96000)

static void imx95_sai_tx_update_flags(IMX95SaiState *s)
{
    uint32_t tcsr = R(s, SAI_TCSR) & ~TCSR_RO;
    uint32_t watermark = R(s, SAI_TCR1) & 0xff;

    /* FRF: FIFO level at or below the watermark - hardware wants more data. */
    if (s->tx_count <= watermark) {
        tcsr |= TCSR_FRF;
    }
    /* FWF: FIFO empty - one step from underrun. */
    if (s->tx_count == 0) {
        tcsr |= TCSR_FWF;
    }
    R(s, SAI_TCSR) = tcsr;
}

static void imx95_sai_update_irq(IMX95SaiState *s)
{
    uint32_t tcsr = R(s, SAI_TCSR);
    uint32_t rcsr = R(s, SAI_RCSR);
    bool tx = ((tcsr & TCSR_FRF) && (tcsr & TCSR_FRIE)) ||
              ((tcsr & TCSR_FWF) && (tcsr & TCSR_FWIE)) ||
              ((tcsr & TCSR_FEF) && (tcsr & TCSR_FEIE));
    bool rx = ((rcsr & TCSR_FRF) && (rcsr & TCSR_FRIE)) ||
              ((rcsr & TCSR_FWF) && (rcsr & TCSR_FWIE)) ||
              ((rcsr & TCSR_FEF) && (rcsr & TCSR_FEIE));

    qemu_set_irq(s->irq, (tx || rx) ? 1 : 0);
}

/* Clock one word out of the transmit FIFO. */
static void imx95_sai_tx_tick(void *opaque)
{
    IMX95SaiState *s = opaque;

    bool dma = R(s, SAI_TCSR) & TCSR_FRDE;
    uint32_t watermark = R(s, SAI_TCR1) & 0xff;

    if (!(R(s, SAI_TCSR) & TCSR_TE)) {
        return;
    }

    if (s->tx_count > 0) {
        /* Clock one word out of the FIFO. */
        s->tx_rptr = (s->tx_rptr + 1) % IMX95_SAI_FIFO_DEPTH;
        s->tx_count--;
        s->tx_words++;
    } else if (!dma) {
        /* PIO underrun: latch the sticky error flag. */
        R(s, SAI_TCSR) |= TCSR_FEF;
    }

    /*
     * DMA-driven playback: as the FIFO drains past the watermark, request a
     * burst from the eDMA. The eDMA services the request synchronously, writing
     * the next samples back into this FIFO via TDR0 - so the transfer is paced
     * by this drain (the audio word rate), which is what keeps the period
     * interrupts coming at real time and the playback from under-running.
     */
    if (dma && s->tx_count <= watermark) {
        qemu_irq_pulse(s->dma_req);
    }

    imx95_sai_tx_update_flags(s);
    imx95_sai_update_irq(s);

    timer_mod(s->tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SAI_TX_WORD_NS);
}

static void imx95_sai_tx_push(IMX95SaiState *s, uint32_t word)
{
    if (s->tx_count < IMX95_SAI_FIFO_DEPTH) {
        s->tx_fifo[s->tx_wptr] = word;
        s->tx_wptr = (s->tx_wptr + 1) % IMX95_SAI_FIFO_DEPTH;
        s->tx_count++;
    }
    imx95_sai_tx_update_flags(s);
    imx95_sai_update_irq(s);
}

static void imx95_sai_tx_reset_fifo(IMX95SaiState *s)
{
    s->tx_rptr = 0;
    s->tx_wptr = 0;
    s->tx_count = 0;
    imx95_sai_tx_update_flags(s);
}

/* ---- receive (capture) path ------------------------------------------- */

static void imx95_sai_rx_update_flags(IMX95SaiState *s)
{
    uint32_t rcsr = R(s, SAI_RCSR) & ~TCSR_RO;
    uint32_t watermark = R(s, SAI_RCR1) & 0xff;

    /* FRF: FIFO level above the watermark - data is ready to be drained. */
    if (s->rx_count > watermark) {
        rcsr |= TCSR_FRF;
    }
    /* FWF: FIFO full - one step from overrun. */
    if (s->rx_count >= IMX95_SAI_FIFO_DEPTH) {
        rcsr |= TCSR_FWF;
    }
    R(s, SAI_RCSR) = rcsr;
}

/*
 * Synthesise the next captured sample: a sawtooth sweeping the S16 range in
 * 256-sample periods. Non-silent and trivially verifiable by a capture oracle.
 */
static uint16_t imx95_sai_rx_synth(IMX95SaiState *s)
{
    uint16_t v = (uint16_t)(int16_t)(s->rx_phase << 8);

    s->rx_phase = (s->rx_phase + 1) & 0xff;
    return v;
}

static void imx95_sai_rx_push(IMX95SaiState *s)
{
    if (s->rx_count < IMX95_SAI_FIFO_DEPTH) {
        s->rx_fifo[s->rx_wptr] = imx95_sai_rx_synth(s);
        s->rx_wptr = (s->rx_wptr + 1) % IMX95_SAI_FIFO_DEPTH;
        s->rx_count++;
    }
    imx95_sai_rx_update_flags(s);
    imx95_sai_update_irq(s);
}

/* RDR0 read: pop a captured word, synthesising on demand if the FIFO is dry. */
static uint32_t imx95_sai_rx_pop(IMX95SaiState *s)
{
    uint32_t word;

    if (s->rx_count > 0) {
        word = s->rx_fifo[s->rx_rptr];
        s->rx_rptr = (s->rx_rptr + 1) % IMX95_SAI_FIFO_DEPTH;
        s->rx_count--;
    } else {
        /* eDMA burst out-ran the fill tick: hand it a fresh sample. */
        word = imx95_sai_rx_synth(s);
    }
    s->rx_words++;
    imx95_sai_rx_update_flags(s);
    imx95_sai_update_irq(s);
    return word;
}

static void imx95_sai_rx_reset_fifo(IMX95SaiState *s)
{
    s->rx_rptr = 0;
    s->rx_wptr = 0;
    s->rx_count = 0;
    imx95_sai_rx_update_flags(s);
}

/* Fill one word into the receive FIFO at the audio word rate. */
static void imx95_sai_rx_tick(void *opaque)
{
    IMX95SaiState *s = opaque;
    bool dma = R(s, SAI_RCSR) & TCSR_FRDE;
    uint32_t watermark = R(s, SAI_RCR1) & 0xff;

    if (!(R(s, SAI_RCSR) & RCSR_RE)) {
        return;
    }

    imx95_sai_rx_push(s);

    /*
     * As the FIFO fills past the watermark, request an eDMA burst. The eDMA
     * services it synchronously, reading RDR0 (fixed source) into the ring -
     * paced by this fill rate, which keeps the period interrupts at real time.
     */
    if (dma && s->rx_count > watermark) {
        qemu_irq_pulse(s->dma_req);
    }

    timer_mod(s->rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SAI_TX_WORD_NS);
}

/* Queue the exact bytes the DMA wrote to TDR0 for the audio backend. */
static void imx95_sai_cap_push(IMX95SaiState *s, uint32_t value, unsigned size)
{
    unsigned i;

    for (i = 0; i < size && s->cap_count < IMX95_SAI_CAP_SIZE; i++) {
        uint32_t tail = (s->cap_head + s->cap_count) % IMX95_SAI_CAP_SIZE;
        s->cap[tail] = (value >> (8 * i)) & 0xff;
        s->cap_count++;
    }
}

/* The audio backend can take 'free' bytes: hand it the played PCM. */
static void imx95_sai_audio_cb(void *opaque, int free)
{
    IMX95SaiState *s = opaque;

    while (free > 0 && s->cap_count > 0) {
        uint32_t chunk = MIN((uint32_t)free, s->cap_count);
        size_t wrote;

        chunk = MIN(chunk, IMX95_SAI_CAP_SIZE - s->cap_head);   /* ring wrap */
        wrote = audio_be_write(s->audio_be, s->voice, s->cap + s->cap_head,
                               chunk);
        if (wrote == 0) {
            break;
        }
        s->cap_head = (s->cap_head + wrote) % IMX95_SAI_CAP_SIZE;
        s->cap_count -= wrote;
        free -= wrote;
    }
}

static void imx95_sai_voice_set(IMX95SaiState *s, bool on)
{
    if (s->voice && on != s->voice_active) {
        audio_be_set_active_out(s->audio_be, s->voice, on);
        s->voice_active = on;
    }
}

static uint64_t imx95_sai_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95SaiState *s = opaque;

    switch (offset) {
    case SAI_VERID:
        return s->verid;
    case SAI_PARAM:
        return s->param;
    case SAI_TFR0:
        /* Read/write FIFO pointers so the driver can compute the fill. */
        return ((uint32_t)s->tx_wptr << 16) | s->tx_rptr;
    case SAI_RDR0:
        /* Pop a captured word (the eDMA's fixed-address source read). */
        return imx95_sai_rx_pop(s);
    case SAI_RFR0:
        return ((uint32_t)s->rx_wptr << 16) | s->rx_rptr;
    default:
        if ((offset >> 2) >= IMX95_SAI_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx95_sai_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX95SaiState *s = opaque;

    switch (offset) {
    case SAI_VERID:
    case SAI_PARAM:
        /* Read-only identification registers. */
        return;

    case SAI_TDR0:
        imx95_sai_tx_push(s, (uint32_t)value);
        imx95_sai_cap_push(s, (uint32_t)value, size);
        return;

    case SAI_TCSR: {
        uint32_t old = R(s, SAI_TCSR);
        uint32_t v = value;

        if (v & TCSR_FR) {
            imx95_sai_tx_reset_fifo(s);
        }
        /* FIFO/software reset are momentary: never latch them. */
        v &= ~(TCSR_FR | TCSR_SR);

        /* Status field: keep read-only bits, write-1-to-clear the sticky. */
        v = (v & ~TCSR_STATUS) | (old & TCSR_RO) |
            (old & TCSR_W1C & ~((uint32_t)value & TCSR_W1C));
        R(s, SAI_TCSR) = v;

        if ((v & TCSR_TE) && !(old & TCSR_TE)) {
            /* Transmitter enabled: start clocking words out. */
            imx95_sai_tx_update_flags(s);
            imx95_sai_voice_set(s, true);
            timer_mod(s->tx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SAI_TX_WORD_NS);
        } else if (!(v & TCSR_TE) && (old & TCSR_TE)) {
            timer_del(s->tx_timer);
            imx95_sai_voice_set(s, false);
        }
        imx95_sai_update_irq(s);
        return;
    }

    case SAI_RCSR: {
        uint32_t old = R(s, SAI_RCSR);
        uint32_t v = value;

        if (v & TCSR_FR) {
            imx95_sai_rx_reset_fifo(s);
        }
        /* FIFO/software reset are momentary: never latch them. */
        v &= ~(TCSR_FR | TCSR_SR);

        /* Status field: keep read-only bits, write-1-to-clear the sticky. */
        v = (v & ~TCSR_STATUS) | (old & TCSR_RO) |
            (old & TCSR_W1C & ~((uint32_t)value & TCSR_W1C));
        R(s, SAI_RCSR) = v;

        if ((v & RCSR_RE) && !(old & RCSR_RE)) {
            /* Receiver enabled: start filling the FIFO with samples. */
            imx95_sai_rx_update_flags(s);
            timer_mod(s->rx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SAI_TX_WORD_NS);
        } else if (!(v & RCSR_RE) && (old & RCSR_RE)) {
            timer_del(s->rx_timer);
        }
        imx95_sai_update_irq(s);
        return;
    }

    default:
        break;
    }

    if ((offset >> 2) < IMX95_SAI_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx95_sai_ops = {
    .read = imx95_sai_read,
    .write = imx95_sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Allow 16-bit access: the eDMA writes S16 samples to TDR0. */
    .valid = { .min_access_size = 2, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 4 },
};

static void imx95_sai_reset(DeviceState *dev)
{
    IMX95SaiState *s = IMX95_SAI(dev);

    timer_del(s->tx_timer);
    timer_del(s->rx_timer);
    memset(s->regs, 0, sizeof(s->regs));
    imx95_sai_tx_reset_fifo(s);
    imx95_sai_rx_reset_fifo(s);
    s->tx_words = 0;
    s->rx_words = 0;
    s->rx_phase = 0;
    s->cap_head = 0;
    s->cap_count = 0;
    imx95_sai_voice_set(s, false);
    qemu_set_irq(s->irq, 0);
}

static void imx95_sai_realize(DeviceState *dev, Error **errp)
{
    IMX95SaiState *s = IMX95_SAI(dev);
    struct audsettings as = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    /*
     * A capability register that is a CONSTANT can drift from the thing it
     * describes; one COMPUTED FROM it cannot (mcxn947qemu's structural fix).
     *
     * AND ASSERT AGAINST THE THING THE BYTES LAND IN, NOT AGAINST THE NAME YOU
     * GAVE ITS SIZE (mcxn/91, sharpening it). An earlier version of this
     * compared (1 << FIFO_EXP) with IMX95_SAI_FIFO_DEPTH - a MACRO against a
     * MACRO, which is two spellings of one belief agreeing with itself. The
     * guest's samples do not land in a macro; they land in tx_fifo[], whose
     * bound merely HAPPENS to be spelled with that name. Respell it as a
     * literal
     * and the old assert would still pass while PARAM advertised a FIFO we do
     * not have. So: decode the field the way the GUEST decodes it, and compare
     * it to the array we ACTUALLY HAVE. That is the only pair of numbers whose
     * disagreement IS the bug.
     *
     * ">" not "!=": the model may hold MORE than it advertises (SAI1 truthfully
     * reports silicon's 32-deep FIFO while we carry 128). Under-reporting is
     * the
     * safe direction, and an "!=" would forbid it - forcing us to advertise
     * everything we hold, which is the exact over-promise this guard exists to
     * prevent.
     */
    QEMU_BUILD_BUG_ON((1u << SAI_PARAM_FIFO_EXP) >
                      ARRAY_SIZE(((IMX95SaiState *)0)->tx_fifo));
    QEMU_BUILD_BUG_ON((1u << SAI_PARAM_FIFO_EXP) >
                      ARRAY_SIZE(((IMX95SaiState *)0)->rx_fifo));
    /* We implement TDR0/RDR0 only. */
    QEMU_BUILD_BUG_ON(SAI_PARAM_DATALINES > 1);

    /*
     * The build assert only guards the DEFAULT. PARAM is a per-instance
     * PROPERTY, so a board can set any value it likes - including one that
     * promises a FIFO or a dataline count this model cannot honour. Check the
     * value we were actually given, decoded exactly as fsl_sai decodes it, and
     * refuse to realize rather than lie to the guest.
     */
    if ((1u << ((s->param >> 8) & 0xf)) > ARRAY_SIZE(s->tx_fifo)) {
        error_setg(errp, "imx95.sai: param 0x%08x advertises a %u-word FIFO; "
                   "this model holds %zu", s->param,
                   1u << ((s->param >> 8) & 0xf), ARRAY_SIZE(s->tx_fifo));
        return;
    }
    if ((s->param & 0xf) > SAI_PARAM_DATALINES) {
        error_setg(errp, "imx95.sai: param 0x%08x advertises %u data lines; "
                   "this model implements %u", s->param, s->param & 0xf,
                   SAI_PARAM_DATALINES);
        return;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_sai_ops, s,
                          TYPE_IMX95_SAI, IMX95_SAI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_req, "dma-req", 1);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx95_sai_tx_tick, s);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx95_sai_rx_tick, s);

    /*
     * Audio backend: clocked-out samples are handed to the default audio
     * backend, so "-audio driver=wav,path=out.wav" captures the playback. The
     * model assumes the EVK's common 48 kHz S16 stereo format. This is best
     * effort - with no audio configured there is simply no backend and the
     * samples are dropped, leaving the (DMA-paced) datapath unchanged.
     */
    if (audio_be_check(&s->audio_be, NULL)) {
        s->voice = audio_be_open_out(s->audio_be, NULL, "imx95-sai-tx", s,
                                     imx95_sai_audio_cb, &as);
    }
}

static const VMStateDescription vmstate_imx95_sai = {
    .name = TYPE_IMX95_SAI,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95SaiState, IMX95_SAI_REGS),
        VMSTATE_UINT32_ARRAY(tx_fifo, IMX95SaiState, IMX95_SAI_FIFO_DEPTH),
        VMSTATE_UINT32(tx_rptr, IMX95SaiState),
        VMSTATE_UINT32(tx_wptr, IMX95SaiState),
        VMSTATE_UINT32(tx_count, IMX95SaiState),
        VMSTATE_UINT64(tx_words, IMX95SaiState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IMX95SaiState, IMX95_SAI_FIFO_DEPTH),
        VMSTATE_UINT32(rx_rptr, IMX95SaiState),
        VMSTATE_UINT32(rx_wptr, IMX95SaiState),
        VMSTATE_UINT32(rx_count, IMX95SaiState),
        VMSTATE_UINT64(rx_words, IMX95SaiState),
        VMSTATE_UINT16(rx_phase, IMX95SaiState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imx95_sai_properties[] = {
    DEFINE_PROP_UINT32("verid", IMX95SaiState, verid, SAI_VERID_DEFAULT),
    DEFINE_PROP_UINT32("param", IMX95SaiState, param, SAI_PARAM_DEFAULT),
};

static void imx95_sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_sai_realize;
    dc->vmsd = &vmstate_imx95_sai;
    device_class_set_legacy_reset(dc, imx95_sai_reset);
    device_class_set_props(dc, imx95_sai_properties);
    dc->desc = "i.MX 95 Synchronous Audio Interface";
}

static const TypeInfo imx95_sai_types[] = {
    {
        .name = TYPE_IMX95_SAI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95SaiState),
        .class_init = imx95_sai_class_init,
    },
};

DEFINE_TYPES(imx95_sai_types)
