// Fracture test - the claim is that the pieces ARE the original.
//
// Anything can cut a mesh into overlapping shards that look plausible in a
// screenshot. The test that matters is arithmetic: sum the signed volume of
// every piece and it must equal the volume of what went in. That single number
// catches the three ways this can silently go wrong - a cut face left open
// (piece leaks volume), two cells claiming the same space (too much), a gap
// between cells (too little). It cannot be faked by eye.
//
// Second claim: the same seed gives the same rubble, byte for byte. A
// deterministic engine that rolls back cannot have geometry that differs
// between two runs of the same frame.
//
// Runs on the CPU only - no renderer, no Vulkan, no display.
//
//   ./build/test_fracture

#include "dai_fracture.h"
#include "dai_gltf.h"

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

// Signed volume by the divergence theorem: for a closed surface it is exactly
// the enclosed volume, and for an open one it is wrong - which is the point.
static double mesh_volume(const dai_mesh_data &m) {
    double v = 0;
    for (uint32_t i = 0; i + 2 < m.index_count; i += 3) {
        const dai_vec3 &a = m.vertices[m.indices[i]].position;
        const dai_vec3 &b = m.vertices[m.indices[i + 1]].position;
        const dai_vec3 &c = m.vertices[m.indices[i + 2]].position;
        v += ((double)a.x * ((double)b.y * c.z - (double)b.z * c.y)
            - (double)a.y * ((double)b.x * c.z - (double)b.z * c.x)
            + (double)a.z * ((double)b.x * c.y - (double)b.y * c.x)) / 6.0;
    }
    return v;
}

static double total_volume(const std::vector<dai_mesh_data> &v) {
    double t = 0;
    for (const auto &m : v) t += mesh_volume(m);
    return t;
}

// A unit cube, half extent `h`, flat normals, wound outwards. Volume (2h)^3.
static dai_mesh_data make_box(float h) {
    static const float F[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
    static const float Q[6][4][3] = {
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},
        {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
        {{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}},
        {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
        {{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}},
        {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}},
    };
    dai_mesh_data m{};
    m.vertex_count = 24;
    m.index_count = 36;
    m.vertices = (dai_vertex *)std::calloc(24, sizeof(dai_vertex));
    m.indices = (uint32_t *)std::calloc(36, sizeof(uint32_t));
    uint32_t vi = 0, ii = 0;
    for (int f = 0; f < 6; ++f) {
        uint32_t base = vi;
        for (int k = 0; k < 4; ++k) {
            m.vertices[vi].position = { Q[f][k][0] * h, Q[f][k][1] * h, Q[f][k][2] * h };
            m.vertices[vi].normal = { F[f][0], F[f][1], F[f][2] };
            ++vi;
        }
        const uint32_t o[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
        for (int k = 0; k < 6; ++k) m.indices[ii++] = o[k];
    }
    std::snprintf(m.name, sizeof(m.name), "Box");
    return m;
}

static std::vector<dai_mesh_data> run(const dai_mesh_data &in, uint32_t pieces, uint64_t seed) {
    dai_fracture_opts o = dai_fracture_opts_default();
    o.pieces = pieces;
    o.seed = seed;
    char err[256] = { 0 };
    uint32_t n = dai_fracture(&in, 1, &o, nullptr, 0, err, sizeof(err));
    if (!n) { std::printf("  (fracture returned nothing: %s)\n", err); return {}; }
    std::vector<dai_mesh_data> out(n);
    uint32_t got = dai_fracture(&in, 1, &o, out.data(), n, err, sizeof(err));
    CHECK(got == n, "two pass count disagreed: %u then %u", n, got);
    return out;
}

static bool same_geometry(const std::vector<dai_mesh_data> &a, const std::vector<dai_mesh_data> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].vertex_count != b[i].vertex_count || a[i].index_count != b[i].index_count)
            return false;
        for (uint32_t v = 0; v < a[i].vertex_count; ++v) {
            const dai_vec3 &p = a[i].vertices[v].position, &q = b[i].vertices[v].position;
            if (std::memcmp(&p, &q, sizeof(dai_vec3)) != 0) return false;   // bit exact, not close
        }
    }
    return true;
}

static void free_all(std::vector<dai_mesh_data> &v) {
    if (!v.empty()) dai_gltf_free_geometry(v.data(), (uint32_t)v.size());
    v.clear();
}

int main() {
    std::printf("fracture\n");

    dai_mesh_data box = make_box(1.0f);
    const double box_vol = mesh_volume(box);
    CHECK(std::fabs(box_vol - 8.0) < 1e-4, "the test box itself is wrong: %.6f", box_vol);

    // --- volume is conserved, at several piece counts -----------------------
    for (uint32_t pieces : { 2u, 5u, 8u, 20u, 50u }) {
        std::vector<dai_mesh_data> out = run(box, pieces, 42);
        CHECK(!out.empty(), "%u pieces: nothing came back", pieces);
        if (out.empty()) continue;
        double v = total_volume(out);
        CHECK(std::fabs(v - box_vol) < 1e-3,
              "%u pieces: volume %.6f, expected %.6f", pieces, v, box_vol);
        // Every piece must be a solid, not a sliver or an inside out shell.
        for (size_t i = 0; i < out.size(); ++i) {
            double pv = mesh_volume(out[i]);
            CHECK(pv > 1e-6, "%u pieces: piece %zu has volume %.9f", pieces, i, pv);
            CHECK(out[i].index_count >= 12, "%u pieces: piece %zu has %u indices",
                  pieces, i, out[i].index_count);
        }
        std::printf("  %2u asked -> %2zu solid, volume %.6f\n", pieces, out.size(), v);
        free_all(out);
    }

    // --- determinism --------------------------------------------------------
    std::vector<dai_mesh_data> a = run(box, 8, 42);
    std::vector<dai_mesh_data> b = run(box, 8, 42);
    std::vector<dai_mesh_data> c = run(box, 8, 43);
    CHECK(same_geometry(a, b), "the same seed produced different geometry");
    CHECK(!same_geometry(a, c), "two different seeds produced identical geometry");
    std::printf("  seed 42 twice: identical; seed 43: different\n");

    // --- the writer and the reader agree on what was produced ---------------
    {
        std::vector<dai_mesh_write> w(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            w[i].vertices = a[i].vertices;
            w[i].vertex_count = a[i].vertex_count;
            w[i].indices = a[i].indices;
            w[i].index_count = a[i].index_count;
            w[i].name = a[i].name;
        }
        char err[256] = { 0 };
        const char *path = "/tmp/dai_fracture_test.glb";
        CHECK(dai_gltf_write(path, w.data(), (uint32_t)w.size(), err, sizeof(err)) == DAI_OK,
              "writing failed: %s", err);

        std::vector<uint8_t> file;
        FILE *f = std::fopen(path, "rb");
        CHECK(f != nullptr, "cannot reopen what was just written");
        if (f) {
            char chunk[8192];
            size_t n;
            while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
                file.insert(file.end(), chunk, chunk + n);
            std::fclose(f);
        }
        uint32_t back = dai_gltf_read_geometry(file.data(), file.size(), nullptr, 0, err, sizeof(err));
        CHECK(back == a.size(), "wrote %zu meshes, read back %u (%s)", a.size(), back, err);
        std::vector<dai_mesh_data> rd(back ? back : 1);
        if (back) dai_gltf_read_geometry(file.data(), file.size(), rd.data(), back, err, sizeof(err));
        rd.resize(back);
        // The round trip must preserve the volume, which means it preserved the
        // vertex order, the index order and the winding.
        CHECK(std::fabs(total_volume(rd) - total_volume(a)) < 1e-3,
              "round trip changed the volume: %.6f vs %.6f", total_volume(rd), total_volume(a));
        std::printf("  round trip through .glb: %u meshes, volume %.6f\n", back, total_volume(rd));
        free_all(rd);
        std::remove(path);
    }
    free_all(a); free_all(b); free_all(c);

    // --- refusals -----------------------------------------------------------
    {
        char err[256] = { 0 };
        CHECK(dai_fracture(nullptr, 0, nullptr, nullptr, 0, err, sizeof(err)) == 0,
              "no input should have been refused");
        CHECK(err[0] != 0, "a refusal should say why");

        dai_mesh_data empty{};
        err[0] = 0;
        CHECK(dai_fracture(&empty, 1, nullptr, nullptr, 0, err, sizeof(err)) == 0,
              "an empty mesh should have been refused");

        // Asking for one piece is nonsense; it must clamp rather than divide by
        // zero or hand back the original pretending it fractured.
        dai_fracture_opts o = dai_fracture_opts_default();
        o.pieces = 1;
        err[0] = 0;
        uint32_t n = dai_fracture(&box, 1, &o, nullptr, 0, err, sizeof(err));
        CHECK(n >= 2 || n == 0, "pieces=1 produced %u", n);
        std::printf("  refusals: reported, not crashed\n");
    }

    // --- a non centred, non cubic box: the seeds must follow the bounds ------
    {
        dai_mesh_data slab = make_box(1.0f);
        for (uint32_t v = 0; v < slab.vertex_count; ++v) {
            slab.vertices[v].position.x = slab.vertices[v].position.x * 4.0f + 10.0f;
            slab.vertices[v].position.y *= 0.25f;
        }
        double want = mesh_volume(slab);
        std::vector<dai_mesh_data> out = run(slab, 12, 7);
        CHECK(!out.empty(), "an off centre slab produced nothing");
        if (!out.empty()) {
            double got = total_volume(out);
            CHECK(std::fabs(got - want) < 1e-3 * (want > 1 ? want : 1),
                  "slab volume %.6f, expected %.6f", got, want);
            std::printf("  off centre slab (8x0.5x2 at x=10): %zu pieces, volume %.4f of %.4f\n",
                        out.size(), got, want);
        }
        free_all(out);
        dai_gltf_free_geometry(&slab, 1);
    }

    dai_gltf_free_geometry(&box, 1);
    std::printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "ok", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
