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
    args = ap.parse_args()

    text_base = int(args.text_base, 0)
    with open(args.uboot, 'rb') as f:
        uboot = f.read()

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
