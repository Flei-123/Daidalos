// The four things the editor kept getting wrong about "where is this object".
//
//   ./build/test_editor_live
//
// Every case here was a screenshot first: a green collider frame pointing a
// different way than the cube it outlines, a crate that "spawns" higher every
// time you drag it during play, two boxes standing inside each other, and a
// cylinder resting on a corner it does not have. The assertions are the
// measurements that came out of chasing them, so a regression is a number that
// moves and not an opinion.

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

// Angle between two orientations, in degrees. Sign of the dot product is
// ignored: q and -q are the same rotation, and a test that does not know that
// fails at random.
static float quat_angle(dai_quat a, dai_quat b) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    d = std::fabs(d);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * std::acos(d) * 57.2957795f;
}

struct Rig {
    dai_world    *w = nullptr;
    dai_scene    *sc = nullptr;
    dai_doc      *doc = nullptr;
    dai_doc_sync *sync = nullptr;
    dai_editor   *ed = nullptr;

    void make() {
        dai_config cfg{};
        cfg.tick_hz = 60; cfg.max_bodies = 256; cfg.physics_threads = 1; cfg.seed = 3;
        dai_create(&cfg, &w);
        sc = dai_scene_create(w);
        doc = dai_doc_create();
        sync = dai_doc_sync_create(doc, sc);
        ed = dai_editor_create(doc, sync);
        dai_editor_camera(ed, dai_vec3{ 0, 3, 14 }, dai_vec3{ 0, 3, 0 }, dai_vec3{ 0, 1, 0 },
                          60.0f, 0.1f, 200.0f, 800.0f, 600.0f);
    }
    void kill() {
        dai_editor_destroy(ed); dai_doc_sync_destroy(sync);
        dai_doc_destroy(doc); dai_scene_destroy(sc); dai_destroy(w);
    }
    dai_body body_of(dai_node n) const {
        return dai_scene_body(sc, dai_doc_sync_entity(sync, n));
    }
    dai_transform body_transform(dai_node n) const {
        dai_transform t{};
        dai_body_get(w, body_of(n), &t);
        return t;
    }
    dai_node add(dai_vec3 pos, dai_vec3 he, int motion, int shape = DAI_SHAPE_BOX) {
        dai_node_desc r = dai_node_desc_default();
        r.motion = motion; r.half_extent = he; r.position = pos; r.shape = shape;
        return dai_doc_add(doc, &r);
    }
};

// ---------------------------------------------------------------- BUG 1
// The collider wireframe is built from a world transform, the mesh is drawn
// from the body. Whatever the editor answers as "the live transform" has to be
// the one the mesh uses, or the two are drawn in different places.
static void test_live_rotation() {
    std::printf("live transform: rotation follows the body\n");
    Rig r; r.make();
    dai_node n = r.add({ 0, 6, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_DYNAMIC);
    dai_doc_sync_apply(r.sync);
    dai_step(r.w);

    // ---- edit mode: a gizmo rotate must land on the body as well as the doc.
    dai_editor_select(r.ed, n, 0);
    dai_editor_gizmo_mode(r.ed, DAI_GIZMO_ROTATE);
    float cx, cy;
    CHECK(dai_editor_project(r.ed, dai_editor_selection_center(r.ed), &cx, &cy) != 0,
          "the selection centre is behind the camera");
    dai_editor_drag_begin(r.ed, DAI_AXIS_Y, cx + 60.0f, cy);
    dai_editor_drag_update(r.ed, cx, cy + 60.0f);
    dai_editor_drag_end(r.ed);

    dai_quat docq{ 0, 0, 0, 1 };
    dai_doc_world_transform(r.doc, n, nullptr, &docq, nullptr);
    dai_quat bodyq = r.body_transform(n).rotation;
    CHECK(quat_angle(docq, bodyq) < 0.5f,
          "after a gizmo rotate the document and the body disagree by %.3f deg",
          quat_angle(docq, bodyq));
    CHECK(quat_angle(docq, dai_quat{ 0, 0, 0, 1 }) > 5.0f,
          "the rotate drag did not rotate anything - the test proves nothing");

    dai_quat liveq{ 0, 0, 0, 1 };
    CHECK(dai_editor_live_transform(r.ed, n, nullptr, &liveq, nullptr) != 0,
          "live_transform refused a node that exists");
    CHECK(quat_angle(liveq, bodyq) < 0.5f,
          "while editing, live_transform is %.3f deg off the body", quat_angle(liveq, bodyq));

    // ---- play mode: the document is deliberately stale, the body is not.
    dai_editor_play(r.ed);
    dai_body b = r.body_of(n);
    dai_body_set_velocity(r.w, b, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 4.0f, 0 });
    for (int i = 0; i < 30; ++i) dai_step(r.w);

    dai_doc_world_transform(r.doc, n, nullptr, &docq, nullptr);
    bodyq = r.body_transform(n).rotation;
    // This is the bug, stated as a fact: the two DO disagree during play, which
    // is exactly why a wireframe built from the document is wrong.
    CHECK(quat_angle(docq, bodyq) > 30.0f,
          "the body did not turn during play (%.2f deg) - the rest of this test is vacuous",
          quat_angle(docq, bodyq));

    dai_vec3 livep{}; dai_vec3 lives{};
    CHECK(dai_editor_live_transform(r.ed, n, &livep, &liveq, &lives) != 0,
          "live_transform failed during play");
    // Jolt and Talos integrate the spin through different substeps, so the
    // committed rotation after N ticks differs slightly per backend (Jolt
    // <0.01 deg, Talos ~0.04 deg - invisible on a wireframe). The check that
    // must NEVER pass is document-vs-body: that one is 30+ deg.
    const float rot_tol = std::strcmp(dai_backend_name(r.w), "talos") == 0 ? 0.1f : 0.01f;
    CHECK(quat_angle(liveq, bodyq) < rot_tol,
          "during play, live_transform reports the DOCUMENT rotation (%.3f deg off the body)",
          quat_angle(liveq, bodyq));
    dai_vec3 bp = r.body_transform(n).position;
    CHECK(std::fabs(livep.y - bp.y) < 1e-4f,
          "live_transform position %.4f != body %.4f", livep.y, bp.y);
    CHECK(std::fabs(lives.x - 1.0f) < 1e-5f, "scale should come from the document");

    // The narrow question must still give the same answer as the wide one.
    dai_vec3 onlyp{};
    dai_editor_live_position(r.ed, n, &onlyp);
    CHECK(std::fabs(onlyp.y - livep.y) < 1e-6f,
          "live_position and live_transform disagree (%.6f vs %.6f)", onlyp.y, livep.y);
    r.kill();
}

// A collider centre offset moves the body away from the object. Undoing that
// offset with the document's rotation instead of the body's was the other half
// of the same bug: it moved the answer sideways as the body turned.
static void test_live_collider_center() {
    std::printf("live transform: the centre offset is undone with the BODY's rotation\n");
    Rig r; r.make();
    dai_node_desc d = dai_node_desc_default();
    d.motion = DAI_DYNAMIC;
    d.half_extent = { 0.5f, 0.5f, 0.5f };
    d.position = { 0, 6, 0 };
    d.collider_center = { 0.75f, 0, 0 };
    dai_node n = dai_doc_add(r.doc, &d);
    dai_doc_sync_apply(r.sync);
    dai_step(r.w);

    dai_editor_play(r.ed);
    dai_body b = r.body_of(n);
    // A quarter turn about Y: the offset now points along -Z instead of +X, so
    // an implementation that undoes it with the stale document rotation is off
    // by 0.75 * sqrt(2) and it is impossible to miss.
    dai_body_set_velocity(r.w, b, dai_vec3{ 0, 0, 0 }, dai_vec3{ 0, 2.0f, 0 });
    for (int i = 0; i < 30; ++i) dai_step(r.w);

    dai_transform t = r.body_transform(n);
    dai_vec3 livep{};
    dai_quat liveq{ 0, 0, 0, 1 };
    CHECK(dai_editor_live_transform(r.ed, n, &livep, &liveq, nullptr) != 0, "live_transform failed");
    // Stated first and separately: an implementation that undoes the offset
    // with the document rotation AND reports that same document rotation is
    // internally consistent and still wrong, so the reconstruction below would
    // happily pass it.
    CHECK(quat_angle(liveq, t.rotation) < 0.01f,
          "the reported rotation is %.3f deg off the body", quat_angle(liveq, t.rotation));
    dai_quat docq{ 0, 0, 0, 1 };
    dai_doc_world_transform(r.doc, n, nullptr, &docq, nullptr);
    CHECK(quat_angle(docq, t.rotation) > 30.0f,
          "the body only turned %.2f deg - the document is not stale enough to test",
          quat_angle(docq, t.rotation));

    // Reconstruct the body position from the answer: object + offset rotated by
    // the object's own orientation has to be the body again.
    dai_vec3 u{ liveq.x, liveq.y, liveq.z };
    dai_vec3 off{ 0.75f, 0, 0 };
    dai_vec3 uv{ u.y*off.z - u.z*off.y, u.z*off.x - u.x*off.z, u.x*off.y - u.y*off.x };
    dai_vec3 uuv{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
    dai_vec3 rot_off{ off.x + 2.0f*(liveq.w*uv.x + uuv.x),
                      off.y + 2.0f*(liveq.w*uv.y + uuv.y),
                      off.z + 2.0f*(liveq.w*uv.z + uuv.z) };
    float dx = (livep.x + rot_off.x) - t.position.x;
    float dz = (livep.z + rot_off.z) - t.position.z;
    CHECK(std::sqrt(dx*dx + dz*dz) < 1e-3f,
          "object + rotated centre offset is %.4f m away from the body", std::sqrt(dx*dx + dz*dz));
    r.kill();
}

// ---------------------------------------------------------------- BUG 2
// What an 80 pixel upward drag on the Y handle is worth in metres for a box
// sitting at height y, measured while EDITING - where the document has always
// been the right thing to read. This is the yardstick the play mode drag is
// held against, and it is deliberately produced by a different code path.
static float reference_drag(float y) {
    Rig r; r.make();
    dai_node n = r.add({ 0, y, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_KINEMATIC);
    dai_doc_sync_apply(r.sync);
    dai_editor_select(r.ed, n, 0);
    dai_editor_gizmo_mode(r.ed, DAI_GIZMO_TRANSLATE);
    float cx, cy;
    dai_editor_project(r.ed, dai_editor_selection_center(r.ed), &cx, &cy);
    dai_editor_drag_begin(r.ed, DAI_AXIS_Y, cx, cy);
    dai_editor_drag_update(r.ed, cx, cy - 80.0f);
    dai_editor_drag_end(r.ed);
    dai_vec3 p{};
    dai_doc_world_transform(r.doc, n, &p, nullptr, nullptr);
    r.kill();
    return p.y - y;
}

// Drag during play. The pointer moves the object relative to where it IS.
static void test_play_drag() {
    std::printf("play drag: the object moves from its live pose, not the document's\n");
    Rig r; r.make();
    r.add({ 0, -0.5f, 0 }, { 20, 0.5f, 20 }, DAI_STATIC);
    dai_node n = r.add({ 0, 8, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_DYNAMIC);
    dai_doc_sync_apply(r.sync);
    dai_step(r.w);

    dai_editor_select(r.ed, n, 0);
    dai_editor_gizmo_mode(r.ed, DAI_GIZMO_TRANSLATE);
    dai_editor_play(r.ed);
    for (int i = 0; i < 10; ++i) dai_step(r.w);

    dai_vec3 doc_before{};
    dai_doc_world_transform(r.doc, n, &doc_before, nullptr, nullptr);
    float body_before = r.body_transform(n).position.y;
    CHECK(doc_before.y - body_before > 0.1f,
          "the box did not fall in 10 ticks (doc %.3f body %.3f) - nothing to prove",
          doc_before.y, body_before);

    // How far those 80 pixels are worth, measured WITHOUT the play path: an
    // identical box, parked at the height the falling one has reached, dragged
    // the same way while editing. Same camera, same world position, so the
    // pixel-to-metre mapping is identical - and the number does not come from
    // the code under test, which is the whole point. Reading the drag distance
    // back out of the editor after the drag makes the assertion true by
    // construction and proves nothing.
    const float expect = reference_drag(body_before);
    CHECK(expect > 0.5f, "the reference drag only moved %.3f m - too little to test", expect);

    dai_vec3 c = dai_editor_selection_center(r.ed);
    CHECK(std::fabs(c.y - body_before) < 1e-3f,
          "the gizmo sits at %.3f, the body is at %.3f", c.y, body_before);
    float cx, cy;
    dai_editor_project(r.ed, c, &cx, &cy);
    dai_editor_drag_begin(r.ed, DAI_AXIS_Y, cx, cy);
    dai_editor_drag_update(r.ed, cx, cy - 80.0f);
    dai_editor_drag_end(r.ed);

    float body_after = r.body_transform(n).position.y;
    CHECK(std::fabs((body_after - body_before) - expect) < 0.02f,
          "the body moved %.3f, the same drag while editing moves %.3f",
          body_after - body_before, expect);
    CHECK(std::fabs(body_after - (doc_before.y + expect)) > 0.05f,
          "the body landed on document (%.3f) + drag (%.3f) = %.3f - the old bug exactly",
          doc_before.y, expect, doc_before.y + expect);

    // Play mode is a rehearsal: the document may not have moved at all.
    dai_vec3 doc_after{};
    dai_doc_world_transform(r.doc, n, &doc_after, nullptr, nullptr);
    CHECK(std::fabs(doc_after.y - doc_before.y) < 1e-5f,
          "the drag wrote %.4f into the document during play - Stop will not restore",
          doc_after.y - doc_before.y);

    // ...and doing it again must not stack. This is the "it spawns higher every
    // time" the bug report was about: the second drag used to start from the
    // document, which the first drag had already pushed up.
    for (int i = 0; i < 10; ++i) dai_step(r.w);
    float b2 = r.body_transform(n).position.y;
    const float expect2 = reference_drag(b2);
    c = dai_editor_selection_center(r.ed);
    dai_editor_project(r.ed, c, &cx, &cy);
    dai_editor_drag_begin(r.ed, DAI_AXIS_Y, cx, cy);
    dai_editor_drag_update(r.ed, cx, cy - 80.0f);
    dai_editor_drag_end(r.ed);
    float b3 = r.body_transform(n).position.y;
    CHECK(std::fabs((b3 - b2) - expect2) < 0.02f,
          "the second drag moved the body %.3f, the same drag while editing moves %.3f",
          b3 - b2, expect2);

    // Stop still puts everything back, which is the property all of this had to
    // preserve.
    dai_editor_stop(r.ed);
    dai_vec3 doc_end{};
    dai_doc_world_transform(r.doc, n, &doc_end, nullptr, nullptr);
    CHECK(std::fabs(doc_end.y - 8.0f) < 1e-4f, "Stop left the document at %.4f, not 8", doc_end.y);
    CHECK(std::fabs(r.body_transform(n).position.y - 8.0f) < 1e-3f,
          "Stop left the body at %.4f, not 8", r.body_transform(n).position.y);
    r.kill();
}

// While editing, a drag still has to go through the document - that is what
// makes it undoable, and the play path must not have taken that away.
static void test_edit_drag_still_undoable() {
    std::printf("edit drag: still one undo step on the document\n");
    Rig r; r.make();
    dai_node n = r.add({ 0, 3, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_KINEMATIC);
    dai_doc_sync_apply(r.sync);
    dai_step(r.w);
    dai_editor_select(r.ed, n, 0);
    uint32_t depth = dai_editor_undo_depth(r.ed);

    float cx, cy;
    dai_editor_project(r.ed, dai_editor_selection_center(r.ed), &cx, &cy);
    dai_editor_drag_begin(r.ed, DAI_AXIS_Y, cx, cy);
    dai_editor_drag_update(r.ed, cx, cy - 60.0f);
    dai_editor_drag_end(r.ed);

    dai_vec3 p{};
    dai_doc_world_transform(r.doc, n, &p, nullptr, nullptr);
    CHECK(p.y > 3.2f, "the edit drag did not move the document (y = %.3f)", p.y);
    CHECK(dai_editor_undo_depth(r.ed) == depth + 1,
          "an edit drag pushed %u undo steps, expected 1", dai_editor_undo_depth(r.ed) - depth);
    CHECK(dai_editor_undo(r.ed) != 0, "undo refused");
    dai_doc_world_transform(r.doc, n, &p, nullptr, nullptr);
    CHECK(std::fabs(p.y - 3.0f) < 1e-4f, "undo left the box at %.4f", p.y);
    CHECK(std::fabs(r.body_transform(n).position.y - 3.0f) < 1e-3f,
          "undo moved the document but not the body");
    r.kill();
}

// ---------------------------------------------------------------- BUG 3
static void test_penetration() {
    std::printf("penetration: two boxes must not stand inside each other\n");
    Rig r; r.make();
    r.add({ 0, -0.5f, 0 }, { 20, 0.5f, 20 }, DAI_STATIC);
    dai_node a = r.add({ 0, 0.5f, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_DYNAMIC);
    dai_node b = r.add({ 0, 4.0f, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_DYNAMIC);
    dai_doc_sync_apply(r.sync);

    // A freshly made box draws exactly the volume it collides with. If these
    // two ever drift apart, the overlap on screen is a render bug and no
    // amount of solver tuning will fix it - so check before measuring.
    dai_node_desc rec{};
    dai_doc_get(r.doc, a, &rec);
    bool follows = !(rec.render_extent.x || rec.render_extent.y || rec.render_extent.z);
    CHECK(follows, "a fresh box has render_extent %.3f %.3f %.3f - the mesh is already unpinned",
          rec.render_extent.x, rec.render_extent.y, rec.render_extent.z);
    std::vector<dai_render_instance> inst(16);
    uint32_t ni = dai_scene_instances(r.sc, inst.data(), 16, 1.0f);
    CHECK(ni >= 3, "expected three instances, got %u", ni);
    for (uint32_t i = 0; i < ni; ++i) {
        if (inst[i].mesh != DAI_MESH_BOX) continue;
        if (inst[i].scale.x > 1.0f) continue;                 // the ground
        CHECK(std::fabs(inst[i].scale.x - 0.5f) < 1e-5f &&
              std::fabs(inst[i].scale.y - 0.5f) < 1e-5f &&
              std::fabs(inst[i].scale.z - 0.5f) < 1e-5f,
              "the drawn box is %.3f %.3f %.3f, the collider is 0.5 cubed",
              inst[i].scale.x, inst[i].scale.y, inst[i].scale.z);
    }

    float worst = 0.0f, worst_ground = 0.0f;
    for (int i = 0; i < 500; ++i) {
        dai_step(r.w);
        float pen = 1.0f - std::fabs(r.body_transform(b).position.y -
                                     r.body_transform(a).position.y);
        if (pen > worst) worst = pen;
        float pg = 0.5f - r.body_transform(a).position.y;      // ground top is y = 0
        if (pg > worst_ground) worst_ground = pg;
    }
    std::printf("  worst box/box overlap %.4f m, worst box/ground overlap %.4f m\n",
                worst, worst_ground);
    // Jolt solves contacts hard: a 3.5 m drop never compresses past ~1 cm.
    // Talos solves them SOFT (TGS, contact hertz 60 - the stability limit):
    // the same impact compresses transiently ~2 cm box/box and ~6 cm box/ground
    // and recovers within a few ticks. The REST positions below are what must
    // stay exact; the transient ceiling is per backend, from measurement.
    const bool talos = std::strcmp(dai_backend_name(r.w), "talos") == 0;
    const float pen_tol = talos ? 0.05f : 0.01f;  // gemessen 0.034 m (Editor-Stack), TalC-Repro 0.018 m
    const float ground_tol = talos ? 0.08f : 0.01f;
    CHECK(worst < pen_tol, "the boxes sank %.4f m (%.1f cm) into each other", worst, worst * 100.0f);
    CHECK(worst_ground < ground_tol, "a box sank %.4f m into the ground", worst_ground);
    float gap = std::fabs(r.body_transform(b).position.y - r.body_transform(a).position.y);
    CHECK(std::fabs(gap - 1.0f) < 0.01f,
          "at rest the centres are %.4f apart, the halves add up to 1.0", gap);
    r.kill();
}

// Freezing the mesh at the collider's size must not change that size. The
// inspector pins render_extent to the collider the moment the collider is
// touched; if that pin was off by anything, a box would change size the first
// time anyone opened the collider fold.
static void test_freeze_keeps_size() {
    std::printf("mesh freeze: pinning the mesh does not resize it\n");
    Rig r; r.make();
    dai_node n = r.add({ 0, 3, 0 }, { 0.5f, 0.5f, 0.5f }, DAI_KINEMATIC);
    dai_doc_sync_apply(r.sync);

    std::vector<dai_render_instance> before(8);
    uint32_t nb = dai_scene_instances(r.sc, before.data(), 8, 1.0f);
    CHECK(nb == 1, "expected one instance");

    // What the inspector does when the collider is edited: the mesh stops
    // following and keeps the size it had.
    dai_node_desc rec{};
    dai_doc_get(r.doc, n, &rec);
    rec.render_extent = rec.half_extent;
    rec.half_extent = { 0.4f, 0.4f, 0.4f };       // and the collider shrinks
    dai_doc_set(r.doc, n, &rec);
    dai_doc_sync_apply(r.sync);

    std::vector<dai_render_instance> after(8);
    dai_scene_instances(r.sc, after.data(), 8, 1.0f);
    CHECK(std::fabs(after[0].scale.x - before[0].scale.x) < 1e-6f &&
          std::fabs(after[0].scale.y - before[0].scale.y) < 1e-6f &&
          std::fabs(after[0].scale.z - before[0].scale.z) < 1e-6f,
          "freezing the mesh changed it from %.4f to %.4f", before[0].scale.x, after[0].scale.x);
    r.kill();
}

// ---------------------------------------------------------------- BUG 4
static void test_cylinder() {
    std::printf("cylinder: a real collider, not a box wearing a cylinder mesh\n");
    Rig r; r.make();
    r.add({ 0, -0.5f, 0 }, { 20, 0.5f, 20 }, DAI_STATIC);
    // radius 0.5, half height 1.0 - the same convention the capsule uses.
    dai_node n = r.add({ 0, 4, 0 }, { 0.5f, 1.0f, 0.0f }, DAI_DYNAMIC, DAI_SHAPE_CYLINDER);
    dai_doc_sync_apply(r.sync);

    // The mesh has to be the cylinder mesh, at the cylinder's proportions:
    // radius on X and Z, half height on Y. Getting this wrong is how the
    // wireframe and the model ended up different sizes.
    std::vector<dai_render_instance> inst(8);
    uint32_t ni = dai_scene_instances(r.sc, inst.data(), 8, 1.0f);
    int found = 0;
    for (uint32_t i = 0; i < ni; ++i) {
        if (inst[i].mesh != DAI_MESH_CYLINDER) continue;
        ++found;
        CHECK(std::fabs(inst[i].scale.x - 0.5f) < 1e-5f &&
              std::fabs(inst[i].scale.y - 1.0f) < 1e-5f &&
              std::fabs(inst[i].scale.z - 0.5f) < 1e-5f,
              "the cylinder mesh is scaled %.3f %.3f %.3f, expected 0.5 1.0 0.5",
              inst[i].scale.x, inst[i].scale.y, inst[i].scale.z);
    }
    CHECK(found == 1, "the cylinder shape did not select DAI_MESH_CYLINDER (%d found)", found);

    for (int i = 0; i < 400; ++i) dai_step(r.w);
    float y = r.body_transform(n).position.y;
    // A cylinder of half height 1 standing on a floor at y = 0 rests with its
    // centre at 1.0. A box collider of the same half extents rests there too -
    // so the load bearing half of this test is the one below, where it is
    // tipped onto its side: a box would stand on a corner at 1.0, a cylinder
    // lies on its curved wall at its RADIUS, 0.5.
    CHECK(std::fabs(y - 1.0f) < 0.02f, "the upright cylinder rests at %.4f, expected 1.0", y);

    r.kill();

    // The half of this that a box CANNOT fake. Resting height is no good as a
    // test: a box of half extents 0.5 x 1.0 x (z unused, so 0.5) lying on its
    // side rests at 0.5 as well, and the assertion passes while the collider is
    // still wrong. Rolling is the difference the user actually reported - "it
    // stops too early". Laid on its side and pushed, a cylinder rolls; a box
    // slides to a halt against friction.
    Rig r2; r2.make();
    r2.add({ 0, -0.5f, 0 }, { 60, 0.5f, 20 }, DAI_STATIC);
    dai_node_desc roll = dai_node_desc_default();
    roll.motion = DAI_DYNAMIC;
    roll.shape = DAI_SHAPE_CYLINDER;
    roll.half_extent = { 0.5f, 1.0f, 0.0f };
    roll.position = { 0, 0.5f, 0 };
    // 90 degrees about X puts the local Y axis along Z, so it lies down and
    // rolls about Z when pushed along X.
    roll.rotation = { std::sin(0.7853982f), 0, 0, std::cos(0.7853982f) };
    dai_node rn = dai_doc_add(r2.doc, &roll);
    dai_doc_sync_apply(r2.sync);
    dai_body_set_velocity(r2.w, r2.body_of(rn), dai_vec3{ 5.0f, 0, 0 }, dai_vec3{ 0, 0, 0 });
    for (int i = 0; i < 400; ++i) dai_step(r2.w);
    dai_transform rt = r2.body_transform(rn);
    std::printf("  pushed at 5 m/s it rolled %.2f m (a box collider slides ~2 m)\n", rt.position.x);
    CHECK(rt.position.x > 8.0f,
          "it only travelled %.2f m - that is a box sliding, not a cylinder rolling", rt.position.x);
    CHECK(std::fabs(rt.position.y - 0.5f) < 0.03f,
          "while rolling it sits at %.4f, its radius is 0.5", rt.position.y);
    r2.kill();
}

// The scene file stores the shape as a number. A new value at the end of the
// enum must round trip, and an unknown one must be refused rather than quietly
// becoming a box.
static void test_cylinder_roundtrip() {
    std::printf("cylinder: survives save/load, and a bad shape is refused\n");
    dai_doc *d = dai_doc_create();
    dai_node_desc r = dai_node_desc_default();
    r.shape = DAI_SHAPE_CYLINDER;
    r.half_extent = { 0.25f, 1.5f, 0.0f };
    std::snprintf(r.name, sizeof(r.name), "barrel");
    dai_doc_add(d, &r);

    char buf[4096];
    size_t n = dai_doc_to_text(d, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf), "serialised to %zu bytes", n);

    dai_doc *d2 = dai_doc_create();
    char err[256] = { 0 };
    CHECK(dai_doc_from_text(d2, buf, n, err, sizeof(err)) == DAI_OK, "reload failed: %s", err);
    dai_node found = dai_doc_find(d2, "barrel");
    CHECK(found != DAI_INVALID_NODE, "the node did not come back");
    dai_node_desc back{};
    dai_doc_get(d2, found, &back);
    CHECK(back.shape == DAI_SHAPE_CYLINDER, "the shape came back as %d", back.shape);
    CHECK(std::fabs(back.half_extent.y - 1.5f) < 1e-5f, "the half height came back as %.3f",
          back.half_extent.y);

    // A whole, valid scene whose only problem is the shape number - anything
    // less and the parser rejects it before it ever looks at the shape, which
    // is a test that passes for the wrong reason.
    dai_doc *d3 = dai_doc_create();
    const char *future = "daidalos-scene 1\nnext-id 2\n\nnode 1\n  name future\n  shape 97\nend\n";
    CHECK(dai_doc_from_text(d3, future, std::strlen(future), err, sizeof(err)) != DAI_OK,
          "a shape this build does not have was accepted and quietly became a box");
    const char *good = "daidalos-scene 1\nnext-id 2\n\nnode 1\n  name ok\n  shape 4\nend\n";
    CHECK(dai_doc_from_text(d3, good, std::strlen(good), err, sizeof(err)) == DAI_OK,
          "the same scene with a shape this build DOES have was refused: %s", err);
    dai_doc_destroy(d); dai_doc_destroy(d2); dai_doc_destroy(d3);
}

int main() {
    test_live_rotation();
    test_live_collider_center();
    test_play_drag();
    test_edit_drag_still_undoable();
    test_penetration();
    test_freeze_keeps_size();
    test_cylinder();
    test_cylinder_roundtrip();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
