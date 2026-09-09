// Editor panels: hierarchy, inspector, toolbar, viewport routing.
//
//   ./build/test_editor_ui
//
// No renderer needed - the UI produces triangles, and that is what is checked.

#include "dai_editor_ui.h"
#include "dai_show_ui.h"
#include "dai_render.h"
#include "dai_ext.h"
// The blockout host, included the way a real host includes it: section 12
// builds the mesh a box's fields stand for and checks undo on its digest.
#include "dai_blockout_host.inl"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
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

// Every vertex the UI emitted, so clipping can be checked directly.
static void vert_bounds(dai_ui *ui, float *x0, float *y0, float *x1, float *y1) {
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

// The modifier stack on the node: the scene lines, undo, the property names
// and the list the inspector draws. Called from the end of main().
#include "modifier_stack_cases.hpp"

int main() {
    char err[256] = { 0 };
    dai_font *font = dai_font_load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18.0f,
                                   nullptr, 0, err, sizeof(err));
    CHECK(font != nullptr, "font load failed: %s", err);
    if (!font) return 1;
    dai_ui *ui = dai_ui_create(font, 0);

    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1; cfg.seed = 4;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);
    dai_editor_ui *panels = dai_editor_ui_create(ed, ui);
    CHECK(panels != nullptr, "panel creation failed");

    dai_editor_camera(ed, dai_vec3{ 0, 3, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      55.0f, 0.1f, 200.0f, 1280.0f, 720.0f);

    // a parent with two children, plus a loose node
    dai_node_desc r = dai_node_desc_default();
    r.motion = DAI_KINEMATIC;
    std::snprintf(r.name, sizeof(r.name), "Parent");
    dai_node parent = dai_doc_add(doc, &r);
    std::snprintf(r.name, sizeof(r.name), "ChildA");
    r.parent = parent; r.position = { 1, 0, 0 };
    dai_node childA = dai_doc_add(doc, &r);
    std::snprintf(r.name, sizeof(r.name), "ChildB");
    r.position = { -1, 0, 0 };
    dai_doc_add(doc, &r);
    r.parent = 0; r.position = { 0, 0, 4 };
    std::snprintf(r.name, sizeof(r.name), "Loose");
    dai_doc_add(doc, &r);
    dai_doc_sync_apply(sync);
    dai_step(w);

    // The panels are placed by hand rather than through dai_editor_ui_frame,
    // because the default layout is now made of windows the USER moves - a
    // test that clicks at fixed pixels would be asserting where someone
    // dragged the inspector to. dai_editor_ui_frame gets its own check below.
    const float PANEL_X = 8.0f, PANEL_Y = 60.0f, PANEL_W = 240.0f;
    const float INSPECTOR_X = 1280.0f - PANEL_W - 8.0f;
    auto frame = [&](float mx, float my, int down, float wheel = 0.0f) {
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down; in.wheel = wheel;
        dai_ui_begin(ui, 1280, 720, &in);
        // The inspector groups its fields into collapsible components now, and
        // this test walks down the panel clicking every few pixels - which
        // folds the very block it is hunting for. Fold state is not what these
        // checks are about; dai_ui_header has its own test.
        dai_editor_ui_expand_all(panels);
        dai_editor_ui_toolbar(panels, 0.0f, 0.0f, 1280.0f);
        dai_editor_ui_hierarchy(panels, PANEL_X, PANEL_Y, PANEL_W, 360.0f);
        dai_editor_ui_inspector(panels, INSPECTOR_X, PANEL_Y, PANEL_W, 592.0f);
        dai_editor_ui_gizmo(panels);
        dai_ui_end(ui);
    };

    // Widget geometry from the style, not from memory: the editor got compact
    // (5 px padding instead of 8, tighter rows), and every hard coded offset in
    // here silently started pointing at the gap between two fields.
    const dai_ui_style *style = dai_ui_style_of(ui);
    const float LH   = dai_font_line_height(font);
    const float PAD  = style->padding;
    const float SPC  = style->spacing;
    const float ROW_H = LH + 2.0f;                       // dai_ui_tree_item
    const float FIRST_ROW_Y = PANEL_Y + PAD + LH + SPC;  // below the panel title

    // ---- 1. the panels draw something -------------------------------------
    std::printf("panels\n");
    frame(640, 400, 0);
    CHECK(total_verts(ui) > 500, "the editor UI made only %u vertices", total_verts(ui));
    CHECK(total_verts(ui) % 3 == 0, "vertex count is not a multiple of 3");

    // ---- 2. the hierarchy lists everything, folding hides a subtree --------
    frame(640, 400, 0);
    uint32_t rows_open = dai_editor_ui_visible_rows(panels);
    CHECK(rows_open == 4, "hierarchy shows %u rows, expected 4", rows_open);

    // Click the fold arrow of the parent row. The tree no longer starts one
    // row under the panel title - there is a search box above it now, and
    // whatever goes in there next moves the rows again - so the arrow is
    // FOUND rather than computed: walk down the arrow column until a click
    // folds the only foldable row in the fixture, which is the parent. The
    // assertions below are unchanged; only the way the row is located is.
    float row_x = PANEL_X + PAD + 6.0f;              // panel x + arrow column
    float row_y = -1.0f;
    for (float ty = PANEL_Y + PAD; ty < PANEL_Y + 8.0f * ROW_H && row_y < 0.0f; ty += 2.0f) {
        frame(row_x, ty, 0);
        frame(row_x, ty, 1);
        frame(row_x, ty, 0);
        if (dai_editor_ui_visible_rows(panels) == 2) row_y = ty;
    }
    CHECK(row_y >= 0.0f, "no row in the hierarchy's arrow column folds the parent");
    if (row_y < 0.0f) row_y = FIRST_ROW_Y + ROW_H * 0.5f;
    // Back open, nothing selected, and then the click the check is about - so
    // "the fold did not select anything" is a statement about that click and
    // not about the search above it.
    frame(row_x, row_y, 1);
    frame(row_x, row_y, 0);
    dai_editor_deselect_all(ed);
    frame(row_x, row_y, 0);
    frame(row_x, row_y, 1);
    frame(row_x, row_y, 0);
    uint32_t rows_folded = dai_editor_ui_visible_rows(panels);
    CHECK(rows_folded == 2, "folding the parent left %u rows, expected 2", rows_folded);
    CHECK(dai_editor_selection_count(ed) == 0,
          "clicking the fold arrow also changed the selection");

    // unfold again
    frame(row_x, row_y, 1);
    frame(row_x, row_y, 0);
    CHECK(dai_editor_ui_visible_rows(panels) == 4, "unfolding did not restore the rows");

    // ---- 3. clicking a row selects it -------------------------------------
    // Selecting is a FRAME level gesture: the hierarchy records the press, and
    // dai_editor_ui_frame turns it into a selection when the button comes up
    // without a drag - that difference is what makes a row draggable at all.
    // So this one goes through the real frame function, and finds the row by
    // walking down the hierarchy column of the built in layout.
    auto full_frame = [&](float mx, float my, int down) {
        dai_ui_input in{};
        in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down;
        dai_ui_begin(ui, 1280, 720, &in);
        dai_editor_ui_frame(panels, 1280, 720);
        dai_ui_end(ui);
    };
    dai_editor_deselect_all(ed);
    full_frame(-100, -100, 0);
    full_frame(-100, -100, 0);
    {
        float lvx = 0, lvy = 0, lvw = 0, lvh = 0;
        dai_editor_ui_viewport_rect(panels, &lvx, &lvy, &lvw, &lvh);
        float label_x = lvx * 0.45f;                 // inside the hierarchy, past its arrows
        int picked = 0;
        for (float ty = lvy; ty < lvy + 12.0f * ROW_H && !picked; ty += 3.0f) {
            full_frame(label_x, ty, 0);
            full_frame(label_x, ty, 1);
            full_frame(label_x, ty, 0);
            full_frame(label_x, ty, 0);
            if (dai_editor_selection_count(ed) == 1 && dai_editor_selected(ed, 0) == parent)
                picked = 1;
        }
        CHECK(dai_editor_selection_count(ed) == 1, "clicking a row did not select it");
        CHECK(dai_editor_selected(ed, 0) == parent, "the wrong row got selected");
    }

    // ---- 4. the inspector edits the document ------------------------------
    std::printf("inspector\n");
    dai_node_desc before{};
    dai_doc_get(doc, parent, &before);

    // Find the Position row by trying each row rather than hard coding a pixel
    // offset: the test would then only be checking my arithmetic, and it would
    // break every time a field is added above it.
    // WHERE the drag handle of a number sits is not a constant any more: the
    // label column measures the widest name it has to show, so "Position" and
    // "Size (m)" do not start their fields at the same x, and neither of them
    // starts at style->label_w. A test that hard codes that offset is testing
    // its own arithmetic - it presses in the middle of a name and reports that
    // dragging does nothing.
    //
    // So the handle is FOUND, in two cheap steps: hover the panel once per
    // point and keep the places where the UI asks for the horizontal drag
    // cursor (that is what a scrub handle is), then press and drag on those
    // few candidates and keep the one that actually moves position.x.
    auto hover_cursor = [&](float x, float y) {
        frame(x, y, 0);
        return dai_ui_cursor(ui);
    };
    auto scrub_moves = [&](dai_node node, float x, float y, float *dx_out) {
        dai_node_desc a{}, b2{};
        dai_doc_get(doc, node, &a);
        uint32_t depth_before = dai_editor_undo_depth(ed);
        frame(x, y, 0);
        frame(x, y, 1);
        for (int i = 1; i <= 4; ++i) frame(x + (float)i * 3.0f, y, 1);
        frame(x + 12.0f, y, 0);
        dai_doc_get(doc, node, &b2);
        float dx = b2.position.x - a.position.x;
        while (dai_editor_undo_depth(ed) > depth_before) dai_editor_undo(ed);
        if (dx_out) *dx_out = dx;
        return std::fabs(dx) > 1e-6f;
    };
    const float PANEL_R = INSPECTOR_X + PANEL_W - 6.0f;
    float FIELD_X = INSPECTOR_X + PAD + style->label_w + 8.0f;   // a starting guess
    float pos_y = -1.0f;
    int rows_that_moved_x = 0;
    for (float y = 90.0f; y < 400.0f && pos_y < 0.0f; y += 4.0f) {
        for (float x = INSPECTOR_X + PAD; x < PANEL_R && pos_y < 0.0f; x += 3.0f) {
            if (hover_cursor(x, y) != DAI_CURSOR_SIZE_WE) continue;
            float dx = 0.0f;
            if (scrub_moves(parent, x, y, &dx)) { pos_y = y; FIELD_X = x; }
        }
    }
    // How many DIFFERENT rows answer at that x - the overlap check below is
    // about rows, so the sweep that counts them keeps the x it found.
    if (pos_y > 0.0f) {
        for (float y = 90.0f; y < 400.0f; y += 4.0f) {
            float dx = 0.0f;
            if (scrub_moves(parent, FIELD_X, y, &dx)) ++rows_that_moved_x;
        }
    }
    std::printf("  X-Griff der Position-Zeile bei x=%.0f y=%.0f (%d Zeilen antworten)\n",
                (double)FIELD_X, (double)pos_y, rows_that_moved_x);
    CHECK(pos_y > 0.0f, "no row in the inspector edits the X position");
    CHECK(rows_that_moved_x <= 8, "%d different rows changed position.x - fields overlap",
          rows_that_moved_x);

    dai_doc_get(doc, parent, &before);
    uint32_t undo_before = dai_editor_undo_depth(ed);
    frame(FIELD_X, pos_y, 0);
    frame(FIELD_X, pos_y, 1);            // press
    for (int i = 1; i <= 10; ++i) frame(FIELD_X + (float)i * 3.0f, pos_y, 1);
    frame(FIELD_X + 30.0f, pos_y, 0);    // release

    dai_node_desc after{};
    dai_doc_get(doc, parent, &after);
    CHECK(after.position.x > before.position.x,
          "dragging the X field right did not move it right (%.3f -> %.3f)",
          before.position.x, after.position.x);
    CHECK(std::fabs(after.position.y - before.position.y) < 1e-6f, "dragging X also moved Y");
    CHECK(dai_editor_undo_depth(ed) == undo_before + 1,
          "a field drag over 11 frames made %u undo steps, expected 1",
          dai_editor_undo_depth(ed) - undo_before);
    CHECK(dai_editor_undo(ed) == 1, "undo after a field drag failed");
    dai_doc_get(doc, parent, &after);
    CHECK(std::fabs(after.position.x - before.position.x) < 1e-6f,
          "undo did not restore the field value");

    // the name field types into the document
    CHECK(dai_doc_count(doc) == 4, "the inspector probing changed the node count");

    // ...and the LIVE SCENE has to move with it, not just the document. The
    // gizmo reads the document, so a missing resync looks like "the gizmo
    // moves and the object stays" - which is exactly what it did.
    {
        dai_entity ent = dai_doc_sync_entity(sync, parent);
        dai_body body = dai_scene_body(sc, ent);
        dai_transform t0{};
        dai_body_get(w, body, &t0);
        dai_node_desc d0{};
        dai_doc_get(doc, parent, &d0);

        frame(FIELD_X, pos_y, 0);
        frame(FIELD_X, pos_y, 1);
        for (int i = 1; i <= 10; ++i) frame(FIELD_X + (float)i * 3.0f, pos_y, 1);
        frame(FIELD_X + 30.0f, pos_y, 0);

        dai_node_desc d1{};
        dai_doc_get(doc, parent, &d1);
        dai_transform t1{};
        dai_body_get(w, body, &t1);
        std::printf("  Dokument x %.3f -> %.3f, Szene x %.3f -> %.3f\n",
                    d0.position.x, d1.position.x, t0.position.x, t1.position.x);
        CHECK(d1.position.x > d0.position.x, "the inspector did not change the document");
        CHECK(std::fabs(t1.position.x - d1.position.x) < 1e-3f,
              "the body is at x=%.3f while the document says %.3f - the inspector "
              "moves the gizmo and nothing else", t1.position.x, d1.position.x);
        // Exactly one step back - the drag was one transaction. Undoing
        // everything would also undo the nodes this test is built from, and the
        // failures then show up three sections later.
        CHECK(dai_editor_undo(ed) == 1, "undo after the inspector drag failed");
        dai_editor_resync(ed);
    }

    // ---- 5. the toolbar switches gizmo modes ------------------------------
    std::printf("toolbar\n");
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);
    // buttons sit in a row at the top; find Rotate by walking the row
    float bx = PAD + 4.0f, by = PAD + (LH + style->row_pad) * 0.5f;
    int switched = 0;
    for (int i = 0; i < 40 && !switched; ++i) {
        float x = bx + (float)i * 12.0f;
        frame(x, by, 1);
        frame(x, by, 0);
        if (dai_editor_gizmo_mode_get(ed) == DAI_GIZMO_ROTATE) switched = 1;
        if (dai_editor_gizmo_mode_get(ed) == DAI_GIZMO_SCALE) break;
    }
    CHECK(switched, "no toolbar button switched the gizmo to rotate");
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

    // ---- 6. viewport routing ----------------------------------------------
    std::printf("viewport\n");
    dai_editor_deselect_all(ed);
    // A click over a panel must not reach the scene.
    frame(60, 300, 0);
    int consumed = dai_editor_ui_viewport_input(panels, 60, 300, 1);
    CHECK(consumed == 0, "a click over the hierarchy panel reached the viewport");
    dai_editor_ui_viewport_input(panels, 60, 300, 0);

    // A click in the middle of the screen hits the parent cube.
    frame(640, 360, 0);
    dai_editor_ui_viewport_input(panels, 640, 360, 1);
    dai_editor_ui_viewport_input(panels, 640, 360, 0);
    CHECK(dai_editor_selection_count(ed) == 1, "a viewport click selected nothing");

    // Grabbing a gizmo arm starts a drag rather than reselecting.
    dai_vec3 c = dai_editor_selection_center(ed);
    float len = dai_editor_gizmo_scale(ed);
    float ax, ay;
    dai_editor_project(ed, dai_vec3{ c.x + len * 0.7f, c.y, c.z }, &ax, &ay);
    frame(ax, ay, 0);
    dai_editor_ui_viewport_input(panels, ax, ay, 1);
    CHECK(dai_editor_dragging(ed), "clicking the gizmo arm did not start a drag");
    dai_editor_ui_viewport_input(panels, ax + 40.0f, ay, 1);
    dai_editor_ui_viewport_input(panels, ax + 40.0f, ay, 0);
    CHECK(!dai_editor_dragging(ed), "the drag did not end on release");

    // Clicking empty space clears the selection - empty space INSIDE the scene
    // view, because a press on a tab bar or a splitter is not a scene click at
    // all and deliberately keeps the selection.
    {
        float evx = 0, evy = 0, evw = 1280.0f, evh = 720.0f;
        dai_editor_ui_viewport_rect(panels, &evx, &evy, &evw, &evh);
        float ex = evx + 12.0f, ey = evy + evh - 12.0f;    // bottom left of the view
        frame(ex, ey, 0);
        dai_editor_ui_viewport_input(panels, ex, ey, 1);
        dai_editor_ui_viewport_input(panels, ex, ey, 0);
        CHECK(dai_editor_selection_count(ed) == 0,
              "clicking empty space at %.0f,%.0f did not clear the selection", (double)ex, (double)ey);
    }

    // ---- asset browser -----------------------------------------------------
    // The panel does not know where its list comes from - the host fills it,
    // same rule the resolver follows. So what is checked is: an empty browser
    // is honest, a filled one draws more, the selection is real state, and
    // shrinking the list cannot leave the selection pointing past the end.
    std::printf("asset browser\n");
    {
        const char *paths[3] = { "models/crate.glb", "models/scene.glb", "props/lamp.gltf" };
        const char *picked = nullptr;
        int as_tree = -1;

        dai_ui_input in{};
        in.mouse_x = 640; in.mouse_y = 40;          // far from the panel
        dai_ui_begin(ui, 1280, 720, &in);
        int hit = dai_editor_ui_assets(panels, 8, 380, 240, 300, &picked, &as_tree);
        dai_ui_end(ui);
        uint32_t empty_verts = total_verts(ui);
        CHECK(hit == 0 && picked == nullptr, "an empty browser reported a pick");
        CHECK(dai_editor_ui_asset_selected(panels) == -1, "an empty browser has a selection");
        CHECK(empty_verts > 0, "an empty browser drew nothing at all");

        dai_editor_ui_asset_list(panels, paths, 3);
        dai_ui_begin(ui, 1280, 720, &in);
        dai_editor_ui_assets(panels, 8, 380, 240, 300, &picked, &as_tree);
        dai_ui_end(ui);
        CHECK(total_verts(ui) > empty_verts,
              "a browser with 3 files drew %u vertices, an empty one drew %u",
              total_verts(ui), empty_verts);
        CHECK(hit == 0, "just drawing the list reported a pick");

        // A list longer than the panel must say so rather than run off the edge.
        const char *many[12];
        for (int i = 0; i < 12; ++i) many[i] = paths[i % 3];
        dai_editor_ui_asset_list(panels, many, 12);
        dai_ui_begin(ui, 1280, 720, &in);
        dai_editor_ui_assets(panels, 8, 380, 240, 140, &picked, &as_tree);   // short panel
        dai_ui_end(ui);
        CHECK(total_verts(ui) > 0, "a short panel drew nothing");

        // Shrinking the list must not leave a stale index behind.
        dai_editor_ui_asset_list(panels, paths, 3);
        CHECK(dai_editor_ui_asset_selected(panels) < 3,
              "the selection points past the end of the list");
        dai_editor_ui_asset_list(panels, nullptr, 0);
        CHECK(dai_editor_ui_asset_selected(panels) == -1, "clearing the list kept a selection");
        std::printf("  empty %u verts, 3 files %u verts\n", empty_verts, total_verts(ui));
    }

    // ---- 7. scrolling clips ------------------------------------------------
    std::printf("scrolling\n");
    // fill the hierarchy well past the panel height
    for (int i = 0; i < 60; ++i) {
        dai_node_desc n = dai_node_desc_default();
        std::snprintf(n.name, sizeof(n.name), "filler%d", i);
        dai_doc_add(doc, &n);
    }
    frame(120, 300, 0);
    float x0, y0, x1, y1;
    vert_bounds(ui, &x0, &y0, &x1, &y1);
    CHECK(y1 <= 720.0f + 1.0f, "the hierarchy drew down to y=%.1f, past the window", y1);
    // the scroll region ends well above the bottom of the screen; nothing from
    // inside it may be drawn below the panel
    frame(120, 300, 0, -5.0f);           // wheel down
    frame(120, 300, 0);
    uint32_t rows_after_scroll = dai_editor_ui_visible_rows(panels);
    CHECK(rows_after_scroll == 64, "hierarchy lost rows while scrolling (%u)", rows_after_scroll);
    vert_bounds(ui, &x0, &y0, &x1, &y1);
    CHECK(y0 >= -1.0f, "scrolling drew above the window (y=%.1f)", y0);

    // ---- 7b. the built in window layout draws and reports a viewport -------
    {
        dai_ui_input in{};
        in.mouse_x = 640; in.mouse_y = 400;
        dai_ui_begin(ui, 1280, 720, &in);
        dai_editor_ui_frame(panels, 1280, 720);
        dai_ui_end(ui);
        CHECK(total_verts(ui) > 500, "the window layout drew almost nothing");
        float vx = 0, vy = 0, vw = 0, vh = 0;
        dai_editor_ui_viewport_rect(panels, &vx, &vy, &vw, &vh);
        std::printf("  viewport %.0f,%.0f %.0fx%.0f\n", vx, vy, vw, vh);
        CHECK(vw > 400.0f && vh > 300.0f, "the layout left no room for the scene: %.0fx%.0f", vw, vh);
        CHECK(vx > 100.0f, "the hierarchy window does not cut into the viewport (x=%.0f)", vx);
        CHECK(vx + vw < 1180.0f, "the inspector window does not cut into the viewport");
        // Moving a window moves the hole in the layout with it.
        float before_x = vx;
        dai_editor_ui_layout_reset(panels, 1280, 720);
        dai_ui_begin(ui, 1280, 720, &in);
        dai_editor_ui_frame(panels, 1280, 720);
        dai_ui_end(ui);
        dai_editor_ui_viewport_rect(panels, &vx, &vy, &vw, &vh);
        CHECK(std::fabs(vx - before_x) < 1.0f, "a layout reset changed the viewport");
    }

    // ---- 8. play mode is reachable from the toolbar ------------------------
    CHECK(dai_editor_state_get(ed) == DAI_EDITOR_EDIT, "not in edit mode");
    dai_editor_play(ed);
    frame(640, 400, 0);
    CHECK(total_verts(ui) > 500, "the timeline frame produced nothing");
    dai_editor_stop(ed);

    // ---- 9. the collider is not the mesh ----------------------------------
    //
    // The whole point of this session: resizing a Box Collider resizes what
    // the object can hit, NOT the model. Before, the render scale was derived
    // from the collision half extent, so the two could never disagree - which
    // is not a simplification, it is a missing feature: "the crate looks 2 m
    // wide and collides as 1.9 m" is a thing every game needs.
    std::printf("collider vs mesh\n");
    {
        dai_node_desc c = dai_node_desc_default();
        std::snprintf(c.name, sizeof(c.name), "Crate");
        c.motion = DAI_STATIC;
        c.position = { 0, 0, -20 };
        c.half_extent = { 0.5f, 0.5f, 0.5f };
        dai_node crate = dai_doc_add(doc, &c);
        dai_doc_sync_apply(sync);
        dai_step(w);

        auto instance_scale = [&](dai_node node, dai_vec3 *out) {
            dai_entity ent = dai_doc_sync_entity(sync, node);
            dai_body body = dai_scene_body(sc, ent);
            std::vector<dai_render_instance> inst(256);
            uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 1.0f);
            dai_transform t{};
            if (dai_body_get(w, body, &t) != DAI_OK) return false;
            for (uint32_t i = 0; i < n; ++i) {
                dai_vec3 d{ inst[i].position.x - t.position.x, inst[i].position.y - t.position.y,
                            inst[i].position.z - t.position.z };
                if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) < 1e-3f) {
                    *out = inst[i].scale;
                    return true;
                }
            }
            return false;
        };

        dai_vec3 mesh0{};
        CHECK(instance_scale(crate, &mesh0), "the crate has no render instance");

        // Grow the collider by hand, the way the inspector does it.
        dai_node_desc big{};
        dai_doc_get(doc, crate, &big);
        big.render_extent = big.half_extent;         // pin the mesh, as the inspector does
        big.half_extent = { 1.5f, 1.5f, 1.5f };
        dai_doc_set(doc, crate, &big);
        dai_editor_resync(ed);
        dai_step(w);

        dai_vec3 mesh1{};
        CHECK(instance_scale(crate, &mesh1), "the crate lost its render instance");
        CHECK(std::fabs(mesh1.x - mesh0.x) < 1e-4f && std::fabs(mesh1.y - mesh0.y) < 1e-4f,
              "growing the collider from 0.5 to 1.5 changed the drawn mesh from %.3f to %.3f",
              mesh0.x, mesh1.x);

        // ...and the collision shape really did grow: a point 1 m out is
        // inside the new box and was outside the old one.
        dai_node_desc chk{};
        dai_doc_get(doc, crate, &chk);
        CHECK(std::fabs(chk.half_extent.x - 1.5f) < 1e-4f, "the collider did not grow");

        // The mesh can be resized on its own, and the collider does not follow.
        big.render_extent = { 3.0f, 3.0f, 3.0f };
        big.mesh = DAI_MESH_BOX;
        dai_doc_set(doc, crate, &big);
        dai_editor_resync(ed);
        dai_step(w);
        dai_vec3 mesh2{};
        CHECK(instance_scale(crate, &mesh2), "the crate lost its render instance again");
        CHECK(mesh2.x > mesh1.x * 1.5f, "the mesh size field did not resize the mesh (%.3f -> %.3f)",
              mesh1.x, mesh2.x);
        dai_doc_get(doc, crate, &chk);
        CHECK(std::fabs(chk.half_extent.x - 1.5f) < 1e-4f,
              "resizing the mesh moved the collider too");

        // A collider centre offsets the collision box, not the model: the body
        // moves, and the mesh is drawn back where the object is.
        dai_vec3 mesh_pos_before{}, mesh_pos_after{};
        auto instance_pos = [&](dai_node node, dai_vec3 *out) {
            std::vector<dai_render_instance> inst(256);
            uint32_t n = dai_scene_instances(sc, inst.data(), (uint32_t)inst.size(), 1.0f);
            dai_entity ent = dai_doc_sync_entity(sync, node);
            dai_body body = dai_scene_body(sc, ent);
            dai_transform t{};
            if (dai_body_get(w, body, &t) != DAI_OK) return false;
            for (uint32_t i = 0; i < n; ++i) {
                if (std::fabs(inst[i].position.z - (-20.0f)) < 3.0f) { *out = inst[i].position; return true; }
            }
            (void)n;
            return false;
        };
        CHECK(instance_pos(crate, &mesh_pos_before), "no instance to measure");
        big.collider_center = { 2.0f, 0, 0 };
        dai_doc_set(doc, crate, &big);
        dai_editor_resync(ed);
        dai_step(w);
        CHECK(instance_pos(crate, &mesh_pos_after), "no instance after the centre offset");
        CHECK(std::fabs(mesh_pos_after.x - mesh_pos_before.x) < 1e-3f,
              "a collider centre offset moved the MODEL by %.3f",
              mesh_pos_after.x - mesh_pos_before.x);
        dai_transform bt{};
        dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, crate)), &bt);
        CHECK(std::fabs(bt.position.x - 2.0f) < 1e-3f,
              "the collider centre did not move the collision body (x=%.3f)", bt.position.x);

        // The green wireframe is drawn for the selection, and only in the
        // scene view.
        dai_editor_select(ed, crate, 0);
        dai_ui_input gin{};
        dai_ui_begin(ui, 1280, 720, &gin);
        dai_editor_ui_colliders(panels);
        dai_ui_end(ui);
        uint32_t wire = total_verts(ui);
        CHECK(wire > 0, "no collider wireframe was drawn for the selection");
        dai_editor_ui_view_set(panels, DAI_VIEW_GAME);
        dai_ui_begin(ui, 1280, 720, &gin);
        dai_editor_ui_colliders(panels);
        dai_ui_end(ui);
        CHECK(total_verts(ui) == 0, "the game view drew editor wireframes");
        dai_editor_ui_view_set(panels, DAI_VIEW_SCENE);

        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, crate);
        dai_doc_sync_apply(sync);
    }

    // ---- 10. the game view needs a camera, and can get one ----------------
    {
        dai_vec3 eye{}, look{};
        float fov = 0.0f;
        CHECK(dai_editor_ui_game_camera(panels, &eye, &look, &fov) == 0,
              "a scene with no camera reported one");
        dai_node cam = dai_editor_ui_add_camera(panels);
        CHECK(cam != DAI_INVALID_NODE, "adding a camera failed");
        CHECK(dai_editor_ui_game_camera(panels, &eye, &look, &fov) == 1,
              "the camera node is not found by the game view");
        CHECK(fov > 10.0f && fov < 120.0f, "the game camera has a nonsense field of view (%.1f)", fov);
        float dx = look.x - eye.x, dy = look.y - eye.y, dz = look.z - eye.z;
        CHECK(std::fabs(std::sqrt(dx*dx + dy*dy + dz*dz) - 1.0f) < 1e-3f,
              "the game camera's look point is not one unit ahead of it");
        dai_doc_remove(doc, cam);
        dai_doc_sync_apply(sync);
    }

    // ---- 11. the gizmo stands ON the selected object ----------------------
    // The world is not drawn over the whole frame: the renderer puts it in the
    // Scene panel's body rect, with THAT rectangle's aspect. The gizmo is UI,
    // drawn in surface pixels, so it only lands on the object if the editor's
    // camera knows the same rectangle. A host that forgets
    // dai_editor_camera_viewport_rect gets a gizmo a hundred pixels down and
    // to the right of the body it is supposed to move - which is exactly what
    // .gauntlet-shots/09-game-mode-editor.png showed before this check existed.
    //
    // The reference position below is computed the way src/rhi_vulkan_frame.cpp
    // does it - look-at, perspective with the RECT's aspect, then the viewport
    // transform onto the rect - and not with dai_editor_project, or the check
    // would only prove the editor agrees with itself.
    {
        dai_node_desc bd = dai_node_desc_default();
        std::snprintf(bd.name, sizeof(bd.name), "GizmoTarget");
        bd.motion = DAI_KINEMATIC;
        bd.position = { 1.6f, 0.9f, -0.7f };
        dai_node target = dai_doc_add(doc, &bd);
        dai_doc_sync_apply(sync);
        dai_step(w);

        const dai_vec3 eye{ 5.4f, 4.0f, 8.6f }, look{ 0.0f, 1.1f, 0.0f }, up{ 0, 1, 0 };
        const float FOV = 55.0f, FW = 1600.0f, FH = 900.0f;
        dai_editor_camera(ed, eye, look, up, FOV, 0.1f, 200.0f, FW, FH);
        dai_editor_select(ed, target, 0);
        dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

        // One frame to lay the dock out, then the rect it produced - the same
        // two steps examples/editor_demo.cpp takes every frame.
        dai_ui_input gin{};
        gin.mouse_x = -100; gin.mouse_y = -100;
        dai_ui_begin(ui, FW, FH, &gin);
        dai_editor_ui_frame(panels, FW, FH);
        dai_ui_end(ui);
        float vx = 0, vy = 0, vw = 0, vh = 0;
        dai_editor_ui_viewport_rect(panels, &vx, &vy, &vw, &vh);
        CHECK(vw > 100.0f && vh > 100.0f && (vx > 1.0f || vy > 1.0f),
              "the scene body rect is not a sub-rectangle of the frame: %.0f,%.0f %.0fx%.0f",
              (double)vx, (double)vy, (double)vw, (double)vh);
        dai_editor_camera_viewport_rect(ed, vx, vy, vw, vh);

        // Where the RENDERER puts the object's centre, in surface pixels.
        auto ref_project = [&](dai_vec3 p, float *sx, float *sy) {
            dai_vec3 f{ look.x - eye.x, look.y - eye.y, look.z - eye.z };
            float fl = std::sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
            f = { f.x/fl, f.y/fl, f.z/fl };
            dai_vec3 rgt{ f.y*up.z - f.z*up.y, f.z*up.x - f.x*up.z, f.x*up.y - f.y*up.x };
            float rl = std::sqrt(rgt.x*rgt.x + rgt.y*rgt.y + rgt.z*rgt.z);
            rgt = { rgt.x/rl, rgt.y/rl, rgt.z/rl };
            dai_vec3 u2{ rgt.y*f.z - rgt.z*f.y, rgt.z*f.x - rgt.x*f.z, rgt.x*f.y - rgt.y*f.x };
            dai_vec3 v{ p.x - eye.x, p.y - eye.y, p.z - eye.z };
            float z = v.x*f.x + v.y*f.y + v.z*f.z;
            float t = std::tan(FOV * 3.14159265358979323846f / 360.0f);
            float aspect = vw / vh;                       // the RECT's aspect
            float ndc_x = (v.x*rgt.x + v.y*rgt.y + v.z*rgt.z) / (z * t * aspect);
            float ndc_y = (v.x*u2.x + v.y*u2.y + v.z*u2.z) / (z * t);
            *sx = vx + (ndc_x + 1.0f) * 0.5f * vw;        // the viewport transform
            *sy = vy + (1.0f - ndc_y) * 0.5f * vh;
        };
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        dai_doc_world_transform(doc, target, &wp, &wr, &ws);
        float ox = 0, oy = 0;
        ref_project(wp, &ox, &oy);
        CHECK(ox > vx && ox < vx + vw && oy > vy && oy < vy + vh,
              "the fixture object is not inside the scene rect (%.1f,%.1f)", (double)ox, (double)oy);

        // 1. the anchor the gizmo model uses: the point its arms share.
        uint32_t gn = dai_editor_gizmo_lines(ed, nullptr, 0);
        CHECK(gn >= 3, "the translate gizmo produced %u lines", gn);
        std::vector<dai_gizmo_line> gl(gn ? gn : 1);
        dai_editor_gizmo_lines(ed, gl.data(), gn);
        // The anchor is the one point all three ARMS meet at - the arrow tips
        // are shared by four lines each, so counting endpoints is not enough:
        // what identifies the centre is that lines of three DIFFERENT axes
        // touch it.
        dai_vec3 anchor{ 0, 0, 0 };
        int anchor_hits = 0;
        for (uint32_t i = 0; i < gn * 2; ++i) {
            dai_vec3 p = (i & 1u) ? gl[i / 2].b : gl[i / 2].a;
            int axes = 0;
            for (int ax = DAI_AXIS_X; ax <= DAI_AXIS_Z; ++ax) {
                for (uint32_t j = 0; j < gn; ++j) {
                    if (gl[j].axis != ax) continue;
                    float dax = gl[j].a.x - p.x, day = gl[j].a.y - p.y, daz = gl[j].a.z - p.z;
                    float dbx = gl[j].b.x - p.x, dby = gl[j].b.y - p.y, dbz = gl[j].b.z - p.z;
                    if (std::sqrt(dax*dax + day*day + daz*daz) < 1e-4f ||
                        std::sqrt(dbx*dbx + dby*dby + dbz*dbz) < 1e-4f) { ++axes; break; }
                }
            }
            if (axes > anchor_hits) { anchor_hits = axes; anchor = p; }
        }
        CHECK(anchor_hits == 3, "no point is shared by all three gizmo arms (best %d)", anchor_hits);
        float gax = 0, gay = 0;
        CHECK(dai_editor_project(ed, anchor, &gax, &gay) == 1, "the gizmo anchor does not project");
        float d_anchor = std::sqrt((gax - ox)*(gax - ox) + (gay - oy)*(gay - oy));
        CHECK(d_anchor < 5.0f,
              "the gizmo anchor is %.1f px off the object: gizmo at %.1f,%.1f, object at %.1f,%.1f",
              (double)d_anchor, (double)gax, (double)gay, (double)ox, (double)oy);

        // 2. what dai_editor_ui_gizmo really DRAWS. The rotate gizmo is three
        //    rings around the anchor, so the centre of the ink it puts down is
        //    the anchor - a check on the pixels, not on the model behind them.
        dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
        dai_ui_begin(ui, FW, FH, &gin);
        dai_editor_ui_gizmo(panels);
        dai_ui_end(ui);
        float bx0, by0, bx1, by1;
        vert_bounds(ui, &bx0, &by0, &bx1, &by1);
        CHECK(total_verts(ui) > 0, "the gizmo drew nothing at all");
        float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
        float d_drawn = std::sqrt((cx - ox)*(cx - ox) + (cy - oy)*(cy - oy));
        CHECK(d_drawn < 5.0f,
              "the drawn gizmo's centre is %.1f px off the object: %.1f,%.1f vs %.1f,%.1f",
              (double)d_drawn, (double)cx, (double)cy, (double)ox, (double)oy);
        std::printf("  gizmo anchor %.1f px, drawn centre %.1f px from the object centre\n",
                    (double)d_anchor, (double)d_drawn);
        dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, target);
        dai_doc_sync_apply(sync);
        dai_editor_camera(ed, dai_vec3{ 0, 3, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                          55.0f, 0.1f, 200.0f, 1280.0f, 720.0f);
        dai_editor_camera_viewport_rect(ed, 0.0f, 0.0f, 1280.0f, 720.0f);
    }

    // ---- 12. blockout: the recipe the inspector edits IS the mesh -----------
    // A blockout node carries five numbers, not a mesh; the host builds the
    // triangles from them. So "undo" is proved on the mesh, not on the field:
    // the digest of daiblock::finalise(build(shape_of(desc))) before the drag,
    // after it, after undo and after redo. The Size row is FOUND by probing
    // rows the way the Position row is above, not computed from a pixel.
    std::printf("blockout\n");
    {
        // Without a renderer the host has nothing to build into, and its
        // contract is to do nothing rather than crash - checked once here so
        // the include is not decoration.
        {
            dai_ext_host ext{};
            ext.doc = doc; ext.sync = sync; ext.scene = sc; ext.renderer = nullptr;
            dai_blockout_host_sync(&ext);
        }
        auto box_desc = [](const char *name, float x) {
            dai_node_desc b = dai_node_desc_default();
            std::snprintf(b.name, sizeof(b.name), "%s", name);
            b.no_body = b.no_collider = b.no_rigidbody = 1;
            b.position = { x, 0, 0 };
            b.blockout = DAI_BLOCKOUT_BOX;
            b.blockout_size = { 2, 1, 1 };
            b.render_extent = { 1, 1, 1 };
            return b;
        };
        auto mesh_digest = [&](dai_node n) -> uint64_t {
            dai_node_desc q{};
            if (dai_doc_get(doc, n, &q) != DAI_OK) return 0;
            return daiblock::digest(daiblock::finalise(daiblock::build(daiblockhost::shape_of(q))));
        };
        // A frame with the button up first: a field transaction an earlier
        // section left open would swallow the add below into its undo step,
        // and the first undo would then take the box away instead of the drag.
        frame(-100.0f, -100.0f, 0);
        dai_node_desc bd = box_desc("BlockBox", 0.0f);
        dai_node box = dai_doc_add(doc, &bd);
        dai_doc_sync_apply(sync);
        const uint64_t d_old = mesh_digest(box);
        CHECK(d_old != 0, "the box built no mesh");
        CHECK(d_old == mesh_digest(box), "two builds of the same box give different digests");
        {
            dai_node_desc other = bd;
            other.blockout_size = { 2, 1, 1.5f };
            CHECK(daiblock::digest(daiblock::finalise(daiblock::build(daiblockhost::shape_of(other))))
                  != d_old, "a different size gives the same digest - the digest is blind");
        }

        // A frame with the whole input record: Escape is what closes a
        // dropdown a probe happened to open.
        auto frame_in = [&](dai_ui_input in) {
            dai_ui_begin(ui, 1280, 720, &in);
            dai_editor_ui_expand_all(panels);
            dai_editor_ui_toolbar(panels, 0.0f, 0.0f, 1280.0f);
            dai_editor_ui_hierarchy(panels, PANEL_X, PANEL_Y, PANEL_W, 360.0f);
            dai_editor_ui_inspector(panels, INSPECTOR_X, PANEL_Y, PANEL_W, 592.0f);
            dai_editor_ui_gizmo(panels);
            dai_ui_end(ui);
        };
        // A press on a component header folds it, and the blockout sections
        // keep their own fold state - so a probe that folded something puts
        // it back with a second click, and one that opened a dropdown closes
        // it. Judged by the vertex count of a hover-only frame at the same
        // spot: a folded section is fewer triangles, an open dropdown more.
        auto probe_row = [&](float x, float y) {
            frame(x, y, 0);
            uint32_t v0 = total_verts(ui);
            frame(x, y, 1);
            for (int i = 1; i <= 10; ++i) frame(x + (float)i * 3.0f, y, 1);
            frame(x + 30.0f, y, 0);
            return v0;
        };
        auto restore_panel = [&](float x, float y, uint32_t v0) {
            if (dai_ui_popup_active(ui)) {
                dai_ui_input in{};
                in.mouse_x = x; in.mouse_y = y; in.key_escape = 1;
                frame_in(in);
            }
            frame(x, y, 0);
            if (total_verts(ui) != v0) { frame(x, y, 1); frame(x, y, 0); }
            frame(x, y, 0);
        };

        // Where the Size row is for the CURRENT selection - found again for
        // the multi-select below, whose inspector may lay itself out
        // differently.
        auto find_size_row = [&](int *rows_out) {
            float found = -1.0f;
            int rows = 0;
            for (float y = 90.0f; y < 650.0f; y += 4.0f) {
                dai_node_desc a{}, b2{};
                dai_doc_get(doc, box, &a);
                uint32_t depth_before = dai_editor_undo_depth(ed);
                uint32_t v0 = probe_row(FIELD_X, y);
                dai_doc_get(doc, box, &b2);
                if (std::fabs(b2.blockout_size.x - a.blockout_size.x) > 1e-6f) {
                    if (found < 0.0f) found = y;
                    ++rows;
                }
                while (dai_editor_undo_depth(ed) > depth_before) dai_editor_undo(ed);
                restore_panel(FIELD_X, y, v0);
            }
            if (rows_out) *rows_out = rows;
            return found;
        };
        dai_editor_select(ed, box, 0);
        int size_rows = 0;
        float size_y = find_size_row(&size_rows);
        CHECK(size_y > 0.0f, "no row in the inspector edits the blockout size X");
        CHECK(size_rows <= 8, "%d different rows changed blockout_size.x - fields overlap", size_rows);
        CHECK(mesh_digest(box) == d_old, "the probing left the box changed");

        if (size_y > 0.0f) {
            uint32_t undo_before = dai_editor_undo_depth(ed);
            probe_row(FIELD_X, size_y);
            dai_node_desc after{};
            dai_doc_get(doc, box, &after);
            const uint64_t d_new = mesh_digest(box);
            CHECK(after.blockout_size.x > 2.0f,
                  "dragging Size X right did not grow it (2.000 -> %.3f)", after.blockout_size.x);
            CHECK(std::fabs(after.blockout_size.y - 1.0f) < 1e-6f, "dragging Size X also moved Y");
            CHECK(d_new != d_old, "a changed size built the same mesh");
            CHECK(dai_editor_undo_depth(ed) == undo_before + 1,
                  "a size drag over 11 frames made %u undo steps, expected 1",
                  dai_editor_undo_depth(ed) - undo_before);
            CHECK(dai_editor_undo(ed) == 1, "undo after the size drag failed");
            CHECK(mesh_digest(box) == d_old, "undo did not give the old mesh back");
            CHECK(dai_editor_redo(ed) == 1, "redo after the undo failed");
            CHECK(mesh_digest(box) == d_new, "redo did not give the new mesh back");
            dai_editor_undo(ed);
            CHECK(mesh_digest(box) == d_old, "the second undo did not give the old mesh back");
        }

        // Multi-select: the same drag on two selected boxes lands on both,
        // as ONE undo step - the diff mechanism above the seam, not a copy.
        {
            dai_node_desc bd2 = box_desc("BlockBox2", 3.0f);
            dai_node box2 = dai_doc_add(doc, &bd2);
            dai_doc_sync_apply(sync);
            dai_editor_select(ed, box, 0);
            dai_editor_select(ed, box2, 1);
            CHECK(dai_editor_selection_count(ed) == 2, "two boxes were not both selected");
            float size_y2 = find_size_row(nullptr);
            CHECK(size_y2 > 0.0f, "with two boxes selected no row edits the size");
            CHECK(mesh_digest(box) == d_old && mesh_digest(box2) == d_old,
                  "the multi-select probing left a box changed");
            uint32_t undo_before = dai_editor_undo_depth(ed);
            if (size_y2 > 0.0f) probe_row(FIELD_X, size_y2);
            dai_node_desc a1{}, a2{};
            dai_doc_get(doc, box, &a1);
            dai_doc_get(doc, box2, &a2);
            CHECK(a1.blockout_size.x > 2.0f, "multi-select drag did not change the first box");
            CHECK(a2.blockout_size.x > 2.0f, "multi-select drag did not reach the second box");
            CHECK(std::fabs(a1.blockout_size.x - a2.blockout_size.x) < 1e-6f,
                  "the two boxes got different sizes (%.3f vs %.3f)",
                  a1.blockout_size.x, a2.blockout_size.x);
            CHECK(std::fabs(a2.position.x - 3.0f) < 1e-6f, "multi-select copied the position too");
            CHECK(dai_editor_undo_depth(ed) == undo_before + 1,
                  "a multi-select drag made %u undo steps, expected 1",
                  dai_editor_undo_depth(ed) - undo_before);
            CHECK(dai_editor_undo(ed) == 1, "undo of the multi-select drag failed");
            CHECK(mesh_digest(box) == d_old && mesh_digest(box2) == d_old,
                  "undo did not restore both boxes");
            dai_editor_deselect_all(ed);
            dai_doc_remove(doc, box2);
        }

        // CSG: a wall with a doorway under it. The Operation dropdown is
        // switched through the panel and undone; the op and the child count
        // come back. The row is found like the others: the press that opens
        // a dropdown, then the item below it that changes the field.
        {
            dai_node_desc wd = box_desc("BlockWall", 0.0f);
            wd.blockout_size = { 4, 3, 0.2f };
            wd.blockout_pivot = { 0, -1, 0 };
            wd.csg = DAI_CSG_SUBTRACT;
            dai_node wall = dai_doc_add(doc, &wd);
            dai_node_desc dd = box_desc("Doorway", 0.0f);
            dd.parent = wall;
            dd.blockout_size = { 1, 2, 0.5f };
            dd.blockout_pivot = { 0, -1, 0 };
            dai_node door = dai_doc_add(doc, &dd);
            dai_doc_sync_apply(sync);
            CHECK(dai_doc_children(doc, wall, nullptr, 0) == 1, "the doorway is not the wall's child");
            const uint64_t d_wall = mesh_digest(wall);
            dai_editor_select(ed, wall, 0);
            // One frame before the count starts: the Mesh Renderer section
            // gives a node it sees for the first time its materials slot, and
            // that is its own step - not the operation switch under test.
            frame(-100.0f, -100.0f, 0);

            // "Operation" is a longer word than "Position", so its value
            // column starts further right - a dropdown probed at the x that
            // found the Position handle presses the LABEL and never opens.
            // The right hand end of the row is inside the value box whatever
            // the name above it is, so that is where a dropdown is looked for.
            const float OPT_X = INSPECTOR_X + PANEL_W - 24.0f;
            int switched = 0;
            uint32_t undo_before = dai_editor_undo_depth(ed);
            // The depth right before the click that opened the dropdown: the
            // switch is measured against THAT, so a probe row above it that
            // edited and was undone cannot be counted twice.
            uint32_t undo_at_open = undo_before;
            for (float y = 90.0f; y < 650.0f && !switched; y += 4.0f) {
                frame(OPT_X, y, 0);
                uint32_t v0 = total_verts(ui);
                undo_at_open = dai_editor_undo_depth(ed);
                frame(OPT_X, y, 1);
                frame(OPT_X, y, 0);
                if (!dai_ui_popup_active(ui)) {
                    // A click that edited something is undone like every
                    // other probe; a header it may have folded is put back.
                    while (dai_editor_undo_depth(ed) > undo_before) dai_editor_undo(ed);
                    frame(OPT_X, y, 0);
                    if (total_verts(ui) != v0) { frame(OPT_X, y, 1); frame(OPT_X, y, 0); }
                    continue;
                }
                for (float y2 = y + 4.0f; y2 < y + 140.0f && !switched; y2 += 4.0f) {
                    frame(OPT_X, y2, 0);
                    frame(OPT_X, y2, 1);
                    frame(OPT_X, y2, 0);
                    dai_node_desc q{};
                    dai_doc_get(doc, wall, &q);
                    if (q.csg != DAI_CSG_SUBTRACT) { switched = q.csg; break; }
                    if (!dai_ui_popup_active(ui)) break;
                }
                if (!switched) {
                    while (dai_editor_undo_depth(ed) > undo_before) dai_editor_undo(ed);
                    dai_ui_input in{};
                    in.mouse_x = OPT_X; in.mouse_y = y; in.key_escape = 1;
                    frame_in(in);
                    frame(OPT_X, y, 0);
                    if (total_verts(ui) != v0) { frame(OPT_X, y, 1); frame(OPT_X, y, 0); }
                }
            }
            CHECK(switched != 0, "no dropdown in the inspector switches the CSG operation");
            CHECK(switched == DAI_CSG_UNION || switched == DAI_CSG_INTERSECT,
                  "the operation switched to %d, which is no dai_csg_op", switched);
            CHECK(dai_editor_undo_depth(ed) == undo_at_open + 1,
                  "switching the operation made %u undo steps, expected 1",
                  dai_editor_undo_depth(ed) - undo_at_open);
            CHECK(dai_doc_children(doc, wall, nullptr, 0) == 1,
                  "switching the operation changed the child count");
            CHECK(dai_editor_undo(ed) == 1, "undo of the operation switch failed");
            dai_node_desc back{};
            dai_doc_get(doc, wall, &back);
            CHECK(back.csg == DAI_CSG_SUBTRACT, "undo did not restore the operation (%d)", back.csg);
            CHECK(dai_doc_children(doc, wall, nullptr, 0) == 1, "undo changed the child count");
            CHECK(mesh_digest(wall) == d_wall, "undo did not give the wall's own shape back");
            dai_editor_deselect_all(ed);
            dai_doc_remove(doc, door);
            dai_doc_remove(doc, wall);
        }

        // The socket gizmo: a scene with a door socket draws more in the
        // Scene view than the same scene without one - and nothing extra in
        // the Game view, where editor lines do not belong.
        {
            dai_editor_ui_view_set(panels, DAI_VIEW_SCENE);
            dai_editor_camera(ed, dai_vec3{ 0, 3, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                              55.0f, 0.1f, 200.0f, 1280.0f, 720.0f);
            dai_ui_input gin{};
            gin.mouse_x = -100; gin.mouse_y = -100;
            auto scene_verts = [&]() {
                // Two passes, as every host does: the dock lays itself out
                // first, then the frame is drawn into the rect it found.
                for (int pass = 0; pass < 2; ++pass) {
                    dai_ui_begin(ui, 1280, 720, &gin);
                    dai_editor_ui_frame(panels, 1280, 720);
                    dai_ui_end(ui);
                    float lx = 0, ly = 0, lw = 1280, lh = 720;
                    dai_editor_ui_viewport_rect(panels, &lx, &ly, &lw, &lh);
                    dai_editor_camera_viewport_rect(ed, lx, ly, lw, lh);
                }
                return total_verts(ui);
            };
            // Nothing selected: the inspector would otherwise grow a Door
            // Socket section along with the gizmo, and the count would be
            // measuring the panel, not the viewport.
            dai_editor_deselect_all(ed);
            uint32_t without = scene_verts();
            dai_node_desc sd{};
            dai_doc_get(doc, box, &sd);
            sd.door_socket = 1;
            sd.door_offset = { 0, -0.5f, 0.5f };
            sd.door_normal = { 0, 0, 1 };
            sd.door_width = 0.9f;
            sd.door_height = 2.05f;
            dai_doc_set(doc, box, &sd);
            uint32_t with = scene_verts();
            CHECK(with > without, "a door socket drew nothing in the Scene view (%u vs %u verts)",
                  with, without);
            // The frame comes from the WORLD transform: a yawed node still
            // draws its socket, and draws it somewhere else.
            float sx0, sy0, sx1, sy1;
            vert_bounds(ui, &sx0, &sy0, &sx1, &sy1);
            dai_node_desc yd = sd;
            yd.rotation = { 0, 0.7071068f, 0, 0.7071068f };   // 90 degrees about Y
            dai_doc_set(doc, box, &yd);
            uint32_t yawed = scene_verts();
            CHECK(yawed > without, "a yawed node lost its socket gizmo");
            // The Game view is compared with ITSELF, socket on against socket
            // off - the two views differ by their own chrome, not only by
            // the lines under test.
            // Inside dai_editor_ui_frame the view is whichever tab the dock
            // shows, so the Game view is reached the way a user reaches it -
            // through the panel - and lands a frame later.
            dai_doc_set(doc, box, &sd);
            dai_editor_ui_panel_open(panels, "Game");
            scene_verts();
            CHECK(dai_editor_ui_view(panels) == DAI_VIEW_GAME, "opening the Game panel did not switch the view");
            uint32_t game_with = scene_verts();
            dai_node_desc nd = sd;
            nd.door_socket = 0; nd.door_width = 0; nd.door_height = 0;
            dai_doc_set(doc, box, &nd);
            uint32_t game_without = scene_verts();
            CHECK(game_with == game_without,
                  "the Game view drew the socket gizmo (%u vs %u verts)", game_with, game_without);
            dai_editor_ui_panel_open(panels, "Scene");
            scene_verts();
            CHECK(dai_editor_ui_view(panels) == DAI_VIEW_SCENE, "opening the Scene panel did not switch back");
            dai_doc_set(doc, box, &sd);
            dai_editor_camera_viewport_rect(ed, 0.0f, 0.0f, 1280.0f, 720.0f);
        }

        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, box);
        dai_doc_sync_apply(sync);
    }

    // ---- the sentence the show status line ends in --------------------------
    //
    // "1 conflicts" is not a typo a reader forgives: this line is the last
    // thing on the screen before a show is signed off, and a program that
    // cannot count to one is a program whose count of eleven nobody believes
    // either. dai_show_ui_verdict hands over the very string the status line
    // draws, so the check reads the panel rather than a second copy of the
    // wording. The shows below are built by hand - two formations of six
    // drones, one of which parks a pair inside the minimum distance - because
    // a fixture that has to be sampled and solved would be testing the
    // pipeline, and what is under test here is one sentence.
    {
        auto build = [](int faults, uint32_t *conflicts_out) {
            dai_show_settings s = dai_show_settings_default();
            s.drone_count    = 6;
            s.min_distance_m = 2.0f;
            s.v_max_ms       = 8.0f;
            s.a_max_ms2      = 4.0f;
            s.fps            = 10;
            s.seed           = 7u;
            dai_show *sh = dai_show_create(&s);
            dai_show_point a[6], b[6];
            for (int i = 0; i < 6; ++i) {
                a[i] = dai_show_point{};
                a[i].x = (float)i * 10.0f; a[i].y = 30.0f; a[i].z = 0.0f;
                a[i].r = 200; a[i].g = 200; a[i].b = 200; a[i].w = 0;
                b[i] = a[i];
            }
            if (faults >= 1) b[3].x = b[2].x + 0.5f;     // one pair, 0.5 m apart
            if (faults >= 2) b[5].x = b[4].x + 0.5f;     // and a second one
            dai_show_formation_add(sh, "Line", "builtin://line", a, 6, 4.0f);
            dai_show_formation_add(sh, "Line (fault)", "builtin://line", b, 6, 4.0f);
            char serr[256] = { 0 };
            dai_show_solve(sh, serr, sizeof(serr));
            dai_show_validate_show(sh);
            if (conflicts_out) *conflicts_out = dai_show_get_timings(sh).last_validate.conflicts;
            return sh;
        };
        char said[128] = { 0 };
        for (int faults = 0; faults <= 2; ++faults) {
            uint32_t n = 0;
            dai_show *sh = build(faults, &n);
            dai_show_ui *su = dai_show_ui_create(sh);
            dai_show_ui_verdict(su, said, sizeof(said));
            const char *want = (faults == 0) ? "no conflicts"
                             : (faults == 1) ? "1 conflict" : "2 conflicts";
            CHECK(n == (uint32_t)faults, "%d planted fault(s) came back as %u conflicts",
                  faults, n);
            CHECK(std::strcmp(said, want) == 0,
                  "the verdict for %d fault(s) reads \"%s\", not \"%s\"", faults, said, want);
            dai_show_ui_destroy(su);
            dai_show_destroy(sh);
        }
        std::printf("  the show verdict counts: \"no conflicts\" / \"1 conflict\" / "
                    "\"2 conflicts\"\n");
    }

    // ---- the gizmo and the brush, driven the way a hand drives them ---------
    //
    // Everything else about a figure's transform is tested in
    // tests/droneshow_cases_edit.cpp against the document. This block is about
    // the other half, and it is the half nobody had ever exercised: the handle
    // a user grabs in the preview, and the click that paints a drone. Both run
    // through dai_ui with synthetic mouse input on a machine with no GPU - the
    // viewport draws through the 2D canvas, so it needs no renderer.
    //
    // The handle is grabbed WHERE IT WAS DRAWN, read back out of the panel
    // rather than projected a second time here: a test that computes the
    // handle's position itself agrees with its own arithmetic instead of with
    // the gizmo, and the bug that would slip past is exactly the one that
    // matters - a handle drawn somewhere other than where it can be grabbed.
    {
        const float VX = 300.0f, VY = 40.0f, VW = 900.0f, VH = 600.0f;
        dai_show_settings s = dai_show_settings_default();
        s.drone_count    = 16;
        s.min_distance_m = 2.0f;
        s.fps            = 10;
        s.seed           = 3u;
        dai_show *sh = dai_show_create(&s);
        dai_show_point a[16], b[16];
        for (int i = 0; i < 16; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)(i % 4) * 6.0f - 9.0f;
            a[i].y = 40.0f;
            a[i].z = (float)(i / 4) * 6.0f - 9.0f;
            a[i].r = 200; a[i].g = 200; a[i].b = 200; a[i].w = 0;
            b[i] = a[i];
            b[i].y = 60.0f;
        }
        dai_show_formation_add(sh, "Grid", "builtin://grid", a, 16, 4.0f);
        dai_show_formation_add(sh, "Grid up", "builtin://grid", b, 16, 4.0f);
        char serr[256] = { 0 };
        dai_show_solve(sh, serr, sizeof(serr));

        dai_show_ui *su = dai_show_ui_create(sh);
        dai_show_ui_select_formation(su, 1);

        auto frame = [&](float mxp, float myp, int mdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.mouse_down = mdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
        };

        // One frame with the pointer parked outside, so the handles get drawn
        // and reported without anything being grabbed.
        frame(10.0f, 10.0f, 0);
        float hx = 0.0f, hy = 0.0f;
        int   have_handle = dai_show_ui_gizmo_handle(su, 0, &hx, &hy);
        CHECK(have_handle, "the X gizmo handle was never drawn - a figure cannot be moved");
        CHECK(hx >= VX && hx <= VX + VW && hy >= VY && hy <= VY + VH,
              "the X handle was drawn at %.0f,%.0f, outside the %.0fx%.0f preview",
              (double)hx, (double)hy, (double)VW, (double)VH);

        // UP IS UP. The Y handle points along world +Y from the same pivot the
        // X handle starts at, so on a screen it has to be drawn HIGHER - a
        // smaller y. For weeks it was drawn lower, because the camera's up
        // basis was forward x right, which is minus the world up: the whole
        // preview was a vertical mirror of itself, ground grid over the
        // figures, and nobody could point at what was wrong.
        {
            float xhx = 0.0f, xhy = 0.0f, yhx = 0.0f, yhy = 0.0f;
            int hx_ok = dai_show_ui_gizmo_handle(su, 0, &xhx, &xhy);
            int hy_ok = dai_show_ui_gizmo_handle(su, 1, &yhx, &yhy);
            CHECK(hx_ok && hy_ok, "the X and Y handles were not both drawn");
            if (hx_ok && hy_ok)
                CHECK(yhy < xhy - 1.0f,
                      "the Y handle is drawn at y %.1f, the X handle at y %.1f - up is "
                      "pointing down, the whole view is mirrored", (double)yhy, (double)xhy);
        }

        if (have_handle) {
            dai_show_formation_info before;
            dai_show_formation_get(sh, 1, &before);

            // press ON the handle, then drag sixty pixels and let go.
            frame(hx, hy, 0);
            frame(hx, hy, 1);
            frame(hx + 60.0f, hy, 1);
            frame(hx + 60.0f, hy, 0);

            dai_show_formation_info after;
            dai_show_formation_get(sh, 1, &after);
            float dx = after.xf.position.x - before.xf.position.x;
            float dy = after.xf.position.y - before.xf.position.y;
            float dz = after.xf.position.z - before.xf.position.z;
            CHECK(std::fabs(dx) > 0.05f,
                  "dragging the X handle sixty pixels moved the figure %.4f m in X", (double)dx);
            CHECK(std::fabs(dy) < 1e-4f && std::fabs(dz) < 1e-4f,
                  "dragging the X handle also moved Y by %.4f and Z by %.4f - an axis handle "
                  "that moves two axes is not an axis handle", (double)dy, (double)dz);
            std::printf("  the X handle moved the figure %.2f m for 60 px\n", (double)dx);

            // And the same drag with the pointer NOT on a handle turns the
            // camera instead of moving the figure. A preview where every drag
            // moves the show is a preview you cannot look around in.
            dai_show_formation_get(sh, 1, &before);
            frame(VX + 12.0f, VY + 12.0f, 0);
            frame(VX + 12.0f, VY + 12.0f, 1);
            frame(VX + 90.0f, VY + 40.0f, 1);
            frame(VX + 90.0f, VY + 40.0f, 0);
            dai_show_formation_get(sh, 1, &after);
            CHECK(after.xf.position.x == before.xf.position.x &&
                  after.xf.position.y == before.xf.position.y &&
                  after.xf.position.z == before.xf.position.z,
                  "a drag in empty sky moved the figure - the gizmo has no grab radius");
        }

        // ---- the brush ----------------------------------------------------
        // A wide brush over the middle of the preview has to reach SOME point
        // of the figure and none of the others may change: painting is a local
        // edit, and a brush that repaints the whole show is a brush nobody dares
        // to use.
        {
            dai_show_ui_select_formation(su, 0);
            dai_show_ui_seek(su, 0.0f);
            const dai_show_point *fp = dai_show_formation_points(sh, 0);
            std::vector<dai_show_point> was(fp, fp + 16);

            dai_show_ui_pick_mode(su, 1, 1);
            dai_show_ui_brush(su, 400.0f, 255, 0, 0);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 0);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 1);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 0);

            // Painting is a STROKE at the playhead now: the point keeps its
            // own colour, the PLAN burns red from the painted second on. So
            // the check reads the plan, not the figure.
            // The gizmo drags above threw the plan away (a moved figure is a
            // new show), so solve again - the stroke lives on the formation
            // and survives that, which is half the point of the design.
            char berr[256] = { 0 };
            dai_show_solve(sh, berr, sizeof(berr));
            const dai_show_plan *bp = dai_show_get_plan(sh);
            CHECK(bp != nullptr, "the brush test needs a solved show: %s", berr);
            int painted = 0;
            for (int i = 0; i < 16; ++i) {
                dai_show_point s0;
                dai_show_plan_sample(bp, (uint32_t)i, 0.0f, &s0);
                if (s0.r == 255 && s0.g == 0 && s0.b == 0) ++painted;
            }
            CHECK(painted > 0, "a 400 px brush over the middle of the preview painted nothing");
            std::printf("  the brush painted %d of 16 points\n", painted);

            // Picking without painting selects one point and changes no colour.
            dai_show_ui_pick_mode(su, 1, 0);
            std::vector<dai_show_point> before_pick(fp, fp + 16);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 0);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 1);
            frame(VX + VW * 0.5f, VY + VH * 0.5f, 0);
            fp = dai_show_formation_points(sh, 0);
            int changed = 0;
            for (int i = 0; i < 16; ++i)
                if (fp[i].r != before_pick[i].r || fp[i].g != before_pick[i].g ||
                    fp[i].b != before_pick[i].b) ++changed;
            CHECK(changed == 0, "picking a point repainted %d of them", changed);
            CHECK(dai_show_ui_picked_point(su) != 0xFFFFFFFFu,
                  "a click in the preview with picking on selected no point");
        }

        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

    // ---- the show viewport: what a LEFT drag means ------------------------
    //
    // Three things that were one gesture before and are three now: a left drag
    // on empty sky does nothing to the camera, Alt+left orbits, and a left
    // click selects a drone without dragging the point it stands on. The old
    // behaviour made every click a small deformation of the figure.
    {
        std::printf("\n-- show: the left button selects, Alt turns the camera\n");
        const float VX = 300.0f, VY = 40.0f, VW = 900.0f, VH = 600.0f;
        dai_show_settings s = dai_show_settings_default();
        s.drone_count = 16; s.min_distance_m = 2.0f; s.fps = 10; s.seed = 3u;
        dai_show *sh = dai_show_create(&s);
        dai_show_point a[16], b[16];
        for (int i = 0; i < 16; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)(i % 4) * 6.0f - 9.0f;
            a[i].y = 40.0f;
            a[i].z = (float)(i / 4) * 6.0f - 9.0f;
            a[i].r = 200; a[i].g = 200; a[i].b = 200; a[i].w = 0;
            b[i] = a[i];
            b[i].y = 60.0f;
        }
        dai_show_formation_add(sh, "Grid", "builtin://grid", a, 16, 4.0f);
        dai_show_formation_add(sh, "Grid up", "builtin://grid", b, 16, 4.0f);
        char serr[256] = { 0 };
        dai_show_solve(sh, serr, sizeof(serr));
        dai_show_ui *su = dai_show_ui_create(sh);
        dai_show_ui_select_formation(su, 0);
        dai_show_ui_seek(su, 0.0f);

        auto frame = [&](float mxp, float myp, int mdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.mouse_down = mdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
        };
        // A patch of sky well away from the figure and away from the gizmo, so
        // nothing is grabbed by accident.
        const float SKY_X = VX + 60.0f, SKY_Y = VY + 60.0f;

        // 1. No Alt: a left drag across the sky must not turn the camera. The
        //    proof is what is DRAWN - if the camera moved, the X handle moved
        //    with it.
        // Measured on the CAMERA rather than on the gizmo handle: a plain
        // click on empty sky now clears the selection, and an unselected
        // figure has no handle to compare. The eye is the thing the claim is
        // about anyway - "the left button does not turn the camera".
        dai_show_ui_modifiers(su, 0, 0, 0);
        dai_show_ui_select_formation(su, 1);
        frame(SKY_X, SKY_Y, 0);
        float e_a[3] = { 0, 0, 0 }, e_b[3] = { 0, 0, 0 }, e_c[3] = { 0, 0, 0 };
        dai_show_ui_eye(su, e_a);
        frame(SKY_X, SKY_Y, 1);
        frame(SKY_X + 200.0f, SKY_Y + 40.0f, 1);
        frame(SKY_X + 200.0f, SKY_Y + 40.0f, 0);
        frame(SKY_X, SKY_Y, 0);
        dai_show_ui_eye(su, e_b);
        float d_plain = std::fabs(e_a[0] - e_b[0]) + std::fabs(e_a[1] - e_b[1]) +
                        std::fabs(e_a[2] - e_b[2]);
        CHECK(d_plain < 0.01f,
              "a plain left drag moved the camera %.3f m - the left button "
              "belongs to the selection", (double)d_plain);
        // ...and it selected nothing, because it landed on nothing.
        CHECK(!dai_show_ui_has_selection(su),
              "a drag across empty sky kept the figure selected");

        // 2. With Alt held, the same drag DOES orbit - and leaves the
        //    selection alone, because it is a camera gesture.
        dai_show_ui_select_formation(su, 1);
        dai_show_ui_modifiers(su, 1, 0, 0);
        frame(SKY_X, SKY_Y, 0);
        frame(SKY_X, SKY_Y, 1);
        frame(SKY_X + 200.0f, SKY_Y + 40.0f, 1);
        frame(SKY_X + 200.0f, SKY_Y + 40.0f, 0);
        frame(SKY_X, SKY_Y, 0);
        dai_show_ui_eye(su, e_c);
        float d_alt = std::fabs(e_c[0] - e_b[0]) + std::fabs(e_c[1] - e_b[1]) +
                      std::fabs(e_c[2] - e_b[2]);
        CHECK(d_alt > 1.0f, "Alt + left drag did not turn the camera (%.3f m)",
              (double)d_alt);
        CHECK(dai_show_ui_has_selection(su),
              "an Alt orbit over empty sky dropped the selection");
        dai_show_ui_modifiers(su, 0, 0, 0);

        // 3. Delete, and undo bringing it back. The count is the whole proof.
        {
            uint32_t was = dai_show_formation_count(sh);
            dai_show_ui_select_formation(su, 1);
            CHECK(dai_show_ui_delete_selected(su), "delete refused to remove a figure");
            CHECK(dai_show_formation_count(sh) == was - 1u,
                  "delete left %u figures, expected %u",
                  dai_show_formation_count(sh), was - 1u);
            CHECK(dai_show_ui_undo_depth(su) > 0, "delete pushed no undo step");
            CHECK(dai_show_ui_undo(su), "undo refused after a delete");
            CHECK(dai_show_formation_count(sh) == was,
                  "undo left %u figures, expected %u back", dai_show_formation_count(sh), was);
            CHECK(dai_show_ui_redo(su) && dai_show_formation_count(sh) == was - 1u,
                  "redo did not delete it again");
            dai_show_ui_undo(su);
        }

        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

    // ---- one drone: its own selection, its own handle, its own undo -------
    //
    // Three things Justin asked for in one gesture. Clicking a drone must let
    // the FIGURE go (two gizmos on screen and no way to tell which one a drag
    // moves is worse than no gizmo), the handle it gets must be the same
    // handle an object gets, and moving it must be undoable - a move you
    // cannot take back is a move nobody dares to make.
    {
        std::printf("\n-- show: one drone is a selection of its own\n");
        const float VX = 300.0f, VY = 40.0f, VW = 900.0f, VH = 600.0f;
        dai_show_settings s = dai_show_settings_default();
        s.drone_count = 16; s.min_distance_m = 2.0f; s.fps = 10; s.seed = 3u;
        dai_show *sh = dai_show_create(&s);
        dai_show_point a[16], b[16];
        for (int i = 0; i < 16; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)(i % 4) * 6.0f - 9.0f;
            a[i].y = 40.0f;
            a[i].z = (float)(i / 4) * 6.0f - 9.0f;
            a[i].r = 200; a[i].g = 200; a[i].b = 200; a[i].w = 0;
            b[i] = a[i];
            b[i].y = 60.0f;
        }
        dai_show_formation_add(sh, "Grid", "builtin://grid", a, 16, 4.0f);
        dai_show_formation_add(sh, "Grid up", "builtin://grid", b, 16, 4.0f);
        char serr[256] = { 0 };
        dai_show_solve(sh, serr, sizeof(serr));

        dai_show_ui *su = dai_show_ui_create(sh);
        dai_show_ui_modifiers(su, 0, 0, 0);
        dai_show_ui_select_formation(su, 0);
        dai_show_ui_seek(su, 0.0f);

        auto frame = [&](float mxp, float myp, int mdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.mouse_down = mdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
        };

        // Where a drone actually IS on screen: the figure's gizmo is drawn
        // from the pivot, so the first drone is found by walking the preview
        // the way the user does - click, and see whether something answered.
        // The X handle gives the scale of the picture; the drones sit around
        // the pivot it starts from.
        frame(10.0f, 10.0f, 0);
        float px0 = 0.0f, py0 = 0.0f;
        int found = 0;
        // A coarse sweep: 30 px steps over the preview, pressing nothing -
        // only the click below commits. The first spot that selects a drone
        // is the one the rest of the case uses.
        for (float yy = VY + 20.0f; yy < VY + VH - 140.0f && !found; yy += 12.0f)
            for (float xx = VX + 20.0f; xx < VX + VW - 20.0f && !found; xx += 12.0f) {
                frame(xx, yy, 0);
                frame(xx, yy, 1);
                frame(xx, yy, 0);
                if (dai_show_ui_selected_drone(su) != 0xFFFFFFFFu) {
                    found = 1; px0 = xx; py0 = yy;
                }
            }
        CHECK(found, "no click anywhere in the preview ever selected a drone");
        if (found) {
            CHECK(dai_show_ui_selected_is_drone(su),
                  "clicking a drone did not take the selection off the figure - two "
                  "gizmos, and no way to tell which one a drag moves");

            // The handle it gets is the object's handle: all three axes,
            // drawn from the drone, the same length the figure gizmo uses.
            float hx = 0.0f, hy = 0.0f;
            int has = dai_show_ui_point_handle(su, 0, &hx, &hy);
            CHECK(has, "a selected drone got no move handle");
            uint32_t drone = dai_show_ui_selected_drone(su);
            uint32_t f2 = 0, pt = 0; int settled = 0;
            int known = has && dai_show_drone_point_at(sh, drone, dai_show_ui_time(su),
                                                       &f2, &pt, &settled);
            const dai_show_point *before =
                (known && f2 < dai_show_formation_count(sh))
                    ? dai_show_formation_points(sh, f2) : nullptr;
            CHECK(!has || (before && pt < 16u),
                  "the selected drone %u stands on no point the document knows", drone);
            if (has && before && pt < 16u) {
                float was_x = before[pt].x;

                frame(hx, hy, 0);
                frame(hx, hy, 1);
                frame(hx + 50.0f, hy, 1);
                frame(hx + 50.0f, hy, 0);

                const dai_show_point *after = dai_show_formation_points(sh, f2);
                CHECK(after && std::fabs(after[pt].x - was_x) > 0.05f,
                      "dragging the drone's X handle fifty pixels moved it %.4f m",
                      (double)(after ? after[pt].x - was_x : 0.0f));
                CHECK(dai_show_ui_undo_depth(su) > 0,
                      "moving one drone pushed no undo step - Ctrl+Z would do nothing");
                CHECK(dai_show_ui_undo(su), "undo refused after a drone was moved");
                const dai_show_point *back = dai_show_formation_points(sh, f2);
                CHECK(back && std::fabs(back[pt].x - was_x) < 1e-3f,
                      "undo left the drone at %.3f, it started at %.3f",
                      (double)(back ? back[pt].x : 0.0f), (double)was_x);
                CHECK(dai_show_ui_redo(su), "redo refused after undoing a drone move");
            }

            // Selecting a figure again takes the selection back off the drone.
            dai_show_ui_select_formation(su, 1);
            CHECK(!dai_show_ui_selected_is_drone(su),
                  "selecting a figure left the drone selected as well");
            CHECK(dai_show_ui_selected_drone(su) == 0xFFFFFFFFu,
                  "selecting a figure kept a drone selected");
        }

        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

    // ---- the dope sheet: the two numbers a show is made of, dragged --------
    //
    // The timeline is not a slider any more: every figure is a bar with a
    // diamond at each end, and dragging the right one is its hold. The test
    // grabs the handle where the strip PUTS it (the layout is documented in
    // dai_show_ui_viewport: a 54 px Play button at x+6, then the track out to
    // x+w-10, the whole show across it) and checks the document changed.
    {
    // ---- the hierarchy carries rows the way a scene hierarchy does ---------
    //
    // A step is the PARENT and a figure its child, so the list has to do the
    // two things every hierarchy does: re-order inside a parent, and move a
    // child to another parent. Both by carrying the row.
    {
        std::printf("\n-- show: the storyboard carries figures between steps\n");
        const float PX = 0.0f, PY = 0.0f, PW = 320.0f, PH = 600.0f;
        dai_show_settings s3 = dai_show_settings_default();
        s3.drone_count = 8; s3.min_distance_m = 2.0f; s3.fps = 10; s3.seed = 7u;
        dai_show *sh = dai_show_create(&s3);
        dai_show_point a[4];
        for (int i = 0; i < 4; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)i * 5.0f; a[i].y = 30.0f; a[i].z = 0.0f;
            a[i].r = a[i].g = a[i].b = 180;
        }
        dai_show_formation_add(sh, "One",   "builtin://grid", a, 4, 4.0f);
        dai_show_formation_add(sh, "Two",   "builtin://grid", a, 4, 4.0f);
        dai_show_formation_add(sh, "Three", "builtin://grid", a, 4, 4.0f);
        // Three figures, two steps: 0 and 1 in step 1, the third in step 2.
        dai_show_formation_set_group(sh, 2, 1);
        dai_show_ui *su = dai_show_ui_create(sh);

        auto pframe = [&](float mxp, float myp, int mdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.mouse_down = mdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_figures(su, ui, PX, PY, PW, PH);
            dai_ui_end(ui);
        };
        // Row geometry is not guessed: the rows are found by clicking down the
        // panel until the selection says which one was hit.
        float row_y[3] = { -1.0f, -1.0f, -1.0f };
        for (float yy = PY + 4.0f; yy < PY + PH; yy += 2.0f) {
            dai_show_ui_deselect(su);   // otherwise the FIRST y "hits" whatever
                                        // was selected before the scan began
            pframe(PX + 40.0f, yy, 0);
            pframe(PX + 40.0f, yy, 1);
            pframe(PX + 40.0f, yy, 0);
            if (dai_show_ui_has_selection(su)) {
                uint32_t sel = dai_show_ui_selected_formation(su);
                if (sel < 3 && row_y[sel] < 0.0f) row_y[sel] = yy;
            }
        }
        CHECK(row_y[0] > 0.0f && row_y[1] > 0.0f && row_y[2] > 0.0f,
              "the storyboard did not draw three clickable figure rows");

        // Carry figure 0 down onto the third row, which lives in step 2. It
        // must END there: the step it was dropped into is its parent now.
        dai_show_formation_info before{};
        dai_show_formation_get(sh, 0, &before);
        CHECK(before.group == 0, "the fixture did not start in step 1");
        pframe(PX + 40.0f, row_y[0], 0);
        pframe(PX + 40.0f, row_y[0], 1);          // press: arms the carry
        pframe(PX + 40.0f, row_y[2] + 6.0f, 1);   // move: past the middle
        pframe(PX + 40.0f, row_y[2] + 6.0f, 1);
        pframe(PX + 40.0f, row_y[2] + 6.0f, 0);   // release: the drop
        int found = -1;
        for (uint32_t i = 0; i < 3; ++i) {
            dai_show_formation_info fi{};
            if (dai_show_formation_get(sh, i, &fi) && std::strcmp(fi.name, "One") == 0)
                found = (int)fi.group;
        }
        CHECK(found == 1, "the carried figure landed in step %d, expected step 2",
              found + 1);

        // ...and a plain click still selects rather than moves anything.
        {
            dai_show_formation_info fa{}, fb{};
            dai_show_formation_get(sh, 0, &fa);
            pframe(PX + 40.0f, row_y[1], 0);
            pframe(PX + 40.0f, row_y[1], 1);
            pframe(PX + 40.0f, row_y[1], 0);
            dai_show_formation_get(sh, 0, &fb);
            CHECK(std::strcmp(fa.name, fb.name) == 0,
                  "a click on a row re-ordered the list: %s became %s",
                  fa.name, fb.name);
        }
        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

        std::printf("\n-- show: dragging a keyframe changes the show\n");
        const float VX = 0.0f, VY = 0.0f, VW = 1000.0f, VH = 500.0f;
        dai_show_settings s = dai_show_settings_default();
        s.drone_count = 16; s.min_distance_m = 2.0f; s.fps = 10; s.seed = 3u;
        dai_show *sh = dai_show_create(&s);
        dai_show_point a[16], b[16];
        for (int i = 0; i < 16; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)(i % 4) * 6.0f - 9.0f;
            a[i].y = 40.0f;
            a[i].z = (float)(i / 4) * 6.0f - 9.0f;
            a[i].r = 200; a[i].g = 200; a[i].b = 200; a[i].w = 0;
            b[i] = a[i];
            b[i].y = 60.0f;
        }
        dai_show_formation_add(sh, "Grid", "builtin://grid", a, 16, 6.0f);
        dai_show_formation_add(sh, "Grid up", "builtin://grid", b, 16, 6.0f);
        char serr[256] = { 0 };
        dai_show_solve(sh, serr, sizeof(serr));
        dai_show_ui *su = dai_show_ui_create(sh);
        dai_show_ui_modifiers(su, 0, 0, 0);

        auto frame = [&](float mxp, float myp, int mdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.mouse_down = mdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
        };

        const float TL   = 124.0f < (VH * 0.30f) ? 124.0f : (VH * 0.30f > 58.0f ? VH * 0.30f : 58.0f);
        const float ty   = VY + VH - TL;
        const float TX0  = VX + 6.0f + 54.0f + 12.0f;
        const float TX1  = VX + VW - 10.0f;

        // The show's own duration, the way the strip computes it: holds plus
        // the flight between them.
        dai_show_formation_info f0, f1;
        dai_show_formation_get(sh, 0, &f0);
        dai_show_formation_get(sh, 1, &f1);
        dai_show_transition tr1;
        dai_show_transition_get(sh, 1, &tr1);
        float dur = f0.hold_s + tr1.duration_s + f1.hold_s;
        const dai_show_plan *pl = dai_show_get_plan(sh);
        if (pl && dai_show_plan_duration(pl) > dur) dur = dai_show_plan_duration(pl);
        const float PPS = (TX1 - TX0) / dur;

        // The first figure's bar ends at its hold; the diamond is there, on
        // the first row of the sheet.
        float end_x = TX0 + f0.hold_s * PPS;
        float row_y = ty + 3.0f + 16.0f + 2.0f + 9.0f;      // ruler, then half a row

        float hold_before = f0.hold_s;
        frame(end_x, row_y, 0);
        frame(end_x, row_y, 1);
        frame(end_x + 3.0f * PPS, row_y, 1);
        frame(end_x + 3.0f * PPS, row_y, 0);
        dai_show_formation_get(sh, 0, &f0);
        CHECK(f0.hold_s > hold_before + 2.0f,
              "dragging the hold diamond three seconds to the right made the hold %.2f s, "
              "it was %.2f s", (double)f0.hold_s, (double)hold_before);
        CHECK(dai_show_ui_undo_depth(su) > 0, "the drag pushed no undo step");
        // ONE step for the whole drag, not one per frame.
        CHECK(dai_show_ui_undo_depth(su) == 1,
              "one drag left %u undo steps - a drag is one step",
              dai_show_ui_undo_depth(su));
        dai_show_ui_undo(su);
        dai_show_formation_get(sh, 0, &f0);
        CHECK(std::fabs(f0.hold_s - hold_before) < 1e-3f,
              "undo left the hold at %.2f s, expected %.2f s",
              (double)f0.hold_s, (double)hold_before);

        // A press on the ruler scrubs to the second under the pointer.
        float mid_x = TX0 + (TX1 - TX0) * 0.5f;
        frame(mid_x, ty + 6.0f, 0);
        frame(mid_x, ty + 6.0f, 1);
        frame(mid_x, ty + 6.0f, 0);
        CHECK(std::fabs(dai_show_ui_time(su) - dur * 0.5f) < dur * 0.08f,
              "a click halfway along the ruler seeked to %.2f s of %.2f s",
              (double)dai_show_ui_time(su), (double)dur);

        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

    // ---- moving through a show without the right button ---------------------
    //
    // W A S D used to walk only WHILE the right button was held. On a machine
    // where that button never reaches the program - a touchpad, a shell that
    // eats it, a lost capture - that rule leaves a preview nobody can move
    // through at all, which is exactly what was reported. So: the pointer over
    // the preview and no text field owning the keyboard is enough.
    {
        const float VX = 300.0f, VY = 40.0f, VW = 900.0f, VH = 600.0f;
        dai_show_settings s2 = dai_show_settings_default();
        s2.drone_count = 9; s2.min_distance_m = 2.0f; s2.fps = 10; s2.seed = 5u;
        dai_show *sh = dai_show_create(&s2);
        dai_show_point a[9];
        for (int i = 0; i < 9; ++i) {
            a[i] = dai_show_point{};
            a[i].x = (float)(i % 3) * 6.0f - 6.0f; a[i].y = 40.0f;
            a[i].z = (float)(i / 3) * 6.0f - 6.0f;
            a[i].r = 200; a[i].g = 200; a[i].b = 200;
        }
        dai_show_formation_add(sh, "Grid", "builtin://grid", a, 9, 4.0f);
        char e2[256] = { 0 };
        dai_show_solve(sh, e2, sizeof(e2));
        dai_show_ui *su = dai_show_ui_create(sh);
        dai_show_ui_select_formation(su, 0);

        auto vframe = [&](float mxp, float myp, int rdown) {
            dai_ui_input in{};
            in.mouse_x = mxp; in.mouse_y = myp; in.right_down = rdown;
            dai_ui_begin(ui, 1280, 720, &in);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
        };
        float cx2 = VX + VW * 0.5f, cy2 = VY + VH * 0.4f;
        vframe(cx2, cy2, 0);            // draws once, so the panel knows its rect

        float hx0 = 0.0f, hy0 = 0.0f;
        dai_show_ui_gizmo_handle(su, 0, &hx0, &hy0);
        dai_show_nav_input ni{};
        ni.mouse_x = cx2; ni.mouse_y = cy2;
        // SIDEWAYS, not forward: flying straight at the figure keeps it in the
        // middle of the screen, where a check would see nothing move.
        ni.key_d = 1; ni.can_fly = 1; ni.dt = 1.0f / 60.0f;
        for (int i = 0; i < 10; ++i) dai_show_ui_nav(su, &ni);
        vframe(cx2, cy2, 0);
        float hx1 = 0.0f, hy1 = 0.0f;
        dai_show_ui_gizmo_handle(su, 0, &hx1, &hy1);
        CHECK(std::fabs(hx1 - hx0) + std::fabs(hy1 - hy0) > 1.0f,
              "D did not move the show camera without the right button held");

        // ...and holding W with the pointer OUTSIDE the preview must not.
        vframe(10.0f, 10.0f, 0);
        dai_show_ui_gizmo_handle(su, 0, &hx0, &hy0);
        ni.mouse_x = 10.0f; ni.mouse_y = 10.0f;
        for (int i = 0; i < 10; ++i) dai_show_ui_nav(su, &ni);
        vframe(10.0f, 10.0f, 0);
        dai_show_ui_gizmo_handle(su, 0, &hx1, &hy1);
        CHECK(std::fabs(hx1 - hx0) + std::fabs(hy1 - hy0) < 0.5f,
              "D walked the camera while the pointer was over a panel");

        // The right button reported by the HOST alone - dai_ui never sees it -
        // still looks around. Two paths for one gesture, because the whole
        // complaint was that a right drag did nothing.
        vframe(cx2, cy2, 0);
        dai_show_ui_gizmo_handle(su, 0, &hx0, &hy0);
        dai_show_nav_input li{};
        li.dt = 1.0f / 60.0f; li.mouse_right = 1;
        li.mouse_x = cx2; li.mouse_y = cy2;
        dai_show_ui_nav(su, &li);       // first frame only remembers
        li.mouse_x = cx2 + 120.0f;
        dai_show_ui_nav(su, &li);
        vframe(cx2, cy2, 0);
        dai_show_ui_gizmo_handle(su, 0, &hx1, &hy1);
        CHECK(std::fabs(hx1 - hx0) + std::fabs(hy1 - hy0) > 1.0f,
              "a right drag reported by the host alone did not turn the camera");

        // ---- the right drag turns the HEAD, it does not orbit the figure ----
        //
        // Unity's rule, and the whole complaint: a right drag in a scene view
        // pivots about the EYE. The preview is stored as an orbit, so simply
        // adding to yaw swung the eye around the focus - Blender's gesture on
        // Unity's button. The eye must therefore stand still to the millimetre
        // while the view swings.
        {
            float e0[3] = { 0.0f, 0.0f, 0.0f }, e1[3] = { 0.0f, 0.0f, 0.0f };
            dai_show_ui_eye(su, e0);
            dai_show_nav_input ti{};
            ti.dt = 1.0f / 60.0f; ti.mouse_right = 1;
            ti.mouse_x = cx2; ti.mouse_y = cy2;
            dai_show_ui_nav(su, &ti);            // first frame only remembers
            ti.mouse_x = cx2 + 200.0f; ti.mouse_y = cy2 + 60.0f;
            dai_show_ui_nav(su, &ti);
            dai_show_ui_eye(su, e1);
            float moved = std::fabs(e1[0] - e0[0]) + std::fabs(e1[1] - e0[1]) +
                          std::fabs(e1[2] - e0[2]);
            CHECK(moved < 0.01f,
                  "a right drag moved the eye %.3f m - that is an orbit, not a look",
                  (double)moved);
        }

        // ---- and it turns the RIGHT way -------------------------------------
        // Mouse right = look right. Measured on the view DIRECTION, not on a
        // projected handle: a handle can leave the screen, and then the test
        // is measuring nothing. "Right" is the camera's own right vector,
        // built the way the camera builds it: forward x world-up.
        {
            // A FRESH camera: the checks above have already flown, orbited and
            // pitched this one, and a view that is nearly straight down has no
            // meaningful "right" to measure against.
            dai_show_ui *cu = dai_show_ui_create(sh);
            {   // one frame, so the panel knows the rectangle it draws in -
                // the look path only fires over the preview.
                dai_ui_input ci2{};
                ci2.mouse_x = cx2; ci2.mouse_y = cy2;
                dai_ui_begin(ui, 1280, 720, &ci2);
                dai_show_ui_viewport(cu, ui, VX, VY, VW, VH);
                dai_ui_end(ui);
            }
            float f0[3] = { 0, 0, 0 }, f1[3] = { 0, 0, 0 };
            dai_show_ui_forward(cu, f0);
            float up[3] = { 0.0f, 1.0f, 0.0f };
            float r0[3] = { f0[1] * up[2] - f0[2] * up[1],
                            f0[2] * up[0] - f0[0] * up[2],
                            f0[0] * up[1] - f0[1] * up[0] };
            float rl = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
            if (rl > 1e-6f) { r0[0] /= rl; r0[1] /= rl; r0[2] /= rl; }
            dai_show_nav_input yi{};
            yi.dt = 1.0f / 60.0f; yi.mouse_right = 1;
            yi.mouse_x = cx2; yi.mouse_y = cy2;
            dai_show_ui_nav(cu, &yi);
            yi.mouse_x = cx2 + 40.0f;          // the pointer goes RIGHT
            dai_show_ui_nav(cu, &yi);
            dai_show_ui_forward(cu, f1);
            float d = f1[0] * r0[0] + f1[1] * r0[1] + f1[2] * r0[2];
            CHECK(d > 0.05f,
                  "dragging right turned the view LEFT (%.3f along its own "
                  "right vector): the horizontal look is inverted", (double)d);
            dai_show_ui_destroy(cu);
        }

        // ---- the ground grid survives being flown into ----------------------
        //
        // The old grid was a fixed sheet of 20 m squares drawn out to the
        // fence, and every line with one end behind the eye was thrown away
        // whole - so flying down into it made it fall apart. The measure is
        // the amount of geometry the panel emits: from two metres up there
        // must still be a grid, not a handful of stragglers.
        {
            auto ui_verts = [&]() {
                const dai_ui_draw *d = nullptr;
                uint32_t nb = dai_ui_draws(ui, &d);
                uint32_t tot = 0;
                for (uint32_t k = 0; k < nb; ++k) tot += d[k].count;
                return tot;
            };
            dai_show_ui *gu = dai_show_ui_create(sh);
            dai_ui_input gi{};
            gi.mouse_x = cx2; gi.mouse_y = cy2;
            dai_ui_begin(ui, 1280, 720, &gi);
            dai_show_ui_viewport(gu, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
            uint32_t far_v = ui_verts();

            // Down to eye level, close in: the wheel's own zoom, twenty times.
            for (int k = 0; k < 20; ++k) {
                dai_ui_input wi{};
                wi.mouse_x = cx2; wi.mouse_y = cy2; wi.wheel = 1.0f;
                dai_ui_begin(ui, 1280, 720, &wi);
                dai_show_ui_viewport(gu, ui, VX, VY, VW, VH);
                dai_ui_end(ui);
            }
            dai_ui_begin(ui, 1280, 720, &gi);
            dai_show_ui_viewport(gu, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
            uint32_t near_v = ui_verts();
            CHECK(near_v > far_v / 2u,
                  "the grid collapsed when the camera came close: %u vertices "
                  "from far away, %u from up close", far_v, near_v);
            dai_show_ui_destroy(gu);
        }

        // ---- clicking empty sky selects NOTHING -----------------------------
        {
            dai_show_ui_select_formation(su, 0);
            CHECK(dai_show_ui_has_selection(su), "select_formation selected nothing");
            // A press far from every drone, inside the preview: the top-left
            // corner of the viewport, where a 9-point grid at y=40 is not.
            dai_ui_input ce{};
            ce.mouse_x = VX + 8.0f; ce.mouse_y = VY + 8.0f;
            ce.mouse_down = 1;
            dai_ui_begin(ui, 1280, 720, &ce);
            dai_show_ui_viewport(su, ui, VX, VY, VW, VH);
            dai_ui_end(ui);
            CHECK(!dai_show_ui_has_selection(su),
                  "a click on empty sky kept a selection");
            CHECK(!dai_show_ui_delete_selected(su),
                  "Delete removed a figure while nothing was selected");
        }

        // Every fault goes to the log sink now - there is no Validation panel
        // left to read them in.
        {
            static int lines = 0;
            lines = 0;
            dai_show_ui_log_sink(su, [](void *, int, const char *) { ++lines; }, nullptr);
            dai_show_ui_note(su, 1, "a fault the console must hear about");
            CHECK(lines > 0, "the show wrote nothing to the console sink");
        }
        dai_show_ui_destroy(su);
        dai_show_destroy(sh);
    }

    // ---- 20. the door socket's gizmo, counted as line runs ------------------
    //
    // The socket draws an opening (four segments, one closed loop), a cross on
    // the sill point (two segments) and an arrow along the normal. Seen from
    // the side that is a shaft with two barbs; seen END ON the shaft projects
    // to nothing and the old code drew the two barbs alone - a loose V in the
    // opening, which is what the 1100x700 screenshot showed. Head on it is a
    // ring now, and the proof is geometry rather than a look: every segment of
    // the socket's own colour is recovered from the vertex buffer, the segments
    // are joined into connected runs, and the runs are counted and measured.
    std::printf("door socket gizmo\n");
    {
        const float FW = 1600.0f, FH = 900.0f;
        const uint32_t SEL_COL = 0xFF6FD8FFu;      // the selected socket's colour

        dai_node_desc sd = dai_node_desc_default();
        std::snprintf(sd.name, sizeof(sd.name), "DoorSocket.Front");
        sd.no_body = sd.no_collider = sd.no_rigidbody = 1;
        sd.position = { 0.0f, 0.0f, 0.0f };
        sd.door_socket = 1;
        sd.door_offset = { 0.0f, 0.0f, 0.0f };
        sd.door_normal = { 0.0f, 0.0f, 1.0f };
        sd.door_width = 1.0f;
        sd.door_height = 2.1f;
        dai_node socket = dai_doc_add(doc, &sd);
        dai_doc_sync_apply(sync);
        dai_editor_deselect_all(ed);
        dai_editor_select(ed, socket, 0);
        dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

        struct Seg { float x0, y0, x1, y1; };
        // One dai_ui_line is one quad: six vertices, all of the same colour,
        // two triangles over the corners (a,b,c) and (a,c,d). The segment's
        // ends are the middles of the two short sides - a & d at one end, b & c
        // at the other - so the thickness averages out and what comes back is
        // the line that was asked for.
        auto socket_segments = [&](std::vector<Seg> *out) {
            out->clear();
            const dai_ui_draw *d = nullptr;
            uint32_t nb = dai_ui_draws(ui, &d);
            for (uint32_t i = 0; i < nb; ++i)
                for (uint32_t v = 0; v + 6 <= d[i].count; v += 6) {
                    const dai_ui_vertex *q = d[i].vertices + v;
                    int same = 1;
                    for (int k = 0; k < 6; ++k) if (q[k].color != SEL_COL) same = 0;
                    if (!same) continue;
                    Seg s;
                    s.x0 = (q[0].x + q[5].x) * 0.5f; s.y0 = (q[0].y + q[5].y) * 0.5f;
                    s.x1 = (q[1].x + q[2].x) * 0.5f; s.y1 = (q[1].y + q[2].y) * 0.5f;
                    out->push_back(s);
                }
        };
        // Connected runs: two segments belong to the same run when they share
        // an endpoint. Union-find over the endpoints, 0.75 px apart counting as
        // the same point - the projection is in pixels and the frame's corners
        // are computed twice, once per side.
        auto runs_of = [](const std::vector<Seg> &segs, int *closed_max) {
            std::vector<int> par((int)segs.size());
            for (size_t i = 0; i < par.size(); ++i) par[i] = (int)i;
            std::function<int(int)> find = [&](int a) {
                while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; }
                return a;
            };
            auto near_pt = [](float ax, float ay, float bx, float by) {
                return std::fabs(ax - bx) < 0.75f && std::fabs(ay - by) < 0.75f;
            };
            for (size_t i = 0; i < segs.size(); ++i)
                for (size_t j = i + 1; j < segs.size(); ++j) {
                    const Seg &a = segs[i], &b = segs[j];
                    if (near_pt(a.x0, a.y0, b.x0, b.y0) || near_pt(a.x0, a.y0, b.x1, b.y1) ||
                        near_pt(a.x1, a.y1, b.x0, b.y0) || near_pt(a.x1, a.y1, b.x1, b.y1))
                        par[find((int)i)] = find((int)j);
                }
            std::vector<int> root, size_of;
            for (size_t i = 0; i < segs.size(); ++i) {
                int rt = find((int)i);
                size_t k = 0;
                for (; k < root.size(); ++k) if (root[k] == rt) break;
                if (k == root.size()) { root.push_back(rt); size_of.push_back(0); }
                size_of[k]++;
            }
            if (closed_max) {
                // A run is CLOSED when no endpoint of it is used only once.
                *closed_max = 0;
                for (size_t k = 0; k < root.size(); ++k) {
                    std::vector<float> px, py; std::vector<int> deg;
                    for (size_t i = 0; i < segs.size(); ++i) {
                        if (find((int)i) != root[k]) continue;
                        const float ex[2] = { segs[i].x0, segs[i].x1 };
                        const float ey[2] = { segs[i].y0, segs[i].y1 };
                        for (int e = 0; e < 2; ++e) {
                            size_t m = 0;
                            for (; m < px.size(); ++m)
                                if (std::fabs(px[m] - ex[e]) < 0.75f &&
                                    std::fabs(py[m] - ey[e]) < 0.75f) break;
                            if (m == px.size()) { px.push_back(ex[e]); py.push_back(ey[e]); deg.push_back(0); }
                            deg[m]++;
                        }
                    }
                    int open_end = 0;
                    for (size_t m = 0; m < deg.size(); ++m) if (deg[m] < 2) open_end = 1;
                    if (!open_end && size_of[k] > *closed_max) *closed_max = size_of[k];
                }
            }
            return (int)root.size();
        };
        auto shoot = [&](dai_vec3 eye, std::vector<Seg> *out) {
            dai_editor_camera(ed, eye, dai_vec3{ 0.0f, 1.05f, 0.0f }, dai_vec3{ 0, 1, 0 },
                              55.0f, 0.05f, 200.0f, FW, FH);
            dai_ui_input in{};
            in.mouse_x = -100.0f; in.mouse_y = -100.0f;
            dai_ui_begin(ui, FW, FH, &in);
            dai_editor_ui_frame(panels, FW, FH);
            dai_ui_end(ui);
            float vx = 0, vy = 0, vw = 0, vh = 0;
            dai_editor_ui_viewport_rect(panels, &vx, &vy, &vw, &vh);
            dai_editor_camera_viewport_rect(ed, vx, vy, vw, vh);
            dai_ui_begin(ui, FW, FH, &in);
            dai_editor_ui_frame(panels, FW, FH);
            dai_ui_end(ui);
            socket_segments(out);
        };

        // From the side: shaft plus two barbs, all three meeting at the tip.
        std::vector<Seg> side;
        shoot(dai_vec3{ 5.0f, 1.6f, 5.0f }, &side);
        int side_closed = 0;
        int side_runs = runs_of(side, &side_closed);
        CHECK((int)side.size() == 9,
              "the socket drew %d segments from the side, expected 9 (frame 4, cross 2, arrow 3)",
              (int)side.size());
        CHECK(side_runs == 4,
              "the socket drew %d connected runs from the side, expected 4 "
              "(the opening, two cross arms, the arrow)", side_runs);
        CHECK(side_closed == 4,
              "the opening is not a closed loop from the side (largest closed run: %d segments)",
              side_closed);

        // Head on: the arrow would be a dot, so it is a ring - and a ring is a
        // closed run of its own, longer than the four sided opening.
        std::vector<Seg> front;
        shoot(dai_vec3{ 0.0f, 1.05f, 6.0f }, &front);
        int front_closed = 0;
        int front_runs = runs_of(front, &front_closed);
        CHECK((int)front.size() == 30,
              "the socket drew %d segments head on, expected 30 (frame 4, cross 2, ring 24)",
              (int)front.size());
        CHECK(front_runs == 4,
              "the socket drew %d connected runs head on, expected 4 "
              "(the opening, two cross arms, the ring)", front_runs);
        CHECK(front_closed == 24,
              "head on the arrow is not a closed ring (largest closed run: %d segments)",
              front_closed);
        // The symptom the ring replaces: a two segment run with a free end at
        // each side of it - a barb pair with no shaft between them.
        int stub_runs = 0;
        for (size_t i = 0; i < front.size(); ++i) {
            float len = std::sqrt((front[i].x1 - front[i].x0) * (front[i].x1 - front[i].x0) +
                                  (front[i].y1 - front[i].y0) * (front[i].y1 - front[i].y0));
            if (len < 1.0f) ++stub_runs;
        }
        CHECK(stub_runs == 0, "%d of the socket's segments are shorter than a pixel head on",
              stub_runs);
        std::printf("  socket gizmo: %d segments / %d runs from the side, %d / %d head on\n",
                    (int)side.size(), side_runs, (int)front.size(), front_runs);

        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, socket);
        dai_doc_sync_apply(sync);
        dai_editor_camera(ed, dai_vec3{ 0, 3, 10 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                          55.0f, 0.1f, 200.0f, 1280.0f, 720.0f);
        dai_editor_camera_viewport_rect(ed, 0.0f, 0.0f, 1280.0f, 720.0f);
    }

    // ---- a hierarchy name too wide for its panel ends in an ellipsis -------
    //
    // The row used to draw the full name and leave the panel's clip rectangle
    // to cut it: the 1100x700 screenshot showed "DoorSocket.Fro", which is not
    // an abbreviation but a different name, and nothing on the row said there
    // was more. The proof is in the GLYPHS - the last three quads of the row
    // carry the atlas rectangle of '.', which is what an ellipsis is made of -
    // and the whole name comes back on the row's tooltip under the pointer.
    std::printf("hierarchy ellipsis\n");
    {
        const float NARROW_W = 155.0f;                 // the panel that is too small
        const float HX = 8.0f, HY = 60.0f, HH = 360.0f;
        dai_node_desc dd = dai_node_desc_default();
        std::snprintf(dd.name, sizeof(dd.name), "Wall.Front.Doorway");
        dd.no_body = dd.no_collider = dd.no_rigidbody = 1;
        dd.position = { 0, 0, -6 };
        dai_node doorway = dai_doc_add(doc, &dd);
        dai_doc_sync_apply(sync);

        const dai_glyph *dot = dai_font_glyph(font, '.');
        CHECK(dot != nullptr && dot->x1 > dot->x0, "the font has no '.' to make an ellipsis of");

        // Every glyph quad whose atlas rectangle is the one '.' sits in, as the
        // top left corner of the quad: six vertices per glyph, so the corner is
        // taken once per glyph rather than once per vertex.
        struct Dot { float x, y; };
        auto dots_at = [&](float panel_w, float mouse_x, float mouse_y) {
            dai_ui_input in{};
            in.mouse_x = mouse_x; in.mouse_y = mouse_y;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_editor_ui_hierarchy(panels, HX, HY, panel_w, HH);
            dai_ui_end(ui);
            std::vector<Dot> out;
            const dai_ui_draw *dr = nullptr;
            uint32_t nbb = dai_ui_draws(ui, &dr);
            for (uint32_t b = 0; b < nbb; ++b)
                for (uint32_t v = 0; v + 5 < dr[b].count; v += 6) {
                    float ux = 1e9f, uy = 1e9f, qx = 1e9f, qy = 1e9f;
                    for (uint32_t k = 0; k < 6; ++k) {
                        const dai_ui_vertex &p = dr[b].vertices[v + k];
                        if (p.u < ux) ux = p.u;
                        if (p.v < uy) uy = p.v;
                        if (p.x < qx) qx = p.x;
                        if (p.y < qy) qy = p.y;
                    }
                    if (std::fabs(ux - dot->u0) < 1e-6f && std::fabs(uy - dot->v0) < 1e-6f)
                        out.push_back(Dot{ qx, qy });
                }
            return out;
        };
        // Three dots on ONE row, side by side and evenly spaced - an ellipsis,
        // and not the two dots the name itself is spelled with.
        auto ellipsis_runs = [&](const std::vector<Dot> &v, float *row_y) {
            int runs = 0;
            for (size_t i = 0; i + 2 < v.size(); ++i) {
                if (std::fabs(v[i + 1].y - v[i].y) > 0.5f) continue;
                if (std::fabs(v[i + 2].y - v[i].y) > 0.5f) continue;
                float s1 = v[i + 1].x - v[i].x, s2 = v[i + 2].x - v[i + 1].x;
                if (s1 <= 0.0f || std::fabs(s2 - s1) > 0.5f) continue;
                if (s1 > dot->advance + 0.5f) continue;
                ++runs;
                if (row_y) *row_y = v[i].y;
            }
            return runs;
        };

        // The sections above left dozens of nodes in this document, and a row
        // below the fold is not drawn at all - dai_ui_text drops a line whose
        // ink crosses the clip. The new node is the newest, so it is the LAST
        // row: the tree is wheeled to its end before anything is measured, the
        // same way a user would reach it.
        for (int k = 0; k < 40; ++k) {
            dai_ui_input in{};
            in.mouse_x = HX + NARROW_W * 0.5f; in.mouse_y = HY + 120.0f;
            in.wheel = -8.0f;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_editor_ui_hierarchy(panels, HX, HY, NARROW_W, HH);
            dai_ui_end(ui);
        }
        // Two frames per measurement: the first lays the panel out, the second
        // draws it with that layout - the two-pass shape the shot tools use.
        dots_at(NARROW_W, -100.0f, -100.0f);
        std::vector<Dot> narrow = dots_at(NARROW_W, -100.0f, -100.0f);
        float run_y = -1.0f;
        int runs = ellipsis_runs(narrow, &run_y);
        CHECK(runs >= 1,
              "no row of a %.0f px hierarchy ends in '...' - 'Wall.Front.Doorway' is "
              "still cut by the clip rectangle (%u dot glyphs on the whole panel)",
              (double)NARROW_W, (unsigned)narrow.size());

        // ...and it is the WIDTH that shortens it, not the name: given room the
        // same row draws the name whole and no ellipsis run is left.
        dots_at(320.0f, -100.0f, -100.0f);
        std::vector<Dot> wide = dots_at(320.0f, -100.0f, -100.0f);
        CHECK(ellipsis_runs(wide, nullptr) == 0,
              "a 320 px hierarchy shortens a name that fits: %d ellipsis runs",
              ellipsis_runs(wide, nullptr));

        // Nothing is drawn past the panel edge either: an ellipsis that still
        // overflows has shortened nothing.
        {
            dai_ui_input in{};
            in.mouse_x = -100.0f; in.mouse_y = -100.0f;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_editor_ui_hierarchy(panels, HX, HY, NARROW_W, HH);
            dai_ui_end(ui);
            float bx0, by0, bx1, by1;
            vert_bounds(ui, &bx0, &by0, &bx1, &by1);
            CHECK(bx1 <= HX + NARROW_W + 0.5f,
                  "the hierarchy drew out to x=%.1f, %.1f px past its own panel",
                  (double)bx1, (double)(bx1 - (HX + NARROW_W)));
        }

        // The tooltip: hovering the shortened row spells the whole name out,
        // which is where the two dots of "Wall.Front.Doorway" come back - so
        // the hovered frame carries at least two dot glyphs more than the
        // unhovered one.
        if (run_y >= 0.0f) {
            dots_at(NARROW_W, HX + NARROW_W * 0.5f, run_y + 4.0f);
            std::vector<Dot> hovered = dots_at(NARROW_W, HX + NARROW_W * 0.5f, run_y + 4.0f);
            CHECK(hovered.size() >= narrow.size() + 2,
                  "hovering the shortened row added %d dot glyphs - the full name is "
                  "not on a tooltip", (int)hovered.size() - (int)narrow.size());
        }
        dai_doc_remove(doc, doorway);
        dai_doc_sync_apply(sync);
    }

    // ---- the inspector header: the name field never reaches "Static" -------
    //
    // The name box was laid out from a 60 px floor, and in the 200 px
    // inspector of a 1100x700 window that floor is wider than the gap the
    // header has - so the field was drawn straight over the Static checkbox
    // and two clickable things sat on the same pixels. Measured on the
    // rectangles the header emits: the widgets are the track coloured fills
    // inside the 34 px card, grouped by the gaps between them, and the name
    // field has to end before the checkbox starts.
    std::printf("inspector header\n");
    {
        dai_node_desc hd = dai_node_desc_default();
        std::snprintf(hd.name, sizeof(hd.name), "Wall.Front.Doorway.Lintel.Left");
        hd.no_body = hd.no_collider = hd.no_rigidbody = 1;
        dai_node hn = dai_doc_add(doc, &hd);
        dai_doc_sync_apply(sync);
        dai_editor_select(ed, hn, 0);

        const float IW = 200.0f;                     // the inspector at 1100x700
        const float IX = 1100.0f - IW - 8.0f, IY = 60.0f;
        auto header_frame = [&]() {
            dai_ui_input in{};
            in.mouse_x = -100.0f; in.mouse_y = -100.0f;
            dai_ui_begin(ui, 1100, 700, &in);
            dai_editor_ui_expand_all(panels);
            dai_editor_ui_inspector(panels, IX, IY, IW, 592.0f);
            dai_ui_end(ui);
        };
        header_frame();
        header_frame();

        const uint32_t CARD = 0xFF3A3A3Au;           // the header plate's own colour
        float cy0 = -1.0f, cy1 = -1.0f;
        const dai_ui_draw *dr = nullptr;
        uint32_t nbb = dai_ui_draws(ui, &dr);
        for (uint32_t b = 0; b < nbb && cy0 < 0.0f; ++b)
            for (uint32_t v = 0; v < dr[b].count; ++v)
                if (dr[b].vertices[v].color == CARD) {
                    cy0 = dr[b].vertices[v].y; cy1 = cy0 + 34.0f; break;
                }
        CHECK(cy0 >= 0.0f, "the inspector drew no object header card");

        // Every track coloured QUAD in the card, and then those that touch
        // merged into one widget - a rounded rectangle is a middle piece plus
        // four corner slivers, and a rectangle only has vertices at its
        // corners, so grouping raw vertices by their gaps would cut a 14 px
        // checkbox in half down the middle. What comes out is the active box,
        // the icon plate, the name field and the Static checkbox.
        struct Box { float x0, x1, y0, y1; };
        std::vector<Box> boxes;
        if (cy0 >= 0.0f) {
            for (uint32_t b = 0; b < nbb; ++b)
                for (uint32_t v = 0; v + 5 < dr[b].count; v += 6) {
                    Box q{ 1e9f, -1e9f, 1e9f, -1e9f };
                    int mine = 1;
                    for (uint32_t k = 0; k < 6; ++k) {
                        const dai_ui_vertex &p = dr[b].vertices[v + k];
                        if (p.color != style->track) { mine = 0; break; }
                        if (p.x < q.x0) q.x0 = p.x;
                        if (p.x > q.x1) q.x1 = p.x;
                        if (p.y < q.y0) q.y0 = p.y;
                        if (p.y > q.y1) q.y1 = p.y;
                    }
                    if (!mine) continue;
                    if (q.y0 < cy0 - 0.5f || q.y1 > cy1 + 0.5f) continue;
                    boxes.push_back(q);
                }
            for (int again = 1; again;) {
                again = 0;
                for (size_t i = 0; i < boxes.size() && !again; ++i)
                    for (size_t j = i + 1; j < boxes.size() && !again; ++j) {
                        if (boxes[i].x0 > boxes[j].x1 + 0.5f) continue;
                        if (boxes[j].x0 > boxes[i].x1 + 0.5f) continue;
                        if (boxes[i].y0 > boxes[j].y1 + 0.5f) continue;
                        if (boxes[j].y0 > boxes[i].y1 + 0.5f) continue;
                        if (boxes[j].x0 < boxes[i].x0) boxes[i].x0 = boxes[j].x0;
                        if (boxes[j].x1 > boxes[i].x1) boxes[i].x1 = boxes[j].x1;
                        if (boxes[j].y0 < boxes[i].y0) boxes[i].y0 = boxes[j].y0;
                        if (boxes[j].y1 > boxes[i].y1) boxes[i].y1 = boxes[j].y1;
                        boxes.erase(boxes.begin() + (long)j);
                        again = 1;
                    }
            }
        }
        // The name field is the 18 px tall one, the Static checkbox the
        // rightmost 14 px square.
        int name_i = -1, static_i = -1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            float bh = boxes[i].y1 - boxes[i].y0, bw = boxes[i].x1 - boxes[i].x0;
            if (bh > 16.5f && bh < 20.0f && bw > 8.0f) name_i = (int)i;
            if (bh > 12.5f && bh < 15.5f && bw > 12.5f && bw < 15.5f) static_i = (int)i;
        }
        CHECK(name_i >= 0, "the header drew no name field (%u track boxes)",
              (unsigned)boxes.size());
        CHECK(static_i >= 0, "the header drew no Static checkbox (%u track boxes)",
              (unsigned)boxes.size());
        if (name_i >= 0 && static_i >= 0) {
            CHECK(boxes[name_i].x1 <= boxes[static_i].x0 + 0.5f,
                  "at 1100x700 the name field runs to x=%.1f and the Static checkbox "
                  "starts at x=%.1f - they overlap by %.1f px",
                  (double)boxes[name_i].x1, (double)boxes[static_i].x0,
                  (double)(boxes[name_i].x1 - boxes[static_i].x0));
            CHECK(boxes[name_i].x1 <= IX + IW - style->padding + 0.5f,
                  "the name field ends at x=%.1f, past the %.0f px panel",
                  (double)boxes[name_i].x1, (double)IW);
            std::printf("  header at %.0f px: name field %.1f..%.1f, Static at %.1f\n",
                        (double)IW, (double)boxes[name_i].x0, (double)boxes[name_i].x1,
                        (double)boxes[static_i].x0);

            // ...and the name INSIDE it is clipped to the field rather than
            // painted across the checkbox. A name longer than the box still
            // emits its glyphs - the text is laid out from the left edge and
            // does not stop at the right one - so what is measured is the INK
            // THAT SURVIVES: a vertex past its batch's clip rectangle is a
            // vertex the scissor never lets through, which is what the field
            // now wraps itself in.
            // A GLYPH, not a fill: every rectangle in this interface is drawn
            // with a single solid texel of the same atlas, so "has texture
            // coordinates" is not the question - "do they SPAN anything" is.
            // The tick inside the Static checkbox is a fill in exactly the
            // text colour, and counting it as a letter would fail this check
            // no matter what the name field does.
            float ink_x1 = -1e9f;
            for (uint32_t b = 0; b < nbb; ++b)
                for (uint32_t v = 0; v + 5 < dr[b].count; v += 6) {
                    float qx1 = -1e9f, qy0 = 1e9f, qy1 = -1e9f, u0 = 1e9f, u1 = -1e9f;
                    int mine = 1;
                    for (uint32_t k = 0; k < 6; ++k) {
                        const dai_ui_vertex &p = dr[b].vertices[v + k];
                        if (p.color != style->text) { mine = 0; break; }
                        if (p.x > qx1) qx1 = p.x;
                        if (p.y < qy0) qy0 = p.y;
                        if (p.y > qy1) qy1 = p.y;
                        if (p.u < u0) u0 = p.u;
                        if (p.u > u1) u1 = p.u;
                    }
                    if (!mine || u1 - u0 < 1e-6f) continue;        // a fill, not a letter
                    if (qy0 < cy0 - 0.5f || qy1 > cy1 + 0.5f) continue;
                    if (qx1 < boxes[name_i].x0 - 0.5f) continue;   // left of the field
                    float eff = qx1 < dr[b].clip[2] ? qx1 : dr[b].clip[2];
                    if (eff > ink_x1) ink_x1 = eff;
                }
            CHECK(ink_x1 > boxes[name_i].x0,
                  "the name field drew no text at all (ink to x=%.1f)", (double)ink_x1);
            CHECK(ink_x1 <= boxes[static_i].x0 + 0.5f,
                  "a long name is drawn out to x=%.1f, over the Static checkbox at "
                  "x=%.1f", (double)ink_x1, (double)boxes[static_i].x0);
        }
        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, hn);
        dai_doc_sync_apply(sync);
    }

    modifier_stack_cases(doc, sync, ed, panels, ui);

    dai_editor_ui_destroy(panels);
    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);
    dai_ui_destroy(ui);
    dai_font_free(font);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
