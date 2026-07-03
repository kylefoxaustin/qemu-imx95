# i.MX 95 inter-instance interconnect tests

These tests link **two separate QEMU i.MX 95 instances** over a real peripheral
datapath and prove that application bytes cross intact - the board-farm mission
of standing in for hooked-up boards (holobench). This is distinct from
`tests/netc/run-2port.sh`, which bridges two ENETC ports *inside one* instance
via an L2 hub; here the two ends are independent machines joined by a QEMU
socket netdev (`-nic socket,listen=` <-> `-nic socket,connect=`).

## run-eth.sh - ENETC ethernet leg

Two instances, each an `imx95-19x19-evk` booting the same kernel + busybox
initramfs, joined by an ENETC socket link. Role (server/client) is chosen from
the kernel cmdline (`ig_role=`). The proof is a **byte-exact TCP payload**
transferred client->server (the server diffs the received bytes against the
known payload); ICMP is used first only to warm the link.

```
QEMU=build/qemu-system-aarch64 \
KBUILD=$HOME/Documents/linux-imx95-build \
SM_ELF=$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf \
    bash tests/interconnect-imx95/run-eth.sh
```

Env knobs: `PORT` (host TCP port for the socket link, default 12390), `TMO`
(per-instance timeout). PASS prints `RESULT: PASS`; on failure each instance's
serial log is copied to `/tmp/ic-eth-{server,client}.log`.

### Notes on the datapath this exercises

- It drives **both** instances' real ENETC TX and RX BD-ring DMA across the
  wire, not a loopback. A frame leaving one eth0 crosses the socket into the
  other instance's RX ring.
- The link is warmed with ping before the short-lived TCP transfer: a frame
  that arrives before the driver's `ndo_open()` has posted RX buffers is
  discarded by the model (RX overrun, matching hardware), and a fresh TCP
  connect timeout is too short to ride out those few early-boot drops.
- Distinct per-side MACs are set from userspace. The ENETC SI primary-MAC
  register currently reads back all-zero in-guest (a separate model-fidelity
  gap); a shared/zero MAC across a point-to-point link breaks L2, so the test
  assigns `00:04:9f:06:11:22` / `:12:22` explicitly and uses static neighbours.
- The harness waits for the server's listen port to be bound before starting
  the client, because the legacy socket netdev does not retry a failed
  `connect()`.

## run-uart.sh - LPUART leg

Two instances joined by a QEMU socket **chardev** on **LPUART3** (`ttyLP2`):
instance A's `-chardev socket,server=on` <-> instance B's `-chardev socket`
(connect), each wired to `serial_hd(2)` via `-serial chardev:ul`. A byte-exact
text payload is sent client->server over `/dev/ttyLP2` and the server greps the
received stream for the exact payload line.

```
QEMU=build/qemu-system-aarch64 \
KBUILD=$HOME/Documents/linux-imx95-build \
SM_ELF=$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf \
    bash tests/interconnect-imx95/run-uart.sh
```

Env knobs: `PORT` (host TCP port for the chardev link, default 12500), `TMO`.

### Why LPUART3, and what it exercises

- **It exercises the LPUART RX-DMA datapath end to end.** LPUART3 is a
  non-console tty with DTB `dmas`, so Linux drives its RX through a cyclic
  eDMA-RX channel. The model wires LPUART3's `dma-req` to edma2; without that
  (and the LPUART RDMAE->dma-req+IDLE support) received bytes are silently
  dropped. This is the reason a UART leg uses LPUART3 rather than the console.
- **LPUART1 stays the Linux console; LPUART2 is the System Manager's own
  console** (`BOARD_DEBUG_UART_INSTANCE = 2`) and is left on `-serial null` -
  a Linux data link there would collide with the SM's banner. Serial mapping:
  `serial_hd(0)` = console (LPUART1), `serial_hd(1)` = null (LPUART2/SM),
  `serial_hd(2)` = the socket link (LPUART3).
- The dtb is patched to flip only LPUART3's `status` to `okay` (its stock
  `dmas` are kept, so the real DMA path is used, not a PIO fallback).
- The client uses a `reconnect-ms` connecting chardev, so it rides out the
  listen-bind race and any peer flap. This relies on the char-socket
  reconnect-abort fix (`chardev/char-socket.c`): before that fix, a `reconnect-ms`
  client whose connect attempt errored against a vanishing peer `abort()`ed at
  teardown (`yank_unregister_function` on a never-registered channel). That fix
  is a cross-SoC one - originally i.MX93, carried on i.MX91 and here.

## run-spi.sh - LPSPI leg

Two instances, each an LPSPI **master** with a `spi-link` SSI peripheral on its
bus (`-device spi-link,bus=lpspi7,chardev=`), socket-bridged (server <-> a
`reconnect-ms` connecting client). `spi-link` forwards each shifted-out byte
(MOSI) to the chardev and returns peer bytes on MISO from an rx FIFO; the sender
clocks a known payload out over `/dev/spidev0.0`, the receiver clocks dummy bytes
to shift it in and verifies it **byte-exact** (`SPILINK:PASS`).

```
QEMU=build/qemu-system-aarch64 \
KBUILD=$HOME/Documents/linux-imx95-build \
SM_ELF=$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf \
    bash tests/interconnect-imx95/run-spi.sh
```

Env knobs: `PAYLOAD`, `TMO`; needs an aarch64 cross gcc (builds the `spilink.c`
spidev oracle static). `spi-link` (`hw/ssi/spi_link.c`) is the fleet-shared SPI
transport, authored on i.MX 91 and ported verbatim (also on i.MX 93 / MCXN947).

### What this exercises

- **The full eDMA-driven SPI datapath.** On the 19x19 EVK the enabled SPI master
  is **lpspi7** (`spi@42710000`); the harness re-points its `lwn,bk4` child at a
  plain `rohm,dh2228fv` spidev and keeps its `dmas`. The i.MX95 `fsl-lpspi`
  driver runs the transfer over **eDMA** (usedma), which bursts each 8-bit word
  to `TDR` one **byte** at a time. `imx95_lpspi` therefore has to accept
  byte-width MMIO (`lpspi_ops .valid/.impl min_access_size=1`); a 4-byte-only
  window silently dropped every DMA burst so nothing reached the SSI bus. The
  eDMA's `TDR` writes then hit `lpspi_transfer` -> `ssi_transfer` -> `spi-link`
  exactly like a PIO write would (i.MX91/93 run the same driver in PIO).
- The two LPSPI masters are independent - each clocks its own bus - so the link
  is a byte stream in each direction over the socket, not a clocked full-duplex
  pair; a substring search on the receiver rides out the boot-offset window.

## run-can.sh - FlexCAN leg

Two instances, each a **FlexCAN** (`can0`) on its own emulated `can-bus`, joined
by a `can-host-chardev` object over a socket chardev:

```
-object can-bus,id=cb -machine canbus0=cb,canbus1=cb
-chardev socket,id=canl,path=SOCK,server=on|off,reconnect-ms=1000
-object can-host-chardev,id=canh,canbus=cb,chardev=canl
```

A known frame (`id 0x321 "CANLink!"`) crosses `FlexCAN TX -> can-bus ->
can-host-chardev -> socket -> peer -> FlexCAN RX` and is verified **byte-exact**
on the receiver (`CANLINK:PASS`).

```
QEMU=build/qemu-system-aarch64 \
KBUILD=$HOME/Documents/linux-imx95-build \
SM_ELF=$HOME/Documents/nxp/sources/imx-sm/build/mx95evk/m33_image.elf \
    bash tests/interconnect-imx95/run-can.sh
```

Env knobs: `TMO`; needs an aarch64 cross gcc (builds `canlink.c`) and the CAN
kernel modules (`can/can-dev/can-raw/flexcan`) from the kernel build. Reuses the
`tests/flexcan` DT overlay + SocketCAN bring-up helpers.

### What this exercises

- **`can-host-chardev`** (`net/can/can_host_chardev.c`) is a generic `CanHost`
  backend that bridges an emulated `can-bus` to a chardev instead of a host
  SocketCAN interface - so **no host vcan / root** is needed (unlike
  `can-host-socketcan`). The wire format is the raw `qemu_can_frame`; the reader
  reassembles a full frame from the byte stream before injecting it locally.
- Two harness gotchas are baked in: `can-host-chardev` is registered among the
  *delayed* `-object` types (`system/vl.c`) because it references a chardev, and
  **both** canbuses are wired to `cb` (`canbus0=cb,canbus1=cb`) so that whichever
  FlexCAN Linux enumerates as `can0` is on the bridged bus (an unwired `can0`
  silently drops TX).

## Cross-SoC checks - xcheck-spi-91.sh / xcheck-can-mcx.sh

The same transports (`spi_link.c`, `can-host-chardev`) are fleet-shared, so an
i.MX95 instance links byte-exact not just to another i.MX95 but to a **different
SoC's** QEMU over the identical socket-bridge shape. Two self-contained cross-SoC
checks (both ends on one host, one side server / one side reconnect-ms client)
live here; each `skip()`s cleanly when the peer tree is not built locally, and
all peer paths are env-overridable.

- **`xcheck-spi-91.sh`** - i.MX95 <-> i.MX91 SPI, **both directions**, one
  `spi_link.c`. The interesting part is that it bridges two *different* LPSPI
  datapaths transparently: the i.MX95 runs the transfer over **eDMA** (byte-wide
  TDR bursts) while the i.MX91 runs it in **PIO** (32-bit TDR writes). Both
  `91 PIO -> 95 eDMA` and `95 eDMA -> 91 PIO` verify byte-exact. Override the
  peer with `QEMU91`, `DEPLOY91` (or `IMG91`/`DTB91`/`DTC91`), `CPIO91`.

- **`xcheck-can-mcx.sh`** - i.MX95 <-> MCXN947 CAN, **both directions**, one
  `can-host-chardev`. Joins a **Linux SocketCAN** node (95) and a **bare-metal
  Cortex-M33** node (MCX firmware) on one bus over a TCP socket: MCX sends std
  `0x321`, the 95 (`canlink respond`) verifies + replies std `0x322`, the MCX ISR
  verifies the reply. The MCX firmware resends until the peer connects, so the
  slow 95 responder is booted first, then the fast MCX client. Override the peer
  with `QEMU_MCX`, `MCX_ELF`.

Proven pairwise across imx91 / imx93 / imx95 / MCXN947 - Linux-PIO, Linux-eDMA,
and bare-metal masters all interoperate over the same socket bridge.
