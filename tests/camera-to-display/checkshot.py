#!/usr/bin/env python3
"""checkshot.py - is the photograph actually ON the panel?

The hash proves the bytes crossed the pipeline. It says nothing about whether
the DPU scanned them out, so this checks the picture independently: find the
centred capture region in the screendump and correlate it against the source
image. A black panel, a test pattern, or a stale Weston desktop all fail here
even when the hash passes.
"""
import sys

def main():
    shot, src, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    from PIL import Image
    panel = Image.open(shot).convert("RGB")
    want = Image.open(src).convert("RGB").resize((W, H), Image.LANCZOS)
    pw, ph = panel.size
    ox, oy = max((pw - W) // 2, 0), max((ph - H) // 2, 0)
    crop = panel.crop((ox, oy, ox + W, oy + H))

    # Correlate on luma, subsampled - we are asking "is this the same picture",
    # not "is it bit-identical". YUV 4:2:2 and the RGB round trip both lose a
    # little, so an exact match would be the wrong test.
    step = 4
    xs, ys = [], []
    for y in range(0, H, step):
        for x in range(0, W, step):
            r1, g1, b1 = crop.getpixel((x, y))
            r2, g2, b2 = want.getpixel((x, y))
            xs.append(0.299 * r1 + 0.587 * g1 + 0.114 * b1)
            ys.append(0.299 * r2 + 0.587 * g2 + 0.114 * b2)
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in ys)
    r = sxy / (sxx * syy) ** 0.5 if sxx > 0 and syy > 0 else 0.0
    mae = sum(abs(a - b) for a, b in zip(xs, ys)) / n
    ink = sum(1 for a in xs if a > 8) / n

    print(f"panel {pw}x{ph}, capture region +{ox}+{oy} {W}x{H}")
    print(f"correlation r={r:.4f}  luma MAE={mae:.1f}  non-black={100*ink:.1f}%")
    if ink < 0.20:
        print("FAIL: the capture region is essentially black - nothing was scanned out")
        return 1
    if r < 0.90:
        print(f"FAIL: panel content does not match the source image (r={r:.4f})")
        return 1
    print("panel check OK: the photograph is on the display")
    return 0

if __name__ == "__main__":
    sys.exit(main())
