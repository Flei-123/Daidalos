#!/usr/bin/env python3
"""Give assets/daidalos.ico a white outline.

The glyph is dark grey (#252525..#3B3B3B) on transparent, so on a dark title
bar, a dark taskbar or a dark browser tab it simply is not there. The fix is
the one every dark-on-transparent mark needs: dilate the alpha channel, fill
the grown ring with white, and put the original glyph back on top.

Dilation is a max over a DISC, not a box - a box dilation puts square corners
on a round logo, and at 256 px that is visible. The alpha stays antialiased
because the disc max is computed on the float alpha, not on a 1-bit mask.

Every icon size is rendered from the 256 px master and dilated at ITS OWN
scale: downscaling a composite that already has a 3 px outline turns the
outline into grey mush at 16 px.
"""
import sys
import numpy as np
from PIL import Image

# The source is the PRISTINE mark, never the file this script last wrote -
# running it on its own output dilates an outline that is already there, and
# the ring doubles every time. assets/daidalos.ico.bak_p39 is the original
# dark-grey-on-transparent glyph; pass another path as argv[1] to override.
SRC = "assets/daidalos.ico.bak_p39"
DST = "assets/daidalos.ico"
PREVIEW = "assets/daidalos_outline_preview.png"

# Radius in pixels AT THAT SIZE, and deliberately fractional: a 4 px ring on a
# 256 px glyph reads as a sticker, which is what "make it thinner" was about.
# The dilation below is subpixel, so 2.5 is a real 2.5 and not a rounded 3.
SIZES = [(256, 2.5), (128, 1.6), (64, 1.1), (48, 1.0), (32, 0.9), (16, 0.8)]


def dilate_disc(alpha: np.ndarray, r: float) -> np.ndarray:
    """Max filter over a disc of radius r, subpixel. alpha is float32 0..1.

    A whole-pixel disc can only ever be 1, 2, 3 px wide, and the step from 2 to
    3 on a 256 px mark is very visible. Offsets further out than r are not
    dropped but FADED: their contribution is scaled by how much of the pixel
    still falls inside the disc, which is a real 2.5 px ring with an
    antialiased outer edge rather than a 3 px one with a staircase.
    """
    if r <= 0.0:
        return alpha
    ri = int(np.ceil(r))
    h, w = alpha.shape
    pad = np.zeros((h + 2 * ri, w + 2 * ri), dtype=np.float32)
    pad[ri:ri + h, ri:ri + w] = alpha
    out = np.zeros_like(alpha)
    for dy in range(-ri, ri + 1):
        for dx in range(-ri, ri + 1):
            d = float(np.hypot(dx, dy))
            cover = r - d + 0.5
            if cover <= 0.0:
                continue
            if cover > 1.0:
                cover = 1.0
            shifted = pad[ri + dy:ri + dy + h, ri + dx:ri + dx + w] * cover
            out = np.maximum(out, shifted)
    return out


def with_outline(img: Image.Image, r: int) -> Image.Image:
    rgba = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    a = rgba[..., 3]
    grown = dilate_disc(a, r)
    # The ring is white; where the glyph is opaque the glyph wins. Straight
    # source-over of the glyph onto a white plate of the grown alpha.
    plate = np.ones_like(rgba)
    plate[..., 3] = grown
    src_a = a[..., None]
    out_a = src_a + plate[..., 3:4] * (1.0 - src_a)
    # premultiplied composite, then un-premultiply - anything else fringes
    num = rgba[..., :3] * src_a + plate[..., :3] * plate[..., 3:4] * (1.0 - src_a)
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(out_a > 1e-4, num / np.maximum(out_a, 1e-4), 0.0)
    res = np.concatenate([np.clip(rgb, 0, 1), np.clip(out_a, 0, 1)], axis=-1)
    return Image.fromarray((res * 255.0 + 0.5).astype(np.uint8), "RGBA")


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    master = Image.open(src)
    master.size = (256, 256)
    master.load()
    master = master.convert("RGBA")

    # The glyph must not touch the frame or the outline gets clipped. Shrink
    # it into a canvas with room for the widest ring we draw.
    bbox = master.getbbox()
    pad = 6
    inner = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    glyph = master.crop(bbox)
    gw = 256 - 2 * pad
    gh = int(round(glyph.height * gw / glyph.width))
    if gh > 256 - 2 * pad:
        gh = 256 - 2 * pad
        gw = int(round(glyph.width * gh / glyph.height))
    glyph = glyph.resize((gw, gh), Image.LANCZOS)
    inner.paste(glyph, ((256 - gw) // 2, (256 - gh) // 2))
    master = inner

    layers = []
    for size, r in SIZES:
        img = master if size == 256 else master.resize((size, size), Image.LANCZOS)
        layers.append(with_outline(img, r))

    big = layers[0]
    big.save(PREVIEW)
    big.save(DST, format="ICO",
             sizes=[(s, s) for s, _ in SIZES],
             append_images=layers[1:])
    # Pillow's ICO writer re-scales from the first image, so write the layers
    # explicitly and verify what actually landed in the file.
    chk = Image.open(DST)
    print("wrote", DST, sorted(chk.info["sizes"]))
    print("preview", PREVIEW, big.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
