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

## Status (stage 4c)

With the model + this DTB the ENETC PF probes end to end:

- `netc-blk-ctrl` NETCMIX init succeeds, the ECAM bus enumerates, and the PF
  appears at `0002:00:08.0 [1131:e101]` with BAR0/BAR2 assigned.
- `nxp_enetc4` binds (`enabling device (0000 -> 0002)`), the CBDR command ring
  drains, and the SI/port registers configure.
- `enetc_alloc_msix` succeeds: 7 MSI-X vectors are allocated and mapped to LPIs
  through the GICv3 ITS (`MAPD`/`MAPTI` for DeviceID 0x63).
- `register_netdev` creates **`eth0`**, and the `fixed-link` brings it up:

  ```
  fsl_enetc4 0002:00:08.0 eth0: configuring for fixed/rgmii-id link mode
  fsl_enetc4 0002:00:08.0 eth0: Link is Up - 1Gbps/Full - flow control off
  ```

  `ip link set eth0 up` reports `operstate up`.

What remains for actual traffic is the BD-ring DMA path (TX/RX descriptor
rings) bound to a QEMU `-netdev` backend; until then the NIC model accepts no
frames (`can_receive` returns false). That is the next step.

## Run

```sh
./run.sh
# or override paths:
KBUILD=/path/to/linux-build QEMU=/path/to/qemu-system-aarch64 ./run.sh
```
