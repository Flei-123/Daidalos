// Bevel and subdivide, measured against arithmetic done on paper.
//
// Included by tests/test_doc.cpp after blockout_csg_cases.hpp, in the same
// binary and with the same CHECK macro - a second file rather than a second
// section because the two operators live in their own header
// (include/dai_modifier_edge.h) and are worked on by their own author, and a
// test nobody else edits is a test that keeps meaning what it says.
//
// Every number below is either arithmetic anybody can redo, or a property of
// the surface that has to hold whatever the numbers are. What is proved:
//
//   * a cube of side s bevelled by b with ONE segment comes out with exactly
//     6 + 12 + 8 = 26 faces - the six originals pulled back, twelve chamfer
//     strips, eight corner triangles - and each of those three groups is
//     counted by the direction it faces, not by where it happens to sit in
//     the list;
//   * its volume is s^3 - 6*b^2*s + (16/3)*b^3, to 1e-4, for three widths.
//     Where that comes from: each of the twelve edges loses a prism whose
//     section is the right triangle b by b, over the length that is left
//     between the corners, 12 * (b^2/2) * (s - 2b); each of the eight corners
//     loses a further 5*b^3/6, the part of the corner cube that no edge prism
//     already took. 6*b^2*s - 12*b^3 + (20/3)*b^3 = 6*b^2*s - (16/3)*b^3;
//   * the SAME cube divided flat first - 24 faces, then 96 - bevels to the
//     same volume to the micrometre: the chamfer follows the SHAPE, not the
//     tessellation, which is the one thing an edge based bevel can get wrong
//     without any test noticing;
//   * the angle threshold works on the shape it exists for: a 16 sided
//     cylinder disagrees with itself by 22.5 degrees along its wall and by 90
//     at its rims, so at 30 degrees the wall is untouched and only the rims
//     are broken, and at 20 the wall breaks too - counted in faces;
//   * two to four segments give a round edge: the strip count, the corner
//     patch count and a volume that rises towards the cube as the chamfer
//     becomes a fillet, every level closed;
//   * a bevel of a CSG RESULT - the wall with the doorway from
//     blockout_csg_cases.hpp - is closed, has no degenerate triangle, no
//     vertex pair closer than the weld tolerance, no face lit against its own
//     winding, and removes the volume the chamfer arithmetic says it should
//     over the edges that are really there;
//   * subdivide flat keeps the volume to the last digit and quarters the
//     faces; subdivide smooth (Catmull-Clark) keeps the surface closed, stays
//     inside the cube it came from and outside the ball it encloses, and
//     tightens towards the limit surface level by level;
//   * both operators are bit identical run twice, over the vertex buffer and
//     the index buffer, by memcmp.
#ifndef DAI_MODIFIER_BEVEL_CASES_HPP
#define DAI_MODIFIER_BEVEL_CASES_HPP

#include "dai_doc.h"
#include "dai_modifier.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// "Is this still a solid?" - the same six questions blockout_csg_cases.hpp
// asks of a boolean, asked of a modifier result. A bevel is where slivers are
// born for exactly the reason a boolean is: it moves every corner at once.
static void check_mod_solid(const char *what, const daiblock::Mesh &m) {
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
    CHECK(daiblock::volume(m) > 0, "%s: signed volume %.6f - the solid is inside out",
          what, daiblock::volume(m));
}

// The closest two DISTINCT welded positions in a mesh. Two corners nearer to
// each other than daiblock's weld tolerance are one corner that got written
// down twice: the surface counts as closed only because the count is done by
// position, and the exporter writes a solid nobody can print.
static double mod_closest_pair(const daiblock::Mesh &m) {
    std::map<daiblock::detail::Key, daiblock::V3> pts;
    for (size_t i = 0; i < m.verts.size(); ++i) {
        daiblock::V3 p = daiblock::v3(m.verts[i].position.x, m.verts[i].position.y,
                                      m.verts[i].position.z);
        pts[daiblock::detail::key_of(p)] = p;
    }
    std::vector<daiblock::V3> a;
    a.reserve(pts.size());
    for (std::map<daiblock::detail::Key, daiblock::V3>::const_iterator it = pts.begin();
         it != pts.end(); ++it)
        a.push_back(it->second);
    double best = 1e30;
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = i + 1; j < a.size(); ++j) {
            double d = daiblock::length(daiblock::sub(a[i], a[j]));
            if (d < best) best = d;
        }
    return a.size() < 2 ? 1e30 : best;
}

// How many faces of a solid look along one of the axes, along a 45 degree
// edge direction (two equal components, one zero: a chamfer strip) and along
// a corner direction (three equal components: a corner patch). On a bevelled
// box that is exactly the three groups the operator is supposed to produce,
// and counting them by DIRECTION means the test never has to know the order
// they were built in.
static void mod_face_groups(const daiblock::Solid &s, int *axis, int *edge, int *corner) {
    int a = 0, e = 0, c = 0;
    for (size_t i = 0; i < s.polys.size(); ++i) {
        double n[3] = { std::fabs(s.polys[i].normal.x), std::fabs(s.polys[i].normal.y),
                        std::fabs(s.polys[i].normal.z) };
        int zero = 0, one = 0, diag = 0;
        for (int k = 0; k < 3; ++k) {
            if (n[k] < 1e-6) ++zero;
            else if (std::fabs(n[k] - 1.0) < 1e-6) ++one;
            else if (std::fabs(n[k] - 0.70710678) < 1e-4) ++diag;
        }
        if (one == 1 && zero == 2) ++a;
        else if (diag == 2 && zero == 1) ++e;
        else if (std::fabs(n[0] - 0.57735027) < 1e-4 && std::fabs(n[1] - 0.57735027) < 1e-4 &&
                 std::fabs(n[2] - 0.57735027) < 1e-4) ++c;
    }
    if (axis) *axis = a;
    if (edge) *edge = e;
    if (corner) *corner = c;
}

static daiblock::Solid mod_box(float sx, float sy, float sz, float pivot_y) {
    daiblock::Shape s;
    s.kind = DAI_BLOCKOUT_BOX;
    s.size[0] = sx; s.size[1] = sy; s.size[2] = sz;
    s.pivot[1] = pivot_y;
    return daiblock::build(s);
}

static bool mod_same_mesh(const daiblock::Mesh &a, const daiblock::Mesh &b) {
    if (a.verts.size() != b.verts.size() || a.idx.size() != b.idx.size()) return false;
    if (!a.verts.empty() &&
        std::memcmp(&a.verts[0], &b.verts[0], a.verts.size() * sizeof(dai_vertex)) != 0)
        return false;
    if (!a.idx.empty() &&
        std::memcmp(&a.idx[0], &b.idx[0], a.idx.size() * sizeof(uint32_t)) != 0)
        return false;
    return true;
}

static void test_modifier_bevel() {
    std::printf("\n-- modifiers: bevel and subdivide --\n");

    const double s = 1.0;
    daiblock::Solid cube = mod_box(1.0f, 1.0f, 1.0f, 0.0f);
    CHECK(cube.polys.size() == 6, "the fixture cube has %zu faces", cube.polys.size());

    // --- the chamfer, counted and weighed ---------------------------------
    {
        const double widths[3] = { 0.05, 0.1, 0.2 };
        for (int i = 0; i < 3; ++i) {
            double b = widths[i];
            daimod::Bevel p;
            p.width = b;
            p.segments = 1;
            p.angle = 30.0;
            daiblock::Solid r = daimod::bevel(cube, p);
            daiblock::Mesh m = daiblock::finalise(r);
            char what[96];
            std::snprintf(what, sizeof(what), "cube 1 m bevelled %.2f m, 1 segment", b);
            check_mod_solid(what, m);

            int axis = 0, edges = 0, corners = 0;
            mod_face_groups(r, &axis, &edges, &corners);
            CHECK(edges == 12, "%s: %d chamfer strip(s), one per edge of a cube is 12",
                  what, edges);
            CHECK(corners == 8, "%s: %d corner patch(es), one per corner of a cube is 8",
                  what, corners);
            CHECK(axis == 6, "%s: %d face(s) still look along an axis, the six originals do",
                  what, axis);
            CHECK(r.polys.size() == 26, "%s: %zu faces, 6 + 12 + 8 = 26", what,
                  r.polys.size());
            CHECK(m.idx.size() / 3 == 44,
                  "%s: %zu triangles, 6 quads + 12 quads + 8 triangles = 44", what,
                  m.idx.size() / 3);

            double want = s * s * s - 6.0 * b * b * s + (16.0 / 3.0) * b * b * b;
            double got = daiblock::volume(m);
            CHECK(std::fabs(got - want) < 1e-4,
                  "%s holds %.9f m3; 1 - 6*b^2*s + (16/3)*b^3 = 1 - %.6f + %.6f = %.9f",
                  what, got, 6.0 * b * b * s, (16.0 / 3.0) * b * b * b, want);
        }
        std::printf("  chamfer: 26 faces and s^3 - 6*b^2*s + (16/3)*b^3 at 0.05, 0.10, 0.20 m\n");
    }

    // --- the chamfer does not care how the cube was tessellated -----------
    // A flat subdivision moves nothing, so a bevel that follows the SHAPE has
    // to return the same volume from the 6 face cube, the 24 face one and the
    // 96 face one. A bevel that follows the FACE LIST returns three answers.
    {
        daimod::Bevel p;
        p.width = 0.1;
        p.segments = 1;
        p.angle = 30.0;
        double want = 1.0 - 6.0 * 0.01 + (16.0 / 3.0) * 0.001;
        double plain = daiblock::volume(daiblock::finalise(daimod::bevel(cube, p)));
        daimod::Subdiv d1;
        d1.level = 1;
        daiblock::Solid div1 = daimod::subdivide(cube, d1);
        daimod::Subdiv d2;
        d2.level = 2;
        daiblock::Solid div2 = daimod::subdivide(cube, d2);
        CHECK(div1.polys.size() == 24 && div2.polys.size() == 96,
              "the divided cubes have %zu and %zu faces, expected 24 and 96",
              div1.polys.size(), div2.polys.size());
        double one = daiblock::volume(daiblock::finalise(daimod::bevel(div1, p)));
        double two = daiblock::volume(daiblock::finalise(daimod::bevel(div2, p)));
        check_mod_solid("the 24 face cube bevelled", daiblock::finalise(daimod::bevel(div1, p)));
        check_mod_solid("the 96 face cube bevelled", daiblock::finalise(daimod::bevel(div2, p)));
        CHECK(std::fabs(one - plain) < 1e-6 && std::fabs(two - plain) < 1e-6,
              "the same cube bevelled at 6, 24 and 96 faces holds %.9f, %.9f, %.9f m3 - "
              "the arithmetic says %.9f every time", plain, one, two, want);
        std::printf("  the chamfer follows the shape: 6, 24 and 96 face cubes agree to 1e-6\n");
    }

    // --- the angle threshold, on the shape it exists for -------------------
    // A 16 sided cylinder: 22.5 degrees between neighbouring wall faces, 90 at
    // the two rims. At 30 the wall must survive whole - 16 faces looking
    // sideways and no more - and at 20 every wall edge breaks as well, which
    // is another 16 strips looking sideways.
    {
        daiblock::Shape cs;
        cs.kind = DAI_BLOCKOUT_CYLINDER;
        cs.size[0] = 1.0f; cs.size[1] = 1.0f; cs.size[2] = 1.0f;
        cs.segments = 16;
        daiblock::Solid cyl = daiblock::build(cs);
        double before = daiblock::volume(daiblock::finalise(cyl));

        daimod::Bevel keep;
        keep.width = 0.02;
        keep.segments = 1;
        keep.angle = 30.0;
        daiblock::Solid kept = daimod::bevel(cyl, keep);
        daiblock::Mesh km = daiblock::finalise(kept);
        check_mod_solid("cylinder bevelled at 30 degrees", km);
        int sideways = 0;
        for (size_t i = 0; i < kept.polys.size(); ++i)
            if (std::fabs(kept.polys[i].normal.y) < 1e-6) ++sideways;
        CHECK(sideways == 16,
              "the 22.5 degree wall broke at a 30 degree threshold: %d sideways faces, 16 expected",
              sideways);
        CHECK(kept.polys.size() == 50,
              "cylinder bevelled at 30 degrees has %zu faces: 16 wall + 2 caps + 32 rim strips = 50",
              kept.polys.size());
        double after = daiblock::volume(km);
        CHECK(after < before && before - after < 0.01,
              "the rim chamfer took %.6f m3 off a %.6f m3 cylinder", before - after, before);

        daimod::Bevel all = keep;
        all.angle = 20.0;
        daiblock::Solid broke = daimod::bevel(cyl, all);
        check_mod_solid("cylinder bevelled at 20 degrees", daiblock::finalise(broke));
        int sideways2 = 0;
        for (size_t i = 0; i < broke.polys.size(); ++i)
            if (std::fabs(broke.polys[i].normal.y) < 1e-6) ++sideways2;
        CHECK(sideways2 == 32,
              "at a 20 degree threshold the 22.5 degree wall edges must break too: "
              "%d sideways faces, 16 wall + 16 strips = 32 expected", sideways2);
        std::printf("  threshold: the round wall survives 30 degrees and breaks at 20\n");
    }

    // --- more than one segment is a fillet, not a chamfer -------------------
    // Faces: the 6 originals, 12*seg strips, and a corner patch fanned into
    // 3*seg triangles at each of the 8 corners. The volume rises towards the
    // cube as the section goes from a straight cut to a quarter circle, and
    // every level stays closed.
    {
        double prev = 0;
        for (int seg = 1; seg <= 4; ++seg) {
            daimod::Bevel p;
            p.width = 0.1;
            p.segments = seg;
            p.angle = 30.0;
            daiblock::Solid r = daimod::bevel(cube, p);
            daiblock::Mesh m = daiblock::finalise(r);
            char what[96];
            std::snprintf(what, sizeof(what), "cube bevelled 0.10 m, %d segment(s)", seg);
            check_mod_solid(what, m);
            size_t want = seg == 1 ? 26 : (size_t)(6 + 12 * seg + 8 * 3 * seg);
            CHECK(r.polys.size() == want, "%s: %zu faces, expected %zu", what,
                  r.polys.size(), want);
            double v = daiblock::volume(m);
            CHECK(v > prev && v < 1.0,
                  "%s holds %.9f m3 - between the previous level's %.9f and the cube's 1.0",
                  what, v, prev);
            prev = v;
        }
        std::printf("  segments 1..4: the chamfer rounds off and the volume climbs back\n");
    }

    // --- a bevel on a boolean result ---------------------------------------
    // The wall with the doorway that blockout_csg_cases.hpp measures: 4 x 3 x
    // 0.2 minus 1 x 2 x 0.5, 2.0 m3. Its edges are what a bevel has to
    // survive - a reveal that ends in the middle of a face (a T junction), a
    // face that a boolean cut in two along the top of the opening, and two
    // CONCAVE edges where the jambs meet the head.
    //
    // What the chamfer should take off: every convex edge over the threshold
    // loses b^2/2 per metre and the two concave ones gain it back.
    //   the box:      4 edges x 4 m along X, but the two at the floor lose
    //                 the 1 m the doorway takes out of them          14.0 m
    //                 4 edges x 3 m along Y                          12.0 m
    //                 4 edges x 0.2 m along Z                         0.8 m
    //   the opening:  1 m head + 2 x 2 m jamb, on both faces          10.0 m
    //   concave:      2 x 0.2 m where jamb meets head                 -0.4 m
    // 36.4 m of net chamfer, so b^2/2 * 36.4 = 18.2*b^2 m3 off, and the
    // corners of it are a millimetre-scale correction on top.
    {
        daiblock::Solid wall = mod_box(4.0f, 3.0f, 0.2f, -1.0f);
        daiblock::Solid door = mod_box(1.0f, 2.0f, 0.5f, -1.0f);
        daiblock::Solid cut = daiblock::csg(wall, door, DAI_CSG_SUBTRACT);
        double base = daiblock::volume(daiblock::finalise(cut));
        CHECK(std::fabs(base - 2.0) < 1e-4, "the fixture wall holds %.6f m3, expected 2.0", base);

        const double widths[2] = { 0.02, 0.05 };
        for (int i = 0; i < 2; ++i) {
            double b = widths[i];
            daimod::Bevel p;
            p.width = b;
            p.segments = 1;
            p.angle = 30.0;
            daiblock::Solid r = daimod::bevel(cut, p);
            daiblock::Mesh m = daiblock::finalise(r);
            char what[96];
            std::snprintf(what, sizeof(what), "wall minus door, bevelled %.2f m", b);
            check_mod_solid(what, m);
            CHECK(daiblock::normal_mismatches(m) == 0,
                  "%s: %d triangle(s) whose normal disagrees with their winding", what,
                  daiblock::normal_mismatches(m));
            double nearest = mod_closest_pair(m);
            CHECK(nearest > daiblock::detail::WELD,
                  "%s: two corners %.9f m apart, closer than the %.9f m weld tolerance - "
                  "that is one corner written down twice", what, nearest, daiblock::detail::WELD);
            double removed = base - daiblock::volume(m);
            double want = 18.2 * b * b;
            CHECK(std::fabs(removed - want) < 0.03 * want,
                  "%s took %.6f m3 off; 36.4 m of net chamfer at b^2/2 = %.6f m3 (%.2f%% out)",
                  what, removed, want, 100.0 * (removed - want) / want);
        }
        // Three segments over the same corners: the rounded version has to
        // survive the concave pair as well as the convex ones.
        daimod::Bevel round3;
        round3.width = 0.02;
        round3.segments = 3;
        round3.angle = 30.0;
        daiblock::Mesh rm = daiblock::finalise(daimod::bevel(cut, round3));
        check_mod_solid("wall minus door, filleted 0.02 m in 3 segments", rm);
        CHECK(daiblock::normal_mismatches(rm) == 0,
              "the filleted wall has %d triangle(s) lit against their winding",
              daiblock::normal_mismatches(rm));
        CHECK(mod_closest_pair(rm) > daiblock::detail::WELD,
              "the filleted wall has two corners %.9f m apart", mod_closest_pair(rm));
        std::printf("  the doorway takes a chamfer: closed, no slivers, and 18.2*b^2 m3 lighter\n");
    }

    // --- subdivide, flat --------------------------------------------------
    {
        size_t want[3] = { 24, 96, 384 };
        for (int level = 1; level <= 3; ++level) {
            daimod::Subdiv d;
            d.level = level;
            d.smooth = false;
            daiblock::Solid r = daimod::subdivide(cube, d);
            daiblock::Mesh m = daiblock::finalise(r);
            char what[96];
            std::snprintf(what, sizeof(what), "cube subdivided flat, level %d", level);
            check_mod_solid(what, m);
            CHECK(r.polys.size() == want[level - 1], "%s: %zu faces, expected %zu", what,
                  r.polys.size(), want[level - 1]);
            CHECK(std::fabs(daiblock::volume(m) - 1.0) < 1e-9,
                  "%s holds %.9f m3 - a flat division moves nothing, so it is still 1.0",
                  what, daiblock::volume(m));
        }
        std::printf("  flat subdivision: 24, 96, 384 faces and the volume to the last digit\n");
    }

    // --- subdivide, smooth ------------------------------------------------
    // Catmull-Clark pulls the cage towards the limit surface, so the result is
    // strictly inside the cube it came from and strictly outside the ball IT
    // encloses - measured, not asserted: the ball is the largest one that fits
    // inside the result, which is the smallest distance from the centre to any
    // of its face planes.
    {
        double prev = 1.0;
        size_t want[3] = { 24, 96, 384 };
        for (int level = 1; level <= 3; ++level) {
            daimod::Subdiv d;
            d.level = level;
            d.smooth = true;
            daiblock::Solid r = daimod::subdivide(cube, d);
            daiblock::Mesh m = daiblock::finalise(r);
            char what[96];
            std::snprintf(what, sizeof(what), "cube subdivided smooth, level %d", level);
            check_mod_solid(what, m);
            CHECK(r.polys.size() == want[level - 1], "%s: %zu faces, expected %zu", what,
                  r.polys.size(), want[level - 1]);

            double inradius = 1e30;
            for (size_t i = 0; i < r.polys.size(); ++i) {
                double d0 = daiblock::dot(r.polys[i].pts[0], r.polys[i].normal);
                if (d0 < inradius) inradius = d0;
            }
            double ball = 4.0 / 3.0 * 3.14159265358979323846 * inradius * inradius * inradius;
            double v = daiblock::volume(m);
            CHECK(v < prev, "%s holds %.6f m3, the level before it held %.6f - the cage is "
                  "supposed to tighten", what, v, prev);
            CHECK(v > ball,
                  "%s holds %.6f m3 and the ball of radius %.6f m inside it holds %.6f",
                  what, v, inradius, ball);
            float lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
            daiblock::bounds(m, lo, hi);
            // Never outside the cube, and from level 2 on strictly inside it:
            // at level 1 the six face centres are still exactly where they
            // were - a face point IS the centroid of its face - and it is the
            // level after that which pulls them in.
            float edge = level == 1 ? 0.5f : 0.499f;
            CHECK(lo[0] >= -edge && hi[0] <= edge && lo[1] >= -edge && hi[1] <= edge &&
                  lo[2] >= -edge && hi[2] <= edge,
                  "%s reaches %.4f..%.4f - a smoothed cube stays inside the cube", what,
                  (double)lo[0], (double)hi[0]);
            prev = v;
        }
        // The tick box moves the points and NOTHING else: the same faces in
        // the same order, so a flat and a smooth level 2 differ in position
        // and not in topology.
        daimod::Subdiv flat;
        flat.level = 2;
        flat.smooth = false;
        daimod::Subdiv soft = flat;
        soft.smooth = true;
        CHECK(daimod::subdivide(cube, flat).polys.size() ==
              daimod::subdivide(cube, soft).polys.size(),
              "flat and smooth subdivision disagree about how many faces there are");
        std::printf("  Catmull-Clark: inside the cube, outside its own ball, tighter every level\n");
    }

    // --- the same stack twice is the same bytes ----------------------------
    {
        daimod::Bevel p;
        p.width = 0.07;
        p.segments = 3;
        p.angle = 30.0;
        daiblock::Mesh a = daiblock::finalise(daimod::bevel(cube, p));
        daiblock::Mesh b = daiblock::finalise(daimod::bevel(cube, p));
        CHECK(mod_same_mesh(a, b),
              "two bevels of the same cube differ: %zu/%zu vertices, %zu/%zu indices",
              a.verts.size(), b.verts.size(), a.idx.size(), b.idx.size());

        daimod::Subdiv d;
        d.level = 2;
        d.smooth = true;
        daiblock::Mesh c = daiblock::finalise(daimod::subdivide(cube, d));
        daiblock::Mesh e = daiblock::finalise(daimod::subdivide(cube, d));
        CHECK(mod_same_mesh(c, e), "two smooth subdivisions of the same cube differ");

        // And through the stack, on the shape with the boolean in it: bevel
        // after subdivide, twice, byte for byte.
        daiblock::Solid wall = mod_box(4.0f, 3.0f, 0.2f, -1.0f);
        daiblock::Solid door = mod_box(1.0f, 2.0f, 0.5f, -1.0f);
        daiblock::Solid cut = daiblock::csg(wall, door, DAI_CSG_SUBTRACT);
        daimod::Bevel q;
        q.width = 0.02;
        q.segments = 2;
        q.angle = 30.0;
        daiblock::Mesh f = daiblock::finalise(daimod::bevel(daimod::subdivide(cut, d), q));
        daiblock::Mesh g = daiblock::finalise(daimod::bevel(daimod::subdivide(cut, d), q));
        check_mod_solid("the doorway, subdivided smooth and bevelled", f);
        CHECK(mod_same_mesh(f, g), "subdivide then bevel is not deterministic on the doorway");
        std::printf("  determinism: %zu vertices and %zu indices, bit identical, twice each\n",
                    f.verts.size(), f.idx.size());
    }

    // --- a bevel that changes nothing SAYS so -------------------------------
    //
    // Two ways a bevel legitimately hands its input straight back: nothing is
    // sharper than the angle it was told to break, and there is no room for
    // the width it was given at any of the eight halvings it tries. Both are
    // correct answers and both are invisible - the entry sits in the inspector
    // with its numbers and the viewport does not move, which is exactly what a
    // broken modifier looks like. So the reason comes out with the result and
    // the panel says it (daimod::Inert, the marker in
    // src/dai_editor_ui_blockout_inspector.inl).
    {
        daimod::Bevel none;
        none.width = 0.05;
        none.segments = 1;
        none.angle = 120.0;              // a cube's edges are 90: nothing breaks
        int why = -1;
        daiblock::Solid same = daimod::bevel(cube, none, &why);
        CHECK(why == daimod::INERT_NO_EDGES,
              "a bevel that broke no edge reported %d, not INERT_NO_EDGES", why);
        CHECK(same.polys.size() == cube.polys.size(),
              "a bevel that broke no edge changed the shape: %zu faces, was %zu",
              same.polys.size(), cube.polys.size());

        // The contract behind the flag, on shapes rather than on one case: the
        // reason is set EXACTLY when the operator handed the shape back
        // untouched. A width far larger than the shape is not one of those -
        // face_width() clamps a chamfer to 45% of the face it sits on, so five
        // metres of bevel on a one metre cube is a bevel, not a refusal - and
        // a test that asserted otherwise would be asserting a bug.
        {
            daiblock::Solid wall2 = mod_box(4.0f, 3.0f, 0.2f, -1.0f);
            daiblock::Solid door2 = mod_box(1.0f, 2.0f, 0.5f, -1.0f);
            daiblock::Solid shapes[3] = { cube, mod_box(2.0f, 0.02f, 2.0f, 0.0f),
                                          daiblock::csg(wall2, door2, DAI_CSG_SUBTRACT) };
            const double widths[3] = { 0.01, 0.35, 5.0 };
            for (int si = 0; si < 3; ++si)
                for (int wi = 0; wi < 3; ++wi) {
                    daimod::Bevel b2;
                    b2.width = widths[wi];
                    b2.segments = 1;
                    b2.angle = 30.0;
                    int w2 = -1;
                    daiblock::Solid out2 = daimod::bevel(shapes[si], b2, &w2);
                    int untouched = out2.polys.size() == shapes[si].polys.size();
                    CHECK(w2 >= 0 && w2 <= daimod::INERT_NO_ROOM,
                          "shape %d at width %g reported %d, which is not a reason",
                          si, widths[wi], w2);
                    CHECK(!(w2 != daimod::INERT_NONE) || untouched,
                          "shape %d at width %g reported reason %d and STILL changed the "
                          "shape: %zu faces, was %zu", si, widths[wi], w2,
                          out2.polys.size(), shapes[si].polys.size());
                    if (!untouched)
                        CHECK(w2 == daimod::INERT_NONE,
                              "shape %d at width %g bevelled and still reported %d",
                              si, widths[wi], w2);
                }
        }

        // ...and one that really runs says nothing at all.
        daimod::Bevel real;
        real.width = 0.05;
        real.segments = 1;
        real.angle = 30.0;
        why = -1;
        daiblock::Solid cut2 = daimod::bevel(cube, real, &why);
        CHECK(why == daimod::INERT_NONE,
              "a bevel that ran reported %d, not INERT_NONE", why);
        CHECK(cut2.polys.size() == 26,
              "the bevel that ran made %zu faces, not 26", cut2.polys.size());

        // Through the STACK, which is where the inspector reads it: entry by
        // entry, in stack order, and an entry that is switched off reports
        // nothing because it was never asked to do anything.
        daimod::Stack st;
        daimod::Mod m0;                  // bevel, angle 120 - nothing to break
        m0.type = daimod::MOD_BEVEL; m0.amount = 0.05; m0.count = 1; m0.angle = 120.0;
        daimod::Mod m1 = m0;             // the same one, switched off
        m1.off = 1;
        daimod::Mod m2;                  // a bevel that does run
        m2.type = daimod::MOD_BEVEL; m2.amount = 0.05; m2.count = 1; m2.angle = 30.0;
        st.mods.push_back(m0);
        st.mods.push_back(m1);
        st.mods.push_back(m2);
        daimod::Result res = daimod::apply(st, cube);
        CHECK(res.inert.size() == 3, "the result reports %zu entries for a stack of 3",
              res.inert.size());
        CHECK(res.inert_at(0) == daimod::INERT_NO_EDGES,
              "entry 1 of the stack reported %d, not INERT_NO_EDGES", res.inert_at(0));
        CHECK(res.inert_at(1) == daimod::INERT_NONE,
              "an entry that is switched OFF reported %d - it never ran", res.inert_at(1));
        CHECK(res.inert_at(2) == daimod::INERT_NONE,
              "the entry that ran reported %d", res.inert_at(2));
        CHECK(res.any_inert(), "a stack with a bevel that did nothing says it did");
        CHECK(res.solid.polys.size() == 26,
              "the stack made %zu faces, not the 26 of one bevel", res.solid.polys.size());

        // The report the host leaves for the panel: two bits per entry, keyed
        // by node. Same map the inspector reads, and it only remembers a node
        // that has something to say.
        uint32_t mask = 0;
        for (size_t i = 0; i < res.inert.size(); ++i)
            mask |= ((uint32_t)(res.inert[i] & 3)) << (i * 2);
        daimod::report_inert(4242u, mask);
        CHECK(daimod::inert_of(4242u) == mask,
              "the inert report for a node came back as %u, not %u",
              (unsigned)daimod::inert_of(4242u), (unsigned)mask);
        daimod::report_inert(4242u, 0u);
        CHECK(daimod::inert_of(4242u) == 0u,
              "a node whose stack is clean again still carries a report");
        std::printf("  a bevel that changes nothing reports WHY: no edge over the angle, "
                    "no room for the width\n");
    }
}

#endif // DAI_MODIFIER_BEVEL_CASES_HPP
