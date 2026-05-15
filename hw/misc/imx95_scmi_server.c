/*
 * Minimal SCMI server stub for the qemu-imx95 emulator
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Watches MU2 doorbell triggers, decodes the SMT-format SCMI message
 * the agent placed in sram0, dispatches it to a handler, writes the
 * response back, and asserts the matching GIP bit on the MU so the
 * agent can collect the response.
 *
 * SMT layout (per ARM DEN0056 and U-Boot drivers/firmware/scmi/smt.h):
 *
 *   0x00  u32 reserved
 *   0x04  u32 channel_status   (bit 0 = free, bit 1 = error)
 *   0x08  u32 reserved[2]
 *   0x10  u32 flags            (bit 0 = intr_enabled)
 *   0x14  u32 length           (header dword + payload bytes)
 *   0x18  u32 msg_header       (id | type<<8 | protocol<<10 | token<<18)
 *   0x1C  u8  msg_payload[...]
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/misc/imx95_scmi_server.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/dma.h"
#include "migration/vmstate.h"

/* SMT header offsets. */
#define SMT_CHAN_STATUS         0x04
#define SMT_FLAGS               0x10
#define SMT_LENGTH              0x14
#define SMT_MSG_HEADER          0x18
#define SMT_MSG_PAYLOAD         0x1C

#define SMT_CHAN_FREE           (1u << 0)
#define SMT_CHAN_ERROR          (1u << 1)

/* msg_header bit fields (per SMT_HEADER_* macros in U-Boot smt.h). */
#define MSG_HDR_MSG_ID(h)       ((h) & 0xFFu)
#define MSG_HDR_MSG_TYPE(h)     (((h) >> 8) & 0x3u)
#define MSG_HDR_PROTOCOL_ID(h)  (((h) >> 10) & 0xFFu)
#define MSG_HDR_TOKEN(h)        (((h) >> 18) & 0x3FFFu)

/*
 * Read/write helpers. The SCMI server runs in the MMIO write thread
 * (with BQL held), so dma_memory_read/write on the system address space
 * is safe and simple.
 */
static uint32_t smt_read32(IMX95SCMIServerState *s, uint32_t offset)
{
    uint32_t v = 0;
    dma_memory_read(&address_space_memory, s->shmem_base + offset,
                    &v, sizeof(v), MEMTXATTRS_UNSPECIFIED);
    return le32_to_cpu(v);
}

static void smt_write32(IMX95SCMIServerState *s, uint32_t offset, uint32_t v)
{
    uint32_t le = cpu_to_le32(v);
    dma_memory_write(&address_space_memory, s->shmem_base + offset,
                     &le, sizeof(le), MEMTXATTRS_UNSPECIFIED);
}

static void smt_write(IMX95SCMIServerState *s, uint32_t offset,
                      const void *buf, size_t len)
{
    dma_memory_write(&address_space_memory, s->shmem_base + offset,
                     buf, len, MEMTXATTRS_UNSPECIFIED);
}

/*
 * Write status (and optional extra payload) into the response area and
 * finalise the SMT channel: update length, set the channel-free bit,
 * and raise the response doorbell on the MU.
 */
static void scmi_complete(IMX95SCMIServerState *s, unsigned int idx,
                          int32_t status, const void *extra, size_t extra_len)
{
    /* Status word goes at the start of the payload area. */
    smt_write32(s, SMT_MSG_PAYLOAD, (uint32_t)status);
    if (extra && extra_len) {
        smt_write(s, SMT_MSG_PAYLOAD + 4, extra, extra_len);
    }
    /* length covers msg_header (4 bytes) + payload (status + extra). */
    smt_write32(s, SMT_LENGTH, 4 + 4 + extra_len);

    /* Mark channel free, clear error. */
    uint32_t cs = smt_read32(s, SMT_CHAN_STATUS);
    cs |= SMT_CHAN_FREE;
    cs &= ~SMT_CHAN_ERROR;
    smt_write32(s, SMT_CHAN_STATUS, cs);

    /* Tell the agent the response is ready. */
    imx_mu_assert_gip(s->mu, idx);
}

/* ----- Base protocol handlers (0x10) ----- */

static void scmi_base(IMX95SCMIServerState *s, unsigned int idx,
                      uint8_t msg_id, uint16_t token)
{
    switch (msg_id) {
    case SCMI_MSG_PROTOCOL_VERSION: {
        uint32_t version = cpu_to_le32(0x00020000); /* SCMI 2.0 */
        scmi_complete(s, idx, SCMI_SUCCESS, &version, sizeof(version));
        return;
    }
    case SCMI_MSG_PROTOCOL_ATTRIBUTES: {
        /* num_protocols in [15:0], num_agents in [23:16]. */
        uint32_t attrs = cpu_to_le32(2 | (1u << 16));
        scmi_complete(s, idx, SCMI_SUCCESS, &attrs, sizeof(attrs));
        return;
    }
    case SCMI_MSG_PROTOCOL_MESSAGE_ATTRIBUTES: {
        uint32_t mattr = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &mattr, sizeof(mattr));
        return;
    }
    case SCMI_MSG_BASE_DISCOVER_VENDOR: {
        char vendor[16] = "QEMU-imx95";
        scmi_complete(s, idx, SCMI_SUCCESS, vendor, sizeof(vendor));
        return;
    }
    case SCMI_MSG_BASE_DISCOVER_SUB_VENDOR: {
        char sub[16] = "stub";
        scmi_complete(s, idx, SCMI_SUCCESS, sub, sizeof(sub));
        return;
    }
    case SCMI_MSG_BASE_DISCOVER_IMPLEMENTATION_VERSION: {
        uint32_t impl = cpu_to_le32(0x00010000);
        scmi_complete(s, idx, SCMI_SUCCESS, &impl, sizeof(impl));
        return;
    }
    case SCMI_MSG_BASE_DISCOVER_LIST_PROTOCOLS: {
        /*
         * Response: num_protocols (u32), then packed u8 protocol IDs.
         * We advertise CLOCK and PINCTRL since those are what U-Boot's
         * imx9_probe_mu() explicitly probes by DT phandle.
         */
        uint8_t buf[8] = {0};
        uint32_t n = 2;
        memcpy(buf, &(uint32_t){cpu_to_le32(n)}, 4);
        buf[4] = SCMI_PROTOCOL_CLOCK;
        buf[5] = SCMI_PROTOCOL_PINCTRL;
        scmi_complete(s, idx, SCMI_SUCCESS, buf, sizeof(buf));
        return;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: BASE protocol unhandled msg_id 0x%02x token 0x%x\n",
                      __func__, msg_id, token);
        scmi_complete(s, idx, SCMI_NOT_SUPPORTED, NULL, 0);
        return;
    }
}

/* ----- Common stub for any other advertised protocol ----- */

static void scmi_protocol_stub(IMX95SCMIServerState *s, unsigned int idx,
                               uint8_t protocol_id, uint8_t msg_id,
                               uint16_t token)
{
    switch (msg_id) {
    case SCMI_MSG_PROTOCOL_VERSION: {
        /* SCMI v3 for clock/pinctrl is current per the ARM spec. */
        uint32_t version = cpu_to_le32(0x00030000);
        scmi_complete(s, idx, SCMI_SUCCESS, &version, sizeof(version));
        return;
    }
    case SCMI_MSG_PROTOCOL_ATTRIBUTES: {
        /*
         * Stub: report zero clocks / zero pins. U-Boot SPL probes the
         * protocol and then asks for specific resources by ID; with
         * zero advertised, those follow-ups will fall into the
         * NOT_SUPPORTED path below and surface in logs for option-C
         * iteration.
         */
        uint32_t attrs = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &attrs, sizeof(attrs));
        return;
    }
    case SCMI_MSG_PROTOCOL_MESSAGE_ATTRIBUTES: {
        uint32_t mattr = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &mattr, sizeof(mattr));
        return;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: protocol 0x%02x unhandled msg_id 0x%02x token 0x%x\n",
                      __func__, protocol_id, msg_id, token);
        scmi_complete(s, idx, SCMI_NOT_SUPPORTED, NULL, 0);
        return;
    }
}

/* ----- Doorbell entry point (called from imx_mu) ----- */

static void scmi_doorbell(void *opaque, unsigned int idx)
{
    IMX95SCMIServerState *s = opaque;
    uint32_t chan_status = smt_read32(s, SMT_CHAN_STATUS);

    /*
     * The agent clears the channel-free bit before doorbelling. If the
     * bit is set, the message isn't ours - skip and don't ack.
     */
    if (chan_status & SMT_CHAN_FREE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: doorbell on idx %u but SMT channel still free\n",
                      __func__, idx);
        return;
    }

    uint32_t msg_header  = smt_read32(s, SMT_MSG_HEADER);
    uint8_t  protocol_id = MSG_HDR_PROTOCOL_ID(msg_header);
    uint8_t  msg_id      = MSG_HDR_MSG_ID(msg_header);
    uint16_t token       = MSG_HDR_TOKEN(msg_header);

    qemu_log_mask(LOG_UNIMP,
                  "scmi-server: doorbell idx=%u protocol=0x%02x msg=0x%02x token=0x%x\n",
                  idx, protocol_id, msg_id, token);

    switch (protocol_id) {
    case SCMI_PROTOCOL_BASE:
        scmi_base(s, idx, msg_id, token);
        return;
    case SCMI_PROTOCOL_CLOCK:
    case SCMI_PROTOCOL_PINCTRL:
        scmi_protocol_stub(s, idx, protocol_id, msg_id, token);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: protocol 0x%02x not advertised; rejecting\n",
                      __func__, protocol_id);
        scmi_complete(s, idx, SCMI_NOT_SUPPORTED, NULL, 0);
        return;
    }
}

/* ----- QOM glue ----- */

static const Property imx95_scmi_server_properties[] = {
    DEFINE_PROP_LINK("mu", IMX95SCMIServerState, mu,
                     TYPE_IMX_MU, IMXMUState *),
    DEFINE_PROP_UINT64("shmem-base", IMX95SCMIServerState, shmem_base,
                       0x445b1000ULL),
    DEFINE_PROP_UINT64("shmem-size", IMX95SCMIServerState, shmem_size,
                       0x400ULL),
    DEFINE_PROP_UINT32("inbound-channels", IMX95SCMIServerState,
                       inbound_channels, 0x3),
};

static void imx95_scmi_server_realize(DeviceState *dev, Error **errp)
{
    IMX95SCMIServerState *s = IMX95_SCMI_SERVER(dev);

    if (!s->mu) {
        error_setg(errp, "%s: 'mu' link property must be set",
                   TYPE_IMX95_SCMI_SERVER);
        return;
    }
    imx_mu_set_doorbell_handler(s->mu, scmi_doorbell, s);
}

static const VMStateDescription vmstate_imx95_scmi_server = {
    .name = TYPE_IMX95_SCMI_SERVER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        /*
         * The server itself holds no run-time state worth migrating;
         * the SMT shared-memory buffer is the source of truth for any
         * in-flight exchange, and the MU it points at owns its own
         * register state.
         */
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_scmi_server_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_scmi_server_realize;
    dc->vmsd = &vmstate_imx95_scmi_server;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "qemu-imx95 SCMI server stub (v0.1, base + clock + pinctrl)";
    device_class_set_props(dc, imx95_scmi_server_properties);
}

static const TypeInfo imx95_scmi_server_info = {
    .name           = TYPE_IMX95_SCMI_SERVER,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95SCMIServerState),
    .class_init     = imx95_scmi_server_class_init,
};

static void imx95_scmi_server_register_types(void)
{
    type_register_static(&imx95_scmi_server_info);
}

type_init(imx95_scmi_server_register_types)
