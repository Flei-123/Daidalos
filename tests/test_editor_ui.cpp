// Editor panels: hierarchy, inspector, toolbar, viewport routing.
//
//   ./build/test_editor_ui
//
// No renderer needed - the UI produces triangles, and that is what is checked.

#include "dai_editor_ui.h"
#include <cstdio>
#include <cmath>
#include <cstring>
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

    // click the fold arrow of the parent row. Rows start below the panel title.
    float row_x = PANEL_X + PAD + 6.0f;              // panel x + arrow column
    float row_y = FIRST_ROW_Y + ROW_H * 0.5f;
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
    float label_x = PANEL_X + PAD + 30.0f;           // past the arrow column
    frame(label_x, row_y, 1);
    frame(label_x, row_y, 0);
    CHECK(dai_editor_selection_count(ed) == 1, "clicking a row did not select it");
    CHECK(dai_editor_selected(ed, 0) == parent, "the wrong row got selected");

    // ---- 4. the inspector edits the document ------------------------------
    std::printf("inspector\n");
    dai_node_desc before{};
    dai_doc_get(doc, parent, &before);

    // Find the Position row by trying each row rather than hard coding a pixel
    // offset: the test would then only be checking my arithmetic, and it would
    // break every time a field is added above it.
    const float FIELD_X = INSPECTOR_X + PAD + style->label_w + 8.0f;   // past the label column
    float pos_y = -1.0f;
    int rows_that_moved_x = 0;
    for (float y = 90.0f; y < 400.0f; y += 4.0f) {
        dai_node_desc a{}, b2{};
        dai_doc_get(doc, parent, &a);
        uint32_t depth_before = dai_editor_undo_depth(ed);
        frame(FIELD_X, y, 0);
        frame(FIELD_X, y, 1);
        frame(FIELD_X + 12.0f, y, 1);
        frame(FIELD_X + 12.0f, y, 0);
        dai_doc_get(doc, parent, &b2);
        if (std::fabs(b2.position.x - a.position.x) > 1e-6f) {
            if (pos_y < 0.0f) pos_y = y;
            ++rows_that_moved_x;
        }
        while (dai_editor_undo_depth(ed) > depth_before) dai_editor_undo(ed);
    }
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

    // Clicking empty space clears the selection.
    frame(1000, 690, 0);
    dai_editor_ui_viewport_input(panels, 20, 700, 1);
    dai_editor_ui_viewport_input(panels, 20, 700, 0);
    CHECK(dai_editor_selection_count(ed) == 0, "clicking empty space did not clear the selection");

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
