A55 CPU hotplug under the EL3 firmware chain
============================================

run.sh offlines each A55 secondary and brings it back. It FAILS by default,
where PSCI is answered by TF-A BL31 (el3-firmware=on):

    psci: CPU1 may not have shut down cleanly (AFFINITY_INFO reports 0)
    CPU1 may not have shut down cleanly: -110
    psci: failed to boot CPU1 (-22)

Uniform across cpu1..cpu5, so it is not one core's MPIDR or index. Offlining
all five leaves the board on one CPU with no way back.

`EL3=off ./run.sh` runs the same sweep on the same U-Boot chain with QEMU's own
PSCI and PASSES all twelve checks (`psci: CPUn killed (polled 0 ms)`). That is
the control, and it carries two loads: the sweep and this script can go green,
so a permanently-red test is not being mistaken for an unpassable one; and the
model's own core start/stop machinery is sound, so the defect is confined to
the BL31 path.


What this is NOT
----------------

It is not a missing SRC or GPC wire in this model, which is what the model's
own comment in fsl_imx95_a55_run_handler() used to assert: that an A55 is
stopped through its SRC per-core reset line, which the model ignores. That was
inference, and it is wrong. Measured, with the markers and controls below:

  * no write to any CORTEXMIX_COREn SLICE_SW_CTRL reset line (RST_RSTR_0),
  * no write to CA55_CPUWAIT,
  * no write anywhere in the GPC,
  * no SCMI CPU_STOP (protocol 0x82, message 0x05) - not in the CPU_OFF
    window, not anywhere in the whole run,
  * and no reads of any of it either, beyond the System Manager's own
    background polling, whose rate is indistinguishable from the control.

BL31 does not ask the hardware to stop an A55. It parks the core itself: CPU1
ends up halted at EL3 inside BL31's "wfi; b ." loop, which is what a stopped
core looks like on silicon too. The core really does stop. So there is no
reset line to wire here, and wiring one would be modelling a transaction that
no firmware in this chain performs.


What IS broken
--------------

BL31 never publishes AFF_STATE_OFF for the core it just parked. Linux polls
AFFINITY_INFO ten times at 10 ms (hence -110 after ~100 ms), always gets ON,
and the later CPU_ON is refused with -22 - ALREADY_ON, decided by BL31 locally
without a single MMIO access or SCMI message. Waiting longer does not help:
45 s after the offline the state is still ON.

That is inside BL31 (v2.14.1, lf-6.18.20-2.0.0-rc1, built Jun 8 2026), whose
source is not in this tree, so the next step needs TF-A's imx95 platform code
- specifically its pwr_domain_off - and not more work on this model. Note that
CPU_ON at boot *does* go through the SM: five SCMI CPU_START messages
(0x82/0x04) and six CPU_RESET_VECTOR_SET (0x82/0x06) are visible at SMP
bringup. So BL31 uses the SM to start a core and not to stop one.


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
firmware did nothing". It is now set on both; that change is what made the 45
CPU-protocol messages above visible.

Finally, QEMU itself is the only vantage that can say where a stopped core is:

    (qmp) human-monitor-command "info registers -a"
    CPU#1  PC=000000008a207b9c  PSTATE=...3cd ---- EL3h

0x8a207b9c is the instruction after the wfi in BL31's park loop, which is
where a core halted in WFI sits. X30=0x8a207288 names the call site.
