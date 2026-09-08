// The boolean, measured against the numbers on the review's own bar.
//
// Included by tests/test_doc.cpp right after blockout_cases.hpp, in the same
// binary and with the same CHECK macro - a second file rather than a second
// section because the boolean lives in its own header
// (include/dai_blockout_csg.h) and is worked on by its own author, and a test
// nobody else edits is a test that keeps meaning what it says.
//
// Every number below is arithmetic anybody can redo on paper. What is proved:
//
//   * box 4 x 3 x 0.2 minus box 1 x 2 x 0.5, centred and on the floor:
//     2.4 - 0.4 = 2.0 m3 to 1e-4, every edge on exactly two faces, NO
//     degenerate triangle, every normal agreeing with its winding;
//   * the union of two overlapping boxes, counted once where they overlap;
//   * an arch and a cylinder as cutters - the two shapes with sloped faces,
//     where a plane test that is finer than the snap grid recurses for ever;
//   * a cutter turned 45 degrees and a wedge, so the boolean is proved on
//     planes that are not axis aligned;
//   * two builds of the same boolean are bit identical, and the result
//     survives a second boolean;
//   * the same door hole computed from a DOCUMENT - a wall node with csg 2
//     and a Box child with a transform - through daiblockhost::shape_of and
//     daiblockhost::solid_of THEMSELVES, included from
//     include/dai_blockout_host.inl rather than copied into this file, and a
//     cut wall joined to a second wall, so the boolean's output is proved as
//     the boolean's input.
//
// A generator's own checks (volume, closedness, outward normals) are in
// blockout_cases.hpp; this file takes the generators as given and measures
// what csg() makes of them.
#ifndef DAI_BLOCKOUT_CSG_CASES_HPP
#define DAI_BLOCKOUT_CSG_CASES_HPP

#include "dai_blockout.h"
#include "dai_doc.h"
#include "dai_render.h"
#include "dai_ext.h"
// The EDITOR'S OWN recipe, not a copy of it: shape_of() and solid_of() come
// out of the file the viewport includes, so what is measured below is what is
// drawn. The file was restated here once, and a restatement is a second
// source of truth that goes stale in silence. It is includable in a binary
// without a renderer because everything it needs from one it reaches through
// dai_ext_host, and dai_blockout_host_sync answers a null renderer by doing
// nothing - the same seam tests/test_editor_ui.cpp uses in its section 12.
#include "dai_blockout_host.inl"

#include <cmath>
#include <cstdio>
#include <cstring>

// "Is this the result of a boolean on two solids?" - closed, consistently
// wound, no zero area triangle, and every stored normal the one its winding
// implies. Stricter than blockout_cases.hpp's check_solid on purpose: a
// boolean is where slivers are born.
static void check_csg_solid(const char *what, const daiblock::Mesh &m) {
    int open = 0, bad = 0;
    daiblock::edge_report(m, &open, &bad);
    CHECK(!m.idx.empty(), "%s produced no triangles", what);
    CHECK(open == 0, "%s: %d edge(s) with fewer than two faces - the surface is open", what, open);
    CHECK(bad == 0, "%s: %d edge(s) shared by more than two faces", what, bad);
    CHECK(daiblock::inconsistent_edges(m) == 0,
          "%s: %d edge(s) walked twice the same way - a face is wound backwards",
          what, daiblock::inconsistent_edges(m));
    CHECK(daiblock::degenerate_triangles(m) == 0,
          "%s: %d degenerate triangle(s) - zero area, or two corners on one point",
          what, daiblock::degenerate_triangles(m));
    CHECK(daiblock::normal_mismatches(m) == 0,
          "%s: %d triangle(s) whose normal disagrees with their winding",
          what, daiblock::normal_mismatches(m));
    CHECK(daiblock::volume(m) > 0, "%s: signed volume %.6f - the solid is inside out",
          what, daiblock::volume(m));
}

static daiblock::Solid csg_box(float sx, float sy, float sz, float pivot_y,
                               const float pos[3], const float rot[4]) {
    daiblock::Shape s;
    s.kind = DAI_BLOCKOUT_BOX;
    s.size[0] = sx; s.size[1] = sy; s.size[2] = sz;
    s.pivot[1] = pivot_y;
    const float one[3] = { 1, 1, 1 };
    return daiblock::transform(daiblock::build(s), pos, rot, one);
}

// What used to stand here: two file-local functions that were a hand copy of
// shape_of() and solid_of() from include/dai_blockout_host.inl. The
// copy is gone. The cases below call daiblockhost's own two functions, so a
// change to the host's rules - a new default, a new child kind, a different
// order of children in the boolean - shows up as a failure HERE instead of
// quietly making the test and the viewport disagree about the same document.

static void test_blockout_csg_measured() {
    std::printf("blockout: the boolean against the bar - 2.4 - 0.4 = 2.0, closed, no slivers\n");

    const float id_rot[4] = { 0, 0, 0, 1 };
    const float one[3] = { 1, 1, 1 };
    const float origin[3] = { 0, 0, 0 };

    // --- the door hole the whole round is judged on -----------------------
    // A 4 x 3 x 0.2 wall on the floor, a 1 x 2 x 0.5 box centred and on the
    // floor: the cutter is thicker than the wall so the hole goes through.
    daiblock::Solid wall = csg_box(4.0f, 3.0f, 0.2f, -1.0f, origin, id_rot);
    daiblock::Solid door = csg_box(1.0f, 2.0f, 0.5f, -1.0f, origin, id_rot);
    {
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(wall, door, DAI_CSG_SUBTRACT));
        check_csg_solid("wall 4x3x0.2 minus door 1x2x0.5", m);
        CHECK(std::fabs(daiblock::volume(m) - 2.0) < 1e-4,
              "wall minus door holds %.6f m3, the maths says 2.4 - 0.4 = 2.0", daiblock::volume(m));
        // The result of a subtraction is not convex, but every face of THIS
        // one still looks away from the wall's centroid except the four in
        // the reveal - and those are exactly the faces of the hole.
        float lo[3], hi[3];
        daiblock::bounds(m, lo, hi);
        CHECK(std::fabs(lo[1]) < 1e-5f && std::fabs(hi[1] - 3.0f) < 1e-5f &&
              std::fabs(lo[2] + 0.1f) < 1e-5f && std::fabs(hi[2] - 0.1f) < 1e-5f,
              "the cut wall's outline moved: y %.3f..%.3f, z %.3f..%.3f", lo[1], hi[1], lo[2], hi[2]);
        // No triangle covers the middle of the doorway at waist height: a
        // point inside the hole must not sit on any face of the result.
        int covering = 0;
        for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
            const dai_vec3 &a = m.verts[m.idx[i]].position;
            const dai_vec3 &b = m.verts[m.idx[i + 1]].position;
            const dai_vec3 &c = m.verts[m.idx[i + 2]].position;
            // Faces in the wall plane (|z| = 0.1) whose XY bounds contain (0, 1).
            if (std::fabs(std::fabs(a.z) - 0.1f) > 1e-4f) continue;
            float minx = std::min(a.x, std::min(b.x, c.x)), maxx = std::max(a.x, std::max(b.x, c.x));
            float miny = std::min(a.y, std::min(b.y, c.y)), maxy = std::max(a.y, std::max(b.y, c.y));
            if (minx < -0.49f && maxx > 0.49f && miny < 0.01f && maxy > 1.99f) ++covering;
        }
        CHECK(covering == 0, "%d wall face(s) still span the whole doorway - the hole is a picture", covering);
    }

    // --- union of two overlapping boxes -------------------------------------
    // A 2 x 1 x 1 box and a 2 x 1 x 1 box moved 1 m along X and 0.5 m along
    // Z: they share a 1 x 1 x 0.5 block, so 2 + 2 - 0.5 = 3.5 m3.
    {
        const float at[3] = { 1.0f, 0.0f, 0.5f };
        daiblock::Solid a = csg_box(2.0f, 1.0f, 1.0f, 0.0f, origin, id_rot);
        daiblock::Solid b = csg_box(2.0f, 1.0f, 1.0f, 0.0f, at, id_rot);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, b, DAI_CSG_UNION));
        check_csg_solid("two overlapping boxes joined", m);
        CHECK(std::fabs(daiblock::volume(m) - 3.5) < 1e-4,
              "the union holds %.6f m3, the maths says 2 + 2 - 0.5 = 3.5", daiblock::volume(m));
        CHECK(std::fabs(daiblock::area(m) - (10.0 + 10.0 - 2.0 * (1.0 * 0.5 + 1.0 * 1.0 + 0.5 * 1.0) ) ) < 1e-3,
              "the union's surface is %.6f m2 - faces inside the overlap survived", daiblock::area(m));
        // The intersection of the same pair is the block they share.
        daiblock::Mesh x = daiblock::finalise(daiblock::csg(a, b, DAI_CSG_INTERSECT));
        check_csg_solid("the block two boxes share", x);
        CHECK(std::fabs(daiblock::volume(x) - 0.5) < 1e-4,
              "the shared block is %.6f m3, not 0.5", daiblock::volume(x));
        CHECK(daiblock::inward_faces(x) == 0,
              "%d face(s) of the convex intersection point inward", daiblock::inward_faces(x));
    }

    // --- the arch as a cutter: sloped planes, and the recursion they cause --
    // A 1.2 m span, 1 m rise arch, 0.6 m deep, standing at y = 1.5 in the
    // wall: its ring is subtracted, so the wall loses ring x wall thickness.
    {
        daiblock::Shape arch;
        arch.kind = DAI_BLOCKOUT_ARCH;
        arch.size[0] = 1.2f; arch.size[1] = 1.0f; arch.size[2] = 0.6f;
        arch.segments = 12;
        arch.thickness = 0.2f;
        arch.pivot[1] = -1.0f;
        const float at[3] = { 0, 1.5f, 0 };
        daiblock::Solid ring = daiblock::transform(daiblock::build(arch), at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(wall, ring, DAI_CSG_SUBTRACT));
        check_csg_solid("wall minus arch ring", m);
        double pi = 3.14159265358979323846, n = 12.0;
        double outer = 0.5 * n * std::sin(pi / n) * 0.6 * 1.0;
        double inner = 0.5 * n * std::sin(pi / n) * 0.4 * 0.8;
        double want = 2.4 - (outer - inner) * 0.2;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "wall minus arch holds %.6f m3, the maths says %.6f", daiblock::volume(m), want);
    }

    // --- a rotated cutter: a square door post turned 45 degrees about Y -----
    // A 1 x 2 x 1 box, on the floor, turned 45 degrees: in plan it is the
    // diamond |x| + |z| <= sqrt(2)/2. The wall's slab |z| <= 0.1 cuts a strip
    // of it whose width at z is 2 (sqrt(2)/2 - |z|), so the area removed is
    // 2 (sqrt(2)/2 * 0.2 - 0.01) and the volume that times the 2 m height.
    // Four sloped planes, none axis aligned, and a number with a closed form.
    {
        const float yaw45[4] = { 0, 0.38268343f, 0, 0.92387953f };
        daiblock::Solid post = csg_box(1.0f, 2.0f, 1.0f, -1.0f, origin, yaw45);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(wall, post, DAI_CSG_SUBTRACT));
        check_csg_solid("wall minus a post turned 45 degrees", m);
        double want = 2.4 - 2.0 * (std::sqrt(2.0) * 0.5 * 0.2 - 0.01) * 2.0;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "wall minus the turned post holds %.6f m3, the maths says %.6f",
              daiblock::volume(m), want);
    }

    // --- a wedge as the cutter: one sloped plane through the slab -----------
    // A 1 x 2 x 0.5 wedge on the floor, full height at z = -0.25 and nothing
    // at z = +0.25: its height at z is 2 (0.25 - z) / 0.5, and over the slab
    // |z| <= 0.1 that integrates to 4 * 0.25 * 0.2 = 0.2 m2, times 1 m wide.
    {
        daiblock::Shape ramp;
        ramp.kind = DAI_BLOCKOUT_WEDGE;
        ramp.size[0] = 1.0f; ramp.size[1] = 2.0f; ramp.size[2] = 0.5f;
        ramp.pivot[1] = -1.0f;
        daiblock::Solid w = daiblock::transform(daiblock::build(ramp), origin, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(wall, w, DAI_CSG_SUBTRACT));
        check_csg_solid("wall minus a wedge", m);
        CHECK(std::fabs(daiblock::volume(m) - 2.2) < 1e-4,
              "wall minus the wedge holds %.6f m3, the maths says 2.4 - 0.2 = 2.2",
              daiblock::volume(m));
    }

    // --- a cylinder standing in the wall: a round column bored out ----------
    // A 0.6 m wide, 4 m tall 16-gon prism at x = 1, from y = -1 to 3, so it
    // covers the wall's whole 3 m height. What it removes is the part of the
    // 16-gon inside the slab |z| <= 0.1, times 3 m. The 16-gon's width is
    // linear in z between its corners and no corner lies inside the strip
    // except the two on z = 0, which is a row boundary below - so the row sum
    // is the exact area, not an estimate.
    {
        daiblock::Shape col;
        col.kind = DAI_BLOCKOUT_CYLINDER;
        col.size[0] = 0.6f; col.size[1] = 4.0f; col.size[2] = 0.6f;
        col.segments = 16;
        const float at[3] = { 1.0f, 1.0f, 0.0f };
        daiblock::Solid bore = daiblock::transform(daiblock::build(col), at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(wall, bore, DAI_CSG_SUBTRACT));
        check_csg_solid("wall minus a vertical bore", m);
        const double pi = 3.14159265358979323846, n = 16.0;
        const int rows = 20000;
        double strip = 0;
        for (int r = 0; r < rows; ++r) {
            double z = -0.1 + 0.2 * ((double)r + 0.5) / rows;
            double xmin = 1e9, xmax = -1e9;
            for (int i = 0; i < 16; ++i) {
                double a0 = 2.0 * pi * i / n, a1 = 2.0 * pi * (i + 1) / n;
                double x0 = 0.3 * std::cos(a0), z0 = -0.3 * std::sin(a0);
                double x1 = 0.3 * std::cos(a1), z1 = -0.3 * std::sin(a1);
                if ((z0 <= z && z < z1) || (z1 <= z && z < z0)) {
                    double x = x0 + (x1 - x0) * (z - z0) / (z1 - z0);
                    if (x < xmin) xmin = x;
                    if (x > xmax) xmax = x;
                }
            }
            if (xmax > xmin) strip += (xmax - xmin) * (0.2 / rows);
        }
        double want = 2.4 - strip * 3.0;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "wall minus the bore holds %.6f m3, the maths says %.6f", daiblock::volume(m), want);
    }

    // --- determinism, at the boolean's level ----------------------------------
    {
        daiblock::Mesh a = daiblock::finalise(daiblock::csg(wall, door, DAI_CSG_SUBTRACT));
        daiblock::Mesh b = daiblock::finalise(daiblock::csg(wall, door, DAI_CSG_SUBTRACT));
        CHECK(a.verts.size() == b.verts.size() && a.idx.size() == b.idx.size() &&
              std::memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(dai_vertex)) == 0 &&
              std::memcmp(a.idx.data(), b.idx.data(), a.idx.size() * sizeof(uint32_t)) == 0,
              "two runs of the same boolean are not bit identical");
        // ... and a boolean's result is a solid a second boolean can take.
        const float at[3] = { 1.4f, 1.6f, 0 };
        daiblock::Solid window = csg_box(0.6f, 0.6f, 0.5f, 0.0f, at, id_rot);
        daiblock::Mesh twice = daiblock::finalise(
            daiblock::csg(daiblock::csg(wall, door, DAI_CSG_SUBTRACT), window, DAI_CSG_SUBTRACT));
        check_csg_solid("wall minus door minus window", twice);
        CHECK(std::fabs(daiblock::volume(twice) - (2.0 - 0.6 * 0.6 * 0.2)) < 1e-4,
              "the twice cut wall holds %.6f m3, the maths says %.6f",
              daiblock::volume(twice), 2.0 - 0.6 * 0.6 * 0.2);
    }

    // --- the door hole as a document: wall node, csg 2, Box child moved -----
    // The wall is a node with csg = subtract; the door is its child, a
    // 1 x 2 x 0.5 box scaled 2 in its own X (so 2 x 2 x 0.5), turned 90
    // degrees about Y and moved 0.5 m along X. Turned, its 0.5 m side spans
    // the wall and its 2 m side goes through it: the hole is 0.5 wide, 2
    // high and the wall's 0.2 thick, so 2.4 - 0.5 * 2 * 0.2 = 2.2 m3. The
    // transform is the proof: an untransformed child would take out 0.4.
    {
        dai_doc *d = dai_doc_create();
        dai_node_desc r = dai_node_desc_default();
        std::snprintf(r.name, sizeof(r.name), "%s", "Wall");
        r.blockout = DAI_BLOCKOUT_BOX;
        r.blockout_size = { 4.0f, 3.0f, 0.2f };
        r.blockout_pivot = { 0.0f, -1.0f, 0.0f };
        r.csg = DAI_CSG_SUBTRACT;
        dai_node wall_node = dai_doc_add(d, &r);

        dai_node_desc c = dai_node_desc_default();
        std::snprintf(c.name, sizeof(c.name), "%s", "Doorway");
        c.parent = wall_node;
        c.blockout = DAI_BLOCKOUT_BOX;
        c.blockout_size = { 1.0f, 2.0f, 0.5f };
        c.blockout_pivot = { 0.0f, -1.0f, 0.0f };
        c.position = { 0.5f, 0.0f, 0.0f };
        c.rotation = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };
        c.scale = { 2.0f, 1.0f, 1.0f };
        dai_node door_node = dai_doc_add(d, &c);
        CHECK(dai_doc_children(d, wall_node, nullptr, 0) == 1, "the doorway is not the wall's child");

        daiblock::Solid from_doc = daiblockhost::solid_of(d, wall_node, 0);
        daiblock::Mesh m = daiblock::finalise(from_doc);
        check_csg_solid("wall minus door, from the document", m);
        CHECK(std::fabs(daiblock::volume(m) - 2.2) < 1e-4,
              "the document's wall holds %.6f m3, the maths says 2.4 - 0.5 * 2 * 0.2 = 2.2",
              daiblock::volume(m));
        // The hole is where the child's transform put it: x = 0.5 +- 0.25,
        // not the untransformed 0 +- 0.5. Front faces spanning the moved
        // doorway would mean the transform was ignored; and (0, 1), the
        // middle of where the UNMOVED door would be, must still be wall -
        // some triangle of the wall plane has that point in its box.
        int covering = 0, at_origin = 0;
        for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
            const dai_vec3 &a = m.verts[m.idx[i]].position;
            const dai_vec3 &b = m.verts[m.idx[i + 1]].position;
            const dai_vec3 &cc = m.verts[m.idx[i + 2]].position;
            if (std::fabs(std::fabs(a.z) - 0.1f) > 1e-4f) continue;
            float minx = std::min(a.x, std::min(b.x, cc.x)), maxx = std::max(a.x, std::max(b.x, cc.x));
            float miny = std::min(a.y, std::min(b.y, cc.y)), maxy = std::max(a.y, std::max(b.y, cc.y));
            if (minx < 0.26f && maxx > 0.74f && miny < 0.01f && maxy > 1.99f) ++covering;
            if (minx < 0.0f && maxx > 0.0f && miny < 1.0f && maxy > 1.0f) ++at_origin;
        }
        CHECK(covering == 0, "%d wall face(s) span the moved doorway - the child's transform was ignored", covering);
        CHECK(at_origin > 0, "no wall face covers (0, 1) - the hole is where the child is NOT");

        // Same recipe, same answer: the document route and the direct call
        // are one computation, bit for bit.
        const float pos[3] = { 0.5f, 0.0f, 0.0f };
        const float yaw90[4] = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };
        const float deep[3] = { 2.0f, 1.0f, 1.0f };
        daiblock::Shape ds = daiblockhost::shape_of(c);
        daiblock::Mesh direct = daiblock::finalise(daiblock::csg(
            wall, daiblock::transform(daiblock::build(ds), pos, yaw90, deep), DAI_CSG_SUBTRACT));
        CHECK(daiblock::digest(direct) == daiblock::digest(m),
              "the document's wall and the direct boolean differ: %llx vs %llx",
              (unsigned long long)daiblock::digest(direct), (unsigned long long)daiblock::digest(m));

        // Moving the door in the document moves the hole: the same wall with
        // the child 1 m further along still loses 0.2 m3, and a different set
        // of triangles.
        dai_node_desc moved = c;
        moved.position = { 1.5f, 0.0f, 0.0f };
        dai_doc_begin(d, "Move the doorway");
        dai_doc_set(d, door_node, &moved);
        dai_doc_commit(d);
        daiblock::Mesh m2 = daiblock::finalise(daiblockhost::solid_of(d, wall_node, 0));
        check_csg_solid("wall minus the moved door, from the document", m2);
        CHECK(std::fabs(daiblock::volume(m2) - 2.2) < 1e-4,
              "the wall with the moved door holds %.6f m3, not 2.2", daiblock::volume(m2));
        CHECK(daiblock::digest(m2) != daiblock::digest(m),
              "the door moved in the document but the wall did not change");
        dai_doc_destroy(d);
    }

    // --- the boolean's output as the boolean's input: cut wall + second wall
    // The cut wall (2.0 m3) joined to a 0.2 x 3 x 4 wall standing across its
    // end at x = 2: the side wall covers x = 1.9..2.1 and the cut wall ends
    // at 2, so the two share a 0.1 x 3 x 0.2 post and the union is
    // 2.0 + 2.4 - 0.06 = 4.34 m3, the door hole still in it.
    {
        daiblock::Solid cut = daiblock::csg(wall, door, DAI_CSG_SUBTRACT);
        const float at[3] = { 2.0f, 0.0f, 0.0f };
        daiblock::Solid side = csg_box(0.2f, 3.0f, 4.0f, -1.0f, at, id_rot);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(cut, side, DAI_CSG_UNION));
        check_csg_solid("cut wall joined to a second wall", m);
        CHECK(std::fabs(daiblock::volume(m) - 4.34) < 1e-4,
              "the corner holds %.6f m3, the maths says 2.0 + 2.4 - 0.06 = 4.34", daiblock::volume(m));
        // ... and the other way round, since union is commutative and the
        // BSP is not: same volume, same surface area, whichever is the base.
        daiblock::Mesh n = daiblock::finalise(daiblock::csg(side, cut, DAI_CSG_UNION));
        check_csg_solid("second wall joined to the cut wall", n);
        CHECK(std::fabs(daiblock::volume(n) - daiblock::volume(m)) < 1e-4,
              "A | B holds %.6f m3 but B | A %.6f", daiblock::volume(m), daiblock::volume(n));
        CHECK(std::fabs(daiblock::area(n) - daiblock::area(m)) < 1e-3,
              "A | B has %.6f m2 of surface but B | A %.6f", daiblock::area(m), daiblock::area(n));
        // Two walls that only touch: the sum of both, and the seam between
        // them is not a face any more (the 4 x 0.2 slab is 2.4 + 0.8 = 3.2).
        const float next[3] = { 0.0f, 0.0f, 0.2f };
        daiblock::Solid behind = csg_box(4.0f, 1.0f, 0.2f, -1.0f, next, id_rot);
        daiblock::Mesh t = daiblock::finalise(daiblock::csg(wall, behind, DAI_CSG_UNION));
        check_csg_solid("two walls back to back", t);
        CHECK(std::fabs(daiblock::volume(t) - 3.2) < 1e-4,
              "the doubled wall holds %.6f m3, the maths says 2.4 + 0.8 = 3.2", daiblock::volume(t));
    }
}

#endif /* DAI_BLOCKOUT_CSG_CASES_HPP */
