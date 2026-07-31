// Editor core: picking, selection, drag edits and undo.
//
//   ./build/test_editor

#include "dai_editor.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static dai_vec3 pos_of(dai_world *w, dai_scene *sc, dai_entity e) {
    dai_transform t{};
    dai_body_get(w, dai_scene_body(sc, e), &t);
    return t.position;
}

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1; cfg.seed = 3;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world creation failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    std::printf("editor core\n");

    // three static cubes in a row, so a pick has an unambiguous right answer
    dai_entity ents[3];
    for (int i = 0; i < 3; ++i) {
        dai_entity_desc d = dai_entity_desc_default();
        d.body.shape = DAI_SHAPE_BOX;
        d.body.motion = DAI_KINEMATIC;            // kinematic: movable by the editor, not by gravity
        d.body.half_extent = { 0.5f, 0.5f, 0.5f };
        d.body.position = { -3.0f + (float)i * 3.0f, 0.0f, 0.0f };
        ents[i] = dai_scene_spawn(sc, &d);
    }
    dai_step(w);

    dai_editor *ed = dai_editor_create(sc);
    CHECK(ed != nullptr, "editor creation failed");
    // camera looking down -Z from +Z, so screen x maps to world x
    dai_editor_camera(ed, dai_vec3{ 0, 0, 12 }, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);

    // ---- 1. the ray under the screen centre points straight ahead
    dai_vec3 o, d;
    dai_editor_ray(ed, 400, 300, &o, &d);
    CHECK(std::fabs(d.x) < 1e-4f && std::fabs(d.y) < 1e-4f && d.z < -0.99f,
          "centre ray is (%.3f %.3f %.3f), expected (0 0 -1)", d.x, d.y, d.z);
    dai_editor_ray(ed, 800, 300, &o, &d);
    CHECK(d.x > 0.2f, "the ray at the right edge does not point right (x %.3f)", d.x);

    // ---- 2. picking hits the middle cube in the middle of the screen
    dai_entity hit = dai_editor_pick(ed, 400, 300);
    CHECK(hit == ents[1], "centre pick got entity %u, expected the middle cube %u", hit, ents[1]);

    // and something off to the side hits nothing
    CHECK(dai_editor_pick(ed, 20, 20) == DAI_INVALID_ENTITY, "a pick into empty space hit something");

    // ---- 3. selection, additive and toggling
    dai_editor_select(ed, ents[1], 0);
    CHECK(dai_editor_selection_count(ed) == 1, "selection has %u entries", dai_editor_selection_count(ed));
    dai_editor_select(ed, ents[2], 1);
    CHECK(dai_editor_selection_count(ed) == 2, "additive select did not add");
    dai_editor_select(ed, ents[2], 1);
    CHECK(dai_editor_selection_count(ed) == 1, "clicking a selected entity again did not toggle it off");
    CHECK(dai_editor_is_selected(ed, ents[1]), "the wrong entity stayed selected");

    dai_editor_select(ed, ents[0], 1);
    dai_vec3 c = dai_editor_selection_center(ed);
    CHECK(std::fabs(c.x - (-1.5f)) < 1e-3f, "selection centre x is %.3f, expected -1.5", c.x);

    // ---- 4. a direct move is one undo step
    dai_editor_select(ed, ents[1], 0);
    dai_vec3 before = pos_of(w, sc, ents[1]);
    dai_editor_move_selection(ed, dai_vec3{ 2.0f, 1.0f, 0.0f });
    dai_vec3 after = pos_of(w, sc, ents[1]);
    CHECK(std::fabs(after.x - before.x - 2.0f) < 1e-3f && std::fabs(after.y - before.y - 1.0f) < 1e-3f,
          "move went to (%.3f %.3f), expected +2/+1 from (%.3f %.3f)", after.x, after.y, before.x, before.y);
    CHECK(dai_editor_undo_depth(ed) == 1, "undo depth is %u after one move", dai_editor_undo_depth(ed));

    CHECK(dai_editor_undo(ed) == 1, "undo reported nothing to do");
    dai_vec3 undone = pos_of(w, sc, ents[1]);
    CHECK(std::fabs(undone.x - before.x) < 1e-3f && std::fabs(undone.y - before.y) < 1e-3f,
          "undo left the cube at (%.3f %.3f)", undone.x, undone.y);
    CHECK(dai_editor_redo(ed) == 1 && std::fabs(pos_of(w, sc, ents[1]).x - after.x) < 1e-3f,
          "redo did not restore the move");
    dai_editor_undo(ed);

    // ---- 5. a drag is ONE undo step however many updates it takes
    uint32_t depth = dai_editor_undo_depth(ed);
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    CHECK(dai_editor_dragging(ed) == 1, "drag did not start");
    for (int i = 1; i <= 10; ++i) dai_editor_drag_update(ed, 400.0f + (float)i * 10.0f, 300);
    dai_editor_drag_end(ed);
    CHECK(dai_editor_undo_depth(ed) == depth + 1, "a 10 step drag made %u undo entries",
          dai_editor_undo_depth(ed) - depth);
    dai_vec3 dragged = pos_of(w, sc, ents[1]);
    CHECK(dragged.x > before.x + 0.5f, "dragging right moved the cube to x %.3f from %.3f", dragged.x, before.x);
    CHECK(std::fabs(dragged.y - before.y) < 1e-3f, "an X drag also moved Y to %.3f", dragged.y);
    CHECK(dai_editor_undo(ed) == 1 && std::fabs(pos_of(w, sc, ents[1]).x - before.x) < 1e-3f,
          "undoing the drag did not restore the position");

    // ---- 6. snapping
    dai_editor_snap(ed, 1.0f, 0.0f);
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    dai_editor_drag_update(ed, 412, 300);          // a small, unsnapped amount
    dai_vec3 snapped = pos_of(w, sc, ents[1]);
    float rel = snapped.x - before.x;
    CHECK(std::fabs(rel - std::round(rel)) < 1e-3f, "snapped drag gave a delta of %.3f", rel);
    dai_editor_drag_end(ed);
    dai_editor_snap(ed, 0.0f, 0.0f);

    // ---- 7. editing after an undo drops the redo branch
    dai_editor_undo(ed);
    uint32_t redo_before = dai_editor_redo_depth(ed);
    CHECK(redo_before > 0 || dai_editor_undo_depth(ed) == 0, "nothing to redo after an undo");
    dai_editor_move_selection(ed, dai_vec3{ 0.5f, 0, 0 });
    CHECK(dai_editor_redo_depth(ed) == 0, "editing after undo left %u redo steps",
          dai_editor_redo_depth(ed));

    dai_editor_destroy(ed);
    dai_scene_destroy(sc);
    dai_destroy(w);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
