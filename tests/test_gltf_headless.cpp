// glTF WITHOUT a renderer - the half of the importer that is arithmetic.
//
//   ./build/test_gltf_headless [assets/test] [outdir]
//
// tests/test_gltf.cpp opens a Vulkan device before it reads its first byte, so
// it sits on run_tests.sh's "needs GPU" list and nothing ever runs it here. But
// the parts of glTF that can be wrong without a GPU - the container, the
// accessors, the index ranges, the winding, the writer and the round trip - are
// most of it, and an excluded test rots: `dai_gltf_read_geometry` and
// `dai_gltf_write` are what the blockout exporter and the fracture baker stand
// on, and neither of them has ever owned a renderer.
//
// So this suite reads the SAME file test_gltf reads - assets/test/blender_scene.glb,
// exported by a real Blender 4.5 through tools/make_testscene.py, five meshes
// with normals, UVs and a non uniform scale among them - and checks the
// geometry it gets back against what a mesh has to be. Nothing here is a
// fixture we wrote ourselves; the only thing this file produces is the round
// trip, and that is the point of it.

#include "dai_gltf.h"
#include "dai_render.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static std::vector<uint8_t> read_file(const std::string &path) {
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

// What a triangle soup has to satisfy whatever produced it. Every one of these
// is a way a real importer bug shows up: an accessor read at the wrong stride
// gives NaNs, a byte offset off by one gives indices past the end, a normal
// read as a byte gives lengths that are not 1, and a mesh whose bounding box
// has no volume in any axis was read as zero.
struct MeshVerdict {
    int    finite = 1;
    int    indexed_in_range = 1;
    int    normals_unit = 1;
    int    uvs_finite = 1;
    int    has_area = 1;
    double area = 0.0;
    dai_vec3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
};

static MeshVerdict judge(const dai_mesh_data &m) {
    MeshVerdict v;
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        const dai_vertex &q = m.vertices[i];
        const float f[8] = { q.position.x, q.position.y, q.position.z,
                             q.normal.x, q.normal.y, q.normal.z, q.u, q.v };
        for (int k = 0; k < 6; ++k) if (!std::isfinite(f[k])) v.finite = 0;
        for (int k = 6; k < 8; ++k) if (!std::isfinite(f[k])) v.uvs_finite = 0;
        float nl = std::sqrt(q.normal.x * q.normal.x + q.normal.y * q.normal.y +
                             q.normal.z * q.normal.z);
        if (std::fabs(nl - 1.0f) > 1e-3f) v.normals_unit = 0;
        if (q.position.x < v.lo.x) v.lo.x = q.position.x;
        if (q.position.y < v.lo.y) v.lo.y = q.position.y;
        if (q.position.z < v.lo.z) v.lo.z = q.position.z;
        if (q.position.x > v.hi.x) v.hi.x = q.position.x;
        if (q.position.y > v.hi.y) v.hi.y = q.position.y;
        if (q.position.z > v.hi.z) v.hi.z = q.position.z;
    }
    for (uint32_t i = 0; i < m.index_count; ++i)
        if (m.indices[i] >= m.vertex_count) v.indexed_in_range = 0;
    // Twice the surface area, summed over the triangles. A mesh whose triangles
    // are all degenerate reads back with the right counts and draws nothing.
    for (uint32_t i = 0; i + 2 < m.index_count && v.indexed_in_range; i += 3) {
        const dai_vec3 &a = m.vertices[m.indices[i]].position;
        const dai_vec3 &b = m.vertices[m.indices[i + 1]].position;
        const dai_vec3 &c = m.vertices[m.indices[i + 2]].position;
        dai_vec3 e1{ b.x - a.x, b.y - a.y, b.z - a.z };
        dai_vec3 e2{ c.x - a.x, c.y - a.y, c.z - a.z };
        dai_vec3 n{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                    e1.x * e2.y - e1.y * e2.x };
        v.area += std::sqrt((double)n.x * n.x + (double)n.y * n.y + (double)n.z * n.z) * 0.5;
    }
    if (!(v.area > 1e-6)) v.has_area = 0;
    return v;
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "assets/test";
    std::string outdir = argc > 2 ? argv[2] : "/tmp";

    std::printf("glTF, headless: %s\n", dir.c_str());

    // ---- 1. the container ---------------------------------------------------
    // Reading a .glb is reading a header, two chunks and a JSON document, and
    // every one of those can be wrong on its own.
    std::vector<uint8_t> glb = read_file(dir + "/blender_scene.glb");
    CHECK(glb.size() > 1024, "assets/test/blender_scene.glb is %u bytes - fixture missing?",
          (uint32_t)glb.size());
    if (glb.size() < 1024) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    CHECK(std::memcmp(glb.data(), "glTF", 4) == 0, "the fixture does not start with the glTF magic");

    char err[256] = { 0 };
    dai_mesh_data meshes[32];
    uint32_t n = dai_gltf_read_geometry(glb.data(), glb.size(), meshes, 32, err, sizeof(err));
    CHECK(n > 0, "read_geometry found no primitive in blender_scene.glb: %s", err);
    CHECK(n == 5, "blender_scene.glb has five objects, read_geometry found %u", n);
    if (!n) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

    // The count is answered even when nothing is filled: a caller sizes its
    // array from it before it has one.
    uint32_t only_count = dai_gltf_read_geometry(glb.data(), glb.size(), nullptr, 0, err, sizeof(err));
    CHECK(only_count == n, "counting without an output said %u, filling said %u", only_count, n);
    {
        dai_mesh_data one[1];
        uint32_t capped = dai_gltf_read_geometry(glb.data(), glb.size(), one, 1, err, sizeof(err));
        CHECK(capped == n, "a one element array changed the count to %u", capped);
        CHECK(one[0].vertex_count > 0, "the first primitive came back empty when capped");
        dai_gltf_free_geometry(one, 1);
    }

    // ---- 2. every primitive, against what a mesh has to be ------------------
    uint32_t total_v = 0, total_i = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const dai_mesh_data &m = meshes[i];
        MeshVerdict v = judge(m);
        total_v += m.vertex_count;
        total_i += m.index_count;
        CHECK(m.vertex_count > 0, "primitive %u has no vertices", i);
        CHECK(m.index_count > 0, "primitive %u has no indices", i);
        CHECK(m.index_count % 3 == 0, "primitive %u has %u indices, not a whole number of triangles",
              i, m.index_count);
        CHECK(m.vertices != nullptr && m.indices != nullptr,
              "primitive %u reported counts but no arrays", i);
        CHECK(v.indexed_in_range, "primitive %u indexes past its %u vertices", i, m.vertex_count);
        CHECK(v.finite, "primitive %u has a non finite position or normal", i);
        CHECK(v.uvs_finite, "primitive %u has a non finite texture coordinate", i);
        CHECK(v.normals_unit, "primitive %u has normals that are not unit length", i);
        CHECK(v.has_area, "primitive %u encloses no area - every triangle is degenerate", i);
        // Two axes with extent, not three: the fixture's ground IS a plane, and
        // a surface is allowed to be flat. A mesh with extent in one axis or
        // none is a line or a point, and that is a read that went wrong.
        int fat_axes = (v.hi.x - v.lo.x > 1e-6f) + (v.hi.y - v.lo.y > 1e-6f) +
                       (v.hi.z - v.lo.z > 1e-6f);
        CHECK(fat_axes >= 2,
              "primitive %u has extent in %d axes: %.3f x %.3f x %.3f", i, fat_axes,
              (double)(v.hi.x - v.lo.x), (double)(v.hi.y - v.lo.y), (double)(v.hi.z - v.lo.z));
        CHECK(m.name[0] != 0, "primitive %u came back nameless", i);
        // The mesh's own space: Blender's objects are metres, and a primitive
        // read at the wrong scale (a byte accessor taken as a float, say) is
        // out by orders of magnitude rather than by a little.
        float span = v.hi.x - v.lo.x;
        if (v.hi.y - v.lo.y > span) span = v.hi.y - v.lo.y;
        if (v.hi.z - v.lo.z > span) span = v.hi.z - v.lo.z;
        CHECK(span > 0.01f && span < 1000.0f,
              "primitive %u (%s) spans %.4f m - that is not a mesh in metres", i, m.name, (double)span);
        std::printf("  %-18s %5u verts %5u idx  %.3f m span, area %.4f\n",
                    m.name, m.vertex_count, m.index_count, (double)span, v.area);
    }
    CHECK(total_v > 100 && total_i > 300, "the whole file is %u verts / %u indices - too little to be it",
          total_v, total_i);

    // ---- 3. reading twice gives the same thing ------------------------------
    // The importer holds no state between calls, and a reader that does is a
    // reader whose second answer depends on its first.
    {
        dai_mesh_data again[32];
        uint32_t n2 = dai_gltf_read_geometry(glb.data(), glb.size(), again, 32, err, sizeof(err));
        CHECK(n2 == n, "the second read found %u primitives, the first %u", n2, n);
        for (uint32_t i = 0; i < n2 && i < n; ++i) {
            CHECK(again[i].vertex_count == meshes[i].vertex_count &&
                  again[i].index_count == meshes[i].index_count,
                  "primitive %u changed size between two reads", i);
            int same = 1;
            for (uint32_t k = 0; k < again[i].vertex_count && k < meshes[i].vertex_count; ++k)
                if (std::memcmp(&again[i].vertices[k].position, &meshes[i].vertices[k].position,
                                sizeof(dai_vec3)) != 0) same = 0;
            CHECK(same, "primitive %u came back with different positions on the second read", i);
        }
        dai_gltf_free_geometry(again, n2 < 32 ? n2 : 32);
    }

    // ---- 4. the round trip: write what was read, read it back ---------------
    // This is the promise the blockout exporter makes - a CSG result written
    // out is the same geometry when it comes back - and it needs no renderer
    // to be true or false.
    for (uint32_t i = 0; i < n; ++i) {
        const dai_mesh_data &m = meshes[i];
        dai_mesh_write mw{};
        mw.vertices = m.vertices;
        mw.vertex_count = m.vertex_count;
        mw.indices = m.indices;
        mw.index_count = m.index_count;
        mw.name = m.name;
        std::string path = outdir + "/gltf_headless_" + std::to_string(i) + ".glb";
        dai_result rc = dai_gltf_write(path.c_str(), &mw, 1, err, sizeof(err));
        CHECK(rc == DAI_OK, "writing primitive %u failed: %s", i, err);
        if (rc != DAI_OK) continue;

        std::vector<uint8_t> back = read_file(path);
        CHECK(back.size() > 128, "the written %s is %u bytes", path.c_str(), (uint32_t)back.size());
        CHECK(std::memcmp(back.data(), "glTF", 4) == 0, "the writer did not emit a .glb header");

        dai_mesh_data rt[4];
        uint32_t rn = dai_gltf_read_geometry(back.data(), back.size(), rt, 4, err, sizeof(err));
        CHECK(rn == 1, "the written primitive %u read back as %u primitives", i, rn);
        if (rn != 1) continue;
        CHECK(rt[0].vertex_count == m.vertex_count,
              "round trip of %s: %u vertices out, %u back", m.name, m.vertex_count, rt[0].vertex_count);
        CHECK(rt[0].index_count == m.index_count,
              "round trip of %s: %u indices out, %u back", m.name, m.index_count, rt[0].index_count);
        CHECK(rt[0].index_count / 3 == m.index_count / 3,
              "round trip of %s changed the triangle count", m.name);
        int idx_same = rt[0].index_count == m.index_count;
        for (uint32_t k = 0; k < rt[0].index_count && idx_same; ++k)
            if (rt[0].indices[k] != m.indices[k]) idx_same = 0;
        CHECK(idx_same, "round trip of %s reordered the indices", m.name);
        float worst_p = 0.0f, worst_nrm = 0.0f;
        for (uint32_t k = 0; k < rt[0].vertex_count && k < m.vertex_count; ++k) {
            const dai_vertex &a = m.vertices[k], &b = rt[0].vertices[k];
            float dp = std::fabs(a.position.x - b.position.x) + std::fabs(a.position.y - b.position.y) +
                       std::fabs(a.position.z - b.position.z);
            float dn = std::fabs(a.normal.x - b.normal.x) + std::fabs(a.normal.y - b.normal.y) +
                       std::fabs(a.normal.z - b.normal.z);
            if (dp > worst_p) worst_p = dp;
            if (dn > worst_nrm) worst_nrm = dn;
        }
        CHECK(worst_p < 1e-5f, "round trip of %s moved a vertex by %.7f", m.name, (double)worst_p);
        CHECK(worst_nrm < 1e-5f, "round trip of %s turned a normal by %.7f", m.name, (double)worst_nrm);
        MeshVerdict rv = judge(rt[0]);
        MeshVerdict ov = judge(m);
        CHECK(rv.indexed_in_range && rv.finite && rv.normals_unit,
              "the round trip of %s is not a valid mesh any more", m.name);
        CHECK(std::fabs(rv.area - ov.area) < 1e-4 * (ov.area + 1.0),
              "round trip of %s changed its surface area: %.6f -> %.6f", m.name, ov.area, rv.area);

        // Determinism: the same geometry written twice is the same bytes. A
        // writer that stamps a date or walks a hash map is a writer whose
        // output cannot be diffed, and the blockout round rests on that diff.
        std::string path2 = outdir + "/gltf_headless_" + std::to_string(i) + "_b.glb";
        CHECK(dai_gltf_write(path2.c_str(), &mw, 1, err, sizeof(err)) == DAI_OK,
              "the second write of %s failed: %s", m.name, err);
        std::vector<uint8_t> back2 = read_file(path2);
        CHECK(back2.size() == back.size() && !back2.empty() &&
              std::memcmp(back.data(), back2.data(), back.size()) == 0,
              "writing %s twice gave two different files (%u vs %u bytes)",
              m.name, (uint32_t)back.size(), (uint32_t)back2.size());
        std::remove(path.c_str());
        std::remove(path2.c_str());
        dai_gltf_free_geometry(rt, 1);
    }

    // ---- 5. two meshes in one file stay two meshes --------------------------
    {
        dai_mesh_write mw[2];
        std::memset(mw, 0, sizeof(mw));
        mw[0].vertices = meshes[0].vertices; mw[0].vertex_count = meshes[0].vertex_count;
        mw[0].indices = meshes[0].indices;   mw[0].index_count = meshes[0].index_count;
        mw[0].name = "first";
        uint32_t last = n - 1;
        mw[1].vertices = meshes[last].vertices; mw[1].vertex_count = meshes[last].vertex_count;
        mw[1].indices = meshes[last].indices;   mw[1].index_count = meshes[last].index_count;
        mw[1].name = "second";
        std::string path = outdir + "/gltf_headless_pair.glb";
        CHECK(dai_gltf_write(path.c_str(), mw, 2, err, sizeof(err)) == DAI_OK,
              "writing two meshes failed: %s", err);
        std::vector<uint8_t> back = read_file(path);
        dai_mesh_data rt[4];
        uint32_t rn = dai_gltf_read_geometry(back.data(), back.size(), rt, 4, err, sizeof(err));
        CHECK(rn == 2, "two meshes written, %u read back", rn);
        if (rn == 2) {
            CHECK(rt[0].vertex_count == meshes[0].vertex_count &&
                  rt[1].vertex_count == meshes[last].vertex_count,
                  "the pair came back with the wrong sizes (%u, %u)",
                  rt[0].vertex_count, rt[1].vertex_count);
            CHECK(std::strcmp(rt[0].name, "first") == 0 || rt[0].name[0] != 0,
                  "the first mesh of the pair lost its name");
            CHECK(rt[0].index_count + rt[1].index_count ==
                  meshes[0].index_count + meshes[last].index_count,
                  "the pair lost triangles in the round trip");
            dai_gltf_free_geometry(rt, rn);
        }
        std::remove(path.c_str());
    }

    // ---- 6. what a broken file does -----------------------------------------
    // Every one of these used to be a segfault in some importer somewhere. An
    // error is an answer; a crash is not.
    {
        err[0] = 0;
        CHECK(dai_gltf_read_geometry(nullptr, 0, meshes, 0, err, sizeof(err)) == 0,
              "reading nothing found something");
        const char junk[64] = "this is not a glTF file, it is a sentence";
        err[0] = 0;
        CHECK(dai_gltf_read_geometry(junk, sizeof(junk), nullptr, 0, err, sizeof(err)) == 0,
              "a sentence parsed as glTF");
        CHECK(err[0] != 0, "a rejected file came back without a reason");
        // A truncated .glb: the header is right, the chunks are cut off.
        std::vector<uint8_t> cut(glb.begin(), glb.begin() + 64);
        err[0] = 0;
        uint32_t cn = dai_gltf_read_geometry(cut.data(), cut.size(), nullptr, 0, err, sizeof(err));
        CHECK(cn == 0, "a 64 byte .glb yielded %u primitives", cn);
        // ...and one whose length field lies about the file.
        std::vector<uint8_t> lying = glb;
        lying[8] = 0xFF; lying[9] = 0xFF; lying[10] = 0xFF; lying[11] = 0x7F;
        err[0] = 0;
        uint32_t ln = dai_gltf_read_geometry(lying.data(), lying.size(), nullptr, 0, err, sizeof(err));
        CHECK(ln == 0 || ln == n, "a .glb claiming 2 GB answered %u primitives", ln);
        err[0] = 0;
        CHECK(dai_gltf_write("/does/not/exist/nowhere.glb", nullptr, 0, err, sizeof(err)) != DAI_OK,
              "writing nothing to nowhere reported success");
        CHECK(err[0] != 0, "a refused write came back without a reason");
    }

    dai_gltf_free_geometry(meshes, n);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
