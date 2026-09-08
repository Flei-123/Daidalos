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
// cannot leave the editor is a wall nobody can put in a game.
#ifndef DAI_BLOCKOUT_GLTF_CASES_HPP
#define DAI_BLOCKOUT_GLTF_CASES_HPP

#include "dai_blockout.h"
#include "dai_gltf.h"

#include <cstdio>
#include <vector>

// The result of a boolean is an ordinary mesh, which means it goes out through
// the ordinary exporter. Written to a .glb and read back, triangle for
// triangle: a wall with a doorway that cannot leave the editor is a wall
// nobody can put in a game.
static void test_blockout_gltf() {
    std::printf("blockout: a cut wall leaves through dai_gltf_write\n");

    daiblock::Shape wall;
    wall.kind = daiblock::KIND_BOX;
    wall.size[0] = 4.0f; wall.size[1] = 3.0f; wall.size[2] = 0.3f;
    wall.pivot[1] = -1.0f;
    daiblock::Shape hole;
    hole.kind = daiblock::KIND_BOX;
    hole.size[0] = 1.0f; hole.size[1] = 2.0f; hole.size[2] = 1.0f;
    hole.pivot[1] = -1.0f;
    const float id_rot[4] = { 0, 0, 0, 1 };
    const float one[3] = { 1, 1, 1 };
    const float at[3] = { 0, 0, 0 };
    daiblock::Mesh m = daiblock::finalise(
        daiblock::csg(daiblock::build(wall),
                      daiblock::transform(daiblock::build(hole), at, id_rot, one),
                      daiblock::OP_SUBTRACT));

    dai_mesh_write w{};
    w.vertices = m.verts.data();
    w.vertex_count = (uint32_t)m.verts.size();
    w.indices = m.idx.data();
    w.index_count = (uint32_t)m.idx.size();
    w.name = "Wall";
    char err[256] = { 0 };
    const char *path = "/tmp/dai_blockout_wall.glb";
    CHECK(dai_gltf_write(path, &w, 1, err, sizeof(err)) == DAI_OK,
          "the cut wall could not be written as glTF: %s", err);

    std::FILE *f = std::fopen(path, "rb");
    CHECK(f != nullptr, "the exported .glb is not there");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> bytes((size_t)(size > 0 ? size : 0));
        size_t rd = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        CHECK(rd == bytes.size() && rd > 0, "the exported .glb could not be read back");

        dai_mesh_data md[4];
        char rerr[256] = { 0 };
        uint32_t n = dai_gltf_read_geometry(bytes.data(), bytes.size(), md, 4, rerr, sizeof(rerr));
        CHECK(n == 1, "the exported wall came back as %u primitives: %s", n, rerr);
        if (n == 1) {
            CHECK(md[0].vertex_count == m.verts.size() && md[0].index_count == m.idx.size(),
                  "the round trip changed the triangle count: %u/%u vs %zu/%zu",
                  md[0].vertex_count, md[0].index_count, m.verts.size(), m.idx.size());
            daiblock::Mesh back;
            back.verts.assign(md[0].vertices, md[0].vertices + md[0].vertex_count);
            back.idx.assign(md[0].indices, md[0].indices + md[0].index_count);
            int open_e = 0, bad_e = 0;
            daiblock::edge_report(back, &open_e, &bad_e);
            CHECK(open_e == 0 && bad_e == 0,
                  "the wall that came back from glTF is not closed: %d open, %d shared by more than two",
                  open_e, bad_e);
            CHECK(daiblock::inconsistent_edges(back) == 0,
                  "the wall that came back from glTF has a face wound backwards");
            double want = 4.0 * 3.0 * 0.3 - 1.0 * 2.0 * 0.3;
            CHECK(std::fabs(daiblock::volume(back) - want) < 1e-4,
                  "the exported wall holds %.6f m3, the original %.6f",
                  daiblock::volume(back), want);
            dai_gltf_free_geometry(md, n);
        }
        std::remove(path);
    }
}


#endif /* DAI_BLOCKOUT_GLTF_CASES_HPP */
