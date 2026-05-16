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

then proceeds into post-banner work which is **v0.2 scope** and
not yet handled, so SPL will print error messages and reset:

    Failed to get ROM passover data, scmi_err = -1, size_of(out) = 68
    SCMI: failure at rom_boot_info
    SPL: failed to boot from all boot devices
    ### ERROR ### Please RESET the board ###

That's the expected end-state for v0.1: banner reached, then
graceful failure where the next milestone picks up (SCMI vendor
protocol 0x84 for `rom_boot_info`, plus a uSDHC/eMMC model so
SPL can actually load the next stage).

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
