/*
 * NXP i.MX 95 Audio Transceiver (XCVR / SPDIF)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Ported from the i.MX93 XCVR model (shared IP). The control registers (at
 * offset 0x800) use the i.MX SET/CLR/TOG alias pattern: each register R is also
 * writable at R+4 (set bits), R+8 (clear bits) and R+0xC (toggle bits). The
 * PHY/PLL sub-registers are reached through an indirect "AI" interface
 * (PHY_AI_CTRL/WDATA/RDATA): a toggle of the PLL/PHY bit triggers an access the
 * hardware acknowledges by setting the matching DONE bit, which the driver
 * polls. The firmware-code RAM sits below the registers (the driver writes
 * xcvr-imx95.bin into it page by page); the firmware itself is not executed.
 * Modelling these lets the fsl_xcvr driver probe (firmware load + PHY/PLL
 * bring-up) and register its SPDIF card, with a functional TX playback FIFO.
 */

#include "qemu/osdep.h"
#include "hw/audio/imx95_xcvr.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define XCVR_VERSION        0x00
#define XCVR_PHY_AI_CTRL    0x90
#define XCVR_PHY_AI_WDATA   0xa0
#define XCVR_PHY_AI_RDATA   0xa4

#define AI_CTRL_RWB         (1u << 31)
#define AI_TOG_PLL          (1u << 24)
#define AI_DONE_PLL         (1u << 25)
#define AI_TOG_PHY          (1u << 26)
#define AI_DONE_PHY         (1u << 27)

/*
 * RM reset values, keyed by offset within the "regs" window (the DT gives
 * fsl_xcvr a separate reg region at base+0x800, so the driver's offset 0 is the
 * RM's 0x800). Everything here was memset to 0 before, which is a claim the
 * silicon does not make - and fsl_xcvr READ-MODIFY-WRITES several of these via
 * regmap_update_bits(), so a zero reset is laundered into the guest's own
 * config. VERSION is 0 on silicon (the RM *and* the driver's own reg_defaults
 * table agree); the 0x00010000 we used to return was invented.
 */
/* EXT_CTRL: CORE/CMDC/DPTH held in reset, SLEEP_MODE, RX_FWM=TX_FWM=64. */
#define XCVR_EXT_CTRL_RESET     0xf8204040
#define XCVR_RX_CMDC_CTRL       0xc0
#define XCVR_RX_CMDC_CTRL_RESET 0x00281b02
#define XCVR_RX_DPATH_CTRL      0x180
#define XCVR_RX_DPATH_CTRL_RST  0x00002c89
#define XCVR_DMAC_PRE_MATCH     0x1e0
#define XCVR_DMAC_PRE_MATCH_RST 0x4e1ff872  /* SPDIF preamble match pattern */
#define XCVR_DMAC_DTS_PRE_MATCH 0x1f0
#define XCVR_DMAC_DTS_PRE_RST   0x1387fe1c
#define XCVR_HPD_DBNC_CTRL      0x2a0
#define XCVR_HPD_DBNC_CTRL_RST  0x00030d40

#define RFDR_FIFO 0x0c00
#define TFDR_FIFO 0x0e00

/*
 * EXT_CTRL lives at MMIO 0x810 = REG_OFF(0x800) + 0x10, so its index into the
 * control-register window is 0x10/4. Its bits gate the transmit datapath.
 */
#define XCVR_EXT_CTRL_REL       0x10
#define EXT_CTRL_TX_DPTH_RESET  (1u << 27)  /* TX datapath in reset       */
/*
 * The driver gates the TX (playback) DMA with DMA_DIS(tx) == BIT(24)
 * (DMA_WR_DIS) - counterintuitively NOT DMA_RD_DIS (BIT(25), which is the RX
 * gate). On TRIGGER_START it clears BIT(24) to let the eDMA feed the TX FIFO,
 * so the TX datapath is active once that bit is clear.
 */
#define EXT_CTRL_TX_DMA_DIS     (1u << 24)  /* DMA_WR_DIS - gates TX DMA   */
#define EXT_CTRL_SPDIF_MODE     (1u << 23)  /* SPDIF mode selected         */
#define EXT_CTRL_TX_FWM_MASK    0x7f        /* TX FIFO watermark [6:0]     */

/* SPDIF stereo: 2 ch x 48 kHz = 96000 words/s. */
#define XCVR_TX_WORD_NS (NANOSECONDS_PER_SECOND / 96000)

/* TX clocks once SPDIF mode is on, the datapath released and DMA enabled. */
static bool xcvr_tx_active(IMX95XcvrState *s)
{
    uint32_t ec = s->regs[XCVR_EXT_CTRL_REL >> 2];

    return (ec & EXT_CTRL_SPDIF_MODE) && !(ec & EXT_CTRL_TX_DPTH_RESET) &&
           !(ec & EXT_CTRL_TX_DMA_DIS);
}

/* Queue clocked-out bytes for the audio backend. */
static void xcvr_cap_push(IMX95XcvrState *s, uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4 && s->cap_count < IMX95_XCVR_CAP_SIZE; i++) {
        uint32_t tail = (s->cap_head + s->cap_count) % IMX95_XCVR_CAP_SIZE;
        s->cap[tail] = (value >> (8 * i)) & 0xff;
        s->cap_count++;
    }
}

static void xcvr_audio_cb(void *opaque, int free)
{
    IMX95XcvrState *s = opaque;

    while (free > 0 && s->cap_count > 0) {
        uint32_t chunk = MIN((uint32_t)free, s->cap_count);
        size_t wrote;

        chunk = MIN(chunk, IMX95_XCVR_CAP_SIZE - s->cap_head);
        wrote = audio_be_write(s->audio_be, s->voice, s->cap + s->cap_head,
                               chunk);
        if (wrote == 0) {
            break;
        }
        s->cap_head = (s->cap_head + wrote) % IMX95_XCVR_CAP_SIZE;
        s->cap_count -= wrote;
        free -= wrote;
    }
}

static void xcvr_voice_set(IMX95XcvrState *s, bool on)
{
    if (s->voice && on != s->voice_active) {
        audio_be_set_active_out(s->audio_be, s->voice, on);
        s->voice_active = on;
    }
}

static void xcvr_tx_push(IMX95XcvrState *s, uint32_t word)
{
    if (s->tx_count < IMX95_XCVR_FIFO_DEPTH) {
        s->tx_fifo[s->tx_wptr] = word;
        s->tx_wptr = (s->tx_wptr + 1) % IMX95_XCVR_FIFO_DEPTH;
        s->tx_count++;
    }
}

/* Clock one word out of the transmit FIFO at the audio word rate. */
static void xcvr_tx_tick(void *opaque)
{
    IMX95XcvrState *s = opaque;
    uint32_t watermark = s->regs[XCVR_EXT_CTRL_REL >> 2] & EXT_CTRL_TX_FWM_MASK;

    if (!xcvr_tx_active(s)) {
        return;
    }
    if (s->tx_count > 0) {
        s->tx_rptr = (s->tx_rptr + 1) % IMX95_XCVR_FIFO_DEPTH;
        s->tx_count--;
        s->tx_words++;
    }
    /* As the FIFO drains past the watermark, request the next eDMA burst. */
    if (s->tx_count <= watermark) {
        qemu_irq_pulse(s->dma_req);
    }
    timer_mod(s->tx_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + XCVR_TX_WORD_NS);
}

/* Perform the indirect PHY/PLL access and acknowledge it via the DONE bits. */
static void xcvr_ai_complete(IMX95XcvrState *s)
{
    uint32_t ctrl = s->regs[XCVR_PHY_AI_CTRL >> 2];
    uint8_t addr = ctrl & 0xff;

    if (ctrl & AI_CTRL_RWB) {                       /* read */
        s->regs[XCVR_PHY_AI_RDATA >> 2] = s->ai_sub[addr];
    } else {                                        /* write */
        s->ai_sub[addr] = s->regs[XCVR_PHY_AI_WDATA >> 2];
    }
    /* DONE bit follows the TOG bit so the driver's poll completes. */
    ctrl &= ~(AI_DONE_PLL | AI_DONE_PHY);
    ctrl |= (ctrl & AI_TOG_PLL) ? AI_DONE_PLL : 0;
    ctrl |= (ctrl & AI_TOG_PHY) ? AI_DONE_PHY : 0;
    s->regs[XCVR_PHY_AI_CTRL >> 2] = ctrl;
}

static uint64_t xcvr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95XcvrState *s = opaque;
    uint32_t rel, sub;

    if (getenv("XCVR_DBG") && offset != RFDR_FIFO && offset != TFDR_FIFO) {
        fprintf(stderr, "[xcvr] RD off=0x%04x sz=%u\n",
                (unsigned)offset, size);
    }

    if (offset < IMX95_XCVR_REG_OFF) {
        uint64_t v = 0;
        unsigned b;

        /*
         * size may be up to 8 (firmware load uses 8-byte accesses), so the
         * accumulator must be 64-bit: a 32-bit << (b*8) for b >= 4 is UB.
         */
        for (b = 0; b < size && offset + b < IMX95_XCVR_RAM_SIZE; b++) {
            v |= (uint64_t)s->ram[offset + b] << (b * 8);
        }
        return v;
    }
    if (offset == RFDR_FIFO || offset == TFDR_FIFO) {
        return 0;
    }
    rel = offset - IMX95_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX95_XCVR_NUM_REGS) {
        return 0;
    }
    sub = rel & 0xf;
    /* SET/CLR/TOG aliases read back as the base register (except AI RDATA). */
    if (rel != XCVR_PHY_AI_RDATA && rel != XCVR_PHY_AI_WDATA &&
        (sub == 4 || sub == 8 || sub == 0xc)) {
        return s->regs[(rel & ~0xf) >> 2];
    }
    return s->regs[rel >> 2];
}

static void xcvr_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMX95XcvrState *s = opaque;
    uint32_t rel, sub, base;

    if (getenv("XCVR_DBG")) {
        fprintf(stderr, "[xcvr] WR off=0x%04x sz=%u val=0x%08x\n",
                (unsigned)offset, size, (uint32_t)value);
    }

    if (offset < IMX95_XCVR_REG_OFF) {
        uint32_t b;

        for (b = 0; b < size && offset + b < IMX95_XCVR_RAM_SIZE; b++) {
            s->ram[offset + b] = (value >> (b * 8)) & 0xff;
        }
        return;
    }
    if (offset == TFDR_FIFO) {
        /* The eDMA writes S32 SPDIF samples here; enqueue + hand to backend. */
        xcvr_tx_push(s, (uint32_t)value);
        xcvr_cap_push(s, (uint32_t)value);
        return;
    }
    if (offset == RFDR_FIFO) {
        return;
    }
    rel = offset - IMX95_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX95_XCVR_NUM_REGS) {
        return;
    }

    /* WDATA/RDATA are distinct registers, not SET/CLR aliases. */
    if (rel == XCVR_PHY_AI_WDATA || rel == XCVR_PHY_AI_RDATA) {
        s->regs[rel >> 2] = value;
        return;
    }

    sub = rel & 0xf;
    base = (rel & ~0xf) >> 2;
    switch (sub) {
    case 4:
        s->regs[base] |= value;     /* SET */
        break;
    case 8:
        s->regs[base] &= ~value;    /* CLR */
        break;
    case 0xc:
        s->regs[base] ^= value;     /* TOG */
        break;
    default:
        s->regs[rel >> 2] = value;
        base = rel >> 2;
        break;
    }

    /* An AI toggle triggers the indirect access. */
    if (base == (XCVR_PHY_AI_CTRL >> 2)) {
        xcvr_ai_complete(s);
    }

    /* EXT_CTRL gates the transmit datapath: start/stop clocking on a change. */
    if (base == (XCVR_EXT_CTRL_REL >> 2)) {
        bool active = xcvr_tx_active(s);

        if (active && !timer_pending(s->tx_timer)) {
            xcvr_voice_set(s, true);
            timer_mod(s->tx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + XCVR_TX_WORD_NS);
        } else if (!active && timer_pending(s->tx_timer)) {
            timer_del(s->tx_timer);
            xcvr_voice_set(s, false);
        }
    }
}

static const MemoryRegionOps xcvr_ops = {
    .read = xcvr_read,
    .write = xcvr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The driver loads firmware into the code RAM with memcpy_toio(), which on
     * arm64 emits 8-byte stores; allow up to 8-byte accesses so those land
     * (the RAM/FIFO paths copy byte-wise by size, and the control registers are
     * only ever touched 4 bytes at a time by the regmap).
     */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void xcvr_reset(DeviceState *dev)
{
    IMX95XcvrState *s = IMX95_XCVR(dev);

    timer_del(s->tx_timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ram, 0, sizeof(s->ram));
    memset(s->ai_sub, 0, sizeof(s->ai_sub));
    /* RM reset column - see the XCVR_*_RESET notes above. VERSION reads 0. */
    s->regs[XCVR_EXT_CTRL_REL >> 2]       = XCVR_EXT_CTRL_RESET;
    s->regs[XCVR_RX_CMDC_CTRL >> 2]       = XCVR_RX_CMDC_CTRL_RESET;
    s->regs[XCVR_RX_DPATH_CTRL >> 2]      = XCVR_RX_DPATH_CTRL_RST;
    s->regs[XCVR_DMAC_PRE_MATCH >> 2]     = XCVR_DMAC_PRE_MATCH_RST;
    s->regs[XCVR_DMAC_DTS_PRE_MATCH >> 2] = XCVR_DMAC_DTS_PRE_RST;
    s->regs[XCVR_HPD_DBNC_CTRL >> 2]      = XCVR_HPD_DBNC_CTRL_RST;
    s->tx_rptr = s->tx_wptr = s->tx_count = 0;
    s->tx_words = 0;
    s->cap_head = s->cap_count = 0;
    xcvr_voice_set(s, false);
}

static void xcvr_realize(DeviceState *dev, Error **errp)
{
    IMX95XcvrState *s = IMX95_XCVR(dev);
    struct audsettings as = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S32,
        .big_endian = false,
    };
    int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &xcvr_ops, s,
                          TYPE_IMX95_XCVR, IMX95_XCVR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (i = 0; i < IMX95_XCVR_NUM_IRQS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[i]);
    }
    qdev_init_gpio_out_named(dev, &s->dma_req, "dma-req", 1);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, xcvr_tx_tick, s);

    /* Best-effort audio backend: -audio captures the SPDIF playback. */
    if (audio_be_check(&s->audio_be, NULL)) {
        s->voice = audio_be_open_out(s->audio_be, NULL, "imx95-xcvr-tx", s,
                                     xcvr_audio_cb, &as);
    }
}

static const VMStateDescription vmstate_xcvr = {
    .name = TYPE_IMX95_XCVR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95XcvrState, IMX95_XCVR_NUM_REGS),
        VMSTATE_UINT8_ARRAY(ram, IMX95XcvrState, IMX95_XCVR_RAM_SIZE),
        VMSTATE_UINT32_ARRAY(ai_sub, IMX95XcvrState, 256),
        VMSTATE_UINT32_ARRAY(tx_fifo, IMX95XcvrState, IMX95_XCVR_FIFO_DEPTH),
        VMSTATE_UINT32(tx_rptr, IMX95XcvrState),
        VMSTATE_UINT32(tx_wptr, IMX95XcvrState),
        VMSTATE_UINT32(tx_count, IMX95XcvrState),
        VMSTATE_UINT64(tx_words, IMX95XcvrState),
        VMSTATE_END_OF_LIST()
    },
};

static void xcvr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xcvr_realize;
    dc->vmsd = &vmstate_xcvr;
    device_class_set_legacy_reset(dc, xcvr_reset);
    dc->desc = "i.MX95 audio transceiver (SPDIF)";
}

static const TypeInfo xcvr_types[] = {
    {
        .name = TYPE_IMX95_XCVR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX95XcvrState),
        .class_init = xcvr_class_init,
    },
};

DEFINE_TYPES(xcvr_types)
