# NETC / ENETC test (i.MX 95)

Boots the emulated i.MX 95 and shows how far Linux's `enetc4_pf` (`nxp_enetc4`)
driver gets binding to the modelled ENETC PFs on the NETC integrated ECAM bus
(PCI `1131:e101`). The machine models **three station interfaces** — ENETC0
(`ethernet@0,0`, devfn `0x0`, 1G), ENETC1 (`ethernet@8,0`, devfn `0x40`, 1G),
and ENETC2 (`ethernet@10,0`, devfn `0x80`, the EVK's **10G** `10gbase-r` port).
The MAC + BD-ring datapath is identical across all three — the wire speed is a
property of the (fixed-)link, not the model — so the 10G port is just a third
PF brought up via a fixed-link `10gbase-r` node (no real Aquantia PHY / EMDIO):
`enetc4_pf` reports `eth2: Link is Up - 10Gbps/Full`.

## Files

- `patch-dtb.py` — rewrites the decompiled base DTS so **both** `ethernet@0,0`
  and `ethernet@8,0` are enabled with a fixed MAC + `fixed-link`, with the
  efuse-nvmem MAC cells removed; preserves each node's real `reg`/`clocks`/
  `phandle` so it works on either board's DTS. Also rewrites the NETC `msi-map`
  to ITS identity. See the header comment for why a text splice (not
  `fdtoverlay`) and the phandle-agnostic `msi-map` match.
- `run.sh` — single-port probe demo: decompiles the base DTB, applies
  `patch-dtb.py`, recompiles with the kernel `scripts/dtc/dtc`, and boots QEMU
  with one `-nic`, printing the ENETC probe lines.
- `run-10g.sh` — fixed-link 10G test: all three PFs up, the ENETC2 one at
  10Gbps via a fixed-link `10gbase-r` node (no real PHY / EMDIO).
- `patch-dtb-real10g.py` / `run-real-10g.sh` — **10G through the REAL chain**
  (Path B): the 1G ports are fixed-linked, but the 10G `ethernet@10,0` keeps its
  real `phy-handle` (Aquantia AQR113C on the NETC EMDIO, `pcie@4cb00000`) and
  `managed = "in-band-status"` — only the never-resolving efuse nvmem MAC is
  swapped for a fixed one. PASS = a port reports `Link is Up - 10Gbps/Full` with
  the `Aquantia AQR113C` driver bound and no `pcs_config` timeout, i.e. the link
  came up through the modelled EMDIO -> AQR113C -> xPCS path, not a fixed-link.
- `run-2port.sh [evk|frdm]` — **two-port back-to-back traffic test** on either
  board's device tree (see "Two-port back-to-back" below). Joins both PFs on one
  QEMU L2 hub and pings eth0 <-> eth1; PASS means frames really crossed the
  modelled wire. Self-contained (assembles its initramfs from busybox).
- `load-soak.sh` — the long throughput/stability soak launcher (see "Throughput
  / stability load soak" below). Requires host `iperf`.
- `build-load-initramfs.sh` — (re)builds the iperf load initramfs the soak boots.
- `netc-load-initramfs.cpio.gz` — the committed, prebuilt load initramfs so the
  soak runs without network access.

> **Artifact-path convention:** these scripts use `KBUILD` (default
> `~/Documents/linux-imx95-build`) for the kernel build and `SM_ELF` for the SM
> firmware — they do **not** use the repo-wide `IMX95_ARTIFACTS` convention the
> top-level README documents. Override `KBUILD`/`QEMU`/`SM_ELF`/`INITRD` per run.

## Port placement (devfn 0x0 + 0x40) and the gpex root relocation

The two PFs sit at the devfns the BSP DT maps to ENETC0 (`ethernet@0,0` = devfn
`0x0`) and ENETC1 (`ethernet@8,0` = devfn `0x40`). Two things make this work:

- **devfn 0 is freed.** gpex normally parks its own root bridge at devfn 00.0
  (`hw/pci-host/gpex.c` hardcodes `PCI_DEVFN(0,0)`), which is exactly where the
  BSP puts ENETC0. The real NETC ECAM is an integrated-endpoint
  `pci-host-ecam-generic` with no host bridge, so the machine relocates gpex's
  root device to the unused slot 31.0 (no dtsi node maps there), letting ENETC0
  occupy 00.0 like the boards expect.
- **both slots are NETCMIX-accepted.** `netc-blk-ctrl`'s `imx95_netcmix_init()`
  walks the enetc child nodes and its link-MII `switch` only accepts ENETC0
  (devfn 0x0) and ENETC1 (devfn 0x40); any other devfn returns `-EINVAL` and the
  whole block-control probe fails ("Initializing NETCMIX failed"), so
  `of_platform_populate` never runs. devfn 0x0 and 0x40 are the two it accepts.

## Status (v2.0.0 — eth0 + ping working, 24 h soak passed)

With the model + this DTB the ENETC PF probes end to end **and passes traffic**:

- `netc-blk-ctrl` NETCMIX init succeeds, the ECAM bus enumerates, and the PF
  appears at `0002:00:08.0 [1131:e101]` with BAR0/BAR2 assigned.
- `nxp_enetc4` binds (`enabling device (0000 -> 0002)`), the CBDR command ring
  drains, and the SI/port registers configure.
- `enetc_alloc_msix` succeeds: MSI-X vectors are allocated and mapped to LPIs
  through the GICv3 ITS, and the ring interrupts are delivered to the CPU.
- `register_netdev` creates **`eth0`**, and the `fixed-link` brings it up
  (`Link is Up - 1Gbps/Full`).
- TX/RX BD-ring DMA works against a `-nic user` (slirp) backend - ping the
  slirp gateway succeeds:

  ```
  64 bytes from 10.0.2.2: seq=0 ttl=255 time=...
  3 packets transmitted, 3 packets received, 0% packet loss
  ```

Two datapath behaviours a slirp boot test cannot reach are covered
deterministically by `tests/qtest/fsl-enetc-test.c` instead:

- **multi-buffer RX scatter** (`rx-scatter`) — a 1500-MTU slirp backend never
  returns a frame bigger than one buffer, so the qtest injects a 700-byte frame
  and checks the model split it across the expected BDs with only the final BD
  flagged;
- **RX ring wraparound under sustained RX** (`rx-wraparound`) — a slow,
  RTT-bound ping never wraps the 2048-entry ring, so the qtest drives 261
  frames through a 64-BD ring (4+ full laps), re-posting each consumed BD as the
  driver does, and verifies every frame lands in the correct wrapping BD with
  intact content and the producer index ends at the right modular position.

### Bring-up notes

- The PF is wired with `qemu_configure_nic_device(..., match_default=true)`, so
  attach a backend with `-nic user,model=fsl-enetc` (a bare `-netdev` is not
  matched; use `-nic`).
- `patch-dtb.py` also rewrites the NETC `pcie@4ca00000` `msi-map` to identity
  (RID -> DeviceID 1:1). The BSP remaps RIDs to DeviceID 0x6x, but QEMU's ITS
  uses the PCI requester-ID directly as the DeviceID, so the BSP remap would
  leave the ITS without a matching device entry and drop every MSI.
- The guest must program a non-zero MAC (`ip link set eth0 address ...`); the
  BSP normally pulls it from an efuse nvmem that is absent under emulation.

## Two-port back-to-back (both boards)

`run-2port.sh` brings up **both** ENETC PFs and pumps real traffic between them,
on either board's device tree:

```sh
./run-2port.sh          # 19x19 EVK dtb (default)
./run-2port.sh frdm     # 15x15 FRDM dtb (its native 1G+1G config)
```

The two PFs are joined on one QEMU L2 hub (`-nic hubport,hubid=0` x2), so a frame
leaving eth0 arrives at eth1. To prove the frame actually crosses the modelled
hardware (and isn't short-circuited by the kernel's same-host local delivery),
the guest `/init` moves eth1 into its own network namespace — eth0 stays in the
root netns at `10.0.0.1`, eth1 lives in netns `p1` at `10.0.0.2` — and pings
both ways. A `ZRESULT PASS` means frames really went eth0 -> hub -> eth1 and back
through both device models' TX and RX BD-ring DMA paths. Both boards pass:

```
RESULT: PASS (imx95-19x19-evk, both ports, back-to-back)
RESULT: PASS (imx95-15x15-frdm, both ports, back-to-back)
```

The same machine (`-M imx95-19x19-evk`) runs both DTBs; only `-dtb` differs.
Booting the **15x15 FRDM** dtb additionally needs LPSPI3 (`spi@42550000`) backed
— the FRDM enables it and an unmapped access there aborts in `fsl_lpspi_probe`
before NETC probes — so the machine stubs that bank (it already stubs the other
LPSPIs and the FRDM display bridge). The FRDM dtb may not ship in `KBUILD`; build
it once:

```sh
make -C <linux-src> O=$KBUILD ARCH=arm64 freescale/imx95-15x15-frdm.dtb
```

## Run (single port)

```sh
./run.sh
# or override paths:
KBUILD=/path/to/linux-build QEMU=/path/to/qemu-system-aarch64 ./run.sh
```

## Throughput / stability load soak

`load-soak.sh` runs the datapath hard for ~24h: it boots a second,
iperf-carrying initramfs whose `/init` brings `eth0` up and loops
`iperf -c 10.0.2.2 -t 300 -d` against a host-side `iperf -s` sink, printing a
`ZSNAP` line every 60s with eth0's rx/tx counters + `MemFree`. **Requires host
`iperf`** (the script starts the sink). A full 24 h run moved 4.76 B frames /
7.19 TB at ~661 Mbps avg (740 Mbps peak) with `rxerr=rxdrop=txerr=txdrop=0`,
zero kernel anomalies, and flat memory.

```sh
./load-soak.sh                 # detaches; ~24.2h outer timeout backstop
QEMU=$PWD/../../build/qemu-netc-soak-frozen ./load-soak.sh   # soak a pinned binary
TIMEOUT_S=600 ./load-soak.sh   # short run
```

It prints a run dir under `build/netc-load-soak-<ts>/`; watch progress with
`grep ZSNAP <run>/serial.log | tail` and confirm health with
`grep -aoE '(rx|tx)(err|drop)=[0-9]+' <run>/serial.log | sort -u` (want all 0).
Note `load-soak.sh` does `pkill -x iperf` on start, so run only one soak at a
time.

- `build-load-initramfs.sh` regenerates `netc-load-initramfs.cpio.gz` from
  scratch: busybox (reused offline from `tests/busybox-initramfs`) plus a
  dynamically-linked aarch64 `iperf` and its glibc/libstdc++ runtime fetched
  from Ubuntu 22.04 (jammy) arm64 `.deb`s. The prebuilt cpio is committed so
  the soak runs without network; rebuild only to bump iperf/libc versions.
