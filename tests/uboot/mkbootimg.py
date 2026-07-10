#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a raw boot image the i.MX 95 SPL can load U-Boot proper from.

On silicon the boot ROM reads a flash.bin containing several i.MX containers,
records where it found them in the ROM passover table, and hands off to SPL.
SPL then re-reads the device to find where the *first* image set ends, and
loads the *second* container (U-Boot proper) from there.

Our machine writes the passover itself (fsl_imx95_write_rom_passover), which
reports boot device eMMC/uSDHC1 and img_ofs = 0x8000. So this image needs:

SPL walks image set 0 as up to three containers (ELE, v2x, SPL) before deciding
where the set ends, stepping by container_hdr_alignment() - 0x400 here. So set 0
needs two header-only containers, or SPL parses U-Boot's container as part of
set 0 and looks for the next set past the end of U-Boot itself.

  0x8000  container A - header only. get_container_size() -> 16, so the v2x
          probe steps to ALIGN(16, 0x400) + 0x8000 = 0x8400.
  0x8400  container C - header only. Parses, is not v2x firmware, so SPL takes
          it as the last container of set 0: end = 16 + 0x8400 = 0x8410,
          rounded up to 1 KiB -> 0x8800.
  0x8800  container B - one image entry pointing at U-Boot proper.
  0x8c00  U-Boot proper (image offset 0x400 within container B; SPL requires
          image offsets to be block-aligned).

No signature block, no AHAB: hab_flags = 0 and sig_blk_offset = 0, which is
what an unsigned development container looks like.

usage: mkbootimg.py <u-boot.bin> <out.img> [--text-base 0x90200000]
"""
import argparse
import struct
import subprocess
import sys

CONTAINER_TAG = 0x87
CONTAINER_VERSION = 0
HDR_SIZE = 16
IMG_ENTRY_SIZE = 128

SET_A_OFF = 0x8000        # must match PASSOVER_IMG_OFS in hw/arm/fsl-imx95.c
SET_C_OFF = 0x8400        # ALIGN(16, container_hdr_alignment()) + SET_A_OFF
SET_B_OFF = 0x8800        # ROUND(SET_C_OFF + 16, 1 KiB)
IMG_OFF_IN_B = 0x400      # block-aligned, as read_auth_image() demands
IMAGE_SIZE = 64 << 20


def container_hdr(length, num_images):
    """version, length_lsb, length_msb, tag, flags, sw_version,
       fuse_version, num_images, sig_blk_offset, reserved"""
    return struct.pack('<BBBBIHBBHH',
                       CONTAINER_VERSION,
                       length & 0xff, (length >> 8) & 0xff,
                       CONTAINER_TAG,
                       0,          # flags
                       0,          # sw_version
                       0,          # fuse_version
                       num_images,
                       0,          # sig_blk_offset - unsigned container
                       0)


def boot_img(offset, size, dst, entry):
    """offset, size, dst, entry, hab_flags, meta, hash[64], iv[32]"""
    return (struct.pack('<IIQQII', offset, size, dst, entry, 0, 0)
            + b'\0' * 64 + b'\0' * 32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('uboot')
    ap.add_argument('out')
    ap.add_argument('--text-base', default='0x90200000')
    ap.add_argument('--uboot-elf',
                    help='u-boot ELF; if given, the OP-TEE device-tree fixup '
                         'is neutralised (see --no-optee)')
    ap.add_argument('--no-optee', action='store_true',
                    help='Build boot media for a U-Boot with no OP-TEE. The '
                         'vendor imx95_evk U-Boot fabricates an optee '
                         'reserved-memory node from rom_pointer[], which SPL '
                         'only fills when it loads OP-TEE as BL32. With no '
                         'OP-TEE those registers are garbage and the node '
                         'reserves nonsense, panicking Linux. This machine '
                         'does not model OP-TEE, so patch ft_add_optee_node to '
                         'return early - exactly what CONFIG_OPTEE=n does.')
    args = ap.parse_args()

    text_base = int(args.text_base, 0)
    with open(args.uboot, 'rb') as f:
        uboot = bytearray(f.read())

    if args.no_optee:
        if not args.uboot_elf:
            sys.exit('--no-optee requires --uboot-elf to locate the symbol')
        out = subprocess.check_output(
            ['aarch64-linux-gnu-nm', args.uboot_elf], text=True)
        sym = [l for l in out.splitlines() if l.endswith(' ft_add_optee_node')]
        if not sym:
            sys.exit('ft_add_optee_node not found in ' + args.uboot_elf)
        off = int(sym[0].split()[0], 16) - text_base
        if not 0 <= off < len(uboot) - 8:
            sys.exit('ft_add_optee_node offset 0x%x out of range' % off)
        # mov w0, #0 ; ret  -> the function becomes "return 0"
        uboot[off:off + 8] = struct.pack('<II', 0x52800000, 0xd65f03c0)
        print('no-optee: patched ft_add_optee_node at u-boot+0x%x' % off)

    img = bytearray(b'\0' * IMAGE_SIZE)

    # Set 0: two header-only containers, so SPL's walk ends before container B.
    img[SET_A_OFF:SET_A_OFF + HDR_SIZE] = container_hdr(HDR_SIZE, 0)
    img[SET_C_OFF:SET_C_OFF + HDR_SIZE] = container_hdr(HDR_SIZE, 0)

    # Container B: one image, U-Boot proper.
    hdr_b = container_hdr(HDR_SIZE + IMG_ENTRY_SIZE, 1)
    ent_b = boot_img(IMG_OFF_IN_B, len(uboot), text_base, text_base)
    img[SET_B_OFF:SET_B_OFF + HDR_SIZE] = hdr_b
    img[SET_B_OFF + HDR_SIZE:SET_B_OFF + HDR_SIZE + IMG_ENTRY_SIZE] = ent_b

    data_off = SET_B_OFF + IMG_OFF_IN_B
    end = data_off + len(uboot)
    if end > IMAGE_SIZE:
        sys.exit(f'u-boot too large: needs {end} bytes, image is {IMAGE_SIZE}')
    img[data_off:end] = uboot

    with open(args.out, 'wb') as f:
        f.write(img)

    print(f'{args.out}: set0 containers @0x{SET_A_OFF:x},0x{SET_C_OFF:x}; '
          f'u-boot container @0x{SET_B_OFF:x}; '
          f'u-boot ({len(uboot)} bytes) @0x{data_off:x} -> 0x{text_base:x}')


if __name__ == '__main__':
    main()
