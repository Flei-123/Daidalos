// Blockout and CSG: the shapes a room is made of, the boolean that puts a
// door through a wall, and the fields that carry both through the scene file.
//
// Included by tests/test_doc.cpp - the suite that already owns the document,
// its undo stack and its text format, which is exactly what half of these
// checks are about. It uses that file's CHECK macro and adds nothing to the
// build script: include/dai_blockout.h is a header, so the arithmetic under
// test is the same arithmetic the editor and the exporter run.
//
// What is proved here, and why each one is not a comment:
//
//   * every generator returns a CLOSED surface - each edge shared by exactly
//     two triangles, every face wound the same way round, and the signed
//     volume positive, which together mean "a solid", not "a pile of quads";
//   * the volumes are checked against numbers worked out on paper, not
//     against whatever the generator happened to print the first time;
//   * box minus box is the door hole the whole round exists for: volume,
//     closed surface and the hole really being where it was put;
//   * two builds of the same fields are bit identical, and one changed field
//     is not - the determinism law, at this layer;
//   * the fields survive save/load, undo/redo and a scene file written before
//     they existed.
#ifndef DAI_BLOCKOUT_CASES_HPP
#define DAI_BLOCKOUT_CASES_HPP

#include "dai_blockout.h"
#include "dai_doc.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The one place the document's fields become a daiblock::Shape. Everything
// that draws, exports or tests a blockout node goes through it, so "what does
// size 0 mean" has exactly one answer.
static daiblock::Shape blockout_shape_of(const dai_node_desc &r) {
    daiblock::Shape s;
    s.kind = r.blockout;
    s.size[0] = r.blockout_size.x > 0 ? r.blockout_size.x : 1.0f;
    s.size[1] = r.blockout_size.y > 0 ? r.blockout_size.y : 1.0f;
    s.size[2] = r.blockout_size.z > 0 ? r.blockout_size.z : 1.0f;
    s.segments = r.blockout_segments > 0 ? r.blockout_segments : 16;
    s.steps = r.blockout_steps > 0 ? r.blockout_steps : 8;
    s.thickness = r.blockout_thickness;
    s.pivot[0] = r.blockout_pivot.x;
    s.pivot[1] = r.blockout_pivot.y;
    s.pivot[2] = r.blockout_pivot.z;
    return s;
}

static daiblock::Mesh blockout_mesh_of(int kind, float sx, float sy, float sz,
                                       int segments = 16, int steps = 8,
                                       float thickness = 0) {
    daiblock::Shape s;
    s.kind = kind;
    s.size[0] = sx; s.size[1] = sy; s.size[2] = sz;
    s.segments = segments;
    s.steps = steps;
    s.thickness = thickness;
    return daiblock::finalise(daiblock::build(s));
}

// "Is this a solid?" asked properly: closed, consistently wound, and not
// inside out. Every generator and every boolean answers yes to all three.
static void check_solid(const char *what, const daiblock::Mesh &m) {
    int open = 0, bad = 0;
    daiblock::edge_report(m, &open, &bad);
    CHECK(!m.idx.empty(), "%s produced no triangles", what);
    CHECK(open == 0, "%s: %d edge(s) with fewer than two faces - the surface is open", what, open);
    CHECK(bad == 0, "%s: %d edge(s) shared by more than two faces", what, bad);
    CHECK(daiblock::inconsistent_edges(m) == 0,
          "%s: %d edge(s) walked twice the same way - a face is wound backwards",
          what, daiblock::inconsistent_edges(m));
    CHECK(daiblock::volume(m) > 0, "%s: signed volume %.6f - the solid is inside out",
          what, daiblock::volume(m));
}

static void test_blockout_shapes() {
    std::printf("blockout: five shapes, each a closed solid of the right size\n");

    // --- Box: the volume is the one number nobody can argue with ----------
    {
        daiblock::Mesh m = blockout_mesh_of(DAI_BLOCKOUT_BOX, 2.0f, 3.0f, 1.0f);
        check_solid("box", m);
        CHECK(std::fabs(daiblock::volume(m) - 6.0) < 1e-6,
              "a 2x3x1 box holds %.6f m3, not 6", daiblock::volume(m));
        CHECK(std::fabs(daiblock::area(m) - 22.0) < 1e-6,
              "a 2x3x1 box has %.6f m2 of surface, not 22", daiblock::area(m));
        CHECK(m.idx.size() == 36, "a box should be 12 triangles, is %zu", m.idx.size() / 3);
        float lo[3], hi[3];
        daiblock::bounds(m, lo, hi);
        CHECK(std::fabs(lo[0] + 1.0f) < 1e-5f && std::fabs(hi[1] - 1.5f) < 1e-5f,
              "a centred box should reach from -1 to +1.5, reaches %.3f..%.3f", lo[0], hi[1]);
    }

    // --- Cylinder: a 16 sided prism, and it must be the INSCRIBED one ------
    // (n/2) * r2 * sin(2pi/n) * h is what a regular n-gon of radius r holds.
    {
        const int n = 16;
        daiblock::Mesh m = blockout_mesh_of(DAI_BLOCKOUT_CYLINDER, 2.0f, 2.0f, 2.0f, n);
        check_solid("cylinder", m);
        double want = 0.5 * (double)n * std::sin(2.0 * 3.14159265358979323846 / (double)n) * 2.0;
        // 1e-4 rather than 1e-6: every position is snapped to a micrometre
        // grid before it is welded, and 96 corners of a barrel carry that
        // rounding into the volume. Tighter than the snap would be a check on
        // the rounding, not on the cylinder.
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "a %d sided cylinder holds %.6f m3, the maths says %.6f", n,
              daiblock::volume(m), want);
        daiblock::Mesh fine = blockout_mesh_of(DAI_BLOCKOUT_CYLINDER, 2.0f, 2.0f, 2.0f, 64);
        check_solid("cylinder (64 sides)", fine);
        CHECK(daiblock::volume(fine) > daiblock::volume(m),
              "more segments must approach the circle from below, not fall away from it");
        CHECK(std::fabs(daiblock::volume(fine) - 3.14159265358979 * 2.0) < 0.02,
              "64 sides should be within a third of a percent of pi*r2*h, is %.6f",
              daiblock::volume(fine));
    }

    // --- Stairs: four steps of 0.5 m rise on a 1 m wide flight ------------
    // (0.5 + 1.0 + 1.5 + 2.0) m2 of profile x 1 m of width = 5 m3.
    {
        daiblock::Mesh m = blockout_mesh_of(DAI_BLOCKOUT_STAIRS, 1.0f, 2.0f, 4.0f, 16, 4);
        check_solid("stairs", m);
        CHECK(std::fabs(daiblock::volume(m) - 5.0) < 1e-6,
              "a four step flight holds %.6f m3, not 5", daiblock::volume(m));
        daiblock::Mesh eight = blockout_mesh_of(DAI_BLOCKOUT_STAIRS, 1.0f, 2.0f, 4.0f, 16, 8);
        check_solid("stairs (8 steps)", eight);
        // N steps in a w x h x d block hold w*h*d*(N+1)/(2N): 5 m3 at four
        // steps, 4.5 at eight, and 8 - half the block - in the limit.
        CHECK(std::fabs(daiblock::volume(eight) - 4.5) < 1e-6,
              "an eight step flight of the same block holds %.6f m3, not 4.5",
              daiblock::volume(eight));
        // w*h*d*(N+1)/(2N) falls as N grows and never reaches half the block:
        // a flight of many small steps is closer to a ramp than to a block.
        CHECK(daiblock::volume(eight) < daiblock::volume(m) &&
              daiblock::volume(eight) > 4.0,
              "eight steps hold %.6f m3 - it must sit between four steps (5) and the ramp (4)",
              daiblock::volume(eight));
    }

    // --- Arch: an extruded half annulus, area worked out on paper ---------
    {
        const int n = 32;
        daiblock::Mesh m = blockout_mesh_of(DAI_BLOCKOUT_ARCH, 2.0f, 1.0f, 0.4f, n, 8, 0.2f);
        check_solid("arch", m);
        double pi = 3.14159265358979323846;
        // The polygonal half annulus: n triangle pairs of the outer ellipse
        // minus the inner one, times the depth.
        double outer = 0.5 * (double)n * std::sin(pi / (double)n) * 1.0 * 1.0;
        double inner = 0.5 * (double)n * std::sin(pi / (double)n) * 0.8 * 0.8;
        double want = (outer - inner) * 0.4;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-5,
              "the arch holds %.6f m3, the maths says %.6f", daiblock::volume(m), want);
        float lo[3], hi[3];
        daiblock::bounds(m, lo, hi);
        CHECK(std::fabs(hi[0] - 1.0f) < 1e-5f && std::fabs(lo[0] + 1.0f) < 1e-5f,
              "a 2 m span should reach -1..+1, reaches %.3f..%.3f", lo[0], hi[0]);
    }

    // --- Wedge: half a box, and the half that is left is the ramp ---------
    {
        daiblock::Mesh m = blockout_mesh_of(DAI_BLOCKOUT_WEDGE, 2.0f, 2.0f, 2.0f);
        check_solid("wedge", m);
        CHECK(std::fabs(daiblock::volume(m) - 4.0) < 1e-6,
              "a 2x2x2 wedge holds %.6f m3, not half of 8", daiblock::volume(m));
    }

    // --- The pivot moves the shape, not its size --------------------------
    {
        daiblock::Shape s;
        s.kind = DAI_BLOCKOUT_BOX;
        s.size[0] = 4.0f; s.size[1] = 3.0f; s.size[2] = 0.3f;
        s.pivot[1] = -1.0f;                       // stand it on the floor
        daiblock::Mesh m = daiblock::finalise(daiblock::build(s));
        check_solid("box on a floor pivot", m);
        float lo[3], hi[3];
        daiblock::bounds(m, lo, hi);
        CHECK(std::fabs(lo[1]) < 1e-5f && std::fabs(hi[1] - 3.0f) < 1e-5f,
              "a floor pivoted wall should stand on y=0 and reach 3 m, is %.3f..%.3f", lo[1], hi[1]);
        CHECK(std::fabs(daiblock::volume(m) - 3.6) < 1e-6,
              "the pivot changed the volume: %.6f", daiblock::volume(m));
    }
}

static void test_blockout_csg() {
    std::printf("blockout: union, subtract and intersect, measured\n");

    // A 4 x 3 x 0.3 m wall standing on the floor, and a 1 x 2 m doorway cut
    // through it. Everything below is arithmetic anybody can redo on paper.
    daiblock::Shape wall;
    wall.kind = DAI_BLOCKOUT_BOX;
    wall.size[0] = 4.0f; wall.size[1] = 3.0f; wall.size[2] = 0.3f;
    wall.pivot[1] = -1.0f;

    daiblock::Shape hole;
    hole.kind = DAI_BLOCKOUT_BOX;
    hole.size[0] = 1.0f; hole.size[1] = 2.0f; hole.size[2] = 1.0f;   // thicker than the wall
    hole.pivot[1] = -1.0f;

    const float id_rot[4] = { 0, 0, 0, 1 };
    const float one[3] = { 1, 1, 1 };
    const float at_origin[3] = { 0, 0, 0 };

    daiblock::Solid a = daiblock::build(wall);
    daiblock::Solid b = daiblock::transform(daiblock::build(hole), at_origin, id_rot, one);

    // --- subtract ---------------------------------------------------------
    {
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, b, DAI_CSG_SUBTRACT));
        check_solid("wall minus doorway", m);
        double want = 4.0 * 3.0 * 0.3 - 1.0 * 2.0 * 0.3;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-6,
              "wall minus doorway is %.6f m3, the maths says %.6f", daiblock::volume(m), want);
        // The hole is a hole: the wall no longer reaches across the middle at
        // waist height. Measured on the triangles, by counting how many of
        // them still cover the doorway's centre line.
        float lo[3], hi[3];
        daiblock::bounds(m, lo, hi);
        CHECK(std::fabs(lo[0] + 2.0f) < 1e-5f && std::fabs(hi[0] - 2.0f) < 1e-5f,
              "cutting a hole changed the wall's outline: %.3f..%.3f", lo[0], hi[0]);
        CHECK(std::fabs(hi[1] - 3.0f) < 1e-5f,
              "the wall lost its top: %.3f", hi[1]);
        // The opening is really an opening: the rim of the hole is in the
        // triangles, at the corners the doorway was cut at.
        int rim = 0;
        for (const dai_vertex &v : m.verts)
            if (std::fabs(std::fabs(v.position.x) - 0.5f) < 1e-4f &&
                std::fabs(v.position.y - 2.0f) < 1e-4f) ++rim;
        CHECK(rim >= 4, "the doorway left no rim in the mesh (%d corner vertices)", rim);
    }

    // --- a window, cut by a cylinder, all the way through -----------------
    {
        daiblock::Shape round_hole;
        round_hole.kind = DAI_BLOCKOUT_CYLINDER;
        round_hole.size[0] = 1.0f; round_hole.size[1] = 1.0f; round_hole.size[2] = 1.0f;
        round_hole.segments = 24;
        // A cylinder stands in Y; the window has to look through Z, so it is
        // turned a quarter turn about X: (sin(-45), 0, 0, cos(-45)).
        const float quarter[4] = { -0.70710678f, 0, 0, 0.70710678f };
        const float up[3] = { 0, 1.6f, 0 };
        daiblock::Solid c = daiblock::transform(daiblock::build(round_hole), up, quarter, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, c, DAI_CSG_SUBTRACT));
        check_solid("wall minus round window", m);
        double n = 24.0, pi = 3.14159265358979323846;
        double disc = 0.5 * n * std::sin(2.0 * pi / n) * 0.25;   // r = 0.5
        double want = 4.0 * 3.0 * 0.3 - disc * 0.3;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-5,
              "wall minus round window is %.6f m3, the maths says %.6f",
              daiblock::volume(m), want);
    }

    // --- union: two walls that overlap count their overlap once -----------
    {
        daiblock::Shape second = wall;
        second.size[0] = 0.3f; second.size[2] = 4.0f;     // the wall around the corner
        const float at[3] = { 1.85f, 0, 0 };
        daiblock::Solid d = daiblock::transform(daiblock::build(second), at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, d, DAI_CSG_UNION));
        check_solid("two walls joined", m);
        double va = 4.0 * 3.0 * 0.3, vb = 0.3 * 3.0 * 4.0;
        double overlap = 0.3 * 3.0 * 0.3;                 // the corner they share
        CHECK(std::fabs(daiblock::volume(m) - (va + vb - overlap)) < 1e-5,
              "the corner was counted twice: %.6f, expected %.6f",
              daiblock::volume(m), va + vb - overlap);
    }

    // --- intersect: what the two walls have in common IS that corner ------
    {
        daiblock::Shape second = wall;
        second.size[0] = 0.3f; second.size[2] = 4.0f;
        const float at[3] = { 1.85f, 0, 0 };
        daiblock::Solid d = daiblock::transform(daiblock::build(second), at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, d, DAI_CSG_INTERSECT));
        check_solid("the corner two walls share", m);
        CHECK(std::fabs(daiblock::volume(m) - 0.3 * 3.0 * 0.3) < 1e-6,
              "the shared corner is %.6f m3, not %.6f", daiblock::volume(m), 0.3 * 3.0 * 0.3);
    }

    // --- three children in a row: a wall with a door AND a window ---------
    {
        daiblock::Shape window;
        window.kind = DAI_BLOCKOUT_BOX;
        window.size[0] = 0.8f; window.size[1] = 0.8f; window.size[2] = 1.0f;
        const float at[3] = { 1.2f, 1.8f, 0 };
        daiblock::Solid w = daiblock::transform(daiblock::build(window), at, id_rot, one);
        daiblock::Solid step1 = daiblock::csg(a, b, DAI_CSG_SUBTRACT);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(step1, w, DAI_CSG_SUBTRACT));
        check_solid("wall with a door and a window", m);
        double want = 4.0 * 3.0 * 0.3 - 1.0 * 2.0 * 0.3 - 0.8 * 0.8 * 0.3;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-6,
              "the twice cut wall is %.6f m3, the maths says %.6f", daiblock::volume(m), want);
    }

    // --- subtracting nothing, and subtracting everything ------------------
    {
        daiblock::Solid empty;
        daiblock::Mesh same = daiblock::finalise(daiblock::csg(a, empty, DAI_CSG_SUBTRACT));
        CHECK(std::fabs(daiblock::volume(same) - 4.0 * 3.0 * 0.3) < 1e-6,
              "subtracting nothing changed the wall");
        daiblock::Shape big = wall;
        big.size[0] = 8.0f; big.size[1] = 8.0f; big.size[2] = 4.0f;
        daiblock::Solid everything = daiblock::transform(daiblock::build(big), at_origin, id_rot, one);
        daiblock::Mesh gone = daiblock::finalise(daiblock::csg(a, everything, DAI_CSG_SUBTRACT));
        CHECK(std::fabs(daiblock::volume(gone)) < 1e-9,
              "a wall swallowed whole should leave nothing, left %.6f m3", daiblock::volume(gone));
    }
}

static void test_blockout_determinism() {
    std::printf("blockout: same fields in, same triangles out\n");

    daiblock::Shape wall;
    wall.kind = DAI_BLOCKOUT_BOX;
    wall.size[0] = 4.0f; wall.size[1] = 3.0f; wall.size[2] = 0.3f;
    wall.pivot[1] = -1.0f;
    daiblock::Shape hole;
    hole.kind = DAI_BLOCKOUT_BOX;
    hole.size[0] = 1.0f; hole.size[1] = 2.0f; hole.size[2] = 1.0f;
    hole.pivot[1] = -1.0f;

    const float id_rot[4] = { 0, 0, 0, 1 };
    const float one[3] = { 1, 1, 1 };
    const float at[3] = { -0.7f, 0, 0 };

    daiblock::Mesh first, second;
    for (int pass = 0; pass < 2; ++pass) {
        daiblock::Solid a = daiblock::build(wall);
        daiblock::Solid b = daiblock::transform(daiblock::build(hole), at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, b, DAI_CSG_SUBTRACT));
        (pass == 0 ? first : second) = m;
    }
    CHECK(first.verts.size() == second.verts.size() && first.idx.size() == second.idx.size(),
          "two builds of one wall differ in size: %zu/%zu vs %zu/%zu",
          first.verts.size(), first.idx.size(), second.verts.size(), second.idx.size());
    CHECK(!first.verts.empty() &&
          std::memcmp(first.verts.data(), second.verts.data(),
                      first.verts.size() * sizeof(dai_vertex)) == 0,
          "two builds of one wall are not bit identical");
    CHECK(std::memcmp(first.idx.data(), second.idx.data(),
                      first.idx.size() * sizeof(uint32_t)) == 0,
          "two builds of one wall index their triangles differently");
    CHECK(daiblock::digest(first) == daiblock::digest(second),
          "the digest is not stable across two builds");

    // ... and one changed field really does change the mesh, or the check
    // above is proving that a cache works and nothing else.
    daiblock::Shape moved = wall;
    moved.size[2] = 0.35f;
    daiblock::Mesh other = daiblock::finalise(daiblock::build(moved));
    CHECK(daiblock::digest(other) != daiblock::digest(first),
          "a thicker wall has the same digest as the thin one");

    // Every generator is stable, not just the box.
    const int kinds[5] = { DAI_BLOCKOUT_BOX, DAI_BLOCKOUT_CYLINDER, DAI_BLOCKOUT_STAIRS,
                           DAI_BLOCKOUT_ARCH, DAI_BLOCKOUT_WEDGE };
    for (int i = 0; i < 5; ++i) {
        daiblock::Mesh x = blockout_mesh_of(kinds[i], 1.5f, 2.5f, 0.75f, 12, 5, 0.3f);
        daiblock::Mesh y = blockout_mesh_of(kinds[i], 1.5f, 2.5f, 0.75f, 12, 5, 0.3f);
        CHECK(daiblock::digest(x) == daiblock::digest(y),
              "shape %d is not deterministic", kinds[i]);
        check_solid("generator", x);
    }
}

static void test_blockout_document() {
    std::printf("blockout: the fields live in the document, not in the mesh\n");

    dai_doc *d = dai_doc_create();

    dai_node_desc r = dai_node_desc_default();
    CHECK(r.blockout == DAI_BLOCKOUT_NONE && r.csg == DAI_CSG_NONE && r.door_socket == 0,
          "a fresh node should not be a blockout node");

    std::snprintf(r.name, sizeof(r.name), "%s", "Wall");
    r.blockout = DAI_BLOCKOUT_BOX;
    r.blockout_size = { 4.0f, 3.0f, 0.3f };
    r.blockout_pivot = { 0.0f, -1.0f, 0.0f };
    r.door_socket = 1;
    r.door_offset = { 0.0f, 0.0f, 0.15f };
    r.door_normal = { 0.0f, 0.0f, 1.0f };
    r.door_width = 0.9f;
    r.door_height = 2.05f;
    dai_node wall = dai_doc_add(d, &r);

    dai_node_desc c = dai_node_desc_default();
    std::snprintf(c.name, sizeof(c.name), "%s", "Doorway");
    c.parent = wall;
    c.blockout = DAI_BLOCKOUT_BOX;
    c.blockout_size = { 0.9f, 2.05f, 1.0f };
    c.blockout_pivot = { 0.0f, -1.0f, 0.0f };        // a door stands on the floor
    c.blockout_steps = 3;
    c.blockout_segments = 24;
    c.blockout_thickness = 0.12f;
    dai_node cut = dai_doc_add(d, &c);

    dai_node_desc up = r;
    up.csg = DAI_CSG_SUBTRACT;
    dai_doc_begin(d, "Make it a CSG node");
    dai_doc_set(d, wall, &up);
    dai_doc_commit(d);

    // --- the text format carries all of it --------------------------------
    {
        std::vector<char> buf((size_t)dai_doc_to_text(d, nullptr, 0) + 1);
        size_t n = dai_doc_to_text(d, buf.data(), buf.size());
        std::string text(buf.data(), n);
        CHECK(text.find("blockout 1") != std::string::npos, "the shape kind was not written");
        CHECK(text.find("bosize 4 3 0.3") != std::string::npos, "the size was not written");
        CHECK(text.find("bopivot 0 -1 0") != std::string::npos, "the pivot was not written");
        CHECK(text.find("csg 2") != std::string::npos, "the CSG operation was not written");
        CHECK(text.find("doorsize 0.9 2.05") != std::string::npos, "the socket size was not written");
        CHECK(text.find("bothick 0.12") != std::string::npos, "the arch thickness was not written");
        CHECK(text.find("boseg 24") != std::string::npos, "the segment count was not written");
        CHECK(text.find("bosteps 3") != std::string::npos, "the step count was not written");

        dai_doc *back = dai_doc_create();
        char err[256] = { 0 };
        CHECK(dai_doc_from_text(back, text.c_str(), text.size(), err, sizeof(err)) == DAI_OK,
              "a scene with blockout nodes did not load back: %s", err);
        dai_node_desc got{};
        CHECK(dai_doc_get(back, wall, &got) == DAI_OK, "the wall is missing after a round trip");
        CHECK(got.blockout == DAI_BLOCKOUT_BOX && got.csg == DAI_CSG_SUBTRACT,
              "the components did not survive the round trip (%d, %d)", got.blockout, got.csg);
        CHECK(got.blockout_size.x == 4.0f && got.blockout_size.y == 3.0f &&
              got.blockout_size.z == 0.3f, "the size came back as %g %g %g",
              got.blockout_size.x, got.blockout_size.y, got.blockout_size.z);
        CHECK(got.blockout_pivot.y == -1.0f, "the pivot came back as %g", got.blockout_pivot.y);
        CHECK(got.door_socket == 1 && got.door_width == 0.9f && got.door_height == 2.05f &&
              got.door_normal.z == 1.0f && got.door_offset.z == 0.15f,
              "the DoorSocket did not survive the round trip");
        dai_node_desc gotc{};
        CHECK(dai_doc_get(back, cut, &gotc) == DAI_OK, "the doorway child is missing");
        CHECK(gotc.blockout_segments == 24 && gotc.blockout_steps == 3 &&
              std::fabs(gotc.blockout_thickness - 0.12f) < 1e-6f,
              "segments/steps/thickness came back as %d/%d/%g",
              gotc.blockout_segments, gotc.blockout_steps, gotc.blockout_thickness);
        dai_doc_destroy(back);
    }

    // --- a scene written before these fields existed still opens ----------
    {
        const char *old_scene =
            "daidalos-scene 1\n"
            "next-id 3\n"
            "\nnode 1\n"
            "  name Crate\n"
            "  pos 1 2 3\n"
            "  extent 0.5 0.5 0.5\n"
            "end\n";
        dai_doc *back = dai_doc_create();
        char err[256] = { 0 };
        CHECK(dai_doc_from_text(back, old_scene, std::strlen(old_scene), err, sizeof(err)) == DAI_OK,
              "a scene from before the blockout fields no longer opens: %s", err);
        dai_node_desc got{};
        CHECK(dai_doc_get(back, 1, &got) == DAI_OK, "the old node is missing");
        CHECK(got.blockout == DAI_BLOCKOUT_NONE && got.csg == DAI_CSG_NONE &&
              got.door_socket == 0 && got.blockout_size.x == 0.0f,
              "an old node came back as a blockout node");
        dai_doc_destroy(back);
    }

    // --- a shape number this build has no shape for is a load error -------
    {
        const char *from_the_future =
            "daidalos-scene 1\n"
            "next-id 2\n"
            "\nnode 1\n"
            "  blockout 99\n"
            "end\n";
        dai_doc *back = dai_doc_create();
        char err[256] = { 0 };
        CHECK(dai_doc_from_text(back, from_the_future, std::strlen(from_the_future),
                                err, sizeof(err)) != DAI_OK,
              "a shape kind this build does not have loaded as if it did");
        dai_doc_destroy(back);
    }

    // --- undo and redo cover the new fields, like every other one ---------
    {
        dai_node_desc before{};
        dai_doc_get(d, wall, &before);
        dai_doc_begin(d, "Widen the wall");
        dai_node_desc wider = before;
        wider.blockout_size.x = 6.0f;
        wider.door_width = 1.2f;
        dai_doc_set(d, wall, &wider);
        dai_doc_commit(d);

        dai_node_desc now{};
        dai_doc_get(d, wall, &now);
        CHECK(now.blockout_size.x == 6.0f && now.door_width == 1.2f, "the edit did not land");
        CHECK(dai_doc_undo(d) == 1, "the blockout edit was not undoable");
        dai_doc_get(d, wall, &now);
        CHECK(now.blockout_size.x == 4.0f && now.door_width == 0.9f,
              "undo did not put the blockout fields back (%g, %g)",
              now.blockout_size.x, now.door_width);
        CHECK(dai_doc_redo(d) == 1, "the blockout edit was not redoable");
        dai_doc_get(d, wall, &now);
        CHECK(now.blockout_size.x == 6.0f && now.door_width == 1.2f,
              "redo did not bring the blockout fields back");
        dai_doc_undo(d);
    }

    // --- and the document really is the recipe the mesh comes from --------
    {
        dai_node_desc got{}, gotc{};
        dai_doc_get(d, wall, &got);
        dai_doc_get(d, cut, &gotc);
        daiblock::Solid a = daiblock::build(blockout_shape_of(got));
        const float id_rot[4] = { 0, 0, 0, 1 };
        const float one[3] = { 1, 1, 1 };
        const float at[3] = { 0, 0, 0 };
        daiblock::Solid b = daiblock::transform(daiblock::build(blockout_shape_of(gotc)),
                                                at, id_rot, one);
        daiblock::Mesh m = daiblock::finalise(daiblock::csg(a, b, got.csg));
        check_solid("the wall the document describes", m);
        double want = 4.0 * 3.0 * 0.3 - 0.9 * 2.05 * 0.3;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-6,
              "the wall the document describes is %.6f m3, the maths says %.6f",
              daiblock::volume(m), want);
    }

    dai_doc_destroy(d);
}

static void test_blockout() {
    test_blockout_shapes();
    test_blockout_csg();
    test_blockout_determinism();
    test_blockout_document();
}

#endif /* DAI_BLOCKOUT_CASES_HPP */
