// Editor core: picking, selection, gizmo maths, drags, undo.
//
//   ./build/test_editor
//
// The editor edits the document, so undo assertions here are really assertions
// that the document layer is wired up correctly.

#include "dai_editor.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool near3(dai_vec3 a, dai_vec3 b, float eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}
static dai_vec3 world_of(dai_doc *d, dai_node n) {
    dai_vec3 p{};
    dai_doc_world_transform(d, n, &p, nullptr, nullptr);
    return p;
}

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1; cfg.seed = 3;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world creation failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    std::printf("editor core\n");

    // three cubes in a row, so a pick has an unambiguous right answer
    dai_node cubes[3];
    for (int i = 0; i < 3; ++i) {
        dai_node_desc r = dai_node_desc_default();
        r.motion = DAI_KINEMATIC;         // movable by the editor, not by gravity
        r.half_extent = { 0.5f, 0.5f, 0.5f };
        r.position = { -3.0f + (float)i * 3.0f, 0.0f, 0.0f };
        snprintf(r.name, sizeof(r.name), "cube%d", i);
        cubes[i] = dai_doc_add(doc, &r);
    }
    dai_doc_sync_apply(sync);
    dai_step(w);

    dai_editor *ed = dai_editor_create(doc, sync);
    CHECK(ed != nullptr, "editor creation failed");
    // camera looking down -Z from +Z, so screen x maps to world x
    dai_editor_camera(ed, dai_vec3{ 0, 0, 12 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);

    // ---- 1. rays ----------------------------------------------------------
    dai_vec3 o, d;
    dai_editor_ray(ed, 400, 300, &o, &d);
    CHECK(std::fabs(d.x) < 1e-4f && std::fabs(d.y) < 1e-4f && d.z < -0.99f,
          "centre ray is (%.3f %.3f %.3f), expected (0 0 -1)", d.x, d.y, d.z);
    dai_editor_ray(ed, 800, 300, &o, &d);
    CHECK(d.x > 0.2f, "the ray at the right edge does not point right (x %.3f)", d.x);

    // project must be the exact inverse of ray, or the gizmo hit test and the
    // drawn gizmo would sit in different places
    float px = 0, py = 0;
    CHECK(dai_editor_project(ed, dai_vec3{ 0, 0, 0 }, &px, &py) &&
          std::fabs(px - 400) < 0.5f && std::fabs(py - 300) < 0.5f,
          "the origin projects to (%.1f %.1f), expected (400 300)", px, py);
    dai_editor_ray(ed, 620, 210, &o, &d);
    dai_vec3 far_pt = { o.x + d.x * 12.0f, o.y + d.y * 12.0f, o.z + d.z * 12.0f };
    CHECK(dai_editor_project(ed, far_pt, &px, &py) &&
          std::fabs(px - 620) < 0.5f && std::fabs(py - 210) < 0.5f,
          "ray/project round trip gave (%.1f %.1f), expected (620 210)", px, py);
    CHECK(dai_editor_project(ed, dai_vec3{ 0, 0, 100 }, &px, &py) == 0,
          "a point behind the camera reported as visible");

    // ---- 2. picking -------------------------------------------------------
    dai_node hit = dai_editor_pick(ed, 400, 300);
    CHECK(hit == cubes[1], "centre pick got node %u, expected the middle cube %u", hit, cubes[1]);
    CHECK(dai_editor_pick(ed, 20, 580) == DAI_INVALID_NODE, "a pick into empty space found something");

    // ---- 3. selection -----------------------------------------------------
    dai_editor_select(ed, cubes[0], 0);
    CHECK(dai_editor_selection_count(ed) == 1, "select failed");
    dai_editor_select(ed, cubes[2], 1);
    CHECK(dai_editor_selection_count(ed) == 2, "additive select failed");
    CHECK(dai_editor_is_selected(ed, cubes[2]), "is_selected is wrong");
    dai_editor_select(ed, cubes[2], 1);
    CHECK(dai_editor_selection_count(ed) == 1, "additive select must toggle an already selected node");
    dai_editor_select(ed, cubes[2], 1);
    CHECK(near3(dai_editor_selection_center(ed), dai_vec3{ 0, 0, 0 }),
          "the centre of cubes at -3 and +3 should be the origin");
    dai_editor_deselect_all(ed);
    CHECK(dai_editor_selection_count(ed) == 0, "deselect_all failed");

    // ---- 4. gizmo ---------------------------------------------------------
    std::printf("gizmo\n");
    dai_editor_select(ed, cubes[1], 0);
    float g_near = dai_editor_gizmo_scale(ed);
    CHECK(g_near > 0.0f, "gizmo scale is zero with a selection");
    // twice as far away must mean twice the world size, so it stays the same
    // number of pixels on screen
    dai_editor_camera(ed, dai_vec3{ 0, 0, 24 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);
    float g_far = dai_editor_gizmo_scale(ed);
    CHECK(std::fabs(g_far - 2.0f * g_near) < 1e-3f,
          "gizmo is not screen constant: %.4f at 12m, %.4f at 24m", g_near, g_far);
    dai_editor_camera(ed, dai_vec3{ 0, 0, 12 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);

    uint32_t nlines = dai_editor_gizmo_lines(ed, nullptr, 0);
    CHECK(nlines > 3, "translate gizmo has only %u lines", nlines);
    std::vector<dai_gizmo_line> lines(nlines);
    dai_editor_gizmo_lines(ed, lines.data(), nlines);
    int seen_x = 0, seen_y = 0, seen_z = 0, seen_plane = 0;
    for (const dai_gizmo_line &l : lines) {
        if (l.axis == DAI_AXIS_X) seen_x = 1;
        if (l.axis == DAI_AXIS_Y) seen_y = 1;
        if (l.axis == DAI_AXIS_Z) seen_z = 1;
        if (l.axis >= DAI_AXIS_XY && l.axis <= DAI_AXIS_YZ) seen_plane = 1;
    }
    CHECK(seen_x && seen_y && seen_z && seen_plane, "the gizmo is missing handles");

    // clicking a point along the projected X axis must select the X handle
    dai_vec3 centre = dai_editor_selection_center(ed);
    float len = dai_editor_gizmo_scale(ed);
    dai_vec3 on_x = { centre.x + len * 0.75f, centre.y, centre.z };
    CHECK(dai_editor_project(ed, on_x, &px, &py), "projection of the X handle failed");
    CHECK(dai_editor_gizmo_hit(ed, px, py) == DAI_AXIS_X,
          "clicking the X arm hit %d instead of X", dai_editor_gizmo_hit(ed, px, py));
    dai_vec3 on_y = { centre.x, centre.y + len * 0.75f, centre.z };
    dai_editor_project(ed, on_y, &px, &py);
    CHECK(dai_editor_gizmo_hit(ed, px, py) == DAI_AXIS_Y, "clicking the Y arm did not hit Y");
    CHECK(dai_editor_gizmo_hit(ed, 10, 590) == DAI_AXIS_NONE, "a click far away hit a handle");

    // the XY plane handle sits between the arms
    dai_vec3 on_xy = { centre.x + len * 0.375f, centre.y + len * 0.375f, centre.z };
    dai_editor_project(ed, on_xy, &px, &py);
    CHECK(dai_editor_gizmo_hit(ed, px, py) == DAI_AXIS_XY,
          "the XY plane handle was not hit (got %d)", dai_editor_gizmo_hit(ed, px, py));

    dai_editor_gizmo_hover(ed, px, py);
    CHECK(dai_editor_gizmo_hovered(ed) == DAI_AXIS_XY, "hover was not recorded");

    // rotate mode draws rings, not arrows
    dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
    uint32_t rot_lines = dai_editor_gizmo_lines(ed, nullptr, 0);
    CHECK(rot_lines >= 90, "rotate gizmo has %u lines, expected three rings", rot_lines);
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

    // ---- 5. dragging ------------------------------------------------------
    std::printf("drags\n");
    dai_editor_select(ed, cubes[1], 0);
    dai_vec3 start = world_of(doc, cubes[1]);
    uint32_t undo_before = dai_editor_undo_depth(ed);

    // drag along X in the plane z = 0: the world delta is exactly what the
    // pixels under the cursor say it is
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    CHECK(dai_editor_dragging(ed), "drag did not start");
    dai_editor_drag_update(ed, 440, 300);
    dai_editor_drag_update(ed, 480, 300);
    dai_vec3 mid = world_of(doc, cubes[1]);
    CHECK(mid.x > start.x + 0.1f, "dragging right did not move the cube right (%.3f -> %.3f)",
          start.x, mid.x);
    CHECK(std::fabs(mid.y - start.y) < 1e-4f && std::fabs(mid.z - start.z) < 1e-4f,
          "an X drag moved the cube off its axis");
    dai_editor_drag_end(ed);
    CHECK(!dai_editor_dragging(ed), "drag did not end");
    CHECK(dai_editor_undo_depth(ed) == undo_before + 1,
          "a drag of two updates produced %u undo steps, expected 1",
          dai_editor_undo_depth(ed) - undo_before);
    CHECK(std::strcmp(dai_editor_undo_name(ed), "Move") == 0,
          "undo step is named '%s', expected 'Move'", dai_editor_undo_name(ed));

    // the physics body followed
    dai_transform t{};
    dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, cubes[1])), &t);
    CHECK(near3(t.position, mid, 1e-3f), "the body did not follow the drag (%.3f vs %.3f)",
          t.position.x, mid.x);

    CHECK(dai_editor_undo(ed) == 1, "undo failed");
    CHECK(near3(world_of(doc, cubes[1]), start), "undo did not restore the position");
    CHECK(dai_editor_redo(ed) == 1, "redo failed");
    CHECK(near3(world_of(doc, cubes[1]), mid), "redo did not reapply the move");
    dai_editor_undo(ed);

    // snapping rounds the delta, not the position: a cube that already sits off
    // grid must not jump onto it
    dai_node_desc rec{};
    dai_doc_get(doc, cubes[1], &rec);
    rec.position = { 0.37f, 0, 0 };
    dai_doc_set(doc, cubes[1], &rec);
    dai_editor_snap(ed, 1.0f, 45.0f, 0.0f);
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    dai_editor_drag_update(ed, 460, 300);
    float snapped = world_of(doc, cubes[1]).x;
    dai_editor_drag_end(ed);
    CHECK(std::fabs(snapped - std::round(snapped)) > 0.3f,
          "snapping moved an off grid cube onto the grid (x %.3f)", snapped);
    CHECK(std::fabs((snapped - 0.37f) - std::round(snapped - 0.37f)) < 1e-3f,
          "the delta %.3f is not a whole number of grid steps", snapped - 0.37f);
    dai_editor_snap(ed, 0, 0, 0);

    // cancel restores the start state and leaves no undo step
    undo_before = dai_editor_undo_depth(ed);
    dai_vec3 before_cancel = world_of(doc, cubes[1]);
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    dai_editor_drag_update(ed, 600, 300);
    CHECK(!near3(world_of(doc, cubes[1]), before_cancel), "the drag did not move anything");
    dai_editor_drag_cancel(ed);
    CHECK(near3(world_of(doc, cubes[1]), before_cancel), "cancel did not restore the position");
    CHECK(dai_editor_undo_depth(ed) == undo_before, "cancel left an undo step behind");

    // ---- 6. rotate --------------------------------------------------------
    dai_doc_get(doc, cubes[1], &rec);
    rec.position = { 0, 0, 0 };
    rec.rotation = { 0, 0, 0, 1 };
    dai_doc_set(doc, cubes[1], &rec);
    dai_editor_gizmo_mode(ed, DAI_GIZMO_ROTATE);
    // Look down at the scene: a Y ring is unusable from a camera sitting
    // exactly in its plane, which is the next test.
    dai_editor_camera(ed, dai_vec3{ 0, 9, 12 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);
    len = dai_editor_gizmo_scale(ed);
    // grab the Y ring at +X and drag round to +Z: a quarter turn
    float sx, sy2, ex, ey;
    dai_editor_project(ed, dai_vec3{ len, 0, 0 }, &sx, &sy2);
    dai_editor_project(ed, dai_vec3{ 0, 0, len }, &ex, &ey);
    dai_editor_drag_begin(ed, DAI_AXIS_Y, sx, sy2);
    CHECK(dai_editor_dragging(ed), "the rotate drag did not start");
    dai_editor_drag_update(ed, ex, ey);
    dai_editor_drag_end(ed);
    dai_doc_get(doc, cubes[1], &rec);
    // 90 degrees about Y -> quaternion (0, +-0.7071, 0, 0.7071)
    CHECK(std::fabs(std::fabs(rec.rotation.y) - 0.7071f) < 0.02f &&
          std::fabs(std::fabs(rec.rotation.w) - 0.7071f) < 0.02f,
          "rotate drag gave (%.3f %.3f %.3f %.3f), expected a 90 degree Y turn",
          rec.rotation.x, rec.rotation.y, rec.rotation.z, rec.rotation.w);
    CHECK(std::strcmp(dai_editor_undo_name(ed), "Rotate") == 0, "rotate step is misnamed");
    dai_editor_undo(ed);
    dai_doc_get(doc, cubes[1], &rec);
    CHECK(std::fabs(rec.rotation.w - 1.0f) < 1e-3f, "undo did not restore the rotation");

    // a ring seen exactly edge-on still has to be usable: the camera sits in
    // the y=0 plane here, so no ray ever meets the Y ring and the editor falls
    // back to the angle swept on screen
    dai_editor_camera(ed, dai_vec3{ 0, 0, 12 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);
    dai_editor_drag_begin(ed, DAI_AXIS_Y, 500, 300);
    CHECK(dai_editor_dragging(ed), "an edge-on ring refused the drag");
    dai_editor_drag_update(ed, 400, 200);          // a quarter sweep on screen
    dai_editor_drag_end(ed);
    dai_doc_get(doc, cubes[1], &rec);
    CHECK(std::fabs(rec.rotation.w - 1.0f) > 1e-3f, "the edge-on fallback rotated nothing");
    dai_editor_undo(ed);

    // ---- 7. scale ---------------------------------------------------------
    dai_editor_gizmo_mode(ed, DAI_GIZMO_SCALE);
    len = dai_editor_gizmo_scale(ed);
    dai_editor_project(ed, dai_vec3{ len, 0, 0 }, &sx, &sy2);
    dai_editor_project(ed, dai_vec3{ len * 2.0f, 0, 0 }, &ex, &ey);
    dai_editor_drag_begin(ed, DAI_AXIS_X, sx, sy2);
    dai_editor_drag_update(ed, ex, ey);
    dai_editor_drag_end(ed);
    dai_doc_get(doc, cubes[1], &rec);
    CHECK(rec.scale.x > 1.5f && std::fabs(rec.scale.y - 1.0f) < 1e-3f,
          "an X scale drag gave scale (%.3f %.3f %.3f)", rec.scale.x, rec.scale.y, rec.scale.z);
    // the collision shape has to follow, or things look bigger than they hit
    dai_doc_sync_apply(sync);
    dai_body scaled_body = dai_scene_body(sc, dai_doc_sync_entity(sync, cubes[1]));
    CHECK(scaled_body != DAI_INVALID_BODY, "the scaled node lost its body");
    dai_editor_undo(ed);
    dai_doc_get(doc, cubes[1], &rec);
    CHECK(std::fabs(rec.scale.x - 1.0f) < 1e-3f, "undo did not restore the scale");
    dai_editor_gizmo_mode(ed, DAI_GIZMO_TRANSLATE);

    // ---- 8. delete is undoable now ----------------------------------------
    std::printf("delete and duplicate\n");
    dai_editor_select(ed, cubes[0], 0);
    dai_vec3 doomed_pos = world_of(doc, cubes[0]);
    CHECK(dai_editor_delete_selection(ed) == DAI_OK, "delete failed");
    CHECK(!dai_doc_valid(doc, cubes[0]), "the node survived the delete");
    CHECK(dai_editor_selection_count(ed) == 0, "the selection should be empty after a delete");
    CHECK(dai_editor_undo(ed) == 1, "undo of delete failed");
    CHECK(dai_doc_valid(doc, cubes[0]), "undo did not bring the node back");
    CHECK(near3(world_of(doc, cubes[0]), doomed_pos), "the restored node is in the wrong place");
    CHECK(dai_doc_sync_entity(sync, cubes[0]) != DAI_INVALID_ENTITY,
          "the restored node has no entity - the sync did not run");
    dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, cubes[0])), &t);
    CHECK(near3(t.position, doomed_pos, 1e-3f), "the restored body is in the wrong place");

    // ---- 9. duplicate -----------------------------------------------------
    uint32_t count_before = dai_doc_count(doc);
    dai_editor_select(ed, cubes[0], 0);
    uint32_t made = dai_editor_duplicate_selection(ed);
    CHECK(made == 1, "duplicating one node made %u copies", made);
    CHECK(dai_doc_count(doc) == count_before + 1, "the copy is not in the document");
    dai_node copy = dai_editor_selected(ed, 0);
    CHECK(copy != cubes[0] && dai_doc_valid(doc, copy), "the copy did not become the selection");
    CHECK(near3(world_of(doc, copy), doomed_pos), "the copy is not where the original was");
    // A copy of an uncoloured node must NOT pick a new palette colour - the
    // colour comes from the id, and the copy has a new one, which is how
    // duplicating used to repaint things. The displayed colour is baked in.
    {
        dai_node_desc src{}, cpy{};
        dai_doc_get(doc, cubes[0], &src);
        dai_doc_get(doc, copy, &cpy);
        CHECK(cpy.color.x != 0.0f || cpy.color.y != 0.0f || cpy.color.z != 0.0f,
              "the copy kept colour 0,0,0 - it will recolour from its new id");
        dai_vec3 shown{};
        if (dai_editor_node_color(ed, cubes[0], &shown)) {
            CHECK(std::fabs(cpy.color.x - shown.x) < 1e-4f &&
                  std::fabs(cpy.color.y - shown.y) < 1e-4f &&
                  std::fabs(cpy.color.z - shown.z) < 1e-4f,
                  "the copy is a different colour than the original shows");
        }
    }
    CHECK(dai_editor_undo(ed) == 1, "undo of duplicate failed");
    CHECK(dai_doc_count(doc) == count_before, "undo did not remove the copy");

    // ---- 10. parent and child selected together ---------------------------
    // The child must not move twice: once because it is selected, once because
    // its parent carried it.
    dai_node_desc pr = dai_node_desc_default();
    pr.motion = DAI_KINEMATIC;
    pr.position = { 0, -8, 0 };
    dai_node par = dai_doc_add(doc, &pr);
    pr.position = { 2, 0, 0 };
    pr.parent = par;
    dai_node chi = dai_doc_add(doc, &pr);
    dai_doc_sync_apply(sync);

    dai_vec3 chi_start = world_of(doc, chi);
    dai_editor_select(ed, par, 0);
    dai_editor_select(ed, chi, 1);
    dai_editor_move_selection(ed, dai_vec3{ 5, 0, 0 });
    dai_vec3 chi_end = world_of(doc, chi);
    CHECK(std::fabs((chi_end.x - chi_start.x) - 5.0f) < 1e-3f,
          "the child moved by %.3f, expected 5 (it was carried twice)", chi_end.x - chi_start.x);

    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
