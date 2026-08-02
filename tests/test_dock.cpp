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

    // ---- 8. dropping on an edge really docks ------------------------------
    // A drag that ends with the button up must still know where it was
    // pointing: the target is chosen while the button is HELD and used on the
    // RELEASE frame. (This is the regression test for "dragging never docks".)
    std::printf("edge dock\n");
    dai_dock_reset(dock);
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    float cx, cy, cw, ch;
    dai_dock_panel(dock, "Scene", &cx, &cy, &cw, &ch);
    dai_dock_panel_end(dock);
    dai_dock_panel(dock, "Inspector", &ix, &iy, &iw, &ih);
    dai_dock_panel_end(dock);
    tab_x = ix + 20.0f; tab_y = iy - 10.0f;
    float drop_x = cx + 4.0f, drop_y = cy + ch * 0.5f;   // left edge of the scene
    frame(tab_x, tab_y, 1);                              // grab the Inspector tab
    frame(tab_x + 30.0f, tab_y + 60.0f, 1);              // past the drag threshold
    frame(drop_x, drop_y, 1);                            // hover the left zone
    frame(drop_x, drop_y, 1);
    frame(drop_x, drop_y, 0);                            // drop
    frame(-1, -1, 0);
    float ix2, iy2, iw2, ih2, cx2, cy2, cw2, ch2;
    CHECK(dai_dock_panel(dock, "Inspector", &ix2, &iy2, &iw2, &ih2) != 0,
          "the inspector vanished after the edge drop");
    dai_dock_panel_end(dock);
    dai_dock_panel(dock, "Scene", &cx2, &cy2, &cw2, &ch2);
    dai_dock_panel_end(dock);
    std::printf("  inspector x %.0f -> %.0f, scene x %.0f -> %.0f\n", ix, ix2, cx, cx2);
    CHECK(ix2 < ix - 100.0f,
          "the inspector did not move to the scene's left edge (x %.0f -> %.0f)", ix, ix2);
    CHECK(cx2 > ix2 + iw2 - 1.0f, "the scene did not make room for the docked inspector");
    no_overlaps("after edge docking");

    // ---- 9. dropping on a tab bar stacks tabs ------------------------------
    std::printf("tab dock\n");
    dai_dock_reset(dock);
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    dai_dock_panel(dock, "Hierarchy", &hx, &hy, &hw, &hh);
    dai_dock_panel_end(dock);
    dai_dock_panel(dock, "Inspector", &ix, &iy, &iw, &ih);
    dai_dock_panel_end(dock);
    tab_x = ix + 20.0f; tab_y = iy - 10.0f;
    float bar_x = hx + hw * 0.5f, bar_y = hy - 10.0f;    // the hierarchy's tab bar
    frame(tab_x, tab_y, 1);
    frame(tab_x - 30.0f, tab_y + 60.0f, 1);
    frame(bar_x, bar_y, 1);
    frame(bar_x, bar_y, 0);
    frame(-1, -1, 0);
    CHECK(dai_dock_visible(dock, "Inspector") == 1, "the dropped tab was not selected");
    CHECK(dai_dock_visible(dock, "Hierarchy") == 0, "the hierarchy should sit behind the dropped tab");
    float ix3, iy3, iw3, ih3;
    dai_dock_panel(dock, "Inspector", &ix3, &iy3, &iw3, &ih3);
    dai_dock_panel_end(dock);
    CHECK(std::fabs(ix3 - hx) < 2.0f && std::fabs(iw3 - hw) < 2.0f,
          "the docked tab does not fill the hierarchy's old body (%.0f,%.0f vs %.0f,%.0f)",
          ix3, iw3, hx, hw);
    no_overlaps("after tab docking");

    // ---- 10. a locked pair cannot be separated ------------------------------
    std::printf("lock pair\n");
    dai_dock_reset(dock);
    dai_dock_lock_pair(dock, "Scene", "Game");
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    {
        // Drag the Game tab onto the Hierarchy's tab bar: without the lock it
        // would stack there and the two views would live in different leaves.
        dai_dock_focus(dock, "Game");
        frame(-1, -1, 0);
        float gx, gy, gw, gh, hx2, hy2, hw2, hh2;
        dai_dock_panel(dock, "Game", &gx, &gy, &gw, &gh);
        dai_dock_panel_end(dock);
        dai_dock_panel(dock, "Hierarchy", &hx2, &hy2, &hw2, &hh2);
        dai_dock_panel_end(dock);
        // The Game tab is the second of the bar: past "Scene"'s width.
        float scene_tab = dai_ui_text_width(ui, "Scene") + 26.0f;
        float game_tab = dai_ui_text_width(ui, "Game") + 26.0f;
        float tabx = gx + scene_tab + game_tab * 0.5f, taby = gy - 10.0f;
        float barx = hx2 + hw2 * 0.5f, bary = hy2 - 10.0f;
        frame(tabx, taby, 1);
        frame(tabx - 40.0f, taby + 80.0f, 1);
        frame(barx, bary, 1);
        frame(barx, bary, 0);
        frame(-1, -1, 0);
        char dump[1024];
        dai_dock_dump(dock, dump, sizeof(dump));
        std::printf("  %s\n", dump);
        const char *g = std::strstr(dump, "\"Game\"");
        CHECK(g != nullptr, "Game vanished from the layout");
        if (g) {
            const char *open = g;
            while (open > dump && std::strncmp(open, "{ leaf", 6) != 0) --open;
            const char *close = std::strstr(g, "}");
            bool same = close && std::strstr(open, "\"Scene\"") &&
                        std::strstr(open, "\"Scene\"") < close;
            CHECK(same, "dragging Game away left Scene behind - the pair was split");
        }
        no_overlaps("after dragging a locked tab");
    }

    // ---- 11. a separated layout from disk is fused on load -------------------
    std::printf("lock pair restore\n");
    {
        dai_dock *dock3 = dai_dock_create();
        dai_dock_add(dock3, "Scene", DAI_DOCK_NONE, 0.0f);
        dai_dock_add_tab(dock3, "Game", "Scene");
        dai_dock_add(dock3, "Hierarchy", DAI_DOCK_LEFT, 0.20f);
        dai_dock_lock_pair(dock3, "Scene", "Game");
        // A layout file written before the lock existed: Scene and Game in
        // two leaves. One frame later they must share one again.
        CHECK(dai_dock_from_text(dock3,
              "dock 1\n{ h 0.5000 { leaf 0 \"Scene\" } { leaf 0 \"Game\" } }\n") == DAI_OK,
              "could not stage a separated layout");
        dai_ui_input in{};
        in.mouse_x = -1; in.mouse_y = -1;
        dai_ui_begin(ui, W, H, &in);
        dai_dock_begin(dock3, ui, 0, 30, W, H - 30);
        dai_dock_end(dock3);
        dai_ui_end(ui);
        char dump[1024];
        dai_dock_dump(dock3, dump, sizeof(dump));
        std::printf("  %s\n", dump);
        const char *g = std::strstr(dump, "\"Game\"");
        bool fused = false;
        if (g) {
            const char *open = g;
            while (open > dump && std::strncmp(open, "{ leaf", 6) != 0) --open;
            const char *close = std::strstr(g, "}");
            fused = close && std::strstr(open, "\"Scene\"") &&
                    std::strstr(open, "\"Scene\"") < close;
        }
        CHECK(fused, "a separated layout from disk was not fused back");
        dai_dock_destroy(dock3);
    }

    // ---- 12. the leaf menu: right click and the kebab button -----------------
    std::printf("leaf menu\n");
    dai_dock_reset(dock);
    frame(-1, -1, 0);
    frame(-1, -1, 0);
    auto frame_r = [&](float mx, float my, int down, int rdown) {
        rects.clear();
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down; in.right_down = rdown;
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
    float row_h = dai_font_line_height(font) + 8.0f;
    {
        // Right click the Hierarchy's tab: the menu opens on it, item 0 is
        // Close Tab.
        float hx2, hy2, hw2, hh2;
        dai_dock_panel(dock, "Hierarchy", &hx2, &hy2, &hw2, &hh2);
        dai_dock_panel_end(dock);
        float tx = hx2 + 20.0f, ty = hy2 - 10.0f;
        frame_r(tx, ty, 0, 1);                                // right press opens it
        frame_r(tx, ty, 0, 0);
        frame_r(-1, -1, 0, 0);                                // it stays open
        frame_r(tx + 12.0f, ty + 4.0f + row_h * 0.5f, 1, 0);  // item 0: Close Tab
        frame_r(-1, -1, 0, 0);
        CHECK(dai_dock_is_open(dock, "Hierarchy") == 0,
              "Close Tab in the leaf menu did not close the hierarchy");
        no_overlaps("after Close Tab from the leaf menu");

        // Reopen it: right click the Scene bar, Add Tab: Hierarchy is item 1
        // (Scene and Game are in that leaf already).
        float sx2, sy2, sw2, sh2;
        dai_dock_panel(dock, "Scene", &sx2, &sy2, &sw2, &sh2);
        dai_dock_panel_end(dock);
        float tx2 = sx2 + 20.0f, ty2 = sy2 - 10.0f;
        frame_r(tx2, ty2, 0, 1);
        frame_r(tx2, ty2, 0, 0);
        frame_r(-1, -1, 0, 0);
        frame_r(tx2 + 12.0f, ty2 + 4.0f + row_h * 1.5f, 1, 0);  // item 1: Add Tab
        frame_r(-1, -1, 0, 0);
        CHECK(dai_dock_is_open(dock, "Hierarchy") == 1,
              "Add Tab did not reopen the closed panel");
        CHECK(dai_dock_visible(dock, "Hierarchy") == 1,
              "the re-added tab was not selected");
        no_overlaps("after Add Tab from the leaf menu");
    }
    {
        // The kebab button at the bar's right end: press, release, menu.
        float px2, py2, pw2, ph2;
        dai_dock_panel(dock, "Project", &px2, &py2, &pw2, &ph2);
        dai_dock_panel_end(dock);
        float kx = px2 + pw2 - 9.0f, ky = py2 - 10.0f;
        frame_r(kx, ky, 1, 0);            // press the kebab - nothing yet
        CHECK(dai_dock_is_open(dock, "Project") == 1,
              "the kebab press alone closed something");
        frame_r(kx, ky, 0, 0);            // release: the menu opens
        frame_r(-1, -1, 0, 0);
        // The popup is clamped to the screen; the Project bar sits at the
        // right edge, so click well inside where it must have landed.
        frame_r(W - 100.0f, ky + 4.0f + row_h * 0.5f, 1, 0);  // item 0: Close Tab
        frame_r(-1, -1, 0, 0);
        CHECK(dai_dock_is_open(dock, "Project") == 0,
              "the kebab menu did not open on release (Project never closed)");
        no_overlaps("after the kebab menu");
    }

    dai_dock_destroy(dock);
    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
