#!/usr/bin/env python3
"""checkdev.py - did the ISP reconstruct the scene, or merely produce pixels?

Correlates the developed output against the RGB the Bayer was sampled from,
PER CHANNEL, and gates on the weakest. A luma-only check is weak against the
most likely defect: a wrong Bayer phase swaps red and blue, which luma barely
notices, while per-channel correlation drives those two sharply negative.
"""
import sys

def main():
    dev, ref, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    thresh = float(sys.argv[5]) if len(sys.argv) > 5 else 0.95
    d = open(dev, "rb").read()
    r = open(ref, "rb").read()
    # Bytes per pixel is whatever the pipeline negotiated - this one settles on
    # BGR3 (3 bytes), not the 4 an RGB32 guess would assume.
    bpp = len(d) // (W * H)
    if bpp not in (3, 4):
        print(f"FAIL: developed output is {len(d)} bytes for {W}x{H} "
              f"({bpp} B/px) - not a packed 24- or 32-bit image")
        return 1
    print(f"  developed {W}x{H} at {bpp} bytes/pixel ({len(d):,} bytes)")

    names = ("red", "green", "blue")
    worst = 1.0
    for c in range(3):
        xs, ys = [], []
        for y in range(1, H - 1, 2):
            for x in range(1, W - 1, 2):
                # developed is packed B,G,R,A
                xs.append(d[(y * W + x) * bpp + (2 - c)])   # packed B,G,R
                ys.append(r[(y * W + x) * 3 + c])
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
        sxx = sum((a - mx) ** 2 for a in xs)
        syy = sum((b - my) ** 2 for b in ys)
        cor = sxy / (sxx * syy) ** 0.5 if sxx > 0 and syy > 0 else 0.0
        print(f"  {names[c]:5s} correlation r={cor:+.4f}")
        worst = min(worst, cor)

    nonzero = sum(1 for i in range(0, W * H * bpp, bpp)
                  if d[i] or d[i + 1] or d[i + 2])
    print(f"  non-black {100*nonzero/(W*H):.1f}%   weakest channel r={worst:+.4f}")
    if nonzero < W * H * 0.2:
        print("FAIL: the developed frame is essentially black")
        return 1
    if worst < thresh:
        print(f"FAIL: the ISP produced a picture, but not THE picture (r={worst:+.4f})")
        return 1
    print("developed image reconstructs the source scene")
    return 0

if __name__ == "__main__":
    sys.exit(main())
