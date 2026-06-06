NXP i.MX 95 19x19 Evaluation Kit (``imx95-19x19-evk``)
======================================================

The ``imx95-19x19-evk`` machine models the NXP i.MX 95 19x19 LPDDR5
Evaluation Kit. The i.MX 95 is a heterogeneous SoC; the machine
correspondingly emulates a fixed topology of **6 Cortex-A55 application
cores, 1 Cortex-M33** that runs the NXP **System Manager** (SM)
firmware, **and 1 Cortex-M7** real-time core that the System Manager
boots, manages and fault-recovers (see ``Cortex-M7 real-time core``
below).

A defining property of this SoC is that the System Manager firmware is
the only SCMI provider: Linux's clock, perf-domain, power, sensor, and
reset operations are all served by the SM running on the M33 over a
shared mailbox (MU2), not by software inside QEMU. Booting Linux to
userspace on this machine therefore requires the SM firmware as an
input — see ``Required artifacts`` below.

Supported devices
-----------------

The ``imx95-19x19-evk`` machine implements the following devices:

 * 6 Cortex-A55 application cores
 * 1 Cortex-M33 (System Manager core; runs NXP SM firmware)
 * 1 Cortex-M7 (real-time core; SM-booted and SM-managed)
 * Generic Interrupt Controller (GICv3, GIC-600 layout)
 * LPUART serial controllers (LPUART1 = Linux console; LPUART2 = SM
   debug monitor; LPUART3 = M7 console)
 * NXP Messaging Unit v2 (MU2) with the dual-aperture A55-MUA <->
   M33-MUB cross-connect that carries SCMI; plus MUI_A5 (M7 <-> SM
   SCMI) and MU7 (A55 <-> M7 rpmsg) cross-connects
 * BLK_CTRL_S_AONMIX M7 CPU-WAIT gate (the SM's M7 hold/run control)
 * System Counter (24 MHz live up-counter with compare-match IRQ; the
   Linux broadcast clockevent on this machine)
 * uSDHC storage controller (SDMA, used by U-Boot SD boot)
 * LPI2C masters (interrupt-driven; used by the SM for PMIC / IO
   expander bring-up, and by Linux for the wm8962 audio codec and the
   USB-VBUS / audio-power IO expanders)
 * NETC Ethernet: a functional GICv3 ITS, an integrated-ECAM PCIe host,
   and three from-scratch ENETC v4 Ethernet PFs (``hw/net/fsl_enetc.c``,
   PCI ``1131:e101``) with BD-ring DMA and MSI-X via the ITS; the stock
   ``nxp_enetc4`` driver brings up two 1G ports (``eth0`` / ``eth1``)
   and the 10G ``eth2`` (fixed-link ``10gbase-r``)
 * FlexCAN: all five controllers on QEMU's CAN bus subsystem (real
   frame TX/RX, validated with the Linux ``flexcan`` driver)
 * usb2 ChipIdea USB host (``-device usb-kbd`` enumerates as a USB HID
   keyboard; the VBUS PCAL6524 expander on LPI2C is modelled)
 * DPU display: the primary-plane FetchLayer scans a framebuffer out of
   guest DRAM to a QEMU console, and the FrameGen vblank / shadow-load
   interrupts are funnelled through a ``fsl,imx-irqsteer`` model, so the
   ``dpu95`` driver binds, atomic commits complete on interrupt, and the
   kernel boot logo renders at 1920x1200 on the LVDS panel timing
 * Audio: eDMA (``hw/dma/imx95_edma.c``), SAI and MICFIL front-ends, and
   a WM8962 I2C codec, so all three EVK ASoC cards register
   (``wm8962-audio``, ``micfil-audio``, ``bt-sco-audio``)
 * HW JPEG codecs (``mxc-jpeg``) register as V4L2 mem2mem devices
   ``/dev/video2`` (decode) and ``/dev/video3`` (encode)
 * Functional models for the SM bring-up path: ANATOP (PLL lock and
   DFS), SRC (system reset controller), GPC (general power
   controller), ELE responder, FSB (fuse shadow block), eMcem, PF53
   and PCAL6408A on LPI2C
 * Logging stubs for blocks that are not on the boot critical path or
   that have no behaviour to emulate (Wave6 VPU, GPU/NPU MMIO, PCIe
   link training, the camera ISI/ISP, watchdog, etc.)

The Mali-G310 (GPU), Wave6 (VPU), and Neutron (NPU) are intentionally
probe-time stubs only; no rendering, codec, or inference occurs. The DPU
and the HW JPEG / audio paths are modelled to their registration bar (the
drivers bind and the devices enumerate, and the DPU additionally renders
the boot logo), but functional compositing, playback / capture and codec
encode-decode are not driven. See ``Caveats`` below.

Boot options
------------

The ``imx95-19x19-evk`` machine can start a Linux kernel directly
using ``-kernel``, but **only together with the System Manager firmware
loaded onto the Cortex-M33** via ``-device loader,...,cpu-num=6``.
Booting Linux without the SM firmware leaves the M33 halted, nothing
answers SCMI, and Linux will hang at ``arm-scmi`` probe.

Required artifacts
''''''''''''''''''

Four artifacts are needed; all are built from NXP source trees:

* **System Manager firmware** (``m33_image.elf``). Built from the NXP
  ``imx-sm`` source tree, with the ``mx95evk`` board configuration:

  .. code-block:: bash

     git clone https://github.com/nxp-imx/imx-sm.git
     cd imx-sm
     make config=mx95evk
     # Result: build/mx95evk/m33_image.elf

* **aarch64 Linux kernel** (``Image``). Either the NXP BSP kernel
  (e.g. ``linux-imx`` 6.12.49) built with the NXP defconfig, or a
  mainline kernel built with the standard arm64 defconfig — both have
  been tested.

* **Device tree blob** (``imx95-19x19-evk.dtb``). Produced by the same
  kernel build under
  ``arch/arm64/boot/dts/freescale/imx95-19x19-evk.dtb``.

* **Initramfs** (``*.cpio.gz``) containing an ``/init``. Any aarch64
  initramfs will do; the project ships a script that builds a static
  BusyBox initramfs at ``tests/busybox-initramfs/build.sh`` (it
  downloads Ubuntu's prebuilt static-aarch64 BusyBox; no cross
  compiler required on the host).

Direct Linux Kernel Boot
''''''''''''''''''''''''

Once the four artifacts are built, boot Linux to userspace as
follows:

.. code-block:: bash

  $ qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \\
      -kernel Image \\
      -dtb imx95-19x19-evk.dtb \\
      -initrd initramfs.cpio.gz \\
      -device loader,file=m33_image.elf,cpu-num=6 \\
      -append "earlycon=lpuart32,mmio32,0x44380010 \\
               console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \\
      -serial mon:stdio -serial null

On a successful boot, Linux negotiates SCMI with the real SM
(``SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333``), brings up
all 6 A55 cores via PSCI, mounts the initramfs, and starts ``/init``.

LPUART1 (``ttyLP0``) is Linux's console; it is wired to the first
``-serial`` backend above. LPUART2 carries the System Manager's own
debug monitor; it is wired to the second ``-serial`` backend
(``null`` above), and can be redirected to a separate file or pty if
desired.

System Manager standalone
'''''''''''''''''''''''''

To exercise the SM in isolation (no Linux), boot it with LPUART2 as
the only console and stop at the SM debug monitor prompt:

.. code-block:: bash

  $ qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \\
      -device loader,file=m33_image.elf,cpu-num=6 \\
      -serial null -serial mon:stdio

Cortex-M7 real-time core
''''''''''''''''''''''''

The i.MX 95 System Manager owns the Cortex-M7's whole lifecycle. Unlike a
generic remoteproc model where Linux starts the M-core, on this SoC the SM
boots the M7 **before** the A-cluster and the AP logical machine does not own
the M7; Linux's ``imx_rproc`` driver therefore only *attaches* to the
already-running M7 (kernel log: ``imx-rproc imx95-cm7: lmm(1) not under Linux
Control``).

Load M7 firmware onto the M7 (CPU index 7) with a second loader device:

.. code-block:: bash

  $ qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \\
      -device loader,file=m33_image.elf,cpu-num=6 \\
      -device loader,file=cm7_firmware.elf,cpu-num=7 \\
      ... (kernel/dtb/initrd as above for a full Linux + M7 boot)

When the SM firmware is present it boots the M7 itself: the machine fabricates
the boot-ROM image handover the SM reads at startup to learn it owns the M7
(real hardware gets this from the boot ROM; the direct ``-device loader`` path
bypasses it), so the SM runs its full ``DEV_SM_CpuStart`` for the M7. With no SM
loaded, the M7 is released directly at machine-init-done if its reset vector was
populated. The M7's console is LPUART3.

Two SM-managed behaviours are modelled:

* **rpmsg.** Once Linux has attached, the stock NXP ``imx_rpmsg_pingpong``
  kernel module exchanges 100 messages with an M7 rpmsg-lite client over the
  MU7 cross-connect (doorbell kicks) and shared vrings (payload) in the M7 DRAM
  carveout, printing ``goodbye!`` after the final round-trip. The
  ``Cortex-M7 rpmsg ping/pong`` functional test below exercises this end to
  end (see also ``tests/m7-rpmsg/``).

* **Fault recovery.** The M7 logical machine is configured ``reaction=lm_reset``,
  so an M7 fault triggers an SM-driven cold reset of the M7. When the M7 asserts
  ``SYSRESETREQ`` it is routed to the SM as ``CM7_SYSRESETREQ_IRQn``; the SM runs
  ``LMM_SystemLmReset`` (stop then start the M7), printing ``Reset LM 1`` on its
  debug monitor. The ``Cortex-M7 fault recovery`` functional test below
  exercises this end to end.

Caveats
-------

* **Deep cpuidle requires** ``cpuidle.off=1`` **on the kernel command
  line.** Linux's ``cpu-pd-wait`` idle state quiesces the per-CPU GIC
  CPU interface before WFI; QEMU's GICv3 model has no WakeRequest
  path, so a halted CPU is not woken by a pending interrupt while its
  interface is disabled. The kernel-command-line workaround is the
  v1.0 recommendation; the deeper fix needs upstream QEMU work in
  ``hw/intc/arm_gicv3_cpuif.c``.

* **No hardware-accelerated rendering, codec, or inference.** Mali-G310,
  Wave6 VPU, and Neutron NPU are logging stubs. The Mali kernel
  driver in particular fails probe with ``-22`` at the product-ID
  check (``Unknown GPU Product ID 0``), so no Mali Vulkan ICD is
  installed and ``vulkaninfo`` reports
  ``ERROR_INCOMPATIBLE_DRIVER``.

* **Display is scanout-only.** The DPU models the primary-plane
  FetchLayer + FrameGen, so ``dpu95`` binds and the kernel boot logo
  renders to a QEMU console (the FrameGen vblank is driven through a
  modelled ``fsl,imx-irqsteer``, so atomic commits complete on
  interrupt instead of timing out). Multi-plane compositing, the
  DSI / LVDS-LDB bridge timing, and the camera ISI/ISP are not
  modelled. The HW JPEG codecs register as V4L2 m2m devices and the
  three audio cards register, but the JPEG/VPU codec engines and the
  SAI/MICFIL playback-capture datapath are not driven, so a
  ``STREAMON`` binds but no frames / samples move.

* **Networking works; no Linux block storage.** The three NETC ENETC
  ports bring up ``eth0`` / ``eth1`` (1G) and ``eth2`` (10G) (attach a
  host backend with ``-nic`` / ``-netdev``). Linux's ``sdhci-esdhc-imx``
  driver, however,
  still defers probe on an unmet dependency, so ``/dev/mmcblk*`` is
  empty even with an SD-card image attached via ``-drive ...,if=sd``
  (the uSDHC model itself works — U-Boot SPL boots from SD).

* **icount is not the default boot mode.** ``-icount shift=auto``
  delays interrupt/timer delivery to idle CPUs on this heterogeneous
  A55+M33 machine, where both sides can WFI between SCMI/MU
  round-trips, inflating the boot ~9x. Enable icount only when
  debugging a race; do not use it for normal boots.

Functional test
---------------

``tests/functional/aarch64/test_imx95_evk.py`` has three env-gated cases; each
takes its artifacts from environment variables and skips cleanly (naming the
unset variables) if any are absent, since the NXP-source-built artifacts are
not redistributable:

* **Linux to userspace on the real SM** — needs ``QEMU_TEST_IMX95_KERNEL``,
  ``QEMU_TEST_IMX95_DTB``, ``QEMU_TEST_IMX95_INITRD`` and
  ``QEMU_TEST_IMX95_SM_FW``. Boots Linux and checks for the SCMI handshake
  followed by an init-printed userspace banner (~14 s on a development host).

* **Cortex-M7 rpmsg ping/pong** — needs ``QEMU_TEST_IMX95_KERNEL``,
  ``QEMU_TEST_IMX95_DTB``, ``QEMU_TEST_IMX95_SM_FW``,
  ``QEMU_TEST_IMX95_M7_RPMSG_FW`` (the M7 rpmsg-lite pingpong firmware) and
  ``QEMU_TEST_IMX95_RPMSG_INITRD`` (an initramfs bundling the matching
  ``imx_rpmsg_pingpong.ko`` and an ``/init`` that loads it). Boots the full
  A55 + SM + M7 stack and checks for the SCMI handshake followed by the
  module's ``goodbye!`` line — printed only after the 100-message exchange
  with the M7 over MU7 completes. The M7 firmware is NXP's
  ``rpmsg_lite_pingpong_rtos_linux_remote`` multicore example; the NXP BSP
  ships it prebuilt in the target rootfs at
  ``/usr/lib/firmware/imx95-19x19-evk_m7_TCM_rpmsg_lite_pingpong_rtos_linux_remote.{elf,bin}``,
  or it can be built from the MCUXpresso SDK multicore examples. See
  ``tests/m7-rpmsg/`` for a standalone harness and an initramfs recipe.

* **Cortex-M7 fault recovery** — needs ``QEMU_TEST_IMX95_SM_FW`` and
  ``QEMU_TEST_IMX95_M7_FW`` (an M7 fixture that asserts ``SYSRESETREQ`` on its
  first boot; build ``cm7_fault.elf`` via ``make -C tests/cm7-hello``). Boots
  the SM + M7 alone and checks the SM debug monitor for ``Reset LM 1`` — the
  SM taking the M7 fault and cold-resetting the M7 logical machine.

.. code-block:: bash

  $ QEMU_TEST_IMX95_SM_FW=/path/to/m33_image.elf \\
    QEMU_TEST_IMX95_M7_FW=/path/to/cm7_fault.elf \\
    meson test -C build --suite thorough func-aarch64-imx95_evk
