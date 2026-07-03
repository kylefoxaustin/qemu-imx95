/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * spi-link - an SSI peripheral that bridges an SPI bus to a QEMU chardev, so
 * two emulator instances can pass real data over an SPI board-to-board link.
 * Attach one to each instance's named LPSPI bus and connect the two chardevs
 * with a socket:
 *
 *   -chardev socket,id=spil,... -device spi-link,bus=lpspi1,chardev=spil
 *
 * On every SPI transfer the master shifts out a byte (MOSI): we forward it to
 * the chardev (-> the peer instance). The byte shifted in (MISO) is taken from
 * an rx FIFO fed by the peer over the chardev, or 0xff (idle-high) when none is
 * queued. So each direction is an independent, FIFO-buffered byte stream - one
 * side's master writes, the other's master clocks the bytes in. This models the
 * data path of a board-to-board SPI link (not cycle-accurate clock duplex).
 */
#include "qemu/osdep.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SPI_LINK "spi-link"
OBJECT_DECLARE_SIMPLE_TYPE(SpiLinkState, SPI_LINK)

#define SPI_LINK_FIFO_DEPTH 256

struct SpiLinkState {
    SSIPeripheral parent_obj;

    CharFrontend chr;
    Fifo8 rx;               /* bytes shifted in from the peer (MISO) */
};

static uint32_t spi_link_transfer(SSIPeripheral *dev, uint32_t val)
{
    SpiLinkState *s = SPI_LINK(dev);
    uint8_t out = val & 0xff;
    uint8_t in = 0xff;      /* MISO idle-high when the peer sent nothing */

    /* MOSI byte -> the peer instance over the chardev socket. */
    qemu_chr_fe_write_all(&s->chr, &out, 1);

    if (!fifo8_is_empty(&s->rx)) {
        in = fifo8_pop(&s->rx);
    }
    return in;
}

static int spi_link_can_receive(void *opaque)
{
    SpiLinkState *s = opaque;

    return fifo8_num_free(&s->rx);
}

static void spi_link_receive(void *opaque, const uint8_t *buf, int size)
{
    SpiLinkState *s = opaque;
    int i;

    for (i = 0; i < size && !fifo8_is_full(&s->rx); i++) {
        fifo8_push(&s->rx, buf[i]);
    }
}

static void spi_link_realize(SSIPeripheral *dev, Error **errp)
{
    SpiLinkState *s = SPI_LINK(dev);

    fifo8_create(&s->rx, SPI_LINK_FIFO_DEPTH);
    qemu_chr_fe_set_handlers(&s->chr, spi_link_can_receive, spi_link_receive,
                             NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_spi_link = {
    .name = "spi-link",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, SpiLinkState),
        VMSTATE_FIFO8(rx, SpiLinkState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property spi_link_props[] = {
    DEFINE_PROP_CHR("chardev", SpiLinkState, chr),
};

static void spi_link_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = spi_link_realize;
    k->transfer = spi_link_transfer;
    dc->desc = "SPI board-to-board link (SSI peripheral <-> chardev)";
    dc->vmsd = &vmstate_spi_link;
    device_class_set_props(dc, spi_link_props);
}

static const TypeInfo spi_link_info = {
    .name          = TYPE_SPI_LINK,
    .parent        = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SpiLinkState),
    .class_init    = spi_link_class_init,
};

static void spi_link_register_types(void)
{
    type_register_static(&spi_link_info);
}

type_init(spi_link_register_types)
