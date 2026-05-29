# tests/spl-banner

End-to-end smoke test for v0.1: build NXP's U-Boot SPL for
`imx95-19x19-evk` and run it on the `imx95-19x19-evk` QEMU machine
type until it prints its banner over LPUART1.

## Why a banner is the milestone

Getting the banner means every piece of the v0.1 stack works:
real ARM64 instructions on the emulated A55, the MU device,
the SCMI server stub handling base / clock / pinctrl messages,
the ELE server stub handling `ele_get_info()`, the ULP watchdog
stub satisfying `disable_wdog()`, the OCRAM region covering SPL's
TEXT base through its BSS+stack, the SMMU stub for
`disable_smmuv3()`, the GPIO stubs for `gpio_reset()`, the LPUART
model itself running at 24 MHz with no real baud-rate divider,
and the chardev plumbing from the LPUART up to the host terminal.

## Host packages

On top of the QEMU build deps (see top-level README):

    sudo apt install -y gcc-aarch64-linux-gnu \
                        bison flex libssl-dev libgnutls28-dev \
                        efitools

The `efitools` package is needed for `cert-to-efi-sig-list` which
U-Boot's full build invokes for EFI Capsule images; the SPL itself
does not use it but the build fails without it.

## Build U-Boot SPL

From the repo root:

    mkdir -p tests/spl-banner/uboot-build
    make -C references/uboot-imx \
        O=$PWD/tests/spl-banner/uboot-build \
        imx95_19x19_evk_defconfig \
        CROSS_COMPILE=aarch64-linux-gnu-

    make -C references/uboot-imx \
        O=$PWD/tests/spl-banner/uboot-build \
        CROSS_COMPILE=aarch64-linux-gnu- -j4

Warnings about missing `mx95a0-ahab-container.img`, `bl31.bin`,
`oei-m33-tcm.bin`, and `m33_image.bin` are expected - those blobs
are for assembling the full silicon-bootable container image
(`flash.bin`), which we do not use. The SPL binary itself
(`tests/spl-banner/uboot-build/spl/u-boot-spl.bin`, ~170 KiB) is
all we need.

## Run

    ./build/qemu-system-aarch64 \
        -M imx95-19x19-evk \
        -nographic \
        -m 2G \
        -device loader,file=tests/spl-banner/uboot-build/spl/u-boot-spl.bin,addr=0x20480000,cpu-num=0,force-raw=on

`-device loader` is used instead of `-kernel` because U-Boot SPL
is linked to `CONFIG_SPL_TEXT_BASE = 0x20480000` (the OCRAM-A
region), not the DRAM base where `-kernel` would land it.

Quit with `Ctrl-A x`.

## Expected output

After a few seconds (SCMI clock probe registers ~80 stub clocks
synchronously which takes some boot time on TCG), SPL prints:

    U-Boot SPL 2025.04-g99518e6b6f20 (May 15 2026 - 12:37:01 -0500)
    get_reset_reason:-1 for SYS
    Normal Boot

then proceeds into post-banner work. As of the v0.2 frontier
landing the imx-misc SCMI protocol, the uSDHC1/2/3 controllers,
and the SDHCI clock-path fix, SPL completes MMC bring-up and
moves on to the AHAB-container parse step:

    Trying to boot from MMC1
    Boot stage: USB Serial Download
    Image set: 0, offset: 0x0
    Parse seco container failed -14
    SPL: failed to boot from all boot devices

That's the expected end-state without an SD image: the controller
talks to the (anonymous) bus but there is no card, so the read
fails. With an SD image attached (see "Booting from SD" below)
SPL gets through `mmc init`, reads sector 0, and rejects the
all-zero data as not a valid container. Closing the v0.2
milestone needs an actual NXP AHAB / SECO container on the SD
image at offset 32 KiB (`CONTAINER_HDR_MMCSD_OFFSET`).

## Booting a payload from SD

`tests/sd-boot/build-container.py` wraps an arbitrary raw binary in
the SECO / V2X / SCU / U-Boot-proper container chain that SPL
expects on imx95-A0 silicon (see the script header for the SD
layout). With a real payload landing at the right offset, SPL
parses through SECO->V2X->SCU and then loads + jumps into the
target.

End-to-end demo using the existing `tests/hello-imx95/hello.bin`
as a placeholder payload (it just prints "Hello from i.MX 95!"
on LPUART1 and halts):

    ./tests/sd-boot/build-container.py \
        --payload tests/hello-imx95/hello.bin \
        --dst-addr 0x90200000 \
        --entry-addr 0x90200000 \
        --output tests/sd-boot/sd.img

    ./build/qemu-system-aarch64 \
        -M imx95-19x19-evk \
        -nographic \
        -m 2G \
        -device loader,file=tests/spl-banner/uboot-build/spl/u-boot-spl.bin,addr=0x20480000,cpu-num=0,force-raw=on \
        -drive if=none,format=raw,file=tests/sd-boot/sd.img,id=sd0 \
        -device sd-card,drive=sd0

Expected end-state for the v0.2 milestone:

    U-Boot SPL 2025.04-... (May 15 2026 - 12:37:01 -0500)
    Normal Boot
    Trying to boot from MMC1
    Boot stage: Primary
    Image set: 0, offset: 0x8000
    Load image from MMC/SD 0x8c00
    Hello from i.MX 95!

The bare `-device sd-card,drive=sd0` (no explicit `bus=`) lands
on uSDHC1's anonymous sd-bus, because `fsl-imx95.c` realises the
three uSDHC instances in reverse order so usdhc1 ends up at the
top of the unnamed-bus stack — see the comment in
`fsl_imx95_realize()`.

Pointing the payload at U-Boot proper (`u-boot.bin`) currently
does *not* reach the U-Boot prompt: the SD read stalls at ~512 KiB
into the 1.3 MiB transfer (still under investigation; tracked in
`docs/imx95/todo.md` under "Before v0.3").

## Loading SPL via `-device loader`

The build outputs three SPL binaries:

  - `u-boot-spl-nodtb.bin` (~160 KiB): raw SPL code only
  - `u-boot-spl.bin` (~170 KiB): SPL code + appended DTB (this is
    what runs)
  - `u-boot-spl-dtb.bin`: same as above (alias)

`u-boot-spl.bin` is what `-device loader` should point at - the
SPL relies on its embedded DTB for DM initialisation, and that DTB
contains the `bootph-pre-ram`-marked nodes that DM binds in
pre-relocation phase.

## Cleaning

    make -C references/uboot-imx O=$PWD/tests/spl-banner/uboot-build mrproper
    rm -rf tests/spl-banner/uboot-build
