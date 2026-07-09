/*
 * NXP NETC Timer (IEEE 1588 PTP clock) - PCI function 1131:ee02
 *
 * The i.MX 95 NETC block exposes its PTP hardware clock as its own PCI
 * function on the NETC integrated-ECAM bus, alongside the ENETC PFs and the
 * central EMDIO. Linux binds it with drivers/ptp/ptp_netc.c and registers a
 * /dev/ptpN PHC.
 *
 * The register file is almost entirely write-only from the driver's point of
 * view: netc_timer_init() programs CK_SEL/TE, the tick period, the prescaler
 * and the FIPER controls, and nothing except the revision register is read
 * back for a decision. What matters for fidelity is the one thing the driver
 * *does* read continuously:
 *
 *   gettimex64() reads TMR_CUR_TIME_L/H (0xf0/0xf4) - NOT the TMR_CNT
 *   registers it wrote. On real silicon CUR_TIME = free-running CNT + OFF.
 *
 * So a backing-store register file, where CUR_TIME reads back whatever was
 * last written, would hand the guest a *frozen* timestamp: clock_gettime()
 * would succeed and return a plausible, constant, wrong time. That is a silent
 * wrong answer - the worst failure class - so CUR_TIME is computed on read
 * from a QEMU clock instead. Reads are cheap and no periodic timer is needed.
 *
 * Not modelled: PPS / periodic output (FIPER), external timestamp capture
 * (ETTS) and the MSI-X event interrupt. Those degrade honestly - the driver
 * programs registers that do nothing and no event ever fires. In particular
 * TMR_STAT.ETSx_VLD is left clear: netc_timer_handle_etts_event() spins while
 * that bit is set, so faking it would hang the guest.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX95_NETC_TIMER "imx95-netc-timer"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95NetcTimer, IMX95_NETC_TIMER)

#define TIMER_VENDOR_ID     0x1131      /* PCI_VENDOR_ID_NXP2 (Philips) */
#define TIMER_DEVICE_ID     0xee02      /* PCI_DEVICE_ID_NXP2_NETC_TIMER */

/* BAR0 is a 128 KiB window; the NETC global sub-block lives at +0x10000. */
#define TIMER_BAR0_SIZE     0x20000

#define TIMER_MSIX_VECTORS   1
#define TIMER_MSIX_BAR       2
#define TIMER_MSIX_TABLE_OFF 0x0
#define TIMER_MSIX_PBA_OFF   0x1000
#define TIMER_MSIX_BAR_SIZE  0x2000

/* Register offsets the driver touches (ptp_netc.c). */
#define TMR_CTRL            0x0080
#define TMR_TEVENT          0x0084
#define TMR_TEMASK          0x0088
#define TMR_STAT            0x0094
#define TMR_CNT_L           0x0098
#define TMR_CNT_H           0x009c
#define TMR_ADD             0x00a0
#define TMR_PRSC            0x00a8
#define TMR_ECTRL           0x00ac
#define TMR_OFF_L           0x00b0
#define TMR_OFF_H           0x00b4
#define TMR_CUR_TIME_L      0x00f0
#define TMR_CUR_TIME_H      0x00f4

/* NETC global block inside BAR0: IP block revision register. */
#define NETC_GLOBAL_IPBRR0  0x10bf8
#define IPBRR0_IP_REV_MASK  0xffff
/*
 * Real i.MX 95 NETC is IP revision 4.1. ptp_netc only uses this to pick
 * alarm_num (1 for 4.1, else 2); it is not a probe gate either way.
 */
#define NETC_IP_REV_4_1     0x0401

struct IMX95NetcTimer {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    MemoryRegion msix_bar;

    /* Backing store for the write-mostly registers (RMW must read back). */
    uint32_t regs[TIMER_BAR0_SIZE / 4];

    /*
     * Free-running clock state. cnt_base is the counter value the driver last
     * wrote (ns); t_base is the host clock at that moment. CUR_TIME is derived
     * as cnt_base + (now - t_base) + off, so the PHC advances at ~1 ns/ns.
     */
    uint64_t cnt_base;
    uint64_t t_base;

    /* Latched on a CUR_TIME_L / CNT_L read so the H read pairs with it. */
    uint32_t cur_time_hi_latch;
    uint32_t cnt_hi_latch;
};

static inline uint64_t timer_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static inline uint32_t reg_r(IMX95NetcTimer *s, hwaddr off)
{
    return s->regs[off >> 2];
}

static inline void reg_w(IMX95NetcTimer *s, hwaddr off, uint32_t v)
{
    s->regs[off >> 2] = v;
}

static uint64_t timer_offset(IMX95NetcTimer *s)
{
    return ((uint64_t)reg_r(s, TMR_OFF_H) << 32) | reg_r(s, TMR_OFF_L);
}

/* The free-running counter, in nanoseconds. */
static uint64_t timer_count(IMX95NetcTimer *s)
{
    return s->cnt_base + (timer_now() - s->t_base);
}

static void timer_set_count(IMX95NetcTimer *s, uint64_t cnt)
{
    s->cnt_base = cnt;
    s->t_base = timer_now();
}

static uint64_t timer_bar0_read(void *opaque, hwaddr off, unsigned size)
{
    IMX95NetcTimer *s = opaque;
    uint64_t v;

    switch (off) {
    /*
     * 64-bit values are two 32-bit accesses, low word first (netc_global.h).
     * Latch the high word when the low word is read so the pair is coherent
     * even though the counter is still advancing underneath.
     */
    case TMR_CUR_TIME_L:
        v = timer_count(s) + timer_offset(s);
        s->cur_time_hi_latch = v >> 32;
        return (uint32_t)v;
    case TMR_CUR_TIME_H:
        return s->cur_time_hi_latch;

    case TMR_CNT_L:
        v = timer_count(s);
        s->cnt_hi_latch = v >> 32;
        return (uint32_t)v;
    case TMR_CNT_H:
        return s->cnt_hi_latch;

    /*
     * No external-timestamp capture is modelled, so ETS1_VLD/ETS2_VLD must
     * stay clear: netc_timer_handle_etts_event() spins while they are set.
     */
    case TMR_STAT:
        return 0;

    case NETC_GLOBAL_IPBRR0:
        return NETC_IP_REV_4_1;

    default:
        if (off + 4 <= TIMER_BAR0_SIZE) {
            return reg_r(s, off);
        }
        return 0;
    }
}

static void timer_bar0_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    IMX95NetcTimer *s = opaque;

    if (off + 4 > TIMER_BAR0_SIZE) {
        return;
    }

    switch (off) {
    case TMR_CNT_L:
        /* settime64()/init seed the counter low word first, then the high. */
        timer_set_count(s, (timer_count(s) & ~0xffffffffULL) | (uint32_t)val);
        reg_w(s, TMR_CNT_L, val);
        return;
    case TMR_CNT_H:
        timer_set_count(s, ((uint64_t)val << 32) |
                        (uint32_t)timer_count(s));
        reg_w(s, TMR_CNT_H, val);
        return;

    case TMR_TEVENT:
        /* Write-1-to-clear: the driver writes back the bits it read. */
        reg_w(s, TMR_TEVENT, reg_r(s, TMR_TEVENT) & ~(uint32_t)val);
        return;

    case NETC_GLOBAL_IPBRR0:
        return;                        /* read-only */

    default:
        reg_w(s, off, val);
        return;
    }
}

static const MemoryRegionOps timer_bar0_ops = {
    .read = timer_bar0_read,
    .write = timer_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void timer_reset_hold(Object *obj, ResetType type)
{
    IMX95NetcTimer *s = IMX95_NETC_TIMER(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->cnt_base = 0;
    s->t_base = timer_now();
    s->cur_time_hi_latch = 0;
    s->cnt_hi_latch = 0;
}

static void timer_realize(PCIDevice *pci_dev, Error **errp)
{
    IMX95NetcTimer *s = IMX95_NETC_TIMER(pci_dev);
    uint8_t *cfg = pci_dev->config;
    int ret;

    pci_set_word(cfg + PCI_VENDOR_ID, TIMER_VENDOR_ID);
    pci_set_word(cfg + PCI_DEVICE_ID, TIMER_DEVICE_ID);
    pci_set_word(cfg + PCI_CLASS_DEVICE, PCI_CLASS_SYSTEM_OTHER);
    pci_config_set_prog_interface(cfg, 0x80);
    pci_config_set_interrupt_pin(cfg, 0);

    memory_region_init_io(&s->bar0, OBJECT(s), &timer_bar0_ops, s,
                          "netc-timer-bar0", TIMER_BAR0_SIZE);
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar0);

    memory_region_init(&s->msix_bar, OBJECT(s), "netc-timer-msix",
                       TIMER_MSIX_BAR_SIZE);
    pci_register_bar(pci_dev, TIMER_MSIX_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->msix_bar);

    /*
     * ptp_netc requires exactly one MSI-X vector (pci_alloc_irq_vectors(1,1)
     * -> -EPERM otherwise), so the capability must be present even though no
     * event source is modelled and the vector is never notified.
     */
    ret = msix_init(pci_dev, TIMER_MSIX_VECTORS,
                    &s->msix_bar, TIMER_MSIX_BAR, TIMER_MSIX_TABLE_OFF,
                    &s->msix_bar, TIMER_MSIX_BAR, TIMER_MSIX_PBA_OFF,
                    0, errp);
    if (ret) {
        return;
    }
    msix_vector_use(pci_dev, 0);
}

static void timer_exit(PCIDevice *pci_dev)
{
    IMX95NetcTimer *s = IMX95_NETC_TIMER(pci_dev);

    msix_unuse_all_vectors(pci_dev);
    msix_uninit(pci_dev, &s->msix_bar, &s->msix_bar);
}

static const VMStateDescription vmstate_netc_timer = {
    .name = "imx95-netc-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, IMX95NetcTimer),
        VMSTATE_UINT32_ARRAY(regs, IMX95NetcTimer, TIMER_BAR0_SIZE / 4),
        VMSTATE_UINT64(cnt_base, IMX95NetcTimer),
        VMSTATE_UINT64(t_base, IMX95NetcTimer),
        VMSTATE_UINT32(cur_time_hi_latch, IMX95NetcTimer),
        VMSTATE_UINT32(cnt_hi_latch, IMX95NetcTimer),
        VMSTATE_END_OF_LIST()
    }
};

static void timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = timer_realize;
    k->exit = timer_exit;
    k->vendor_id = TIMER_VENDOR_ID;
    k->device_id = TIMER_DEVICE_ID;
    k->class_id = PCI_CLASS_SYSTEM_OTHER;
    k->revision = 4;
    rc->phases.hold = timer_reset_hold;
    dc->desc = "i.MX 95 NETC Timer (IEEE 1588 PTP clock)";
    dc->vmsd = &vmstate_netc_timer;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo netc_timer_info = {
    .name = TYPE_IMX95_NETC_TIMER,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IMX95NetcTimer),
    .class_init = timer_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void netc_timer_register_types(void)
{
    type_register_static(&netc_timer_info);
}

type_init(netc_timer_register_types)
