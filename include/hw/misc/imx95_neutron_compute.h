/*
 * NXP i.MX 95 Neutron NPU - compute backend seam.
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The mailbox front end (hw/misc/imx95_neutron.c) is the same for every
 * inference path; what differs is what happens between the RUN_ACK doorbell
 * and the DONE completion. This header defines the seam:
 *
 *   NeutronBackendOps::run(req, res)
 *
 * is called for each mailbox command the driver rings the doorbell for, sees
 * the carveout bases + mailbox args the guest posted, and fills in the retcode
 * + guest error_code the front end then writes back into MBOX0 / MBOX1.
 *
 * Backends implemented today:
 *   - "null" (default): no compute, guest output buffer untouched. The
 *     model's historical behaviour, kept honest by a non-zero error_code in
 *     MBOX1 (NEUTRON_UNCOMPUTED_ERRCODE_DEFAULT).
 *
 * Planned:
 *   - "runner": exec the operator-supplied neutron-runner as a subprocess to
 *     compute bit-exact outputs and DMA them back into the guest carveout (see
 *     docs/neutron-two-track-roadmap.md).
 *
 * The seam is intentionally narrow: no MMIO, no IRQ, no timers - a backend
 * only sees the guest-posted args and reports a retcode. Threading model is
 * decided in imx95_neutron.c (today: from a QEMUTimer callback under the
 * BQL; future runner backend will use a worker thread + BH per M2/M3 in the
 * integration plan).
 */

#ifndef HW_MISC_IMX95_NEUTRON_COMPUTE_H
#define HW_MISC_IMX95_NEUTRON_COMPUTE_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/core/qdev.h"

typedef struct AddressSpace AddressSpace;

/*
 * Request into a compute backend: the mailbox command word plus the driver-
 * visible carveout base + the three RUN args the driver posts (tensor
 * offset, microcode offset, tensor count). BASEDDR/BASEINOUT/BASESPILL are
 * all the same phys address on real silicon (the reserved-memory carveout);
 * we surface them as one uint64 so a backend can DMA into it.
 *
 * 'as' is the AddressSpace a threaded/subprocess backend uses to DMA into
 * the guest carveout (dma_memory_read/write). The null backend does not use
 * it. Populated by the front end from the machine's system address space.
 */
typedef struct NeutronComputeReq {
    uint32_t cmd;                /* MBOX3 command word (RUN, KERNELS, ...) */
    uint64_t carveout_base;      /* BASEDDR = BASEINOUT = BASESPILL */
    uint32_t tensor_offset;      /* RUN arg 0 (MBOX4) */
    uint32_t microcode_offset;   /* RUN arg 1 (MBOX5) */
    uint32_t tensor_count;       /* RUN arg 2 (MBOX6) */
    AddressSpace *as;            /* guest DMA space (system memory) */
    /*
     * Front-end-suggested guest error_code for a non-computing backend to
     * report in MBOX1 on RUN. The null backend uses this so it does not need
     * to reach back into the front-end state. Compute backends (runner,
     * future native) ignore this on success.
     */
    uint32_t uncomputed_errcode_hint;
} NeutronComputeReq;


/*
 * Response from a compute backend. The front end writes retcode into MBOX0
 * (the driver waits on this) and guest_errcode into MBOX1 (the driver
 * copies this to userspace as neutron_uapi_result_status.error_code).
 *
 * A backend that actually DMA'd outputs back into the carveout sets
 * output_written = true; a non-computing backend leaves it false and the
 * front end logs the uncomputed-inference telemetry.
 */
typedef struct NeutronComputeRes {
    uint32_t retcode;            /* MBOX0 (DONE / RESET / ...) */
    uint32_t guest_errcode;      /* MBOX1 (0 = success, non-zero = fault) */
    bool     output_written;     /* did the backend DMA outputs back? */
} NeutronComputeRes;

/*
 * A compute backend. Just one method today (run); a future backend that
 * wants long-lived state (spawned subprocess, model cache, ...) can wire in
 * an init/fini pair without changing the mailbox front end.
 *
 * ctx is opaque backend state; the front end passes whatever was set via
 * neutron_backend_bind(). run() is called under the BQL from a QEMUTimer
 * for the null backend; a threaded backend will call it from a worker.
 */
typedef struct NeutronBackendOps {
    const char *name;
    void (*run)(void *ctx, const NeutronComputeReq *req,
                NeutronComputeRes *res);
} NeutronBackendOps;

/*
 * Runner-backend config (M3). Populated from QOM properties on the neutron
 * device; consumed by neutron_backend_runner_new(). All strings are owned by
 * the caller (the device's QOM string properties); the returned context
 * copies what it needs.
 *
 * Returns a heap-allocated context on success (register it via s->backend =
 * &neutron_backend_runner and s->backend_ctx = ctx). Returns NULL and sets
 * errp on failure - caller should log and fall back to the null backend.
 *
 * neutron_backend_runner_free() releases the context.
 */
typedef struct NeutronRunnerCfg {
    const char *runner_path;      /* absolute path to neutron-runner ELF */
    const char *fixtures_path;    /* JSON manifest of known models */
    const char *scratch_dir;      /* per-job scratch root (may be NULL) */
    uint32_t    timeout_ms;       /* subprocess deadline (0 -> 10000) */
    bool        keep_scratch;     /* retain scratch dir on success */
} NeutronRunnerCfg;

/*
 * Guest error_code family for runner-backend failures. Sits in the same
 * 0x95E* space the null backend uses for "did not compute", so a guest that
 * already knows to check MBOX1 for 0x95E0-family faults picks these up
 * automatically.
 */
#define NEUTRON_RUNNER_ERR_FIXTURE_MISS   0x95E1u
#define NEUTRON_RUNNER_ERR_EXEC_FAILED    0x95E2u
#define NEUTRON_RUNNER_ERR_TIMEOUT        0x95E3u
#define NEUTRON_RUNNER_ERR_SIZE_MISMATCH  0x95E4u
#define NEUTRON_RUNNER_ERR_DMA_FAILED     0x95E5u

/*
 * In-tree null backend (imx95_neutron_backend_null.c). Stateless: ctx is
 * unused. Reports retcode=DONE and, on RUN, guest_errcode =
 * req->uncomputed_errcode_hint so the front end's operator-configured honest
 * fault flows through cleanly. This is the Track B / upstreamable backend;
 * the front end must always be able to bind it without any Track A code.
 */
extern const NeutronBackendOps neutron_backend_null;

/*
 * Runner backend (imx95_neutron_backend_runner.c). Track A / downstream only.
 * The symbol only exists when CONFIG_IMX95_NEUTRON_RUNNER is set; the front
 * end must not reference it unconditionally. The prototypes are declared
 * unconditionally so the runner .c compiles cleanly without depending on
 * config-devices.h include ordering; unresolved references at link time are
 * prevented by gating callers with #ifdef CONFIG_IMX95_NEUTRON_RUNNER.
 */
extern const NeutronBackendOps neutron_backend_runner;

void *neutron_backend_runner_new(const NeutronRunnerCfg *cfg, Error **errp);
void  neutron_backend_runner_free(void *ctx);

/*
 * Register the runner-backend QOM properties on the neutron DeviceClass. Called
 * from the front end's class_init. The real runner backend registers
 * "neutron-runner-path", "neutron-runner-fixtures", "neutron-runner-scratch-dir",
 * "neutron-runner-timeout-ms", and "neutron-keep-scratch" and points them at
 * the front end's state via the offsets it fills into cfg_offsets. The stub
 * registers nothing, so a null-only build never exposes any neutron-runner-*
 * property on the QOM surface. This is how the front end stays literally free
 * of Track A property-name strings.
 *
 * The offsets are the byte offsets of the corresponding fields inside the front
 * end's state struct (IMX95NeutronState). Passing them in keeps this seam
 * type-agnostic - the backend does not need to know the struct type; it just
 * uses the offsets with DEFINE_PROP_STRING_offset()-style property registration.
 */
typedef struct NeutronRunnerPropOffsets {
    size_t runner_path_off;        /* offset of char *runner_path */
    size_t fixtures_off;           /* offset of char *runner_fixtures */
    size_t scratch_dir_off;        /* offset of char *runner_scratch_dir */
    size_t timeout_ms_off;         /* offset of uint32_t runner_timeout_ms */
    size_t keep_scratch_off;       /* offset of bool keep_scratch */
} NeutronRunnerPropOffsets;

void neutron_backend_runner_add_props(DeviceClass *dc,
                                      const NeutronRunnerPropOffsets *o);

/*
 * "compute-backend=runner" requested but not compiled in? The stub reports
 * true; the real runner reports false. Front end uses this to hard-fail an
 * explicit runner request in a null-only build (per Phase 0.5 reviewer ask).
 */
bool neutron_backend_runner_is_stub(void);

#endif /* HW_MISC_IMX95_NEUTRON_COMPUTE_H */
