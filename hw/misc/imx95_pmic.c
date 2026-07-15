/*
 * Minimal I2C device models for the i.MX 95 EVK System Manager:
 *   - PF09 (PF0900) PMIC          (TYPE_PF09_PMIC,  addr 0x08, CRC'd)
 *   - PCAL6408A 8-bit IO expander (TYPE_PCAL6408A,  addr 0x20)
 *   - PF53 buck regulator         (TYPE_PF53_PMIC,  addr 0x2a/0x29)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The NXP SM firmware probes/configures both over LPI2C1 during board
 * init (BRD_SM_SerialDevicesInit); a failed transfer aborts SM init.
 * These are register-file models: writes store, reads return the stored
 * byte. The PF09 driver protects each transfer with a J1850 CRC, so the
 * PF09 read path appends the CRC the driver expects
 * (CRC_J1850((devAddr<<1)|1, reg, data)); without it PF09_Init fails.
 * No actual PMIC/regulator or GPIO behaviour is modelled.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"

/* ---- J1850 CRC (matches imx-sm components/crc/crc.c CRC_J1850) ---- */
#define CRC_J1850_POLY  0x1Du

static uint8_t crc_j1850(const uint8_t *p, unsigned n)
{
    uint32_t crc = 0xffu;

    for (unsigned i = 0; i < n; i++) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? ((crc << 1) ^ CRC_J1850_POLY)
                                : ((crc << 1) & 0xffu);
        }
    }
    return (uint8_t)crc;
}

/* ============================ PF09 PMIC ============================ */

#define TYPE_PF09_PMIC "pf09-pmic"
OBJECT_DECLARE_SIMPLE_TYPE(PF09State, PF09_PMIC)

#define PF09_NUM_REG    256

struct PF09State {
    I2CSlave    parent_obj;

    uint8_t     regs[PF09_NUM_REG];
    uint8_t     cur_reg;
    uint32_t    wcount;     /* bytes received since START(write) */
    uint32_t    rcount;     /* bytes returned since START(read) */
};

static int pf09_event(I2CSlave *i2c, enum i2c_event event)
{
    PF09State *s = PF09_PMIC(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->wcount = 0;
        break;
    case I2C_START_RECV:
        s->rcount = 0;
        break;
    default:
        break;
    }
    return 0;   /* ACK */
}

static int pf09_send(I2CSlave *i2c, uint8_t data)
{
    PF09State *s = PF09_PMIC(i2c);

    if (s->wcount == 0) {
        s->cur_reg = data;          /* register address */
    } else if (s->wcount == 1) {
        s->regs[s->cur_reg] = data; /* data byte */
    }
    /* wcount >= 2 is the trailing CRC byte: accept and ignore. */
    s->wcount++;
    return 0;   /* ACK */
}

static uint8_t pf09_recv(I2CSlave *i2c)
{
    PF09State *s = PF09_PMIC(i2c);
    uint8_t val;

    if (s->rcount == 0) {
        val = s->regs[s->cur_reg];
    } else {
        /* CRC over (devAddr<<1)|1, reg, data - what PF09_PmicRead checks. */
        uint8_t buf[3] = {
            (uint8_t)((i2c->address << 1) | 1u),
            s->cur_reg,
            s->regs[s->cur_reg],
        };
        val = crc_j1850(buf, 3);
    }
    s->rcount++;
    return val;
}

/*
 * OFF-SoC RESET-VALUE NOTE (audited 2026-07-14; do NOT relabel this "clean").
 *
 * This memset-to-0 is SCAFFOLD, not the chip's power-on state. A real PF09
 * comes up with per-rail OTP defaults in its SW*_VRUN / LDO*_RUN registers; we
 * zero them and seed only REV_ID so PF09_Init proceeds. That is the class
 * 93/91emulator found in their PCA9451A - and the honest move their RM-golden
 * audits skipped is to write the analysis here, not let "the RM doesn't
 * describe it" masquerade as "checked clean".
 *
 * Severity measured from the consumer, NOT inherited from the sibling boards:
 *   - On i.MX95 the PMIC is owned by the M33 System Manager, not read directly
 *     by Linux (contrast 91/93, whose pca9450 driver reads OTP straight).
 *   - The only voltage domain the SM exposes to the AP is DEV_SM_VOLT_ARM = SW1
 *     (configs/mx95evk/config_lmm.h). The SM SETS SW1 to BOARD_VOLT_ARM before
 *     the AP runs (the A55 cannot execute without its rail set), so the
 *     AP-facing BRD_SM_VoltageLevelGet -> PF09_VoltageGet(SW1) reads back the
 *     WRITTEN value, never our 0. No fabricated voltage reaches Linux.
 *   - Rails the SM manages are written before read; rails neither written nor
 *     AP-read are not consumer-touched (our rule), so their 0 is invisible.
 *     The machine boots to userspace on the real SM - the reachable paths
 *     tolerate this.
 *
 * So this is NOT a silent-wrong-to-Linux bug, and it is left as labelled
 * scaffold rather than "fixed" with a GUESS: no PF09 datasheet is in 95_docs/
 * (RM only), and an unsourced OTP byte would read as measured - the exact
 * laundering audited against all week. The sourceable path, if a future
 * consumer makes a rail reachable, is 93's method: target voltages from the
 * board DT + vsel encoding from mainline pf0900-regulator.c - a DERIVED value,
 * labelled. A belt-and-suspenders SCMI VOLTAGE_LEVEL_GET check on the ARM
 * domain is deferred to board time, stated rather than claimed.
 */
static void pf09_reset(DeviceState *dev)
{
    PF09State *s = PF09_PMIC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cur_reg = 0;
    s->wcount = 0;
    s->rcount = 0;
    /* REV_ID >= 0x20 lets PF09_Init skip the STANDBY-monitor write. */
    s->regs[0x00] = 0x20;
}

static const VMStateDescription vmstate_pf09 = {
    .name = TYPE_PF09_PMIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PF09State),
        VMSTATE_UINT8_ARRAY(regs, PF09State, PF09_NUM_REG),
        VMSTATE_UINT8(cur_reg, PF09State),
        VMSTATE_UINT32(wcount, PF09State),
        VMSTATE_UINT32(rcount, PF09State),
        VMSTATE_END_OF_LIST()
    },
};

static void pf09_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pf09_event;
    k->recv = pf09_recv;
    k->send = pf09_send;
    dc->vmsd = &vmstate_pf09;
    device_class_set_legacy_reset(dc, pf09_reset);
    dc->desc = "NXP PF09 PMIC (i.MX95 SM stub)";
}

static const TypeInfo pf09_info = {
    .name          = TYPE_PF09_PMIC,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PF09State),
    .class_init    = pf09_class_init,
};

/* ========================== PCAL6408A ============================= */

#define TYPE_PCAL6408A "pcal6408a"
OBJECT_DECLARE_SIMPLE_TYPE(PCAL6408AState, PCAL6408A)

#define PCAL6408A_NUM_REG   256

struct PCAL6408AState {
    I2CSlave    parent_obj;

    uint8_t     regs[PCAL6408A_NUM_REG];
    uint8_t     cur_reg;
    uint32_t    wcount;
};

static int pcal6408a_event(I2CSlave *i2c, enum i2c_event event)
{
    PCAL6408AState *s = PCAL6408A(i2c);

    if (event == I2C_START_SEND) {
        s->wcount = 0;
    }
    return 0;
}

static int pcal6408a_send(I2CSlave *i2c, uint8_t data)
{
    PCAL6408AState *s = PCAL6408A(i2c);

    if (s->wcount == 0) {
        s->cur_reg = data;
    } else {
        s->regs[s->cur_reg++] = data;
    }
    s->wcount++;
    return 0;
}

static uint8_t pcal6408a_recv(I2CSlave *i2c)
{
    PCAL6408AState *s = PCAL6408A(i2c);

    return s->regs[s->cur_reg++];
}

static void pcal6408a_reset(DeviceState *dev)
{
    PCAL6408AState *s = PCAL6408A(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cur_reg = 0;
    s->wcount = 0;
}

static const VMStateDescription vmstate_pcal6408a = {
    .name = TYPE_PCAL6408A,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCAL6408AState),
        VMSTATE_UINT8_ARRAY(regs, PCAL6408AState, PCAL6408A_NUM_REG),
        VMSTATE_UINT8(cur_reg, PCAL6408AState),
        VMSTATE_UINT32(wcount, PCAL6408AState),
        VMSTATE_END_OF_LIST()
    },
};

static void pcal6408a_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcal6408a_event;
    k->recv = pcal6408a_recv;
    k->send = pcal6408a_send;
    dc->vmsd = &vmstate_pcal6408a;
    device_class_set_legacy_reset(dc, pcal6408a_reset);
    dc->desc = "NXP PCAL6408A IO expander (i.MX95 SM stub)";
}

static const TypeInfo pcal6408a_info = {
    .name          = TYPE_PCAL6408A,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PCAL6408AState),
    .class_init    = pcal6408a_class_init,
};

/*
 * PCAL6524 - the 24-bit (3-bank) sibling of the PCAL6408A. The register-file
 * model above is bank-agnostic (a flat 256-entry array with a register pointer
 * and auto-increment), so the wider part needs no new logic: it inherits the
 * PCAL6408A class wholesale. On the 19x19 EVK this expander sits on lpi2c7 at
 * 0x22; one of its GPIOs (port 0, line 3) is the enable for the USB host's
 * 5V VBUS fixed-regulator, so the usb2 ChipIdea host can't bind until the
 * gpio-pca953x driver probes this device and the regulator turns on.
 */
#define TYPE_PCAL6524 "pcal6524"

static const TypeInfo pcal6524_info = {
    .name          = TYPE_PCAL6524,
    .parent        = TYPE_PCAL6408A,
};

/*
 * ADP5585 - ADI GPIO/PWM IO-expander MFD (i2c-1 at 0x34 on the 19x19 EVK). The
 * adp5585 driver reads the ID register (0x00) and rejects the device unless its
 * manufacturer-id nibble (bits [7:4]) is 0x2, so the register file is the same
 * as the PCAL6408A but with that ID byte seeded; the gpio/pwm sub-devices the
 * MFD then adds just drive the remaining registers.
 */
#define TYPE_ADP5585 "adp5585"

#define ADP5585_ID_REG          0x00
#define ADP5585_MAN_ID_VALUE    0x20   /* bits [7:4] = 0x2 */

static void adp5585_reset(DeviceState *dev)
{
    PCAL6408AState *s = PCAL6408A(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[ADP5585_ID_REG] = ADP5585_MAN_ID_VALUE;
    s->cur_reg = 0;
    s->wcount = 0;
}

static void adp5585_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, adp5585_reset);
    dc->desc = "ADI ADP5585 GPIO/PWM expander";
}

static const TypeInfo adp5585_info = {
    .name          = TYPE_ADP5585,
    .parent        = TYPE_PCAL6408A,
    .class_init    = adp5585_class_init,
};

/* ============================ PF53 PMIC ============================ */

/*
 * PF5301 / PF5302 (PF53-family) buck regulators on the same LPI2C bus.
 * BRD_SM_SerialDevicesInit calls PF53_Init() on each, which only reads
 * the DEV_ID register and checks that the transfer ACKs (it does not
 * validate the value, and the SM configures crcEn=false for these). So a
 * plain register file that ACKs is sufficient to get past SM init.
 */
#define TYPE_PF53_PMIC "pf53-pmic"
OBJECT_DECLARE_SIMPLE_TYPE(PF53State, PF53_PMIC)

#define PF53_NUM_REG    256

struct PF53State {
    I2CSlave    parent_obj;

    uint8_t     regs[PF53_NUM_REG];
    uint8_t     cur_reg;
    uint32_t    wcount;
};

static int pf53_event(I2CSlave *i2c, enum i2c_event event)
{
    PF53State *s = PF53_PMIC(i2c);

    if (event == I2C_START_SEND) {
        s->wcount = 0;
    }
    return 0;
}

static int pf53_send(I2CSlave *i2c, uint8_t data)
{
    PF53State *s = PF53_PMIC(i2c);

    if (s->wcount == 0) {
        s->cur_reg = data;
    } else {
        s->regs[s->cur_reg++] = data;
    }
    s->wcount++;
    return 0;
}

static uint8_t pf53_recv(I2CSlave *i2c)
{
    PF53State *s = PF53_PMIC(i2c);

    return s->regs[s->cur_reg++];
}

/*
 * Same off-SoC scaffold class and same measured-down severity as pf09_reset()
 * above - see that comment. The PF53 (SW1) is the SM's own DVFS buck for the
 * ARM/SoC rail; the SM programs it via PF53_VoltageSet before use and tracks
 * the level in software (brd_sm_voltage.c s_levelArm), so our zeroed VOUT is
 * written before any consumer reads it. Left as labelled scaffold, not seeded
 * with a guessed OTP byte (no PF53 datasheet in 95_docs/).
 */
static void pf53_reset(DeviceState *dev)
{
    PF53State *s = PF53_PMIC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cur_reg = 0;
    s->wcount = 0;
}

static const VMStateDescription vmstate_pf53 = {
    .name = TYPE_PF53_PMIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PF53State),
        VMSTATE_UINT8_ARRAY(regs, PF53State, PF53_NUM_REG),
        VMSTATE_UINT8(cur_reg, PF53State),
        VMSTATE_UINT32(wcount, PF53State),
        VMSTATE_END_OF_LIST()
    },
};

static void pf53_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pf53_event;
    k->recv = pf53_recv;
    k->send = pf53_send;
    dc->vmsd = &vmstate_pf53;
    device_class_set_legacy_reset(dc, pf53_reset);
    dc->desc = "NXP PF53 PMIC (i.MX95 SM stub)";
}

static const TypeInfo pf53_info = {
    .name          = TYPE_PF53_PMIC,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PF53State),
    .class_init    = pf53_class_init,
};

static void imx95_pmic_register_types(void)
{
    type_register_static(&pf09_info);
    type_register_static(&pcal6408a_info);
    type_register_static(&pcal6524_info);
    type_register_static(&adp5585_info);
    type_register_static(&pf53_info);
}

type_init(imx95_pmic_register_types)
