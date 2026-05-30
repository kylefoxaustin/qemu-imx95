#!/usr/bin/env python3
#
# Functional tests for imx95-19x19-evk: boot Linux on the 6 Cortex-A55s
# with the real NXP System Manager firmware on the Cortex-M33, and exercise
# the System-Manager-orchestrated Cortex-M7 (boot + fault recovery).
#
# The boot artifacts (aarch64 Linux Image, imx95-19x19-evk.dtb, an initramfs
# cpio.gz, the NXP System Manager firmware m33_image.elf, and the M7 fault
# fixture cm7_fault.elf) are built from NXP source trees / the in-tree M7
# firmware and are not redistributable as QEMU test assets, so the tests take
# them from environment variables and skip cleanly if any are absent. See
# docs/system/arm/imx95-evk.rst for the build recipes.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os

from qemu_test import LinuxKernelTest


class Imx95EvkMachine(LinuxKernelTest):
    """
    Boot stock NXP BSP Linux on imx95-19x19-evk together with the real NXP
    System Manager (SM) firmware on the emulated Cortex-M33, and exercise the
    SM-orchestrated Cortex-M7. The SM is the only SCMI provider on this machine
    (there is no built-in software SCMI server), so a successful boot validates
    the M33 <-> A55 mailbox cross-connect as well as the Linux boot path; the
    M7 test validates the SM's ownership of the M7's lifecycle.
    """

    def _require(self, *envvars):
        """Return a dict of resolved artifact paths, or skip if any is unset."""
        artifacts = {}
        missing = []
        for envvar in envvars:
            path = os.environ.get(envvar)
            if path and os.path.exists(path):
                artifacts[envvar] = path
            else:
                missing.append(envvar)
        if missing:
            self.skipTest(
                'requires NXP-source-built boot artifacts (see '
                'docs/system/arm/imx95-evk.rst for build recipes); set '
                + ', '.join(missing))
        return artifacts

    def test_aarch64_imx95_evk_real_sm(self):
        """Linux boots to userspace on the 6 A55s, SCMI served by the real SM."""
        art = self._require('QEMU_TEST_IMX95_KERNEL', 'QEMU_TEST_IMX95_DTB',
                            'QEMU_TEST_IMX95_INITRD', 'QEMU_TEST_IMX95_SM_FW')
        self.require_accelerator("tcg")
        self.set_machine('imx95-19x19-evk')
        # LPUART1 (ttyLP0, index 0) is the Linux console; LPUART2 carries
        # the SM debug monitor and is left null here.
        self.vm.set_console(console_index=0)
        # The machine has a fixed heterogeneous topology of 6 Cortex-A55 +
        # 1 Cortex-M33 (SM) + 1 Cortex-M7 (RT), so do not pass -smp; the
        # machine's default of 8 is the only accepted value.
        self.vm.add_args(
            '-m', '2G',
            '-kernel', art['QEMU_TEST_IMX95_KERNEL'],
            '-dtb',    art['QEMU_TEST_IMX95_DTB'],
            '-initrd', art['QEMU_TEST_IMX95_INITRD'],
            '-device', 'loader,file=%s,cpu-num=6' % art['QEMU_TEST_IMX95_SM_FW'],
            '-append',
            'earlycon=lpuart32,mmio32,0x44380010 '
            'console=ttyLP0,115200 cpuidle.off=1 rdinit=/init',
        )
        self.vm.launch()
        # Load-bearing signal #1: SCMI handshake against the real SM.
        # Confirms the dual-aperture MU2 cross-connect (A55 MUA <-> M33 MUB)
        # is working end-to-end and the SM answered protocol negotiation.
        self.wait_for_console_pattern(
            "SCMI Protocol v2.1 'NXP:IMX' Firmware version 0x333")
        # Load-bearing signal #2: PID 1 ran. The string is printed by the
        # /init in the committed initramfs (tests/busybox-initramfs/build.sh).
        self.wait_for_console_pattern('=== imx95 busybox userspace ===')

    def test_aarch64_imx95_evk_m7_fault_recovery(self):
        """The real SM boots the M7, takes its fault, and cold-resets it.

        No A55/Linux: the SM (M33) and the M7 fault fixture run alone. The
        fixture asserts SYSRESETREQ on its first boot; the SM routes that to
        CM7_SYSRESETREQ_IRQn, runs lm_reset on the M7 logical machine, and
        prints the recovery on its debug monitor (LPUART2). Observing that
        line confirms the whole SM-managed M7 lifecycle - the SM had to learn
        it owns the M7 (from the machine's fabricated boot-ROM handover),
        enable the fault IRQ, and drive the CpuStop/CpuStart cold cycle.
        """
        art = self._require('QEMU_TEST_IMX95_SM_FW', 'QEMU_TEST_IMX95_M7_FW')
        self.require_accelerator("tcg")
        self.set_machine('imx95-19x19-evk')
        # Console on LPUART2 (index 1) = the SM debug monitor; LPUART1 (the
        # A55 console) is left null since no Linux runs here.
        self.vm.set_console(console_index=1)
        self.vm.add_args(
            '-m', '2G',
            '-device', 'loader,file=%s,cpu-num=6' % art['QEMU_TEST_IMX95_SM_FW'],
            '-device', 'loader,file=%s,cpu-num=7' % art['QEMU_TEST_IMX95_M7_FW'],
        )
        self.vm.launch()
        # The SM comes up...
        self.wait_for_console_pattern('SM Debug Monitor')
        # ...then the M7 faults and the SM cold-resets the M7 logical machine.
        self.wait_for_console_pattern('Reset LM 1')


if __name__ == '__main__':
    LinuxKernelTest.main()
