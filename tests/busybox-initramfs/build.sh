#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a static-aarch64 BusyBox initramfs for exercising userspace on the
# imx95 machine (validation Tier 1.3). No cross-compiler needed: it fetches
# the prebuilt static aarch64 busybox from Ubuntu's arm64 busybox-static
# package and packs an initramfs whose /init brings up proc/sys, runs a few
# sanity commands, and drops to an interactive shell.
#
# Output: ./busybox-initramfs.cpio.gz  (override with OUT=)
# Then:   INITRD=$(pwd)/busybox-initramfs.cpio.gz ../swap-boot/run.sh
set -eu

DEB_URL=${DEB_URL:-http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3.1_arm64.deb}
OUT=${OUT:-./busybox-initramfs.cpio.gz}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "fetching busybox-static (arm64) ..."
wget -qO "$WORK/bb.deb" "$DEB_URL"
dpkg-deb -x "$WORK/bb.deb" "$WORK/x"
BB=$(find "$WORK/x" -name busybox -type f | head -1)
# Verify it's an AArch64 ELF without depending on the `file` package (absent on
# minimal hosts): ELF e_machine is 2 bytes at offset 18, little-endian for
# ELFDATA2LSB, and EM_AARCH64 == 0xB7.
em=$(od -An -tx1 -j18 -N2 "$BB" | tr -d ' ')
[ "$em" = "b700" ] || { echo "not an aarch64 busybox (e_machine=$em): $BB" >&2; exit 1; }

root="$WORK/root"
mkdir -p "$root"/{bin,proc,sys,dev}
cp "$BB" "$root/bin/busybox"
cat > "$root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
echo "=== imx95 busybox userspace ==="
uname -a
echo "processors: $(grep -c '^processor' /proc/cpuinfo)"
exec /bin/busybox sh
INIT
chmod +x "$root/init"

( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$OUT"
echo "wrote $OUT ($(stat -c%s "$OUT") bytes)"
