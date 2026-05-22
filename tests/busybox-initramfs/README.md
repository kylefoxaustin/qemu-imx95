# BusyBox initramfs (userspace shakeout)

A static-aarch64 BusyBox initramfs for exercising real userspace on the imx95
machine — the validation campaign's Tier 1.3 ("userspace is genuinely alive").

```
tests/busybox-initramfs/build.sh
INITRD=$(pwd)/tests/busybox-initramfs/busybox-initramfs.cpio.gz tests/swap-boot/run.sh
```

`build.sh` fetches the prebuilt static aarch64 busybox from Ubuntu's arm64
`busybox-static` package (no cross-compiler needed) and packs an initramfs
whose `/init` mounts proc/sys, prints `uname` + the CPU count, and drops to a
shell. The generated `.cpio.gz` is not committed (build it locally).

Observed with this initramfs (2026-05-21): all 6 Cortex-A55 cores present
(`/proc/cpuinfo` part 0xd05), `ps`/`ls`/`mount`/`dmesg` work — userspace is
genuinely alive. Two anomalies under sustained interactive use are still being
characterized (see docs/imx95/known-limitations.md): typed console input not
yet observed reaching the shell, and intermittent post-boot SCMI timeouts.
