# i.MX 95 RGPIO test

Exercises `hw/gpio/imx95_gpio.c` — the `fsl,imx95-gpio` / `fsl,imx8ulp-gpio`
RGPIO controller the Linux `gpio-vf610` driver binds.

`run.sh` boots stock Linux and checks two things:

1. **Binding** — `gpio-vf610` registers the four *enabled* SoC GPIO controllers
   (`0x43810000`..`0x43850000` = GPIO2–5) as real gpiochips. GPIO1
   (`0x47400000`, AONMIX) is `status = "disabled"` in the stock dtb, so it is
   correctly absent.
2. **Functional data path** — drives an output pin via `devmem`
   (PDDR/PSOR/PCOR) and reads its pad state back from PDIR. On the old UNIMP
   stub PDIR read back 0; the real model reflects `(PDOR & PDDR)`.

```
bash tests/gpio/run.sh
```

PASS = 4 gpiochips bound + the devmem loopback tracks + zero faults.

## Notes

- Within each 4 KiB window: GPIO data block at `+0x40` (PDOR/PSOR/PCOR/PTOR/
  PDIR/PDDR), PORT block at `+0x80` (PCR[n] with the IRQC field at bits[19:16],
  ISFR at `+0x120`).
- The model has 32 input + 32 output gpio lines and two GIC outputs; the driver
  requests only the first SPI and its chained handler walks the whole ISFR, so
  output 0 carries every pin.
- SoC-GPIO consumers in the EVK dtb are modest (mostly interrupt-parents for
  i2c expanders, e.g. GPIO2 gates the audio PCAL6408A's INT line); there are no
  gpio-keys/leds on this board.
