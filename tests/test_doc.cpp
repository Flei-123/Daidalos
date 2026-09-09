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

// Blockout, CSG and the DoorSocket - the shapes a room is made of. Their
// checks live in their own file (they are geometry, not document plumbing)
// but run in this binary, because half of what they prove is that the fields
// survive this file's format, its undo stack and its round trip.
#include "blockout_cases.hpp"
// ... and the boolean measured against the bar - closed, no slivers, and
// volumes with a closed form - in its own file next to it.
#include "blockout_csg_cases.hpp"
// ... and the modifier stack's two edge operators - bevel and subdivide -
// measured the same way: face counts by the direction they face, volumes
// against a formula worked out on paper, and the boolean's own result put
// through the bevel to prove it survives a T junction.
#include "modifier_bevel_cases.hpp"
// ... and the path the editor actually takes: dai_blockout_host_sync against a
// renderer that counts. One field change, one upload - and undo brings the old
// key AND the old triangles back. The two renderer entry points are defined in
// that file, so it has to come after everything that only reads the document.
#include "blockout_host_cases.hpp"

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

// Duplicate names count UP - they do not grow a tail of parentheses.
//
// The bug this pins: strip_copy_suffix used to remove exactly one " (N)".
// Everything made by those builds is named "Box (2) (1) (1)", and stripping
// one suffix off that leaves "Box (2) (1)" as the base - so the next copy was
// "Box (2) (1) (1)" all over again. Copy of a copy means "another one of
// those", so the list must read Box, Box (1), Box (2), Box (3).
static void test_unique_names() {
    std::printf("duplicate names: count up, never grow parentheses\n");

    // --- counting up, including copying a copy -----------------------------
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc rec{};
        const char *want[4] = { "Box", "Box (1)", "Box (2)", "Box (3)" };
        // Each round asks for the name the PREVIOUS round produced - which is
        // exactly what duplicating the newest copy does.
        const char *ask = "Box";
        for (int i = 0; i < 4; ++i) {
            dai_node n = add_named(d, ask, { (float)i, 0, 0 });
            CHECK(dai_doc_get(d, n, &rec) == DAI_OK, "copy %d was not added", i);
            CHECK(std::strcmp(rec.name, want[i]) == 0,
                  "copy %d should be '%s', is '%s'", i, want[i], rec.name);
            ask = want[i];
        }
        dai_doc_destroy(d);
    }

    // --- a name already spoiled by an older build is repaired ---------------
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc rec{};
        dai_node n = add_named(d, "Crate (2) (1) (1)", { 0, 0, 0 });
        CHECK(dai_doc_get(d, n, &rec) == DAI_OK, "spoiled name was not added");
        CHECK(std::strcmp(rec.name, "Crate") == 0,
              "'Crate (2) (1) (1)' into an empty scene should be 'Crate', is '%s'",
              rec.name);

        dai_node deep = add_named(d, "A (1) (2) (3) (4) (5)", { 1, 0, 0 });
        CHECK(dai_doc_get(d, deep, &rec) == DAI_OK, "deep name was not added");
        CHECK(std::strcmp(rec.name, "A") == 0,
              "five suffixes should all come off, got '%s'", rec.name);
        dai_doc_destroy(d);
    }

    // --- names that only LOOK like a suffix are left alone -----------------
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc rec{};
        struct { const char *in, *out; } keep[] = {
            { "Box(2)",      "Box(2)"      },   // no space: somebody's name
            { "Mark (v2)",   "Mark (v2)"   },   // not digits
            { "Level 1",     "Level 1"     },   // no parentheses at all
            { "Wall ()",     "Wall ()"     },   // empty parentheses
        };
        for (auto &k : keep) {
            dai_node n = add_named(d, k.in, { 0, 0, 0 });
            CHECK(dai_doc_get(d, n, &rec) == DAI_OK, "'%s' was not added", k.in);
            CHECK(std::strcmp(rec.name, k.out) == 0,
                  "'%s' should stay '%s', became '%s'", k.in, k.out, rec.name);
        }
        // "(2)" strips to nothing - it must fall back to what was asked for,
        // not to an empty name.
        dai_node bare = add_named(d, "(2)", { 0, 0, 0 });
        CHECK(dai_doc_get(d, bare, &rec) == DAI_OK, "'(2)' was not added");
        CHECK(rec.name[0] != 0, "a name that is only a suffix became empty");
        dai_doc_destroy(d);
    }

    // --- uniqueness is per level, and stripping does not leak across levels -
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc rec{};
        dai_node pa = add_named(d, "P1", { 0, 0, 0 });
        dai_node pb = add_named(d, "P2", { 5, 0, 0 });
        dai_node ka = add_named(d, "Box", { 0, 0, 0 }, pa);
        dai_node kb = add_named(d, "Box", { 0, 0, 0 }, pb);
        CHECK(dai_doc_get(d, ka, &rec) == DAI_OK && std::strcmp(rec.name, "Box") == 0,
              "first child should be 'Box', is '%s'", rec.name);
        CHECK(dai_doc_get(d, kb, &rec) == DAI_OK && std::strcmp(rec.name, "Box") == 0,
              "a child of ANOTHER parent may also be 'Box', is '%s'", rec.name);
        dai_node kc = add_named(d, "Box", { 1, 0, 0 }, pa);
        CHECK(dai_doc_get(d, kc, &rec) == DAI_OK && std::strcmp(rec.name, "Box (1)") == 0,
              "second child of the same parent should be 'Box (1)', is '%s'", rec.name);
        dai_doc_destroy(d);
    }

    // --- a rename onto a taken name counts up too --------------------------
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc rec{};
        add_named(d, "Lamp", { 0, 0, 0 });
        dai_node other = add_named(d, "Spare", { 1, 0, 0 });
        CHECK(dai_doc_get(d, other, &rec) == DAI_OK, "could not read 'Spare'");
        std::snprintf(rec.name, sizeof(rec.name), "Lamp");
        CHECK(dai_doc_set(d, other, &rec) == DAI_OK, "rename was refused");
        CHECK(dai_doc_get(d, other, &rec) == DAI_OK, "could not read the rename back");
        CHECK(std::strcmp(rec.name, "Lamp (1)") == 0,
              "renaming onto a taken name should give 'Lamp (1)', gave '%s'", rec.name);
        dai_doc_destroy(d);
    }
}

// The Text component survives a save and a load - including the part that is
// easiest to get wrong, a label with SPACES in it. A format that splits on
// whitespace turns "Press any key" into "Press" and loses the rest, and it
// does so quietly: the scene still loads, the label is just shorter.
static void test_text_component() {
    std::printf("text component: round trip, spaces and all\n");
    dai_doc *d = dai_doc_create();
    dai_node_desc r = dai_node_desc_default();
    std::snprintf(r.name, sizeof(r.name), "Score");
    r.text_on = 1;
    std::snprintf(r.text, sizeof(r.text), "Press any key to start");
    r.text_size = 32.0f;
    r.text_color = { 0.9f, 0.3f, 0.2f };
    r.text_anchor = 7;
    r.text_x = -12.0f;
    r.text_y = 20.0f;
    dai_node n = dai_doc_add(d, &r);

    // A second one that uses a KEY instead of words.
    dai_node_desc k = dai_node_desc_default();
    std::snprintf(k.name, sizeof(k.name), "Lives");
    k.text_on = 1;
    std::snprintf(k.text, sizeof(k.text), "@hud.lives");
    dai_node n2 = dai_doc_add(d, &k);

    std::vector<char> buf(dai_doc_to_text(d, nullptr, 0) + 1);
    dai_doc_to_text(d, buf.data(), buf.size());

    dai_doc *e = dai_doc_create();
    char err[256] = { 0 };
    CHECK(dai_doc_from_text(e, buf.data(), std::strlen(buf.data()), err, sizeof(err)) == DAI_OK,
          "the scene with a Text component did not load: %s", err);

    dai_node_desc got{};
    CHECK(dai_doc_get(e, n, &got) == DAI_OK, "the text node is gone");
    CHECK(got.text_on == 1, "text_on did not survive");
    CHECK(std::strcmp(got.text, "Press any key to start") == 0,
          "the label came back as '%s' - a space split it", got.text);
    CHECK(got.text_size == 32.0f, "size is %.1f", got.text_size);
    CHECK(got.text_anchor == 7, "anchor is %d", got.text_anchor);
    CHECK(got.text_x == -12.0f && got.text_y == 20.0f, "offset is %.1f %.1f", got.text_x, got.text_y);
    CHECK(got.text_color.x > 0.89f && got.text_color.z < 0.21f, "colour did not survive");

    dai_node_desc got2{};
    CHECK(dai_doc_get(e, n2, &got2) == DAI_OK, "the key node is gone");
    CHECK(std::strcmp(got2.text, "@hud.lives") == 0, "the key came back as '%s'", got2.text);

    // A node with no Text component writes no text lines at all - the format
    // only stores what differs, and a scene full of "text 0" would be noise.
    dai_doc *f = dai_doc_create();
    dai_node_desc plain = dai_node_desc_default();
    std::snprintf(plain.name, sizeof(plain.name), "Crate");
    dai_doc_add(f, &plain);
    std::vector<char> b2(dai_doc_to_text(f, nullptr, 0) + 1);
    dai_doc_to_text(f, b2.data(), b2.size());
    CHECK(std::strstr(b2.data(), "text") == nullptr,
          "a node without a label still wrote text lines");

    dai_doc_destroy(d); dai_doc_destroy(e); dai_doc_destroy(f);
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
    // The fields that used to be silently dropped by the writer: a trigger
    // volume that saves as a solid wall is a bug you only notice in play mode,
    // and the mesh size is the whole reason a collider can differ from a model.
    rec.trigger = 1;
    rec.collider_center = { 0.0f, 0.25f, 0.0f };
    rec.render_extent = { 0.75f, 0.75f, 0.75f };
    rec.no_collider = 1;
    rec.no_rigidbody = 1;
    std::snprintf(rec.script, sizeof(rec.script), "spin.js");
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
    CHECK(back.trigger == 1, "Is Trigger did not survive being saved");
    CHECK(back.collider_center.y == 0.25f, "the collider centre did not survive being saved");
    CHECK(back.render_extent.x == 0.75f, "the mesh size did not survive being saved");
    CHECK(back.no_collider == 1 && back.no_rigidbody == 1,
          "the component toggles did not survive being saved");
    CHECK(std::strcmp(back.script, "spin.js") == 0, "the script path did not survive being saved");
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

    // a node with no_body is a pure transform/graphics node: no BODY, but it
    // still renders - removing the physics from a crate must not make the
    // crate vanish, which is exactly what it used to do. Children still hang
    // off it.
    dai_node_desc grp = dai_node_desc_default();
    snprintf(grp.name, sizeof(grp.name), "Group");
    grp.no_body = 1;
    grp.position = { 0, 100, 0 };
    dai_node group = dai_doc_add(d, &grp);
    dai_node under = add_named(d, "Under", { 0, 0, 0 }, group);
    dai_doc_sync_apply(sy);
    dai_entity ge = dai_doc_sync_entity(sy, group);
    CHECK(ge != DAI_INVALID_ENTITY, "a no_body node is not rendered - the mesh vanishes");
    CHECK(dai_scene_body(sc, ge) == DAI_INVALID_BODY, "a no_body node got a rigid body");
    {
        // and it is drawn where the document puts it
        std::vector<dai_render_instance> inst(64);
        uint32_t ni = dai_scene_instances(sc, inst.data(), 64, 1.0f);
        int found = 0;
        for (uint32_t i = 0; i < ni; ++i)
            if (std::fabs(inst[i].position.y - 100.0f) < 0.5f) found = 1;
        CHECK(found, "the render-only node is not in the instance list");
    }
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

    // ---- 12b. a material takes the palette's guess away --------------------
    //
    // A renderable with no colour of its own is given one out of a palette, so
    // a scene of untextured boxes is readable instead of uniformly grey. The
    // shader MULTIPLIES that colour onto the material, and the day the object
    // got a material file the guess turned into a lie: the example room's four
    // walls wear the same near white raufaser map and came out pink, mint and
    // gold, one palette entry each. So a material is the authority - unless
    // somebody actually chose a colour, which still tints.
    std::printf("material vs. the palette colour\n");
    {
        dai_node mn = add_named(d, "Painted", { 12, 0, 0 });
        dai_doc_sync_apply(sy);
        dai_entity me = dai_doc_sync_entity(sy, mn);
        CHECK(me != DAI_INVALID_ENTITY, "no entity for the painted node");

        auto colour_of = [&](dai_entity want) {
            dai_render_instance inst[64];
            uint32_t cnt = dai_scene_instances(sc, inst, 64, 1.0f);
            (void)want;
            // The node stands at x = 12 on its own, so the instance is found
            // by where it is rather than by an entity id the render list does
            // not carry.
            for (uint32_t i = 0; i < cnt; ++i)
                if (std::fabs(inst[i].position.x - 12.0f) < 1e-2f)
                    return inst[i].color;
            return dai_vec3{ -1, -1, -1 };
        };

        dai_vec3 auto_c = colour_of(me);
        CHECK(auto_c.x >= 0.0f, "the painted node draws no instance");
        CHECK(auto_c.x != 1.0f || auto_c.y != 1.0f || auto_c.z != 1.0f,
              "a node with no material and no colour should get a palette colour, got white");

        dai_scene_set_material(sc, me, 7);
        dai_vec3 with_mat = colour_of(me);
        CHECK(with_mat.x == 1.0f && with_mat.y == 1.0f && with_mat.z == 1.0f,
              "a material must draw at full colour, got %.2f %.2f %.2f - the palette "
              "guess is still multiplying the maps",
              (double)with_mat.x, (double)with_mat.y, (double)with_mat.z);

        // A colour somebody chose is not a guess and survives the material.
        dai_scene_set_color(sc, me, dai_vec3{ 0.9f, 0.2f, 0.2f });
        dai_vec3 tinted = colour_of(me);
        CHECK(std::fabs(tinted.x - 0.9f) < 1e-4f && std::fabs(tinted.y - 0.2f) < 1e-4f,
              "a chosen colour must still tint a material, got %.2f %.2f %.2f",
              (double)tinted.x, (double)tinted.y, (double)tinted.z);
    }

    // ---- 13. the asset resolver -------------------------------------------
    std::printf("asset resolver\n");
    {
        // Two pieces on purpose: an imported model is usually more than one
        // object, and one scene node has to be able to draw all of them.
        struct Res {
            static uint32_t fn(const char *path, dai_render_part *out, uint32_t max, void *user) {
                int *calls = (int *)user;
                ++*calls;
                if (std::strcmp(path, "models/known.glb") != 0) return 0;
                const dai_render_part parts[2] = {
                    { 42, 7, { 0, 0, 0 }, { 0, 0, 0, 1 }, { 2, 2, 2 } },
                    { 43, 8, { 1, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } },
                };
                for (uint32_t i = 0; i < 2 && out && i < max; ++i) out[i] = parts[i];
                return 2;
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
        int found_second = 0;
        for (uint32_t i = 0; i < ni; ++i) {
            if (inst[i].mesh == 42 && inst[i].material == 7) found_known = 1;
            if (inst[i].mesh == 43 && inst[i].material == 8) found_second = 1;
        }
        CHECK(found_known, "the resolved mesh and material never reached a render instance");
        CHECK(found_second, "the model's second piece was dropped - one node must draw all of them");
        CHECK(dai_scene_part_count(sc, ke) == 2,
              "the entity carries %u pieces, the asset has 2", dai_scene_part_count(sc, ke));
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

    // ---- 14. prefabs -------------------------------------------------------
    // The point of a prefab is leverage: a hundred crates cost a hundred lines,
    // and fixing the crate fixes all hundred. So the checks are about what is
    // NOT in the scene file, and about a change to the original reaching the
    // instances.
    std::printf("prefabs\n");
    {
        dai_doc *lib = dai_doc_create();
        dai_node_desc cd = dai_node_desc_default();
        snprintf(cd.name, sizeof(cd.name), "CrateBody");
        cd.half_extent = { 0.5f, 0.5f, 0.5f };
        dai_node body = dai_doc_add(lib, &cd);
        dai_node_desc ld = dai_node_desc_default();
        snprintf(ld.name, sizeof(ld.name), "CrateLid");
        ld.parent = body;
        ld.position = { 0, 1, 0 };
        dai_doc_add(lib, &ld);

        const char *pf = "/tmp/dai_prefab_crate.scene";
        CHECK(dai_doc_prefab_save(lib, body, pf) == DAI_OK, "saving the prefab failed");
        dai_doc_destroy(lib);

        dai_doc *scene = dai_doc_create();
        dai_node_desc ground = dai_node_desc_default();
        snprintf(ground.name, sizeof(ground.name), "Ground");
        dai_doc_add(scene, &ground);

        char perr[256] = { 0 };
        dai_node inst_a = dai_doc_prefab_instantiate(scene, pf, 0, ".", perr, sizeof(perr));
        CHECK(inst_a != 0, "instantiating the prefab failed: %s", perr);
        dai_node inst_b = dai_doc_prefab_instantiate(scene, pf, 0, ".", perr, sizeof(perr));
        CHECK(inst_b != 0 && inst_b != inst_a, "the second instance did not get its own nodes");
        // Two pieces per instance, plus the ground. There is no third node:
        // the prefab's ROOT is the instance now. The empty wrapper this test
        // used to count was the "parent inside a parent" every instantiate
        // left behind, and the click that was supposed to select the crate
        // kept landing on it.
        CHECK(dai_doc_count(scene) == 1 + 2 * 2, "the scene has %u nodes, expected 5",
              dai_doc_count(scene));
        CHECK(dai_doc_undo_depth(scene) >= 2, "each instantiate should be one undo step");

        // The instance's pieces must NOT be written into the scene file.
        const char *sp = "/tmp/dai_prefab_scene.scene";
        CHECK(dai_doc_save(scene, sp) == DAI_OK, "saving the scene failed");
        size_t text_len = dai_doc_to_text(scene, nullptr, 0);
        std::vector<char> text(text_len + 1, 0);
        dai_doc_to_text(scene, text.data(), text.size());
        CHECK(std::strstr(text.data(), "CrateLid") == nullptr,
              "the prefab's children were written into the scene - the reference bought nothing");
        CHECK(std::strstr(text.data(), "prefab ") != nullptr,
              "the scene does not record the prefab reference at all");

        // Reopening expands them again.
        dai_doc *back = dai_doc_create();
        char lerr[256] = { 0 };
        CHECK(dai_doc_load(back, sp, lerr, sizeof(lerr)) == DAI_OK, "reopening failed: %s", lerr);
        CHECK(dai_doc_count(back) == dai_doc_count(scene),
              "reopened scene has %u nodes, the original had %u",
              dai_doc_count(back), dai_doc_count(scene));
        CHECK(dai_doc_find(back, "CrateLid") != 0, "the prefab was not expanded on load");

        // Change the original: every instance has to follow.
        dai_doc *lib2 = dai_doc_create();
        CHECK(dai_doc_load(lib2, pf, lerr, sizeof(lerr)) == DAI_OK, "reopening the prefab failed");
        dai_node lid = dai_doc_find(lib2, "CrateLid");
        CHECK(lid != 0, "the prefab lost its lid");
        dai_node_desc lrec{};
        dai_doc_get(lib2, lid, &lrec);
        snprintf(lrec.name, sizeof(lrec.name), "CrateHatch");
        dai_doc_set(lib2, lid, &lrec);
        CHECK(dai_doc_save(lib2, pf) == DAI_OK, "resaving the prefab failed");
        dai_doc_destroy(lib2);

        uint32_t rebuilt = dai_doc_prefab_reload(back, ".");
        CHECK(rebuilt == 2, "%u instances rebuilt, expected 2", rebuilt);
        CHECK(dai_doc_find(back, "CrateHatch") != 0,
              "the edited prefab did not reach the instances");
        CHECK(dai_doc_find(back, "CrateLid") == 0, "the old piece is still there after a reload");
        CHECK(dai_doc_count(back) == 5, "reload changed the node count to %u", dai_doc_count(back));

        // A prefab that contains itself must be refused, not recursed into.
        dai_doc *evil = dai_doc_create();
        dai_node_desc ed = dai_node_desc_default();
        snprintf(ed.name, sizeof(ed.name), "Loop");
        snprintf(ed.prefab, sizeof(ed.prefab), "dai_prefab_loop.scene");
        dai_doc_add(evil, &ed);
        CHECK(dai_doc_save(evil, "/tmp/dai_prefab_loop.scene") == DAI_OK, "saving the loop failed");
        dai_doc_destroy(evil);
        dai_doc *loaded = dai_doc_create();
        char eerr[256] = { 0 };
        dai_result lr = dai_doc_load(loaded, "/tmp/dai_prefab_loop.scene", eerr, sizeof(eerr));
        CHECK(lr == DAI_OK, "a self referencing prefab took the whole load down");
        CHECK(dai_doc_count(loaded) < 20, "a self referencing prefab expanded %u times",
              dai_doc_count(loaded));
        dai_doc_destroy(loaded);

        // A missing prefab leaves the instance node, empty.
        dai_doc *miss = dai_doc_create();
        dai_node_desc md = dai_node_desc_default();
        snprintf(md.name, sizeof(md.name), "Gone");
        snprintf(md.prefab, sizeof(md.prefab), "no_such_prefab.scene");
        dai_doc_add(miss, &md);
        dai_doc_save(miss, "/tmp/dai_prefab_missing.scene");
        dai_doc_destroy(miss);
        dai_doc *miss2 = dai_doc_create();
        CHECK(dai_doc_load(miss2, "/tmp/dai_prefab_missing.scene", eerr, sizeof(eerr)) == DAI_OK,
              "a missing prefab made the whole scene fail to open");
        CHECK(dai_doc_count(miss2) == 1, "the instance node did not survive a missing prefab");
        dai_doc_destroy(miss2);

        std::printf("  2 instances, %u nodes, scene file %zu bytes without the pieces\n",
                    dai_doc_count(back), text_len);
        dai_doc_destroy(back);
        dai_doc_destroy(scene);
    }

    // ---- editing a node must not repaint it black -------------------------
    //
    // A node with no colour of its own gets one from the scene's palette when
    // it spawns. The document still holds zero, and the sync used to push that
    // zero back on every update - so moving a crate in the editor turned it
    // black. Nothing crashed, nothing failed to build, the scene just went dark
    // one object at a time.
    {
        dai_doc *cd = dai_doc_create();
        dai_scene *cs = dai_scene_create(w);
        dai_doc_sync *cy = dai_doc_sync_create(cd, cs);

        dai_node_desc nd = dai_node_desc_default();
        std::snprintf(nd.name, sizeof(nd.name), "Uncoloured");
        nd.motion = DAI_DYNAMIC;
        dai_node n = dai_doc_add(cd, &nd);
        dai_doc_sync_apply(cy);

        dai_render_instance inst[16];
        uint32_t ni = dai_scene_instances(cs, inst, 16, 0.0f);
        CHECK(ni >= 1, "the uncoloured node did not spawn");
        dai_vec3 spawned = ni ? inst[0].color : dai_vec3{ 0, 0, 0 };
        CHECK(spawned.x + spawned.y + spawned.z > 0.01f,
              "a node with no colour spawned black (%.2f %.2f %.2f)",
              spawned.x, spawned.y, spawned.z);

        // Move it, exactly as dragging a gizmo does.
        dai_node_desc edit{};
        CHECK(dai_doc_get(cd, n, &edit) == DAI_OK, "could not read the node back");
        edit.position = { 3.0f, 1.0f, 0.0f };
        dai_doc_set(cd, n, &edit);
        dai_doc_sync_apply(cy);

        ni = dai_scene_instances(cs, inst, 16, 0.0f);
        dai_vec3 after = ni ? inst[0].color : dai_vec3{ 0, 0, 0 };
        CHECK(after.x + after.y + after.z > 0.01f,
              "moving an uncoloured node painted it black (%.2f %.2f %.2f)",
              after.x, after.y, after.z);
        CHECK(std::fabs(after.x - spawned.x) < 0.001f &&
              std::fabs(after.y - spawned.y) < 0.001f &&
              std::fabs(after.z - spawned.z) < 0.001f,
              "moving a node changed its colour");

        // An explicit colour must still win.
        edit.color = { 0.9f, 0.1f, 0.1f };
        dai_doc_set(cd, n, &edit);
        dai_doc_sync_apply(cy);
        ni = dai_scene_instances(cs, inst, 16, 0.0f);
        CHECK(ni && inst[0].color.x > 0.8f && inst[0].color.y < 0.2f,
              "an explicit colour did not reach the scene");
        std::printf("  uncoloured node keeps its palette colour across edits\n");

        dai_doc_sync_destroy(cy);
        dai_doc_destroy(cd);
        dai_scene_destroy(cs);
    }

    dai_doc_sync_destroy(sy);
    dai_doc_destroy(d);
    dai_scene_destroy(sc);
    dai_destroy(w);

    test_text_component();
    test_unique_names();
    test_blockout();
    test_blockout_csg_measured();
    test_blockout_csg_matrix();
    test_modifier_bevel();
    test_blockout_host();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
