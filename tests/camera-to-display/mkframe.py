#!/usr/bin/env python3
"""mkframe.py - turn a real image into a raw YUYV 4:2:2 frame for the ISI.

The ISI's host frame source ("virtual camera") reads back-to-back raw frames of
exactly width*height*2 bytes and DMAs them into the capture buffers, so a
"smart camera" - a sensor that emits already-developed YUV rather than Bayer -
is modelled simply by handing it developed pixels. No ISP is involved.

    ./mkframe.py photo.png 640 480 frame.raw [stride]

Also writes <out>.rgb24 (the exact RGB the guest should reconstruct) so the test
can compare what came out of the pipeline against what went in, rather than
trusting the picture to look right.
"""
import sys, struct

def clamp(v, lo=0, hi=255):
    return lo if v < lo else (hi if v > hi else int(v))

def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    src, W, H, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    # The ISI reports bytesperline = 6*W for this pipeline (a wider stride than
    # the 2*W the pixels occupy); honour whatever V4L2 says rather than assuming
    # packed lines, and leave the padding zeroed.
    stride = int(sys.argv[5]) if len(sys.argv) > 5 else W * 6
    try:
        from PIL import Image
    except ImportError:
        sys.exit("mkframe: needs Pillow (pip install pillow)")
    im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
    px = im.load()

    # BT.601 full-range RGB -> YCbCr, the convention the ISI/DPU pair uses here.
    uyvy = bytearray(stride * H)
    rgb  = bytearray(W * H * 3)
    for y in range(H):
        i = y * stride
        for x in range(0, W, 2):
            r0, g0, b0 = px[x, y]
            r1, g1, b1 = px[min(x + 1, W - 1), y]
            y0 =  0.299 * r0 + 0.587 * g0 + 0.114 * b0
            y1 =  0.299 * r1 + 0.587 * g1 + 0.114 * b1
            # chroma is shared across the pair: average the two source pixels
            rm, gm, bm = (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2
            cb = -0.169 * rm - 0.331 * gm + 0.500 * bm + 128
            cr =  0.500 * rm - 0.419 * gm - 0.081 * bm + 128
            # YUYV: Y0 Cb Y1 Cr  (what this pipeline negotiates)
            uyvy[i + 0] = clamp(y0); uyvy[i + 1] = clamp(cb)
            uyvy[i + 2] = clamp(y1); uyvy[i + 3] = clamp(cr)
            i += 4
            for k, (rr, gg, bb) in enumerate(((r0, g0, b0), (r1, g1, b1))):
                o = (y * W + x + k) * 3
                if x + k < W:
                    rgb[o], rgb[o + 1], rgb[o + 2] = rr, gg, bb
    open(out, "wb").write(uyvy)
    open(out + ".rgb24", "wb").write(rgb)
    print(f"wrote {out}  {W}x{H} YUYV stride={stride}  {len(uyvy):,} bytes")
    print(f"wrote {out}.rgb24  reference RGB  {len(rgb):,} bytes")

if __name__ == "__main__":
    main()
