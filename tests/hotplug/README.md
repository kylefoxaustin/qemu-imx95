A55 CPU hotplug under the EL3 firmware chain
============================================

run.sh offlines each A55 secondary and brings it back. It FAILS by default,
where PSCI is answered by TF-A BL31 (el3-firmware=on). The offline itself now
completes:

    psci: CPU1 killed (polled 32 ms)

but bringing the core back does not:

    CPU1: failed to come online
    CPU1: failed in unknown state : 0x0

`EL3=off ./run.sh` runs the same sweep on the same U-Boot chain with QEMU's own
PSCI and PASSES all twelve checks (`psci: CPUn killed (polled 0 ms)`). That is
the control: the sweep and this script can go green, so a permanently-red test
is not being mistaken for an unpassable one.


What the old failure was
------------------------

Before the Cortex-A55 IMPLEMENTATION DEFINED registers were added to the CPU
model (target/arm/tcg/cpu64.c), the offline failed earlier:

    psci: CPU1 may not have shut down cleanly (AFFINITY_INFO reports 0)
    CPU1 may not have shut down cleanly: -110
    psci: failed to boot CPU1 (-22)

This file used to call that a BL31 gap, because CPU1 ended up halted at EL3
in a "wfi; b ." loop with no SRC, GPC, CA55_CPUWAIT or SCMI traffic in the
CPU_OFF window. That loop was BL31's panic handler, not a park. With -d int
the core takes an UNDEF at EL3 on its first instruction of the power-down
sequence:

    0x8a2076e4: d538f2e1  mrs x1, s3_0_c15_c2_7      (CPUPWRCTLR_EL1)

in TF-A's cortex_a55_core_pwr_dwn, which runs before the platform's
pwr_domain_off, and then panics while holding the PSCI locks. That is why
nothing reached the hardware, why AFFINITY_INFO stayed ON, and why the later
CPU_ON was refused. It was a model gap: QEMU's cortex-a55 had none of the
core's IMPLEMENTATION DEFINED registers, and TF-A's i.MX 9 platform code also
writes the DSU's CLUSTERPWRDN_EL1 on the same path. Both are now provided.

With them, the CPU_OFF window shows what BL31's imx_pwr_domain_off asks for:
three SCMI messages to the SM on MU1 for CPU 3 (A55 core 1) - LPM_MODE_SET
(0x82/0x0a), IRQ_WAKE_SET (0x82/0x08) and SLEEP_MODE_SET (0x82/0x07, SUSPEND)
- and the SM programming that core's GPC CMC_MODE_CTRL (cpu_ctrl 3, offset
0x10) for SUSPEND. BL31 then reports the core OFF and Linux logs "CPU1
killed".


What IS broken
--------------

The core is meant to be powered down by the GPC when it executes WFI in
SUSPEND sleep mode, and powered back up, and reset at its RVBAR, when the SM
wakes it. The GPC CMC A55 low-power state machine (CMC_MODE_CTRL /
CMC_MODE_STAT driving the SRC power switch) is not modelled - a model gap,
not a firmware one - so the core just
stays in BL31's power-down WFI with its power domain still on. For the
re-online BL31 sends CPU_RESET_VECTOR_SET (0x82/0x06) and CPU_START
(0x82/0x04); the SM's CPU_RunModeSet() then sees a powered core with
CA55_CPUWAIT clear, reads that as already running, and does nothing. The core
never comes back.

PSCI CPU_SUSPEND (cpuidle) has the same gap: it no longer takes an UNDEF at
EL3, but waking a core from the GPC suspend state is not modelled either. So
keep `cpuidle.off=1` and do not offline CPUs under TF-A until the GPC
state machine is modelled.


How it was measured, so it can be re-measured
---------------------------------------------

Four instruments, all opt-in trace events added for this:

    -trace enable=imx95_src_write    -trace enable=imx95_src_read
    -trace enable=imx95_gpc_write    -trace enable=imx95_gpc_read
    -trace enable=imx95_aonmix_cpuwait
    -trace enable=imx_mu_scmi_request

Two things make the result readable:

1. Markers in the trace stream itself. The trace has no timestamps, so the
   CPU_OFF window cannot be located by eye in a 400k-line log - the first
   attempt at this mistook a poweroff teardown for the hotplug sequence. The
   guest writes a magic value to SRC_GEN+0x3f0 (plain storage, inside the
   block but below the first mix slice, never touched by the SM or by this
   model's status mirrors) immediately before and after each operation:

       busybox devmem 0x444603f0 32 0xAAAA0001

   The CPU_OFF window is then exactly the lines between two markers.

2. A control window. Read tracing is dominated by the SM's steady-state
   polling - ~580 GPC and ~590 SRC reads per thousand lines, forever. An
   equal-length window before the markers has 583/297/294 of them against
   584/296/292 inside, so none of it belongs to the CPU_OFF. Without that
   control the reads read as evidence of a polling loop, which is the first
   thing this looked like.

imx_mu_scmi_request also had to be extended to see this at all: scmi_trace_buf
was set only on sm_mu, Linux's channel, so BL31's own secure agent on
ap_s_mu was invisible and every operation BL31 handles itself read as "the
firmware did nothing". It is now set on both, and the event prints each
request's shared-memory address (shmem=0x44221000 for BL31 on MU1,
shmem=0x445b1000 for Linux on MU2), so the two agents can be told apart.

An UNDEF at EL3 does not show up in any of these traces; add `-d int` and
look for "Undefined Instruction ... from EL3 to EL3".

Finally, QEMU itself is the only vantage that can say where a stopped core is:

    (qmp) human-monitor-command "info registers -a"
    CPU#1  PC=000000008a207b9c  PSTATE=...3cd ---- EL3h

0x8a207b9c was the instruction after the wfi in BL31's panic loop, which is
where a core halted in WFI sits; X30 names the call site. Before reading such
a loop as a deliberate park, disassemble it and check `-d int` for the
exception that led there.
