// The UI layer: geometry, interaction and pixels.
//
//   DAI_SHADER_DIR=shaders ./build/test_ui [outdir]

#include "dai_ui.h"
#include "dai_render.h"
#include "ui_cases.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static uint32_t total_verts(dai_ui *ui) {
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d), total = 0;
    for (uint32_t i = 0; i < n; ++i) total += d[i].count;
    return total;
}

int main(int argc, char **argv) {
    std::string outdir = argc > 1 ? argv[1] : "/tmp";
    char err[256] = {0};
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 20.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) return 1;

    dai_ui *ui = dai_ui_create(font, 0);

    // Sections 1 to 5 live in tests/ui_cases.hpp now, and are run from there
    // by this suite and by tests/test_ui_headless.cpp - which is in the
    // headless list, so they are RUN on a machine with no GPU instead of being
    // excluded along with the pixel section below. Same code, one more caller.
    uicases::Counters uc;
    uicases::run_all(ui, &uc);
    g_pass += uc.pass;
    g_fail += uc.fail;

    // ---- 6. and it actually reaches the screen
    {
        dai_render_desc rd{}; rd.width = 640; rd.height = 360; rd.msaa = 1; rd.shadow_size = -1;
        dai_renderer *r = dai_render_create(&rd, err, sizeof(err));
        if (!r) { std::printf("  renderer unavailable (%s), skipping pixels\n", err); }
        else {
            uint32_t aw = 0, ah = 0;
            const uint8_t *rgba = dai_font_atlas_rgba(font, &aw, &ah);
            dai_texture ftex = dai_render_texture_create(r, rgba, aw, ah, 0);
            dai_material_desc md = dai_material_desc_default();
            md.base_color_tex = ftex;
            dai_render_material_create(r, &md);          // so the UI pass can bind it

            dai_ui *ui2 = dai_ui_create(font, ftex);
            dai_render_sky(r, 0);
            dai_render_clear_color(r, 0.05f, 0.06f, 0.08f);
            dai_render_camera(r, dai_vec3{0,1,5}, dai_vec3{0,0,0}, dai_vec3{0,1,0}, 55, 0.1f, 100);

            std::vector<uint8_t> px((size_t)640 * 360 * 4);
            dai_render_ui(r, nullptr, 0, nullptr, nullptr, 0);
            dai_render_frame(r, nullptr, 0);
            dai_render_readback(r, px.data(), px.size());
            double before = 0;
            for (size_t i = 0; i < px.size(); i += 4) before += px[i] + px[i+1] + px[i+2];

            dai_ui_input in{};
            dai_ui_begin(ui2, 640, 360, &in);
            dai_ui_panel_begin(ui2, 24, 24, 320, 180, "Daidalos");
            dai_ui_label(ui2, "Frame 1234  |  60 fps");
            dai_ui_label(ui2, "Bodies: 512   Partikel: 3400");
            dai_ui_separator(ui2);
            dai_ui_button(ui2, "Neu starten");
            float v = 0.7f;
            dai_ui_slider(ui2, "Lautstaerke", &v, 0.0f, 1.0f);
            dai_ui_progress(ui2, 0.42f, "Laden 42%");
            dai_ui_panel_end(ui2);
            dai_ui_end(ui2);

            const dai_ui_draw *d = nullptr;
            uint32_t nb = dai_ui_draws(ui2, &d);
            std::vector<dai_ui_vertex> verts;
            std::vector<uint32_t> counts, texes;
            for (uint32_t i = 0; i < nb; ++i) {
                verts.insert(verts.end(), d[i].vertices, d[i].vertices + d[i].count);
                counts.push_back(d[i].count);
                texes.push_back(d[i].texture);
            }
            dai_render_ui(r, verts.data(), (uint32_t)verts.size(), counts.data(), texes.data(), nb);
            dai_render_frame(r, nullptr, 0);
            dai_render_readback(r, px.data(), px.size());
            double after = 0;
            size_t bright = 0;
            for (size_t i = 0; i < px.size(); i += 4) {
                after += px[i] + px[i+1] + px[i+2];
                if (px[i] > 180 && px[i+1] > 180 && px[i+2] > 180) ++bright;
            }
            dai_render_write_png(r, (outdir + "/ui.png").c_str());
            std::printf("  %u batches, %zu vertices, %zu bright pixels (text)\n", nb, verts.size(), bright);
            CHECK(after > before * 1.05, "the UI did not change the frame (%.0f -> %.0f)", before, after);
            CHECK(bright > 200, "only %zu bright pixels - the glyphs are not drawing", bright);
            dai_ui_destroy(ui2);
            dai_render_destroy(r);
        }
    }

    dai_ui_destroy(ui);
    dai_font_free(font);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
