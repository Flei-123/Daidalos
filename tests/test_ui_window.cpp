// Movable windows, and the reserved white texel they are drawn with.
//
// The white texel is not a detail: every solid rectangle in the interface -
// panel backgrounds, buttons, field boxes, gizmo lines - is a quad pointing at
// one texel of the font atlas. For a long time that texel was the empty border
// pixel, alpha 0, so the entire editor rendered as text floating over the
// scene with no background behind it and no button under the labels. A test
// that only counts vertices cannot see that; this one reads the atlas.

#include "dai_ui.h"
#include "dai_font.h"
#include "dai_icons.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_ui_input mouse(float x, float y, int down) {
    dai_ui_input in{};
    in.mouse_x = x; in.mouse_y = y; in.mouse_down = down;
    return in;
}

// Alpha of the vertex colour, i.e. what the rectangle will actually be worth
// once the shader multiplies it by the texel it points at.
static uint32_t alpha_of(uint32_t c) { return (c >> 24) & 0xFFu; }

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load_ui(14.0f, err, sizeof(err));
    if (!font) { std::printf("no font: %s\n", err); return 0; }   // no font, nothing to test

    // ---- 1. the atlas actually has a solid texel where it says it does ----
    std::printf("[1] Der weisse Texel\n");
    float wu = -1.0f, wv = -1.0f;
    dai_font_white_uv(font, &wu, &wv);
    uint32_t aw = 0, ah = 0;
    const uint8_t *atlas = dai_font_atlas(font, &aw, &ah);
    CHECK(wu > 0.0f && wv > 0.0f, "white uv is still (%.4f, %.4f) - the corner texel", wu, wv);
    uint32_t px = (uint32_t)(wu * (float)aw), py = (uint32_t)(wv * (float)ah);
    uint8_t cov = atlas[(size_t)py * aw + px];
    std::printf("  uv (%.4f, %.4f) -> Texel (%u, %u) = %u\n", wu, wv, px, py, cov);
    CHECK(cov == 255, "the texel the UI draws its rectangles with has coverage %u, not 255", cov);
    // And its neighbours, because bilinear filtering samples them.
    // Bilinear filtering samples the four texels around that uv. The uv sits
    // on the corner between them by construction, so all four have to be
    // solid - otherwise every rectangle comes out at 75% alpha and the panels
    // are subtly see through.
    uint8_t n00 = atlas[(size_t)(py - 1) * aw + (px - 1)];
    uint8_t n10 = atlas[(size_t)(py - 1) * aw + px];
    uint8_t n01 = atlas[(size_t)py * aw + (px - 1)];
    CHECK(n00 == 255 && n10 == 255 && n01 == 255,
          "the solid block is smaller than the filter footprint (%u, %u, %u)", n00, n10, n01);

    dai_ui *ui = dai_ui_create(font, 0);

    // ---- 2. a panel really does draw a background ------------------------
    std::printf("\n[2] Ein Panel zeichnet einen Hintergrund\n");
    {
        dai_ui_input in = mouse(-100, -100, 0);
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_panel_begin(ui, 40, 40, 200, 120, "Test");
        dai_ui_label(ui, "hello");
        dai_ui_panel_end(ui);
        dai_ui_end(ui);

        const dai_ui_draw *draws = nullptr;
        uint32_t nb = dai_ui_draws(ui, &draws);
        uint32_t solid = 0;
        for (uint32_t b = 0; b < nb; ++b)
            for (uint32_t i = 0; i < draws[b].count; ++i) {
                const dai_ui_vertex &v = draws[b].vertices[i];
                if (std::fabs(v.u - wu) < 1e-6f && std::fabs(v.v - wv) < 1e-6f &&
                    alpha_of(v.color) > 8) ++solid;
            }
        std::printf("  %u Vertices zeigen auf den weissen Texel\n", solid);
        CHECK(solid >= 6, "the panel drew no opaque background at all");
    }

    // ---- 3. dragging the title bar moves the window ----------------------
    std::printf("\n[3] Fenster verschieben\n");
    dai_ui_window win = dai_ui_window_make(100, 100, 240, 200);
    {
        dai_ui_input in = mouse(150, 108, 1);          // press on the title bar
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_window_begin(ui, "Alpha", &win);
        dai_ui_window_end(ui);
        dai_ui_end(ui);

        in = mouse(210, 168, 1);                       // drag by (60, 60)
        dai_ui_begin(ui, 800, 600, &in);
        dai_ui_window_begin(ui, "Alpha", &win);
        dai_ui_window_end(ui);
        dai_ui_end(ui);
    }
    std::printf("  Fenster steht bei (%.0f, %.0f), erwartet (160, 160)\n", win.x, win.y);
    CHECK(std::fabs(win.x - 160.0f) < 1.5f && std::fabs(win.y - 160.0f) < 1.5f,
          "the window did not follow the pointer: (%.1f, %.1f)", win.x, win.y);

    // Releasing ends the drag: further motion must not move it.
    {
        dai_ui_input in = mouse(210, 168, 0);
        dai_ui_begin(ui, 800, 600, &in); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
        in = mouse(400, 400, 0);
        dai_ui_begin(ui, 800, 600, &in); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
    }
    CHECK(std::fabs(win.x - 160.0f) < 1.5f, "the window kept moving after the button came up");

    // ---- 4. the resize grip ---------------------------------------------
    std::printf("\n[4] Groesse aendern\n");
    {
        float w0 = win.w, h0 = win.h;
        float gx = win.x + win.w - 4.0f, gy = win.y + win.h - 4.0f;
        dai_ui_input in = mouse(gx, gy, 1);
        dai_ui_begin(ui, 800, 600, &in); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
        in = mouse(gx + 50.0f, gy + 30.0f, 1);
        dai_ui_begin(ui, 800, 600, &in); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
        std::printf("  %0.f x %0.f -> %0.f x %0.f\n", w0, h0, win.w, win.h);
        CHECK(win.w > w0 + 40.0f && win.h > h0 + 20.0f, "the grip did not resize the window");
        // ...and never below the minimum.
        in = mouse(win.x - 200.0f, win.y - 200.0f, 1);
        dai_ui_begin(ui, 800, 600, &in); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
        CHECK(win.w >= win.min_w && win.h >= win.min_h,
              "the window collapsed past its minimum: %.0f x %.0f", win.w, win.h);
        dai_ui_input up = mouse(0, 0, 0);
        dai_ui_begin(ui, 800, 600, &up); dai_ui_window_begin(ui, "Alpha", &win); dai_ui_window_end(ui); dai_ui_end(ui);
    }

    // ---- 5. clicking raises, and the one on top swallows the click -------
    std::printf("\n[5] Ueberlappende Fenster\n");
    {
        dai_ui_window a = dai_ui_window_make(100, 100, 200, 200);
        dai_ui_window b = dai_ui_window_make(180, 140, 200, 200);   // overlaps a
        int a_clicks = 0, b_clicks = 0;
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            if (dai_ui_window_begin(ui, "A", &a)) { if (dai_ui_button(ui, "abtn")) a_clicks++; }
            dai_ui_window_end(ui);
            if (dai_ui_window_begin(ui, "B", &b)) { if (dai_ui_button(ui, "bbtn")) b_clicks++; }
            dai_ui_window_end(ui);
            dai_ui_end(ui);
        };
        frame(-100, -100, 0);
        CHECK(std::strcmp(dai_ui_window_front(ui), "B") == 0,
              "the last window drawn should start in front, got '%s'", dai_ui_window_front(ui));

        // Click A's title bar - to the RIGHT of the fold arrow, which occupies
        // the first title-bar-height pixels and would collapse the window
        // instead of raising it.
        frame(a.x + 80.0f, a.y + 8.0f, 1);
        frame(a.x + 80.0f, a.y + 8.0f, 0);
        std::printf("  nach dem Klick auf A ist '%s' vorne\n", dai_ui_window_front(ui));
        CHECK(std::strcmp(dai_ui_window_front(ui), "A") == 0, "clicking A did not raise it");

        // A's button now sits under B's area? No: A is in front, so a click in
        // the overlap belongs to A. Press A's button through the overlap.
        int before_a = a_clicks, before_b = b_clicks;
        // Dead centre of A's first widget: title bar height plus the padding
        // plus half a row. Guessing at "40 pixels down" hits the gap between
        // widgets as soon as the style changes.
        float bar = dai_font_line_height(font) + 6.0f;
        const dai_ui_style *st = dai_ui_style_of(ui);
        float bx = a.x + 30.0f;
        float by = a.y + bar + st->padding + (dai_font_line_height(font) + st->row_pad) * 0.5f;
        frame(bx, by, 1);
        frame(bx, by, 0);
        std::printf("  Klick im Ueberlappungsbereich: A +%d, B +%d\n",
                    a_clicks - before_a, b_clicks - before_b);
        CHECK(a_clicks == before_a + 1, "the front window did not get the click either - "
              "then the test below proves nothing");
        CHECK(b_clicks == before_b, "the window behind reacted to a click that was over the front one");

        // The other direction: a click in the part of B that A does NOT cover
        // must reach B.
        before_b = b_clicks;
        float b_only_y = b.y + b.h - 20.0f;             // below A's bottom edge
        frame(b.x + 100.0f, b_only_y, 1);
        frame(b.x + 100.0f, b_only_y, 0);
        CHECK(dai_ui_window_front(ui) && std::strcmp(dai_ui_window_front(ui), "B") == 0,
              "clicking the exposed part of B did not raise it");
    }

    // ---- 6. collapsing ---------------------------------------------------
    std::printf("\n[6] Einklappen\n");
    {
        dai_ui_window c = dai_ui_window_make(300, 300, 200, 160);
        int body_drawn = 0;
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            body_drawn = dai_ui_window_begin(ui, "Fold", &c);
            dai_ui_window_end(ui);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0);
        CHECK(body_drawn == 1, "a fresh window should draw its body");
        frame(306, 306, 1);          // the fold arrow, top left of the title bar
        frame(306, 306, 0);
        frame(-1, -1, 0);
        std::printf("  eingeklappt: %d, Koerper gezeichnet: %d\n", c.collapsed, body_drawn);
        CHECK(c.collapsed == 1, "clicking the arrow did not collapse the window");
        CHECK(body_drawn == 0, "a collapsed window still emitted its body");
    }

    // ---- 7. docking ------------------------------------------------------
    std::printf("\n[7] Andocken\n");
    {
        dai_ui_window d = dai_ui_window_make(300, 300, 200, 160);
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_dock_area(ui, 0, 30, 800, 550);      // below a toolbar
            dai_ui_window_begin(ui, "Dock", &d);
            dai_ui_window_end(ui);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0);
        CHECK(d.dock == DAI_DOCK_NONE, "a fresh window should be floating");

        // Drag the title bar to the left edge and let go there.
        frame(d.x + 80.0f, d.y + 8.0f, 1);
        frame(20.0f, 300.0f, 1);
        frame(20.0f, 300.0f, 0);          // drop
        frame(-1, -1, 0);
        std::printf("  nach dem Fallenlassen links: dock=%d slot=%d rect %.0f,%.0f %.0fx%.0f\n",
                    d.dock, d.dock_slot, d.x, d.y, d.w, d.h);
        CHECK(d.dock == DAI_DOCK_LEFT, "dropping at the left edge did not dock it");
        CHECK(std::fabs(d.x) < 1.0f, "a left docked window sits at x=%.1f", d.x);
        CHECK(std::fabs(d.y - 30.0f) < 1.0f, "it ignored the dock area's top (y=%.1f)", d.y);
        CHECK(std::fabs(d.h - 550.0f) < 1.0f, "it did not take the full height (%.1f)", d.h);
        CHECK(std::fabs(d.w - 200.0f) < 1.0f, "docking changed its width to %.1f", d.w);

        // Its width is still its own: the grip drags the split.
        float gx = d.x + d.w - 4.0f, gy = d.y + d.h - 4.0f;
        frame(gx, gy, 1);
        frame(gx + 60.0f, gy, 1);
        frame(gx + 60.0f, gy, 0);
        std::printf("  Breite nach dem Ziehen der Kante: %.0f\n", d.w);
        CHECK(d.w > 240.0f, "the resize grip does not widen a docked window (%.0f)", d.w);

        // Dropping in the top third of an edge takes half of it.
        frame(d.x + 80.0f, d.y + 8.0f, 1);
        frame(780.0f, 90.0f, 1);
        frame(780.0f, 90.0f, 0);
        frame(-1, -1, 0);
        std::printf("  rechts oben abgelegt: dock=%d slot=%d h=%.0f\n", d.dock, d.dock_slot, d.h);
        CHECK(d.dock == DAI_DOCK_RIGHT, "dropping at the right edge did not dock right");
        CHECK(d.dock_slot == 1, "dropping in the top third should remember the upper half");
        // Alone on an edge, a docked window gets ALL of it - the half is where
        // it lands once a neighbour joins, which is how Unity's dock regions
        // behave. A window alone in a half of nothing is just small.
        CHECK(std::fabs(d.h - 550.0f) < 1.0f, "alone on the edge it should be full height, is %.0f", d.h);
        CHECK(std::fabs((d.x + d.w) - 800.0f) < 1.0f, "a right docked window must touch the right edge");

        // Picking it up again releases it.
        frame(d.x + 80.0f, d.y + 8.0f, 1);
        frame(400.0f, 300.0f, 1);
        std::printf("  waehrend des Ziehens: dock=%d\n", d.dock);
        CHECK(d.dock == DAI_DOCK_NONE, "dragging a docked window did not undock it");
        frame(400.0f, 300.0f, 0);
    }

    // ---- 7b. a docked stack shares its edge -------------------------------
    std::printf("\n[7b] Andock-Stack (Nachbarn resizen mit)\n");
    {
        dai_ui_window top = dai_ui_window_docked(DAI_DOCK_LEFT, 1, 200);
        dai_ui_window bot = dai_ui_window_docked(DAI_DOCK_LEFT, 2, 200);
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_dock_area(ui, 0, 30, 800, 550);
            dai_ui_window_begin(ui, "Top", &top);
            dai_ui_window_end(ui);
            dai_ui_window_begin(ui, "Bottom", &bot);
            dai_ui_window_end(ui);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0);
        frame(-1, -1, 0);      // dock sizes settle on the second frame
        std::printf("  Top %.0f,%.0f %.0fx%.0f   Bottom %.0f,%.0f %.0fx%.0f\n",
                    top.x, top.y, top.w, top.h, bot.x, bot.y, bot.w, bot.h);
        CHECK(std::fabs(top.y - 30.0f) < 1.0f, "the first window starts at the dock top (%.0f)", top.y);
        CHECK(top.h > 260.0f && top.h < 290.0f, "two docked windows split the edge, top is %.0f", top.h);
        CHECK(std::fabs(bot.y - (top.y + top.h)) < 1.5f,
              "the second window starts where the first ends (%.0f vs %.0f)", bot.y, top.y + top.h);
        CHECK(std::fabs((bot.y + bot.h) - 580.0f) < 1.5f, "the stack does not fill the edge (%.0f)",
              bot.y + bot.h);

        // Drag the split between them down by 100 px: the top one grows, the
        // bottom one shrinks - both, not one.
        float sx = top.x + top.w * 0.5f, sy = top.y + top.h - 2.0f;
        float th0 = top.h, bh0 = bot.h;
        frame(sx, sy, 1);
        frame(sx, sy + 40.0f, 1);
        frame(sx, sy + 100.0f, 1);
        frame(sx, sy + 100.0f, 0);
        frame(-1, -1, 0);
        std::printf("  nach dem Split-Ziehen: Top h=%.0f, Bottom h=%.0f\n", top.h, bot.h);
        CHECK(top.h > th0 + 60.0f, "dragging the split down did not grow the top window (%.0f -> %.0f)",
              th0, top.h);
        CHECK(bot.h < bh0 - 60.0f, "the neighbour did not shrink (%.0f -> %.0f) - a split moves BOTH",
              bh0, bot.h);
        CHECK(std::fabs((top.h + bot.h) - (th0 + bh0)) < 4.0f,
              "the stack grew by %.0f px - docked windows share a fixed edge",
              (top.h + bot.h) - (th0 + bh0));
    }

    // ---- 8. component headers -------------------------------------------
    std::printf("\n[8] Klapp-Kopfzeilen (Inspector-Komponenten)\n");
    {
        int open = 1, enabled = 1;
        int inner_drawn = 0, hit = 0;
        float hx = 100.0f, hy = 100.0f;
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, hx, hy, 240, 300, nullptr);
            hit = dai_ui_header(ui, "Rigidbody", &open, &enabled);
            inner_drawn = 0;
            if (open) { dai_ui_label(ui, "mass"); inner_drawn = 1; }
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0);
        CHECK(inner_drawn == 1, "an open header should show its contents");

        // Click the title: folds.
        float title_x = hx + 40.0f, title_y = hy + 12.0f;
        frame(title_x, title_y, 1);
        frame(title_x, title_y, 0);
        std::printf("  nach dem Klick auf den Titel: offen=%d, Inhalt=%d\n", open, inner_drawn);
        CHECK(open == 0, "clicking the header did not fold it");
        CHECK(inner_drawn == 0, "a folded header still drew its contents");
        CHECK(enabled == 1, "folding also toggled the component off");

        // Click the checkbox on the right: switches the component off without
        // unfolding - two things in one bar, and they must not be the same
        // click.
        float box_x = hx + 240.0f - 12.0f;
        frame(box_x, title_y, 1);
        frame(box_x, title_y, 0);
        std::printf("  nach dem Klick auf die Box: offen=%d, aktiv=%d\n", open, enabled);
        CHECK(enabled == 0, "the header checkbox did not switch the component off");
        CHECK(open == 0, "the checkbox also toggled the fold state");
    }

    // ---- 9. icons -------------------------------------------------------
    std::printf("\n[9] SVG-Icons in der Oberflaeche\n");
    {
        // A texture id the font atlas cannot be confused with: the whole point
        // is that icon quads end up in their OWN batch, pointing at their own
        // atlas. Getting this wrong is how the UI once drew every glyph out of
        // the wrong texture and produced a wall of white boxes.
        const dai_texture ICON_TEX = 77;
        dai_icons *icons = dai_icons_create(16.0f);
        CHECK(icons != nullptr, "no icon set");
        dai_ui_set_icons(ui, icons, ICON_TEX);
        CHECK(dai_ui_has_icon(ui, "play") == 1, "the ui cannot see the built-in icons");
        CHECK(dai_ui_has_icon(ui, "not-an-icon") == 0, "an unknown icon reported present");

        int clicked = 0;
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 800, 34, nullptr);
            dai_ui_row(ui, 24.0f);
            clicked = dai_ui_icon_button(ui, "play", "Play", 0);
            dai_ui_icon_button(ui, "stop", "Stop", 1);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        };

        frame(-1, -1, 0);
        const dai_ui_draw *draws = nullptr;
        uint32_t nd = dai_ui_draws(ui, &draws);
        uint32_t icon_verts = 0, icon_batches = 0;
        for (uint32_t i = 0; i < nd; ++i)
            if (draws[i].texture == ICON_TEX) { ++icon_batches; icon_verts += draws[i].count; }
        std::printf("  %u Batches, davon %u mit Icon-Textur, %u Vertices\n", nd, icon_batches, icon_verts);
        CHECK(icon_batches >= 1, "no draw batch used the icon texture");
        CHECK(icon_verts == 12, "two icons should be two quads (12 vertices), got %u", icon_verts);

        // The uvs have to point inside the icon's own cell, not at the whole
        // atlas: a quad with 0..1 uvs draws all thirty icons squashed into one
        // button, which looks like noise and is easy to miss on a dark panel.
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        dai_icons_uv(icons, "play", &u0, &v0, &u1, &v1);
        CHECK(u1 > u0 && v1 > v0, "the play icon has an empty uv rectangle");
        CHECK(u1 - u0 < 0.5f && v1 - v0 < 0.5f, "one icon claims half the atlas");
        bool found = false;
        for (uint32_t i = 0; i < nd && !found; ++i) {
            if (draws[i].texture != ICON_TEX) continue;
            for (uint32_t v = 0; v < draws[i].count; ++v) {
                const dai_ui_vertex &vx = draws[i].vertices[v];
                if (std::fabs(vx.u - u0) < 1e-4f && std::fabs(vx.v - v0) < 1e-4f) { found = true; break; }
            }
        }
        CHECK(found, "no icon vertex carries the play icon's uv");

        // Clicking one.
        frame(12.0f, 12.0f, 1);
        frame(12.0f, 12.0f, 0);
        CHECK(clicked == 1, "clicking an icon button did nothing");

        // Hovering one names it. An icon only toolbar without tooltips is a
        // memory test, so the tooltip is part of the widget, not decoration.
        frame(12.0f, 12.0f, 0);
        nd = dai_ui_draws(ui, &draws);
        int top_layer_verts = 0;
        for (uint32_t i = 0; i < nd; ++i)
            if (draws[i].texture != ICON_TEX) top_layer_verts += (int)draws[i].count;
        frame(400.0f, 400.0f, 0);          // pointer away from the toolbar
        const dai_ui_draw *draws2 = nullptr;
        uint32_t nd2 = dai_ui_draws(ui, &draws2);
        int away_verts = 0;
        for (uint32_t i = 0; i < nd2; ++i)
            if (draws2[i].texture != ICON_TEX) away_verts += (int)draws2[i].count;
        std::printf("  Vertices mit Tooltip %d, ohne %d\n", top_layer_verts, away_verts);
        CHECK(top_layer_verts > away_verts, "hovering an icon button drew no tooltip");

        dai_ui_set_icons(ui, nullptr, 0);
        dai_icons_free(icons);
    }

    // ---- 10. numeric fields: typing and the axis drag ---------------------
    std::printf("\n[10] Zahlenfelder\n");
    {
        float value = 0.5f;
        float xyz[3] = { 1.0f, 2.0f, 3.0f };
        auto frame = [&](float mx, float my, int down) {
            dai_ui_input in = mouse(mx, my, down);
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
            dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
            dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0);

        // Small values must not vanish. The old display snapped anything under
        // 0.005 to "0", so every friction anyone typed read back as zero.
        value = 0.0034f;
        frame(-1, -1, 0);
        // Click the field and type. Field 1's rect: label 62 px, so the box
        // starts at x=5+62.
        float fx = 5.0f + 62.0f + 10.0f, fy = 5.0f + 10.0f;
        frame(fx, fy, 1); frame(fx, fy, 0);          // click into it
        // clear with backspace, then type 0.75
        dai_ui_input keys = mouse(fx, fy, 0);
        keys.key_backspace = 1;
        for (int i = 0; i < 6; ++i) {
            dai_ui_begin(ui, 800, 600, &keys);
            dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
            dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
            dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        }
        uint32_t digits[4] = { '0', '.', '7', '5' };
        for (int d = 0; d < 4; ++d) {
            dai_ui_input t = mouse(fx, fy, 0);
            t.text[0] = digits[d];
            dai_ui_begin(ui, 800, 600, &t);
            dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
            dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
            dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
            dai_ui_panel_end(ui);
            dai_ui_end(ui);
        }
        std::printf("  vor Enter: %.4f\n", value);
        CHECK(value == 0.0034f, "typing changed the value before Enter (%.4f)", value);
        dai_ui_input ent = mouse(fx, fy, 0);
        ent.key_enter = 1;
        dai_ui_begin(ui, 800, 600, &ent);
        dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
        dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
        dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        std::printf("  nach Enter: %.4f\n", value);
        CHECK(std::fabs(value - 0.75f) < 1e-4f, "Enter committed %.4f, not 0.75", value);

        // Letters do not belong in a number.
        dai_ui_input t = mouse(fx, fy, 0);
        frame(fx, fy, 1); frame(fx, fy, 0);
        t.text[0] = 'a'; t.text[1] = 'b';
        dai_ui_begin(ui, 800, 600, &t);
        dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
        dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
        dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        dai_ui_input ent2 = mouse(fx, fy, 0);
        ent2.key_enter = 1;
        dai_ui_begin(ui, 800, 600, &ent2);
        dai_ui_panel_begin(ui, 0, 0, 320, 400, nullptr);
        dai_ui_num_field(ui, "Friction", &value, 0.005f, 0.0f, 10.0f, "fric");
        dai_ui_num_vec3(ui, "Position", xyz, 0.02f);
        dai_ui_panel_end(ui);
        dai_ui_end(ui);
        CHECK(std::fabs(value - 0.75f) < 1e-4f, "letters got into the number (%.4f)", value);

        // The axis letter drags. The vec3 row is one widget below the field:
        // h = line_height + row_pad, so aim one row down, at the X letter.
        float row_h = 13.0f + 4.0f + 3.0f;
        float ax = 5.0f + 62.0f + 4.0f;    // the X of the first field
        float ay = 5.0f + row_h + 10.0f;
        float before_x = xyz[0];
        frame(ax, ay, 1);
        for (int i = 1; i <= 10; ++i) frame(ax + (float)i * 3.0f, ay, 1);
        frame(ax + 30.0f, ay, 0);
        std::printf("  Achsen-Drag: X %.3f -> %.3f\n", before_x, xyz[0]);
        CHECK(xyz[0] > before_x, "dragging the X letter did not change X (%.3f)", xyz[0]);
        CHECK(std::fabs(xyz[1] - 2.0f) < 1e-4f, "the X drag touched Y too");
    }

    // ---- 11. the popup menu ----------------------------------------------
    std::printf("\n[11] Aufklapp-Menue\n");
    {
        dai_ui_popup menu{};
        static const dai_ui_menu_item ITEMS[] = {
            { nullptr, "Rename", "F2" },
            { nullptr, "Duplicate", "Ctrl+D" },
            { nullptr, "Delete", "Del" },
        };
        int pick = -99;
        int dead_clicks = 0;
        auto frame = [&](float mx, float my, int down, int rdown) {
            dai_ui_input in = mouse(mx, my, down);
            in.right_down = rdown;
            dai_ui_begin(ui, 800, 600, &in);
            dai_ui_panel_begin(ui, 0, 0, 800, 600, nullptr);
            if (dai_ui_button(ui, "under the menu")) ++dead_clicks;
            dai_ui_panel_end(ui);
            pick = dai_ui_popup_menu(ui, &menu, ITEMS, 3);
            dai_ui_end(ui);
        };
        frame(-1, -1, 0, 0);
        CHECK(pick == -2 || pick == -99, "a menu that was never opened is reporting clicks");

        dai_ui_popup_open(&menu, 100.0f, 100.0f);
        frame(-1, -1, 0, 0);
        CHECK(menu.open == 1, "the menu closed itself");

        // Click the second entry. The row height is the FONT's line height
        // plus 8, plus a 4 px pad at the top - asking the font instead of
        // guessing 13 is the difference between a test and a superstition.
        float row_h = dai_font_line_height(font) + 8.0f;
        float entry_y = 100.0f + 4.0f + row_h * 1.5f;
        frame(140.0f, entry_y, 1, 0);
        // Immediate mode means the answer is THIS frame's return value - the
        // release frame after it returns "nothing happened" again.
        CHECK(pick == 1, "the second entry did not answer on the press frame (pick=%d)", pick);
        frame(140.0f, entry_y, 0, 0);
        CHECK(menu.open == 0, "choosing an entry left the menu open");

        // Reopen and dismiss by clicking elsewhere: the click must NOT reach
        // the button underneath.
        dai_ui_popup_open(&menu, 100.0f, 100.0f);
        frame(-1, -1, 0, 0);
        frame(10.0f, 10.0f, 1, 0);        // over the "under the menu" button
        frame(10.0f, 10.0f, 0, 0);
        CHECK(menu.open == 0, "clicking away did not close the menu");
        CHECK(dead_clicks == 0, "the dismiss click pressed the button under the menu");
    }

    dai_ui_destroy(ui);
    dai_font_free(font);
    std::printf("\n==================================\n");
    std::printf("  %d bestanden, %d fehlgeschlagen\n", g_pass, g_fail);
    std::printf("==================================\n");
    return g_fail == 0 ? 0 : 1;
}
