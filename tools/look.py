#!/usr/bin/env python3
"""look.py - the renderer's mirror.

Turns a rendered frame (PPM/PNG) into something a language model can actually
LOOK at: a luminance ASCII view, a colour-region map, and hard numbers about
silhouettes. This is what makes "check the image yourself" possible without a
display.

  python3 tools/look.py frame.ppm            ascii + stats
  python3 tools/look.py frame.ppm --cols=120 wider view
"""
import sys, os
import numpy as np
from PIL import Image

RAMP = " .:-=+*#%@"           # dark -> bright


def load(path):
    im = Image.open(path).convert("RGB")
    return np.asarray(im).astype(np.float32) / 255.0


def ascii_view(img, cols=96):
    h, w, _ = img.shape
    rows = max(1, int(cols * h / w * 0.5))
    ys = (np.linspace(0, h, rows + 1)).astype(int)
    xs = (np.linspace(0, w, cols + 1)).astype(int)
    lum = img @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            block = lum[ys[r]:max(ys[r] + 1, ys[r + 1]), xs[c]:max(xs[c] + 1, xs[c + 1])]
            v = float(block.mean())
            line.append(RAMP[min(len(RAMP) - 1, int(v * len(RAMP)))])
        out.append("".join(line))
    return out


def color_view(img, cols=96):
    fams = {
        'k': (0.05, 0.05, 0.06),
        'g': (0.5, 0.5, 0.5),
        'r': (0.85, 0.25, 0.2),
        'o': (0.9, 0.55, 0.2),
        'y': (0.9, 0.85, 0.3),
        'l': (0.55, 0.8, 0.3),
        'G': (0.25, 0.7, 0.35),
        'c': (0.3, 0.8, 0.85),
        'b': (0.3, 0.5, 0.85),
        'p': (0.6, 0.4, 0.85),
        'm': (0.85, 0.35, 0.7),
        'w': (0.95, 0.95, 0.95),
    }
    keys = list(fams)
    ref = np.array([fams[k] for k in keys], dtype=np.float32)
    h, w, _ = img.shape
    rows = max(1, int(cols * h / w * 0.5))
    ys = np.linspace(0, h, rows + 1).astype(int)
    xs = np.linspace(0, w, cols + 1).astype(int)
    out = []
    for r in range(rows):
        line = []
        for c in range(cols):
            block = img[ys[r]:max(ys[r] + 1, ys[r + 1]), xs[c]:max(xs[c] + 1, xs[c + 1])]
            m = block.reshape(-1, 3).mean(0)
            d = ((ref - m) ** 2).sum(1)
            line.append(keys[int(d.argmin())])
        out.append("".join(line))
    return out


def stats(img, name):
    h, w, _ = img.shape
    lum = img @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    print(f"--- {name}: {w}x{h}")
    print(f"    luminance  min {lum.min():.3f}  mean {lum.mean():.3f}  max {lum.max():.3f}")
    q = (img * 24).astype(np.int32)
    uniq = len(np.unique(q.reshape(-1, 3), axis=0))
    print(f"    distinct shades (quantised): {uniq}")
    bands = 8
    prof = []
    for i in range(bands):
        a, b = i * h // bands, (i + 1) * h // bands
        prof.append(f"{lum[a:b].mean():.3f}")
    print("    row bands top->bottom: " + " ".join(prof))
    top, bot = lum[: h // 2].mean(), lum[h // 2:].mean()
    print(f"    top half {top:.3f} | bottom half {bot:.3f} -> "
          f"{'ground below (ok)' if bot > top else 'BRIGHTER ON TOP (suspicious)'}")
    bg = img[0, 0]
    diff = np.abs(img - bg).sum(2)
    print(f"    pixels differing from corner colour: {100.0 * (diff > 0.08).mean():.1f}%")

    # objective "does this look like a photograph" numbers
    p2, p98 = np.percentile(lum, 2), np.percentile(lum, 98)
    clip_lo = 100.0 * (lum < 0.02).mean()
    clip_hi = 100.0 * (lum > 0.98).mean()
    gx = np.abs(np.diff(lum, axis=1)).mean()
    gy = np.abs(np.diff(lum, axis=0)).mean()
    sat = (img.max(2) - img.min(2)).mean()
    print(f"    EXPOSURE mean {lum.mean():.3f} (target 0.35-0.55) | "
          f"contrast p2..p98 {p2:.3f}..{p98:.3f} = {p98-p2:.3f} (target >0.45)")
    print(f"    clipping black {clip_lo:.2f}% white {clip_hi:.2f}% (target <2% each) | "
          f"detail {0.5*(gx+gy):.4f} | saturation {sat:.3f}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cols = 96
    crop = None
    stretch = False
    for a in sys.argv[1:]:
        if a.startswith("--cols"):
            cols = int(a.split("=")[1]) if "=" in a else 96
        elif a.startswith("--crop"):          # --crop=x0,y0,x1,y1 in 0..1
            crop = [float(v) for v in a.split("=")[1].split(",")]
        elif a.startswith("--stretch"):       # normalise contrast before viewing
            stretch = True
    for p in args:
        img = load(p)
        if crop:
            h, w, _ = img.shape
            img = img[int(crop[1]*h):int(crop[3]*h), int(crop[0]*w):int(crop[2]*w)]
        if stretch:
            lo, hi = np.percentile(img, 2), np.percentile(img, 98)
            img = np.clip((img - lo) / max(1e-6, hi - lo), 0, 1)
        stats(img, os.path.basename(p))
        print("    luminance view:")
        for line in ascii_view(img, cols):
            print("      " + line)
        print("    colour view:")
        for line in color_view(img, cols):
            print("      " + line)


if __name__ == "__main__":
    main()
