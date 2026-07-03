/*
 * CAN host connection over a QEMU chardev (board-to-board CAN link)
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A CanHost backend that bridges an emulated CAN bus to a QEMU chardev instead
 * of a host SocketCAN interface, so two QEMU instances can be joined into one
 * CAN segment over a socket with no host-kernel (vcan) dependency - the same
 * chardev-socket shape the ethernet/UART/SPI board-to-board links use:
 *
 *   -object can-bus,id=canbus0
 *   -machine canbus0=canbus0                 # a FlexCAN on the bus
 *   -chardev socket,id=canlink,path=SOCK,server=on|off,...
 *   -object can-host-chardev,id=h0,canbus=canbus0,chardev=canlink
 *
 * The wire format is the raw qemu_can_frame (both ends are the same QEMU
 * binary), one frame at a time; the reader reassembles a full frame from the
 * byte stream before injecting it onto the local bus.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "net/can_emu.h"
#include "net/can_host.h"
#include "qom/object.h"

#define TYPE_CAN_HOST_CHARDEV "can-host-chardev"
OBJECT_DECLARE_SIMPLE_TYPE(CanHostChardev, CAN_HOST_CHARDEV)

struct CanHostChardev {
    CanHostState    parent;

    char            *chardev_id;
    CharFrontend    chr;
    uint8_t         rxbuf[sizeof(qemu_can_frame)];
    unsigned        rxlen;
};

/* bus -> chardev: a frame from the local CAN bus goes out over the socket. */
static ssize_t can_host_chardev_receive(CanBusClientState *client,
                                        const qemu_can_frame *frames,
                                        size_t frames_cnt)
{
    CanHostState *ch = container_of(client, CanHostState, bus_client);
    CanHostChardev *c = CAN_HOST_CHARDEV(ch);
    size_t i;

    for (i = 0; i < frames_cnt; i++) {
        qemu_chr_fe_write_all(&c->chr, (const uint8_t *)&frames[i],
                              sizeof(qemu_can_frame));
    }
    return frames_cnt;
}

static bool can_host_chardev_can_receive(CanBusClientState *client)
{
    return true;
}

/* chardev -> bus: reassemble a full frame from the byte stream + inject it. */
static int can_host_chardev_chr_can_read(void *opaque)
{
    CanHostChardev *c = opaque;

    return sizeof(qemu_can_frame) - c->rxlen;
}

static void can_host_chardev_chr_read(void *opaque, const uint8_t *buf,
                                      int size)
{
    CanHostChardev *c = opaque;
    CanHostState *ch = CAN_HOST(c);
    int i;

    for (i = 0; i < size; i++) {
        c->rxbuf[c->rxlen++] = buf[i];
        if (c->rxlen == sizeof(qemu_can_frame)) {
            qemu_can_frame frame;

            memcpy(&frame, c->rxbuf, sizeof(frame));
            can_bus_client_send(&ch->bus_client, &frame, 1);
            c->rxlen = 0;
        }
    }
}

static CanBusClientInfo can_host_chardev_bus_client_info = {
    .can_receive = can_host_chardev_can_receive,
    .receive     = can_host_chardev_receive,
};

static void can_host_chardev_connect(CanHostState *ch, Error **errp)
{
    CanHostChardev *c = CAN_HOST_CHARDEV(ch);
    Chardev *chr;

    if (!c->chardev_id) {
        error_setg(errp, "'chardev' property not set");
        return;
    }
    chr = qemu_chr_find(c->chardev_id);
    if (!chr) {
        error_setg(errp, "chardev '%s' not found", c->chardev_id);
        return;
    }
    if (!qemu_chr_fe_init(&c->chr, chr, errp)) {
        return;
    }

    c->rxlen = 0;
    ch->bus_client.info = &can_host_chardev_bus_client_info;
    qemu_chr_fe_set_handlers(&c->chr, can_host_chardev_chr_can_read,
                             can_host_chardev_chr_read, NULL, NULL, c, NULL,
                             true);
}

static void can_host_chardev_disconnect(CanHostState *ch)
{
    CanHostChardev *c = CAN_HOST_CHARDEV(ch);

    qemu_chr_fe_deinit(&c->chr, false);
}

static char *can_host_chardev_get_chardev(Object *obj, Error **errp)
{
    CanHostChardev *c = CAN_HOST_CHARDEV(obj);

    return g_strdup(c->chardev_id);
}

static void can_host_chardev_set_chardev(Object *obj, const char *value,
                                         Error **errp)
{
    CanHostChardev *c = CAN_HOST_CHARDEV(obj);

    g_free(c->chardev_id);
    c->chardev_id = g_strdup(value);
}

static void can_host_chardev_finalize(Object *obj)
{
    CanHostChardev *c = CAN_HOST_CHARDEV(obj);

    g_free(c->chardev_id);
}

static void can_host_chardev_class_init(ObjectClass *klass, const void *data)
{
    CanHostClass *chc = CAN_HOST_CLASS(klass);

    object_class_property_add_str(klass, "chardev",
                                  can_host_chardev_get_chardev,
                                  can_host_chardev_set_chardev);
    chc->connect = can_host_chardev_connect;
    chc->disconnect = can_host_chardev_disconnect;
}

static const TypeInfo can_host_chardev_info = {
    .name          = TYPE_CAN_HOST_CHARDEV,
    .parent        = TYPE_CAN_HOST,
    .instance_size = sizeof(CanHostChardev),
    .instance_finalize = can_host_chardev_finalize,
    .class_init    = can_host_chardev_class_init,
};

static void can_host_chardev_register_types(void)
{
    type_register_static(&can_host_chardev_info);
}

type_init(can_host_chardev_register_types);
