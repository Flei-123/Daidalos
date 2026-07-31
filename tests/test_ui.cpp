// The UI layer: geometry, interaction and pixels.
//
//   DAI_SHADER_DIR=shaders ./build/test_ui [outdir]

#include "dai_ui.h"
#include "dai_render.h"
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

    // ---- 1. a frame with widgets produces geometry, an empty one does not
    {
        dai_ui_input in{};
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_end(ui);
        CHECK(total_verts(ui) == 0, "an empty frame produced %u vertices", total_verts(ui));

        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 20, 20, 260, 200, "Debug");
        dai_ui_label(ui, "Hallo Welt");
        dai_ui_button(ui, "Start");
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        uint32_t v = total_verts(ui);
        CHECK(v > 100, "a panel with a label and a button made only %u vertices", v);
        CHECK(v % 3 == 0, "vertex count %u is not a multiple of 3 - not triangles", v);
    }

    // ---- 2. a button reports a click exactly once, on release, and only
    //         when the pointer is actually over it
    {
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in{}; in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 200, 100, nullptr);
            int r = dai_ui_button(ui, "OK");
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            return r;
        };
        CHECK(frame(100, 20, 0) == 0, "hovering already counted as a click");
        CHECK(frame(100, 20, 1) == 0, "press alone counted as a click");
        CHECK(frame(100, 20, 0) == 1, "release over the button did not report a click");
        CHECK(frame(100, 20, 0) == 0, "the click repeated on the next frame");
        // press inside, release outside: must NOT fire
        frame(100, 20, 1);
        CHECK(frame(700, 500, 0) == 0, "releasing outside the button still fired it");
        // and the pointer being over UI has to be reported
        frame(100, 20, 0);
        CHECK(dai_ui_wants_mouse(ui) == 1, "pointer over a panel is not reported as over UI");
        frame(700, 550, 0);
        CHECK(dai_ui_wants_mouse(ui) == 0, "pointer far from any panel is reported as over UI");
    }

    // ---- 3. slider follows the pointer and clamps
    {
        float value = 5.0f;
        auto drag = [&](float mx, int down) {
            dai_ui_input in{}; in.mouse_x = mx; in.mouse_y = 10; in.mouse_down = down;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 200, 100, nullptr);
            dai_ui_slider(ui, "Speed", &value, 0.0f, 10.0f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        };
        drag(100, 0);
        drag(100, 1);                     // grab in the middle
        CHECK(fabsf(value - 5.0f) < 1.5f, "grabbing mid track jumped the value to %.2f", value);
        drag(1000, 1);
        CHECK(fabsf(value - 10.0f) < 0.01f, "dragging past the end gave %.2f, expected 10", value);
        drag(-500, 1);
        CHECK(fabsf(value - 0.0f) < 0.01f, "dragging past the start gave %.2f, expected 0", value);
    }

    // ---- 4. checkbox toggles on press
    {
        int on = 0;
        // a click needs an edge, so establish "not pressed" first
        dai_ui_input up{}; up.mouse_x = 20; up.mouse_y = 10; up.mouse_down = 0;
        dai_ui_begin(ui, 800, 600, &up);
        dai_ui_panel_begin(ui, 0, 0, 200, 100, nullptr);
        dai_ui_checkbox(ui, "Vsync", &on);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);

        dai_ui_input in{}; in.mouse_x = 20; in.mouse_y = 10; in.mouse_down = 1;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 0, 0, 200, 100, nullptr);
        int changed = dai_ui_checkbox(ui, "Vsync", &on);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        CHECK(changed == 1 && on == 1, "checkbox did not toggle (changed %d, value %d)", changed, on);
    }

    // ---- 5. sprites from a second texture become their own batch
    {
        dai_ui_input in{};
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 0, 0, 200, 200, nullptr);
        dai_ui_label(ui, "Icons");
        dai_ui_image(ui, 7, 32, 32, 0.0f, 0.0f, 0.5f, 0.5f, 0xFFFFFFFF);
        dai_ui_label(ui, "after");
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        const dai_ui_draw *d = nullptr;
        uint32_t n = dai_ui_draws(ui, &d);
        bool has_font = false, has_sprite = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (d[i].texture == 0) has_font = true;
            if (d[i].texture == 7) has_sprite = true;
        }
        CHECK(n >= 3, "sprite in the middle should split the batches, got %u", n);
        CHECK(has_font && has_sprite, "batches lost a texture (font %d, sprite %d)", has_font, has_sprite);
    }

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
