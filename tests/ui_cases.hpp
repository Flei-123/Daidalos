// The UI cases that need a FONT but no renderer - shared, so both suites run
// the same ones.
//
// tests/test_ui.cpp is on run_tests.sh's "needs GPU" list, and rightly: its
// last section creates a Vulkan device, draws the vertex buffer and counts lit
// pixels, which is the only way to prove that what the layer emits reaches the
// screen. But that is one section out of six. The other five - batching,
// clicks, sliders, checkboxes, the layout cursor - are arithmetic over a glyph
// atlas, they are what every panel in the editor is built out of, and while
// they sat behind the GPU exclusion nothing ran them at all.
//
// So they live here, once, and are run twice: by tests/test_ui.cpp before it
// opens its device, and by tests/test_ui_headless.cpp which never opens one and
// is in the headless suite list. A case moved into this file is not a case that
// got weaker - it is the same code, called from one more place.
//
// The caller supplies the counters, because both suites print their own total.

#ifndef DAI_UI_CASES_HPP
#define DAI_UI_CASES_HPP

#include "dai_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace uicases {

struct Counters {
    int pass = 0;
    int fail = 0;
};

#define UIC_CHECK(cond, ...) do { \
    if (cond) { ++c->pass; } \
    else { ++c->fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

inline uint32_t total_verts(dai_ui *ui) {
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d), total = 0;
    for (uint32_t i = 0; i < n; ++i) total += d[i].count;
    return total;
}

// ---- 1. a frame with widgets produces geometry, an empty one does not
inline void case_geometry(dai_ui *ui, Counters *c) {
    dai_ui_input in{};
    dai_ui_begin(ui, 800, 600, &in);
    dai_ui_end(ui);
    UIC_CHECK(total_verts(ui) == 0, "an empty frame produced %u vertices", total_verts(ui));

    dai_ui_begin(ui, 800, 600, &in);
    dai_ui_panel_begin(ui, 20, 20, 260, 200, "Debug");
    dai_ui_label(ui, "Hallo Welt");
    dai_ui_button(ui, "Start");
    dai_ui_panel_end(ui);
    dai_ui_end(ui);
    uint32_t v = total_verts(ui);
    UIC_CHECK(v > 100, "a panel with a label and a button made only %u vertices", v);
    UIC_CHECK(v % 3 == 0, "vertex count %u is not a multiple of 3 - not triangles", v);
}

// ---- 2. a button reports a click exactly once, on release, and only when the
//         pointer is actually over it
inline void case_button(dai_ui *ui, Counters *c) {
    auto frame = [&](float mx, float my, int down) {
        dai_ui_input in{}; in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 0, 0, 200, 100, nullptr);
        int r = dai_ui_button(ui, "OK");
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        return r;
    };
    UIC_CHECK(frame(100, 20, 0) == 0, "hovering already counted as a click");
    UIC_CHECK(frame(100, 20, 1) == 0, "press alone counted as a click");
    UIC_CHECK(frame(100, 20, 0) == 1, "release over the button did not report a click");
    UIC_CHECK(frame(100, 20, 0) == 0, "the click repeated on the next frame");
    // press inside, release outside: must NOT fire
    frame(100, 20, 1);
    UIC_CHECK(frame(700, 500, 0) == 0, "releasing outside the button still fired it");
    // and the pointer being over UI has to be reported
    frame(100, 20, 0);
    UIC_CHECK(dai_ui_wants_mouse(ui) == 1, "pointer over a panel is not reported as over UI");
    frame(700, 550, 0);
    UIC_CHECK(dai_ui_wants_mouse(ui) == 0, "pointer far from any panel is reported as over UI");
}

// ---- 3. slider follows the pointer and clamps
inline void case_slider(dai_ui *ui, Counters *c) {
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
    UIC_CHECK(std::fabs(value - 5.0f) < 1.5f, "grabbing mid track jumped the value to %.2f",
              (double)value);
    drag(1000, 1);
    UIC_CHECK(std::fabs(value - 10.0f) < 0.01f, "dragging past the end gave %.2f, expected 10",
              (double)value);
    drag(-500, 1);
    UIC_CHECK(std::fabs(value - 0.0f) < 0.01f, "dragging past the start gave %.2f, expected 0",
              (double)value);
}

// ---- 4. checkbox toggles on press
inline void case_checkbox(dai_ui *ui, Counters *c) {
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
    UIC_CHECK(changed == 1 && on == 1, "checkbox did not toggle (changed %d, value %d)",
              changed, on);
}

// ---- 5. sprites from a second texture become their own batch
inline void case_batching(dai_ui *ui, Counters *c) {
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
    UIC_CHECK(n >= 3, "sprite in the middle should split the batches, got %u", n);
    UIC_CHECK(has_font && has_sprite, "batches lost a texture (font %d, sprite %d)",
              (int)has_font, (int)has_sprite);
}

// ---- a row must not leak into the next panel
inline void case_row_leak(dai_ui *ui, Counters *c) {
    dai_ui_input in{};
    dai_ui_begin(ui, 800, 600, &in);
    dai_ui_panel_begin(ui, 0, 0, 400, 60, nullptr);
    dai_ui_row(ui, 24.0f);
    dai_ui_button(ui, "A");
    dai_ui_button(ui, "B");
    dai_ui_panel_end(ui);
    dai_ui_panel_begin(ui, 0, 100, 200, 300, nullptr);
    dai_ui_label(ui, "one");
    dai_ui_label(ui, "two");
    dai_ui_label(ui, "three");
    dai_ui_panel_end(ui);
    dai_ui_end(ui);
    // The three labels stack downwards, so the tallest vertex must be well
    // below the second panel's top edge. If the row leaked they would sit side
    // by side and run off the right of a 200px panel instead.
    const dai_ui_draw *d = nullptr;
    uint32_t nb = dai_ui_draws(ui, &d);
    float maxy = 0, maxx = 0;
    for (uint32_t i = 0; i < nb; ++i)
        for (uint32_t v = 0; v < d[i].count; ++v) {
            if (d[i].vertices[v].y > maxy) maxy = d[i].vertices[v].y;
            if (d[i].vertices[v].x > maxx) maxx = d[i].vertices[v].x;
        }
    UIC_CHECK(maxy > 150.0f, "labels after a row did not stack downwards (max y %.1f)",
              (double)maxy);
    UIC_CHECK(maxx < 420.0f, "a widget ran past the panels (max x %.1f)", (double)maxx);
}

// Everything above, in the order test_ui.cpp used to run it in.
inline void run_all(dai_ui *ui, Counters *c) {
    case_geometry(ui, c);
    case_button(ui, c);
    case_slider(ui, c);
    case_checkbox(ui, c);
    case_batching(ui, c);
    case_row_leak(ui, c);
}

} // namespace uicases

#endif /* DAI_UI_CASES_HPP */
