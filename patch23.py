# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

# ------------------------------------------------------------- dai_svg
P = 'include/dai_svg.h'
s = rw(P)
s = sub1(s,
"""DAI_API int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad);""",
"""DAI_API int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad);

/* A fill/stroke color override for exactly one shape, applied at parse time.
 * The built in icons are drawn in one neutral color and tinted by the widget;
 * some icons need to carry their own color (a warning triangle is yellow, an
 * error cross is red), and baking a second icon set is worse than one byte
 * swap at load. `rgba` is 0xAABBGGRR. */
DAI_API void dai_svg_shape_color(dai_svg *s, uint32_t index, uint32_t rgba);
/* The color of a shape, 0 = "ask the widget". */
DAI_API uint32_t dai_svg_shape_color_get(const dai_svg *s, uint32_t index);
/* Rasterises with per-shape colors: out is RGBA, color comes from the shape,
 * coverage from the same path as dai_svg_rasterize. Shapes with no override
 * come out white, exactly what the mono path produced. */
DAI_API int dai_svg_rasterize_rgba(const dai_svg *s, uint8_t *out, int w, int h, float pad);""",
"svg color decl")
wr(P, s)

P = 'src/dai_svg.cpp'
s = rw(P)
s = sub1(s,
"""struct Shape {
    std::vector<SubPath> subs;
    bool  fill = true;
    bool  evenodd = false;
    bool  stroke = false;""",
"""struct Shape {
    std::vector<SubPath> subs;
    uint32_t color = 0;      // 0xAABBGGRR, 0 = no override (widget tints)
    bool  fill = true;
    bool  evenodd = false;
    bool  stroke = false;""", "shape color field")

s = sub1(s,
"""uint32_t dai_svg_shape_count(const dai_svg *s) { return s ? (uint32_t)s->shapes.size() : 0; }""",
"""uint32_t dai_svg_shape_count(const dai_svg *s) { return s ? (uint32_t)s->shapes.size() : 0; }

void dai_svg_shape_color(dai_svg *s, uint32_t index, uint32_t rgba) {
    if (!s || index >= s->shapes.size()) return;
    s->shapes[index].color = rgba;
}

uint32_t dai_svg_shape_color_get(const dai_svg *s, uint32_t index) {
    if (!s || index >= s->shapes.size()) return 0;
    return s->shapes[index].color;
}""", "svg color api")

# rgba rasterize: same geometry path, but per-shape paint. Simplest correct
# version: rasterize each shape to its own coverage, then composite.
s = sub1(s,
"""int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad) {""",
"""int dai_svg_rasterize_rgba(const dai_svg *s, uint8_t *out, int w, int h, float pad) {
    if (!s || !out || w <= 0 || h <= 0) return 0;
    std::memset(out, 0, (size_t)w * h * 4);
    // One shape at a time through the mono path, into its own coverage, then
    // composited. Cheaper than a second rasteriser, and the shapes share the
    // flattening code either way.
    std::vector<uint8_t> cov((size_t)w * h);
    std::vector<uint8_t> tmp((size_t)w * h);
    for (size_t si = 0; si < s->shapes.size(); ++si) {
        dai_svg one;
        one.vx = s->vx; one.vy = s->vy; one.vw = s->vw; one.vh = s->vh;
        one.shapes.push_back(s->shapes[si]);
        std::memset(tmp.data(), 0, tmp.size());
        if (!dai_svg_rasterize(&one, tmp.data(), w, h, pad)) continue;
        uint32_t col = s->shapes[si].color ? s->shapes[si].color : 0xFFFFFFFFu;
        uint8_t r = (uint8_t)(col & 0xFF), g = (uint8_t)((col >> 8) & 0xFF),
                b = (uint8_t)((col >> 16) & 0xFF), a = (uint8_t)((col >> 24) & 0xFF);
        for (size_t i = 0; i < tmp.size(); ++i) {
            uint32_t ca = (uint32_t)tmp[i] * a / 255u;
            uint8_t *px = out + i * 4;
            // source over destination
            uint32_t da = px[3];
            uint32_t na = ca + da * (255 - ca) / 255u;
            if (!na) continue;
            px[0] = (uint8_t)((r * ca + px[0] * da * (255 - ca) / 255u) / na);
            px[1] = (uint8_t)((g * ca + px[1] * da * (255 - ca) / 255u) / na);
            px[2] = (uint8_t)((b * ca + px[2] * da * (255 - ca) / 255u) / na);
            px[3] = (uint8_t)na;
        }
    }
    return 1;
}

int dai_svg_rasterize(const dai_svg *s, uint8_t *out, int w, int h, float pad) {""",
"svg rgba raster")
wr(P, s)
print("patch23 done")
