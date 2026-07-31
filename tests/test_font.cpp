// TrueType parsing and rasterisation, checked against facts that are true of
// any correct font renderer - no reference images, which would only pin down
// this rasteriser's exact anti-aliasing rather than its correctness.
//
//   ./build/test_font [/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf]

#include "dai_font.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static float ink(const uint8_t *atlas, uint32_t aw, const dai_glyph *g, uint32_t ah) {
    if (!g) return 0.0f;
    uint32_t x0 = (uint32_t)(g->u0 * aw), x1 = (uint32_t)(g->u1 * aw);
    uint32_t y0 = (uint32_t)(g->v0 * ah), y1 = (uint32_t)(g->v1 * ah);
    double sum = 0; uint32_t n = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) { sum += atlas[(size_t)y * aw + x]; ++n; }
    return n ? (float)(sum / n / 255.0) : 0.0f;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    char err[256] = {0};
    dai_font *f = dai_font_load(path, 32.0f, nullptr, 0, err, sizeof(err));
    CHECK(f != nullptr, "loading %s failed: %s", path, err);
    if (!f) return 1;

    uint32_t aw = 0, ah = 0;
    const uint8_t *atlas = dai_font_atlas(f, &aw, &ah);
    std::printf("  atlas %ux%u, line height %.1f px, ascent %.1f px\n",
                aw, ah, dai_font_line_height(f), dai_font_ascent(f));
    CHECK(aw >= 128 && ah >= 64, "atlas is %ux%u, implausibly small", aw, ah);
    CHECK(dai_font_line_height(f) > 32.0f && dai_font_line_height(f) < 64.0f,
          "line height %.1f for a 32 px font is out of range", dai_font_line_height(f));

    // space carries no ink but must advance
    const dai_glyph *sp = dai_font_glyph(f, ' ');
    CHECK(sp && sp->advance > 3.0f, "space has no advance");
    CHECK(sp && ink(atlas, aw, sp, ah) < 0.01f, "space has ink in it");

    // a filled letter must actually have coverage, and more than a thin one
    const dai_glyph *M = dai_font_glyph(f, 'M');
    const dai_glyph *i = dai_font_glyph(f, 'i');
    CHECK(M && ink(atlas, aw, M, ah) > 0.25f, "'M' rasterised to %.3f coverage - the fill is broken",
          M ? ink(atlas, aw, M, ah) : 0.0f);
    CHECK(M && i && M->advance > i->advance, "'M' is not wider than 'i' (%.1f vs %.1f)",
          M ? M->advance : 0, i ? i->advance : 0);

    // 'o' has a hole: the middle must be lighter than the sides. This is the
    // test that catches a rasteriser using the wrong winding rule.
    const dai_glyph *o = dai_font_glyph(f, 'o');
    if (o) {
        uint32_t x0 = (uint32_t)(o->u0 * aw), x1 = (uint32_t)(o->u1 * aw);
        uint32_t y0 = (uint32_t)(o->v0 * ah), y1 = (uint32_t)(o->v1 * ah);
        uint32_t cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        int centre = atlas[(size_t)cy * aw + cx];
        int left   = atlas[(size_t)cy * aw + x0 + 1];
        std::printf("  'o' centre %d, left edge %d\n", centre, left);
        CHECK(centre < 96 && left > 96, "'o' has no hole (centre %d, edge %d) - winding rule is wrong",
              centre, left);
    }

    // accented letters come from the Latin-1 range and are composite glyphs
    const dai_glyph *a_uml = dai_font_glyph(f, 0xE4);      // a umlaut
    CHECK(a_uml && ink(atlas, aw, a_uml, ah) > 0.15f, "composite glyph (a with umlaut) is empty");

    // measuring: pure arithmetic over advances, and UTF-8 must decode
    uint32_t glyphs = 0;
    float w = dai_font_measure(f, "Hallo Welt", &glyphs);
    CHECK(glyphs == 10, "measured %u glyphs in \"Hallo Welt\", expected 10", glyphs);
    CHECK(w > 60.0f && w < 300.0f, "\"Hallo Welt\" is %.1f px at 32 px height, implausible", w);
    float w2 = dai_font_measure(f, "Grusse: Gruesse", &glyphs);
    CHECK(w2 > w, "longer string measured shorter");

    uint32_t off = 0;
    std::string utf8 = "A\xC3\xA4\xE2\x82\xAC";           // A, a-umlaut, euro sign
    uint32_t c1 = dai_utf8_next(utf8.c_str(), &off);
    uint32_t c2 = dai_utf8_next(utf8.c_str(), &off);
    uint32_t c3 = dai_utf8_next(utf8.c_str(), &off);
    CHECK(c1 == 'A' && c2 == 0xE4 && c3 == 0x20AC, "UTF-8 decode gave %u %u %u", c1, c2, c3);
    CHECK(dai_utf8_next(utf8.c_str(), &off) == 0, "UTF-8 decode did not stop at the end");

    // rgba expansion keeps the coverage in alpha
    uint32_t rw = 0, rh = 0;
    const uint8_t *rgba = dai_font_atlas_rgba(f, &rw, &rh);
    CHECK(rw == aw && rh == ah, "rgba atlas is a different size");
    bool same = true;
    for (size_t k = 0; k < (size_t)aw * ah && same; ++k)
        if (rgba[k * 4 + 3] != atlas[k] || rgba[k * 4] != 255) same = false;
    CHECK(same, "rgba expansion does not match the coverage atlas");

    dai_font_free(f);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
