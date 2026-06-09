/*
 * NXP i.MX 95 MIPI-DSI host (Synopsys DesignWare DSI core)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX95 MIPI-DSI bridge (nxp,imx95-mipi-dsi) wraps the generic
 * dw-mipi-dsi core, which the imx95-mipi-dsi glue drives to attach the DSI
 * panel into the DPU's display pipeline (DPU CRTC -> pixel link -> DSI host ->
 * panel). This is a register-file model that satisfies the three status reads
 * the dw-mipi-dsi core polls during bridge enable + panel command transfer, so
 * the bridge enables and the panel's DSI init command stream completes without
 * the model transporting real pixels (the DPU model does the framebuffer
 * scanout):
 *   - DSI_VERSION (0x00): the core reads the IP version; report 0x31333100
 *     (HWVER_131) so it takes the v1.31 timing path.
 *   - DSI_CMD_PKT_STATUS (0x74): the command/payload FIFO status; report 0 so
 *     the "FIFO not full" / "command done" polls pass instantly (the panel
 *     driver's DCS init sequence completes).
 *   - DSI_PHY_STATUS (0xb0): report PHY_LOCK | PHY_STOP_STATE_CLK_LANE so the
 *     D-PHY lock / clock-lane stop-state polls pass.
 * Every other register reads back what was written.
 */

#include "qemu/osdep.h"
#include "hw/display/imx95_dsi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define DSI_VERSION         0x00
#define DSI_VERSION_VALUE   0x31333100  /* dw-mipi-dsi HWVER_131 */
#define DSI_CMD_PKT_STATUS  0x74
/*
 * Command/payload FIFO idle: all EMPTY bits set, all FULL/BUSY bits clear -
 * GEN_CMD_EMPTY (bit0) | GEN_PLD_W_EMPTY (bit2) | GEN_PLD_R_EMPTY (bit4). The
 * core polls !FULL before a write and EMPTY after, so a command transfer (the
 * panel's DCS init) completes instantly.
 */
#define CMD_PKT_STATUS_IDLE 0x15
#define DSI_PHY_STATUS      0xb0
#define   PHY_LOCK              (1u << 0)
#define   PHY_STOP_STATE_CLK    (1u << 2)

static uint64_t imx95_dsi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX95DsiState *s = opaque;

    switch (offset) {
    case DSI_VERSION:
        return DSI_VERSION_VALUE;
    case DSI_CMD_PKT_STATUS:
        /* FIFOs idle (empty, not full); commands complete immediately. */
        return CMD_PKT_STATUS_IDLE;
    case DSI_PHY_STATUS:
        return PHY_LOCK | PHY_STOP_STATE_CLK;
    default:
        return s->regs[offset / 4];
    }
}

static void imx95_dsi_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX95DsiState *s = opaque;

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx95_dsi_ops = {
    .read = imx95_dsi_read,
    .write = imx95_dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx95_dsi_reset(DeviceState *dev)
{
    IMX95DsiState *s = IMX95_DSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx95_dsi_init(Object *obj)
{
    IMX95DsiState *s = IMX95_DSI(obj);

    memory_region_init_io(&s->iomem, obj, &imx95_dsi_ops, s,
                          TYPE_IMX95_DSI, IMX95_DSI_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_imx95_dsi = {
    .name = TYPE_IMX95_DSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX95DsiState, IMX95_DSI_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_dsi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 95 MIPI-DSI host (DesignWare)";
    device_class_set_legacy_reset(dc, imx95_dsi_reset);
    dc->vmsd = &vmstate_imx95_dsi;
}

static const TypeInfo imx95_dsi_types[] = {
    {
        .name           = TYPE_IMX95_DSI,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX95DsiState),
        .instance_init  = imx95_dsi_init,
        .class_init     = imx95_dsi_class_init,
    },
};

DEFINE_TYPES(imx95_dsi_types)
