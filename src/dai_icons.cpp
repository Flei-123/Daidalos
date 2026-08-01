// The built-in icon set and the atlas it is packed into.
//
// The sources below are ordinary SVG - 24x24 viewBox, 2 unit strokes, round
// caps and joins. They are written out in full rather than generated because
// an icon is a drawing, and a drawing in a table of numbers is unreadable and
// unfixable. Anyone can paste one of these into a browser and see it.

#include "dai_icons.h"
#include "dai_svg.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A stroked icon: fill nothing, draw the outline. The wrapper is repeated per
// icon rather than concatenated at build time so each string is a valid,
// standalone SVG file - which is what makes them checkable in a browser.
#define STROKE_HEAD                                                            \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' " \
    "stroke='currentColor' stroke-width='2' stroke-linecap='round' "           \
    "stroke-linejoin='round'>"
#define SOLID_HEAD                                                             \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' "             \
    "fill='currentColor' stroke='none'>"
#define TAIL "</svg>"

struct Builtin { const char *name; const char *svg; };

const Builtin BUILTIN[] = {
// ---- gizmo modes --------------------------------------------------------
{ "move", STROKE_HEAD
  "<polyline points='5 9 2 12 5 15'/><polyline points='9 5 12 2 15 5'/>"
  "<polyline points='15 19 12 22 9 19'/><polyline points='19 9 22 12 19 15'/>"
  "<line x1='2' y1='12' x2='22' y2='12'/><line x1='12' y1='2' x2='12' y2='22'/>" TAIL },

{ "rotate", STROKE_HEAD
  "<polyline points='21 4 21 10 15 10'/>"
  "<path d='M19.4 15a8.5 8.5 0 1 1-2-8.9L21 10'/>" TAIL },

{ "scale", STROKE_HEAD
  "<polyline points='15 3 21 3 21 9'/><polyline points='9 21 3 21 3 15'/>"
  "<line x1='21' y1='3' x2='14' y2='10'/><line x1='3' y1='21' x2='10' y2='14'/>" TAIL },

// ---- transport ----------------------------------------------------------
{ "play",  SOLID_HEAD "<path d='M7 4.5 19 12 7 19.5Z'/>" TAIL },
{ "pause", SOLID_HEAD "<rect x='6' y='4' width='4' height='16' rx='1'/>"
                      "<rect x='14' y='4' width='4' height='16' rx='1'/>" TAIL },
{ "stop",  SOLID_HEAD "<rect x='6' y='6' width='12' height='12' rx='1.5'/>" TAIL },

// ---- editing ------------------------------------------------------------
{ "undo", STROKE_HEAD
  "<polyline points='9 14 4 9 9 4'/><path d='M20 20v-7a4 4 0 0 0-4-4H4'/>" TAIL },
{ "redo", STROKE_HEAD
  "<polyline points='15 14 20 9 15 4'/><path d='M4 20v-7a4 4 0 0 1 4-4h12'/>" TAIL },
{ "copy", STROKE_HEAD
  "<rect x='9' y='9' width='13' height='13' rx='2'/>"
  "<path d='M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1'/>" TAIL },
{ "trash", STROKE_HEAD
  "<polyline points='3 6 5 6 21 6'/>"
  "<path d='M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2'/>"
  "<line x1='10' y1='11' x2='10' y2='17'/><line x1='14' y1='11' x2='14' y2='17'/>" TAIL },
{ "save", STROKE_HEAD
  "<path d='M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z'/>"
  "<polyline points='17 21 17 13 7 13 7 21'/><polyline points='7 3 7 8 15 8'/>" TAIL },

// ---- chrome -------------------------------------------------------------
{ "layout", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/>"
  "<line x1='3' y1='9' x2='21' y2='9'/><line x1='9' y1='21' x2='9' y2='9'/>" TAIL },
{ "chevron-right", STROKE_HEAD "<polyline points='9 18 15 12 9 6'/>" TAIL },
{ "chevron-down",  STROKE_HEAD "<polyline points='6 9 12 15 18 9'/>" TAIL },
{ "plus",  STROKE_HEAD "<line x1='12' y1='5' x2='12' y2='19'/>"
                       "<line x1='5' y1='12' x2='19' y2='12'/>" TAIL },
{ "check", STROKE_HEAD "<polyline points='20 6 9 17 4 12'/>" TAIL },
{ "close", STROKE_HEAD "<line x1='18' y1='6' x2='6' y2='18'/>"
                       "<line x1='6' y1='6' x2='18' y2='18'/>" TAIL },
{ "search", STROKE_HEAD "<circle cx='11' cy='11' r='8'/>"
                        "<line x1='21' y1='21' x2='16.65' y2='16.65'/>" TAIL },
{ "settings", STROKE_HEAD
  "<circle cx='12' cy='12' r='3'/>"
  "<path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.6a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z'/>" TAIL },

// ---- components and scene objects ---------------------------------------
{ "box", STROKE_HEAD
  "<path d='M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z'/>"
  "<polyline points='3.3 7 12 12 20.7 7'/><line x1='12' y1='22' x2='12' y2='12'/>" TAIL },
{ "sphere", STROKE_HEAD
  "<circle cx='12' cy='12' r='9'/><ellipse cx='12' cy='12' rx='4' ry='9'/>"
  "<line x1='3' y1='12' x2='21' y2='12'/>" TAIL },
{ "capsule", STROKE_HEAD
  "<rect x='7' y='3' width='10' height='18' rx='5'/>"
  "<path d='M7 12h10'/>" TAIL },
{ "eye", STROKE_HEAD
  "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z'/>"
  "<circle cx='12' cy='12' r='3'/>" TAIL },
{ "eye-off", STROKE_HEAD
  "<path d='M17.9 17.9A10 10 0 0 1 12 20C5 20 1 12 1 12a18.4 18.4 0 0 1 5.1-5.9'/>"
  "<path d='M9.9 4.2A9.1 9.1 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.2 3.2'/>"
  "<path d='M14.1 14.1a3 3 0 1 1-4.2-4.2'/>"
  "<line x1='1' y1='1' x2='23' y2='23'/>" TAIL },
{ "layers", STROKE_HEAD
  "<polygon points='12 2 2 7 12 12 22 7 12 2'/>"
  "<polyline points='2 17 12 22 22 17'/><polyline points='2 12 12 17 22 12'/>" TAIL },
{ "sun", STROKE_HEAD
  "<circle cx='12' cy='12' r='5'/>"
  "<line x1='12' y1='1' x2='12' y2='3'/><line x1='12' y1='21' x2='12' y2='23'/>"
  "<line x1='4.2' y1='4.2' x2='5.6' y2='5.6'/><line x1='18.4' y1='18.4' x2='19.8' y2='19.8'/>"
  "<line x1='1' y1='12' x2='3' y2='12'/><line x1='21' y1='12' x2='23' y2='12'/>"
  "<line x1='4.2' y1='19.8' x2='5.6' y2='18.4'/><line x1='18.4' y1='5.6' x2='19.8' y2='4.2'/>" TAIL },
{ "camera", STROKE_HEAD
  "<path d='M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z'/>"
  "<circle cx='12' cy='13' r='4'/>" TAIL },
{ "grid", STROKE_HEAD
  "<rect x='3' y='3' width='18' height='18' rx='2'/>"
  "<line x1='3' y1='9' x2='21' y2='9'/><line x1='3' y1='15' x2='21' y2='15'/>"
  "<line x1='9' y1='3' x2='9' y2='21'/><line x1='15' y1='3' x2='15' y2='21'/>" TAIL },

// ---- assets -------------------------------------------------------------
{ "folder", STROKE_HEAD
  "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>" TAIL },
{ "file", STROKE_HEAD
  "<path d='M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z'/>"
  "<polyline points='13 2 13 9 20 9'/>" TAIL },
};

const uint32_t BUILTIN_COUNT = (uint32_t)(sizeof(BUILTIN) / sizeof(BUILTIN[0]));

struct Icon {
    std::string name;
    std::vector<uint8_t> px;      // size x size coverage
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

} // namespace

struct dai_icons {
    float size = 16.0f;
    int   cell = 18;
    std::vector<Icon> icons;
    std::vector<uint8_t> atlas;
    std::vector<uint8_t> atlas_rgba;
    uint32_t aw = 0, ah = 0;
    bool dirty = true;
};

namespace {

void repack(dai_icons *ic) {
    if (!ic->dirty) return;
    ic->dirty = false;
    uint32_t n = (uint32_t)ic->icons.size();
    if (!n) { ic->aw = ic->ah = 0; ic->atlas.clear(); return; }

    // A grid, not a shelf packer: every icon is the same square, so the
    // clever version would produce exactly the same layout.
    uint32_t cols = 1;
    while (cols * cols < n) ++cols;
    uint32_t rows = (n + cols - 1) / cols;
    uint32_t w = 1, h = 1;
    while (w < cols * (uint32_t)ic->cell) w *= 2;
    while (h < rows * (uint32_t)ic->cell) h *= 2;
    ic->aw = w; ic->ah = h;
    ic->atlas.assign((size_t)w * h, 0);
    ic->atlas_rgba.clear();

    int s = (int)ic->size;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t cx = (i % cols) * (uint32_t)ic->cell;
        uint32_t cy = (i / cols) * (uint32_t)ic->cell;
        Icon &it = ic->icons[i];
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x)
                ic->atlas[(size_t)(cy + 1 + (uint32_t)y) * w + (cx + 1 + (uint32_t)x)] =
                    it.px[(size_t)y * s + x];
        // Half a texel in from the edge of the icon: sampling exactly on the
        // boundary picks up the neighbour's first column when the quad is
        // scaled, which is how an icon grows a stray line down one side.
        it.u0 = (float)(cx + 1) / (float)w;
        it.v0 = (float)(cy + 1) / (float)h;
        it.u1 = (float)(cx + 1 + (uint32_t)s) / (float)w;
        it.v1 = (float)(cy + 1 + (uint32_t)s) / (float)h;
    }
}

int add_svg(dai_icons *ic, const char *name, const char *svg) {
    if (!ic || !name || !svg) return 0;
    char err[128];
    dai_svg *doc = dai_svg_parse(svg, 0, err, sizeof(err));
    if (!doc) return 0;
    int s = (int)ic->size;
    Icon it;
    it.name = name;
    it.px.assign((size_t)s * s, 0);
    // One pixel of padding inside the cell: round caps and joins reach half a
    // stroke past the path, and a 2 unit stroke on a 24 unit box that touches
    // the edge would otherwise lose its outer half.
    int ok = dai_svg_rasterize(doc, it.px.data(), s, s, ic->size * (1.0f / 16.0f));
    dai_svg_free(doc);
    if (!ok) return 0;

    for (Icon &existing : ic->icons) {
        if (existing.name == name) { existing.px = std::move(it.px); ic->dirty = true; return 1; }
    }
    ic->icons.push_back(std::move(it));
    ic->dirty = true;
    return 1;
}

} // namespace

dai_icons *dai_icons_create(float pixel_size) {
    if (pixel_size < 6.0f) pixel_size = 6.0f;
    if (pixel_size > 256.0f) pixel_size = 256.0f;
    dai_icons *ic = new dai_icons();
    ic->size = std::floor(pixel_size + 0.5f);
    ic->cell = (int)ic->size + 2;
    ic->icons.reserve(BUILTIN_COUNT);
    for (uint32_t i = 0; i < BUILTIN_COUNT; ++i)
        add_svg(ic, BUILTIN[i].name, BUILTIN[i].svg);
    repack(ic);
    return ic;
}

void dai_icons_free(dai_icons *ic) { delete ic; }

int dai_icons_add(dai_icons *ic, const char *name, const char *svg_text) {
    if (!add_svg(ic, name, svg_text)) return 0;
    repack(ic);
    return 1;
}

int dai_icons_add_file(dai_icons *ic, const char *name, const char *path) {
    if (!ic || !path) return 0;
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return 0; }
    std::string buf((size_t)n, '\0');
    size_t got = std::fread(&buf[0], 1, (size_t)n, f);
    std::fclose(f);
    buf.resize(got);
    return dai_icons_add(ic, name, buf.c_str());
}

const uint8_t *dai_icons_atlas(const dai_icons *ic, uint32_t *w, uint32_t *h) {
    if (!ic) return nullptr;
    repack(const_cast<dai_icons *>(ic));
    if (w) *w = ic->aw;
    if (h) *h = ic->ah;
    return ic->atlas.empty() ? nullptr : ic->atlas.data();
}

const uint8_t *dai_icons_atlas_rgba(dai_icons *ic, uint32_t *w, uint32_t *h) {
    if (!ic) return nullptr;
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
}

int dai_icons_uv(const dai_icons *ic, const char *name,
                 float *u0, float *v0, float *u1, float *v1) {
    if (!ic || !name) return 0;
    repack(const_cast<dai_icons *>(ic));
    for (const Icon &it : ic->icons) {
        if (it.name == name) {
            if (u0) *u0 = it.u0;
            if (v0) *v0 = it.v0;
            if (u1) *u1 = it.u1;
            if (v1) *v1 = it.v1;
            return 1;
        }
    }
    return 0;
}

float    dai_icons_size(const dai_icons *ic)  { return ic ? ic->size : 0.0f; }
uint32_t dai_icons_count(const dai_icons *ic) { return ic ? (uint32_t)ic->icons.size() : 0; }

const char *dai_icons_name(const dai_icons *ic, uint32_t index) {
    if (!ic || index >= ic->icons.size()) return nullptr;
    return ic->icons[index].name.c_str();
}
