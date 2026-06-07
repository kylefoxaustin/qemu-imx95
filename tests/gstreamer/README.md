# JPEG decode — GStreamer real-app end-to-end validation

This is the real-media-stack proof for `hw/misc/imx95_jpeg.c`. The qtest
(`tests/qtest/imx95-jpeg-test.c`) drives the decode engine directly; this drives
the whole stack:

```
gst-launch-1.0 filesrc location=test.jpg ! jpegparse ! v4l2jpegdec ! \
    videoconvert ! filesink location=out.raw
        -> the real mxc-jpeg V4L2 mem2mem driver
        -> our imx95_jpeg model
        -> a decoded NV12 frame
```

It validates what the qtest can't: the driver's buffer queueing / `STREAMON` /
IRQ-completion timing, and the **NV12** output path (GStreamer negotiates NV12;
the qtest only exercises BGR).

```
bash tests/gstreamer/run.sh
```

PASS = the pipeline reaches EOS, `out.raw` is a 256×256 NV12 frame (98304 B),
and its Y plane is the luma of the solid-colour source (`Y ≈ 98` for the
checked-in `test-256.jpg`, RGB 200,50,80).

## Requirements (and why it SKIPs)

Unlike the other tests, the GStreamer userspace is **not in the repo**. The
script harvests `gst-launch-1.0` + the needed plugins (`libgstvideo4linux2`
exposes the mxc-jpeg mem2mem device as `v4l2jpegdec`, plus `jpegformat`, `jpeg`,
`coreelements`, `videoconvertscale`) and their shared-library closure (resolved
with `readelf`, which reads aarch64 ELFs on an x86 host) from a BSP
`imx-image-full` rootfs, and pairs them with **our** vermagic-matching
`mxc-jpeg-encdec.ko` + `v4l2-jpeg.ko` from `$KBUILD` (the rootfs's own modules
are built for a different kernel and won't load). If `$BSP_ROOTFS` has no
`gst-launch-1.0`, the test prints `SKIP` and exits 0.

Override via env: `QEMU`, `KBUILD` (Image + dtb + in-tree mxc-jpeg modules),
`SM_ELF`, `BSP_ROOTFS`.

## Notes / gotchas

- The `gst-launch` ELF interpreter is `/usr/lib/ld-linux-aarch64.so.1`, not
  `/lib/...` — the loader is staged at both.
- The mxc-jpeg driver rejects images ≤ 64×64 ("width or height should be
  > 64"), hence the 256×256 fixture.
- Needs an **aarch64** busybox (taken from `tests/busybox-initramfs`); the host
  x86 one won't run in the guest.
- Encode through GStreamer (`v4l2jpegenc`) additionally needs a raw source
  element (`videotestsrc`) not staged here; encode is already covered by the
  qtest's encode→decode round-trip.
