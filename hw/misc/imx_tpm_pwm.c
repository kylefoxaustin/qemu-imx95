/*
 * NXP i.MX TPM-based PWM ("fsl,imx7ulp-pwm")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Timer/PWM Module (TPM) drives the pwm-imx-tpm driver. Its probe reads
 * PARAM[7:0] for the channel count and allocates that many PWM lines, so the
 * old UNIMP stub (PARAM == 0) failed with "failed to add PWM chip" (-EINVAL).
 *
 * This models the registers the driver touches: PARAM (channel count, read-
 * only), GLOBAL (a soft reset), SC (status/control - prescale/clock-mode),
 * CNT (the free-running counter), MOD (the period modulo) and the per-channel
 * CnSC (control: the ELS field enables the channel) / CnV (compare = duty).
 * The driver programs period/duty/enable and reads them back; the model stores
 * them so pwmchip registration and the pwm sysfs/consumer (backlight) paths
 * work. There is no electrical output to drive in emulation.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX_TPM_PWM "imx.tpm-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXTPMPWMState, IMX_TPM_PWM)

#define TPM_PWM_MMIO        0x1000
#define TPM_PWM_CHANNELS    6           /* TPM has up to 6 PWM channels */

#define TPM_PARAM           0x04        /* [7:0] = channel count (RO) */
#define TPM_GLOBAL          0x08        /* bit1 = soft reset          */
#define TPM_SC              0x10        /* status/control             */
#define TPM_CNT             0x14        /* free-running counter       */
#define TPM_MOD             0x18        /* period modulo              */
#define TPM_CnSC(n)         (0x20 + (n) * 0x8)
#define TPM_CnV(n)          (0x24 + (n) * 0x8)

#define TPM_GLOBAL_RST      (1u << 1)

struct IMXTPMPWMState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t sc;
    uint32_t cnt;
    uint32_t mod;
    uint32_t cnsc[TPM_PWM_CHANNELS];
    uint32_t cnv[TPM_PWM_CHANNELS];
};

static void imx_tpm_pwm_do_reset(IMXTPMPWMState *s)
{
    s->sc = 0;
    s->cnt = 0;
    s->mod = 0;
    memset(s->cnsc, 0, sizeof(s->cnsc));
    memset(s->cnv, 0, sizeof(s->cnv));
}

static uint64_t imx_tpm_pwm_read(void *opaque, hwaddr off, unsigned size)
{
    IMXTPMPWMState *s = opaque;
    int ch;

    for (ch = 0; ch < TPM_PWM_CHANNELS; ch++) {
        if (off == TPM_CnSC(ch)) {
            return s->cnsc[ch];
        }
        if (off == TPM_CnV(ch)) {
            return s->cnv[ch];
        }
    }

    switch (off) {
    case TPM_PARAM:
        return TPM_PWM_CHANNELS;     /* [7:0]: channel count */
    case TPM_GLOBAL:
        return 0;
    case TPM_SC:
        return s->sc;
    case TPM_CNT:
        return s->cnt;
    case TPM_MOD:
        return s->mod;
    default:
        return 0;
    }
}

static void imx_tpm_pwm_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    IMXTPMPWMState *s = opaque;
    uint32_t v = val;
    int ch;

    for (ch = 0; ch < TPM_PWM_CHANNELS; ch++) {
        if (off == TPM_CnSC(ch)) {
            s->cnsc[ch] = v;
            return;
        }
        if (off == TPM_CnV(ch)) {
            s->cnv[ch] = v;
            return;
        }
    }

    switch (off) {
    case TPM_GLOBAL:
        if (v & TPM_GLOBAL_RST) {
            imx_tpm_pwm_do_reset(s);
        }
        break;
    case TPM_SC:
        s->sc = v;
        break;
    case TPM_CNT:
        s->cnt = 0;             /* any write clears the counter */
        break;
    case TPM_MOD:
        s->mod = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx_tpm_pwm_ops = {
    .read = imx_tpm_pwm_read,
    .write = imx_tpm_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx_tpm_pwm_reset(DeviceState *dev)
{
    imx_tpm_pwm_do_reset(IMX_TPM_PWM(dev));
}

static void imx_tpm_pwm_init(Object *obj)
{
    IMXTPMPWMState *s = IMX_TPM_PWM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &imx_tpm_pwm_ops, s,
                          TYPE_IMX_TPM_PWM, TPM_PWM_MMIO);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_imx_tpm_pwm = {
    .name = TYPE_IMX_TPM_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sc, IMXTPMPWMState),
        VMSTATE_UINT32(cnt, IMXTPMPWMState),
        VMSTATE_UINT32(mod, IMXTPMPWMState),
        VMSTATE_UINT32_ARRAY(cnsc, IMXTPMPWMState, TPM_PWM_CHANNELS),
        VMSTATE_UINT32_ARRAY(cnv, IMXTPMPWMState, TPM_PWM_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx_tpm_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx_tpm_pwm;
    device_class_set_legacy_reset(dc, imx_tpm_pwm_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX TPM PWM";
}

static const TypeInfo imx_tpm_pwm_info = {
    .name          = TYPE_IMX_TPM_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXTPMPWMState),
    .instance_init = imx_tpm_pwm_init,
    .class_init    = imx_tpm_pwm_class_init,
};

static void imx_tpm_pwm_register_types(void)
{
    type_register_static(&imx_tpm_pwm_info);
}

type_init(imx_tpm_pwm_register_types)
