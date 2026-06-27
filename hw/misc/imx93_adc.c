/*
 * NXP i.MX 93/95 ADC ("nxp,imx93-adc")
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The imx93_adc iio driver expects a small state machine, not a flat stub:
 *  - power down / up: it sets MCR.PWDN and polls MSR.ADCSTATUS for POWER_DOWN,
 *    so the UNIMP stub (MSR == 0 = IDLE) looped on "ADC do not in power down
 *    mode". Here MSR reflects PWDN.
 *  - calibration: it sets MCR.CALSTART and polls MSR.CALBUSY to clear, then
 *    checks CALFAIL. We complete instantly (CALBUSY/CALFAIL stay 0).
 *  - conversion: it programs NCMR0 (channel mask), sets MCR.NSTART, waits for
 *    the end-of-conversion interrupt, then reads PCDRn for the sample. On
 *    NSTART we latch ISR.EOC|ECH and raise the IRQ; PCDRn returns the
 *    per-channel conversion value.
 *
 * Fidelity: there is no analog pin to sample in emulation, so the conversion
 * value comes from the operator - each channel is a read/write QOM property
 * "adc-ch0".."adc-ch7" settable at runtime via QMP qom-set (the model's analog
 * of the board's pin voltage). Whatever the operator injects is what the guest
 * reads back, so ADC-consuming code sees a faithful, controllable datapath
 * rather than a hidden constant. The default is a distinct-per-channel test
 * pattern (0x100 + ch*0x111) so an un-driven channel is still deterministic.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX93_ADC "imx93.adc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93ADCState, IMX93_ADC)

#define IMX93_ADC_MMIO      0x10000
#define IMX93_ADC_NUM_IRQ   3
#define IMX93_ADC_CHANNELS  8

#define ADC_MCR             0x00
#define ADC_MSR             0x04
#define ADC_ISR             0x10
#define ADC_IMR             0x20
#define ADC_CIMR0           0x24
#define ADC_CTR0            0x94
#define ADC_NCMR0           0xa4
#define ADC_PCDR0           0x100   /* .. PCDR7 at 0x11c */
#define ADC_CALSTAT         0x39c

#define MCR_PWDN            (1u << 0)
#define MCR_CALSTART        (1u << 14)
#define MCR_NSTART          (1u << 24)

#define MSR_ADCSTATUS_IDLE        0
#define MSR_ADCSTATUS_POWER_DOWN  1
/* CALBUSY (bit29) / CALFAIL (bit30) left 0: calibration always succeeds. */

#define ISR_ECH             (1u << 0)
#define ISR_EOC             (1u << 1)
#define ISR_EOC_ECH         (ISR_EOC | ISR_ECH)

struct IMX93ADCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t mcr;
    uint32_t isr;
    uint32_t imr;
    uint32_t cimr0;
    uint32_t ctr0;
    uint32_t ncmr0;

    /* Per-channel conversion value (operator-settable; see file header). */
    uint32_t chval[IMX93_ADC_CHANNELS];

    qemu_irq irq[IMX93_ADC_NUM_IRQ];
};

static void imx93_adc_update_irq(IMX93ADCState *s)
{
    /* The imx93_adc driver requests interrupt index 2 (the EOC line). */
    qemu_set_irq(s->irq[2], (s->isr & s->imr & ISR_EOC_ECH) != 0);
}

/*
 * Default per-channel test pattern when the operator hasn't set a value;
 * distinct per channel so an un-driven channel is still deterministic.
 */
static uint32_t imx93_adc_default(int ch)
{
    return (0x100 + ch * 0x111) & 0xfff;
}

static uint64_t imx93_adc_read(void *opaque, hwaddr off, unsigned size)
{
    IMX93ADCState *s = opaque;
    int ch;

    if (off >= ADC_PCDR0 && off < ADC_PCDR0 + IMX93_ADC_CHANNELS * 4) {
        ch = (off - ADC_PCDR0) / 4;
        return s->chval[ch] & 0xfff;        /* operator-injected conversion */
    }

    switch (off) {
    case ADC_MCR:
        return s->mcr;
    case ADC_MSR:
        return (s->mcr & MCR_PWDN) ? MSR_ADCSTATUS_POWER_DOWN
                                   : MSR_ADCSTATUS_IDLE;
    case ADC_ISR:
        return s->isr;
    case ADC_IMR:
        return s->imr;
    case ADC_CIMR0:
        return s->cimr0;
    case ADC_CTR0:
        return s->ctr0;
    case ADC_NCMR0:
        return s->ncmr0;
    case ADC_CALSTAT:
        return 0;                   /* calibration passed */
    default:
        return 0;
    }
}

static void imx93_adc_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    IMX93ADCState *s = opaque;
    uint32_t v = val;

    switch (off) {
    case ADC_MCR:
        s->mcr = v & ~(MCR_NSTART | MCR_CALSTART);  /* both self-clear */
        if (v & MCR_NSTART) {
            /* End-of-conversion / end-of-chain for the requested scan. */
            s->isr |= ISR_EOC_ECH;
            imx93_adc_update_irq(s);
        }
        /* CALSTART completes instantly (MSR.CALBUSY stays 0). */
        break;
    case ADC_ISR:
        s->isr &= ~v;               /* write 1 to clear */
        imx93_adc_update_irq(s);
        break;
    case ADC_IMR:
        s->imr = v;
        imx93_adc_update_irq(s);
        break;
    case ADC_CIMR0:
        s->cimr0 = v;
        break;
    case ADC_CTR0:
        s->ctr0 = v;
        break;
    case ADC_NCMR0:
        s->ncmr0 = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps imx93_adc_ops = {
    .read = imx93_adc_read,
    .write = imx93_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_adc_reset(DeviceState *dev)
{
    IMX93ADCState *s = IMX93_ADC(dev);
    int ch;

    s->mcr = MCR_PWDN;              /* powers up in power-down, like the HW */
    s->isr = 0;
    s->imr = 0;
    s->cimr0 = 0;
    s->ctr0 = 0;
    s->ncmr0 = 0;
    for (ch = 0; ch < IMX93_ADC_CHANNELS; ch++) {
        s->chval[ch] = imx93_adc_default(ch);
    }
    imx93_adc_update_irq(s);
}

static void imx93_adc_init(Object *obj)
{
    IMX93ADCState *s = IMX93_ADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &imx93_adc_ops, s,
                          TYPE_IMX93_ADC, IMX93_ADC_MMIO);
    sysbus_init_mmio(sbd, &s->iomem);
    for (i = 0; i < IMX93_ADC_NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    /*
     * Per-channel conversion value, settable at runtime via QMP qom-set
     * (e.g. qom-set <path> adc-ch3 0x555) - the operator drives the "voltage".
     */
    for (i = 0; i < IMX93_ADC_CHANNELS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "adc-ch%d", i);
        object_property_add_uint32_ptr(obj, name, &s->chval[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static const VMStateDescription vmstate_imx93_adc = {
    .name = TYPE_IMX93_ADC,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMX93ADCState),
        VMSTATE_UINT32(isr, IMX93ADCState),
        VMSTATE_UINT32(imr, IMX93ADCState),
        VMSTATE_UINT32(cimr0, IMX93ADCState),
        VMSTATE_UINT32(ctr0, IMX93ADCState),
        VMSTATE_UINT32(ncmr0, IMX93ADCState),
        VMSTATE_UINT32_ARRAY_V(chval, IMX93ADCState, IMX93_ADC_CHANNELS, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx93_adc;
    device_class_set_legacy_reset(dc, imx93_adc_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "NXP i.MX93 ADC";
}

static const TypeInfo imx93_adc_info = {
    .name          = TYPE_IMX93_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX93ADCState),
    .instance_init = imx93_adc_init,
    .class_init    = imx93_adc_class_init,
};

static void imx93_adc_register_types(void)
{
    type_register_static(&imx93_adc_info);
}

type_init(imx93_adc_register_types)
