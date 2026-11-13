/*
 * NXP i.MX 95 BLK_CTRL_S_AONMIX
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX95_AONMIX_H
#define HW_MISC_IMX95_AONMIX_H

#include "qom/object.h"

#define TYPE_IMX95_AONMIX "imx95.aonmix"
OBJECT_DECLARE_SIMPLE_TYPE(IMX95AonmixState, IMX95_AONMIX)

/* Cores gated by CA55_CPUWAIT, one bit each, and one RVBARADDR pair each. */
#define IMX95_AONMIX_A55_CORES  6

/*
 * The reset vector the System Manager programmed for A55 core @core, as a
 * byte address, or 0 if it never programmed one. On silicon the RVBARADDR
 * register output is wired to the core's RVBAR input, so the SoC applies
 * this when the core's CPUWAIT gate opens.
 */
uint64_t imx95_aonmix_a55_rvbar(IMX95AonmixState *s, unsigned core);

#endif
