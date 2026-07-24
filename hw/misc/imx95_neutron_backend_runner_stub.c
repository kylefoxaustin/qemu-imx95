/*
 * NXP i.MX 95 Neutron NPU - runner backend stub.
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Compiled when CONFIG_IMX95_NEUTRON is enabled but
 * CONFIG_IMX95_NEUTRON_RUNNER is not. Provides the symbols the front end
 * references (neutron_backend_runner{,_new,_free,_add_props,_is_stub}) so
 * imx95_neutron.c does not need any per-target #ifdef. The stub reports
 * "runner not compiled in" at bind time; the front end then either hard-fails
 * an explicit compute-backend=runner request (upstream/null-only policy) or
 * warns and falls back to null (downstream policy). See docs/neutron-two-
 * track-roadmap.md Phase 0.5.
 *
 * The stub's neutron_backend_runner_add_props() registers no QOM properties.
 * That is the whole point: a null-only build never exposes any
 * "neutron-runner-*" property on the QOM surface, so the upstream-facing
 * property set is clean. The Track A property names literally do not appear
 * in the front end.
 *
 * Rationale: QEMU poisons per-target CONFIG_* macros in common code
 * (libsystem is compiled once for all targets). We cannot check
 * CONFIG_IMX95_NEUTRON_RUNNER inside imx95_neutron.c; the build system
 * decides which file - real runner or this stub - supplies the symbols.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/imx95_neutron_compute.h"

const NeutronBackendOps neutron_backend_runner = {
    .name = "runner",
    .run  = NULL, /* Never called: _new() always fails in the stub build. */
};

void *neutron_backend_runner_new(const NeutronRunnerCfg *cfg, Error **errp)
{
    (void)cfg;
    error_setg(errp,
               "CONFIG_IMX95_NEUTRON_RUNNER is not enabled in this build");
    return NULL;
}

void neutron_backend_runner_free(void *ctx)
{
    (void)ctx;
}

void neutron_backend_runner_add_props(DeviceClass *dc,
                                      const NeutronRunnerPropOffsets *o)
{
    /*
     * Deliberately empty: a null-only build must not advertise any
     * "neutron-runner-*" property on the QOM surface. The Kconfig invariant
     * (STUB xor RUNNER) guarantees this file and the real backend are never
     * linked into the same binary.
     */
    (void)dc;
    (void)o;
}

bool neutron_backend_runner_is_stub(void)
{
    return true;
}
