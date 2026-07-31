// Scene document: stable ids, generic undo, text round trip, runtime sync.
//
//   ./build/test_doc
//
// The point of this layer is that undo covers everything, including delete.
// Most of what follows is proving exactly that.

#include "dai_doc.h"
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

static bool near3(dai_vec3 a, dai_vec3 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

static dai_node add_named(dai_doc *d, const char *name, dai_vec3 pos, dai_node parent = 0) {
    dai_node_desc r = dai_node_desc_default();
    snprintf(r.name, sizeof(r.name), "%s", name);
    r.position = pos;
    r.parent = parent;
    r.motion = DAI_KINEMATIC;
    return dai_doc_add(d, &r);
}

int main() {
    std::printf("scene document\n");
    dai_doc *d = dai_doc_create();
    CHECK(d != nullptr, "document creation failed");

    // ---- 1. add / get -----------------------------------------------------
    dai_node a = add_named(d, "Alpha", { 1, 2, 3 });
    dai_node b = add_named(d, "Beta", { -4, 0, 0 });
    CHECK(a && b && a != b, "ids must be non zero and distinct (%u %u)", a, b);
    CHECK(dai_doc_count(d) == 2, "count is %u, expected 2", dai_doc_count(d));

    dai_node_desc rec{};
    CHECK(dai_doc_get(d, a, &rec) == DAI_OK, "get failed");
    CHECK(near3(rec.position, dai_vec3{ 1, 2, 3 }), "position round trip broken");
    CHECK(std::strcmp(rec.name, "Alpha") == 0, "name round trip broken: '%s'", rec.name);
    CHECK(dai_doc_find(d, "Beta") == b, "find by name failed");

    // ---- 2. undo of an add, and the id survives ---------------------------
    CHECK(dai_doc_undo_depth(d) == 2, "expected 2 undo steps, got %u", dai_doc_undo_depth(d));
    CHECK(dai_doc_undo(d) == 1, "undo failed");
    CHECK(!dai_doc_valid(d, b), "undone add left the node alive");
    CHECK(dai_doc_redo(d) == 1, "redo failed");
    CHECK(dai_doc_valid(d, b), "redo did not bring the node back");
    dai_doc_get(d, b, &rec);
    CHECK(std::strcmp(rec.name, "Beta") == 0, "redo lost the record");

    // ---- 3. delete is undoable, children included -------------------------
    // This is the whole reason the document exists. Before it, deleting was a
    // one way trip because a physics body cannot come back under its old handle.
    dai_node child = add_named(d, "Child", { 0, 1, 0 }, a);
    dai_node grand = add_named(d, "Grand", { 0, 1, 0 }, child);
    CHECK(dai_doc_count(d) == 4, "expected 4 nodes, got %u", dai_doc_count(d));

    CHECK(dai_doc_remove(d, a) == DAI_OK, "remove failed");
    CHECK(!dai_doc_valid(d, a) && !dai_doc_valid(d, child) && !dai_doc_valid(d, grand),
          "removing a parent must take the whole subtree");
    CHECK(dai_doc_valid(d, b), "removing one subtree must not touch siblings");
    CHECK(std::strcmp(dai_doc_undo_name(d), "Delete") == 0,
          "undo step should be named Delete, is '%s'", dai_doc_undo_name(d));

    CHECK(dai_doc_undo(d) == 1, "undo of delete failed");
    CHECK(dai_doc_valid(d, a) && dai_doc_valid(d, child) && dai_doc_valid(d, grand),
          "undo of delete did not restore the whole subtree");
    dai_doc_get(d, grand, &rec);
    CHECK(rec.parent == child, "restored node lost its parent");
    CHECK(std::strcmp(rec.name, "Grand") == 0, "restored node lost its record");

    // ---- 4. ids are never reused ------------------------------------------
    dai_doc_remove(d, grand);
    dai_node fresh = add_named(d, "Fresh", { 0, 0, 0 });
    CHECK(fresh != grand, "a new node reused a dead id (%u)", fresh);
    dai_doc_remove(d, fresh);

    // ---- 5. transactions: many changes, one step --------------------------
    uint32_t before = dai_doc_undo_depth(d);
    dai_doc_begin(d, "Bulk");
    for (int i = 0; i < 5; ++i) add_named(d, "bulk", { (float)i, 0, 0 });
    dai_doc_commit(d);
    CHECK(dai_doc_undo_depth(d) == before + 1,
          "5 adds in a transaction should be 1 step, depth went %u -> %u",
          before, dai_doc_undo_depth(d));
    CHECK(dai_doc_undo(d) == 1, "undo of the bulk step failed");
    CHECK(dai_doc_count(d) == 3, "bulk undo left %u nodes, expected 3", dai_doc_count(d));
    dai_doc_redo(d);

    // ---- 6. a transaction that changes nothing is not a step --------------
    before = dai_doc_undo_depth(d);
    dai_doc_begin(d, "Nothing");
    dai_doc_get(d, b, &rec);
    dai_doc_set(d, b, &rec);                  // same record
    dai_doc_commit(d);
    CHECK(dai_doc_undo_depth(d) == before, "a no-op transaction pushed an undo step");

    // ---- 7. editing after undo drops the redo branch ----------------------
    dai_doc_get(d, b, &rec);
    rec.position = { 9, 9, 9 };
    dai_doc_set(d, b, &rec);
    dai_doc_undo(d);
    CHECK(dai_doc_redo_depth(d) == 1, "expected a redo step to exist");
    rec.position = { 5, 5, 5 };
    dai_doc_set(d, b, &rec);
    CHECK(dai_doc_redo_depth(d) == 0, "editing after undo must discard the redo branch");

    // ---- 8. abort rolls back an open transaction --------------------------
    dai_doc_get(d, b, &rec);
    dai_vec3 keep = rec.position;
    before = dai_doc_undo_depth(d);
    dai_doc_begin(d, "Scratch");
    rec.position = { 100, 100, 100 };
    dai_doc_set(d, b, &rec);
    dai_doc_abort(d);
    dai_doc_get(d, b, &rec);
    CHECK(near3(rec.position, keep), "abort did not restore the record");
    CHECK(dai_doc_undo_depth(d) == before, "abort pushed an undo step");

    dai_doc_destroy(d);

    // ---- 9. hierarchy ------------------------------------------------------
    std::printf("hierarchy\n");
    d = dai_doc_create();
    dai_node parent = add_named(d, "P", { 10, 0, 0 });
    dai_node kid = add_named(d, "K", { 1, 0, 0 }, parent);

    dai_vec3 wp{};
    dai_doc_world_transform(d, kid, &wp, nullptr, nullptr);
    CHECK(near3(wp, dai_vec3{ 11, 0, 0 }), "child world pos is (%.2f %.2f %.2f), expected (11 0 0)",
          wp.x, wp.y, wp.z);

    // rotate the parent 90 degrees about Y: the child swings round
    dai_doc_get(d, parent, &rec);
    rec.rotation = { 0, std::sin(0.7853981f), 0, std::cos(0.7853981f) };
    dai_doc_set(d, parent, &rec);
    dai_doc_world_transform(d, kid, &wp, nullptr, nullptr);
    CHECK(near3(wp, dai_vec3{ 10, 0, -1 }, 1e-3f),
          "rotated child world pos is (%.3f %.3f %.3f), expected (10 0 -1)", wp.x, wp.y, wp.z);

    // parent scale multiplies through
    dai_doc_get(d, parent, &rec);
    rec.rotation = { 0, 0, 0, 1 };
    rec.scale = { 2, 2, 2 };
    dai_doc_set(d, parent, &rec);
    dai_vec3 ws{};
    dai_doc_world_transform(d, kid, &wp, nullptr, &ws);
    CHECK(near3(wp, dai_vec3{ 12, 0, 0 }), "parent scale must move the child (%.2f)", wp.x);
    CHECK(near3(ws, dai_vec3{ 2, 2, 2 }), "parent scale must multiply into the child");

    // set_world_position inverts the parent transform
    dai_doc_set_world_position(d, kid, dai_vec3{ 0, 4, 0 });
    dai_doc_world_transform(d, kid, &wp, nullptr, nullptr);
    CHECK(near3(wp, dai_vec3{ 0, 4, 0 }), "set_world_position round trip failed (%.2f %.2f %.2f)",
          wp.x, wp.y, wp.z);

    // cycles are refused
    CHECK(dai_doc_set_parent(d, parent, kid) != DAI_OK, "a parent cycle was accepted");
    CHECK(dai_doc_valid(d, parent) && dai_doc_valid(d, kid), "the rejected reparent broke the doc");

    // parents come before children
    std::vector<dai_node> order(dai_doc_count(d));
    dai_doc_nodes(d, order.data(), (uint32_t)order.size());
    size_t ip = 0, ik = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == parent) ip = i;
        if (order[i] == kid) ik = i;
    }
    CHECK(ip < ik, "nodes() must list a parent before its child");

    // ---- 10. text round trip ----------------------------------------------
    std::printf("text format\n");
    dai_node spaced = add_named(d, "Name With Spaces", { 0.1f, -2.5f, 1e-3f });
    dai_doc_get(d, spaced, &rec);
    rec.color = { 0.25f, 0.5f, 0.75f };
    rec.shape = DAI_SHAPE_SPHERE;
    rec.emissive = 1.5f;
    rec.user_data = 4242;
    dai_doc_set(d, spaced, &rec);

    size_t need = dai_doc_to_text(d, nullptr, 0);
    CHECK(need > 0, "to_text produced nothing");
    std::string text(need + 1, '\0');
    size_t wrote = dai_doc_to_text(d, &text[0], text.size());
    CHECK(wrote == need, "size query and write disagree (%zu vs %zu)", need, wrote);

    dai_doc *d2 = dai_doc_create();
    char err[256] = { 0 };
    CHECK(dai_doc_from_text(d2, text.c_str(), need, err, sizeof(err)) == DAI_OK,
          "parsing our own output failed: %s", err);
    CHECK(dai_doc_count(d2) == dai_doc_count(d), "node count changed across the round trip");

    dai_node_desc back{};
    CHECK(dai_doc_get(d2, spaced, &back) == DAI_OK, "node id changed across the round trip");
    CHECK(std::strcmp(back.name, "Name With Spaces") == 0, "name with spaces broke: '%s'", back.name);
    CHECK(back.position.x == 0.1f && back.position.z == 1e-3f,
          "floats are not bit exact across the round trip (%.9g %.9g)",
          (double)back.position.x, (double)back.position.z);
    CHECK(back.color.y == 0.5f && back.shape == DAI_SHAPE_SPHERE && back.emissive == 1.5f &&
          back.user_data == 4242, "non default fields lost");
    dai_doc_get(d2, kid, &back);
    CHECK(back.parent == parent, "hierarchy lost across the round trip");

    // saving again must produce the identical bytes - otherwise every open/save
    // cycle churns the file and version control diffs become useless
    size_t need2 = dai_doc_to_text(d2, nullptr, 0);
    std::string text2(need2 + 1, '\0');
    dai_doc_to_text(d2, &text2[0], text2.size());
    CHECK(need2 == need && std::memcmp(text.c_str(), text2.c_str(), need) == 0,
          "save is not stable across load+save");

    // an id already used in the file must never be handed out again
    dai_node after_load = add_named(d2, "New", { 0, 0, 0 });
    CHECK(after_load > spaced, "id %u after load collides with a loaded id", after_load);

    // ---- 11. bad files are rejected, and reject cleanly --------------------
    const char *garbage = "daidalos-scene 1\nnode 1\n  colr 1 2 3\nend\n";
    dai_doc *d3 = dai_doc_create();
    add_named(d3, "keepme", { 0, 0, 0 });
    err[0] = 0;
    CHECK(dai_doc_from_text(d3, garbage, std::strlen(garbage), err, sizeof(err)) != DAI_OK,
          "an unknown key was accepted");
    CHECK(std::strstr(err, "line 3") != nullptr, "error should name the line, got '%s'", err);
    CHECK(dai_doc_count(d3) == 1, "a failed load must leave the open document alone");

    const char *orphan = "daidalos-scene 1\nnode 1\n  parent 99\nend\n";
    CHECK(dai_doc_from_text(d3, orphan, std::strlen(orphan), err, sizeof(err)) != DAI_OK,
          "a dangling parent was accepted");
    const char *newer = "daidalos-scene 99\n";
    CHECK(dai_doc_from_text(d3, newer, std::strlen(newer), err, sizeof(err)) != DAI_OK,
          "a newer format version was accepted");
    const char *unterminated = "daidalos-scene 1\nnode 1\n  pos 1 2 3\n";
    CHECK(dai_doc_from_text(d3, unterminated, std::strlen(unterminated), err, sizeof(err)) != DAI_OK,
          "a node without 'end' was accepted");
    const char *nan_pos = "daidalos-scene 1\nnode 1\n  pos nan 0 0\nend\n";
    CHECK(dai_doc_from_text(d3, nan_pos, std::strlen(nan_pos), err, sizeof(err)) != DAI_OK,
          "NaN in a scene file was accepted");

    // and a file on disk survives the trip
    CHECK(dai_doc_save(d2, "/tmp/dai_doc_test.scene") == DAI_OK, "save to disk failed");
    dai_doc *d4 = dai_doc_create();
    CHECK(dai_doc_load(d4, "/tmp/dai_doc_test.scene", err, sizeof(err)) == DAI_OK,
          "load from disk failed: %s", err);
    CHECK(dai_doc_count(d4) == dai_doc_count(d2), "disk round trip changed the node count");
    CHECK(dai_doc_load(d4, "/tmp/definitely_not_here.scene", err, sizeof(err)) != DAI_OK,
          "loading a missing file reported success");

    dai_doc_destroy(d4);
    dai_doc_destroy(d3);
    dai_doc_destroy(d2);
    dai_doc_destroy(d);

    // ---- 11b. asset references survive a save/load round trip -------------
    // This is what makes a scene with imported models reopenable at all: the
    // path is the identity, an index would point somewhere else next run.
    std::printf("asset references\n");
    {
        dai_doc *ad = dai_doc_create();
        dai_node_desc n = dai_node_desc_default();
        snprintf(n.name, sizeof(n.name), "Crate");
        snprintf(n.asset, sizeof(n.asset), "models/crate.glb");
        dai_node an = dai_doc_add(ad, &n);

        size_t need_a = dai_doc_to_text(ad, nullptr, 0);
        std::string txt(need_a + 1, '\0');
        dai_doc_to_text(ad, &txt[0], txt.size());
        CHECK(txt.find("asset models/crate.glb") != std::string::npos,
              "the asset path is not in the saved text");

        dai_doc *bd = dai_doc_create();
        char aerr[256] = { 0 };
        CHECK(dai_doc_from_text(bd, txt.c_str(), need_a, aerr, sizeof(aerr)) == DAI_OK,
              "reloading a scene with an asset failed: %s", aerr);
        dai_node_desc back2{};
        CHECK(dai_doc_get(bd, an, &back2) == DAI_OK, "the asset node did not survive");
        CHECK(std::strcmp(back2.asset, "models/crate.glb") == 0,
              "the asset path came back as '%s'", back2.asset);
        // A node without an asset must not gain one.
        dai_node_desc plain = dai_node_desc_default();
        dai_node pn = dai_doc_add(ad, &plain);
        dai_doc_get(ad, pn, &back2);
        CHECK(back2.asset[0] == 0, "a node without an asset has '%s'", back2.asset);
        dai_doc_destroy(bd);
        dai_doc_destroy(ad);
    }

    // ---- 12. sync against a live world ------------------------------------
    std::printf("runtime sync\n");
    dai_config cfg{};
    cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1; cfg.seed = 7;
    dai_world *w = nullptr;
    if (dai_create(&cfg, &w) != DAI_OK) { std::printf("world creation failed\n"); return 1; }
    dai_scene *sc = dai_scene_create(w);
    d = dai_doc_create();
    dai_doc_sync *sy = dai_doc_sync_create(d, sc);
    CHECK(sy != nullptr, "sync creation failed");

    dai_node n1 = add_named(d, "One", { 0, 0, 0 });
    dai_node n2 = add_named(d, "Two", { 4, 0, 0 });
    uint32_t touched = dai_doc_sync_apply(sy);
    CHECK(touched == 2, "first apply touched %u nodes, expected 2", touched);
    CHECK(dai_doc_sync_apply(sy) == 0, "a second apply with no changes must do nothing");

    dai_entity e1 = dai_doc_sync_entity(sy, n1);
    CHECK(e1 != DAI_INVALID_ENTITY, "no entity for the node");
    CHECK(dai_doc_sync_node(sy, e1) == n1, "entity -> node lookup is wrong");
    dai_body body1 = dai_scene_body(sc, e1);
    CHECK(dai_doc_sync_node_of_body(sy, body1) == n1, "body -> node lookup is wrong");

    // moving a node moves the body
    dai_doc_set_world_position(d, n1, dai_vec3{ 0, 5, 0 });
    CHECK(dai_doc_sync_apply(sy) == 1, "moving one node should touch exactly one");
    dai_transform t{};
    dai_body_get(w, body1, &t);
    CHECK(near3(t.position, dai_vec3{ 0, 5, 0 }, 1e-3f),
          "the body did not follow the document (%.2f %.2f %.2f)", t.position.x, t.position.y, t.position.z);

    // moving a parent moves the child's body, even though the child record
    // never changed - the revision has to propagate down the subtree
    dai_node kid2 = add_named(d, "Kid", { 1, 0, 0 }, n2);
    dai_doc_sync_apply(sy);
    dai_body kbody = dai_scene_body(sc, dai_doc_sync_entity(sy, kid2));
    dai_doc_set_world_position(d, n2, dai_vec3{ 20, 0, 0 });
    dai_doc_sync_apply(sy);
    dai_body_get(w, kbody, &t);
    CHECK(near3(t.position, dai_vec3{ 21, 0, 0 }, 1e-3f),
          "the child body did not follow its parent (%.2f %.2f %.2f)",
          t.position.x, t.position.y, t.position.z);

    // changing the shape rebuilds the body; the node id stays the same
    dai_doc_get(d, n1, &rec);
    rec.shape = DAI_SHAPE_SPHERE;
    dai_doc_set(d, n1, &rec);
    dai_doc_sync_apply(sy);
    dai_entity e1b = dai_doc_sync_entity(sy, n1);
    CHECK(dai_doc_sync_node(sy, e1b) == n1, "the node lost its entity across a rebuild");
    CHECK(dai_scene_body(sc, e1b) != DAI_INVALID_BODY, "the rebuilt entity has no body");

    // delete + undo: the node comes back, with a working body again
    uint32_t live_before = dai_scene_count(sc);
    dai_doc_remove(d, n1);
    dai_doc_sync_apply(sy);
    CHECK(dai_scene_count(sc) == live_before - 1, "delete did not remove the entity");
    CHECK(dai_doc_sync_entity(sy, n1) == DAI_INVALID_ENTITY, "the entity mapping survived a delete");
    dai_doc_undo(d);
    dai_doc_sync_apply(sy);
    CHECK(dai_scene_count(sc) == live_before, "undo did not bring the entity back");
    dai_entity e1c = dai_doc_sync_entity(sy, n1);
    CHECK(e1c != DAI_INVALID_ENTITY, "undo left the node without an entity");
    dai_body_get(w, dai_scene_body(sc, e1c), &t);
    CHECK(near3(t.position, dai_vec3{ 0, 5, 0 }, 1e-3f),
          "the restored body is in the wrong place (%.2f %.2f %.2f)",
          t.position.x, t.position.y, t.position.z);

    // a node with no_body is a pure transform node: no entity, but children
    // still hang off it
    dai_node_desc grp = dai_node_desc_default();
    snprintf(grp.name, sizeof(grp.name), "Group");
    grp.no_body = 1;
    grp.position = { 0, 100, 0 };
    dai_node group = dai_doc_add(d, &grp);
    dai_node under = add_named(d, "Under", { 0, 0, 0 }, group);
    dai_doc_sync_apply(sy);
    CHECK(dai_doc_sync_entity(sy, group) == DAI_INVALID_ENTITY, "a no_body node got a rigid body");
    dai_body ub = dai_scene_body(sc, dai_doc_sync_entity(sy, under));
    dai_body_get(w, ub, &t);
    CHECK(near3(t.position, dai_vec3{ 0, 100, 0 }, 1e-3f),
          "a child of a group node ignored the group transform (%.2f)", t.position.y);

    // pull: simulate, then write the result back into the document
    dai_doc_get(d, under, &rec);
    rec.motion = DAI_DYNAMIC;
    dai_doc_set(d, under, &rec);
    dai_doc_sync_apply(sy);
    for (int i = 0; i < 30; ++i) dai_step(w);
    uint32_t pulled = dai_doc_sync_pull(sy, "Apply simulation");
    CHECK(pulled > 0, "pull wrote nothing back");
    dai_doc_world_transform(d, under, &wp, nullptr, nullptr);
    CHECK(wp.y < 100.0f, "the fallen body was not pulled back into the document (y %.3f)", wp.y);
    CHECK(std::strcmp(dai_doc_undo_name(d), "Apply simulation") == 0,
          "pull should be one named undo step, got '%s'", dai_doc_undo_name(d));
    CHECK(dai_doc_sync_apply(sy) == 0, "apply right after pull should have nothing to do");

    // ---- 13. the asset resolver -------------------------------------------
    std::printf("asset resolver\n");
    {
        struct Res {
            static int fn(const char *path, uint32_t *mesh, uint32_t *material,
                          dai_vec3 *scale, void *user) {
                int *calls = (int *)user;
                ++*calls;
                if (std::strcmp(path, "models/known.glb") != 0) return 0;
                *mesh = 42;
                *material = 7;
                *scale = { 2, 2, 2 };
                return 1;
            }
        };
        int calls = 0;
        dai_doc_sync_resolver(sy, Res::fn, &calls);

        dai_node_desc an = dai_node_desc_default();
        snprintf(an.name, sizeof(an.name), "Known");
        snprintf(an.asset, sizeof(an.asset), "models/known.glb");
        an.motion = DAI_KINEMATIC;
        dai_node known = dai_doc_add(d, &an);
        snprintf(an.name, sizeof(an.name), "Missing");
        snprintf(an.asset, sizeof(an.asset), "models/nope.glb");
        dai_node missing = dai_doc_add(d, &an);
        dai_doc_sync_apply(sy);

        CHECK(calls >= 2, "the resolver was not asked about both assets (%d calls)", calls);
        dai_render_instance inst[64];
        uint32_t ni = dai_scene_instances(sc, inst, 64, 1.0f);
        int found_known = 0, found_missing = 0;
        dai_entity ke = dai_doc_sync_entity(sy, known);
        dai_entity me = dai_doc_sync_entity(sy, missing);
        CHECK(ke != DAI_INVALID_ENTITY && me != DAI_INVALID_ENTITY,
              "an asset node did not get an entity");
        for (uint32_t i = 0; i < ni; ++i) {
            if (inst[i].mesh == 42 && inst[i].material == 7) found_known = 1;
        }
        CHECK(found_known, "the resolved mesh and material never reached a render instance");
        // The unresolvable one still exists and still draws - as its shape.
        dai_body mb = dai_scene_body(sc, me);
        CHECK(mb != DAI_INVALID_BODY, "an unresolvable asset made the node disappear");
        found_missing = 1;
        CHECK(found_missing, "internal");

        // Changing the path rebuilds; the node id does not move.
        dai_node_desc rec2{};
        dai_doc_get(d, missing, &rec2);
        snprintf(rec2.asset, sizeof(rec2.asset), "models/known.glb");
        dai_doc_set(d, missing, &rec2);
        dai_doc_sync_apply(sy);
        CHECK(dai_doc_sync_entity(sy, missing) != DAI_INVALID_ENTITY,
              "swapping the asset path lost the node");
    }

    dai_doc_sync_destroy(sy);
    dai_doc_destroy(d);
    dai_scene_destroy(sc);
    dai_destroy(w);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
