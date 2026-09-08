// The blockout mesh on its way OUT: a wall with a doorway cut into it,
// written as a .glb by dai_gltf_write and read straight back by
// dai_gltf_read_geometry.
//
// Included by tests/test_fracture.cpp rather than by the document suite for
// one reason: that binary is the headless one that already links the glTF
// writer, the reader and the JSON encoder, and build.sh - which names every
// translation unit and is frozen - is not going to grow a new one for a
// hundred lines. It uses the host file's CHECK macro; include/dai_blockout.h
// is a header, so the geometry under test is the geometry the editor builds.
//
// What it proves: the result of a boolean is an ORDINARY mesh. Same triangle
// count out as in, still closed, still the volume the maths says - a wall that
// cannot leave the editor is a wall nobody can put in a game. Three results
// make the trip: the subtracted door (the round's own picture), the union of
// two boxes, and the wall minus the arch - the one with sloped faces, which
// is where a boolean's triangles are the least ordinary.
#ifndef DAI_BLOCKOUT_GLTF_CASES_HPP
#define DAI_BLOCKOUT_GLTF_CASES_HPP

#include "dai_blockout.h"
#include "dai_gltf.h"

#include <cmath>
#include <cstdio>
#include <vector>

// One mesh out through dai_gltf_write, back through dai_gltf_read_geometry,
// and held to what it was: the same vertex and index count, closed, wound
// one way, every normal the one its winding implies, and the volume the
// maths says to 1e-4.
static void blockout_gltf_roundtrip(const char *what, const char *path,
                                    const daiblock::Mesh &m, double want) {
    CHECK(!m.idx.empty(), "%s: nothing to export", what);

    dai_mesh_write w{};
    w.vertices = m.verts.data();
    w.vertex_count = (uint32_t)m.verts.size();
    w.indices = m.idx.data();
    w.index_count = (uint32_t)m.idx.size();
    w.name = what;
    char err[256] = { 0 };
    CHECK(dai_gltf_write(path, &w, 1, err, sizeof(err)) == DAI_OK,
          "%s could not be written as glTF: %s", what, err);

    std::FILE *f = std::fopen(path, "rb");
    CHECK(f != nullptr, "%s: the exported .glb is not there", what);
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes((size_t)(size > 0 ? size : 0));
    size_t rd = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    CHECK(rd == bytes.size() && rd > 0, "%s: the exported .glb could not be read back", what);

    dai_mesh_data md[4];
    char rerr[256] = { 0 };
    uint32_t n = dai_gltf_read_geometry(bytes.data(), bytes.size(), md, 4, rerr, sizeof(rerr));
    CHECK(n == 1, "%s came back as %u primitives: %s", what, n, rerr);
    if (n == 1) {
        CHECK(md[0].vertex_count == m.verts.size() && md[0].index_count == m.idx.size(),
              "%s: the round trip changed the triangle count: %u/%u vs %zu/%zu",
              what, md[0].vertex_count, md[0].index_count, m.verts.size(), m.idx.size());
        daiblock::Mesh back;
        back.verts.assign(md[0].vertices, md[0].vertices + md[0].vertex_count);
        back.idx.assign(md[0].indices, md[0].indices + md[0].index_count);
        int open_e = 0, bad_e = 0;
        daiblock::edge_report(back, &open_e, &bad_e);
        CHECK(open_e == 0 && bad_e == 0,
              "%s that came back from glTF is not closed: %d open, %d shared by more than two",
              what, open_e, bad_e);
        CHECK(daiblock::inconsistent_edges(back) == 0,
              "%s that came back from glTF has a face wound backwards", what);
        CHECK(daiblock::degenerate_triangles(back) == 0,
              "%s that came back from glTF has %d degenerate triangle(s)",
              what, daiblock::degenerate_triangles(back));
        CHECK(daiblock::normal_mismatches(back) == 0,
              "%s that came back from glTF has %d triangle(s) lit against their winding",
              what, daiblock::normal_mismatches(back));
        CHECK(std::fabs(daiblock::volume(back) - want) < 1e-4,
              "the exported %s holds %.6f m3, the maths says %.6f",
              what, daiblock::volume(back), want);
        CHECK(std::fabs(daiblock::volume(back) - daiblock::volume(m)) < 1e-4,
              "%s: %.6f m3 went out, %.6f m3 came back", what, daiblock::volume(m),
              daiblock::volume(back));
        dai_gltf_free_geometry(md, n);
    }
    std::remove(path);
}

// The result of a boolean is an ordinary mesh, which means it goes out through
// the ordinary exporter. Written to a .glb and read back, triangle for
// triangle: a wall with a doorway that cannot leave the editor is a wall
// nobody can put in a game.
static void test_blockout_gltf() {
    std::printf("blockout: a cut wall, a union and a wall minus an arch leave through dai_gltf_write\n");

    const float id_rot[4] = { 0, 0, 0, 1 };
    const float one[3] = { 1, 1, 1 };
    const float at[3] = { 0, 0, 0 };

    // --- the doorway: 4 x 3 x 0.3 minus 1 x 2 x 1, both on the floor ---------
    {
        daiblock::Shape wall;
        wall.kind = daiblock::KIND_BOX;
        wall.size[0] = 4.0f; wall.size[1] = 3.0f; wall.size[2] = 0.3f;
        wall.pivot[1] = -1.0f;
        daiblock::Shape hole;
        hole.kind = daiblock::KIND_BOX;
        hole.size[0] = 1.0f; hole.size[1] = 2.0f; hole.size[2] = 1.0f;
        hole.pivot[1] = -1.0f;
        daiblock::Mesh m = daiblock::finalise(
            daiblock::csg(daiblock::build(wall),
                          daiblock::transform(daiblock::build(hole), at, id_rot, one),
                          daiblock::OP_SUBTRACT));
        blockout_gltf_roundtrip("Wall", "/tmp/dai_blockout_wall.glb", m,
                                4.0 * 3.0 * 0.3 - 1.0 * 2.0 * 0.3);
    }

    // --- the union: two 2 x 1 x 1 boxes sharing a 1 x 1 x 0.5 block ----------
    {
        daiblock::Shape box;
        box.kind = daiblock::KIND_BOX;
        box.size[0] = 2.0f; box.size[1] = 1.0f; box.size[2] = 1.0f;
        const float shifted[3] = { 1.0f, 0.0f, 0.5f };
        daiblock::Mesh m = daiblock::finalise(
            daiblock::csg(daiblock::transform(daiblock::build(box), at, id_rot, one),
                          daiblock::transform(daiblock::build(box), shifted, id_rot, one),
                          daiblock::OP_UNION));
        blockout_gltf_roundtrip("Union", "/tmp/dai_blockout_union.glb", m, 2.0 + 2.0 - 0.5);
    }

    // --- the wall minus the arch: the sloped faces of a 12-gon ring ---------
    // Same numbers as tests/blockout_csg_cases.hpp: a 4 x 3 x 0.2 wall loses
    // the ring's cross section (outer half 12-gon minus inner) times 0.2.
    {
        daiblock::Shape wall;
        wall.kind = daiblock::KIND_BOX;
        wall.size[0] = 4.0f; wall.size[1] = 3.0f; wall.size[2] = 0.2f;
        wall.pivot[1] = -1.0f;
        daiblock::Shape arch;
        arch.kind = daiblock::KIND_ARCH;
        arch.size[0] = 1.2f; arch.size[1] = 1.0f; arch.size[2] = 0.6f;
        arch.segments = 12;
        arch.thickness = 0.2f;
        arch.pivot[1] = -1.0f;
        const float up[3] = { 0, 1.5f, 0 };
        daiblock::Mesh m = daiblock::finalise(
            daiblock::csg(daiblock::build(wall),
                          daiblock::transform(daiblock::build(arch), up, id_rot, one),
                          daiblock::OP_SUBTRACT));
        const double pi = 3.14159265358979323846, n = 12.0;
        double outer = 0.5 * n * std::sin(pi / n) * 0.6 * 1.0;
        double inner = 0.5 * n * std::sin(pi / n) * 0.4 * 0.8;
        blockout_gltf_roundtrip("WallMinusArch", "/tmp/dai_blockout_arch.glb", m,
                                2.4 - (outer - inner) * 0.2);
    }
}


#endif /* DAI_BLOCKOUT_GLTF_CASES_HPP */
