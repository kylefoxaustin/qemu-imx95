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
doorbell → DONE). This full-stack e2e currently **SKIPs** at a known SM/SCMI
follow-on: the neutron device probe calls `pm_runtime_resume_and_get()`, which
powers the NPU on through its SCMI power-domain (`IMX95_PD_NPU`); the real SM
firmware does not complete that power-on, so the device driver never binds
(`/dev/neutron0` is absent) and the LiteRT delegate cannot apply. The firmware
carveout maps and the remoteproc registers, confirming the harness is sound — the
remaining work is SM/SCMI NPU power-domain + clock support, not the NPU model.

## Running

```
KBUILD=<kernel build>  SM_ELF=<m33_image.elf>  \
BSP_ROOTFS=<imx-image-full rootfs>   ./tests/neutron/run.sh
```

SKIPs if the Neutron userspace (NeutronFirmware.elf / benchmark_model / a
`*neutron_delegate.so`) is not present in `BSP_ROOTFS`.
