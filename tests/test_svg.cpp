// The SVG rasteriser and the icon atlas built on top of it.
//
// This test reads PIXELS, not structures. A parser that returns the right
// number of shapes and then draws nothing is the exact failure the font had
// for weeks - every glyph "worked", and the interface was a row of white
// boxes. So: is there ink, is it where the path said, is the hole a hole, and
// is an icon a drawing rather than a filled square.

#include "dai_svg.h"
#include "dai_icons.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static std::vector<uint8_t> raster(const char *svg, int w, int h, float pad = 0.0f) {
    char err[128] = { 0 };
    std::vector<uint8_t> px((size_t)w * h, 0);
    dai_svg *doc = dai_svg_parse(svg, 0, err, sizeof(err));
    if (!doc) { std::printf("  parse failed: %s\n", err); return px; }
    dai_svg_rasterize(doc, px.data(), w, h, pad);
    dai_svg_free(doc);
    return px;
}

static int at(const std::vector<uint8_t> &px, int w, int x, int y) {
    return (int)px[(size_t)y * w + x];
}

static double ink(const std::vector<uint8_t> &px) {
    double s = 0;
    for (uint8_t v : px) s += v;
    return s / (255.0 * (double)px.size());
}

// A crude ASCII dump - the fastest way to see WHY a shape is wrong, and the
// thing that found the upside down font atlas.
static void dump(const std::vector<uint8_t> &px, int w, int h, const char *title) {
    std::printf("  %s\n", title);
    const char *ramp = " .:-=+*#%@";
    for (int y = 0; y < h; ++y) {
        std::printf("    ");
        for (int x = 0; x < w; ++x) std::printf("%c", ramp[at(px, w, x, y) * 9 / 255]);
        std::printf("\n");
    }
}

int main() {
    std::printf("==================================\n");
    std::printf(" SVG\n");
    std::printf("==================================\n");

    // ---- 1. a filled rectangle lands where it was asked to -----------------
    std::printf("\n[1] Gefuelltes Rechteck\n");
    {
        auto px = raster("<svg viewBox='0 0 10 10'><rect x='2' y='2' width='6' height='6' "
                         "fill='black'/></svg>", 10, 10);
        CHECK(at(px, 10, 5, 5) > 250, "the middle of a filled rect is %d, not solid", at(px, 10, 5, 5));
        CHECK(at(px, 10, 0, 0) == 0, "ink outside the rect at the corner (%d)", at(px, 10, 0, 0));
        CHECK(at(px, 10, 1, 5) == 0, "ink one pixel left of the rect");
        CHECK(at(px, 10, 2, 2) > 250, "the rect's own top left corner is empty");
        double frac = ink(px);
        CHECK(frac > 0.34 && frac < 0.38, "6x6 of 10x10 should cover 36%%, covers %.1f%%", frac * 100);
    }

    // ---- 2. a stroke is ink ON the line and nowhere else --------------------
    std::printf("\n[2] Linie mit Strichbreite\n");
    {
        auto px = raster("<svg viewBox='0 0 20 20'><line x1='2' y1='10' x2='18' y2='10' "
                         "stroke='black' stroke-width='4' fill='none'/></svg>", 20, 20);
        CHECK(at(px, 20, 10, 10) > 250, "nothing on the line itself (%d)", at(px, 20, 10, 10));
        CHECK(at(px, 20, 10, 3) == 0, "ink seven pixels above a horizontal line");
        CHECK(at(px, 20, 10, 15) == 0, "ink five pixels below a horizontal line");
        // 16 long, 4 wide, in 400 pixels
        double frac = ink(px);
        CHECK(frac > 0.14 && frac < 0.18, "a 16x4 stroke should cover 16%%, covers %.1f%%", frac * 100);
    }

    // ---- 3. fill-rule: the hole has to be a hole ---------------------------
    std::printf("\n[3] Loch (evenodd und nonzero)\n");
    {
        // Two concentric circles, one path, even-odd.
        auto eo = raster("<svg viewBox='0 0 20 20'><path fill-rule='evenodd' fill='black' "
                         "d='M10 2A8 8 0 1 0 10 18A8 8 0 1 0 10 2Z "
                         "M10 6A4 4 0 1 1 10 14A4 4 0 1 1 10 6Z'/></svg>", 20, 20);
        CHECK(at(eo, 20, 10, 10) == 0, "even-odd filled the hole in (%d)", at(eo, 20, 10, 10));
        CHECK(at(eo, 20, 10, 3) > 200, "the outer ring is missing");

        // The same shape relying on winding direction instead: the inner circle
        // is swept the OTHER way (flag 1 instead of 0), so its winding cancels
        // the outer one. Reorienting subpaths "for consistency" fills this one
        // in, which is exactly why the rasteriser must not do that.
        auto nz = raster("<svg viewBox='0 0 20 20'><path fill='black' "
                         "d='M10 2A8 8 0 1 0 10 18A8 8 0 1 0 10 2Z "
                         "M10 6A4 4 0 1 1 10 14A4 4 0 1 1 10 6Z'/></svg>", 20, 20);
        CHECK(at(nz, 20, 10, 10) == 0, "non-zero winding filled the hole in (%d)", at(nz, 20, 10, 10));
        CHECK(at(nz, 20, 10, 3) > 200, "the outer ring is missing under non-zero");

        // ...and with both rings the same way round it is solid, which is the
        // control: if this one is hollow too, the hole is an accident.
        auto solid = raster("<svg viewBox='0 0 20 20'><path fill='black' "
                            "d='M10 2A8 8 0 1 0 10 18A8 8 0 1 0 10 2Z "
                            "M10 6A4 4 0 1 0 10 14A4 4 0 1 0 10 6Z'/></svg>", 20, 20);
        CHECK(at(solid, 20, 10, 10) > 200, "two rings wound the same way left a hole");
    }

    // ---- 4. curves and arcs actually curve ---------------------------------
    std::printf("\n[4] Kurven\n");
    {
        // A quarter disc in the top left: the arc bulges out past the chord.
        auto px = raster("<svg viewBox='0 0 20 20'><path fill='black' "
                         "d='M2 2 L18 2 A16 16 0 0 1 2 18 Z'/></svg>", 20, 20);
        CHECK(at(px, 20, 4, 4) > 200, "the corner inside the quarter disc is empty");
        CHECK(at(px, 20, 17, 17) == 0, "ink outside the arc");
        // Halfway along the diagonal the arc is still inside the shape, the
        // straight chord would already have cut it off.
        CHECK(at(px, 20, 12, 12) > 128, "the arc is flat - it cut the corner (%d)", at(px, 20, 12, 12));

        auto cub = raster("<svg viewBox='0 0 20 20'><path fill='none' stroke='black' "
                          "stroke-width='2' d='M2 18 C2 2 18 2 18 18'/></svg>", 20, 20);
        CHECK(at(cub, 20, 10, 5) > 100, "the top of a cubic arch has no ink (%d)", at(cub, 20, 10, 5));
        CHECK(at(cub, 20, 10, 15) == 0, "the inside of the arch is filled");
    }

    // ---- 5. transforms -----------------------------------------------------
    std::printf("\n[5] transform\n");
    {
        auto px = raster("<svg viewBox='0 0 20 20'><g transform='translate(10,0)'>"
                         "<rect x='0' y='0' width='6' height='6' fill='black'/></g></svg>", 20, 20);
        CHECK(at(px, 20, 12, 3) > 250, "translate did not move the rect");
        CHECK(at(px, 20, 3, 3) == 0, "the rect is still at the origin as well");

        auto sc = raster("<svg viewBox='0 0 20 20'><g transform='scale(2)'>"
                         "<rect x='1' y='1' width='4' height='4' fill='black'/></g></svg>", 20, 20);
        CHECK(at(sc, 20, 9, 9) > 250, "scale(2) did not grow the rect");
        CHECK(at(sc, 20, 11, 11) == 0, "scale(2) grew it too far");
    }

    // ---- 6. fill='none' means none -----------------------------------------
    std::printf("\n[6] fill=none, style=\"\"\n");
    {
        auto px = raster("<svg viewBox='0 0 20 20'><rect x='2' y='2' width='16' height='16' "
                         "fill='none' stroke='black' stroke-width='2'/></svg>", 20, 20);
        CHECK(at(px, 20, 10, 10) == 0, "fill='none' was filled anyway (%d)", at(px, 20, 10, 10));
        CHECK(at(px, 20, 10, 2) > 200, "the outline is missing");

        // the same written as a style attribute, which is the other half of
        // what icon exporters emit
        auto st = raster("<svg viewBox='0 0 20 20'><rect x='2' y='2' width='16' height='16' "
                         "style='fill:none;stroke:black;stroke-width:2'/></svg>", 20, 20);
        CHECK(at(st, 20, 10, 10) == 0, "style='fill:none' was ignored");
        CHECK(at(st, 20, 10, 2) > 200, "style='stroke' was ignored");
    }

    // ---- 7. the same icon at two sizes -------------------------------------
    std::printf("\n[7] Dieselbe Quelle, zwei Groessen\n");
    {
        const char *check = "<svg viewBox='0 0 24 24' fill='none' stroke='black' "
                            "stroke-width='2' stroke-linecap='round'>"
                            "<polyline points='20 6 9 17 4 12'/></svg>";
        auto small = raster(check, 16, 16, 1.0f);
        auto big   = raster(check, 48, 48, 3.0f);
        double a = ink(small), b = ink(big);
        // Coverage is SUPPOSED to match: a 2 unit stroke on a 24 unit box is a
        // twelfth of the width at every size. That is what makes it a vector.
        std::printf("  Deckung 16px %.1f%%, 48px %.1f%%\n", a * 100, b * 100);
        CHECK(a > 0.03 && a < 0.45, "the 16 px tick covers %.1f%% - blank or a blob", a * 100);
        CHECK(std::fabs(a - b) < 0.02, "the same drawing covers %.1f%% at 16 px and %.1f%% at 48",
              a * 100, b * 100);
        // What actually separates "rasterised at 48" from "rasterised at 16 and
        // stretched": how many pixels are FULLY covered. At 16 px the tick's
        // stroke is about 1.3 px wide, so nearly every pixel of it is partial;
        // at 48 px it is 4 px wide with a solid core. A stretched bitmap has
        // the small image's soft edges no matter how big it is drawn.
        auto solid_count = [](const std::vector<uint8_t> &px) {
            int n = 0;
            for (uint8_t v : px) if (v == 255) ++n;
            return n;
        };
        int s16 = solid_count(small), s48 = solid_count(big);
        std::printf("  voll gedeckte Pixel: 16px %d, 48px %d\n", s16, s48);
        CHECK(s48 > 60, "the 48 px tick has only %d solid pixels - it was scaled, not drawn", s48);
        CHECK(s48 > s16 * 20, "solid pixels barely grew (%d -> %d)", s16, s48);
        dump(small, 16, 16, "check, 16x16:");
    }

    // ---- 8. the built-in set -----------------------------------------------
    std::printf("\n[8] Eingebautes Icon-Set\n");
    {
        dai_icons *ic = dai_icons_create(16.0f);
        CHECK(ic != nullptr, "the icon set did not build");
        uint32_t n = dai_icons_count(ic);
        std::printf("  %u Icons, Atlas ", n);
        uint32_t aw = 0, ah = 0;
        const uint8_t *atlas = dai_icons_atlas(ic, &aw, &ah);
        std::printf("%ux%u\n", aw, ah);
        CHECK(n >= 25, "only %u icons in the built-in set", n);
        CHECK(atlas != nullptr && aw > 0 && ah > 0, "no atlas");

        // Every single one has to be a drawing: not empty, and not a solid
        // block. A solid block is what a rasteriser produces when it fills
        // everything it fails to understand, and it is invisible in a
        // screenshot of a dark interface.
        int empty = 0, blob = 0;
        std::string worst_empty, worst_blob;
        for (uint32_t i = 0; i < n; ++i) {
            const char *name = dai_icons_name(ic, i);
            float u0, v0, u1, v1;
            CHECK(dai_icons_uv(ic, name, &u0, &v0, &u1, &v1) == 1, "no uv for '%s'", name);
            uint32_t x0 = (uint32_t)(u0 * (float)aw + 0.5f), y0 = (uint32_t)(v0 * (float)ah + 0.5f);
            uint32_t x1 = (uint32_t)(u1 * (float)aw + 0.5f), y1 = (uint32_t)(v1 * (float)ah + 0.5f);
            double sum = 0;
            uint32_t cnt = 0;
            for (uint32_t y = y0; y < y1; ++y)
                for (uint32_t x = x0; x < x1; ++x) { sum += atlas[(size_t)y * aw + x]; ++cnt; }
            double frac = cnt ? sum / (255.0 * cnt) : 0.0;
            if (frac < 0.04) { ++empty; if (worst_empty.empty()) worst_empty = name; }
            if (frac > 0.80) { ++blob;  if (worst_blob.empty())  worst_blob = name; }
        }
        CHECK(empty == 0, "%d icons are blank, e.g. '%s'", empty, worst_empty.c_str());
        CHECK(blob == 0, "%d icons are solid blocks, e.g. '%s'", blob, worst_blob.c_str());

        CHECK(dai_icons_uv(ic, "no-such-icon", nullptr, nullptr, nullptr, nullptr) == 0,
              "an unknown icon name returned a uv rectangle");

        // Icons do not overlap in the atlas: read the one texel column between
        // two neighbours, it has to be empty.
        CHECK(dai_icons_size(ic) == 16.0f, "asked for 16 px, got %.0f", dai_icons_size(ic));

        // A project's own icon, added from source.
        int added = dai_icons_add(ic, "custom-dot",
            "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='9' fill='black'/></svg>");
        CHECK(added == 1, "adding an icon from SVG source failed");
        CHECK(dai_icons_count(ic) == n + 1, "the added icon is not in the set");
        CHECK(dai_icons_uv(ic, "custom-dot", nullptr, nullptr, nullptr, nullptr) == 1,
              "the added icon has no uv");

        // and one drawn to look at
        {
            const uint8_t *a2 = dai_icons_atlas(ic, &aw, &ah);
            float u0, v0, u1, v1;
            dai_icons_uv(ic, "play", &u0, &v0, &u1, &v1);
            uint32_t x0 = (uint32_t)(u0 * (float)aw + 0.5f), y0 = (uint32_t)(v0 * (float)ah + 0.5f);
            std::vector<uint8_t> tile(16 * 16);
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x)
                    tile[(size_t)y * 16 + x] = a2[(size_t)(y0 + (uint32_t)y) * aw + (x0 + (uint32_t)x)];
            dump(tile, 16, 16, "play, aus dem Atlas:");
        }

        dai_icons_free(ic);
    }

    // ---- 9. junk in, nothing out (not a crash) -----------------------------
    std::printf("\n[9] Muell\n");
    {
        char err[128];
        CHECK(dai_svg_parse("", 0, err, sizeof(err)) == nullptr, "empty text parsed");
        CHECK(dai_svg_parse("<svg></svg>", 0, err, sizeof(err)) == nullptr, "an empty svg parsed");
        CHECK(dai_svg_parse("<svg viewBox='0 0 1 1'><path d='M M M z'/></svg>", 0, err, sizeof(err)) == nullptr,
              "a path with no geometry produced a shape");
        dai_svg *doc = dai_svg_parse("<svg viewBox='0 0 10 10'><rect width='5' height='5' fill='black'/>"
                                     "<path d='M0 0 Q'/></svg>", 0, err, sizeof(err));
        CHECK(doc != nullptr, "a truncated path threw away the whole document");
        if (doc) {
            std::vector<uint8_t> px(100, 0);
            CHECK(dai_svg_rasterize(doc, px.data(), 10, 10, 0) == 1, "rasterising failed");
            CHECK(ink(px) > 0.2, "the good half of the document did not draw");
            dai_svg_free(doc);
        }
    }

    std::printf("\n==================================\n");
    std::printf("  %d bestanden, %d fehlgeschlagen\n", g_pass, g_fail);
    std::printf("==================================\n");
    return g_fail ? 1 : 0;
}
