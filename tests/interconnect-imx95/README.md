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
- The client uses a plain connecting chardev (no `reconnect`) after the harness
  waits for the server's listen port - the socket chardev's reconnect path can
  crash at teardown.
