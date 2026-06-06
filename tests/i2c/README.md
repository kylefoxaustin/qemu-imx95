# i.MX 95 LPI2C i2c-2 bus test

The `lpi2c@42530000` bus (Linux **i2c-2**) was a UNIMP stub, so its on-board
devices timed out (`-110`). With a real master + register-file slaves they
probe:

- **PCA953x** IO-expander at `2-0020`
- **PCA9632** LED controller at `2-0062` (`leds-pca963x` → `/sys/class/leds/`)

```
bash tests/i2c/run.sh
```

PASS = the LED class device registers and the expander probes (no `-110`).
Both are register-file i2c devices, so the existing `pcal6408a` model serves
both — the `leds-pca963x` driver only needs its MODE/LEDOUT/PWM register reads
and writes to land.
