# qemu-imx95 helper scripts

## probe_stall.py — A55 hang debugger

When a Linux probe or initcall *hangs* on the A55 (rather than faulting), there
is no fault to dump, and a fixed-address monitor read misses the stalled
kworker because its kernel stack is at a non-deterministic address across runs.
`probe_stall.py` automates the frame-pointer-unwinding technique (methodology
"Pillar 5"):

1. Launches QEMU with an HMP monitor on a private unix socket.
2. Waits until execution is sitting in the stall.
3. Reads the live frame pointer (`X29`) of the chosen CPU.
4. Dumps that exact stack in the same run.
5. Walks the AArch64 frame chain offline (`[fp]` = caller fp, `[fp+8]` = LR).
6. `addr2line`s each return address against `vmlinux`.
7. Samples a few times (hangs can be racy); a consistent leaf names the culprit.

### Usage

```
scripts/probe_stall.py --vmlinux <vmlinux> --cpu <n> --delay <seconds> \
    [--samples 3] -- <full QEMU command>
```

`--delay` is wall-clock seconds; pick a value comfortably past the stall.

### Caveats

The QEMU command (everything after `--`) must not contain its own `-monitor`
argument — `probe_stall.py` injects one for HMP access. Route the guest console
somewhere stable: `-serial file:/tmp/console.log -serial null` (as in the
example below) or `-serial mon:stdio` for interactive use. Never pass a manual
`-monitor`.

### Example

```
scripts/probe_stall.py --vmlinux ~/linux-imx95-build/vmlinux --cpu 2 \
    --delay 30 --samples 3 -- \
    ./build/qemu-system-aarch64 -M imx95-19x19-evk -m 2G -display none \
    -kernel Image -dtb imx95-19x19-evk.dtb -initrd initramfs.cpio.gz \
    -append 'console=ttyLP0,115200 rdinit=/init' \
    -device loader,file=m33_image.elf,cpu-num=6 \
    -serial file:/tmp/console.log -serial null
```

This pinned the v0.10 DPU probe hang (`dpu95_be_read`) and the cpuidle wedge
(the PSCI-suspend chain). See `docs/methodology.md` for the full pillar set.
