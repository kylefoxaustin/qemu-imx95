.. SPDX-License-Identifier: GPL-2.0-or-later

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

Booting from the card, as the ROM does
'''''''''''''''''''''''''''''''''''''

The ``-device loader`` lines above hand firmware straight to a core, which is
not how a board starts. With **no** ``-kernel``, the machine instead stands in
for the boot ROM: it walks the AHAB container set 32 KiB into the device on
uSDHC1, loads each executable image to the address its container entry names,
and only then starts the M33 and lets the SM release the A55. So a card
carrying a full image set needs nothing else on the command line:

.. code-block:: bash

  $ qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \\
      -drive if=none,format=raw,file=boot.img,id=mmc0 \\
      -device emmc,drive=mmc0 \\
      -serial mon:stdio -serial null

``tests/uboot/mkbootimg.py --sm m33_image.bin --spl u-boot-spl.bin`` builds
such an image; ``tests/uboot/run.sh`` boots one through to a U-Boot prompt and
a byte-exact TFTP transfer. Note that the SM goes in as the raw
``m33_image.bin``, not the ELF: a container entry carries an explicit load
address, which is what the ROM works from.

The ROM reads only images whose container entry declares ``IMG_TYPE_EXEC`` with
an i.MX 9 core id, so media built for the ``-device loader`` flow - where set 0
declares no images at all - is left untouched and boots exactly as it did
before. The walk itself can be followed with
``-D walk.log -trace 'imx95_rom_*'``:

.. code-block:: none

  imx95_rom_container container at 0x8000 hdr_len 0x10 images 0
  imx95_rom_container container at 0x8400 hdr_len 0x110 images 2
  imx95_rom_image load core 1 from 0x9000 to 0x1ffc0000 size 0x2c750
  imx95_rom_image load core 2 from 0x35800 to 0x20480000 size 0x2aaa1

The vendor's EL3 firmware
'''''''''''''''''''''''''

The chain above is the one our own build produces, where U-Boot itself runs at
EL3 and QEMU answers its PSCI SMCs. A board runs firmware below U-Boot, and
that boots here too. Set 1 of an NXP ``imx-boot``/``flash_all`` image holds
three images, not one - BL31 at 0x8a200000, U-Boot at 0x90200000 and OP-TEE at
0x8c000000 - and SPL takes its entry point from image 0, so it enters BL31
rather than U-Boot. Build media the same way and tell the machine that EL3 now
belongs to the firmware:

.. code-block:: bash

  $ python3 tests/uboot/mkbootimg.py u-boot.bin boot.img \\
      --sm m33_image.bin --spl u-boot-spl.bin \\
      --bl31 bl31.bin --optee tee.bin
  $ qemu-system-aarch64 -M imx95-19x19-evk,el3-firmware=on -m 2G \\
      -display none \\
      -drive if=none,format=raw,file=boot.img,id=mmc0 \\
      -device emmc,drive=mmc0 -serial mon:stdio -serial null

``el3-firmware=on`` leaves QEMU's built-in PSCI handler off the SMC conduit so
BL31 owns EL3, as it does on silicon. Without it QEMU answers the SMCs BL31
exists to implement. It cannot be derived: the card is attached after the CPUs
are realized, so nothing can see that set 1 holds a BL31.

The chain then runs ROM -> SM -> SPL -> BL31 (EL3) -> OP-TEE (S-EL1) ->
U-Boot (BL33), which ``tests/uboot/run-tfa.sh`` checks stage by stage. Two
pieces of the model exist only for it:

* **The SM's second SCMI channel.** Its own configuration declares two agents
  for the AP - ``SCMI_AGENT1 name="AP-S", secure`` on MU1 and
  ``SCMI_AGENT2 name="AP-NS"`` on MU2 - and TF-A is the secure one. MU1 is
  cross-connected like MU2, with one shared SMT page behind two apertures.
* **Two more ELE services.** BL31 uses the ELE mailbox at 0x47540000, which is
  neither of the two U-Boot uses, and OP-TEE's RNG init polls
  ``ELE_CMD_TRNG_STATE`` until both of its state bytes report ready.

BL31 and OP-TEE are NXP BSP artifacts and are not redistributable, so they are
operator-supplied; both can be taken out of a BSP ``imx-boot`` image, which is
a container set in the format ``mkbootimg.py`` documents.

One thing this does not yet give you: a released BSP image is built for
silicon revision 2.0 or later, so its containers are 0x4000-aligned and carry
version-2 headers, while this machine reports rev 1.1 and both it and SPL walk
a set with the 0x400 stride ``is_imx95_a0()`` selects - so such an image has to
be rebuilt rather than used as it is.

Linux SMP on the secure chain
'''''''''''''''''''''''''''''

``tests/uboot/run-tfa-linux.sh`` carries that same image set up into Linux and
checks that every A55 comes online. Nothing in QEMU starts them: Linux issues a
PSCI CPU_ON, BL31 answers it, BL31 asks the SM over the secure channel, and the
SM programs ``CA55_RVBARADDR<n>`` and clears that core's ``CA55_CPUWAIT`` bit.
The machine turns the released bit into a reset of that core at the vector it
was handed, which is both where it starts and how it comes up at EL3, where
BL31's warm-boot path expects to be entered.

Two details of the model are load-bearing for this, and both were silent until
real firmware asked:

* **All six CPUWAIT bits.** Only core 0's gate was wired, because on the
  non-secure path QEMU's own PSCI starts the secondaries and the register is
  never read.
* **MPIDR.MT.** The six A55s are one DynamIQ cluster, so the core number is in
  Aff1 and Aff0 is a thread level - and MPIDR.MT is what tells a reader so.
  The affinity fields here matched the DTS while that bit stayed clear, which
  none of Linux, U-Boot or TF-A objects to, because each numbers cores its own
  way. OP-TEE's generic ``get_core_pos_mpidr()`` does object: with MT clear it
  re-normalises the MPIDR as though Aff0 held the core, derives core positions
  0, 4, 8, ..., and parks every core whose position lands past
  ``CFG_TEE_CORE_NB_CORE`` in its ``unhandled_cpu`` loop. What that looked like
  from the console was five cores timing out in Linux with "failed to come
  online", with the first parked core still holding a BL31 spinlock the rest
  were waiting on.

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
