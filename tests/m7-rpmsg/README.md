# M7 rpmsg ping/pong (Linux ⇄ M7 over MU7)

Boots the full **A55 + M33(SM) + M7** stack and runs NXP's stock
`imx_rpmsg_pingpong` against an M7 rpmsg-lite client: 100 messages, kicks over
the modelled **MU7 cross-connect**, payload in the shared vrings of the M7 DRAM
carveout. The remote prints `goodbye!` after the final round-trip.

This exercises the rpmsg *data path* end-to-end (the MU7 `TR`→`RR` peer
forwarding in `hw/misc/imx_mu.c`, wired in `hw/arm/fsl-imx95.c`) on top of the
SM-orchestrated M7 boot. The matching CI case is
`tests/functional/aarch64/test_imx95_evk.py::...m7_rpmsg_pingpong`.

## Artifacts

None are redistributable, so the harness takes them from the environment.

| var | what | where to get it |
| --- | --- | --- |
| `M7_FW` | M7 rpmsg-lite pingpong firmware, **raw TCM `.bin`** | NXP BSP target rootfs: `/usr/lib/firmware/imx95-19x19-evk_m7_TCM_rpmsg_lite_pingpong_rtos_linux_remote.bin`, or build the MCUXpresso SDK multicore example `rpmsg_lite_pingpong_rtos_linux_remote` for `MIMX95` |
| `INITRD` | initramfs bundling `imx_rpmsg_pingpong.ko` + an `/init` that loads it | build it (below) |
| `SM_ELF` | System Manager firmware `m33_image.elf` | NXP `imx-sm` tree (see `docs/system/arm/imx95-evk.rst`) |
| `KERNEL` / `DTB` | stock NXP BSP `Image` + `imx95-19x19-evk.dtb` | NXP `linux-imx` build |

The M7 firmware is loaded as a raw image at the M7 TCM alias `0x203c0000`
(`-device loader,...,addr=0x203c0000,force-raw=on`) — *not* as an ELF with
`cpu-num`. The SM boots the M7 from there.

## Building the initramfs

The kernel ships `imx_rpmsg_pingpong` as a module (`CONFIG_IMX_RPMSG_PINGPONG=m`
in the BSP). Stage it plus an `/init` that loads it into a BusyBox initramfs:

```sh
# 1. the matching module from your kernel build
cp .../linux-imx95-build/drivers/rpmsg/imx_rpmsg_pingpong.ko  root/

# 2. an /init that mounts /proc + /sys, inserts the module, and waits for
#    the round-trips to finish (the demo channel binds, then 100 messages):
cat > init <<'EOF'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
echo "=== RPMSG PINGPONG TEST ==="
insmod /root/imx_rpmsg_pingpong.ko
sleep 5            # let the 100-message exchange run to "goodbye!"
exec /bin/busybox sh
EOF
chmod +x init

# 3. pack (find . | cpio -o -H newc | gzip > rpmsg-initramfs.cpio.gz)
```

The module must match the running kernel; `insmod` the `.ko` from that exact
build. `imx_rpmsg_pingpong` autostarts on bind (no arguments) and prints
`get N (src: ...)` per round-trip, then `goodbye!`.

## Run

```sh
M7_FW=/path/to/...rpmsg_lite_pingpong_rtos_linux_remote.bin \
INITRD=/path/to/rpmsg-initramfs.cpio.gz \
SM_ELF=/path/to/m33_image.elf \
KERNEL=/path/to/Image DTB=/path/to/imx95-19x19-evk.dtb \
tests/m7-rpmsg/run.sh
```

Expected tail:

```
PASS: SCMI handshake against the real SM (M33 <-> A55 MU2 up)
PASS: rpmsg ping/pong completed (last 'get 101 (src', module said goodbye)
=== M7 rpmsg ping/pong verified: 100 messages over MU7 + vrings ===
```
