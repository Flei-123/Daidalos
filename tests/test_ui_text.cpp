// Does drawn text actually look like text?
//
// This exists because it did not, for a long time, on every platform, and every
// test still passed. The UI names a texture per batch; the renderer used to
// find a descriptor set by scanning the material table for something that
// happened to use that texture, falling back to material 0 when nothing did. A
// font atlas is created directly and belongs to no material, so it always hit
// the fallback and every glyph was sampled from the default white texture.
// Text rendered as rows of solid white boxes.
//
// The UI tests counted vertices and passed. The screenshot tools wrote PNGs
// nobody opened. The bug was found by a user, in a photograph.
//
// So this looks at the pixels. The measure is coverage: what fraction of the
// text's bounding box is lit. Real glyphs at this size cover roughly a quarter
// to a half of their line - letters are mostly holes. Solid boxes cover
// essentially all of it, and a missing font covers none. Anything near 100% is
// the bug coming back.
//
//   DAI_SHADER_DIR=shaders ./build/test_ui_text

#include "dai_font.h"
#include "dai_render.h"
#include "dai_ui.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    const uint32_t W = 512, H = 128;

    char err[256] = { 0 };
    dai_render_desc rd{};
    rd.width = W; rd.height = H; rd.msaa = 1;
    dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
    if (!r) { std::printf("renderer unavailable: %s\n", err); return 77; }

    dai_font *font = dai_font_load_ui(17.0f, err, sizeof(err));
    if (!font) { std::printf("no system font: %s\n", err); dai_render_destroy(r); return 77; }

    uint32_t aw = 0, ah = 0;
    const uint8_t *atlas = dai_font_atlas(font, &aw, &ah);
    std::vector<uint8_t> rgba((size_t)aw * ah * 4);
    for (size_t i = 0; i < (size_t)aw * ah; ++i) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255; rgba[i*4+3] = atlas[i];
    }
    dai_texture font_tex = dai_render_texture_create(r, rgba.data(), aw, ah, 0);
    CHECK(font_tex != 0, "the font atlas did not upload");

    dai_ui *ui = dai_ui_create(font, font_tex);

    dai_ui_input in{};
    dai_ui_begin(ui, (float)W, (float)H, &in);
    // Text and nothing else - no panel. A panel draws a background, a border and
    // a separator, and those are bright pixels that have nothing to do with the
    // glyphs; measuring them as if they were text is how the first version of
    // this check fooled itself.
    //
    // T, not H: H is symmetric top to bottom, so it cannot tell a correctly
    // oriented glyph from an upside down one. That is exactly how a flipped
    // font atlas survived.
    dai_ui_text(ui, 20.0f, 40.0f, "TTTTTTTTTTTTTTTT", 0xFFFFFFFFu);
    dai_ui_end(ui);

    const dai_ui_draw *draws = nullptr;
    uint32_t nb = dai_ui_draws(ui, &draws);
    CHECK(nb > 0, "the UI produced no draws");

    std::vector<dai_ui_vertex> verts;
    std::vector<uint32_t> counts;
    std::vector<dai_texture> texes;
    int text_batches = 0;
    for (uint32_t i = 0; i < nb; ++i) {
        verts.insert(verts.end(), draws[i].vertices, draws[i].vertices + draws[i].count);
        counts.push_back(draws[i].count);
        texes.push_back(draws[i].texture);
        if (draws[i].texture == font_tex) ++text_batches;
    }
    CHECK(text_batches > 0, "no batch referenced the font atlas - the label drew nothing");

    dai_render_camera(r, dai_vec3{ 0, 1, 4 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 100.0f);
    dai_render_ui(r, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), nb);
    CHECK(dai_render_frame(r, nullptr, 0) == DAI_OK, "the frame did not render");

    std::vector<uint8_t> px((size_t)W * H * 4);
    dai_render_readback(r, px.data(), px.size());

    // Find the lit pixels and the box they occupy. The panel behind the text is
    // dark, so "lit" means the glyphs.
    uint32_t minx = W, maxx = 0, miny = H, maxy = 0;
    size_t lit = 0;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t *p = &px[((size_t)y * W + x) * 4];
            if (p[0] > 170 && p[1] > 170 && p[2] > 170) {
                ++lit;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    CHECK(lit > 0, "nothing bright was drawn at all - no text reached the frame");
    if (!lit) {
        dai_ui_destroy(ui); dai_font_free(font); dai_render_destroy(r);
        std::printf("FAILED: %d checks, %d failures\n", g_pass + g_fail, g_fail);
        return 1;
    }

    const double box = (double)(maxx - minx + 1) * (double)(maxy - miny + 1);
    const double coverage = 100.0 * (double)lit / box;
    std::printf("  text box %ux%u at %u,%u - %.1f%% of it is lit\n",
                maxx - minx + 1, maxy - miny + 1, minx, miny, coverage);

    // The load bearing assertion. Letters are mostly holes; solid boxes are not.
    CHECK(coverage < 75.0,
          "text covers %.1f%% of its own box - these are boxes, not glyphs "
          "(the font texture is not reaching the shader)", coverage);
    CHECK(coverage > 8.0, "text covers only %.1f%% of its box - almost nothing was drawn",
          coverage);

    // A row of H's is vertical strokes with gaps between them, so a horizontal
    // scan has to alternate. A row of solid boxes never does.
    //
    // Scan every line and keep the best: the panel draws a separator that is one
    // continuous bright run, and picking the middle line by chance would measure
    // that instead of the text.
    int best = 0;
    uint32_t best_y = miny;
    for (uint32_t y = miny; y <= maxy; ++y) {
        int transitions = 0;
        bool prev_lit = false;
        for (uint32_t x = minx; x <= maxx; ++x) {
            const uint8_t *p = &px[((size_t)y * W + x) * 4];
            const bool is_lit = p[0] > 170 && p[1] > 170 && p[2] > 170;
            if (is_lit != prev_lit) ++transitions;
            prev_lit = is_lit;
        }
        if (transitions > best) { best = transitions; best_y = y; }
    }
    std::printf("  busiest scanline (y=%u): %d light/dark transitions\n", best_y, best);
    CHECK(best >= 8,
          "the busiest row across 16 H's has only %d transitions - the text is drawn "
          "as solid runs, not glyphs", best);

    // ---- orientation --------------------------------------------------------
    //
    // A 'T' is heavy at the top and thin below. Upside down it is still text,
    // still covers the right fraction of its box and still has the right number
    // of transitions - every other check here passes. Only the balance of ink
    // between the halves catches it, and the atlas WAS being written upside
    // down: glyphs were rasterised in font space, where y grows upwards, and
    // stored straight into an image, where it grows down.
    const uint32_t midline = (miny + maxy) / 2;
    size_t top_ink = 0, bottom_ink = 0;
    for (uint32_t y = miny; y <= maxy; ++y) {
        for (uint32_t x = minx; x <= maxx; ++x) {
            const uint8_t *p = &px[((size_t)y * W + x) * 4];
            if (p[0] > 170 && p[1] > 170 && p[2] > 170) {
                if (y <= midline) ++top_ink; else ++bottom_ink;
            }
        }
    }
    std::printf("  ink above the middle: %zu, below: %zu\n", top_ink, bottom_ink);
    CHECK(top_ink > bottom_ink * 2,
          "a row of T's has %zu lit pixels on top and %zu below - the glyphs are "
          "upside down (a T is a bar over a stem)", top_ink, bottom_ink);

    dai_ui_destroy(ui);
    dai_font_free(font);
    dai_render_destroy(r);

    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
