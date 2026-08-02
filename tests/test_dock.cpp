// Docked panels tile, they never overlap.
//
//   ./build/test_dock
//
// This test exists because the previous layout system let the scene view lie
// on top of the inspector and swallow its clicks. In a tree that cannot
// happen, and this file proves it: every visible panel rectangle is checked
// against every other one for overlap, after splits, after tab drags, after
// tearing a panel out into a floating window and dropping it back.

#include "dai_dock.h"

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

struct R { float x, y, w, h; };

static bool overlap(const R &a, const R &b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) return 1;
    dai_ui *ui = dai_ui_create(font, 0);

    dai_dock *dock = dai_dock_create();
    dai_dock_add(dock, "Scene", DAI_DOCK_NONE, 0.0f);
    dai_dock_add_tab(dock, "Game", "Scene");
    dai_dock_add(dock, "Hierarchy", DAI_DOCK_LEFT, 0.20f);
    dai_dock_add(dock, "Inspector", DAI_DOCK_RIGHT, 0.22f);
    dai_dock_add(dock, "Project", DAI_DOCK_BOTTOM, 0.28f);

    const char *ALL[] = { "Scene", "Game", "Hierarchy", "Inspector", "Project" };
    const float W = 1280.0f, H = 720.0f;

    std::vector<R> rects;
    auto frame = [&](float mx, float my, int down) {
        rects.clear();
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, W, H, &in);
        dai_dock_begin(dock, ui, 0, 30, W, H - 30);
        for (const char *t : ALL) {
            float x, y, w, h;
            if (!dai_dock_panel(dock, t, &x, &y, &w, &h)) continue;
            rects.push_back(R{ x, y, w, h });
            dai_dock_panel_end(dock);
        }
        dai_dock_end(dock);
        dai_ui_end(ui);
    };
    auto no_overlaps = [&](const char *when) {
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j)
                if (overlap(rects[i], rects[j])) {
                    ++g_fail;
                    std::printf("  FAIL %s: panel %zu (%.0f,%.0f %.0fx%.0f) overlaps panel %zu "
                                "(%.0f,%.0f %.0fx%.0f)\n", when, i, rects[i].x, rects[i].y,
                                rects[i].w, rects[i].h, j, rects[j].x, rects[j].y,
                                rects[j].w, rects[j].h);
                    return;
                }
        ++g_pass;
    };

    // ---- 1. the default layout -------------------------------------------
    std::printf("default layout\n");
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    CHECK(rects.size() == 4, "expected 4 visible panels (Game is a tab behind Scene), got %zu",
          rects.size());
    no_overlaps("default layout");
    CHECK(dai_dock_visible(dock, "Scene") == 1, "Scene should be the selected tab");
    CHECK(dai_dock_visible(dock, "Game") == 0, "Game is behind Scene and must not be visible");

    // Every panel is inside the area it was given, and none is degenerate.
    for (size_t i = 0; i < rects.size(); ++i) {
        CHECK(rects[i].w > 20.0f && rects[i].h > 20.0f,
              "panel %zu came out %.0fx%.0f", i, rects[i].w, rects[i].h);
        CHECK(rects[i].x >= -0.5f && rects[i].y >= 29.5f &&
              rects[i].x + rects[i].w <= W + 0.5f && rects[i].y + rects[i].h <= H + 0.5f,
              "panel %zu left the dock area", i);
    }

    // The panels together must cover the area: a tree has no gaps either.
    float total = 0.0f;
    for (const R &r : rects) total += r.w * r.h;
    float area = W * (H - 30.0f);
    // Tab bars are part of the area and not of any body, so a little is lost.
    CHECK(total > area * 0.90f, "the panels cover only %.0f%% of the dock area",
          100.0 * (double)total / (double)area);

    // ---- 2. switching to the Game tab ------------------------------------
    std::printf("tabs\n");
    dai_dock_focus(dock, "Game");
    frame(-1, -1, 0);
    CHECK(dai_dock_visible(dock, "Game") == 1, "clicking the Game tab did not show it");
    CHECK(dai_dock_visible(dock, "Scene") == 0, "Scene is still visible behind Game");
    no_overlaps("after switching tabs");
    dai_dock_focus(dock, "Scene");
    frame(-1, -1, 0);

    // ---- 3. dragging a tab out makes a floating window --------------------
    std::printf("tear off\n");
    // Grab the Inspector's tab: its leaf's tab bar is the top of its rect.
    float ix, iy, iw, ih;
    frame(-1, -1, 0);
    CHECK(dai_dock_panel(dock, "Inspector", &ix, &iy, &iw, &ih) != 0 ||
          dai_dock_visible(dock, "Inspector"), "no inspector to grab");
    dai_dock_panel_end(dock);
    float tab_x = ix + 20.0f, tab_y = iy - 10.0f;      // the bar sits above the body
    frame(tab_x, tab_y, 1);                            // press on the tab
    frame(tab_x + 40.0f, tab_y + 200.0f, 1);           // drag well into the middle
    frame(400.0f, 400.0f, 1);
    frame(400.0f, 400.0f, 0);                          // drop
    frame(-1, -1, 0);
    no_overlaps("after dropping a tab in the middle");
    CHECK(dai_dock_visible(dock, "Inspector") == 1 || dai_dock_visible(dock, "Scene") == 1,
          "the dropped panel disappeared entirely");

    // ---- 4. the layout survives a round trip ------------------------------
    std::printf("layout text\n");
    size_t need = dai_dock_to_text(dock, nullptr, 0);
    CHECK(need > 0, "to_text produced nothing");
    std::string text(need + 1, '\0');
    dai_dock_to_text(dock, &text[0], text.size());

    dai_dock *dock2 = dai_dock_create();
    dai_dock_add(dock2, "Scene", DAI_DOCK_NONE, 0.0f);
    dai_dock_add_tab(dock2, "Game", "Scene");
    dai_dock_add(dock2, "Hierarchy", DAI_DOCK_LEFT, 0.20f);
    dai_dock_add(dock2, "Inspector", DAI_DOCK_RIGHT, 0.22f);
    dai_dock_add(dock2, "Project", DAI_DOCK_BOTTOM, 0.28f);
    CHECK(dai_dock_from_text(dock2, text.c_str()) == DAI_OK, "parsing our own layout failed");
    size_t need2 = dai_dock_to_text(dock2, nullptr, 0);
    std::string text2(need2 + 1, '\0');
    dai_dock_to_text(dock2, &text2[0], text2.size());
    CHECK(std::strcmp(text.c_str(), text2.c_str()) == 0,
          "a layout does not survive being written and read back");
    dai_dock_destroy(dock2);

    // ---- 5. closing and reopening -----------------------------------------
    std::printf("close / reopen\n");
    dai_dock_close(dock, "Project");
    frame(-1, -1, 0);
    CHECK(dai_dock_is_open(dock, "Project") == 0, "Project did not close");
    CHECK(dai_dock_visible(dock, "Project") == 0, "a closed panel is still drawn");
    no_overlaps("after closing a panel");
    for (const R &r : rects)
        CHECK(r.w > 20.0f && r.h > 20.0f, "closing left a degenerate panel %.0fx%.0f", r.w, r.h);

    // ---- 6. reset puts everything back ------------------------------------
    dai_dock_reset(dock);
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    CHECK(dai_dock_visible(dock, "Hierarchy") == 1, "reset lost the hierarchy");
    CHECK(dai_dock_visible(dock, "Inspector") == 1, "reset lost the inspector");
    CHECK(dai_dock_visible(dock, "Project") == 1, "reset did not reopen the closed panel");
    no_overlaps("after reset");

    // ---- 7. the splitter moves BOTH neighbours ----------------------------
    std::printf("splitter\n");
    frame(-1, -1, 0);
    float hx, hy, hw, hh;
    dai_dock_panel(dock, "Hierarchy", &hx, &hy, &hw, &hh);
    dai_dock_panel_end(dock);
    float sx = hx + hw + 1.0f, sy = hy + hh * 0.5f;
    float before = hw;
    frame(sx, sy, 1);
    frame(sx + 80.0f, sy, 1);
    frame(sx + 80.0f, sy, 0);
    frame(-1, -1, 0);
    dai_dock_panel(dock, "Hierarchy", &hx, &hy, &hw, &hh);
    dai_dock_panel_end(dock);
    std::printf("  hierarchy %.0f -> %.0f\n", before, hw);
    CHECK(hw > before + 40.0f, "dragging the split did not widen the hierarchy (%.0f -> %.0f)",
          before, hw);
    no_overlaps("after dragging a splitter");

    dai_dock_destroy(dock);
    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
