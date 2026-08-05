# -*- coding: utf-8 -*-
import sys, io

def rw(p): return io.open(p, encoding='utf-8').read()
def wr(p, s): io.open(p, 'w', encoding='utf-8').write(s)
def sub1(s, old, new, tag):
    if old not in s: print("MISS", tag); sys.exit(1)
    if s.count(old) != 1: print("AMBIG", tag, s.count(old)); sys.exit(1)
    print("ok", tag); return s.replace(old, new)

P = 'src/dai_icons.cpp'
s = rw(P)

s = sub1(s,
"""struct Icon {
    std::string name;
    std::vector<uint8_t> px;      // size x size coverage
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};""",
"""struct Icon {
    std::string name;
    std::vector<uint8_t> px;      // size x size coverage
    std::vector<uint8_t> rgba;    // size x size x 4, colored icons only
    bool  colored = false;        // carries its own colors, never tinted
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};""", "icon rgba")

s = sub1(s,
"""const uint32_t BUILTIN_COUNT = (uint32_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0]));""",
"""const uint32_t BUILTIN_COUNT = (uint32_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0]));

// Some icons carry their own color: a warning is yellow and an error is red
// in every theme, and tinting them with the text color is how they turned
// gray. Written as shape overrides onto the parsed document at load.
const struct { const char *name; uint32_t shape; uint32_t rgba; } COLOR_RULES[] = {
    { "warning", 0, 0xFF36C8F0u },      // triangle, #F0C836
    { "warning", 1, 0xFF1A1A1Au },      // the bar, dark on yellow
    { "warning", 2, 0xFF1A1A1Au },
    { "error",   0, 0xFF4B55D9u },      // circle, #D9554B
    { "error",   1, 0xFFFFFFFFu },      // the cross, white
    { "error",   2, 0xFFFFFFFFu },
    { "info",    0, 0xFFCE8C4Cu },      // circle, #4C8CCE
    { "info",    1, 0xFFFFFFFFu },
    { "info",    2, 0xFFFFFFFFu },
    { "camera",  0, 0xFFCE8C4Cu },
    { "camera",  1, 0xFFFFFFFFu },
    { "light",   0, 0xFF36C8F0u },
    { "light",   1, 0xFF36C8F0u },
    { "volume",  0, 0xFF5FD47Cu },
    { "volume",  1, 0xFF5FD47Cu },
    { "volume",  2, 0xFF5FD47Cu },
    { "volume-x",0, 0xFF4B55D9u },
    { "volume-x",1, 0xFF4B55D9u },
    { "volume-x",2, 0xFF4B55D9u },
};
const uint32_t COLOR_RULES_COUNT = (uint32_t)(sizeof(COLOR_RULES) / sizeof(COLOR_RULES[0]));""",
"color rules")

s = sub1(s,
"""int add_svg(dai_icons *ic, const char *name, const char *svg) {
    if (!ic || !name || !svg) return 0;
    char err[128];
    dai_svg *doc = dai_svg_parse(svg, 0, err, sizeof(err));
    if (!doc) return 0;
    int s = (int)ic->size;
    Icon it;
    it.name = name;
    it.px.assign((size_t)s * s, 0);""",
"""int add_svg(dai_icons *ic, const char *name, const char *svg) {
    if (!ic || !name || !svg) return 0;
    char err[128];
    dai_svg *doc = dai_svg_parse(svg, 0, err, sizeof(err));
    if (!doc) return 0;
    for (uint32_t ri = 0; ri < COLOR_RULES_COUNT; ++ri) {
        if (std::strcmp(COLOR_RULES[ri].name, name) != 0) continue;
        dai_svg_shape_color(doc, COLOR_RULES[ri].shape, COLOR_RULES[ri].rgba);
    }
    bool colored = false;
    for (uint32_t si = 0; si < dai_svg_shape_count(doc); ++si)
        if (dai_svg_shape_color_get(doc, si)) { colored = true; break; }
    int s = (int)ic->size;
    Icon it;
    it.name = name;
    it.colored = colored;
    it.px.assign((size_t)s * s, 0);
    if (colored) {
        it.rgba.assign((size_t)s * s * 4, 0);
        dai_svg_rasterize_rgba(doc, it.rgba.data(), s, s, ic->size * (1.0f / 16.0f));
    }""", "add_svg colored")

# the RGBA atlas: colored cells write their colors, mono cells stay white
s = sub1(s,
"""const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h) {
    repack(ic);
    if (ic->atlas_rgba.size() != ic->atlas.size() * 4) {
        ic->atlas_rgba.resize(ic->atlas.size() * 4);
        for (size_t i = 0; i < ic->atlas.size(); ++i) {
            ic->atlas_rgba[i * 4 + 0] = 255;
            ic->atlas_rgba[i * 4 + 1] = 255;
            ic->atlas_rgba[i * 4 + 2] = 255;
            ic->atlas_rgba[i * 4 + 3] = ic->atlas[i];
        }
    }
    if (w) *w = ic->aw;
    if (h) *h = ic->ah;
    return ic->atlas_rgba.empty() ? nullptr : ic->atlas_rgba.data();
}""",
"""const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h) {
    repack(ic);
    if (ic->atlas_rgba.size() != ic->atlas.size() * 4) {
        ic->atlas_rgba.resize(ic->atlas.size() * 4);
        for (size_t i = 0; i < ic->atlas.size(); ++i) {
            ic->atlas_rgba[i * 4 + 0] = 255;
            ic->atlas_rgba[i * 4 + 1] = 255;
            ic->atlas_rgba[i * 4 + 2] = 255;
            ic->atlas_rgba[i * 4 + 3] = ic->atlas[i];
        }
    }
    // Colored icons overwrite their cell with their own colors. Everything
    // else stays white-with-alpha, which is what the widget tints.
    {
        uint32_t cols = 1;
        while (cols * cols < ic->icons.size()) ++cols;
        int s = (int)ic->size;
        for (uint32_t i = 0; i < ic->icons.size(); ++i) {
            const Icon &it = ic->icons[i];
            if (!it.colored || it.rgba.empty()) continue;
            uint32_t cx = (i % cols) * (uint32_t)ic->cell;
            uint32_t cy = (i / cols) * (uint32_t)ic->cell;
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x) {
                    size_t dst = ((size_t)(cy + 1 + (uint32_t)y) * ic->aw + (cx + 1 + (uint32_t)x)) * 4;
                    size_t src = ((size_t)y * s + x) * 4;
                    ic->atlas_rgba[dst + 0] = it.rgba[src + 0];
                    ic->atlas_rgba[dst + 1] = it.rgba[src + 1];
                    ic->atlas_rgba[dst + 2] = it.rgba[src + 2];
                    ic->atlas_rgba[dst + 3] = it.rgba[src + 3];
                }
        }
    }
    if (w) *w = ic->aw;
    if (h) *h = ic->ah;
    return ic->atlas_rgba.empty() ? nullptr : ic->atlas_rgba.data();
}""", "rgba atlas colored")
wr(P, s)

P = 'include/dai_icons.h'
s = rw(P)
s = sub1(s,
"""/* The same expanded to white RGBA with coverage in alpha. */
DAI_API const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h);""",
"""/* The same expanded to RGBA: white with coverage in alpha for the tinted
 * icons, real colors for the colored ones. */
DAI_API const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h);
/* 1 when the icon carries its own colors and must be drawn WITHOUT a tint. */
DAI_API int dai_icons_colored(const dai_icons *ic, const char *name);""", "colored decl")
wr(P, s)

P = 'src/dai_icons.cpp'
s = rw(P)
s = sub1(s,
"""uint32_t dai_icons_count(const dai_icons *ic) {""",
"""int dai_icons_colored(const dai_icons *ic, const char *name) {
    if (!ic || !name) return 0;
    for (const Icon &it : ic->icons)
        if (it.name == name) return it.colored ? 1 : 0;
    return 0;
}

uint32_t dai_icons_count(const dai_icons *ic) {""", "colored impl")
wr(P, s)

# dai_ui_icon_at respects it
P = 'src/dai_ui.cpp'
s = rw(P)
s = sub1(s,
"""void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
                    float size, uint32_t color) {""",
"""void dai_ui_icon_at(dai_ui *ui, const char *name, float x, float y,
                    float size, uint32_t color) {
    // A colored icon is its own picture; tinting it with the text color is
    // what turned the yellow warning triangle gray.
    if (ui && ui->icons && dai_icons_colored(ui->icons, name))
        color = 0xFFFFFFFFu;""", "ui icon colored")
wr(P, s)
print("patch24 done")
