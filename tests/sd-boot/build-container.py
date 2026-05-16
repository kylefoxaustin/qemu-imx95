#!/usr/bin/env python3
"""
Build a minimal i.MX95-style AHAB chain and write it onto an SD image
so U-Boot SPL can parse + load U-Boot proper from sector 0+.

Layout of the produced SD image (matches the on-silicon NXP flash.bin
chaining convention; SPL walks SECO -> V2X -> SCU containers in
`get_imageset_end()` to discover where U-Boot proper lives):

  0x0000_0000  ---                              (empty)
  0x0000_8000  SECO container header (16 B)     (num_images = 0)
  0x0000_8400  V2X container (16 B + 2x 128 B)  (advertises v2x_fw)
  0x0000_8800  SCU container header (16 B)     (num_images = 0)
  0x0000_8C00  U-Boot proper container          (header + 1 image)
  0x0001_0000  payload                          (u-boot.bin, raw)

SPL flow (per `arch/arm/mach-imx/image-container.c` +
`common/spl/spl_imx_container.c`):

  1. SECO at 0x8000: header-only, length = 16. value_container[0] = 16,
     hdr_length = 16.
  2. offset[1] = ALIGN(16, container_hdr_alignment) + 0x8000.
     `container_hdr_alignment()` returns 0x400 for imx95-A0 (which is
     what our ELE soc_rev = 0xA1 makes is_imx95_a0() evaluate to),
     so offset[1] = 0x8400.
  3. V2X at 0x8400: num_images = 2, first image's hab_flags encodes
     IMG_TYPE_V2X_PRI_FW (0x0B) + CORE_V2X_PRI (9) so
     is_v2x_fw_container() returns true. Container length = 16 + 256
     = 272 (= 0x110). offset[2] = ALIGN(0x110, 0x400) + 0x8400 = 0x8800.
  4. SCU at 0x8800: header-only, length = 16. value_container[2] = 16.
  5. Return value_container[2] + offset[2] = 0x8810. ROUND(., 1 KiB)
     -> 0x8C00. SPL loads the U-Boot proper container from there.
  6. read_auth_container() at 0x8C00 parses 1 image, reads payload
     from SD (image.offset + 0x8C00 = 0x7400 + 0x8C00 = 0x10000)
     into image.dst, then SPL jumps to image.entry.

This generator deliberately produces *unsigned* containers:
CONFIG_AHAB_BOOT is off in our SPL build, so `valid_container_hdr()`
only checks (tag == 0x87, version == 0); no signature is verified.

Usage:
    tests/sd-boot/build-container.py \\
        --payload tests/spl-banner/uboot-build/u-boot.bin \\
        --dst-addr 0x90200000 \\
        --entry-addr 0x90200000 \\
        --output tests/sd-boot/sd.img

Notes:
- Payload offset within the SD image is hard-coded at 64 KiB to leave
  margin for the SECO/V2X/SCU/U-Boot container chain. boot_img_t.offset
  is encoded relative to the U-Boot proper container start (0x8C00),
  so 0x10000 - 0x8C00 = 0x7400.
- `spl_get_bl_len(info)` for an SD card is 512; all image offsets
  and sizes are 512-aligned by construction (0x7400 is 512-aligned,
  and the payload is padded up to the next 512 boundary).
"""

import argparse
import struct
import sys
from pathlib import Path

CONTAINER_TAG = 0x87
CONTAINER_VERSION = 0
SECO_OFFSET = 0x8000        # img_ofs reported by our SCMI ROM_PASSOVER stub
V2X_OFFSET = 0x8400         # ALIGN(SECO_LEN, 0x400) + SECO_OFFSET
SCU_OFFSET = 0x8800         # ALIGN(V2X_LEN, 0x400) + V2X_OFFSET
UBOOT_CTNR_OFFSET = 0x8C00  # ROUND(SCU_END, 1 KiB)
PAYLOAD_OFFSET = 0x10000
HASH_LEN = 64
IV_LEN = 32

# is_v2x_fw_container() flag encoding (image-container.c:90-104):
#   IMG_TYPE_V2X_PRI_FW = 0x0B, CORE_V2X_PRI = 9   ->  primary
#   IMG_TYPE_V2X_SND_FW = 0x0C, CORE_V2X_SND = 10  ->  secondary
# Both images must match for is_v2x_fw_container() to return true.
V2X_PRI_HAB_FLAGS = (9 << 4) | 0x0B   # = 0x9B
V2X_SND_HAB_FLAGS = (10 << 4) | 0x0C  # = 0xAC

# struct container_hdr (include/imx_container.h:23)
#   u8 version, u8 length_lsb, u8 length_msb, u8 tag,
#   u32 flags, u16 sw_version, u8 fuse_version, u8 num_images,
#   u16 sig_blk_offset, u16 reserved
HDR_FMT = "<BBBBLHBBHH"
HDR_LEN = struct.calcsize(HDR_FMT)
assert HDR_LEN == 16, HDR_LEN

# struct boot_img_t (include/imx_container.h:36)
#   u32 offset, u32 size, u64 dst, u64 entry, u32 hab_flags, u32 meta,
#   u8 hash[64], u8 iv[32]
IMG_FMT = "<LLQQLL64s32s"
IMG_LEN = struct.calcsize(IMG_FMT)
assert IMG_LEN == 128, IMG_LEN


def empty_container() -> bytes:
    """Header-only container with num_images=0, used for the SECO and
    SCU slots. SPL's get_imageset_end() only checks tag/version via
    valid_container_hdr() to consider these "present"; the read path
    that requires num_images>=1 is only taken for the U-Boot proper
    slot at the end of the chain.
    """
    return struct.pack(
        HDR_FMT,
        CONTAINER_VERSION,
        HDR_LEN & 0xFF,                # length_lsb = 16
        (HDR_LEN >> 8) & 0xFF,         # length_msb
        CONTAINER_TAG,
        0,                             # flags
        0,                             # sw_version
        0,                             # fuse_version
        0,                             # num_images
        0,                             # sig_blk_offset
        0,                             # reserved
    )


def build_v2x_container() -> bytes:
    """Two-image v2x container that satisfies is_v2x_fw_container().
    The image bodies are empty (offset=size=0). num_images=2 falls
    inside MIN_V2X_CTNR_IMG_NUM..MAX_V2X_CTNR_IMG_NUM (=2..4), and
    the first image's hab_flags encodes V2X_PRI_FW + CORE_V2X_PRI.
    Length = 16 (header) + 2 * 128 (images) = 272 bytes.
    """
    length = HDR_LEN + 2 * IMG_LEN
    hdr = struct.pack(
        HDR_FMT,
        CONTAINER_VERSION,
        length & 0xFF,
        (length >> 8) & 0xFF,
        CONTAINER_TAG,
        0,                             # flags
        0,                             # sw_version
        0,                             # fuse_version
        2,                             # num_images
        0,                             # sig_blk_offset
        0,                             # reserved
    )
    img_pri = struct.pack(IMG_FMT, 0, 0, 0, 0,
                          V2X_PRI_HAB_FLAGS, 0,
                          bytes(HASH_LEN), bytes(IV_LEN))
    img_snd = struct.pack(IMG_FMT, 0, 0, 0, 0,
                          V2X_SND_HAB_FLAGS, 0,
                          bytes(HASH_LEN), bytes(IV_LEN))
    return hdr + img_pri + img_snd


def build_uboot_container(payload_size: int, dst: int, entry: int) -> bytes:
    """Container that loads `payload_size` bytes from the SD into `dst`
    and jumps to `entry`. The image-data offset is encoded relative to
    the container header start (== UBOOT_CTNR_OFFSET in the SD image).
    """
    img_offset = PAYLOAD_OFFSET - UBOOT_CTNR_OFFSET   # = 0x7400
    img = struct.pack(
        IMG_FMT,
        img_offset,                    # offset (relative to ctnr)
        payload_size,                  # size
        dst,                           # dst (load address)
        entry,                         # entry (jump address)
        0,                             # hab_flags
        0,                             # meta
        bytes(HASH_LEN),               # hash (unused without AHAB_BOOT)
        bytes(IV_LEN),                 # iv (unused)
    )

    length = HDR_LEN + IMG_LEN
    hdr = struct.pack(
        HDR_FMT,
        CONTAINER_VERSION,
        length & 0xFF,                 # length_lsb
        (length >> 8) & 0xFF,          # length_msb
        CONTAINER_TAG,
        0,                             # flags
        0,                             # sw_version
        0,                             # fuse_version
        1,                             # num_images
        0,                             # sig_blk_offset
        0,                             # reserved
    )

    return hdr + img


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--payload", required=True, type=Path,
                    help="Raw binary to wrap (e.g. u-boot.bin)")
    ap.add_argument("--dst-addr", required=True, type=lambda x: int(x, 0),
                    help="Load address (where SPL places the payload)")
    ap.add_argument("--entry-addr", required=True, type=lambda x: int(x, 0),
                    help="Entry point (jumped to after load)")
    ap.add_argument("--output", required=True, type=Path,
                    help="SD image to (over)write")
    ap.add_argument("--size", default=64 * 1024 * 1024,
                    type=lambda x: int(x, 0),
                    help="Total SD image size in bytes (default: 64 MiB)")
    args = ap.parse_args()

    payload = args.payload.read_bytes()
    # 512-byte align for SD block reads
    pad = (-len(payload)) % 512
    payload_padded = payload + bytes(pad)

    if PAYLOAD_OFFSET + len(payload_padded) > args.size:
        print(f"error: payload ({len(payload_padded)} B) + offset "
              f"(0x{PAYLOAD_OFFSET:x}) exceeds image size "
              f"(0x{args.size:x})", file=sys.stderr)
        return 2

    seco = empty_container()
    v2x = build_v2x_container()
    scu = empty_container()
    uboot = build_uboot_container(len(payload), args.dst_addr, args.entry_addr)

    image = bytearray(args.size)
    image[SECO_OFFSET:SECO_OFFSET + len(seco)] = seco
    image[V2X_OFFSET:V2X_OFFSET + len(v2x)] = v2x
    image[SCU_OFFSET:SCU_OFFSET + len(scu)] = scu
    image[UBOOT_CTNR_OFFSET:UBOOT_CTNR_OFFSET + len(uboot)] = uboot
    image[PAYLOAD_OFFSET:PAYLOAD_OFFSET + len(payload_padded)] = payload_padded

    args.output.write_bytes(bytes(image))
    print(f"wrote {args.output} "
          f"({args.size // (1024 * 1024)} MiB; "
          f"seco @ 0x{SECO_OFFSET:x}, "
          f"v2x @ 0x{V2X_OFFSET:x}, "
          f"scu @ 0x{SCU_OFFSET:x}, "
          f"u-boot ctnr @ 0x{UBOOT_CTNR_OFFSET:x}, "
          f"payload {len(payload)} B @ 0x{PAYLOAD_OFFSET:x}, "
          f"dst=0x{args.dst_addr:x} entry=0x{args.entry_addr:x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
