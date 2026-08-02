// Play mode and timeline scrubbing.
//
//   ./build/test_play
//
// The promise being tested: pressing play and stopping again leaves the scene
// exactly as it was, and scrubbing lands on bit identical state in both
// directions.

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
// dai_advance deliberately caps catch-up at 8 ticks so a hitch cannot become
// a death spiral, so a host loop calls it once per frame. Do the same here.
static uint32_t run_for(dai_editor *ed, int frames) {
    uint32_t total = 0;
    float alpha = 0.0f;
    for (int i = 0; i < frames; ++i) total += dai_editor_advance(ed, 1.0 / 60.0, &alpha);
    return total;
}

static dai_vec3 world_of(dai_doc *d, dai_node n) {
    dai_vec3 p{};
    dai_doc_world_transform(d, n, &p, nullptr, nullptr);
    return p;
}

int main() {
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 128; cfg.physics_threads = 1;
    cfg.snapshot_ring = 64; cfg.seed = 11;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world creation failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    dai_doc *doc = dai_doc_create();
    dai_doc_sync *sync = dai_doc_sync_create(doc, sc);
    dai_editor *ed = dai_editor_create(doc, sync);
    std::printf("play mode\n");

    dai_node_desc g = dai_node_desc_default();
    std::snprintf(g.name, sizeof(g.name), "Ground");
    g.motion = DAI_STATIC;
    g.half_extent = { 20, 0.5f, 20 };
    g.position = { 0, -0.5f, 0 };
    dai_doc_add(doc, &g);

    dai_node_desc b = dai_node_desc_default();
    std::snprintf(b.name, sizeof(b.name), "Faller");
    b.motion = DAI_DYNAMIC;
    b.position = { 0, 8, 0 };
    dai_node faller = dai_doc_add(doc, &b);
    dai_doc_sync_apply(sync);

    dai_vec3 authored = world_of(doc, faller);
    CHECK(dai_editor_state_get(ed) == DAI_EDITOR_EDIT, "a fresh editor should be in edit mode");

    // ---- 1. editing does not simulate ------------------------------------
    CHECK(run_for(ed, 30) == 0, "the world advanced while editing");
    CHECK(near3(world_of(doc, faller), authored), "the document moved while editing");

    // ---- 2. play runs the simulation, the document stays put -------------
    dai_editor_play(ed);
    CHECK(dai_editor_state_get(ed) == DAI_EDITOR_PLAY, "play did not take");
    uint32_t ticks = run_for(ed, 30);
    CHECK(ticks >= 29, "30 frames at 60Hz stepped only %u ticks", ticks);

    dai_transform t{};
    dai_body body = dai_scene_body(sc, dai_doc_sync_entity(sync, faller));
    dai_body_get(w, body, &t);
    CHECK(t.position.y < authored.y - 0.5f, "the body did not fall (y %.3f)", t.position.y);
    CHECK(near3(world_of(doc, faller), authored),
          "playing changed the document - it must stay the authored scene");

    // ---- 3. scrubbing back and forward lands on identical state ----------
    dai_editor_pause(ed);
    CHECK(dai_editor_state_get(ed) == DAI_EDITOR_PAUSED, "pause did not take");
    dai_tick last = dai_editor_timeline_last(ed);
    dai_tick first = dai_editor_timeline_first(ed);
    CHECK(last > first, "the timeline is empty (%llu..%llu)",
          (unsigned long long)first, (unsigned long long)last);

    uint64_t sum_at_last = dai_checksum(w);
    dai_tick middle = first + (last - first) / 2;
    CHECK(dai_editor_scrub(ed, middle) == 1, "scrubbing to the middle failed");
    CHECK(dai_editor_timeline_tick(ed) == middle, "the scrub landed on the wrong tick");
    dai_body_get(w, body, &t);
    float y_middle = t.position.y;
    uint64_t sum_middle = dai_checksum(w);

    CHECK(dai_editor_scrub(ed, last) == 1, "scrubbing forward again failed");
    CHECK(dai_checksum(w) == sum_at_last,
          "replaying forward did not reproduce the same state - determinism is broken");

    CHECK(dai_editor_scrub(ed, middle) == 1, "second scrub back failed");
    CHECK(dai_checksum(w) == sum_middle, "scrubbing back twice gave a different state");
    dai_body_get(w, body, &t);
    CHECK(std::fabs(t.position.y - y_middle) < 1e-5f, "the body is not where it was at that tick");

    // the very first tick of the run must be reachable and must still have the
    // scene in it - a snapshot holds the state *before* its tick's commands,
    // so an off by one here deletes everything that was spawned that tick
    CHECK(dai_editor_scrub(ed, first) == 1, "scrubbing to the first tick failed");
    CHECK(dai_scene_body(sc, dai_doc_sync_entity(sync, faller)) != DAI_INVALID_BODY,
          "scrubbing to the start of the run deleted the scene");
    dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, faller)), &t);
    CHECK(t.position.y > authored.y - 0.2f,
          "at the first tick the body should barely have moved, y is %.3f", t.position.y);

    // out of range is refused, not clamped silently
    CHECK(first == 0 || dai_editor_scrub(ed, first - 1) == 0,
          "scrubbing before the start of the run was accepted");

    // ---- 4. stop restores the authored scene exactly ----------------------
    dai_editor_scrub(ed, last);
    dai_editor_stop(ed);
    CHECK(dai_editor_state_get(ed) == DAI_EDITOR_EDIT, "stop did not return to edit mode");
    dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, faller)), &t);
    CHECK(near3(t.position, authored, 1e-3f),
          "stop left the body at (%.3f %.3f %.3f), expected the authored (%.3f %.3f %.3f)",
          t.position.x, t.position.y, t.position.z, authored.x, authored.y, authored.z);
    dai_vec3 lin{ 1, 1, 1 }, ang{ 1, 1, 1 };
    dai_body_get_velocity(w, dai_scene_body(sc, dai_doc_sync_entity(sync, faller)), &lin, &ang);
    CHECK(near3(lin, dai_vec3{ 0, 0, 0 }, 1e-3f),
          "stop left the body moving (%.3f %.3f %.3f) - it will drift on the next play",
          lin.x, lin.y, lin.z);

    // ---- 5. keeping a result is explicit ---------------------------------
    uint32_t undo_before = dai_editor_undo_depth(ed);
    dai_editor_play(ed);
    run_for(ed, 30);
    CHECK(dai_editor_undo_depth(ed) == undo_before, "playing pushed an undo step by itself");
    uint32_t written = dai_editor_apply_sim(ed);
    CHECK(written > 0, "apply_sim wrote nothing back");
    CHECK(dai_editor_undo_depth(ed) == undo_before + 1, "apply_sim should be exactly one undo step");
    CHECK(world_of(doc, faller).y < authored.y - 0.5f, "apply_sim did not move the document");
    dai_editor_stop(ed);

    // undo puts the authored scene back
    CHECK(dai_editor_undo(ed) == 1, "undo of apply_sim failed");
    CHECK(near3(world_of(doc, faller), authored), "undo did not restore the authored position");
    dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, faller)), &t);
    CHECK(near3(t.position, authored, 1e-3f), "undo did not put the body back too");

    // ---- 6. a drag in progress does not survive pressing play -------------
    dai_editor_select(ed, faller, 0);
    dai_editor_camera(ed, dai_vec3{ 0, 5, 14 }, dai_vec3{ 0, 4, 0 }, dai_vec3{ 0, 1, 0 },
                      60.0f, 0.1f, 200.0f, 800.0f, 600.0f);
    dai_editor_drag_begin(ed, DAI_AXIS_X, 400, 300);
    dai_editor_drag_update(ed, 500, 300);
    dai_editor_play(ed);
    CHECK(!dai_editor_dragging(ed), "the drag survived pressing play");
    CHECK(near3(world_of(doc, faller), authored),
          "the abandoned drag was left in the document");
    dai_editor_stop(ed);

    // ---- 7. edits made WHILE playing are a rehearsal ----------------------
    // The one that started this: press play, drag a crate somewhere, press
    // stop - and the crate is somewhere else forever. Unity throws those
    // changes away, because a thing you moved to watch it fall is not an edit
    // to the scene. Only "Keep" (apply_sim, tested above) survives.
    {
        dai_vec3 start = world_of(doc, faller);
        uint32_t nodes_before = dai_doc_count(doc);
        uint32_t undo_depth = dai_editor_undo_depth(ed);
        dai_editor_play(ed);
        run_for(ed, 10);

        dai_editor_select(ed, faller, 0);
        // Where the object IS when the nudge happens - it has been falling for
        // ten ticks, so this is nowhere near what the document says.
        dai_vec3 live_before{};
        CHECK(dai_editor_live_position(ed, faller, &live_before) != 0, "no live pose during play");
        dai_editor_move_selection(ed, dai_vec3{ 4, 0, 0 });        // "a drag while playing"

        // This used to read the DOCUMENT, and the document used to be what a
        // move during play wrote to. Both were wrong, and they hid each other:
        // the object moved relative to its pre-play pose rather than to where
        // it actually was, so dragging a falling crate upwards twice put it
        // higher each time. The object still has to move - that half was never
        // in question - it just has to move from where it IS, and leave the
        // document alone so Stop can still be exact.
        dai_vec3 live_after{};
        dai_editor_live_position(ed, faller, &live_after);
        CHECK(std::fabs(live_after.x - (live_before.x + 4.0f)) < 1e-3f,
              "moving during play moved the object to %.3f, expected %.3f",
              live_after.x, live_before.x + 4.0f);
        CHECK(std::fabs(world_of(doc, faller).x - start.x) < 1e-3f,
              "moving during play wrote %.3f into the document; it must stay at %.3f until Stop",
              world_of(doc, faller).x, start.x);

        // ...and a whole new object, which must not survive either
        dai_node_desc extra = dai_node_desc_default();
        extra.position = { 9, 9, 9 };
        dai_node ghost = dai_doc_add(doc, &extra);
        CHECK(dai_doc_valid(doc, ghost), "the node added during play was not added");

        dai_editor_stop(ed);
        dai_vec3 after = world_of(doc, faller);
        CHECK(near3(after, start, 1e-3f),
              "stop left the object at (%.3f %.3f %.3f) - it was moved DURING play and the "
              "document kept it, which is the bug this test exists for",
              after.x, after.y, after.z);
        CHECK(!dai_doc_valid(doc, ghost), "an object created during play survived stop");
        CHECK(dai_doc_count(doc) == nodes_before, "the node count changed across play/stop");
        CHECK(dai_editor_undo_depth(ed) >= undo_depth,
              "undo history went backwards across play mode");

        // and the live body is back where the document says, not where the
        // rehearsal left it
        dai_transform bt{};
        dai_body_get(w, dai_scene_body(sc, dai_doc_sync_entity(sync, faller)), &bt);
        CHECK(near3(bt.position, start, 1e-3f),
              "the body is at (%.3f %.3f %.3f), the document says (%.3f %.3f %.3f)",
              bt.position.x, bt.position.y, bt.position.z, start.x, start.y, start.z);
    }

    dai_editor_destroy(ed);
    dai_doc_sync_destroy(sync);
    dai_doc_destroy(doc);
    dai_scene_destroy(sc);
    dai_destroy(w);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
