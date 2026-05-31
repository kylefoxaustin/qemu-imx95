# FlexCAN end-to-end test (Linux can0 ⇄ can1)

Validates the QEMU **FlexCAN** model (`hw/net/can/flexcan.c`) against the real
Linux `flexcan` driver: two FlexCAN controllers are wired to **one** emulated
CAN bus, so the guest sees `can0` and `can1` on the same bus, and a small
self-test sends frames each way and verifies they arrive intact.

```
   guest: cantest can0 can1
            │  ip link set canN up + send/recv (SocketCAN)
   can0 ───┤                                ┌─── can1
 flexcan1 ─┘  ── emulated can-bus "cb" ──    └─ flexcan2
 (0x443a0000, canbus0)                        (0x425b0000, canbus1)
```

This complements `tests/qtest/flexcan-test.c` (which pokes the model's
registers directly, no kernel): here the *real Linux driver* drives the model
through its full `chip_start` (MCR handshake, RAM init, bit-timing, mailbox
setup, interrupts) and does live TX/RX.

## Run

```sh
tests/flexcan/run.sh            # PASS prints "FLEXCAN SELFTEST: PASS"
```

It needs (override via env; defaults match the project layout):

| var | what |
| --- | --- |
| `QEMU` | `build/qemu-system-aarch64` |
| `KERNEL` / `DTB` | NXP BSP `Image` + stock `imx95-19x19-evk.dtb` |
| `SM_ELF` | System Manager firmware `m33_image.elf` |
| `KBUILD` | kernel build tree — for the CAN `.ko`s **and** `scripts/dtc/{dtc,fdtoverlay}` |
| `XGCC` | aarch64 cross-gcc prefix (default `aarch64-linux-gnu-`) |

Kernel config: `CONFIG_CAN`, `CONFIG_CAN_RAW`, `CONFIG_CAN_DEV`,
`CONFIG_CAN_FLEXCAN` (modules are fine — the harness `insmod`s them).

## How it works

1. **`cantest`** (`cantest.c`, static aarch64): a self-contained SocketCAN
   self-test — no `iproute2`/`can-utils` needed. It sets each interface's
   bitrate and brings it up via rtnetlink, then sends a burst each way and
   checks the frames round-trip. Build standalone with `make`.
2. **CAN-enabled DTB**: the stock EVK DT ships FlexCAN **disabled**, so the
   harness compiles `flexcan-overlay.dtso` and merges it onto a *copy* of the
   base DTB with `fdtoverlay` (the base is never modified). The overlay sets
   the two nodes `okay` and points `xceiver-supply` at a fixed always-on
   regulator — on real hardware that supply is a GPIO "can-stby" regulator
   behind an i2c GPIO expander that does not probe in emulation, so without
   this the FlexCAN nodes defer forever.
3. **initramfs**: BusyBox + the four CAN `.ko`s + `cantest` + an `/init` that
   loads the stack, runs `cantest can0 can1`, prints the verdict, powers off.
4. **boot**: `-object can-bus,id=cb -machine canbus0=cb,canbus1=cb` puts
   flexcan1 (canbus0) and flexcan2 (canbus1) on one bus.

## Talking to a host CAN instead

To bridge guest CAN to the host (e.g. `candump`/`cansend` on the host), point a
controller at host SocketCAN via a `vcan`:

```sh
sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set vcan0 up
qemu-system-aarch64 -M imx95-19x19-evk ... \
  -object can-bus,id=canbus0 \
  -object can-host-socketcan,id=canhost0,if=vcan0,canbus=canbus0 \
  -machine canbus0=canbus0
# guest: ip link set can0 up type can bitrate 500000; cansend can0 123#deadbeef
# host:  candump vcan0
```
