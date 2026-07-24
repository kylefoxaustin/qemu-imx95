/*
 * NXP i.MX 95 Neutron NPU - null compute backend.
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the Track B / upstreamable in-tree backend. It does not compute:
 * it reports retcode=DONE (the driver's completion gate) and, on RUN,
 * copies req->uncomputed_errcode_hint into MBOX1 so the front end's
 * operator-configured honest fault reaches the guest without the backend
 * having to see any front-end state. Non-RUN commands (KERNELS etc.) are
 * ACKed silently with guest_errcode = 0.
 *
 * The seam guarantees this backend has no dependency on the runner
 * subprocess machinery, fixture manifests, /tmp scratch dirs, or any other
 * Track A concept. See docs/neutron-two-track-roadmap.md.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx95_neutron_compute.h"

/* Kept in sync with N_CMD_RUN in imx95_neutron.c. */
#define NEUTRON_NULL_CMD_RUN   0x269
#define NEUTRON_NULL_RET_DONE  0xAD0

static void neutron_backend_null_run(void *ctx, const NeutronComputeReq *req,
                                     NeutronComputeRes *res)
{
    (void)ctx;

    res->retcode        = NEUTRON_NULL_RET_DONE;
    res->output_written = false;
    res->guest_errcode  = (req->cmd == NEUTRON_NULL_CMD_RUN)
                          ? req->uncomputed_errcode_hint
                          : 0;
}

const NeutronBackendOps neutron_backend_null = {
    .name = "null",
    .run  = neutron_backend_null_run,
};
