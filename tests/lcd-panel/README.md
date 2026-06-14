# Attaching an LCD panel to the i.MX95 19x19 EVK

The `imx95-19x19-evk` QEMU machine is **faithful to the real board**: the stock
NXP device tree enables the DPU display controller but attaches **no panel**, so
— exactly like a bare EVK with nothing plugged in — DRM reports
`imx95-dpu ... [drm] Cannot find any crtc or sizes`, there is no `/dev/fb*`, and
a `weston` drm-backend exits in a restart loop. That is correct, not a bug:

* the EVK's display outputs are **LVDS** (via the LDB) and **MIPI-DSI** — there
  is **no DisplayPort/DP-alt display** on this board (the `tcpc@50` /
  `phy@4c1f0040` "Fixed dependency cycle" you may see is USB-C/USB3, unrelated);
* the stock dts ships the panel disabled (no panel attached at the factory), and
  on real hardware you enable it with a device-tree overlay for your panel.

`attach-lcd.sh` does that for you: it splices the EVK's reference **LVDS** chain
into a base dtb — enabling the DPU → pixel-interleaver → LDB(channel 0) →
LVDS0-phy output path and adding a **1280×800 `panel-lvds`** node — so DRM
registers a connector with a fixed mode. The machine itself is **not** modified
(it stays faithful); this is purely a device-tree change, the same one
`tests/weston/run.sh` uses (validated: a Weston desktop and `/dev/fb0` render on
the DPU).

## Use

```
DTC=/path/to/dtc ./attach-lcd.sh imx95-19x19-evk.dtb imx95-19x19-evk-lcd.dtb
qemu-system-aarch64 -M imx95-19x19-evk ... -dtb imx95-19x19-evk-lcd.dtb
```

`dtc` is auto-detected on `$PATH` or in `~/Documents/linux-imx95-build/scripts/dtc`.
Re-run whenever the base dtb changes. For an "attach LCD" toggle in a UI, generate
the `-lcd.dtb` once and have the control swap `-dtb` between the stock and the
`-lcd` dtb.

On a successful boot with the LCD dtb you will see, instead of the "Cannot find
any crtc" message:

```
[drm] Initialized imx95-dpu 1.0.0 for 4b400000.display-controller on minor 0
Console: switching to colour frame buffer device 160x50
imx95-dpu 4b400000.display-controller: [drm] fb0: imx95-dpudrmfb frame buffer device
```
