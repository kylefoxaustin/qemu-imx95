#!/usr/bin/env python3
"""mkbayer.py - sample an image into a raw Bayer mosaic for the NeoISP.

    ./mkbayer.py scene.png 640 480 bayer.raw [rggb|grbg|gbrg|bggr]

Also writes <out>.rgb24, the RGB the mosaic was sampled from, so the test can
ask whether the ISP RECONSTRUCTED the scene rather than merely produced pixels.
"""
import sys

PHASE = {"rggb": (0, 0), "grbg": (1, 0), "gbrg": (0, 1), "bggr": (1, 1)}

def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    src, W, H, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    phase = (sys.argv[5] if len(sys.argv) > 5 else "rggb").lower()
    if phase not in PHASE:
        sys.exit("phase must be one of " + ", ".join(PHASE))
    rx, ry = PHASE[phase]
    from PIL import Image
    im = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
    px = im.load()

    bayer = bytearray(W * H)
    rgb = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            o = (y * W + x) * 3
            rgb[o], rgb[o + 1], rgb[o + 2] = r, g, b
            ox, oy = (x & 1) == rx, (y & 1) == ry
            # each site carries only its own colour - that is what makes it raw
            bayer[y * W + x] = r if (ox and oy) else (b if (not ox and not oy) else g)
    open(out, "wb").write(bayer)
    open(out + ".rgb24", "wb").write(rgb)
    print(f"wrote {out}  {W}x{H} Bayer {phase.upper()}  {len(bayer):,} bytes")

if __name__ == "__main__":
    main()
