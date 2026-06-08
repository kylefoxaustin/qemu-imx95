# i.MX 95 Neutron NPU bring-up — end-to-end harness

`run.sh` boots stock Linux with the Neutron NPU enabled and exercises the whole
software stack against the `hw/misc/imx95_neutron.c` bring-up model:

```
neutron driver probe -> remoteproc loads NeutronFirmware.elf -> RESETCTRL clock-on
  -> firmware "started" handshake (APPCTRL=0xF807) -> /dev/neutron0
  -> TFLite benchmark_model + LiteRT Neutron delegate -> inference via the mailbox
```

The proprietary NPU compute is not modelled, so inference *output* is not
computed — this is a "brings up" milestone, not a functional NPU.

Because the base EVK dtb does not enable the NPU firmware-DDR carveout (the stock
`imx95-19x19-evk-neutron.dtso` overlay puts a 4 GiB pool at 4 GiB, and the
prebuilt dtb has no `__symbols__` for `fdtoverlay`), the harness decompiles the
dtb, splices in a small in-range carveout + the neutron `memory-region`, and
recompiles with the kernel's `dtc` (the `tests/netc/patch-dtb.py` pattern). It
boots with `-m 4G` so the @4 GiB carveout is backed. `neutron` + its remoteproc
are built into the BSP kernel, so no modules are staged; the TFLite app +
delegate + their `.so` closure are harvested (readelf) from the BSP rootfs.

## Status

The kernel-free `tests/qtest/imx95-neutron-test.c` validates the model's
remoteproc + mailbox responder (RESETCTRL clock-on → startup handshake →
doorbell → DONE). This full-stack e2e **PASSES**: the remoteproc registers, the
compute device binds, `NeutronFirmware.elf` loads into the model's DTCM/ITCM, and
`benchmark_model` runs through the LiteRT Neutron delegate.

Two things the dtb patch / model handle to get there (the earlier "blocked on the
SCMI `IMX95_PD_NPU` power-domain" hypothesis was wrong — the rproc acquires that
domain fine):

- **IOMMU**: the compute node (`imx95-neutron@4ab00004`) has `iommus = <&smmu>`,
  but QEMU has no `arm-smmu-v3` model — the driver logs *"no translation
  support!"* and a device needing the IOMMU can't bind. The patch drops the
  `iommus` phandle so the NPU DMAs directly (identity-mapped under emulation).
- **TCM**: `rproc_elf_load_segments` copies the firmware into the NPU's DTCM
  (`0x4ab08000`, 32K) and ITCM (`0x4ab10000`, 64K); the model now backs both as
  RAM so the `memcpy_toio` does not external-abort.

The NPU does **not** actually compute: the delegate offloads 0 nodes and inference
falls back to CPU (which keeps results correct). That proprietary-firmware compute
is the model's fidelity ceiling; what this validates is the whole
driver → firmware-load → delegate → benchmark path.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/neutron/run.sh
```

SKIPs if the Neutron userspace (NeutronFirmware.elf / benchmark_model / a
`*neutron_delegate.so`) is not present in `BSP_ROOTFS`.
