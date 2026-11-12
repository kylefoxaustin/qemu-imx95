#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a raw boot image for the i.MX 95, in either of two shapes.

On silicon the boot ROM reads a flash.bin containing several i.MX containers,
loads the images they declare, records where it found them in the ROM passover
table, and hands off to SPL. SPL then re-reads the device to find where the
*first* image set ends, and loads the *second* container (U-Boot proper) from
there.

--- Default: U-Boot only (the pre-ROM-boot shape) ---

Here SPL and the SM arrive through "-device loader", so set 0 carries no
images at all and exists only to tell SPL where it ends. Our machine writes
the passover itself (fsl_imx95_write_rom_passover), reporting boot device
eMMC/uSDHC1 and img_ofs = 0x8000, so this image needs:

  0x8000  container A - header only. get_container_size() -> 16, so the v2x
          probe steps to ALIGN(16, 0x400) + 0x8000 = 0x8400.
  0x8400  container C - header only. Parses, is not v2x firmware, so SPL takes
          it as the last container of set 0: end = 16 + 0x8400 = 0x8410,
          rounded up to 1 KiB -> 0x8800.
  0x8800  container B - one image entry pointing at U-Boot proper.
  0x8c00  U-Boot proper (image offset 0x400 within container B; SPL requires
          image offsets to be block-aligned).

--- With --sm and --spl: a full image set the emulated ROM loads ---

Set 0 now carries the two images the ROM is responsible for, so no
"-device loader" is needed for either. SPL's walk is unchanged - it still
finds the end of set 0 at the container at 0x8400 - but that container now
declares images instead of being empty:

  0x8000  container A - header only, standing in for the ELE container.
  0x8400  container 0 - two image entries: the SM for the M33 and SPL for the
          A55. Header length 0x10 + 2 * 0x80 = 0x110.
  0x9000  payloads, at offset 0xc00 within that container. 0xc00 rather than
          0x400 so the byte at 0x8800 - where a third container header would
          sit, and where both the ROM and SPL probe for one - stays zero, and
          the walk ends there for a definite reason rather than because a
          payload happened not to look like a container.
  ...     container B (set 1) at ROUND(end of set 0, 1 KiB), exactly where
          SPL's get_imageset_end() will look for it.

--- With --bl31: TF-A below U-Boot ---

Set 1 then carries two images, BL31 first. SPL loads every image in the
container but takes its entry point from image 0 (common/spl/spl_imx_container.c:
`if (i == 0) spl_image->entry_point = image->entry`), so BL31 runs at EL3 and
hands off to U-Boot as BL33 - the order the vendor's own flash.bin uses.

BL31's load address 0x8a200000 is below U-Boot's CFG_SYS_SDRAM_BASE
(0x90000000), so SPL's arch_check_dst_in_secure() calls it secure and stages
the read through the trampoline at CFG_SYS_SDRAM_BASE + PHYS_SDRAM_SIZE - 16M
= 0xff000000 before copying it down. Both addresses are inside the RAM this
machine maps from 0x80000000, so `-m 2G` is enough.

The machine must also be told to stop emulating PSCI, or QEMU answers the SMCs
that belong to BL31: -machine imx95-19x19-evk,el3-firmware=on.

--- With --optee as well ---

The BSP's BL31 is built with a secure-payload dispatcher and hands off to
BL32 before BL33. Give it --optee too, or it enters S-EL1 at address 0 and
loops in its own vector table (PC stuck at VBAR_EL1 + 0x200). Set 1 then
holds the same three images, in the same order, as the vendor's flash.bin:
BL31, U-Boot, OP-TEE.

Image flags follow U-Boot's tools/imx8image.c for an i.MX 9 target:
IMG_TYPE_EXEC (3) in bits[3:0] and the core in bits[7:4] - CORE_ULP_CM33 (1)
for the M33, CORE_ULP_CA35 (2) for the A55.

No signature block, no AHAB: hab_flags carries only type and core, and
sig_blk_offset = 0, which is what an unsigned development container looks
like.

usage: mkbootimg.py <u-boot.bin> <out.img> [--text-base 0x90200000]
                    [--sm m33_image.bin] [--spl u-boot-spl.bin]
                    [--bl31 bl31.bin]
"""
import argparse
import struct
import subprocess
import sys

CONTAINER_TAG = 0x87
CONTAINER_VERSION = 0
HDR_SIZE = 16
IMG_ENTRY_SIZE = 128
HDR_ALIGN = 0x400         # container_hdr_alignment() on i.MX 95 A0

# hab_flags, from U-Boot tools/imx8image.c (soc == IMX9).
IMG_TYPE_EXEC = 0x3
CORE_CM33 = 0x1
CORE_CA55 = 0x2
FLAGS_M33 = IMG_TYPE_EXEC | (CORE_CM33 << 4)
FLAGS_A55 = IMG_TYPE_EXEC | (CORE_CA55 << 4)
# boot_img_t.meta: CORE_IMX95_M33P / CORE_IMX95_A55C0.
META_M33 = 0
META_A55 = 2

SET_A_OFF = 0x8000        # must match PASSOVER_IMG_OFS in hw/arm/fsl-imx95.c
SET_C_OFF = 0x8400        # ALIGN(16, HDR_ALIGN) + SET_A_OFF
SET_B_OFF = 0x8800        # ROUND(SET_C_OFF + 16, 1 KiB), U-Boot-only shape
IMG_OFF_IN_B = 0x400      # block-aligned, as read_auth_image() demands
IMG_OFF_IN_SET0 = 0xc00   # clear of the third container header slot
BLOCK = 512
IMAGE_SIZE = 64 << 20

SM_LOAD = 0x1ffc0000      # M33 ITCM; the SM links and is loaded here
SPL_LOAD = 0x20480000     # CONFIG_SPL_TEXT_BASE for imx95_19x19_evk
BL31_LOAD = 0x8a200000    # as in uboot-imx arch/arm/mach-imx/imx9/scmi/container.cfg
OPTEE_LOAD = 0x8c000000   # TZDRAM; where the vendor's own flash.bin puts tee.bin


def align(x, a):
    return (x + a - 1) & ~(a - 1)


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


def boot_img(offset, size, dst, entry, hab_flags=0, meta=0):
    """offset, size, dst, entry, hab_flags, meta, hash[64], iv[32]"""
    return (struct.pack('<IIQQII', offset, size, dst, entry, hab_flags, meta)
            + b'\0' * 64 + b'\0' * 32)


def place(img, off, blob, what):
    end = off + len(blob)
    if end > IMAGE_SIZE:
        sys.exit(f'{what} too large: needs {end} bytes, image is {IMAGE_SIZE}')
    img[off:end] = blob


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('uboot')
    ap.add_argument('out')
    ap.add_argument('--text-base', default='0x90200000')
    ap.add_argument('--sm',
                    help='SM firmware for the M33 (m33_image.bin, raw). With '
                         '--spl, set 0 carries both images and the emulated '
                         'boot ROM loads them - no "-device loader" needed.')
    ap.add_argument('--spl',
                    help='U-Boot SPL for the A55 (u-boot-spl.bin, raw). '
                         'Requires --sm.')
    ap.add_argument('--bl31',
                    help='TF-A BL31 for EL3 (bl31.bin, raw). Goes into set 1 '
                         'ahead of U-Boot, so SPL enters BL31 and U-Boot '
                         'becomes BL33. Needs '
                         '-machine imx95-19x19-evk,el3-firmware=on.')
    ap.add_argument('--optee',
                    help='OP-TEE OS as BL32 (tee.bin, raw). TF-A BL31 in the '
                         'NXP BSP is built with a secure-payload dispatcher, '
                         'so without a BL32 it enters S-EL1 at address 0 and '
                         'spins in its own vector table. Requires --bl31.')
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

    if bool(args.sm) != bool(args.spl):
        sys.exit('--sm and --spl must be given together')
    if args.optee and not args.bl31:
        sys.exit('--optee needs --bl31: OP-TEE is BL32, loaded under BL31')

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

    # Container A: header only, the ELE container's slot. Both shapes need it,
    # because it is what makes SPL's walk step on to 0x8400.
    place(img, SET_A_OFF, container_hdr(HDR_SIZE, 0), 'container A')

    if not args.sm:
        # Set 0 declares nothing: a second header-only container, so SPL's
        # walk ends at 0x8410 and rounds up to 0x8800.
        place(img, SET_C_OFF, container_hdr(HDR_SIZE, 0), 'container C')
        uboot_cnt_off = SET_B_OFF
        notes = (f'set0 containers @0x{SET_A_OFF:x},0x{SET_C_OFF:x}'
                 ' (no images)')
    else:
        with open(args.sm, 'rb') as f:
            sm = f.read()
        with open(args.spl, 'rb') as f:
            spl = f.read()

        sm_off = IMG_OFF_IN_SET0
        spl_off = align(sm_off + len(sm), BLOCK)
        hdr_len = HDR_SIZE + 2 * IMG_ENTRY_SIZE

        place(img, SET_C_OFF, container_hdr(hdr_len, 2), 'set 0 container')
        place(img, SET_C_OFF + HDR_SIZE,
              boot_img(sm_off, len(sm), SM_LOAD, SM_LOAD,
                       FLAGS_M33, META_M33), 'SM entry')
        place(img, SET_C_OFF + HDR_SIZE + IMG_ENTRY_SIZE,
              boot_img(spl_off, len(spl), SPL_LOAD, SPL_LOAD,
                       FLAGS_A55, META_A55), 'SPL entry')
        place(img, SET_C_OFF + sm_off, sm, 'SM')
        place(img, SET_C_OFF + spl_off, spl, 'SPL')

        # Where SPL's get_imageset_end() will say set 0 ends: the container's
        # total size (max of its header length and its last image's end),
        # rounded up to 1 KiB.
        cnt_size = max(hdr_len, spl_off + len(spl))
        uboot_cnt_off = align(SET_C_OFF + cnt_size, 1024)
        notes = (f'set0 container @0x{SET_C_OFF:x}: '
                 f'SM ({len(sm)} B) @0x{SET_C_OFF + sm_off:x} -> '
                 f'0x{SM_LOAD:x}, SPL ({len(spl)} B) '
                 f'@0x{SET_C_OFF + spl_off:x} -> 0x{SPL_LOAD:x}')

    # Container B: set 1, which SPL loads. BL31 first when present, because
    # SPL's entry point comes from image 0.
    set1 = []
    if args.bl31:
        with open(args.bl31, 'rb') as f:
            bl31 = f.read()
        set1.append(('bl31', bl31, BL31_LOAD, FLAGS_A55, META_A55))
    set1.append(('u-boot', uboot, text_base, 0, 0))
    if args.optee:
        with open(args.optee, 'rb') as f:
            optee = f.read()
        set1.append(('optee', optee, OPTEE_LOAD, FLAGS_A55, META_A55))

    off = IMG_OFF_IN_B
    entries, payloads = [], []
    for _, blob, load, flags, meta in set1:
        entries.append(boot_img(off, len(blob), load, load, flags, meta))
        payloads.append((off, blob))
        off = align(off + len(blob), BLOCK)

    place(img, uboot_cnt_off,
          container_hdr(HDR_SIZE + len(entries) * IMG_ENTRY_SIZE, len(entries)),
          'container B')
    for i, e in enumerate(entries):
        place(img, uboot_cnt_off + HDR_SIZE + i * IMG_ENTRY_SIZE, e,
              'set 1 entry %d' % i)
    for o, blob in payloads:
        place(img, uboot_cnt_off + o, blob, 'set 1 payload')
    data_off = uboot_cnt_off + payloads[-1][0]

    with open(args.out, 'wb') as f:
        f.write(img)

    set1_notes = ', '.join(
        f'{n} ({len(b)} B) @0x{uboot_cnt_off + o:x} -> 0x{l:x}'
        for (n, b, l, _, _), (o, _) in zip(set1, payloads))
    print(f'{args.out}: {notes}; set1 container @0x{uboot_cnt_off:x}: '
          f'{set1_notes}')


if __name__ == '__main__':
    main()
