// The modifier stack as the DOCUMENT holds it: the scene lines, the undo
// step, the conversion into daimod::Stack, the property names a script uses
// and the list the inspector draws.
//
// Included by tests/test_editor_ui.cpp, after its CHECK macro and before
// main(), and called from there - the geometry itself has its own two suites
// (tests/modifier_bevel_cases.hpp, tests/modifier_dup_cases.hpp) and is taken
// as given here. What this file is about is the seam between them:
//
//   * a scene with a stack in it round trips BYTE identical, one without
//     loads exactly as it did before the stack existed, and a `mod` line with
//     a type or a slot this build has no room for is REJECTED rather than
//     quietly dropped;
//   * the same stack computed twice is the same vertices and the same
//     indices, memcmp;
//   * an entry that is switched off changes nothing at all - bit for bit;
//   * the ORDER is the result: array-then-bevel and bevel-then-array are two
//     different meshes and both of them are counted, not eyeballed;
//   * undo of one parameter gives the old mesh back bit for bit;
//   * "modifier.0.width" and "modifier.0.amount" are the same four bytes, and
//     writing one of them measurably moves the triangles;
//   * at 1100x700 the list's last field is inside the inspector.
//
// No renderer is needed for any of it: a mesh here is daimod::build() on the
// solid the node's fields stand for, which is exactly what the host uploads.

#ifndef DAI_MODIFIER_STACK_CASES_HPP
#define DAI_MODIFIER_STACK_CASES_HPP

#include "dai_modifier.h"

#include <cstring>
#include <string>
#include <vector>

namespace modstack {

// ---- the property table, driven the way a host drives it -----------------
// The seam in include/dai_blockout_props.inl is the ONE table the bridge, the
// behaviours and this test go through. It is included here inside the same
// five lambdas examples/editor_demo.cpp gives it, so what is tested is the
// table itself and not a copy of it that agrees with the test.
enum PropKind { P_NONE, P_NUM, P_VEC, P_STR };
struct PropRef {
    PropKind  kind = P_NONE;
    float    *f = nullptr;
    int      *i = nullptr;
    dai_vec3 *v = nullptr;
    char     *str = nullptr;
    size_t    str_len = 0;
    int       bool_of_int = 0;
    int       invert = 0;
};

inline PropRef prop_ref(dai_node_desc &r, const char *name) {
    PropRef p;
    auto num = [&](float *f) { p.kind = P_NUM; p.f = f; return p; };
    auto ival = [&](int *i) { p.kind = P_NUM; p.i = i; return p; };
    auto flag = [&](int *i, int inv) { p.kind = P_NUM; p.i = i; p.bool_of_int = 1; p.invert = inv; return p; };
    auto vec = [&](dai_vec3 *v) { p.kind = P_VEC; p.v = v; return p; };
    auto text = [&](char *c, size_t n) { p.kind = P_STR; p.str = c; p.str_len = n; return p; };
    (void)text;          // the blockout table answers no string properties
    if (!name) return p;
    #include "dai_blockout_props.inl"
    return p;
}

inline void prop_set_num(dai_node_desc &r, const char *name, double value) {
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return;
    if (p.f) { *p.f = (float)value; return; }
    if (!p.i) return;
    if (p.bool_of_int) {
        int on = value != 0.0 ? 1 : 0;
        if (p.invert)       *p.i = on ? 0 : 1;
        else if (!on)       *p.i = 0;
        else if (*p.i == 0) *p.i = 1;
    } else {
        *p.i = (int)value;
    }
}

inline double prop_get_num(dai_node_desc &r, const char *name, double fallback) {
    PropRef p = prop_ref(r, name);
    if (p.kind != P_NUM) return fallback;
    if (p.f) return (double)*p.f;
    if (!p.i) return fallback;
    int v = *p.i;
    if (p.bool_of_int) return (p.invert ? (v == 0) : (v != 0)) ? 1.0 : 0.0;
    return (double)v;
}

inline void prop_set_vec(dai_node_desc &r, const char *name, double x, double y, double z) {
    PropRef p = prop_ref(r, name);
    if (p.kind != P_VEC || !p.v) return;
    p.v->x = (float)x; p.v->y = (float)y; p.v->z = (float)z;
}

// ---- what a node's fields build -----------------------------------------
// shape_of() and stack_of() out of include/dai_blockout_host.inl - the host's
// own conversions, not a second reading of them.
inline daiblock::Mesh mesh_of(const dai_node_desc &r) {
    daiblock::Solid s = daiblock::build(daiblockhost::shape_of(r));
    return daimod::build(daiblockhost::stack_of(r), s);
}

inline bool same_mesh(const daiblock::Mesh &a, const daiblock::Mesh &b) {
    if (a.verts.size() != b.verts.size() || a.idx.size() != b.idx.size()) return false;
    if (!a.verts.empty() &&
        std::memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(dai_vertex)) != 0)
        return false;
    if (!a.idx.empty() &&
        std::memcmp(a.idx.data(), b.idx.data(), a.idx.size() * sizeof(uint32_t)) != 0)
        return false;
    return true;
}

inline dai_modifier mod_bevel(float width, int segments, float angle) {
    dai_modifier m{};
    m.type = DAI_MOD_BEVEL; m.amount = width; m.count = segments; m.angle = angle;
    return m;
}
inline dai_modifier mod_array(int copies, dai_vec3 step) {
    dai_modifier m{};
    m.type = DAI_MOD_ARRAY; m.count = copies; m.offset = step; m.axis = 1;
    return m;
}
inline dai_modifier mod_mirror(int axis, float weld) {
    dai_modifier m{};
    m.type = DAI_MOD_MIRROR; m.axis = axis; m.amount = weld;
    return m;
}

inline std::string doc_text(dai_doc *d) {
    size_t n = dai_doc_to_text(d, nullptr, 0);
    std::vector<char> buf(n + 2, 0);
    dai_doc_to_text(d, buf.data(), buf.size());
    return std::string(buf.data());
}

} // namespace modstack

// `doc`, `sync`, `ed`, `panels` and `ui` are the same objects the rest of the
// suite drives; the section leaves the document as it found it.
static void modifier_stack_cases(dai_doc *doc, dai_doc_sync *sync, dai_editor *ed,
                                 dai_editor_ui *panels, dai_ui *ui) {
    using namespace modstack;
    std::printf("modifier stack\n");

    // A box the whole section works on: 1 m cube, standing where it is put.
    auto cube_desc = [](const char *name) {
        dai_node_desc b = dai_node_desc_default();
        std::snprintf(b.name, sizeof(b.name), "%s", name);
        b.no_body = b.no_collider = b.no_rigidbody = 1;
        b.blockout = DAI_BLOCKOUT_BOX;
        b.blockout_size = dai_vec3{ 1, 1, 1 };
        b.render_extent = dai_vec3{ 1, 1, 1 };
        return b;
    };

    // ---- 1. the scene file --------------------------------------------------
    {
        dai_node_desc r = cube_desc("Stacked");
        r.modifier_count = 3;
        r.modifiers[0] = mod_bevel(0.03f, 2, 45.0f);
        r.modifiers[1] = mod_array(3, dai_vec3{ 1.5f, 0, 0 });
        r.modifiers[1].angle = 15.0f;
        r.modifiers[1].relative = 1;
        r.modifiers[2] = mod_mirror(2, 0.002f);
        r.modifiers[2].off = 1;
        // A slot ABOVE the count, with numbers in it: it is not part of the
        // stack, and it still has to survive the file - deleting an entry and
        // saving must not throw away the one below it.
        r.modifiers[4] = mod_bevel(0.5f, 4, 5.0f);
        r.modifiers[4].param = -0.25f;
        r.modifiers[4].smooth = 1;
        dai_node n = dai_doc_add(doc, &r);

        std::string text = doc_text(doc);
        CHECK(text.find("\n  modcount 3\n") != std::string::npos,
              "the stack's length is not in the scene file");
        CHECK(text.find("\n  mod 4 1 0 0.5 4 5 0 1 0 0 0 0 -0.25\n") != std::string::npos,
              "the slot above the count did not survive the write");

        dai_doc *d2 = dai_doc_create();
        char perr[256] = { 0 };
        dai_result pr = dai_doc_from_text(d2, text.c_str(), text.size(), perr, sizeof(perr));
        CHECK(pr == DAI_OK, "a scene with a modifier stack did not load: %s", perr);
        dai_node_desc back{};
        CHECK(dai_doc_get(d2, n, &back) == DAI_OK, "the stacked node is missing after a load");
        CHECK(std::memcmp(&r.modifiers[0], &back.modifiers[0],
                          sizeof(dai_modifier) * DAI_MODIFIER_MAX) == 0 &&
              back.modifier_count == r.modifier_count,
              "the stack came back different (count %d vs %d)", back.modifier_count,
              r.modifier_count);
        CHECK(doc_text(d2) == text, "the scene file is not byte identical after a round trip");

        // A scene from before the stack existed: no mod lines at all, and the
        // node loads with an empty stack rather than with rubbish.
        {
            dai_doc *d3 = dai_doc_create();
            const char *old_scene =
                "daidalos-scene 1\nnext-id 2\nnode 1\n  name OldWall\n"
                "  blockout 1\n  bosize 4 3 0.2\n  bopivot 0 -1 0\nend\n";
            char e3[256] = { 0 };
            CHECK(dai_doc_from_text(d3, old_scene, std::strlen(old_scene), e3, sizeof(e3)) == DAI_OK,
                  "a scene written before the modifier stack no longer loads: %s", e3);
            dai_node_desc o{};
            CHECK(dai_doc_get(d3, 1, &o) == DAI_OK, "the old node is missing");
            CHECK(o.modifier_count == 0, "an old scene arrived with %d modifiers", o.modifier_count);
            CHECK(doc_text(d3).find("mod") == std::string::npos,
                  "a node without a stack writes modifier lines anyway");
            dai_doc_destroy(d3);
        }

        // Strict, like every other key: a type this build cannot run, a slot
        // that does not exist and a line that stops half way are all errors,
        // not a shrug.
        {
            struct Bad { const char *line; const char *why; };
            const Bad bad[] = {
                { "  mod 0 99 0 0.02 1 30 0 0 0 0 0 0 0\n", "an unknown modifier type" },
                { "  mod 9 1 0 0.02 1 30 0 0 0 0 0 0 0\n",  "a slot past the end of the stack" },
                { "  mod 0 1 0 0.02 1\n",                    "a half written mod line" },
                { "  modcount 12\n",                         "a count longer than the stack" },
            };
            for (const Bad &b : bad) {
                std::string s = "daidalos-scene 1\nnext-id 2\nnode 1\n  name X\n";
                s += b.line;
                s += "end\n";
                dai_doc *d4 = dai_doc_create();
                char e4[256] = { 0 };
                CHECK(dai_doc_from_text(d4, s.c_str(), s.size(), e4, sizeof(e4)) != DAI_OK,
                      "%s was accepted", b.why);
                dai_doc_destroy(d4);
            }
        }

        dai_doc_destroy(d2);
        dai_doc_remove(doc, n);
        dai_doc_sync_apply(sync);
    }

    // ---- 2. the conversion, and the same stack twice ------------------------
    {
        dai_node_desc r = cube_desc("Twice");
        r.modifier_count = 2;
        r.modifiers[0] = mod_array(3, dai_vec3{ 1.5f, 0, 0 });
        r.modifiers[1] = mod_mirror(0, 0.001f);
        r.modifiers[3] = mod_bevel(0.05f, 1, 30.0f);   // above the count: not in the stack
        daimod::Stack st = daiblockhost::stack_of(r);
        CHECK(st.mods.size() == 2, "stack_of took %u entries, the count says 2",
              (unsigned)st.mods.size());
        CHECK(st.mods[0].type == DAI_MOD_ARRAY && st.mods[1].type == DAI_MOD_MIRROR,
              "stack_of reordered the entries");
        CHECK(st.mods[0].count == 3 && st.mods[0].offset[0] == 1.5,
              "stack_of lost the array's numbers");

        daiblock::Mesh a = mesh_of(r), b = mesh_of(r);
        CHECK(!a.idx.empty(), "the stack built no triangles at all");
        CHECK(same_mesh(a, b),
              "the same stack computed twice is not bit identical (%u/%u verts, %u/%u idx)",
              (unsigned)a.verts.size(), (unsigned)b.verts.size(),
              (unsigned)a.idx.size(), (unsigned)b.idx.size());

        // The flags cross the seam as the geometry's bits.
        dai_node_desc s = cube_desc("Flags");
        s.modifier_count = 1;
        s.modifiers[0].type = DAI_MOD_SUBDIVIDE;
        s.modifiers[0].count = 1;
        s.modifiers[0].smooth = 1;
        CHECK((daiblockhost::stack_of(s).mods[0].flags & DAI_MODF_SMOOTH) != 0,
              "smooth did not reach daimod::Mod");
        s.modifiers[0].type = DAI_MOD_ARRAY;
        s.modifiers[0].smooth = 0;
        s.modifiers[0].relative = 1;
        CHECK((daiblockhost::stack_of(s).mods[0].flags & DAI_MODF_RELATIVE) != 0,
              "relative did not reach daimod::Mod");
    }

    // ---- 3. an entry that is off changes nothing ----------------------------
    {
        dai_node_desc plain = cube_desc("Plain");
        dai_node_desc off = plain;
        off.modifier_count = 1;
        off.modifiers[0] = mod_array(4, dai_vec3{ 1.5f, 0, 0 });
        off.modifiers[0].off = 1;
        CHECK(same_mesh(mesh_of(plain), mesh_of(off)),
              "an entry that is switched off changed the mesh");
        // ...and switching it back on does change it, or the check above is
        // testing that nothing works.
        off.modifiers[0].off = 0;
        CHECK(!same_mesh(mesh_of(plain), mesh_of(off)),
              "switching the entry back on changed nothing");
        CHECK(mesh_of(off).idx.size() == mesh_of(plain).idx.size() * 4,
              "4 copies is %u triangles, one box is %u",
              (unsigned)(mesh_of(off).idx.size() / 3), (unsigned)(mesh_of(plain).idx.size() / 3));
    }

    // ---- 4. the order IS the result -----------------------------------------
    // Two copies of one cube, touching along X, and a bevel. Bevel first and
    // the array copies a bevelled cube: exactly twice its triangles, and the
    // two halves meet along a chamfered seam. Array first and the bevel sees
    // ONE solid whose inner faces are gone, so it breaks fewer edges, takes
    // less volume off and lands on a different triangle count.
    {
        dai_node_desc one = cube_desc("One");
        one.modifier_count = 1;
        one.modifiers[0] = mod_bevel(0.05f, 1, 30.0f);
        daiblock::Mesh bevelled = mesh_of(one);

        dai_node_desc ba = cube_desc("BevelThenArray");
        ba.modifier_count = 2;
        ba.modifiers[0] = mod_bevel(0.05f, 1, 30.0f);
        ba.modifiers[1] = mod_array(2, dai_vec3{ 1.0f, 0, 0 });

        dai_node_desc ab = cube_desc("ArrayThenBevel");
        ab.modifier_count = 2;
        ab.modifiers[0] = mod_array(2, dai_vec3{ 1.0f, 0, 0 });
        ab.modifiers[1] = mod_bevel(0.05f, 1, 30.0f);

        daiblock::Mesh m_ba = mesh_of(ba), m_ab = mesh_of(ab);
        CHECK(m_ba.idx.size() == bevelled.idx.size() * 2,
              "bevel then array: %u triangles, twice one bevelled cube is %u",
              (unsigned)(m_ba.idx.size() / 3), (unsigned)(bevelled.idx.size() / 3 * 2));
        CHECK(!same_mesh(m_ba, m_ab),
              "array-then-bevel and bevel-then-array built the same mesh - the order is lost");

        // Counted, not eyeballed: a 2x1x1 block bevelled once keeps more
        // material than two separate cubes bevelled and pushed together, and
        // the difference is the twelve edges along the seam that are no
        // longer edges. Both volumes are reported either way.
        double v_ba = daiblock::volume(daimod::apply(daiblockhost::stack_of(ba),
                          daiblock::build(daiblockhost::shape_of(ba))).solid);
        double v_ab = daiblock::volume(daimod::apply(daiblockhost::stack_of(ab),
                          daiblock::build(daiblockhost::shape_of(ab))).solid);
        CHECK(v_ab > v_ba + 1e-6,
              "array then bevel keeps %.6f m3, bevel then array %.6f m3 - the seam between "
              "the two copies was bevelled either way", v_ab, v_ba);
        // Neither order leaves a hole: whatever the stack does, a closed
        // surface stays closed.
        daiblock::Solid sol_ab = daimod::apply(daiblockhost::stack_of(ab),
                                     daiblock::build(daiblockhost::shape_of(ab))).solid;
        int open_ab = daiblock::open_edges(daiblock::finalise(sol_ab));
        CHECK(open_ab == 0, "array then bevel left %d open edges", open_ab);
    }

    // ---- 5. undo gives the old mesh back, bit for bit ------------------------
    {
        dai_node_desc r = cube_desc("Undone");
        r.modifier_count = 1;
        r.modifiers[0] = mod_bevel(0.02f, 1, 30.0f);
        dai_node n = dai_doc_add(doc, &r);
        dai_doc_sync_apply(sync);
        daiblock::Mesh before = mesh_of(r);
        const uint64_t d_before = daiblock::digest(before);

        dai_node_desc edited = r;
        edited.modifiers[0].amount = 0.06f;
        dai_doc_begin(doc, "Edit");
        dai_doc_set(doc, n, &edited);
        dai_doc_commit(doc);
        dai_node_desc now{};
        dai_doc_get(doc, n, &now);
        daiblock::Mesh after = mesh_of(now);
        CHECK(daiblock::digest(after) != d_before, "a wider bevel built the same mesh");

        CHECK(dai_doc_undo(doc) == 1, "undo of a modifier parameter failed");
        dai_doc_get(doc, n, &now);
        CHECK(now.modifiers[0].amount == 0.02f, "undo left the width at %.3f", now.modifiers[0].amount);
        CHECK(same_mesh(mesh_of(now), before), "undo did not give the old mesh back bit for bit");
        CHECK(dai_doc_redo(doc) == 1, "redo of a modifier parameter failed");
        dai_doc_get(doc, n, &now);
        CHECK(same_mesh(mesh_of(now), after), "redo did not give the new mesh back bit for bit");
        dai_doc_undo(doc);

        // The rebuild key moves with the stack, or the editor would keep
        // showing the mesh the node had before the entry was added.
        {
            uint64_t k0 = 1469598103934665603ull, k1 = 1469598103934665603ull;
            daiblockhost::hash_node(doc, n, k0, 0);
            dai_node_desc more = r;
            more.modifier_count = 2;
            more.modifiers[1] = mod_array(2, dai_vec3{ 2, 0, 0 });
            dai_doc_set(doc, n, &more);
            daiblockhost::hash_node(doc, n, k1, 0);
            CHECK(k0 != k1, "adding a modifier did not change the rebuild key");
            dai_doc_set(doc, n, &r);
        }
        dai_doc_remove(doc, n);
        dai_doc_sync_apply(sync);
    }

    // ---- 6. the names a script says ------------------------------------------
    {
        dai_node_desc r = cube_desc("Scripted");
        daiblock::Mesh plain = mesh_of(r);

        prop_set_num(r, "modifier.count", 1);
        prop_set_num(r, "modifier.0.type", DAI_MOD_ARRAY);
        prop_set_num(r, "modifier.0.copies", 3);
        prop_set_vec(r, "modifier.0.offset", 1.5, 0, 0);
        CHECK(r.modifier_count == 1 && r.modifiers[0].type == DAI_MOD_ARRAY &&
              r.modifiers[0].count == 3 && r.modifiers[0].offset.x == 1.5f,
              "setting an array through the property names did not land in the record");
        daiblock::Mesh arrayed = mesh_of(r);
        CHECK(arrayed.idx.size() == plain.idx.size() * 3,
              "the script's array built %u triangles, three boxes are %u",
              (unsigned)(arrayed.idx.size() / 3), (unsigned)(plain.idx.size()));

        // The aliases are the same storage, not a copy: .width and .amount,
        // .segments and .count, .level, .copies, .thickness, .weld, .shift.
        prop_set_num(r, "modifier.0.width", 0.04);
        CHECK(std::fabs(prop_get_num(r, "modifier.0.amount", -1) - 0.04) < 1e-7,
              "modifier.0.width and .amount are not the same field");
        prop_set_num(r, "modifier.0.thickness", 0.07);
        CHECK(std::fabs(prop_get_num(r, "modifier.0.weld", -1) - 0.07) < 1e-7 &&
              r.modifiers[0].amount == 0.07f,
              ".thickness and .weld are not the same field as .amount");
        prop_set_num(r, "modifier.0.segments", 3);
        CHECK(prop_get_num(r, "modifier.0.count", -1) == 3 &&
              prop_get_num(r, "modifier.0.level", -1) == 3 &&
              prop_get_num(r, "modifier.0.copies", -1) == 3,
              ".segments, .level and .copies are not the same field as .count");
        prop_set_num(r, "modifier.0.shift", -0.5);
        CHECK(std::fabs(prop_get_num(r, "modifier.0.param", 0) + 0.5) < 1e-7,
              ".shift is not .param");
        prop_set_num(r, "modifier.0.smooth", 1);
        CHECK(r.modifiers[0].smooth == 1, ".smooth did not set the flag");
        prop_set_num(r, "modifier.0.relative", 1);
        CHECK(r.modifiers[0].relative == 1 && r.modifiers[0].smooth == 1,
              ".relative overwrote .smooth - they are one field, not two");
        prop_set_num(r, "modifier.0.enabled", 0);
        CHECK(r.modifiers[0].off == 1, ".enabled 0 did not switch the entry off");
        prop_set_num(r, "modifier.0.enabled", 1);
        CHECK(r.modifiers[0].off == 0, ".enabled 1 did not switch it back on");

        // A bevel set entirely by name changes the mesh measurably - the
        // bridge case in tools/bridge_check.py is this, over the socket.
        dai_node_desc b = cube_desc("ScriptedBevel");
        prop_set_num(b, "modifier.count", 1);
        prop_set_num(b, "modifier.0.type", DAI_MOD_BEVEL);
        prop_set_num(b, "modifier.0.width", 0.05);
        prop_set_num(b, "modifier.0.segments", 1);
        prop_set_num(b, "modifier.0.angle", 30);
        daiblock::Mesh bev = mesh_of(b);
        CHECK(bev.idx.size() > plain.idx.size(),
              "a bevel set by name built %u triangles, the plain box has %u",
              (unsigned)(bev.idx.size() / 3), (unsigned)(plain.idx.size() / 3));
        double v_plain = daiblock::volume(daiblock::build(daiblockhost::shape_of(b)));
        double v_bev = daiblock::volume(daimod::apply(daiblockhost::stack_of(b),
                           daiblock::build(daiblockhost::shape_of(b))).solid);
        CHECK(v_bev < v_plain - 1e-5,
              "the bevel took nothing off: %.6f m3 before, %.6f m3 after", v_plain, v_bev);

        // A slot that does not exist answers nothing at all rather than
        // writing past the array.
        CHECK(prop_ref(r, "modifier.8.type").kind == P_NONE, "modifier.8 answered");
        CHECK(prop_ref(r, "modifier.0.nonsense").kind == P_NONE, "an unknown field answered");
    }

    // ---- 7. the list in the inspector, at 1100x700 ---------------------------
    // The layout a user opens, at the small size: 1100x700, the inspector the
    // column the dock gives it there. The list has to be REACHABLE - scrolled
    // to with the wheel, the way a reader reaches it - and its last field has
    // to end up inside the panel, which is the same thing
    // tools/blockout_shot.cpp asserts about its pictures.
    {
        const float W = 1100.0f, H = 700.0f;
        auto frame_at = [&](float mx, float my, int down, float wheel) {
            dai_ui_input in{};
            in.mouse_x = mx; in.mouse_y = my; in.mouse_down = down; in.wheel = wheel;
            dai_ui_begin(ui, W, H, &in);
            dai_editor_ui_frame(panels, W, H);
            dai_ui_end(ui);
        };
        // The sections above this one leave a dropdown standing, and a widget
        // under an open popup does not take the wheel - by design, so that a
        // click meant to close a menu does not also press what is behind it.
        // A click on empty space is what a user does about that.
        for (int k = 0; k < 6; ++k)
            frame_at(40.0f, H - 10.0f, (k % 2) == 0 ? 1 : 0, 0.0f);
        CHECK(!dai_ui_popup_active(ui), "a popup is still open over the inspector");

        // Named after the block in .gauntlet-shots/16b-mit-bevel.png, and not
        // by accident: the header of that picture read "Block.Beve" because
        // the name field was measured against what was left over rather than
        // against the name. The picture and this check are about the same
        // fourteen characters.
        dai_node_desc r = cube_desc("Block.Bevelled");
        r.modifier_count = 3;
        r.modifiers[0] = mod_bevel(0.03f, 2, 30.0f);
        r.modifiers[1] = mod_array(4, dai_vec3{ 1.2f, 0, 0 });
        r.modifiers[2] = mod_mirror(0, 0.001f);
        r.modifiers[2].off = 1;
        dai_node n = dai_doc_add(doc, &r);
        dai_doc_sync_apply(sync);
        dai_editor_select(ed, n, 0);

        frame_at(-100.0f, -100.0f, 0, 0.0f);
        uint32_t with_stack = total_verts(ui);

        // The same node without a stack draws fewer vertices: the list is
        // really on the screen and not a fold nobody opened.
        dai_node_desc bare = r;
        bare.modifier_count = 0;
        for (int i = 0; i < DAI_MODIFIER_MAX; ++i) bare.modifiers[i] = dai_modifier{};
        dai_doc_set(doc, n, &bare);
        frame_at(-100.0f, -100.0f, 0, 0.0f);
        uint32_t without = total_verts(ui);
        CHECK(with_stack > without,
              "the inspector draws the same %u vertices with a three entry stack and "
              "without one", without);
        dai_doc_set(doc, n, &r);

        // ---- and every word in that panel is a WHOLE word --------------------
        // Vertices prove the list is drawn; they say nothing about whether it
        // can be read. The 1100x700 screenshot of exactly this node showed
        // "Pos...", "Siz..." and a name field ending at "Block.Beve" - three
        // rows whose meaning the reader has to guess - and no check in this
        // suite could tell, because what leaves the UI layer is triangles.
        //
        // dai_ui_text_record writes the strings down as they are drawn, with
        // the clip each one was drawn under, so this asks what a reader asks:
        // inside the Inspector, does anything end in an ellipsis, and is
        // anything wider than the box it sits in. "Add Component..." is the
        // one honest ellipsis in the panel - Unity's "this opens a dialog" -
        // and it is named here rather than pattern matched away.
        {
            float ix = 0, iy = 0, iw = 0, ih = 0, dfx = 0, dfy = 0, dfw = 0, dfh = 0;
            dai_editor_ui_inspector_last_field(panels, &dfx, &dfy, &dfw, &dfh,
                                               &ix, &iy, &iw, &ih);
            dai_ui_text_record(ui, 1);
            frame_at(-100.0f, -100.0f, 0, 0.0f);
            char cut_text[96] = { 0 }, over_text[96] = { 0 };
            int n_cut = 0, n_over = 0, saw_name = 0;
            for (uint32_t i = 0; i < dai_ui_text_record_count(ui); ++i) {
                dai_ui_text_rec t;
                if (!dai_ui_text_record_at(ui, i, &t)) continue;
                if (t.x < ix - 0.5f || t.x > ix + iw + 0.5f) continue;
                if (t.y < iy - 0.5f || t.y > iy + ih + 0.5f) continue;
                if (!std::strcmp(t.text, "Add Component...")) continue;
                size_t len = std::strlen(t.text);
                if (len > 3 && !std::strcmp(t.text + len - 3, "...")) {
                    if (!n_cut) std::snprintf(cut_text, sizeof(cut_text), "%s", t.text);
                    ++n_cut;
                    std::printf("    shortened: \"%s\" at %.0f,%.0f (%.0f px wide, clip %.0f)\n",
                                t.text, (double)t.x, (double)t.y, (double)t.w, (double)t.clip_w);
                }
                if (t.x + t.w > t.clip_x + t.clip_w + 0.5f) {
                    if (!n_over) std::snprintf(over_text, sizeof(over_text), "%s", t.text);
                    ++n_over;
                    std::printf("    cut off:   \"%s\" at %.0f,%.0f runs to %.0f, clip ends %.0f\n",
                                t.text, (double)t.x, (double)t.y, (double)(t.x + t.w),
                                (double)(t.clip_x + t.clip_w));
                }
                if (!std::strcmp(t.text, "Block.Bevelled")) saw_name = 1;
            }
            dai_ui_text_record(ui, 0);
            CHECK(n_cut == 0,
                  "at 1100x700 the inspector shortens %d of its own labels to an ellipsis, "
                  "the first of them \"%s\"", n_cut, cut_text);
            CHECK(n_over == 0,
                  "at 1100x700 %d texts in the inspector are wider than the box they are "
                  "drawn in, the first of them \"%s\"", n_over, over_text);
            CHECK(saw_name,
                  "the name field does not show \"Block.Bevelled\" in full at 1100x700");
        }

        // The wheel, one notch at a time and over the panel: the scroll limit
        // is last frame's, so this is a loop and not a single jump.
        float fx = 0, fy = 0, fw = 0, fh = 0, px = 0, py = 0, pw = 0, ph = 0;
        int inside = 0;
        for (int i = 0; i < 80 && !inside; ++i) {
            if (i == 0) frame_at(-100.0f, -100.0f, 0, 0.0f);
            else        frame_at(px + pw * 0.5f, py + ph * 0.5f, 0, -1.0f);
            inside = dai_editor_ui_inspector_last_field(panels, &fx, &fy, &fw, &fh,
                                                        &px, &py, &pw, &ph);
            if (pw <= 0.0f || ph <= 0.0f) break;
        }
        CHECK(inside,
              "at 1100x700 the modifier list's last field %.0f,%.0f %.0fx%.0f is outside the "
              "inspector %.0f,%.0f %.0fx%.0f", (double)fx, (double)fy, (double)fw, (double)fh,
              (double)px, (double)py, (double)pw, (double)ph);
        CHECK(fw > 0.0f && fh > 0.0f, "the last inspector field has no size");
        CHECK(fx >= px - 0.5f && fx + fw <= px + pw + 0.5f,
              "the last field runs out of the panel sideways: %.0f..%.0f in %.0f..%.0f",
              (double)fx, (double)(fx + fw), (double)px, (double)(px + pw));

        dai_editor_deselect_all(ed);
        dai_doc_remove(doc, n);
        dai_doc_sync_apply(sync);
    }
}

#endif /* DAI_MODIFIER_STACK_CASES_HPP */
