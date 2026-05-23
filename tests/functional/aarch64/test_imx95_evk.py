#!/usr/bin/env python3
#
# Functional test that boots Linux on imx95-19x19-evk with the real NXP
# System Manager firmware on the Cortex-M33 and checks the console.
#
# The four boot artifacts (aarch64 Linux Image, imx95-19x19-evk.dtb, an
# initramfs cpio.gz, and the NXP System Manager firmware m33_image.elf) are
# built from NXP source trees and are not redistributable as QEMU test
# assets, so the test takes them from environment variables and skips
# cleanly if any are absent. See docs/system/arm/imx95-evk.rst for the
# build recipes.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os

from qemu_test import LinuxKernelTest


class Imx95EvkMachine(LinuxKernelTest):
    """
    Boot stock NXP BSP Linux on imx95-19x19-evk together with the real NXP
    System Manager (SM) firmware on the emulated Cortex-M33. The SM is the
    only SCMI provider on this machine (there is no built-in software SCMI
    server), so a successful boot validates the M33 <-> A55 mailbox
    cross-connect as well as the Linux boot path.
    """

    REQUIRED_ARTIFACTS = (
        ('QEMU_TEST_IMX95_KERNEL', 'aarch64 Linux kernel Image'),
        ('QEMU_TEST_IMX95_DTB',    'imx95-19x19-evk.dtb device tree blob'),
        ('QEMU_TEST_IMX95_INITRD', 'initramfs cpio.gz containing /init'),
        ('QEMU_TEST_IMX95_SM_FW',  'System Manager firmware m33_image.elf'),
    )

    def setUp(self):
        super().setUp()
        self.artifacts = {}
        missing = []
        for envvar, description in self.REQUIRED_ARTIFACTS:
            path = os.environ.get(envvar)
            if path and os.path.exists(path):
                self.artifacts[envvar] = path
            else:
                missing.append(envvar)
        if missing:
            self.skipTest(
                'requires NXP-source-built boot artifacts (see '
                'docs/system/arm/imx95-evk.rst for build recipes); set '
                + ', '.join(missing))

    def test_aarch64_imx95_evk_real_sm(self):
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
            '-kernel', self.artifacts['QEMU_TEST_IMX95_KERNEL'],
            '-dtb',    self.artifacts['QEMU_TEST_IMX95_DTB'],
            '-initrd', self.artifacts['QEMU_TEST_IMX95_INITRD'],
            '-device', 'loader,file=%s,cpu-num=6' %
                       self.artifacts['QEMU_TEST_IMX95_SM_FW'],
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


if __name__ == '__main__':
    LinuxKernelTest.main()
