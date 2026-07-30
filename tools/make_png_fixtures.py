#!/usr/bin/env python3
"""Generates PNG fixtures with a real encoder, plus the expected RGBA bytes.

The engine's own decoder is checked against these. Every colour type and both
bit depths are covered, and the PNG optimiser is left to pick filters, so the
fixtures exercise all five filter types rather than just "none".

  python3 tools/make_png_fixtures.py /tmp/pngfix
"""
import os, sys, zlib
import numpy as np
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/pngfix"
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(7)


def save(name, im):
    """Writes <name>.png and <name>.rgba (the ground truth the decoder must hit)."""
    im.save(f"{out}/{name}.png", optimize=True)
    ref = im.convert("RGBA")
    open(f"{out}/{name}.rgba", "wb").write(ref.tobytes())


w, h = 64, 48
xs, ys = np.meshgrid(np.arange(w), np.arange(h))
rgb = np.stack([(xs * 4) % 256, (ys * 5) % 256, ((xs + ys) * 3) % 256], -1).astype(np.uint8)
alpha = ((xs * 4) % 256).astype(np.uint8)

save("rgb8", Image.fromarray(rgb, "RGB"))
save("rgba8", Image.fromarray(np.dstack([rgb, alpha]), "RGBA"))
save("grey8", Image.fromarray(rgb[:, :, 0], "L"))
save("greyalpha8", Image.fromarray(np.dstack([rgb[:, :, 0], alpha]), "LA"))
save("palette8", Image.fromarray(rgb, "RGB").convert("P", palette=Image.ADAPTIVE, colors=64))

# 16 bit RGB: the decoder keeps the high byte, so the reference has to as well
rgb16 = (rgb.astype(np.uint16) << 8) | rgb.astype(np.uint16)
im16 = Image.fromarray(rgb16.astype("<u2"), "RGB;16") if False else None
# PIL cannot write 16 bit RGB directly - build the file by hand
def write_png16(path, arr16):
    raw = bytearray()
    for row in arr16:
        raw.append(0)
        raw += row.astype(">u2").tobytes()
    def chunk(t, d):
        c = t + d
        return len(d).to_bytes(4, "big") + c + (zlib.crc32(c) & 0xFFFFFFFF).to_bytes(4, "big")
    hdr = arr16.shape[1].to_bytes(4, "big") + arr16.shape[0].to_bytes(4, "big") + bytes([16, 2, 0, 0, 0])
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)

write_png16(f"{out}/rgb16.png", rgb16)
ref16 = np.dstack([(rgb16 >> 8).astype(np.uint8), np.full((h, w), 255, np.uint8)])
open(f"{out}/rgb16.rgba", "wb").write(ref16.tobytes())

noise = rng.integers(0, 256, (256, 256, 4), dtype=np.uint8)
save("noise_rgba", Image.fromarray(noise, "RGBA"))

grad = np.stack([np.linspace(0, 255, 512).astype(np.uint8)] * 8)
save("gradient", Image.fromarray(np.dstack([grad, grad // 2, 255 - grad]), "RGB"))

blob = rng.integers(0, 64, 200000, dtype=np.uint8).tobytes()   # compresses well
open(f"{out}/blob.bin", "wb").write(blob)
open(f"{out}/blob.z", "wb").write(zlib.compress(blob, 9))

print("fixtures in", out, ":", len(os.listdir(out)), "files")
