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


# ---------------------------------------------------------------- Adam7
# PIL cannot WRITE interlaced PNG, so the fixture is built by hand - and then
# read back with PIL, which CAN read one. If Pillow agrees with the reference
# bytes, the fixture really is Adam7 and not our own misunderstanding of it
# handed to a decoder that shares it.
XO = [0, 4, 0, 2, 0, 1, 0]
YO = [0, 0, 4, 0, 2, 0, 1]
XS = [8, 8, 4, 4, 2, 2, 1]
YS = [8, 8, 8, 4, 4, 2, 2]


def _chunk(t, d):
    c = t + d
    return len(d).to_bytes(4, "big") + c + (zlib.crc32(c) & 0xFFFFFFFF).to_bytes(4, "big")


def _pack_rows(row, depth):
    """One scanline of sub byte palette indices, packed MSB first."""
    if depth == 8:
        return row.astype(np.uint8).tobytes()
    per = 8 // depth
    out = bytearray()
    for i in range(0, len(row), per):
        b = 0
        for j in range(per):
            v = int(row[i + j]) if i + j < len(row) else 0
            b |= (v & ((1 << depth) - 1)) << (8 - depth * (j + 1))
        out.append(b)
    return bytes(out)


def write_png_adam7(path, arr, color, depth=8, palette=None):
    h, w = arr.shape[0], arr.shape[1]
    raw = bytearray()
    for p in range(7):
        pw = (w - XO[p] + XS[p] - 1) // XS[p] if w > XO[p] else 0
        ph = (h - YO[p] + YS[p] - 1) // YS[p] if h > YO[p] else 0
        if pw == 0 or ph == 0:
            continue
        sub = arr[YO[p]::YS[p], XO[p]::XS[p]]
        for row in sub:
            raw.append(0)                      # filter: none
            if color == 3:
                raw += _pack_rows(row, depth)
            else:
                raw += row.astype(np.uint8).tobytes()
    hdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes([depth, color, 0, 0, 1])
    png = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", hdr)
    if palette is not None:
        png += _chunk(b"PLTE", palette)
    png += _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b"")
    open(path, "wb").write(png)


def save_adam7(name, arr, color, depth=8, palette=None, ref=None):
    path = f"{out}/{name}.png"
    write_png_adam7(path, arr, color, depth, palette)
    if ref is None:
        ref = arr
    open(f"{out}/{name}.rgba", "wb").write(ref.tobytes())
    back = np.asarray(Image.open(path).convert("RGBA"))
    assert back.tobytes() == ref.tobytes(), f"{name}: Pillow disagrees with the reference"


rgba_full = np.dstack([rgb, alpha])
save_adam7("i_rgba8", rgba_full, 6, 8, None, rgba_full)
save_adam7("i_rgb8", rgb, 2, 8, None,
           np.dstack([rgb, np.full((h, w), 255, np.uint8)]))
# 17x13: every Adam7 pass is a different shape here, and two of them are empty
small = rgb[:13, :17]
save_adam7("i_small", small, 2, 8, None,
           np.dstack([small, np.full((13, 17), 255, np.uint8)]))

# 4 bit palette, interlaced: the pass has to be unpacked AND repacked bit wise
pal_img = Image.fromarray(rgb, "RGB").convert("P", palette=Image.ADAPTIVE, colors=16)
idx = np.asarray(pal_img)
pal = bytes(pal_img.getpalette()[: 16 * 3])
pal_rgba = np.dstack([
    np.asarray(pal_img.convert("RGB")),
    np.full((h, w), 255, np.uint8),
])
save_adam7("i_pal4", idx, 3, 4, pal, pal_rgba)

# ---------------------------------------------------------------- JPEG
# Smooth pictures on purpose: libjpeg upsamples chroma with a triangle filter
# and this decoder repeats the sample, so a hard edge in a 4:2:0 file would
# measure the difference between two upsamplers rather than the decoder.
jx, jy = np.meshgrid(np.arange(96), np.arange(64))
smooth = np.stack([jx * 2 % 256, jy * 3 % 256, (jx + jy) % 256], -1).astype(np.uint8)
smooth = np.asarray(Image.fromarray(smooth, "RGB").resize((96, 64), Image.BILINEAR))
soft = np.asarray(Image.fromarray(smooth, "RGB").filter(__import__("PIL.ImageFilter", fromlist=["ImageFilter"]).GaussianBlur(2)))


def save_jpeg(name, im, **kw):
    path = f"{out}/{name}.jpg"
    im.save(path, "JPEG", **kw)
    ref = Image.open(path).convert("RGBA")
    open(f"{out}/{name}.rgba", "wb").write(ref.tobytes())


save_jpeg("j_444", Image.fromarray(soft, "RGB"), quality=95, subsampling=0)
save_jpeg("j_422", Image.fromarray(soft, "RGB"), quality=90, subsampling=1)
save_jpeg("j_420", Image.fromarray(soft, "RGB"), quality=90, subsampling=2)
save_jpeg("j_grey", Image.fromarray(soft[:, :, 0], "L"), quality=92)
# 4:2:0 on a size that is not a whole number of MCUs - the decoder pads to 16
# and has to throw the padding away again.
save_jpeg("j_odd", Image.fromarray(soft[:37, :51], "RGB"), quality=92, subsampling=2)
try:
    save_jpeg("j_restart", Image.fromarray(soft, "RGB"), quality=90, subsampling=2,
              restart_marker_rows=1)
except (TypeError, OSError, ValueError) as e:
    print("  (no restart marker fixture:", e, ")")
# Not supported, and the point of the fixture is that it says so politely.
save_jpeg("j_progressive", Image.fromarray(soft, "RGB"), quality=90, progressive=True)


# ---------------------------------------------------------------- damaged
# A file that stops in the middle. Not a hypothetical: the logo Justin dragged
# into the editor on 07.08.2026 was exactly this, and the decoder threw the
# whole picture away over it. What a decoder owes you there is the part that
# IS still a picture, plus a sentence saying how much of it that was.
def truncate_idat(src, dst, keep):
    d = open(src, "rb").read()
    pos, out_chunks = 8, [d[:8]]
    while pos + 8 <= len(d):
        ln = int.from_bytes(d[pos:pos + 4], "big")
        t = d[pos + 4:pos + 8]
        data = d[pos + 8:pos + 8 + ln]
        if t == b"IDAT":
            data = data[: int(len(data) * keep)]
            ln = len(data)
        out_chunks.append(ln.to_bytes(4, "big") + t + data +
                          (zlib.crc32(t + data) & 0xFFFFFFFF).to_bytes(4, "big"))
        pos += 12 + int.from_bytes(d[pos:pos + 4], "big")
        if t == b"IEND":
            break
    open(dst, "wb").write(b"".join(out_chunks))


truncate_idat(f"{out}/rgb8.png", f"{out}/cut_rgb8.png", 0.55)
# The reference stays the WHOLE picture: the test only compares the rows the
# decoder claims to have recovered, which is the contract being checked.
open(f"{out}/cut_rgb8.rgba", "wb").write(open(f"{out}/rgb8.rgba", "rb").read())

print("fixtures in", out, ":", len(os.listdir(out)), "files")
