// The UI layer WITHOUT a device - everything except the pixels.
//
//   ./build/test_ui_headless
//
// tests/test_ui.cpp opens a Vulkan renderer in its last section to prove that
// the vertex buffer reaches the screen, so the whole suite is on run_tests.sh's
// "needs GPU" list and none of it ran here. Its five GPU-less sections live in
// tests/ui_cases.hpp now and are run from both files; this suite runs them and
// then asks the rest of the questions a panel can get wrong with no screen
// anywhere near it:
//
//   * a scrolled region clips what leaves it, and gives the wheel back when
//     there is nothing left to scroll
//   * a widget under a popup is dead, and the popup is drawn above it
//   * a text field takes typed characters, a caret and a commit
//   * dai_ui_last_rect answers where the last widget actually went
//   * two identical frames produce identical vertices - the property every
//     screenshot in .gauntlet-shots/ rests on
//
// A font is needed (the glyph atlas is CPU side); a device is not.

#include "dai_ui.h"
#include "ui_cases.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static void bounds(dai_ui *ui, float *x0, float *y0, float *x1, float *y1) {
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d);
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t v = 0; v < d[i].count; ++v) {
            const dai_ui_vertex &p = d[i].vertices[v];
            if (p.x < *x0) *x0 = p.x;
            if (p.y < *y0) *y0 = p.y;
            if (p.x > *x1) *x1 = p.x;
            if (p.y > *y1) *y1 = p.y;
        }
}

static std::vector<dai_ui_vertex> snapshot(dai_ui *ui) {
    std::vector<dai_ui_vertex> out;
    const dai_ui_draw *d = nullptr;
    uint32_t n = dai_ui_draws(ui, &d);
    for (uint32_t i = 0; i < n; ++i)
        out.insert(out.end(), d[i].vertices, d[i].vertices + d[i].count);
    return out;
}

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    dai_ui *ui = dai_ui_create(font, 0);
    CHECK(ui != nullptr, "the UI layer would not start without a renderer");
    if (!ui) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

    // ---- the shared cases, run on a machine with no GPU ---------------------
    uicases::Counters uc;
    uicases::run_all(ui, &uc);
    g_pass += uc.pass;
    g_fail += uc.fail;
    std::printf("  shared UI cases: %d passed, %d failed\n", uc.pass, uc.fail);

    // ---- a scrolled region clips, and stops at its end ----------------------
    {
        auto scrolled = [&](float wheel, float *maxy) {
            dai_ui_input in{};
            in.mouse_x = 100.0f; in.mouse_y = 100.0f; in.wheel = wheel;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 220, 200, nullptr);
            dai_ui_scroll_begin(ui, "rows", 160.0f);
            for (int i = 0; i < 40; ++i) {
                char t[32];
                std::snprintf(t, sizeof(t), "row %02d", i);
                dai_ui_label(ui, t);
            }
            dai_ui_scroll_end(ui);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            float x0, y0, x1, y1;
            bounds(ui, &x0, &y0, &x1, &y1);
            *maxy = y1;
        };
        float m0 = 0.0f;
        scrolled(0.0f, &m0);
        CHECK(m0 < 210.0f, "forty rows in a 200 px panel drew down to y=%.1f - nothing clipped them",
              (double)m0);
        // The wheel moves it, and the limit is last frame's, so two frames are
        // what a real host gives it.
        float m1 = 0.0f;
        scrolled(-3.0f, &m1);
        scrolled(-3.0f, &m1);
        CHECK(m1 < 210.0f, "the scrolled region drew past its panel (y=%.1f)", (double)m1);
        // ...and rolling the other way past the top changes nothing.
        float top_a = 0.0f, top_b = 0.0f;
        scrolled(20.0f, &top_a);
        scrolled(20.0f, &top_b);
        CHECK(std::fabs(top_a - top_b) < 0.01f,
              "scrolling up at the top kept moving (%.2f then %.2f)", (double)top_a, (double)top_b);
    }

    // ---- a popup covers what is under it ------------------------------------
    {
        int clicked_under = 0;
        dai_ui_popup menu{};
        auto frame = [&](int open_menu, int down) {
            dai_ui_input in{};
            in.mouse_x = 60.0f; in.mouse_y = 18.0f; in.mouse_down = down;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 240, 160, nullptr);
            if (dai_ui_button(ui, "Under")) clicked_under = 1;
            dai_ui_panel_end(ui);
            if (open_menu) {
                static const dai_ui_menu_item items[2] = { { nullptr, "One", nullptr, 0 },
                                                           { nullptr, "Two", nullptr, 0 } };
                if (!menu.open) dai_ui_popup_open(&menu, 40.0f, 20.0f);
                dai_ui_popup_menu(ui, &menu, items, 2);
            }
            dai_ui_end(ui);
        };
        frame(0, 0);
        frame(0, 1);
        frame(0, 0);
        CHECK(clicked_under == 1, "the button under the popup does not work with no popup open");
        clicked_under = 0;
        frame(1, 0);
        frame(1, 1);
        frame(1, 0);
        CHECK(clicked_under == 0, "a click landed on the button UNDER an open popup");
    }

    // ---- a text field takes what is typed -----------------------------------
    {
        char buf[64] = "start";
        auto frame = [&](const char *typed, int down, float mx) {
            dai_ui_input in{};
            in.mouse_x = mx; in.mouse_y = 30.0f; in.mouse_down = down;
            // Code points, edge triggered - what a window backend hands over.
            for (int k = 0; typed && typed[k] && k < 7; ++k) in.text[k] = (uint32_t)typed[k];
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 300, 120, nullptr);
            int ch = dai_ui_text_field(ui, "tf", 10.0f, 20.0f, 200.0f, 20.0f,
                                       buf, sizeof(buf), nullptr);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            return ch;
        };
        frame(nullptr, 0, 60.0f);
        frame(nullptr, 1, 60.0f);       // click into it: now editing
        frame(nullptr, 0, 60.0f);
        int changed = frame("XY", 0, 60.0f);
        CHECK(std::strstr(buf, "XY") != nullptr,
              "typing into a focused field did nothing (\"%s\")", buf);
        CHECK(changed != 0, "the field did not report the typed characters");
        std::snprintf(buf, sizeof(buf), "%s", "start");
        frame(nullptr, 1, 600.0f);      // click far away: focus gone
        frame(nullptr, 0, 600.0f);
        frame("ZZ", 0, 600.0f);
        CHECK(std::strcmp(buf, "start") == 0,
              "an unfocused field took the keyboard (\"%s\")", buf);
    }

    // ---- where the last ROW went -------------------------------------------
    // dai_ui_last_rect is what the hierarchy's drag and drop reads: the row
    // just drawn, so a drop can be aimed at it. A panel that lays its rows out
    // and then reports the wrong rectangle is a panel where the drop lands one
    // row above what the pointer is on - which happened, and cost nine checks
    // in test_editor_ui before anyone noticed.
    {
        dai_ui_input in{};
        int open_a = 1;
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 30, 40, 250, 200, nullptr);
        dai_ui_tree_item_icon(ui, nullptr, "first", 0, 1, &open_a, 0);
        float ax = 0, ay = 0, aw = 0, ah = 0;
        dai_ui_last_rect(ui, &ax, &ay, &aw, &ah);
        dai_ui_tree_item_icon(ui, nullptr, "second", 1, 0, nullptr, 0);
        float bx = 0, by = 0, bw = 0, bh = 0;
        dai_ui_last_rect(ui, &bx, &by, &bw, &bh);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        CHECK(ax >= 30.0f && ax < 280.0f, "the first row claims x=%.1f, outside its panel", (double)ax);
        CHECK(ay > 40.0f && ay < 240.0f, "the first row claims y=%.1f, outside its panel", (double)ay);
        CHECK(aw > 8.0f && ah > 8.0f, "the first row claims to be %.1fx%.1f", (double)aw, (double)ah);
        CHECK(ax + aw <= 30.0f + 250.0f + 0.5f,
              "the first row claims to reach x=%.1f, past its 250 px panel", (double)(ax + aw));
        CHECK(by > ay + 4.0f, "the second row is not below the first (%.1f vs %.1f)",
              (double)by, (double)ay);
        CHECK(std::fabs(bh - ah) < 0.01f, "two rows of the same font are %.1f and %.1f tall",
              (double)ah, (double)bh);
        CHECK(std::fabs(bx - ax) < 0.01f && std::fabs(bw - aw) < 0.01f,
              "the indented row reports a different rectangle than the row above it");
    }

    // ---- the frame settles in ONE frame and then never drifts ---------------
    // Every picture in .gauntlet-shots/ is taken by drawing the same frame
    // twice - once to lay the dock out, once to photograph it - and a layer
    // that drifts between them is a screenshot nobody can diff.
    //
    // "Twice" is not decoration. The label column is a one frame handover on
    // purpose (see LabelCol in src/dai_ui.cpp): a row cannot know how wide the
    // widest name in its panel is until every row has been through, so this
    // frame uses what last frame measured and the value boxes line up. A panel
    // drawn for the FIRST time therefore lands its fields a few pixels off and
    // is right from the second frame on. That is the contract, so that is what
    // is checked: same vertex count immediately, bit identical from the second
    // frame onwards, and no drift after that however long it runs.
    {
        auto draw = [&]() {
            dai_ui_input in{};
            in.mouse_x = 123.0f; in.mouse_y = 77.0f;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_ui_panel_begin(ui, 12, 12, 320, 400, "Panel");
            dai_ui_label(ui, "a label");
            dai_ui_button(ui, "a button");
            float f = 1.025f;
            dai_ui_num_field(ui, "Height", &f, 0.01f, 0.0f, 10.0f, "h");
            float v3[3] = { 1.0f, 0.05f, -2.5f };
            dai_ui_num_vec3(ui, "Size", v3, 0.01f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
            return snapshot(ui);
        };
        std::vector<dai_ui_vertex> a = draw();
        std::vector<dai_ui_vertex> b = draw();
        std::vector<dai_ui_vertex> c = draw();
        std::vector<dai_ui_vertex> d = draw();
        CHECK(!a.empty(), "the frame drew nothing");
        // The count is settled from the very first frame: a column that is
        // still measuring itself may move a box, it may not make one appear.
        CHECK(a.size() == b.size() && b.size() == c.size() && c.size() == d.size(),
              "the frame made %u, %u, %u and %u vertices",
              (uint32_t)a.size(), (uint32_t)b.size(), (uint32_t)c.size(), (uint32_t)d.size());
        int bc = b.size() == c.size() &&
                 std::memcmp(b.data(), c.data(), b.size() * sizeof(dai_ui_vertex)) == 0;
        int cd = c.size() == d.size() &&
                 std::memcmp(c.data(), d.data(), c.size() * sizeof(dai_ui_vertex)) == 0;
        CHECK(bc, "frames two and three are not bit identical - the layout is still drifting");
        CHECK(cd, "frames three and four are not bit identical - the layout never settles");
        // And the settling is SMALL and horizontal: the label column decides
        // where a value box starts, not how tall a row is or what colour it
        // has. A first frame that moves something vertically or recolours it
        // is not a column measuring itself, it is a bug.
        int drift_rows = 0, bad_drift = 0;
        for (size_t i2 = 0; i2 < a.size() && i2 < b.size(); ++i2) {
            if (std::memcmp(&a[i2], &b[i2], sizeof(dai_ui_vertex)) == 0) continue;
            ++drift_rows;
            if (a[i2].y != b[i2].y || a[i2].color != b[i2].color ||
                std::fabs(a[i2].x - b[i2].x) > 40.0f)
                ++bad_drift;
        }
        CHECK(bad_drift == 0,
              "%d of the %d vertices that moved between the first two frames moved "
              "vertically, changed colour or jumped more than 40 px", bad_drift, drift_rows);
        std::printf("  the same frame four times: %u vertices, %d moved once and then never again\n",
                    (uint32_t)a.size(), drift_rows);
    }

    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
