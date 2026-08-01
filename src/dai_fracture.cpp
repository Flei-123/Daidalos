// Voronoi fracture, as a function rather than a program.
//
// It lives here and not in the tool because two callers want it: daifracture
// bakes with it at build time, and a game that really does need a cut decided
// at runtime - a saw, a player drawn cut line - can call the same code and get
// the same pieces. Baking stays the default; determinism is easier to keep when
// the answer was computed once.
//
// The method is Voronoi by half spaces. Scatter N points in the mesh's box;
// piece i is everything closer to point i than to any other, which is the
// intersection of the half spaces bisecting i and each j. A triangle soup
// clipped against a plane is Sutherland-Hodgman, and the hole that leaves gets
// a cap built from the cut edges - without it the pieces are hollow shells and
// look like paper the moment they tumble.
//
// Stated limits: the cap assumes the cut cross section is convex, true for a
// convex mesh and always true of a Voronoi cell, only approximately true for a
// concave model. UVs are not carried onto new faces - a broken stone wants rock
// on the outside and flat colour inside, and inventing coordinates would be
// worse than leaving them at zero.

#include "dai_fracture.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {


struct V3 {
    float x = 0, y = 0, z = 0;
    V3() {}
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3(const dai_vec3 &v) : x(v.x), y(v.y), z(v.z) {}
    dai_vec3 to() const { return { x, y, z }; }
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) { float l = length(a); return l > 1e-12f ? a * (1.0f / l) : V3(0, 1, 0); }

// A triangle carries its vertices' normals so a clipped face keeps the shading
// the artist gave it; only the new inner faces get a flat one.
struct Tri { V3 p[3]; V3 n[3]; };

struct Plane { V3 n; float d; };                 // dot(n, x) <= d is inside
float signed_dist(const Plane &pl, V3 p) { return dot(pl.n, p) - pl.d; }

// Deterministic by construction: same seed, same rubble, on any machine.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 32);
    }
    float unit() { return (float)(next() & 0xFFFFFF) / (float)0xFFFFFF; }
};

// ---- clipping ------------------------------------------------------------

// One triangle against one plane. Produces 0, 1 or 2 triangles, and reports the
// cut segment so the caller can close the hole afterwards.
void clip_tri(const Tri &t, const Plane &pl, std::vector<Tri> &out,
              std::vector<std::pair<V3, V3>> &cut_edges) {
    float d[3] = { signed_dist(pl, t.p[0]), signed_dist(pl, t.p[1]), signed_dist(pl, t.p[2]) };
    int inside[3], in_n = 0, out_n = 0;
    for (int i = 0; i < 3; ++i) {
        inside[i] = d[i] <= 1e-6f;
        if (inside[i]) ++in_n; else ++out_n;
    }
    if (in_n == 3) { out.push_back(t); return; }
    if (in_n == 0) return;

    // Walk the edges, keeping inside vertices and the points where the edge
    // crosses the plane. The crossings are the cut segment.
    V3 poly[4], pnrm[4];
    int pn = 0;
    V3 seg[2];
    int seg_n = 0;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        if (inside[i]) { poly[pn] = t.p[i]; pnrm[pn] = t.n[i]; ++pn; }
        if (inside[i] != inside[j]) {
            float u = d[i] / (d[i] - d[j]);
            V3 p = t.p[i] + (t.p[j] - t.p[i]) * u;
            V3 n = normalize(t.n[i] + (t.n[j] - t.n[i]) * u);
            if (pn < 4) { poly[pn] = p; pnrm[pn] = n; ++pn; }
            if (seg_n < 2) seg[seg_n++] = p;
        }
    }
    if (seg_n == 2) cut_edges.push_back({ seg[0], seg[1] });
    for (int i = 2; i < pn; ++i) {                // fan the resulting polygon
        Tri r;
        r.p[0] = poly[0];   r.n[0] = pnrm[0];
        r.p[1] = poly[i-1]; r.n[1] = pnrm[i-1];
        r.p[2] = poly[i];   r.n[2] = pnrm[i];
        out.push_back(r);
    }
}

// Closes the hole a cut left behind. The cut edges form a loop in the plane;
// fanning them around their centroid fills it. Correct for a convex cross
// section, which is what a Voronoi cell of a convex mesh always gives.
void cap_hole(const std::vector<std::pair<V3, V3>> &edges, const Plane &pl,
              std::vector<Tri> &out) {
    if (edges.size() < 3) return;
    V3 c;
    for (const auto &e : edges) c = c + e.first + e.second;
    c = c * (1.0f / (float)(edges.size() * 2));

    for (const auto &e : edges) {
        V3 a = e.first, b = e.second;
        // Wind so the cap faces out of the piece, along the plane normal.
        V3 face = cross(a - c, b - c);
        Tri t;
        t.p[0] = c;
        if (dot(face, pl.n) >= 0.0f) { t.p[1] = a; t.p[2] = b; }
        else                          { t.p[1] = b; t.p[2] = a; }
        for (int i = 0; i < 3; ++i) t.n[i] = pl.n;
        if (length(cross(t.p[1] - t.p[0], t.p[2] - t.p[0])) < 1e-12f) continue;   // degenerate
        out.push_back(t);
    }
}

std::vector<Tri> clip_against(const std::vector<Tri> &in, const Plane &pl) {
    std::vector<Tri> out;
    std::vector<std::pair<V3, V3>> cut;
    out.reserve(in.size());
    for (const Tri &t : in) clip_tri(t, pl, out, cut);
    if (!out.empty()) cap_hole(cut, pl, out);
    return out;
}

} // namespace

extern "C" {

dai_fracture_opts dai_fracture_opts_default(void) {
    dai_fracture_opts o;
    o.pieces = 8;
    o.seed = 1;
    o.inset = 0.1f;
    return o;
}

uint32_t dai_fracture(const dai_mesh_data *in, uint32_t in_count, const dai_fracture_opts *opts,
                      dai_mesh_data *out, uint32_t max_out, char *err, size_t err_len) {
    auto fail = [&](const char *m) -> uint32_t {
        if (err && err_len) std::snprintf(err, err_len, "%s", m);
        return 0;
    };
    if (!in || !in_count) return fail("no input geometry");
    dai_fracture_opts o = opts ? *opts : dai_fracture_opts_default();
    if (o.pieces < 2) o.pieces = 2;
    if (o.pieces > 256) o.pieces = 256;
    if (o.inset < 0.0f || o.inset > 0.45f) o.inset = 0.1f;

    // Every primitive becomes one triangle soup: a stone exported as three
    // primitives is still one stone, and the pieces should not follow the
    // exporter's idea of where the material boundaries were.
    std::vector<Tri> soup;
    V3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (uint32_t m = 0; m < in_count; ++m) {
        const dai_mesh_data &d = in[m];
        if (!d.vertices || !d.indices) continue;
        for (uint32_t i = 0; i + 2 < d.index_count; i += 3) {
            Tri t;
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                uint32_t ix = d.indices[i + k];
                if (ix >= d.vertex_count) { ok = false; break; }
                const dai_vertex &v = d.vertices[ix];
                t.p[k] = V3(v.position);
                t.n[k] = V3(v.normal);
            }
            if (!ok) continue;
            for (int k = 0; k < 3; ++k) {
                lo = { lo.x < t.p[k].x ? lo.x : t.p[k].x, lo.y < t.p[k].y ? lo.y : t.p[k].y,
                       lo.z < t.p[k].z ? lo.z : t.p[k].z };
                hi = { hi.x > t.p[k].x ? hi.x : t.p[k].x, hi.y > t.p[k].y ? hi.y : t.p[k].y,
                       hi.z > t.p[k].z ? hi.z : t.p[k].z };
            }
            soup.push_back(t);
        }
    }
    if (soup.empty()) return fail("input has no triangles");

    // Seed points, inset from the surface so no cell is a sliver.
    Rng rng(o.seed);
    V3 size = hi - lo;
    const float span = 1.0f - 2.0f * o.inset;
    std::vector<V3> seeds(o.pieces);
    for (uint32_t i = 0; i < o.pieces; ++i)
        seeds[i] = { lo.x + size.x * (o.inset + span * rng.unit()),
                     lo.y + size.y * (o.inset + span * rng.unit()),
                     lo.z + size.z * (o.inset + span * rng.unit()) };

    uint32_t found = 0;
    for (uint32_t i = 0; i < o.pieces; ++i) {
        std::vector<Tri> cell = soup;
        for (uint32_t j = 0; j < o.pieces && !cell.empty(); ++j) {
            if (i == j) continue;
            V3 mid = (seeds[i] + seeds[j]) * 0.5f;
            V3 n = normalize(seeds[j] - seeds[i]);
            Plane pl{ n, dot(n, mid) };
            cell = clip_against(cell, pl);
        }
        if (cell.size() < 4) continue;              // nothing solid survived
        uint32_t slot = found++;
        if (!out || slot >= max_out) continue;      // counting pass

        dai_mesh_data &md = out[slot];
        md = dai_mesh_data{};
        md.vertex_count = (uint32_t)cell.size() * 3;
        md.index_count = md.vertex_count;
        md.vertices = (dai_vertex *)std::malloc(md.vertex_count * sizeof(dai_vertex));
        md.indices = (uint32_t *)std::malloc(md.index_count * sizeof(uint32_t));
        if (!md.vertices || !md.indices) { std::free(md.vertices); std::free(md.indices);
                                          md = dai_mesh_data{}; return fail("out of memory"); }
        uint32_t w = 0;
        for (const Tri &t : cell) {
            for (int k = 0; k < 3; ++k) {
                dai_vertex &v = md.vertices[w];
                v = dai_vertex{};
                v.position = t.p[k].to();
                v.normal = normalize(t.n[k]).to();
                md.indices[w] = w;
                ++w;
            }
        }
        std::snprintf(md.name, sizeof(md.name), "piece%03u", (unsigned)slot);
    }
    if (!found) return fail("every cell came out empty");
    return found;
}

} // extern "C"
