# NETC / ENETC test (i.MX 95)

Boots the emulated i.MX 95 EVK and verifies that Linux's `enetc4_pf`
(`nxp_enetc4`) driver binds to the modelled ENETC0 PF on the NETC integrated
ECAM bus (PCI `1131:e101`).

## Files

- `enetc-overlay.dtso` — device-tree overlay merged onto the base BSP DTB. It
  places an available `ethernet@2,0` node (PCI devfn 02.0, fixed MAC,
  `fixed-link`) and removes the `fsl,enetc-ptp-timer` compatible from the
  (unmodelled) NETC timer node. See the comments in the file for why.
- `run.sh` — compiles + merges the overlay (kernel `scripts/dtc/{dtc,fdtoverlay}`)
  and boots QEMU, printing the ENETC probe lines.

## Why devfn 02.0 (not 00.0)

The BSP DT maps ENETC0 to PCI devfn 00.0, but QEMU's `gpex` host parks its own
root bridge there, and the BSP ethernet nodes pull in efuse-nvmem MAC cells
that never resolve under emulation. devfn 02.0 is in the dtsi `msi-map`
(ITS DevID 0x61) and has no base-tree child, so the overlay can describe it
cleanly.

## Status

Stage 4a/4b: the PF enumerates, `nxp_enetc4` binds, and the MAC is adopted from
the DT. Bringing up `eth0` further currently stops at MSI-X vector allocation
(`pci_alloc_irq_vectors` → `-EBUSY`); the GICv3 ITS itself initialises and is
detected. Wiring MSI-X → ITS to completion (then BD-ring DMA + a `-netdev`
backend for ping) is the remaining NETC work.

## Run

```sh
./run.sh
# or override paths:
KBUILD=/path/to/linux-build QEMU=/path/to/qemu-system-aarch64 ./run.sh
```
