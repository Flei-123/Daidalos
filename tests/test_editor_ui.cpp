// Editor panels: hierarchy, inspector, toolbar, viewport routing.
//
//   ./build/test_editor_ui
//
// No renderer needed - the UI produces triangles, and that is what is checked.

#include "dai_editor_ui.h"
#include "dai_show_ui.h"
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
