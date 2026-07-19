/*
 * NXP i.MX 95 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/imx95_micfil.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/* Register map */
#define MICFIL_CTRL1    0x00    /* Control 1                */
#define MICFIL_CTRL2    0x04    /* Control 2                */
#define MICFIL_STAT     0x08    /* Status (read-only here)  */
#define MICFIL_FIFO_CTRL 0x10   /* FIFO control (watermark) */
#define MICFIL_FIFO_STAT 0x14   /* FIFO status              */
#define MICFIL_DATACH0  0x24    /* Channel 0 data (FIFO pop) */
#define MICFIL_DATACH7  0x40    /* Channel 7 data           */
#define MICFIL_VERID    0x84    /* Version ID (read-only)   */
#define MICFIL_PARAM    0x88    /* Parameter   (read-only)  */

#define MICFIL_CTRL1_MDIS       (1u << 31)  /* Module disable               */
#define MICFIL_CTRL1_PDMIEN     (1u << 29)  /* PDM interface enable         */
#define MICFIL_CTRL1_SRES       (1u << 27)  /* Software reset (self-clearing) */
#define MICFIL_CTRL1_DISEL      (3u << 24)  /* DMA/IRQ select               */
#define MICFIL_CTRL1_DISEL_DMA  (1u << 24)  /* DISEL == 01: DMA request      */

#define MICFIL_FIFO_CTRL_FIFOWMK 0x1f       /* Watermark (bits 4:0)         */

/*
 * fsl-micfil reads BOTH of these at probe (fsl_micfil.c: VERID ->
 * verid.version/feature; PARAM -> param.{hwvad_num,fifo_ptrwid,npair,...}).
 *
 * VERID is an IDENTITY register, so it gets the RM's reset value, 0x020F_0000
 * (major 2, minor 15). We used to invent 0x0100_0000.
 *
 * PARAM IS A CAPABILITY REGISTER, AND IT DOES NOT GET THE RM'S VALUE JUST
 * BECAUSE THE RM SAYS SO. The RM resets it to 0x010B_0154, whose NUM_HWVAD
 * field is 1 and whose HWVAD_ZCD/HWVAD_ENERGY bits are set - i.e. the silicon
 * has one hardware voice-activity detector. THIS MODEL HAS NO HWVAD AT ALL: no
 * detector, no energy mode, no zero-crossing, nothing that would ever raise a
 * VAD event. Advertising it would let a guest enable HWVAD and then wait
 * forever for a detection that cannot come.
 *
 * mcxn947qemu's rule, which they paid for on a uSDHC capability register: ON A
 * CAPABILITY REGISTER, MATCHING THE MANUAL IS THE BUG UNLESS YOU ALSO IMPLEMENT
 * THE CHIP BEHIND IT. So NUM_HWVAD stays 0 and the HWVAD feature bits stay
 * clear - an honest under-report of a detector we do not have.
 *
 * The rest is the RM's: FIFO_PTRWID=5 (we hold 64 words, so this under-reports
 * our own capacity, which is the safe direction; the driver takes its watermark
 * from soc_data anyway) and NPAIR=4.
 */
#define MICFIL_VERID_VALUE  0x020F0000
#define MICFIL_PARAM_VALUE  0x00000154

/*
 * MICFIL capture word period. HARDCODED 48 kHz - SCAFFOLD, not the real rate.
 *
 * On silicon fs = pdm_mclk / (CLKDIV * OSR * 8), where the driver keeps
 * CLKDIV/OSR constant and puts the rate in pdm_mclk (fsl_micfil.c: mclk =
 * rate * clkdiv * osr * 8; clk_set_rate). On i.MX95 clk_set_rate goes over SCMI
 * to the SM, which programs the AUDIO_PLL + PDM root - and the resulting Hz is
 * computed by the SM firmware's clock tree (DEV_SM_ClockRateGet), NOT stored in
 * any register this model can read. Boot-measured: during a real capture the SM
 * DOES feed a genuine pdm_mclk = 49152000 (= 48000 x 1024), so the rate is REAL -
 * but it is NOT DERIVABLE from the model. Instrumenting ANATOP shows the SM
 * leaves AUDIO_PLL1 at DIV=0x13b00 (MFI=1 -> 24 MHz VCO), DFS_DIV=0 - registers
 * that cannot encode 393216000. Because QEMU has no analog PLL, the SM acks
 * POWERUP/LOCK and serves the Hz from its own firmware clock-tree state, not from
 * coherent registers. So there is nothing in the model to derive from (full
 * evidence in known-limitations sec.7). Latent: the capture test exercises only
 * 48 kHz, which the hardcode matches - the SAI-48k masking, one block over.
 */
#define MICFIL_WORD_NS  (1000000000LL / 48000)

#define R(s, off)   ((s)->regs[(off) >> 2])

/* The module clocks samples in once enabled and not disabled. */
static bool imx95_micfil_running(IMX95MicfilState *s)
{
    uint32_t ctrl1 = R(s, MICFIL_CTRL1);

    return (ctrl1 & MICFIL_CTRL1_PDMIEN) && !(ctrl1 & MICFIL_CTRL1_MDIS);
}

/*
 * Synthesise the next captured sample. With no physical PDM mic, the model
 * produces a low-frequency sawtooth (ramped in both the low and high 16 bits so
 * it is non-silent whether the card captures S16 or S32). rx_words is the lone
 * monotonic sample index, advanced whether the word is buffered by the tick or
 * generated on demand by a DATACH0 read - keeping one continuous waveform.
 */
static uint32_t imx95_micfil_next_word(IMX95MicfilState *s)
{
    uint16_t s16 = (uint16_t)((s->rx_words & 0x7f) << 9);

    s->rx_words++;
    return ((uint32_t)s16 << 16) | s16;
}

static void imx95_micfil_rx_reset(IMX95MicfilState *s)
{
    s->rx_rptr = 0;
    s->rx_wptr = 0;
    s->rx_count = 0;
}

/*
 * Receive tick: clock one sample into the FIFO per word period. As the fill
 * passes the watermark, request an eDMA drain (the eDMA reads DATACH0 -> mem,
 * the cyclic capture path).
 */
static void imx95_micfil_rx_tick(void *opaque)
{
    IMX95MicfilState *s = opaque;
    uint32_t watermark = R(s, MICFIL_FIFO_CTRL) & MICFIL_FIFO_CTRL_FIFOWMK;
    uint32_t disel = R(s, MICFIL_CTRL1) & MICFIL_CTRL1_DISEL;
    bool dma = disel == MICFIL_CTRL1_DISEL_DMA;

    if (!imx95_micfil_running(s)) {
        return;
    }

    if (s->rx_count < IMX95_MICFIL_FIFO_DEPTH) {
        s->rx_fifo[s->rx_wptr] = imx95_micfil_next_word(s);
        s->rx_wptr = (s->rx_wptr + 1) % IMX95_MICFIL_FIFO_DEPTH;
        s->rx_count++;
    }

    if (dma && s->rx_count > watermark) {
        qemu_irq_pulse(s->dma_req);
    }

    timer_mod(s->rx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MICFIL_WORD_NS);
}

static uint64_t imx95_micfil_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
        return MICFIL_VERID_VALUE;
    case MICFIL_PARAM:
        return MICFIL_PARAM_VALUE;
    case MICFIL_STAT:
        /* Not busy, no channel/error flags pending. */
        return 0;
    case MICFIL_FIFO_STAT:
        /* No overflow/underflow reported. */
        return 0;
    default:
        if (offset >= MICFIL_DATACH0 && offset <= MICFIL_DATACH7) {
            /*
             * The eDMA reads a channel's data register to drain the FIFO. Pop
             * the next sample; if the FIFO is empty but the module is running
             * (the eDMA bursts several words per request, out-running the
             * word-rate tick), synthesise on demand so a real capture never
             * reads silence.
             */
            uint32_t word;

            if (s->rx_count > 0) {
                word = s->rx_fifo[s->rx_rptr];
                s->rx_rptr = (s->rx_rptr + 1) % IMX95_MICFIL_FIFO_DEPTH;
                s->rx_count--;
            } else {
                word = imx95_micfil_running(s) ? imx95_micfil_next_word(s) : 0;
            }
            return word;
        }
        if ((offset >> 2) >= IMX95_MICFIL_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx95_micfil_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX95MicfilState *s = opaque;

    switch (offset) {
    case MICFIL_VERID:
    case MICFIL_PARAM:
        /* Read-only identification registers. */
        return;
    case MICFIL_CTRL1: {
        bool was_running = imx95_micfil_running(s);
        bool now_running;

        /* Software reset is momentary: never latch it, and drain the FIFO. */
        if (value & MICFIL_CTRL1_SRES) {
            imx95_micfil_rx_reset(s);
        }
        value &= ~MICFIL_CTRL1_SRES;
        R(s, MICFIL_CTRL1) = value;

        now_running = imx95_micfil_running(s);
        if (now_running && !was_running) {
            imx95_micfil_rx_reset(s);
            timer_mod(s->rx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MICFIL_WORD_NS);
        } else if (!now_running && was_running) {
            timer_del(s->rx_timer);
            imx95_micfil_rx_reset(s);
        }
        return;
    }
    default:
        break;
    }

    if ((offset >> 2) < IMX95_MICFIL_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx95_micfil_ops = {
    .read = imx95_micfil_read,
    .write = imx95_micfil_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_micfil_reset(DeviceState *dev)
{
    IMX95MicfilState *s = IMX95_MICFIL(dev);

    timer_del(s->rx_timer);
    memset(s->regs, 0, sizeof(s->regs));
    imx95_micfil_rx_reset(s);
    s->rx_words = 0;
}

static void imx95_micfil_realize(DeviceState *dev, Error **errp)
{
    IMX95MicfilState *s = IMX95_MICFIL(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx95_micfil_ops, s,
                          TYPE_IMX95_MICFIL, IMX95_MICFIL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (i = 0; i < IMX95_MICFIL_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    qdev_init_gpio_out_named(dev, &s->dma_req, "dma-req", 1);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx95_micfil_rx_tick, s);
}

static const VMStateDescription vmstate_imx95_micfil = {
    .name = TYPE_IMX95_MICFIL,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95MicfilState, IMX95_MICFIL_REGS),
        VMSTATE_TIMER_PTR(rx_timer, IMX95MicfilState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IMX95MicfilState,
                             IMX95_MICFIL_FIFO_DEPTH),
        VMSTATE_UINT32(rx_rptr, IMX95MicfilState),
        VMSTATE_UINT32(rx_wptr, IMX95MicfilState),
        VMSTATE_UINT32(rx_count, IMX95MicfilState),
        VMSTATE_UINT64(rx_words, IMX95MicfilState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_micfil_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_micfil_realize;
    dc->vmsd = &vmstate_imx95_micfil;
    device_class_set_legacy_reset(dc, imx95_micfil_reset);
    dc->desc = "i.MX 95 PDM microphone interface";
}

static const TypeInfo imx95_micfil_types[] = {
    {
        .name = TYPE_IMX95_MICFIL,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95MicfilState),
        .class_init = imx95_micfil_class_init,
    },
};

DEFINE_TYPES(imx95_micfil_types)
