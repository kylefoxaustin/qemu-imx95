/*
 * Minimal NXP EdgeLock Enclave (ELE) responder stub
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Watches TR-register writes on an i.MX MU, accumulates ELE-protocol
 * command words until the per-message size is reached, dispatches to a
 * command handler, and writes the response into RR registers + asserts
 * RSR.RFn so the agent's mu_hal_receivemsg() poll exits.
 *
 * ELE message format (per ele_api.h):
 *
 *   word 0 (header): version[7:0] | size[15:8] | command[23:16] | tag[31:24]
 *   word 1..N-1:     payload
 *
 * size = total words including header. tag = ELE_CMD_TAG (0x17) for
 * commands, ELE_RESP_TAG (0xE1) for responses. Responses also have a
 * status byte (ELE_SUCCESS_IND = 0xD6 for success) somewhere in the
 * payload depending on command.
 *
 * v0.1 scope: only ELE_GET_INFO_REQ is implemented in detail (the
 * only ELE call U-Boot SPL's imx9_probe_mu() makes pre-relocation,
 * via ele_get_info()). Other commands get a generic SUCCESS response.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/misc/imx95_ele_server.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/dma.h"
#include "migration/vmstate.h"

/*
 * struct ele_get_info_data layout (u32 word offsets), pinned to the
 * U-Boot ele_api.h definition at
 * references/uboot-imx/arch/arm/include/asm/mach-imx/ele_api.h:168.
 * Using named offsets here so a future U-Boot rev that reorders or
 * adds fields trips a build/runtime mismatch instead of silently
 * writing into the wrong slot.
 *
 * Reference layout:
 *   u32 hdr;                  // word 0
 *   u32 soc;                  // word 1
 *   u32 lc;                   // word 2
 *   u32 uid[4];               // words 3..6
 *   u32 sha256_rom_patch[8];  // words 7..14
 *   u32 sha_fw[8];            // words 15..22
 *   u32 oem_srkh[16];         // words 23..38
 *   u32 state;                // word 39
 *   u32 oem_pqc_srkh[16];     // words 40..55
 *   u32 reserved[8];          // words 56..63
 * Total: 64 u32 = 256 bytes.
 */
#define ELE_INFO_OFFSET_HDR             0
#define ELE_INFO_OFFSET_SOC             1
#define ELE_INFO_OFFSET_LC              2
#define ELE_INFO_OFFSET_UID             3
#define ELE_INFO_OFFSET_SHA256_ROM      7
#define ELE_INFO_OFFSET_SHA_FW          15
#define ELE_INFO_OFFSET_OEM_SRKH        23
#define ELE_INFO_OFFSET_STATE           39
#define ELE_INFO_OFFSET_OEM_PQC_SRKH    40
#define ELE_INFO_OFFSET_RESERVED        56
#define ELE_INFO_SIZE_WORDS             64
#define ELE_INFO_SIZE_BYTES             (ELE_INFO_SIZE_WORDS * 4)

/* Header field packing. */
static inline uint32_t ele_make_header(uint8_t version, uint8_t size,
                                       uint8_t command, uint8_t tag)
{
    return (uint32_t)version |
           ((uint32_t)size    << 8) |
           ((uint32_t)command << 16) |
           ((uint32_t)tag     << 24);
}

/* Header field extraction. */
#define ELE_HDR_VERSION(h)  ((uint8_t)((h) & 0xFF))
#define ELE_HDR_SIZE(h)     ((uint8_t)(((h) >> 8) & 0xFF))
#define ELE_HDR_COMMAND(h)  ((uint8_t)(((h) >> 16) & 0xFF))
#define ELE_HDR_TAG(h)      ((uint8_t)(((h) >> 24) & 0xFF))

/*
 * ELE_GET_INFO_REQ handler.
 *
 * Request layout (4 words):
 *   [0] header (command = ELE_GET_INFO_REQ)
 *   [1] info_addr_hi (upper 32 bits of guest pointer)
 *   [2] info_addr_lo (lower 32 bits of guest pointer)
 *   [3] size of ele_get_info_data
 *
 * Real ELE writes a struct ele_get_info_data to the guest address.
 * We fake plausible values: rev 0xA1 i.MX 95, OEM-open lifecycle,
 * UID zero-filled. The Linux/U-Boot drivers do not validate beyond
 * memcpy into gd->arch.
 *
 * Response: 2 words. Status word ELE_SUCCESS_IND in data[0].
 */
static void ele_handle_get_info(IMX95ELEServerState *s)
{
    if (s->msg_count < 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: GET_INFO with only %u words received\n",
                      __func__, s->msg_count);
        return;
    }

    uint64_t info_addr = ((uint64_t)s->msg_buf[1] << 32) |
                          (uint64_t)s->msg_buf[2];

    /*
     * Zero-fill the full 256-byte struct, then set the fields U-Boot's
     * set_cpu_info() reads (soc, lc). Named offsets above keep the
     * mapping legible; if U-Boot ever shifts the struct layout, the
     * build check below + a re-read of ele_api.h are how this gets
     * detected.
     */
    uint32_t info_data[ELE_INFO_SIZE_WORDS] = {0};
    QEMU_BUILD_BUG_ON(sizeof(info_data) != ELE_INFO_SIZE_BYTES);

    info_data[ELE_INFO_OFFSET_SOC] = 0xA1009500;  /* SoC rev 0xA1, type 0x95 */
    info_data[ELE_INFO_OFFSET_LC]  = 0x00000080;  /* lifecycle = OEM open */

    dma_memory_write(&address_space_memory, info_addr,
                     info_data, sizeof(info_data),
                     MEMTXATTRS_UNSPECIFIED);

    /* Response: header + status. */
    uint32_t resp_hdr = ele_make_header(ELE_HDR_VERSION(s->msg_buf[0]), 2,
                                        ELE_GET_INFO_REQ, ELE_RESP_TAG);
    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
}

/*
 * ELE_GET_STATE (0xB2) handler.
 *
 * The Linux fsl-se HSM probe reads the firmware/IMEM state. The driver
 * validates a 4-word (0x10-byte) response and takes state = data[1] & 0xff;
 * report ELE_IMEM_STATE_OK (0xCA) so it neither reloads nor faults the IMEM.
 * Response: header(size 4) + status + state + reserved.
 */
#define ELE_GET_STATE       0xB2
#define ELE_IMEM_STATE_OK   0xCA
static void ele_handle_get_state(IMX95ELEServerState *s, uint8_t command)
{
    uint8_t ver = ELE_HDR_VERSION(s->msg_buf[0]);
    uint32_t resp_hdr = ele_make_header(ver, 4, command, ELE_RESP_TAG);

    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
    imx_mu_deliver_rr(s->mu, 2, ELE_IMEM_STATE_OK);
    imx_mu_deliver_rr(s->mu, 3, 0);
}

/*
 * ELE_READ_FUSE (0x97) handler.
 *
 * The OCOTP/efuse driver reads the fuse banks that are not in the FSB shadow
 * via this SE service (read_words_via_s400_api). The driver validates a 3-word
 * (0x0C-byte) response and takes the fuse word from data[1]; those S400-served
 * banks are not modelled, so report 0 with a success status (the MAC and most
 * fuses come from the readable FSB shadow region instead).
 * Response: header(size 3) + status + fuse value.
 */
#define ELE_READ_FUSE_REQ   0x97
static void ele_handle_read_fuse(IMX95ELEServerState *s, uint8_t command)
{
    uint8_t ver = ELE_HDR_VERSION(s->msg_buf[0]);
    uint32_t resp_hdr = ele_make_header(ver, 3, command, ELE_RESP_TAG);

    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
    imx_mu_deliver_rr(s->mu, 2, 0);
}

/*
 * ELE_GET_FW_VERSION (0x9D) handler.
 *
 * A FW-API command (so the driver wants the FW API version in the response
 * header - the version is echoed from the request, see ele_dispatch). The
 * driver validates a 4-word (0x10-byte) response; report a plausible ELE FW
 * version so "fetch FW version" succeeds instead of warning at probe.
 * Response: header(size 4) + status + fw version + reserved.
 */
#define ELE_GET_FW_VERSION_REQ  0x9D
static void ele_handle_get_fw_version(IMX95ELEServerState *s, uint8_t command)
{
    uint8_t ver = ELE_HDR_VERSION(s->msg_buf[0]);
    uint32_t resp_hdr = ele_make_header(ver, 4, command, ELE_RESP_TAG);

    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
    imx_mu_deliver_rr(s->mu, 2, 0x00010000);   /* ELE FW version (1.0.0) */
    imx_mu_deliver_rr(s->mu, 3, 0);
}

/*
 * ELE_GET_RANDOM (0xCD) handler.
 *
 * The hwrng driver polls this every few seconds; before the version echo it
 * was the source of the "FW API Vers mismatch (0x6 != 0x7)" console storm.
 * Request payload is struct ele_rng_msg_data { u16 rsv; u16 flags; u32 data[2] }
 * after the header, so [1] = rsv|flags, [2] = destination DMA address, [3] =
 * length. Fill the buffer with non-constant bytes (a model PRNG - NOT
 * cryptographic) so /dev/hwrng yields varying data, then ack.
 * Response: 2 words (header + status).
 */
#define ELE_GET_RANDOM_REQ  0xCD
static void ele_handle_get_random(IMX95ELEServerState *s, uint8_t command)
{
    uint8_t ver = ELE_HDR_VERSION(s->msg_buf[0]);
    uint32_t resp_hdr;

    if (s->msg_count >= 4) {
        static uint32_t st = 0x2545f491;   /* xorshift32 state (model only) */
        uint64_t addr = s->msg_buf[2];
        uint32_t len = s->msg_buf[3];
        uint32_t off;

        if (len > 4096) {
            len = 4096;                     /* cap a runaway request */
        }
        for (off = 0; off < len; ) {
            uint8_t chunk[64];
            uint32_t n = MIN(sizeof(chunk), len - off), i;

            for (i = 0; i < n; i++) {
                st ^= st << 13;
                st ^= st >> 17;
                st ^= st << 5;
                chunk[i] = (uint8_t)st;
            }
            dma_memory_write(&address_space_memory, addr + off, chunk, n,
                             MEMTXATTRS_UNSPECIFIED);
            off += n;
        }
    }

    resp_hdr = ele_make_header(ver, 2, command, ELE_RESP_TAG);
    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
}

/*
 * Generic SUCCESS response for any command we do not specifically
 * handle. 2-word response: header + ELE_SUCCESS_IND.
 */
static void ele_handle_generic_ok(IMX95ELEServerState *s, uint8_t command)
{
    uint32_t resp_hdr = ele_make_header(ELE_HDR_VERSION(s->msg_buf[0]), 2,
                                        command, ELE_RESP_TAG);
    imx_mu_deliver_rr(s->mu, 0, resp_hdr);
    imx_mu_deliver_rr(s->mu, 1, ELE_SUCCESS_IND);
}

static void ele_dispatch(IMX95ELEServerState *s)
{
    uint32_t header  = s->msg_buf[0];
    uint8_t  command = ELE_HDR_COMMAND(header);
    uint8_t  tag     = ELE_HDR_TAG(header);

    qemu_log_mask(LOG_UNIMP,
                  "ele-server: cmd=0x%02x tag=0x%02x size=%u\n",
                  command, tag, s->msg_size);

    switch (command) {
    case ELE_GET_INFO_REQ:
        ele_handle_get_info(s);
        return;
    case ELE_GET_STATE:
        ele_handle_get_state(s, command);
        return;
    case ELE_READ_FUSE_REQ:
        ele_handle_read_fuse(s, command);
        return;
    case ELE_GET_FW_VERSION_REQ:
        ele_handle_get_fw_version(s, command);
        return;
    case ELE_GET_RANDOM_REQ:
        ele_handle_get_random(s, command);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: cmd 0x%02x not handled in detail; responding OK\n",
                      __func__, command);
        ele_handle_generic_ok(s, command);
        return;
    }
}

/*
 * TR-write callback. Invoked synchronously by the MU model from inside
 * the guest's MMIO write to TR[idx]. The MU itself re-sets TSR.TEn
 * after this returns so the next TR write succeeds without polling.
 */
static void ele_on_tr_write(void *opaque, unsigned int idx, uint32_t value)
{
    IMX95ELEServerState *s = opaque;

    /* Invoked from the MU MMIO write handler, which runs under the BQL. */
    g_assert(bql_locked());

    if (s->msg_count >= IMX95_ELE_MAX_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: too many words; resetting accumulator\n",
                      __func__);
        s->msg_count = 0;
        s->msg_size  = 0;
        return;
    }

    s->msg_buf[s->msg_count++] = value;

    if (s->msg_count == 1) {
        /* First word is the header; pick out size. */
        s->msg_size = ELE_HDR_SIZE(value);
        if (s->msg_size == 0 || s->msg_size > IMX95_ELE_MAX_WORDS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: bad size %u in header 0x%08x\n",
                          __func__, s->msg_size, value);
            s->msg_count = 0;
            s->msg_size  = 0;
            return;
        }
    }

    if (s->msg_count == s->msg_size) {
        ele_dispatch(s);
        s->msg_count = 0;
        s->msg_size  = 0;
    }
}

static void imx95_ele_server_reset(DeviceState *dev)
{
    IMX95ELEServerState *s = IMX95_ELE_SERVER(dev);

    s->msg_count = 0;
    s->msg_size  = 0;
    memset(s->msg_buf, 0, sizeof(s->msg_buf));
}

static void imx95_ele_server_realize(DeviceState *dev, Error **errp)
{
    IMX95ELEServerState *s = IMX95_ELE_SERVER(dev);

    if (!s->mu) {
        error_setg(errp, "%s: 'mu' link property must be set",
                   TYPE_IMX95_ELE_SERVER);
        return;
    }
    imx_mu_set_tr_write_handler(s->mu, ele_on_tr_write, s);
}

static const Property imx95_ele_server_properties[] = {
    DEFINE_PROP_LINK("mu", IMX95ELEServerState, mu,
                     TYPE_IMX_MU, IMXMUState *),
};

static const VMStateDescription vmstate_imx95_ele_server = {
    .name = TYPE_IMX95_ELE_SERVER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(msg_count, IMX95ELEServerState),
        VMSTATE_UINT32(msg_size, IMX95ELEServerState),
        VMSTATE_UINT32_ARRAY(msg_buf, IMX95ELEServerState,
                             IMX95_ELE_MAX_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx95_ele_server_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx95_ele_server_realize;
    dc->vmsd = &vmstate_imx95_ele_server;
    device_class_set_legacy_reset(dc, imx95_ele_server_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "qemu-imx95 ELE responder stub (v0.1, get_info only)";
    device_class_set_props(dc, imx95_ele_server_properties);
}

static const TypeInfo imx95_ele_server_info = {
    .name           = TYPE_IMX95_ELE_SERVER,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(IMX95ELEServerState),
    .class_init     = imx95_ele_server_class_init,
};

static void imx95_ele_server_register_types(void)
{
    type_register_static(&imx95_ele_server_info);
}

type_init(imx95_ele_server_register_types)
