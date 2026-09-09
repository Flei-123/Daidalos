// The three modifiers that make more surface than they were given -
// solidify, array and mirror - measured against numbers worked out on paper.
//
// Included by tests/test_fracture.cpp, next to blockout_gltf_cases.hpp and
// for the same reason: that binary is the headless one, build.sh names every
// translation unit and build.sh is frozen. It uses that file's CHECK macro,
// and include/dai_modifier.h is a header, so what is measured here is exactly
// the geometry the editor builds and the exporter writes.
//
// Every number below is arithmetic anybody can redo, and none of it is read
// off the result:
//
//   SOLIDIFY  a flat 2 x 2 sheet at 0.1 m is 0.4 m3 and CLOSED - the test the
//             brief names. A 3 x 3 sheet with a 1 x 1 hole is (9 - 1) * 0.1.
//             Two 1 m2 sheets folded at a right angle are mitred through the
//             fold, so 2 * 0.1 minus the 0.1 x 0.1 corner they share. A
//             closed 1 m box becomes a shell of (1 + 2a)^3 - (1 - 2b)^3, and
//             the three values of `shift` put the material outside, half each
//             way and inside - which is where the slab's bounding box says it
//             is. A wall thicker than the box it is given returns the SOLID
//             part, 4 x 4 x 4 with no inner skin at all.
//   ARRAY     n copies are exactly n times the triangles and n times the
//             volume, and the bounding box grew by (n - 1) * step. A relative
//             offset of 1.0 steps by the shape's own size. The STAIR - one
//             step up and one step forward, the case examples/scripts builds
//             - touches its neighbour along a line: no open edge, no lost
//             triangle, and exactly one four-faced edge per join, which is
//             what two solids touching along a line is.
//   MIRROR    every point has its partner across the plane to 1e-5, on all
//             three axes; the faces that lie on the plane are gone, so half a
//             box comes back as a closed box of 20 triangles rather than 24;
//             and the weld is measured, not asserted - the same half welded
//             at 1 mm is one box and welded at 0.1 mm is two.
//
// Plus, for each of the three: run twice, memcmp the vertices and the indices,
// no degenerate triangle, no normal that disagrees with its winding.
#ifndef DAI_MODIFIER_DUP_CASES_HPP
#define DAI_MODIFIER_DUP_CASES_HPP

#include "dai_modifier.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// "Is this a surface a renderer and an exporter can have?" - the same bar
// blockout_csg_cases.hpp holds the boolean to, minus the closedness, which
// the individual cases ask for where they are entitled to it (an array of
// touching solids has four-faced edges on purpose).
static void check_dup_mesh(const char *what, const daiblock::Mesh &m) {
    CHECK(!m.idx.empty(), "%s produced no triangles", what);
    CHECK(daiblock::degenerate_triangles(m) == 0,
          "%s: %d degenerate triangle(s) - zero area, or two corners on one point",
          what, daiblock::degenerate_triangles(m));
    CHECK(daiblock::normal_mismatches(m) == 0,
          "%s: %d triangle(s) whose normal disagrees with their winding",
          what, daiblock::normal_mismatches(m));
    CHECK(daiblock::volume(m) > 0, "%s: signed volume %.6f - the solid is inside out",
          what, daiblock::volume(m));
}

// ...and closed on top of that: every edge on exactly two triangles.
static void check_dup_closed(const char *what, const daiblock::Mesh &m) {
    check_dup_mesh(what, m);
    int open = 0, bad = 0;
    daiblock::edge_report(m, &open, &bad);
    CHECK(open == 0, "%s: %d edge(s) with fewer than two faces - the surface is open",
          what, open);
    CHECK(bad == 0, "%s: %d edge(s) shared by more than two faces", what, bad);
    CHECK(daiblock::inconsistent_edges(m) == 0,
          "%s: %d edge(s) walked twice the same way - a face is wound backwards",
          what, daiblock::inconsistent_edges(m));
}

// The determinism proof, per operator: the same call twice, byte for byte over
// the vertices AND the indices. Not a digest - a digest that collides is a
// test that passes for the wrong reason, and the arrays are small.
static void check_dup_same_twice(const char *what, const daiblock::Mesh &a,
                                 const daiblock::Mesh &b) {
    CHECK(a.verts.size() == b.verts.size() && a.idx.size() == b.idx.size() &&
          std::memcmp(a.verts.data(), b.verts.data(),
                      a.verts.size() * sizeof(dai_vertex)) == 0 &&
          std::memcmp(a.idx.data(), b.idx.data(),
                      a.idx.size() * sizeof(uint32_t)) == 0,
          "%s is not the same twice - %zu/%zu verts, %zu/%zu indices",
          what, a.verts.size(), b.verts.size(), a.idx.size(), b.idx.size());
}

// --- the fixtures -----------------------------------------------------------

static daiblock::Solid dup_moved(daiblock::Solid s, double dx, double dy, double dz) {
    for (size_t f = 0; f < s.polys.size(); ++f) {
        for (size_t i = 0; i < s.polys[f].pts.size(); ++i) {
            s.polys[f].pts[i].x += dx;
            s.polys[f].pts[i].y += dy;
            s.polys[f].pts[i].z += dz;
        }
        s.polys[f].normal = daiblock::poly_normal(s.polys[f]);
    }
    return s;
}

// One flat quad in the XZ plane, `sx` by `sz`, centred, facing +Y. An OPEN
// surface: four border edges, no volume, and the thing solidify exists for.
static daiblock::Solid dup_sheet(double sx, double sz) {
    daiblock::Solid s;
    double x = sx * 0.5, z = sz * 0.5;
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(-x, 0, -z), daiblock::v3(-x, 0, z),
                                            daiblock::v3(x, 0, z), daiblock::v3(x, 0, -z) }));
    return s;
}

// A square sheet of side `outer` with a square hole of side `hole` in the
// middle, as four convex quads round the hole - a pinwheel, because a frame
// cut into four L pieces would not be convex and Poly is convex by contract.
// Eight border edges: four outside, four round the hole.
static daiblock::Solid dup_holed_sheet(double outer, double hole) {
    daiblock::Solid s;
    double o = outer * 0.5, i = hole * 0.5;
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(-o, 0, -o), daiblock::v3(-o, 0, o),
                                            daiblock::v3(-i, 0, i), daiblock::v3(-i, 0, -i) }));
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(-i, 0, i), daiblock::v3(-o, 0, o),
                                            daiblock::v3(o, 0, o), daiblock::v3(i, 0, i) }));
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(i, 0, i), daiblock::v3(o, 0, o),
                                            daiblock::v3(o, 0, -o), daiblock::v3(i, 0, -i) }));
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(i, 0, -i), daiblock::v3(o, 0, -o),
                                            daiblock::v3(-o, 0, -o), daiblock::v3(-i, 0, -i) }));
    return s;
}

// Two 1 x 1 sheets meeting at a right angle: the floor of a step and the
// riser above it, and the concave fold solidify has to carry a wall through.
static daiblock::Solid dup_fold() {
    daiblock::Solid s;
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(0, 0, 0), daiblock::v3(0, 0, 1),
                                            daiblock::v3(1, 0, 1), daiblock::v3(1, 0, 0) }));
    s.polys.push_back(daiblock::make_poly({ daiblock::v3(1, 0, 0), daiblock::v3(1, 0, 1),
                                            daiblock::v3(1, 1, 1), daiblock::v3(1, 1, 0) }));
    return s;
}

// The axis aligned box a mesh fills, as doubles, so a bounding box can be
// compared against a step size at 1e-6 instead of at float resolution.
static void dup_bounds(const daiblock::Mesh &m, double *lo, double *hi) {
    for (int j = 0; j < 3; ++j) { lo[j] = 0; hi[j] = 0; }
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const dai_vec3 &p = m.verts[i].position;
        double c[3] = { p.x, p.y, p.z };
        for (int j = 0; j < 3; ++j) {
            if (i == 0 || c[j] < lo[j]) lo[j] = c[j];
            if (i == 0 || c[j] > hi[j]) hi[j] = c[j];
        }
    }
}

// Does every point have its reflection in the plane, to `tol`? O(n^2) on
// purpose: these meshes have tens of vertices, and a hash of a rounded
// position is a second tolerance nobody would go looking for.
static int dup_unpartnered(const daiblock::Mesh &m, int axis, double tol) {
    int lonely = 0;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const dai_vec3 &p = m.verts[i].position;
        double want[3] = { p.x, p.y, p.z };
        want[axis] = -want[axis];
        bool found = false;
        for (size_t j = 0; j < m.verts.size() && !found; ++j) {
            const dai_vec3 &q = m.verts[j].position;
            found = std::fabs(q.x - want[0]) <= tol && std::fabs(q.y - want[1]) <= tol &&
                    std::fabs(q.z - want[2]) <= tol;
        }
        if (!found) ++lonely;
    }
    return lonely;
}

// --- solidify ---------------------------------------------------------------

static void test_modifier_solidify() {
    std::printf("[dup 1] solidify - a surface with a wall\n");

    // The brief's own case: a 2 x 2 plane, 0.1 m of material, 0.4 m3, closed.
    // Six faces out of one: two skins and four rim quads, so twelve triangles.
    {
        daimod::Solidify p;
        p.thickness = 0.1;
        daiblock::Solid s = daimod::solidify(dup_sheet(2, 2), p);
        daiblock::Mesh m = daiblock::finalise(s);
        check_dup_closed("solidify(2x2 sheet, 0.1)", m);
        CHECK(std::fabs(daiblock::volume(m) - 0.4) < 1e-4,
              "a 2 x 2 sheet 0.1 m thick holds %.6f m3, not 0.400000",
              daiblock::volume(m));
        CHECK(m.idx.size() == 36,
              "a solidified quad should be 12 triangles, got %zu", m.idx.size() / 3);
        check_dup_same_twice("solidify(2x2 sheet, 0.1)", m,
                             daiblock::finalise(daimod::solidify(dup_sheet(2, 2), p)));
        std::printf("  2 x 2 sheet, 0.1 m: %.6f m3, %zu triangles, closed\n",
                    daiblock::volume(m), m.idx.size() / 3);
    }

    // shift decides WHERE the material goes, and the bounding box is where it
    // went. The sheet faces +Y, so -1 is below it and +1 is above it.
    {
        const double SHIFT[3] = { -1.0, 0.0, 1.0 };
        const double WANT_LO[3] = { -0.1, -0.05, 0.0 };
        const double WANT_HI[3] = { 0.0, 0.05, 0.1 };
        for (int i = 0; i < 3; ++i) {
            daimod::Solidify p;
            p.thickness = 0.1;
            p.shift = SHIFT[i];
            daiblock::Mesh m = daiblock::finalise(daimod::solidify(dup_sheet(2, 2), p));
            double lo[3], hi[3];
            dup_bounds(m, lo, hi);
            check_dup_closed("solidify with a shift", m);
            CHECK(std::fabs(lo[1] - WANT_LO[i]) < 1e-6 && std::fabs(hi[1] - WANT_HI[i]) < 1e-6,
                  "solidify shift %+.0f put the slab at y %.4f..%.4f, expected %.4f..%.4f",
                  SHIFT[i], lo[1], hi[1], WANT_LO[i], WANT_HI[i]);
            CHECK(std::fabs(daiblock::volume(m) - 0.4) < 1e-4,
                  "solidify shift %+.0f holds %.6f m3, not 0.400000", SHIFT[i],
                  daiblock::volume(m));
        }
        std::printf("  shift -1 / 0 / +1: the slab sits below, across and above the sheet\n");
    }

    // A hole in the sheet is a border like any other: it gets its own rim, the
    // result is closed, and the volume is the area that is really there.
    {
        daimod::Solidify p;
        p.thickness = 0.1;
        daiblock::Mesh m = daiblock::finalise(daimod::solidify(dup_holed_sheet(3, 1), p));
        check_dup_closed("solidify(3x3 sheet with a 1x1 hole, 0.1)", m);
        CHECK(std::fabs(daiblock::volume(m) - 0.8) < 1e-4,
              "a holed sheet holds %.6f m3, expected (9 - 1) * 0.1 = 0.800000",
              daiblock::volume(m));
        // Eight rim quads (four outside, four round the hole) on top of the two
        // four-quad skins: 16 quads, 32 triangles.
        CHECK(m.idx.size() == 96,
              "the holed sheet should walk both rims: 32 triangles, got %zu",
              m.idx.size() / 3);
        std::printf("  3 x 3 sheet with a 1 x 1 hole: %.6f m3, %zu triangles, both rims walled\n",
                    daiblock::volume(m), m.idx.size() / 3);
    }

    // The concave fold. Mitred, so the corner the two walls share is counted
    // once: 2 * (1 m2 * 0.1 m) - (0.1 m * 0.1 m * 1 m).
    {
        daimod::Solidify p;
        p.thickness = 0.1;
        p.shift = 1.0;                   /* all of it in front of the surface */
        daiblock::Mesh m = daiblock::finalise(daimod::solidify(dup_fold(), p));
        check_dup_closed("solidify(right angle fold, 0.1)", m);
        double want = 2.0 * 1.0 * 0.1 - 0.1 * 0.1 * 1.0;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "the folded wall holds %.6f m3, expected %.6f - the mitre is wrong",
              daiblock::volume(m), want);
        std::printf("  right angle fold: %.6f m3, mitred through the corner\n",
                    daiblock::volume(m));
    }

    // A CLOSED body becomes a shell, and the wall is the thickness on every
    // face - which is what dup::Push's scale is for. (1 + 2a)^3 - (1 - 2b)^3.
    {
        const double SHIFT[3] = { -1.0, 0.0, 1.0 };
        for (int i = 0; i < 3; ++i) {
            daimod::Solidify p;
            p.thickness = 0.1;
            p.shift = SHIFT[i];
            daiblock::Solid box = daiblock::detail::box_solid(1, 1, 1);
            daiblock::Mesh m = daiblock::finalise(daimod::solidify(box, p));
            check_dup_mesh("solidify(1 m box, 0.1)", m);
            int open = 0, bad = 0;
            daiblock::edge_report(m, &open, &bad);
            CHECK(open == 0 && bad == 0,
                  "the shell of a box is open (%d) or non manifold (%d)", open, bad);
            double out = 0.1 * (1.0 + SHIFT[i]) * 0.5, in = 0.1 * (1.0 - SHIFT[i]) * 0.5;
            double want = std::pow(1.0 + 2 * out, 3.0) - std::pow(1.0 - 2 * in, 3.0);
            CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
                  "the shell of a 1 m box (shift %+.0f) holds %.6f m3, expected %.6f",
                  SHIFT[i], daiblock::volume(m), want);
            CHECK(m.idx.size() == 72,
                  "a box shell is two boxes: 24 triangles, got %zu", m.idx.size() / 3);
        }
        std::printf("  1 m box, 0.1 m wall: a shell of 1.1^3 - 0.9^3 = %.6f m3\n",
                    std::pow(1.1, 3.0) - std::pow(0.9, 3.0));
    }

    // A wall thicker than the part: the part is solid. One skin, no inner
    // surface turned inside out, and the volume of the body it describes.
    {
        daimod::Solidify p;
        p.thickness = 3.0;               /* 1.5 m each way on a 1 m box */
        daiblock::Mesh m = daiblock::finalise(
            daimod::solidify(daiblock::detail::box_solid(1, 1, 1), p));
        check_dup_closed("solidify(1 m box, 3 m)", m);
        CHECK(std::fabs(daiblock::volume(m) - 64.0) < 1e-4,
              "a box eaten through by its own wall holds %.6f m3, expected 4^3 = 64",
              daiblock::volume(m));
        CHECK(m.idx.size() == 36,
              "the solid answer is ONE box: 12 triangles, got %zu", m.idx.size() / 3);
        std::printf("  wall thicker than the box: solid, %.4f m3, %zu triangles\n",
                    daiblock::volume(m), m.idx.size() / 3);
    }

    // Nothing to do is nothing done: a thickness of zero returns the input.
    {
        daimod::Solidify p;
        p.thickness = 0.0;
        daiblock::Solid in = dup_sheet(2, 2);
        daiblock::Mesh a = daiblock::finalise(in);
        daiblock::Mesh b = daiblock::finalise(daimod::solidify(in, p));
        check_dup_same_twice("solidify with zero thickness", a, b);
    }
}

// --- array ------------------------------------------------------------------

static void test_modifier_array() {
    std::printf("[dup 2] array - one thing, n times\n");

    daiblock::Solid box = daiblock::detail::box_solid(1, 1, 1);
    daiblock::Mesh one = daiblock::finalise(box);

    // n copies: n times the triangles, n times the volume, and a bounding box
    // longer by exactly (n - 1) * step.
    {
        for (int n = 2; n <= 5; ++n) {
            daimod::Array p;
            p.count = n;
            p.offset[0] = 1.5;
            daiblock::Mesh m = daiblock::finalise(daimod::array(box, p));
            check_dup_mesh("array of boxes", m);
            int open = 0, bad = 0;
            daiblock::edge_report(m, &open, &bad);
            CHECK(open == 0, "an array of closed boxes has %d open edge(s)", open);
            CHECK(m.idx.size() == one.idx.size() * (size_t)n,
                  "%d copies gave %zu triangles, expected %zu",
                  n, m.idx.size() / 3, one.idx.size() / 3 * (size_t)n);
            CHECK(std::fabs(daiblock::volume(m) - 1.0 * n) < 1e-4,
                  "%d copies of a 1 m3 box hold %.6f m3", n, daiblock::volume(m));
            double lo[3], hi[3], lo1[3], hi1[3];
            dup_bounds(m, lo, hi);
            dup_bounds(one, lo1, hi1);
            CHECK(std::fabs((hi[0] - lo[0]) - ((hi1[0] - lo1[0]) + (n - 1) * 1.5)) < 1e-6,
                  "%d copies at 1.5 m span %.6f m, expected %.6f",
                  n, hi[0] - lo[0], (hi1[0] - lo1[0]) + (n - 1) * 1.5);
            CHECK(std::fabs(hi[1] - lo[1] - 1.0) < 1e-6 && std::fabs(hi[2] - lo[2] - 1.0) < 1e-6,
                  "%d copies grew on an axis the offset does not name", n);
            check_dup_same_twice("array of boxes", m,
                                 daiblock::finalise(daimod::array(box, p)));
        }
        std::printf("  2..5 copies at 1.5 m: n x the triangles, n x the volume, "
                    "(n-1) x 1.5 m longer\n");
    }

    // A RELATIVE offset of 1.0 is one bounding size, so the copies stand
    // shoulder to shoulder: the run is exactly n boxes long.
    {
        daimod::Array p;
        p.count = 4;
        p.offset[0] = 1.0;
        p.relative = true;
        daiblock::Mesh m = daiblock::finalise(daimod::array(box, p));
        check_dup_mesh("array, relative offset", m);
        double lo[3], hi[3];
        dup_bounds(m, lo, hi);
        CHECK(std::fabs((hi[0] - lo[0]) - 4.0) < 1e-6,
              "four boxes at a relative offset of 1.0 span %.6f m, expected 4.0",
              hi[0] - lo[0]);
        CHECK(std::fabs(daiblock::volume(m) - 4.0) < 1e-4,
              "four relative copies hold %.6f m3", daiblock::volume(m));
        std::printf("  relative offset 1.0: four boxes, 4.000 m of run\n");
    }

    // THE STAIR - the case examples/scripts/ builds and tools/blockout_shot
    // draws. A step 1 wide, 0.2 high and 0.3 deep, offset one step up and one
    // step forward, so each copy touches the one below it along a LINE.
    {
        daiblock::Solid step = dup_moved(daiblock::detail::box_solid(1, 0.2, 0.3), 0, 0.1, 0.15);
        daiblock::Mesh stepm = daiblock::finalise(step);
        daimod::Array p;
        p.count = 5;
        p.offset[0] = 0;
        p.offset[1] = 0.2;
        p.offset[2] = 0.3;
        daiblock::Mesh m = daiblock::finalise(daimod::array(step, p));
        check_dup_mesh("array, the stair", m);
        int open = 0, bad = 0;
        daiblock::edge_report(m, &open, &bad);
        CHECK(open == 0, "the stair has %d open edge(s)", open);
        // Four joins, one shared line each, four faces on each of those lines.
        // Two solids touching along a line IS that, and calling it an error
        // would be calling a staircase an error.
        CHECK(bad == 4, "the stair should touch along 4 lines, %d edge(s) report it", bad);
        CHECK(m.idx.size() == stepm.idx.size() * 5,
              "five steps gave %zu triangles, expected %zu",
              m.idx.size() / 3, stepm.idx.size() / 3 * 5);
        CHECK(std::fabs(daiblock::volume(m) - 5 * 0.06) < 1e-4,
              "five 0.06 m3 steps hold %.6f m3, expected 0.300000", daiblock::volume(m));
        double lo[3], hi[3];
        dup_bounds(m, lo, hi);
        CHECK(std::fabs(hi[1] - lo[1] - 5 * 0.2) < 1e-6 && std::fabs(hi[2] - lo[2] - 5 * 0.3) < 1e-6,
              "the stair rises %.4f m over %.4f m, expected 1.0 over 1.5",
              hi[1] - lo[1], hi[2] - lo[2]);
        check_dup_same_twice("array, the stair", m,
                             daiblock::finalise(daimod::array(step, p)));
        std::printf("  stair: 5 steps, %.4f m3, up %.2f m over %.2f m, no open edge\n",
                    daiblock::volume(m), hi[1] - lo[1], hi[2] - lo[2]);
    }

    // Rotation per copy. A box 2 m out on +X, turned 90 degrees about Y four
    // times, lands on all four sides - so the run is symmetric about both
    // horizontal axes and still four separate 1 m3 boxes.
    {
        daiblock::Solid off = dup_moved(daiblock::detail::box_solid(1, 1, 1), 2, 0, 0);
        daimod::Array p;
        p.count = 4;
        p.offset[0] = 0;
        p.rotation = 90.0;
        p.axis = 1;
        daiblock::Mesh m = daiblock::finalise(daimod::array(off, p));
        check_dup_mesh("array with a rotation", m);
        CHECK(std::fabs(daiblock::volume(m) - 4.0) < 1e-4,
              "four turned copies hold %.6f m3", daiblock::volume(m));
        double lo[3], hi[3];
        dup_bounds(m, lo, hi);
        CHECK(std::fabs(lo[0] + 2.5) < 1e-6 && std::fabs(hi[0] - 2.5) < 1e-6 &&
              std::fabs(lo[2] + 2.5) < 1e-6 && std::fabs(hi[2] - 2.5) < 1e-6,
              "90 degrees per copy should land on all four sides, box is "
              "x %.4f..%.4f z %.4f..%.4f", lo[0], hi[0], lo[2], hi[2]);
        std::printf("  4 copies at 90 deg about Y: a ring 5 m across, %.4f m3\n",
                    daiblock::volume(m));
    }

    // One copy is the thing itself, and a count typed with too many zeros
    // costs a wide stair rather than the editor.
    {
        daimod::Array p;
        p.count = 1;
        check_dup_same_twice("array with count 1", one,
                             daiblock::finalise(daimod::array(box, p)));
        p.count = 100000;
        daiblock::Solid big = daimod::array(box, p);
        CHECK(big.polys.size() == box.polys.size() * 512,
              "count 100000 should clamp to 512 copies, got %zu",
              big.polys.size() / box.polys.size());
        std::printf("  count 1 changes nothing, count 100000 clamps to 512\n");
    }
}

// --- mirror -----------------------------------------------------------------

static void test_modifier_mirror() {
    std::printf("[dup 3] mirror - half a thing is the thing\n");

    // Half a box against each of the three planes: the result is symmetric to
    // 1e-5, closed, twice the volume, and the faces that ended up ON the plane
    // are gone - 20 triangles, not 24.
    {
        const char *NAME[3] = { "X", "Y", "Z" };
        for (int axis = 0; axis < 3; ++axis) {
            double d[3] = { 0, 0, 0 };
            d[axis] = -0.5;
            daiblock::Solid half = dup_moved(daiblock::detail::box_solid(1, 1, 1),
                                             d[0], d[1], d[2]);
            daimod::Mirror p;
            p.axis = axis;
            daiblock::Mesh m = daiblock::finalise(daimod::mirror(half, p));
            char what[64];
            std::snprintf(what, sizeof(what), "mirror(half box, %s)", NAME[axis]);
            check_dup_closed(what, m);
            CHECK(dup_unpartnered(m, axis, 1e-5) == 0,
                  "%s: %d point(s) with no partner across the plane",
                  what, dup_unpartnered(m, axis, 1e-5));
            CHECK(std::fabs(daiblock::volume(m) - 2.0) < 1e-4,
                  "%s holds %.6f m3, expected 2.000000", what, daiblock::volume(m));
            CHECK(m.idx.size() == 60,
                  "%s should drop the two faces on the plane: 20 triangles, got %zu",
                  what, m.idx.size() / 3);
            double lo[3], hi[3];
            dup_bounds(m, lo, hi);
            CHECK(std::fabs(lo[axis] + 1.0) < 1e-6 && std::fabs(hi[axis] - 1.0) < 1e-6,
                  "%s spans %.4f..%.4f on its own axis, expected -1..1",
                  what, lo[axis], hi[axis]);
            check_dup_same_twice(what, m, daiblock::finalise(daimod::mirror(half, p)));
        }
        std::printf("  half a box on X, Y and Z: symmetric to 1e-5, closed, 20 triangles\n");
    }

    // The weld, measured. The same half stops 0.5 mm short of the plane:
    // welded at 1 mm it is ONE box, 20 triangles, 2 * 1.0005 m3; welded at
    // 0.1 mm it is two boxes with a 1 mm gap - the two near faces survive, so
    // four more triangles, and the volume is the two halves as they stand.
    {
        daiblock::Solid gap = dup_moved(daiblock::detail::box_solid(1, 1, 1), -0.5005, 0, 0);
        daimod::Mirror wide;
        wide.axis = 0;
        wide.weld = 0.001;
        daiblock::Mesh a = daiblock::finalise(daimod::mirror(gap, wide));
        check_dup_closed("mirror welded at 1 mm", a);
        CHECK(a.idx.size() == 60,
              "welded at 1 mm the seam should be gone: 20 triangles, got %zu",
              a.idx.size() / 3);
        CHECK(dup_unpartnered(a, 0, 1e-5) == 0, "the welded halves are not symmetric");
        CHECK(std::fabs(daiblock::volume(a) - 2.0 * 1.0005) < 1e-4,
              "welded at 1 mm holds %.6f m3, expected %.6f", daiblock::volume(a),
              2.0 * 1.0005);

        daimod::Mirror narrow;
        narrow.axis = 0;
        narrow.weld = 0.0001;
        daiblock::Mesh b = daiblock::finalise(daimod::mirror(gap, narrow));
        check_dup_closed("mirror welded at 0.1 mm", b);
        CHECK(b.idx.size() == 72,
              "welded at 0.1 mm the two near faces stay: 24 triangles, got %zu",
              b.idx.size() / 3);
        CHECK(std::fabs(daiblock::volume(b) - 2.0) < 1e-4,
              "two unwelded halves hold %.6f m3, expected 2.000000", daiblock::volume(b));
        std::printf("  weld 1 mm: one box, 20 triangles. weld 0.1 mm: two boxes, "
                    "24 triangles\n");
    }

    // A shape that does not reach the plane is mirrored, not moved: two
    // separate closed bodies, both whole.
    {
        daiblock::Solid away = dup_moved(daiblock::detail::box_solid(1, 1, 1), 2, 0, 0);
        daimod::Mirror p;
        p.axis = 0;
        daiblock::Mesh m = daiblock::finalise(daimod::mirror(away, p));
        check_dup_closed("mirror of a detached box", m);
        CHECK(m.idx.size() == 72, "a detached box mirrors to 24 triangles, got %zu",
              m.idx.size() / 3);
        CHECK(std::fabs(daiblock::volume(m) - 2.0) < 1e-4,
              "a detached box mirrors to %.6f m3, expected 2.000000", daiblock::volume(m));
        CHECK(dup_unpartnered(m, 0, 1e-5) == 0, "the detached mirror is not symmetric");
        double lo[3], hi[3];
        dup_bounds(m, lo, hi);
        CHECK(std::fabs(lo[0] + 2.5) < 1e-6 && std::fabs(hi[0] - 2.5) < 1e-6,
              "the detached mirror spans %.4f..%.4f, expected -2.5..2.5", lo[0], hi[0]);
        std::printf("  a box 2 m off the plane: two bodies, both whole, symmetric\n");
    }
}

// --- the three of them in a row ---------------------------------------------

static void test_modifier_dup_chain() {
    std::printf("[dup 4] the three in a row - order kept, surface kept\n");

    // A 1 x 1 sheet standing 1 m off the mirror plane becomes a 0.1 m panel,
    // four panels a step apart, and then the four mirrored on X: eight panels,
    // 8 * 0.1 m3, still closed, still symmetric. Every step is one of this
    // file's three, and the result is an ordinary mesh - which is the whole
    // claim of the round.
    daimod::Solidify sp;
    sp.thickness = 0.1;
    daimod::Array ap;
    ap.count = 4;
    ap.offset[0] = 0.4;
    ap.offset[2] = 1.5;
    daimod::Mirror mp;
    mp.axis = 0;

    daiblock::Solid s = dup_moved(dup_sheet(1, 1), 1, 0, 0);
    s = daimod::solidify(s, sp);
    s = daimod::array(s, ap);
    s = daimod::mirror(s, mp);
    daiblock::Mesh m = daiblock::finalise(s);
    check_dup_mesh("solidify -> array -> mirror", m);
    int open = 0, bad = 0;
    daiblock::edge_report(m, &open, &bad);
    CHECK(open == 0, "the chain left %d open edge(s)", open);
    CHECK(bad == 0, "the chain left %d edge(s) with more than two faces", bad);
    CHECK(m.idx.size() == 8 * 36, "eight panels are 96 triangles, got %zu", m.idx.size() / 3);
    CHECK(std::fabs(daiblock::volume(m) - 8 * 0.1) < 1e-4,
          "eight 0.1 m3 panels hold %.6f m3, expected 0.800000", daiblock::volume(m));
    CHECK(dup_unpartnered(m, 0, 1e-5) == 0, "the mirrored run is not symmetric");

    // ...and the order is not a suggestion: mirroring FIRST and then arraying
    // puts the copies somewhere else entirely, with the same triangle count.
    daiblock::Solid t = dup_moved(dup_sheet(1, 1), 1, 0, 0);
    t = daimod::solidify(t, sp);
    t = daimod::mirror(t, mp);
    t = daimod::array(t, ap);
    daiblock::Mesh n = daiblock::finalise(t);
    check_dup_mesh("solidify -> mirror -> array", n);
    CHECK(n.idx.size() == m.idx.size(),
          "the two orders should cost the same triangles: %zu vs %zu",
          n.idx.size() / 3, m.idx.size() / 3);
    double lo[3], hi[3], lo2[3], hi2[3];
    dup_bounds(m, lo, hi);
    dup_bounds(n, lo2, hi2);
    CHECK(std::fabs((hi[0] - lo[0]) - (hi2[0] - lo2[0])) > 1e-3,
          "array-then-mirror and mirror-then-array span the same %.4f m - "
          "the order made no difference, which cannot be right", hi[0] - lo[0]);
    check_dup_same_twice("solidify -> array -> mirror", m, daiblock::finalise(
        daimod::mirror(daimod::array(
            daimod::solidify(dup_moved(dup_sheet(1, 1), 1, 0, 0), sp), ap), mp)));
    std::printf("  solidify -> array -> mirror: %.4f m3 over %.3f m, closed. "
                "The other order spans %.3f m.\n",
                daiblock::volume(m), hi[0] - lo[0], hi2[0] - lo2[0]);
}

// --- on the result of a boolean ---------------------------------------------

static void test_modifier_dup_csg() {
    std::printf("[dup 5] on a boolean - the wall with a doorway in it\n");

    // The round's own fixture: a 4 x 3 x 0.2 wall standing on the floor with a
    // 1 x 2 doorway subtracted out of it. 2.4 - 0.4 = 2.0 m3, and a surface
    // with concave edges all round the reveal - which is where an offset that
    // averages the wrong thing goes wrong.
    daiblock::Shape ws;
    ws.kind = daiblock::KIND_BOX;
    ws.size[0] = 4; ws.size[1] = 3; ws.size[2] = 0.2;
    ws.pivot[1] = -1;
    daiblock::Shape ds;
    ds.kind = daiblock::KIND_BOX;
    ds.size[0] = 1; ds.size[1] = 2; ds.size[2] = 0.5;
    ds.pivot[1] = -1;
    daiblock::Solid cut = daiblock::csg(daiblock::build(ws), daiblock::build(ds),
                                        daiblock::OP_SUBTRACT);
    daiblock::Mesh cutm = daiblock::finalise(cut);
    check_dup_closed("the cut wall itself", cutm);
    CHECK(std::fabs(daiblock::volume(cutm) - 2.0) < 1e-4,
          "the cut wall holds %.6f m3, expected 2.0 - the fixture is wrong",
          daiblock::volume(cutm));

    // A 10 mm skin on it. A shell of thickness t round a surface of area A
    // holds A * t plus a curvature term in t^2; on this wall at 10 mm that
    // term is 2e-6 m3, so the tolerance below is four orders above it and
    // still an order under the mistake it is there to catch (an offset that
    // leans into the wall's thickness holds twice as much).
    {
        daimod::Solidify p;
        p.thickness = 0.01;
        daiblock::Mesh m = daiblock::finalise(daimod::solidify(cut, p));
        check_dup_closed("solidify(cut wall, 0.01)", m);
        double want = daiblock::area(cutm) * 0.01;
        CHECK(std::fabs(daiblock::volume(m) - want) < 1e-4,
              "a 10 mm skin on a %.4f m2 wall holds %.6f m3, expected %.6f",
              daiblock::area(cutm), daiblock::volume(m), want);
        CHECK(m.idx.size() == cutm.idx.size() * 2,
              "a shell is two skins: %zu triangles, expected %zu",
              m.idx.size() / 3, cutm.idx.size() / 3 * 2);
        check_dup_same_twice("solidify(cut wall, 0.01)", m,
                             daiblock::finalise(daimod::solidify(cut, p)));
        std::printf("  10 mm skin on the cut wall: %.6f m3 against %.6f m2 x 0.01\n",
                    daiblock::volume(m), daiblock::area(cutm));
    }

    // HALF the wall with HALF the doorway, mirrored on X, IS the whole wall
    // with the whole doorway - the same volume and the same surface area, to
    // 1e-4. Nothing about that can be arranged by accident: the mirror has to
    // drop the faces on the plane, weld the two halves and get the winding of
    // the reflected half the right way round, or one of the two numbers moves.
    {
        daiblock::Shape hs = ws;
        hs.size[0] = 2;
        hs.pivot[0] = 1;                          /* the half that ends at x = 0 */
        daiblock::Shape hd = ds;
        hd.size[0] = 0.5;
        hd.pivot[0] = 1;
        daiblock::Solid half = daiblock::csg(daiblock::build(hs), daiblock::build(hd),
                                             daiblock::OP_SUBTRACT);
        daimod::Mirror p;
        p.axis = 0;
        daiblock::Mesh m = daiblock::finalise(daimod::mirror(half, p));
        check_dup_closed("mirror(half a cut wall, X)", m);
        CHECK(dup_unpartnered(m, 0, 1e-5) == 0,
              "the mirrored wall has %d point(s) with no partner", dup_unpartnered(m, 0, 1e-5));
        CHECK(std::fabs(daiblock::volume(m) - daiblock::volume(cutm)) < 1e-4,
              "the mirrored half holds %.6f m3, the whole wall holds %.6f",
              daiblock::volume(m), daiblock::volume(cutm));
        CHECK(std::fabs(daiblock::area(m) - daiblock::area(cutm)) < 1e-4,
              "the mirrored half has %.4f m2 of surface, the whole wall has %.4f",
              daiblock::area(m), daiblock::area(cutm));
        std::printf("  half a doorway mirrored: %.6f m3 and %.4f m2, the whole wall exactly\n",
                    daiblock::volume(m), daiblock::area(m));
    }

    // ...and three of the cut wall in a row is three times everything.
    {
        daimod::Array p;
        p.count = 3;
        p.offset[0] = 5;
        daiblock::Mesh m = daiblock::finalise(daimod::array(cut, p));
        check_dup_mesh("array(cut wall, 3)", m);
        int open = 0, bad = 0;
        daiblock::edge_report(m, &open, &bad);
        CHECK(open == 0 && bad == 0, "three cut walls: %d open, %d non manifold", open, bad);
        CHECK(m.idx.size() == cutm.idx.size() * 3,
              "three walls are %zu triangles, expected %zu",
              m.idx.size() / 3, cutm.idx.size());
        CHECK(std::fabs(daiblock::volume(m) - 3 * daiblock::volume(cutm)) < 1e-4,
              "three walls hold %.6f m3, expected %.6f",
              daiblock::volume(m), 3 * daiblock::volume(cutm));
        std::printf("  three cut walls 5 m apart: %.6f m3, %zu triangles\n",
                    daiblock::volume(m), m.idx.size() / 3);
    }
}

static void test_modifier_dup() {
    std::printf("modifiers: solidify, array, mirror\n");
    test_modifier_solidify();
    test_modifier_array();
    test_modifier_mirror();
    test_modifier_dup_chain();
    test_modifier_dup_csg();
}

#endif /* DAI_MODIFIER_DUP_CASES_HPP */
