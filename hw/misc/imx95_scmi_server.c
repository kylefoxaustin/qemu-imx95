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
#include "qemu/main-loop.h"
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

/* ----- Clock protocol (0x14) ----- */

/*
 * The clock SCMI client (drivers/clk/clk_scmi.c) walks 0..num_clocks-1
 * at probe time, registering a CCF clock for each. Then U-Boot drivers
 * that consume clocks via DT phandle look them up by ID and call
 * RATE_GET / CONFIG_SET / etc. Advertise enough clocks to cover the
 * largest IMX95_CLK_* index, plus reasonable RATE_GET / SET / CONFIG
 * answers so consumers don't error out.
 */
/*
 * Advertise enough clocks to cover IMX95_CLK_* indices used by SPL
 * (LPUART1 is index 52 = IMX95_CCM_NUM_CLK_SRC + 11). Each clock
 * must have a UNIQUE name when CCF (CONFIG_SPL_CLK_CCF) registers
 * it; without that, clk_register() rejects duplicates and CCF ends
 * up with zero clocks, which makes clk_enable("ipg") fail -ENOENT
 * inside the LPUART serial driver's probe.
 */
#define SCMI_CLOCK_NUM              80
#define SCMI_CLOCK_DEFAULT_RATE     24000000

/*
 * Per-clock rate table. CLOCK_RATE_GET response is computed from
 * this table; clock_ids not listed fall back to SCMI_CLOCK_DEFAULT_RATE
 * (24 MHz). The v0.1 milestone got away with returning 24 MHz for
 * every clock because the only consumer (LPUART) was natively a
 * 24 MHz IP. v0.2 work that touches uSDHC, ENET, or other clocks
 * with higher source rates needs accurate values here - otherwise
 * the driver's divisor math will compute against the wrong source
 * rate and timing on the bus will fail in subtle ways.
 *
 * Clock IDs derive from the i.MX 95 BSP header
 * references/uboot-imx/dts/upstream/src/arm64/freescale/imx95-clock.h
 * where each IMX95_CLK_<NAME> is defined relative to
 * IMX95_CCM_NUM_CLK_SRC (=41). When adding entries, please leave a
 * comment pointing at the header line so the source-of-truth stays
 * traceable.
 */
struct scmi_clock_rate_entry {
    uint32_t clock_id;
    uint64_t rate_hz;
    const char *name;       /* informational; not delivered to agent */
};

static const struct scmi_clock_rate_entry scmi_clock_rates[] = {
    /* IMX95_CLK_LPUART1 = 41 + 11 = 52. imx95-clock.h:64. */
    { 52, 24000000, "lpuart1" },
    /*
     * Add entries here as v0.2/v0.3 work observes consumers wanting
     * specific rates. Reasonable expected next adds (RM-confirmation
     * needed before trusting these): IMX95_CLK_USDHC1/2/3 at ~400 MHz
     * (sourced from SYSPLL1_PFD1), IMX95_CLK_ENET at 125 MHz or
     * 250 MHz depending on RGMII/SGMII selection.
     */
};

static uint64_t scmi_lookup_clock_rate(uint32_t clock_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(scmi_clock_rates); i++) {
        if (scmi_clock_rates[i].clock_id == clock_id) {
            return scmi_clock_rates[i].rate_hz;
        }
    }
    return SCMI_CLOCK_DEFAULT_RATE;
}

#define SCMI_MSG_CLOCK_ATTRIBUTES       0x03
#define SCMI_MSG_CLOCK_RATE_SET         0x05
#define SCMI_MSG_CLOCK_RATE_GET         0x06
#define SCMI_MSG_CLOCK_CONFIG_SET       0x07
#define SCMI_MSG_CLOCK_NAME_GET         0x08
#define SCMI_MSG_CLOCK_PARENT_SET       0x0D

static void scmi_clock(IMX95SCMIServerState *s, unsigned int idx,
                       uint8_t msg_id, uint16_t token)
{
    switch (msg_id) {
    case SCMI_MSG_PROTOCOL_VERSION: {
        uint32_t version = cpu_to_le32(0x00030000);
        scmi_complete(s, idx, SCMI_SUCCESS, &version, sizeof(version));
        return;
    }
    case SCMI_MSG_PROTOCOL_ATTRIBUTES: {
        uint32_t attrs = cpu_to_le32(SCMI_CLOCK_NUM);
        scmi_complete(s, idx, SCMI_SUCCESS, &attrs, sizeof(attrs));
        return;
    }
    case SCMI_MSG_PROTOCOL_MESSAGE_ATTRIBUTES: {
        uint32_t mattr = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &mattr, sizeof(mattr));
        return;
    }
    case SCMI_MSG_CLOCK_ATTRIBUTES: {
        /*
         * Request payload (1 word): clock_id.
         * Response: attributes (u32) + name (16 bytes). Each clock
         * needs a unique name for CCF's clk_register() not to reject
         * duplicates. Pull clock_id from shmem and embed in the name.
         */
        uint32_t clock_id = smt_read32(s, SMT_MSG_PAYLOAD);
        uint8_t  reply[4 + 16] = {0};

        if (clock_id >= SCMI_CLOCK_NUM) {
            scmi_complete(s, idx, SCMI_NOT_SUPPORTED, NULL, 0);
            return;
        }
        snprintf((char *)(reply + 4), 16, "clk_%u", clock_id);
        scmi_complete(s, idx, SCMI_SUCCESS, reply, sizeof(reply));
        return;
    }
    case SCMI_MSG_CLOCK_RATE_GET: {
        uint32_t clock_id = smt_read32(s, SMT_MSG_PAYLOAD);
        uint64_t hz = scmi_lookup_clock_rate(clock_id);
        uint32_t rate[2];
        rate[0] = cpu_to_le32((uint32_t)(hz & 0xFFFFFFFFu));
        rate[1] = cpu_to_le32((uint32_t)(hz >> 32));
        scmi_complete(s, idx, SCMI_SUCCESS, rate, sizeof(rate));
        return;
    }
    case SCMI_MSG_CLOCK_RATE_SET: {
        /*
         * No-op: the v0.1 stub does not actually change clock rates.
         * We still return SUCCESS so consumers proceed. warn_report_once
         * surfaces the no-op exactly once in default QEMU output (so
         * a developer running without -d still learns about it), and
         * the LOG_GUEST_ERROR per-call line gives -d guest_errors the
         * full clock_id detail.
         */
        uint32_t clock_id = smt_read32(s, SMT_MSG_PAYLOAD);
        warn_report_once("scmi-server: CLOCK_RATE_SET is a stub no-op; "
                         "values are not retained (CLOCK_RATE_GET returns "
                         "the table rate). Enable -d guest_errors for "
                         "per-call detail.");
        qemu_log_mask(LOG_GUEST_ERROR,
                      "scmi-server: CLOCK_RATE_SET on clock_id %u ignored\n",
                      clock_id);
        scmi_complete(s, idx, SCMI_SUCCESS, NULL, 0);
        return;
    }
    case SCMI_MSG_CLOCK_CONFIG_SET: {
        uint32_t clock_id = smt_read32(s, SMT_MSG_PAYLOAD);
        warn_report_once("scmi-server: CLOCK_CONFIG_SET is a stub no-op.");
        qemu_log_mask(LOG_GUEST_ERROR,
                      "scmi-server: CLOCK_CONFIG_SET on clock_id %u ignored\n",
                      clock_id);
        scmi_complete(s, idx, SCMI_SUCCESS, NULL, 0);
        return;
    }
    case SCMI_MSG_CLOCK_PARENT_SET: {
        uint32_t clock_id = smt_read32(s, SMT_MSG_PAYLOAD);
        warn_report_once("scmi-server: CLOCK_PARENT_SET is a stub no-op.");
        qemu_log_mask(LOG_GUEST_ERROR,
                      "scmi-server: CLOCK_PARENT_SET on clock_id %u ignored\n",
                      clock_id);
        scmi_complete(s, idx, SCMI_SUCCESS, NULL, 0);
        return;
    }
    case SCMI_MSG_CLOCK_NAME_GET:
        warn_report_once("scmi-server: CLOCK_NAME_GET is a stub no-op "
                         "(returns empty response).");
        scmi_complete(s, idx, SCMI_SUCCESS, NULL, 0);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: CLOCK unhandled msg_id 0x%02x token 0x%x\n",
                      __func__, msg_id, token);
        scmi_complete(s, idx, SCMI_NOT_SUPPORTED, NULL, 0);
        return;
    }
}

/* ----- NXP-vendor SCMI imx-misc protocol (0x84) ----- */

/*
 * ROM_PASSOVER_GET response layout (per rom_passover_t in U-Boot's
 * arch/arm/include/asm/mach-imx/sys_proto.h:244):
 *
 *   u32 status;
 *   u32 numPassover;
 *   u32 passover[15];   // 60 bytes; packed rom_passover_t struct
 *
 * Total response payload = 4 + 4 + 60 = 68 bytes. Matches the
 * "size_of(out) = 68" the SPL error message reports.
 *
 * The packed rom_passover_t fields the agent unpacks: tag (u16),
 * len (u8 = 0x80), ver (u8), boot_mode (u32), card_addr_mode (u32),
 * bad_blks_of_img_set0 (u32), ap_mu_id (u32),
 * bad_blks_of_img_set1 (u32), boot_stage (u8), img_set_sel (u8),
 * rsv0[2], img_set_end (u32), rom_version (u32),
 * boot_dev_state (u8), boot_dev_inst (u8), boot_dev_type (u8), rsv1,
 * dev_page_size (u32), cnt_header_ofs (u32), img_ofs (u32).
 *
 * v0.2 stub returns numPassover = 0 with the passover block
 * zero-filled. SPL's scmi_get_rom_data() succeeds (no more
 * "scmi_err = -1" message), but the data is uninformative; SPL will
 * fall back to its default boot-device probe sequence rather than
 * using ROM-provided hints. Promote to a real boot-mode hint when
 * the storage backend lands in v0.2.
 */
static void scmi_imx_misc(IMX95SCMIServerState *s, unsigned int idx,
                          uint8_t msg_id, uint16_t token)
{
    switch (msg_id) {
    case SCMI_MSG_PROTOCOL_VERSION: {
        uint32_t version = cpu_to_le32(0x00010000);
        scmi_complete(s, idx, SCMI_SUCCESS, &version, sizeof(version));
        return;
    }
    case SCMI_MSG_PROTOCOL_ATTRIBUTES: {
        uint32_t attrs = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &attrs, sizeof(attrs));
        return;
    }
    case SCMI_MSG_PROTOCOL_MESSAGE_ATTRIBUTES: {
        uint32_t mattr = 0;
        scmi_complete(s, idx, SCMI_SUCCESS, &mattr, sizeof(mattr));
        return;
    }
    case SCMI_MSG_IMX_MISC_RESET_REASON: {
        /*
         * Response struct (scmi_imx_misc_reset_reason_out in
         * scmi_nxp_protocols.h:29): status (4) + bootflags (4) +
         * shutdownflags (4) + extInfo[21] (84). Total 96 bytes.
         * Stub: zero everything, which the agent reads as "no boot
         * reason recorded, no shutdown reason recorded, no
         * extended info" - functionally indistinguishable from a
         * fresh power-on reset, which is what QEMU effectively is.
         */
        uint8_t extra[4 + 4 + 84] = {0};
        scmi_complete(s, idx, SCMI_SUCCESS, extra, sizeof(extra));
        return;
    }
    case SCMI_MSG_IMX_MISC_ROM_PASSOVER_GET: {
        /* Response extra payload: numPassover (u32) + passover[15] (60B). */
        uint8_t extra[4 + 60] = {0};
        warn_report_once("scmi-server: imx-misc ROM_PASSOVER_GET stub "
                         "returning numPassover=0 (SPL will fall back to "
                         "default boot-device probing).");
        scmi_complete(s, idx, SCMI_SUCCESS, extra, sizeof(extra));
        return;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: imx-misc unhandled msg_id 0x%02x token 0x%x\n",
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
         * Stub: report zero resources. U-Boot SPL probes the
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
    uint32_t chan_status;

    /*
     * The MU model invokes this from inside its MMIO write callback,
     * which runs with the BQL held. dma_memory_read/write below
     * require the BQL; assert here so any future refactor that moves
     * the MU dispatch off-thread (e.g., per-vCPU dispatch under
     * -icount) trips loudly instead of silently corrupting memory.
     */
    g_assert(bql_locked());

    chan_status = smt_read32(s, SMT_CHAN_STATUS);

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
        scmi_clock(s, idx, msg_id, token);
        return;
    case SCMI_PROTOCOL_PINCTRL:
        scmi_protocol_stub(s, idx, protocol_id, msg_id, token);
        return;
    case SCMI_PROTOCOL_IMX_MISC:
        scmi_imx_misc(s, idx, msg_id, token);
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

static void imx95_scmi_server_reset(DeviceState *dev)
{
    IMX95SCMIServerState *s = IMX95_SCMI_SERVER(dev);

    /*
     * Real silicon has the M33 SM firmware initialise the SCMI shared
     * memory before releasing the A55 cluster: channel_status is set
     * with the CHANNEL_FREE bit so the agent's first
     * scmi_write_msg_to_smt() finds the channel ready. With no M33
     * actually running, our stub takes that responsibility.
     */
    g_assert(bql_locked());
    smt_write32(s, SMT_CHAN_STATUS, SMT_CHAN_FREE);
}

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
    device_class_set_legacy_reset(dc, imx95_scmi_server_reset);
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
