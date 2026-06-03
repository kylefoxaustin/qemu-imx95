# NETC / ENETC test (i.MX 95)

Boots the emulated i.MX 95 EVK and shows how far Linux's `enetc4_pf`
(`nxp_enetc4`) driver gets binding to the modelled ENETC PF on the NETC
integrated ECAM bus (PCI `1131:e101`).

## Files

- `patch-dtb.py` — rewrites the decompiled base DTS so `ethernet@8,0` (ENETC1)
  is enabled with a fixed MAC + `fixed-link`, with the efuse-nvmem MAC cell
  removed. See the header comment for why a text splice instead of `fdtoverlay`.
- `run.sh` — decompiles the base DTB, applies `patch-dtb.py`, recompiles with
  the kernel `scripts/dtc/dtc`, and boots QEMU, printing the ENETC probe lines.
- `load-soak.sh` — the long throughput/stability soak launcher (see "Throughput
  / stability load soak" below). Requires host `iperf`.
- `build-load-initramfs.sh` — (re)builds the iperf load initramfs the soak boots.
- `netc-load-initramfs.cpio.gz` — the committed, prebuilt load initramfs so the
  soak runs without network access.

> **Artifact-path convention:** these scripts use `KBUILD` (default
> `~/Documents/linux-imx95-build`) for the kernel build and `SM_ELF` for the SM
> firmware — they do **not** use the repo-wide `IMX95_ARTIFACTS` convention the
> top-level README documents. Override `KBUILD`/`QEMU`/`SM_ELF`/`INITRD` per run.

## Why devfn 08.0 (ENETC1)

The modelled PF is placed at PCI devfn 08.0. The BSP DT maps that to ENETC1.
Two constraints drive the choice:

- gpex parks its own root bridge at devfn 00.0 (where the BSP puts ENETC0), so
  the PF cannot live there.
- `netc-blk-ctrl`'s `imx95_netcmix_init()` walks the enetc child nodes and its
  link-MII `switch` only accepts ENETC0 (devfn 0x0) and ENETC1 (devfn 0x40);
  any other devfn returns `-EINVAL` and the whole block-control probe fails
  ("Initializing NETCMIX failed"), so `of_platform_populate` never runs. devfn
  0x40 is the only free, accepted slot.

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

## Run

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
