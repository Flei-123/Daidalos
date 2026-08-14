// Stage 1: a mesh becomes exactly N points that are never too close together.
//
// Implements, from include/dai_show.h:
//     dai_show_sample_feasible
//     dai_show_sample
//
// The order of the work is the part worth writing down. Feasibility FIRST,
// because "N drones at d metres" is a packing question with a closed form
// answer and a tool that discovers the impossibility by failing to converge
// has already wasted the operator's afternoon. Then dart throwing against a
// spatial hash for the Poisson disk property, then Lloyd relaxation to even
// the result out - and the relaxation has to re-check the minimum distance
// afterwards, because moving a point to its cell's centroid can move it
// towards a neighbour.
//
// Everything draws from the seed in the descriptor. There is no rand() here.
//
// HOW THE THREE MODES SHARE ONE MACHINE. Every mode produces the same thing: a
// pool of candidate points that already lie where drones are allowed to be,
// each carrying the colour of the surface it came from. Poisson selection,
// relaxation and the final proof then run on the pool and know nothing about
// triangles. So there is one Poisson implementation and one relaxation rather
// than three, and a bug in either is a bug in all three modes at once - which
// is the only way a safety property stays believable.
//
// WHY THE RELAXATION FILTERS RATHER THAN REPAIRS. Lloyd moves a point to the
// centroid of the region it owns, which is exactly the move that can push it
// into a neighbour. The usual answer is to relax first and repair afterwards,
// which means the guarantee holds only if the repair loop converged. Here the
// move is offered to the constraint and dropped when it breaks it: the point
// set is legal after every single step, so it is legal at the end, and there is
// no convergence to hope for.

#include "dai_show.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ---- small vector maths ---------------------------------------------------

struct V3 { float x, y, z; };

inline V3   mk(float x, float y, float z)   { V3 r = { x, y, z }; return r; }
inline V3   add(V3 a, V3 b)                 { return mk(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3   sub(V3 a, V3 b)                 { return mk(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3   mul(V3 a, float s)              { return mk(a.x * s, a.y * s, a.z * s); }
// Summed in double, and that is about the safety number rather than about
// accuracy: this product is the squared distance that decides whether a drone
// may be placed, and a float sum of three squares of coordinates that are
// hundreds of metres from the origin has already lost the last couple of
// centimetres. In double the comparison against min_distance means what it
// says. (Bit identical results are guaranteed for one binary run twice, which
// is what the guarantee is about; the engine builds with -ffp-contract=fast
// throughout, so a rebuild with different flags rounds differently here exactly
// as it does everywhere else in the engine.)
inline float dot(V3 a, V3 b) {
    return (float)((double)a.x * (double)b.x + (double)a.y * (double)b.y + (double)a.z * (double)b.z);
}
inline V3   cross(V3 a, V3 b) {
    return mk((float)((double)a.y * b.z - (double)a.z * b.y),
              (float)((double)a.z * b.x - (double)a.x * b.z),
              (float)((double)a.x * b.y - (double)a.y * b.x));
}
// Three weighted vectors added up in double, for the same reason dot() is: this
// is where a sample point's coordinates are decided, a hundred metres out from
// the origin, and the last centimetre of that is the one the minimum distance
// is measured in.
inline V3 comb3(V3 a, double wa, V3 b, double wb, V3 c, double wc) {
    return mk((float)((double)a.x * wa + (double)b.x * wb + (double)c.x * wc),
              (float)((double)a.y * wa + (double)b.y * wb + (double)c.y * wc),
              (float)((double)a.z * wa + (double)b.z * wb + (double)c.z * wc));
}
inline float length(V3 a)                   { return std::sqrt(dot(a, a)); }
inline V3 normalise(V3 a) {
    float l = length(a);
    return l > 1e-12f ? mul(a, 1.0f / l) : mk(0.0f, 0.0f, 1.0f);
}

// SplitMix64. Small, seekable, and - the reason it is here rather than
// <random> - specified as integer arithmetic, so it produces the same stream on
// every compiler and every machine. A show that is signed off as collision free
// has to sample the same points tomorrow.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed + 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // [0,1) out of 24 bits: exactly representable in a float, so the same draw
    // never lands on a different side of a comparison after a rebuild.
    float unit() { return (float)(next() >> 40) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return (float)((double)lo + ((double)hi - (double)lo) * (double)unit()); }
};

// ---- packing bounds -------------------------------------------------------

// The densest packings that exist, and therefore the only honest ceilings:
// hexagonal in the plane puts one point per d^2*sqrt(3)/2 of area, face centred
// cubic one per d^3/sqrt(2) of volume. Real sampling never reaches either -
// which is the point. If the request already fails against the theoretical
// optimum, no amount of cleverness in the sampler will save it.
const double HEX_CELL = 0.86602540378443864676;  /* sqrt(3)/2  */
const double FCC_CELL = 0.70710678118654752440;  /* 1/sqrt(2)  */

}  // namespace

// ---------------------------------------------------------------------------
// Measuring the figure
// ---------------------------------------------------------------------------

namespace {

// Everything the feasibility answer needs, in show metres, without allocating:
// dai_show_sample_feasible is called from a panel while a number is being
// typed, so it walks the triangles and divides and does nothing else.
struct Measure {
    double area;        /* total triangle area, m^2                          */
    double volume;      /* enclosed volume by the divergence theorem, m^3    */
    double silhouette;  /* projected outline area along view_dir, m^2        */
    V3     bmin, bmax;  /* bounding box in metres, before centring           */
    uint32_t tris;
};

// The audience's frame: `dir` points from the audience into the figure, u and v
// span the plane they see. Handed in from view_dir so SILHOUETTE and its
// feasibility bound agree on what "the outline" means.
struct View {
    V3 dir, u, v;
};

View view_of(dai_vec3 view_dir) {
    View w;
    w.dir = normalise(mk(view_dir.x, view_dir.y, view_dir.z));
    V3 up = (std::fabs(w.dir.y) < 0.9f) ? mk(0.0f, 1.0f, 0.0f) : mk(1.0f, 0.0f, 0.0f);
    w.u = normalise(cross(up, w.dir));
    w.v = cross(w.dir, w.u);
    return w;
}

int measure(const dai_show_sample_desc *d, Measure *m) {
    if (!d || !d->positions || !d->indices || d->index_count < 3 || d->vertex_count < 3)
        return 0;

    const float scale = (d->scale > 0.0f) ? d->scale : 1.0f;
    View w = view_of(d->view_dir);

    m->area = m->volume = m->silhouette = 0.0;
    m->tris = d->index_count / 3u;
    m->bmin = mk(1e30f, 1e30f, 1e30f);
    m->bmax = mk(-1e30f, -1e30f, -1e30f);

    for (uint32_t t = 0; t < m->tris; ++t) {
        V3 p[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t vi = d->indices[t * 3u + (uint32_t)k];
            if (vi >= d->vertex_count) return 0;
            p[k] = mk(d->positions[vi * 3u + 0] * scale,
                      d->positions[vi * 3u + 1] * scale,
                      d->positions[vi * 3u + 2] * scale);
            m->bmin.x = std::min(m->bmin.x, p[k].x); m->bmax.x = std::max(m->bmax.x, p[k].x);
            m->bmin.y = std::min(m->bmin.y, p[k].y); m->bmax.y = std::max(m->bmax.y, p[k].y);
            m->bmin.z = std::min(m->bmin.z, p[k].z); m->bmax.z = std::max(m->bmax.z, p[k].z);
        }
        V3 n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
        m->area   += 0.5 * (double)length(n);
        m->volume += (double)dot(p[0], cross(p[1], p[2])) / 6.0;

        // Half the sum of the unsigned projected areas. For a closed mesh the
        // front faces and the back faces each cover the outline exactly once,
        // so the half is the outline itself; for an open one it is the honest
        // average of the two sides rather than a double count.
        double ax = (double)dot(p[0], w.u), ay = (double)dot(p[0], w.v);
        double bx = (double)dot(p[1], w.u), by = (double)dot(p[1], w.v);
        double cx = (double)dot(p[2], w.u), cy = (double)dot(p[2], w.v);
        m->silhouette += 0.25 * std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
    }
    m->volume = std::fabs(m->volume);
    return 1;
}

}  // namespace

int dai_show_sample_feasible(const dai_show_sample_desc *d, dai_show_feasibility *out) {
    dai_show_feasibility f;
    std::memset(&f, 0, sizeof(f));

    Measure m;
    if (!measure(d, &m) || d->min_distance_m <= 0.0f) {
        if (out) *out = f;
        return 0;
    }

    const double dmin  = (double)d->min_distance_m;
    const int    solid = (d->mode == DAI_SHOW_SAMPLE_VOLUME);
    const double extent = std::max(std::max((double)(m.bmax.x - m.bmin.x),
                                            (double)(m.bmax.y - m.bmin.y)),
                                   (double)(m.bmax.z - m.bmin.z));

    // The measure the mode actually packs into, and the volume of one cell of
    // the densest packing in that dimension.
    double have, cell;
    if (solid) {
        have = m.volume;
        cell = dmin * dmin * dmin * FCC_CELL;
    } else {
        have = (d->mode == DAI_SHOW_SAMPLE_SILHOUETTE) ? m.silhouette : m.area;
        cell = dmin * dmin * HEX_CELL;
    }

    double cap = (cell > 0.0) ? have / cell : 0.0;
    f.max_points = (cap >= 4294967295.0) ? 4294967295u
                                         : (uint32_t)(cap > 0.0 ? std::floor(cap) : 0.0);

    const double want = (double)d->count;
    if (want > 0.0 && have > 0.0) {
        // The spacing this many points could still hold, from the same formula
        // read the other way round. What the parameter panel offers when the
        // director would rather shrink the safety number than grow the figure -
        // and the reason it is shown is that they should see it is a trade.
        f.achievable_spacing_m = solid
            ? (float)std::cbrt(have / (want * FCC_CELL))
            : (float)std::sqrt(have / (want * HEX_CELL));
    }

    if (want <= (double)f.max_points) {
        f.ok            = 1;
        f.required_scale = 1.0f;
        f.required_size_m = (float)extent;
    } else {
        f.ok = 0;
        double grow;
        if (cap > 0.0) grow = solid ? std::cbrt(want / cap) : std::sqrt(want / cap);
        else           grow = 1e6;
        f.required_scale  = (float)grow;
        f.required_size_m = (float)(extent * grow);
    }

    if (out) *out = f;
    return f.ok;
}

// ---------------------------------------------------------------------------
// The candidate pool: where a drone is allowed to be, plus what colour it is
// ---------------------------------------------------------------------------

namespace {

struct Cand { V3 p; uint8_t r, g, b, w; };

// A triangle soup already in show metres, so nothing downstream has to know
// about scale or centring - a distance in this file is always a distance in
// the sky.
struct Soup {
    std::vector<V3>          p;
    const dai_show_sample_desc *d;
    const uint32_t          *idx;
    uint32_t                 tris;
};

void build_soup(const dai_show_sample_desc *d, const Measure &m, Soup *s) {
    const float scale = (d->scale > 0.0f) ? d->scale : 1.0f;
    V3 c = mul(add(m.bmin, m.bmax), 0.5f);
    V3 to = mk(d->centre.x - c.x, d->centre.y - c.y, d->centre.z - c.z);

    s->d = d;
    s->idx = d->indices;
    s->tris = d->index_count / 3u;
    s->p.resize(d->vertex_count);
    for (uint32_t i = 0; i < d->vertex_count; ++i) {
        s->p[i] = mk((float)((double)d->positions[i * 3u + 0] * scale + (double)to.x),
                     (float)((double)d->positions[i * 3u + 1] * scale + (double)to.y),
                     (float)((double)d->positions[i * 3u + 2] * scale + (double)to.z));
    }
}

// glTF's own order: vertex colours, then the texture, then the material's base
// colour. A figure that looked right in Blender looks right in the viewport,
// which is the only way a show director trusts the preview.
//
// The white channel stays 0: a mesh has an alpha, not a white LED, and mapping
// opacity onto the fourth emitter would wash out every figure that happens to
// carry an alpha channel.
void colour_at(const Soup &s, uint32_t tri, float b0, float b1, float b2, Cand *out) {
    const dai_show_sample_desc *d = s.d;
    uint32_t i0 = s.idx[tri * 3u + 0], i1 = s.idx[tri * 3u + 1], i2 = s.idx[tri * 3u + 2];

    if (d->vertex_rgba) {
        uint32_t c0 = d->vertex_rgba[i0], c1 = d->vertex_rgba[i1], c2 = d->vertex_rgba[i2];
        double r = (double)b0 * (double)(c0 & 0xFFu)
                 + (double)b1 * (double)(c1 & 0xFFu)         + (double)b2 * (double)(c2 & 0xFFu);
        double g = (double)b0 * (double)((c0 >> 8) & 0xFFu)
                 + (double)b1 * (double)((c1 >> 8) & 0xFFu)  + (double)b2 * (double)((c2 >> 8) & 0xFFu);
        double b = (double)b0 * (double)((c0 >> 16) & 0xFFu)
                 + (double)b1 * (double)((c1 >> 16) & 0xFFu) + (double)b2 * (double)((c2 >> 16) & 0xFFu);
        out->r = (uint8_t)(r + 0.5); out->g = (uint8_t)(g + 0.5); out->b = (uint8_t)(b + 0.5);
        out->w = 0;
        return;
    }
    if (d->tex_rgba && d->uvs && d->tex_w > 0 && d->tex_h > 0) {
        float u = (float)((double)b0 * d->uvs[i0 * 2u] + (double)b1 * d->uvs[i1 * 2u]
                        + (double)b2 * d->uvs[i2 * 2u]);
        float v = (float)((double)b0 * d->uvs[i0 * 2u + 1] + (double)b1 * d->uvs[i1 * 2u + 1]
                        + (double)b2 * d->uvs[i2 * 2u + 1]);
        u -= std::floor(u); v -= std::floor(v);                 /* repeat wrap */
        float fx = u * (float)d->tex_w - 0.5f, fy = v * (float)d->tex_h - 0.5f;
        int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        float tx = fx - (float)x0, ty = fy - (float)y0;
        double acc[3] = { 0.0, 0.0, 0.0 };
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                int xi = ((x0 + i) % (int)d->tex_w + (int)d->tex_w) % (int)d->tex_w;
                int yi = ((y0 + j) % (int)d->tex_h + (int)d->tex_h) % (int)d->tex_h;
                const uint8_t *t = d->tex_rgba + ((size_t)yi * d->tex_w + (size_t)xi) * 4u;
                double wgt = (double)(i ? tx : 1.0f - tx) * (double)(j ? ty : 1.0f - ty);
                acc[0] += wgt * (double)t[0]; acc[1] += wgt * (double)t[1]; acc[2] += wgt * (double)t[2];
            }
        }
        out->r = (uint8_t)(acc[0] + 0.5); out->g = (uint8_t)(acc[1] + 0.5);
        out->b = (uint8_t)(acc[2] + 0.5); out->w = 0;
        return;
    }
    out->r = (uint8_t)(d->base_rgba & 0xFFu);
    out->g = (uint8_t)((d->base_rgba >> 8) & 0xFFu);
    out->b = (uint8_t)((d->base_rgba >> 16) & 0xFFu);
    out->w = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Two grids, because neither the mesh nor the point set may ever be walked
// whole. At 10,000 drones a single O(n^2) pass is 50 million distance tests,
// and there are hundreds of passes in a relaxation.
// ---------------------------------------------------------------------------

namespace {

// Triangles bucketed by their footprint in one plane. Volume mode shoots a
// parity ray straight up and only ever looks at the triangles standing over the
// point; silhouette mode does the same along the audience's direction.
struct TriGrid2 {
    int   nx, ny;
    float ox, oy, inv;
    std::vector<uint32_t> start, items;

    void build(const std::vector<float> &bb, uint32_t tris, int res) {
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        for (uint32_t t = 0; t < tris; ++t) {
            mnx = std::min(mnx, bb[t * 4 + 0]); mny = std::min(mny, bb[t * 4 + 1]);
            mxx = std::max(mxx, bb[t * 4 + 2]); mxy = std::max(mxy, bb[t * 4 + 3]);
        }
        if (tris == 0) { mnx = mny = 0.0f; mxx = mxy = 1.0f; }
        float ext = std::max(mxx - mnx, mxy - mny);
        if (ext <= 1e-6f) ext = 1e-6f;
        if (res < 1) res = 1;
        float cell = ext / (float)res;
        ox = mnx; oy = mny; inv = 1.0f / cell;
        nx = (int)((mxx - mnx) * inv) + 1;
        ny = (int)((mxy - mny) * inv) + 1;

        // Counting sort: one pass to count, one to place. No per-cell vector,
        // so a 256x256 grid over a million triangles is two flat arrays.
        start.assign((size_t)nx * (size_t)ny + 1u, 0u);
        for (uint32_t t = 0; t < tris; ++t) {
            int x0, y0, x1, y1;
            range(bb[t * 4 + 0], bb[t * 4 + 1], bb[t * 4 + 2], bb[t * 4 + 3], &x0, &y0, &x1, &y1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) ++start[(size_t)y * nx + x + 1];
        }
        for (size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
        items.resize(start.back());
        std::vector<uint32_t> cur(start.begin(), start.end() - 1);
        for (uint32_t t = 0; t < tris; ++t) {
            int x0, y0, x1, y1;
            range(bb[t * 4 + 0], bb[t * 4 + 1], bb[t * 4 + 2], bb[t * 4 + 3], &x0, &y0, &x1, &y1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) items[cur[(size_t)y * nx + x]++] = t;
        }
    }

    void range(float ax, float ay, float bx, float by, int *x0, int *y0, int *x1, int *y1) const {
        *x0 = std::max(0, std::min(nx - 1, (int)((ax - ox) * inv)));
        *y0 = std::max(0, std::min(ny - 1, (int)((ay - oy) * inv)));
        *x1 = std::max(0, std::min(nx - 1, (int)((bx - ox) * inv)));
        *y1 = std::max(0, std::min(ny - 1, (int)((by - oy) * inv)));
    }

    // The triangles whose footprint covers (x,y), as a half open range into
    // `items`. Returns 0 when the query is outside the grid entirely.
    int at(float x, float y, uint32_t *first, uint32_t *last) const {
        int cx = (int)((x - ox) * inv), cy = (int)((y - oy) * inv);
        if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return 0;
        size_t c = (size_t)cy * nx + cx;
        *first = start[c]; *last = start[c + 1];
        return 1;
    }
};

// Point in a 2D triangle, WATERTIGHT - and the reason it is written this way
// rather than as three float comparisons is a measured bug, not taste.
//
// Volume mode decides "inside" by counting how often a ray upwards crosses the
// mesh. Near the equator of a sphere the triangles stand almost edge on to that
// ray, and a plain float test misses one of the two crossings there: the parity
// flips and points 16 metres outside the figure come back as inside. Drones
// would have been placed in mid air next to the model.
//
// So: every edge is evaluated in double, from the two endpoints put in a
// canonical order (lexicographic in the projected plane, which is bitwise the
// same for both triangles sharing that edge even when the mesh stores the
// corner three times), and a point exactly on an edge is given to whichever
// side the canonical order picked. Two triangles sharing an edge then disagree
// about it by construction, so a crossing is counted exactly once.
bool bary2(float ax, float ay, float bx, float by, float cx, float cy,
           float px, float py, float *b) {
    const double v[3][2] = { { (double)ax, (double)ay },
                             { (double)bx, (double)by },
                             { (double)cx, (double)cy } };
    const double q[2] = { (double)px, (double)py };

    double e[3];
    for (int i = 0; i < 3; ++i) {
        const double *p0 = v[i], *p1 = v[(i + 1) % 3];
        bool swap = (p1[0] < p0[0]) || (p1[0] == p0[0] && p1[1] < p0[1]);
        const double *lo = swap ? p1 : p0, *hi = swap ? p0 : p1;
        double f = (hi[0] - lo[0]) * (q[1] - lo[1]) - (hi[1] - lo[1]) * (q[0] - lo[0]);
        if (f == 0.0) f = 1.0;              /* on the edge: the canonical side */
        e[i] = swap ? -f : f;
    }
    bool inside = (e[0] > 0.0 && e[1] > 0.0 && e[2] > 0.0)
               || (e[0] < 0.0 && e[1] < 0.0 && e[2] < 0.0);
    if (!inside) return false;

    // Only reached for a hit, so the barycentrics are for the colour lookup and
    // the crossing height. A triangle seen exactly edge on has no usable ones;
    // its first corner is as good an answer as any and the parity above is
    // unaffected.
    double den = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1])
               - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]);
    if (den == 0.0) { b[0] = 1.0f; b[1] = b[2] = 0.0f; return true; }
    double b1 = ((q[0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[2][0] - v[0][0]) * (q[1] - v[0][1])) / den;
    double b2 = ((v[1][0] - v[0][0]) * (q[1] - v[0][1]) - (q[0] - v[0][0]) * (v[1][1] - v[0][1])) / den;
    b1 = std::min(std::max(b1, 0.0), 1.0);
    b2 = std::min(std::max(b2, 0.0), 1.0 - b1);
    b[1] = (float)b1; b[2] = (float)b2; b[0] = 1.0f - b[1] - b[2];
    return true;
}

// Points in a hash grid whose cell is the minimum distance, so "is anything
// closer than d" is 27 buckets rather than n tests. Entries are never removed:
// a moved point is inserted again and the query reads the CURRENT position out
// of the caller's array, so a stale entry costs a distance test and can never
// cost a wrong answer.
struct PointGrid {
    static constexpr uint64_t EMPTY = ~0ull;
    float cell;
    uint32_t mask;
    std::vector<uint64_t> keys;
    std::vector<int32_t>  head, next, item;

    void init(float cell_size, uint32_t expect) {
        cell = (cell_size > 1e-6f) ? cell_size : 1e-6f;
        uint32_t n = 16;
        while (n < expect * 2u + 16u) n <<= 1;
        mask = n - 1u;
        keys.assign(n, EMPTY);
        head.assign(n, -1);
        next.clear(); item.clear();
        next.reserve(expect); item.reserve(expect);
    }
    inline int32_t coord(float v) const { return (int32_t)std::floor(v / cell); }
    static uint64_t key_of(int32_t x, int32_t y, int32_t z) {
        return ((uint64_t)(uint32_t)(x + 1048576) & 0x1FFFFFull)
             | (((uint64_t)(uint32_t)(y + 1048576) & 0x1FFFFFull) << 21)
             | (((uint64_t)(uint32_t)(z + 1048576) & 0x1FFFFFull) << 42);
    }
    static uint64_t mix(uint64_t k) {
        k ^= k >> 33; k *= 0xFF51AFD7ED558CCDull;
        k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ull;
        return k ^ (k >> 33);
    }
    void insert(int32_t idx, V3 p) {
        uint64_t k = key_of(coord(p.x), coord(p.y), coord(p.z));
        uint32_t i = (uint32_t)mix(k) & mask;
        while (keys[i] != EMPTY && keys[i] != k) i = (i + 1u) & mask;
        if (keys[i] == EMPTY) { keys[i] = k; head[i] = -1; }
        item.push_back(idx);
        next.push_back(head[i]);
        head[i] = (int32_t)(item.size() - 1u);
    }
    int32_t bucket(int32_t x, int32_t y, int32_t z) const {
        uint64_t k = key_of(x, y, z);
        uint32_t i = (uint32_t)mix(k) & mask;
        while (keys[i] != EMPTY) {
            if (keys[i] == k) return head[i];
            i = (i + 1u) & mask;
        }
        return -1;
    }
    // Anything other than `self` within `r` of p? The whole Poisson property in
    // one call.
    bool crowded(const std::vector<V3> &pos, V3 p, float r, int32_t self) const {
        int32_t cx = coord(p.x), cy = coord(p.y), cz = coord(p.z);
        int reach = (int)std::ceil(r / cell);
        float r2 = r * r;
        for (int z = -reach; z <= reach; ++z)
            for (int y = -reach; y <= reach; ++y)
                for (int x = -reach; x <= reach; ++x) {
                    for (int32_t e = bucket(cx + x, cy + y, cz + z); e >= 0; e = next[(size_t)e]) {
                        int32_t j = item[(size_t)e];
                        if (j == self) continue;
                        V3 dv = sub(pos[(size_t)j], p);
                        if (dot(dv, dv) < r2) return true;
                    }
                }
        return false;
    }
    // Nearest stored point, searched shell by shell and stopped as soon as the
    // next shell cannot beat the best. Ties go to the lower index so two runs
    // pick the same one.
    int32_t nearest(const std::vector<V3> &pos, V3 p, int max_rings, float *out_d2) const {
        int32_t cx = coord(p.x), cy = coord(p.y), cz = coord(p.z);
        int32_t best = -1;
        float bd2 = 1e30f;
        for (int r = 0; r <= max_rings; ++r) {
            for (int z = -r; z <= r; ++z)
                for (int y = -r; y <= r; ++y)
                    for (int x = -r; x <= r; ++x) {
                        if (std::max(std::max(std::abs(x), std::abs(y)), std::abs(z)) != r) continue;
                        for (int32_t e = bucket(cx + x, cy + y, cz + z); e >= 0; e = next[(size_t)e]) {
                            int32_t j = item[(size_t)e];
                            V3 dv = sub(pos[(size_t)j], p);
                            float d2 = dot(dv, dv);
                            if (d2 < bd2 || (d2 == bd2 && j < best)) { bd2 = d2; best = j; }
                        }
                    }
            if (best >= 0 && bd2 <= (float)(r * r) * cell * cell) break;
        }
        if (out_d2) *out_d2 = bd2;
        return best;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// The three pools
// ---------------------------------------------------------------------------

namespace {

// One preparation per mode, done once, then any number of candidate draws. The
// split matters because the pool is refilled in rounds until the fleet fits,
// and rebuilding a triangle grid per round would turn a dense figure into a
// coffee break.
struct Pool {
    const Soup *s = nullptr;
    int mode = DAI_SHOW_SAMPLE_SURFACE;

    // SURFACE: cumulative triangle area, so a draw is one binary search and the
    // point density does not depend on how finely the artist tessellated.
    std::vector<float> cum_area;
    float area_total = 0.0f;

    // VOLUME: triangles bucketed by their XZ footprint for the parity ray.
    TriGrid2 column;
    V3 bmin = mk(0, 0, 0), bmax = mk(0, 0, 0);

    // SILHOUETTE: the outline as 2D segments, the triangles bucketed in the
    // audience's plane, and the width of the band the points are scattered in.
    View  view;
    TriGrid2 proj;
    std::vector<float> seg;        /* 4 floats per segment: a.u a.v b.u b.v   */
    std::vector<float> cum_len;
    float len_total = 0.0f;
    float band = 1.0f;

    void prepare(const Soup &soup, const Measure &m, float min_d, uint32_t count);
    // Draws up to `want` candidates, giving up after `attempts` rejections so a
    // hollow or degenerate figure fails with a message instead of spinning.
    // `widen` opens the silhouette band up on later rounds: the first round
    // asks for the crispest outline the fleet could possibly fit on, and only
    // a round that came up short pays for depth with sharpness.
    void draw(Rng &rng, uint32_t want, uint32_t attempts, float widen,
              std::vector<Cand> &out) const;

private:
    void draw_surface(Rng &, uint32_t, uint32_t, std::vector<Cand> &) const;
    void draw_volume(Rng &, uint32_t, uint32_t, std::vector<Cand> &) const;
    void draw_silhouette(Rng &, uint32_t, uint32_t, float, std::vector<Cand> &) const;
};

uint32_t pick_weighted(const std::vector<float> &cum, float total, float r) {
    float x = r * total;
    uint32_t lo = 0, hi = (uint32_t)cum.size() - 1u;
    while (lo < hi) {                      /* first entry with cum > x */
        uint32_t mid = lo + (hi - lo) / 2u;
        if (cum[mid] > x) hi = mid; else lo = mid + 1u;
    }
    return lo;
}

void Pool::prepare(const Soup &soup, const Measure &m, float min_d, uint32_t count) {
    s = &soup;
    bmin = m.bmin; bmax = m.bmax;
    // The measure ran before centring; the soup is centred, so shift the box.
    V3 c = mul(add(m.bmin, m.bmax), 0.5f);
    V3 to = sub(mk(soup.d->centre.x, soup.d->centre.y, soup.d->centre.z), c);
    bmin = add(bmin, to); bmax = add(bmax, to);

    if (mode == DAI_SHOW_SAMPLE_SURFACE) {
        cum_area.resize(soup.tris);
        double acc = 0.0;
        for (uint32_t t = 0; t < soup.tris; ++t) {
            V3 a = soup.p[soup.idx[t * 3 + 0]], b = soup.p[soup.idx[t * 3 + 1]], cc = soup.p[soup.idx[t * 3 + 2]];
            acc += 0.5 * (double)length(cross(sub(b, a), sub(cc, a)));
            cum_area[t] = (float)acc;
        }
        area_total = (float)acc;
        return;
    }

    if (mode == DAI_SHOW_SAMPLE_VOLUME) {
        std::vector<float> bb(soup.tris * 4u);
        for (uint32_t t = 0; t < soup.tris; ++t) {
            V3 a = soup.p[soup.idx[t * 3 + 0]], b = soup.p[soup.idx[t * 3 + 1]], cc = soup.p[soup.idx[t * 3 + 2]];
            bb[t * 4 + 0] = std::min(a.x, std::min(b.x, cc.x));
            bb[t * 4 + 1] = std::min(a.z, std::min(b.z, cc.z));
            bb[t * 4 + 2] = std::max(a.x, std::max(b.x, cc.x));
            bb[t * 4 + 3] = std::max(a.z, std::max(b.z, cc.z));
        }
        int res = (int)std::sqrt((double)soup.tris) + 1;
        column.build(bb, soup.tris, std::min(res, 256));
        return;
    }

    // ---- silhouette ------------------------------------------------------
    view = view_of(soup.d->view_dir);
    std::vector<float> pu(soup.p.size()), pv(soup.p.size());
    for (size_t i = 0; i < soup.p.size(); ++i) {
        pu[i] = dot(soup.p[i], view.u);
        pv[i] = dot(soup.p[i], view.v);
    }
    std::vector<float> bb(soup.tris * 4u);
    for (uint32_t t = 0; t < soup.tris; ++t) {
        uint32_t i0 = soup.idx[t * 3 + 0], i1 = soup.idx[t * 3 + 1], i2 = soup.idx[t * 3 + 2];
        bb[t * 4 + 0] = std::min(pu[i0], std::min(pu[i1], pu[i2]));
        bb[t * 4 + 1] = std::min(pv[i0], std::min(pv[i1], pv[i2]));
        bb[t * 4 + 2] = std::max(pu[i0], std::max(pu[i1], pu[i2]));
        bb[t * 4 + 3] = std::max(pv[i0], std::max(pv[i1], pv[i2]));
    }
    int res = (int)std::sqrt((double)soup.tris) + 1;
    proj.build(bb, soup.tris, std::min(res, 256));

    // Vertices are welded by position first, because a glTF mesh with split
    // normals has three copies of every corner and edge adjacency found by
    // index would then see a boundary everywhere and call the whole mesh
    // outline. Quantising to a tenth of a millimetre is far below anything a
    // drone can fly and far above float noise.
    struct Weld { int32_t x, y, z; uint32_t v; };
    std::vector<Weld> wk(soup.p.size());
    for (size_t i = 0; i < soup.p.size(); ++i) {
        Weld w;
        w.x = (int32_t)std::lround((double)soup.p[i].x * 10000.0);
        w.y = (int32_t)std::lround((double)soup.p[i].y * 10000.0);
        w.z = (int32_t)std::lround((double)soup.p[i].z * 10000.0);
        w.v = (uint32_t)i;
        wk[i] = w;
    }
    // Sorted on the quantised coordinate itself rather than on a hash of it.
    // A hash here was measured merging 136 of a sphere's 738 corners, which
    // turned a clean outline into a set of edges scattered over the whole
    // model - the kind of bug that produces a plausible looking figure and a
    // silhouette nobody can read.
    std::sort(wk.begin(), wk.end(), [](const Weld &a, const Weld &b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.v < b.v;
    });
    std::vector<uint32_t> weld(soup.p.size());
    for (size_t i = 0; i < wk.size();) {
        size_t j = i;
        while (j < wk.size() && wk[j].x == wk[i].x && wk[j].y == wk[i].y && wk[j].z == wk[i].z) ++j;
        for (size_t k = i; k < j; ++k) weld[wk[k].v] = wk[i].v;
        i = j;
    }

    // An edge is on the outline when the two triangles sharing it face
    // differently in the projection, or when nothing shares it at all - plus
    // the case that costs a whole figure when it is forgotten: an edge between
    // a facing triangle and one standing EDGE ON to the audience.
    //
    // A cube seen straight down an axis is the whole of that case. Its four
    // side faces project to lines, so their signed area is zero, so neither the
    // front nor the back face ever meets a triangle of the opposite sign and
    // the sign-change rule finds NO outline at all - measured: 0 of 400 points
    // placed, the mode simply refused the figure. A degenerate neighbour is not
    // an absent neighbour; it is a face turning away exactly at this edge, and
    // that is what a silhouette is.
    //
    // "Zero" is measured against the projected bounding box rather than against
    // 0.0f, because a figure rotated by a thousandth of a degree has side faces
    // whose area is float noise rather than exactly nothing, and the audience
    // cannot tell those two apart either.
    float ext_u_lo = 1e30f, ext_u_hi = -1e30f, ext_v_lo = 1e30f, ext_v_hi = -1e30f;
    for (size_t i = 0; i < pu.size(); ++i) {
        ext_u_lo = std::min(ext_u_lo, pu[i]); ext_u_hi = std::max(ext_u_hi, pu[i]);
        ext_v_lo = std::min(ext_v_lo, pv[i]); ext_v_hi = std::max(ext_v_hi, pv[i]);
    }
    const float flat_eps = (pu.empty() ? 0.0f
                          : 1e-6f * std::max(1e-12f, (ext_u_hi - ext_u_lo) * (ext_v_hi - ext_v_lo)));

    struct Edge { uint64_t key; uint32_t a, b; int sign; };
    std::vector<Edge> edges;
    edges.reserve(soup.tris * 3u);
    for (uint32_t t = 0; t < soup.tris; ++t) {
        uint32_t i0 = soup.idx[t * 3 + 0], i1 = soup.idx[t * 3 + 1], i2 = soup.idx[t * 3 + 2];
        float ar = (pu[i1] - pu[i0]) * (pv[i2] - pv[i0]) - (pu[i2] - pu[i0]) * (pv[i1] - pv[i0]);
        int sg = (std::fabs(ar) <= flat_eps) ? 0 : (ar > 0.0f ? 1 : -1);
        const uint32_t vi[3] = { i0, i1, i2 };
        for (int e = 0; e < 3; ++e) {
            uint32_t a = weld[vi[e]], b = weld[vi[(e + 1) % 3]];
            uint32_t lo = std::min(a, b), hi = std::max(a, b);
            Edge ed;
            ed.key = ((uint64_t)lo << 32) | (uint64_t)hi;
            ed.a = lo; ed.b = hi; ed.sign = sg;
            edges.push_back(ed);
        }
    }
    std::sort(edges.begin(), edges.end(),
              [](const Edge &x, const Edge &y) { return x.key < y.key; });

    // An edge that survives this is a piece of the outline as the audience
    // sees it. Duplicates are deliberately NOT removed: a symmetrical figure
    // hands the same projected stretch in twice - the torus's front and back
    // halves land on one curve on the retina - and both copies are drawn from,
    // which weights that stretch exactly as often as the geometry occupies it.
    double acc = 0.0;
    for (size_t i = 0; i < edges.size();) {
        size_t j = i;
        int npos = 0, nneg = 0, nflat = 0;
        while (j < edges.size() && edges[j].key == edges[i].key) {
            if (edges[j].sign > 0) ++npos; else if (edges[j].sign < 0) ++nneg; else ++nflat;
            ++j;
        }
        bool boundary = (j - i) == 1;
        bool turns    = (npos > 0 && nneg > 0);
        // Facing meets edge-on: the outline of the cube. Two edge-on triangles
        // meeting each other are NOT an outline - that is the vertical corner
        // of the cube, which projects to a point and would otherwise pull a
        // pile of drones onto four spots.
        bool grazes   = (nflat > 0 && (npos > 0 || nneg > 0));
        if (boundary || turns || grazes) {
            float au = pu[edges[i].a], av = pv[edges[i].a];
            float bu = pu[edges[i].b], bv = pv[edges[i].b];
            seg.push_back(au); seg.push_back(av); seg.push_back(bu); seg.push_back(bv);
            acc += std::sqrt((double)((bu - au) * (bu - au) + (bv - av) * (bv - av)));
            cum_len.push_back((float)acc);
        }
        i = j;
    }
    len_total = (float)acc;

    // How wide the band around the outline has to be for the fleet to fit in
    // it - a full width, half of it either side of the contour. A contour is one dimensional and a fleet is not: 3,000 drones on a
    // 200 m outline at 3 m spacing need roughly fifteen rows, and pretending
    // otherwise would mean dart throwing until the attempt budget ran out and
    // then reporting a figure that is in fact large enough as impossible.
    double need_area = (double)count * (double)min_d * (double)min_d * HEX_CELL / 0.70;
    double from_len  = (len_total > 1e-3f) ? need_area / (double)len_total : (double)min_d;
    band = (float)std::max((double)min_d * 1.5, from_len);
}

void Pool::draw(Rng &rng, uint32_t want, uint32_t attempts, float widen,
                std::vector<Cand> &out) const {
    if (mode == DAI_SHOW_SAMPLE_VOLUME)          draw_volume(rng, want, attempts, out);
    else if (mode == DAI_SHOW_SAMPLE_SILHOUETTE) draw_silhouette(rng, want, attempts, widen, out);
    else                                         draw_surface(rng, want, attempts, out);
}

void Pool::draw_surface(Rng &rng, uint32_t want, uint32_t attempts, std::vector<Cand> &out) const {
    (void)attempts;
    if (area_total <= 0.0f) return;
    for (uint32_t i = 0; i < want; ++i) {
        uint32_t t = pick_weighted(cum_area, area_total, rng.unit());
        // The square root is what keeps the distribution uniform over the
        // triangle rather than piled up in one corner.
        double r1 = std::sqrt((double)rng.unit()), r2 = (double)rng.unit();
        double b0 = 1.0 - r1, b1 = r1 * (1.0 - r2), b2 = r1 * r2;
        V3 a = s->p[s->idx[t * 3 + 0]], b = s->p[s->idx[t * 3 + 1]], c = s->p[s->idx[t * 3 + 2]];
        Cand cd;
        cd.p = comb3(a, b0, b, b1, c, b2);
        colour_at(*s, t, (float)b0, (float)b1, (float)b2, &cd);
        out.push_back(cd);
    }
}

void Pool::draw_volume(Rng &rng, uint32_t want, uint32_t attempts, std::vector<Cand> &out) const {
    uint32_t made = 0;
    for (uint32_t a = 0; a < attempts && made < want; ++a) {
        V3 p = mk(rng.range(bmin.x, bmax.x), rng.range(bmin.y, bmax.y), rng.range(bmin.z, bmax.z));

        // Parity along +Y, over the triangles standing above this column only.
        // The nearest crossing above also hands over the colour: a drone inside
        // a red balloon is red, which is what a director expects to see.
        uint32_t first, last;
        if (!column.at(p.x, p.z, &first, &last)) continue;
        int crossings = 0;
        float best_y = 1e30f;
        uint32_t best_t = 0;
        float best_b[3] = { 1.0f, 0.0f, 0.0f };
        for (uint32_t e = first; e < last; ++e) {
            uint32_t t = column.items[e];
            V3 va = s->p[s->idx[t * 3 + 0]], vb = s->p[s->idx[t * 3 + 1]], vc = s->p[s->idx[t * 3 + 2]];
            float bary[3];
            if (!bary2(va.x, va.z, vb.x, vb.z, vc.x, vc.z, p.x, p.z, bary)) continue;
            float y = (float)((double)bary[0] * va.y + (double)bary[1] * vb.y
                            + (double)bary[2] * vc.y);
            if (y <= p.y) continue;
            ++crossings;
            if (y < best_y) {
                best_y = y; best_t = t;
                best_b[0] = bary[0]; best_b[1] = bary[1]; best_b[2] = bary[2];
            }
        }
        if ((crossings & 1) == 0) continue;                  /* outside */

        Cand cd;
        cd.p = p;
        colour_at(*s, best_t, best_b[0], best_b[1], best_b[2], &cd);
        out.push_back(cd);
        ++made;
    }
}

void Pool::draw_silhouette(Rng &rng, uint32_t want, uint32_t attempts, float widen,
                           std::vector<Cand> &out) const {
    if (seg.empty() || len_total <= 0.0f) return;
    uint32_t made = 0;
    for (uint32_t a = 0; a < attempts && made < want; ++a) {
        uint32_t si = pick_weighted(cum_len, len_total, rng.unit());
        float au = seg[si * 4 + 0], av = seg[si * 4 + 1];
        float bu = seg[si * 4 + 2], bv = seg[si * 4 + 3];
        double t = (double)rng.unit();
        double qu = (double)au + ((double)bu - (double)au) * t;
        double qv = (double)av + ((double)bv - (double)av) * t;

        // Off the outline by up to `band`, on whichever side turns out to be
        // inside the figure - the sign is decided by the inside test below
        // rather than by a normal, which on a self-intersecting outline would
        // be a guess.
        double du = (double)bv - (double)av, dv = (double)au - (double)bu;
        double dl = std::sqrt(du * du + dv * dv);
        if (dl > 1e-9) { du /= dl; dv /= dl; }
        float half = band * widen * 0.5f;
        double off = (double)rng.range(-half, half);
        qu += du * off; qv += dv * off;

        uint32_t first, last;
        if (!proj.at((float)qu, (float)qv, &first, &last)) continue;
        float best_depth = 1e30f;
        uint32_t best_t = 0;
        float best_b[3] = { 1.0f, 0.0f, 0.0f };
        int hit = 0;
        for (uint32_t e = first; e < last; ++e) {
            uint32_t tri = proj.items[e];
            V3 va = s->p[s->idx[tri * 3 + 0]], vb = s->p[s->idx[tri * 3 + 1]], vc = s->p[s->idx[tri * 3 + 2]];
            float aU = dot(va, view.u), aV = dot(va, view.v);
            float bU = dot(vb, view.u), bV = dot(vb, view.v);
            float cU = dot(vc, view.u), cV = dot(vc, view.v);
            float bary[3];
            if (!bary2(aU, aV, bU, bV, cU, cV, (float)qu, (float)qv, bary)) continue;
            float depth = (float)((double)bary[0] * dot(va, view.dir)
                                + (double)bary[1] * dot(vb, view.dir)
                                + (double)bary[2] * dot(vc, view.dir));
            if (!hit || depth < best_depth) {
                best_depth = depth; best_t = tri; hit = 1;
                best_b[0] = bary[0]; best_b[1] = bary[1]; best_b[2] = bary[2];
            }
        }
        if (!hit) continue;                                  /* outside the outline */

        Cand cd;
        cd.p = comb3(view.u, qu, view.v, qv, view.dir, (double)best_depth);
        colour_at(*s, best_t, best_b[0], best_b[1], best_b[2], &cd);
        out.push_back(cd);
        ++made;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Selection, relaxation, proof
// ---------------------------------------------------------------------------

namespace {

void fail(char *err, size_t len, const char *fmt, ...) {
    if (!err || len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

// How much of the theoretical packing this sampler actually reaches. Random
// sequential adsorption saturates near 0.55 of the hexagonal optimum and the
// relaxation between rounds buys a little of that back; 0.45 is where the
// measured runs in tests/droneshow_cases_sample.cpp still succeed with room to
// spare. It is used for one thing only: turning "the darts ran out" into a
// figure size the director can act on. It never loosens the distance.
const double DART_EFFICIENCY = 0.45;

// One or more Lloyd passes over the points placed so far.
//
// The candidate pool doubles as the density function: every candidate is handed
// to its nearest drone, each drone moves to the centroid of what it owns, and
// the move is snapped back onto a candidate so the point stays exactly on the
// surface (or inside the volume, or on the outline) - no projection routine, no
// drift off the figure.
//
// It runs BETWEEN dart rounds as well as at the end, and that is not a
// refinement: dart throwing saturates at roughly half the theoretical packing
// because it leaves gaps too small for a new point but too large to be tight.
// Evening the placed points out reopens those gaps, so a figure that is
// geometrically big enough can actually be filled instead of being refused.
void lloyd(const std::vector<Cand> &cands, const std::vector<V3> &cand_pos,
           const PointGrid &cgrid, float min_d, float own_cell, uint32_t iters,
           std::vector<Cand> &acc, std::vector<V3> &acc_pos) {
    const uint32_t n = (uint32_t)acc.size();
    if (n == 0) return;

    std::vector<double>   sum(3u * (size_t)n);
    std::vector<uint32_t> cnt(n);

    for (uint32_t it = 0; it < iters; ++it) {
        PointGrid own;
        own.init(own_cell, n);
        for (uint32_t i = 0; i < n; ++i) own.insert((int32_t)i, acc_pos[i]);

        std::fill(sum.begin(), sum.end(), 0.0);
        std::fill(cnt.begin(), cnt.end(), 0u);
        for (size_t c = 0; c < cands.size(); ++c) {
            int32_t o = own.nearest(acc_pos, cands[c].p, 3, nullptr);
            if (o < 0) continue;
            sum[(size_t)o * 3 + 0] += (double)cands[c].p.x;
            sum[(size_t)o * 3 + 1] += (double)cands[c].p.y;
            sum[(size_t)o * 3 + 2] += (double)cands[c].p.z;
            ++cnt[(size_t)o];
        }

        // One fresh constraint grid per pass, then the moves are offered to it
        // in index order. Every intermediate state is legal, so the end state is
        // legal - there is no repair pass that could fail.
        PointGrid keep;
        keep.init(min_d, n * 2u);
        for (uint32_t i = 0; i < n; ++i) keep.insert((int32_t)i, acc_pos[i]);

        for (uint32_t i = 0; i < n; ++i) {
            if (cnt[i] == 0) continue;
            V3 centroid = mk((float)(sum[(size_t)i * 3 + 0] / (double)cnt[i]),
                             (float)(sum[(size_t)i * 3 + 1] / (double)cnt[i]),
                             (float)(sum[(size_t)i * 3 + 2] / (double)cnt[i]));
            int32_t c = cgrid.nearest(cand_pos, centroid, 3, nullptr);
            if (c < 0) continue;
            V3 np = cands[(size_t)c].p;
            if (keep.crowded(acc_pos, np, min_d, (int32_t)i)) continue;
            acc_pos[i] = np;
            acc[i]     = cands[(size_t)c];
            keep.insert((int32_t)i, np);
        }
    }
}

}  // namespace

uint32_t dai_show_sample(const dai_show_sample_desc *d, dai_show_point *out, uint32_t max,
                         char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';

    if (!d || !out) { fail(err, err_len, "sampling was given no descriptor or no output"); return 0; }
    if (d->count == 0) { fail(err, err_len, "a formation of zero drones is not a formation"); return 0; }
    if (d->count > max) {
        fail(err, err_len, "the output holds %u points, the formation needs %u", max, d->count);
        return 0;
    }
    if (d->min_distance_m <= 0.0f) {
        fail(err, err_len, "the minimum distance must be positive, not %.3f m", (double)d->min_distance_m);
        return 0;
    }

    Measure m;
    if (!measure(d, &m)) { fail(err, err_len, "the mesh has no usable triangles"); return 0; }

    // Feasibility first, always. The message names the size the figure has to
    // reach, because "impossible" without a number is something the director
    // cannot act on at ten in the evening.
    dai_show_feasibility f;
    dai_show_sample_feasible(d, &f);
    if (!f.ok) {
        const char *what = (d->mode == DAI_SHOW_SAMPLE_VOLUME) ? "volume"
                         : (d->mode == DAI_SHOW_SAMPLE_SILHOUETTE) ? "outline" : "surface";
        fail(err, err_len,
             "%u drones at %.2f m do not fit on this figure's %s: %u fit as it is, "
             "the figure has to be %.1f m across (scale it by %.2fx), or the spacing has to drop to %.2f m",
             d->count, (double)d->min_distance_m, what, f.max_points,
             (double)f.required_size_m, (double)f.required_scale, (double)f.achievable_spacing_m);
        return 0;
    }

    Soup soup;
    build_soup(d, m, &soup);

    Pool pool;
    pool.mode = d->mode;
    pool.prepare(soup, m, d->min_distance_m, d->count);

    Rng rng(d->seed);

    // ---- Poisson disk by dart throwing against the grid ------------------
    //
    // The candidates arrive in the order the generator drew them, which is
    // already random, so accepting greedily in that order IS dart throwing -
    // with the difference that a rejected dart stays around as material for the
    // relaxation instead of being thrown away.
    const uint32_t ROUNDS     = 6u;
    const uint32_t round_size = std::max(d->count * 8u, 2048u);

    std::vector<Cand> cands;
    std::vector<V3>   cand_pos;
    std::vector<Cand> acc;
    std::vector<V3>   acc_pos;
    acc.reserve(d->count); acc_pos.reserve(d->count);
    cands.reserve(round_size);

    PointGrid grid;
    grid.init(d->min_distance_m, d->count);

    const float own_cell = std::max(d->min_distance_m, f.achievable_spacing_m);

    for (uint32_t r = 0; r < ROUNDS && acc.size() < d->count; ++r) {
        size_t base = cands.size();
        // The silhouette band opens up only for a round that was needed: the
        // crispest outline the fleet fits on is the one the audience gets, so
        // round 0 always asks for the band as computed and nothing wider.
        //
        // After that it opens by half again per round rather than by a quarter
        // added. A quarter at a time reaches 2.25x after six rounds, which is a
        // linear answer to an area problem: a band that is 2.25 times as deep
        // holds 2.25 times the drones, and a figure short by a factor of two -
        // a 60 m cube asked for 400 drones at 2 m - would run out of rounds
        // while still short and be refused as impossible although it is not.
        // Measured before and after on exactly that figure: 212 of 400, then
        // 400 of 400.
        float widen = 1.0f;
        for (uint32_t k = 0; k < r; ++k) widen *= 1.5f;
        pool.draw(rng, round_size, round_size * 32u, widen, cands);
        if (cands.size() == base) break;                    /* the figure gives nothing */
        // Round 0 walks the new material only; later rounds rescan everything,
        // because a candidate that had no room before the relaxation may have
        // room after it.
        size_t from = (r == 0) ? base : 0u;
        for (size_t i = from; i < cands.size() && acc.size() < d->count; ++i) {
            if (grid.crowded(acc_pos, cands[i].p, d->min_distance_m, -1)) continue;
            grid.insert((int32_t)acc.size(), cands[i].p);
            acc_pos.push_back(cands[i].p);
            acc.push_back(cands[i]);
        }
        if (acc.size() >= d->count) break;

        // Short. Even out what is there, which opens the gaps the next round of
        // darts needs, and only then ask for more material.
        cand_pos.resize(cands.size());
        for (size_t i = 0; i < cands.size(); ++i) cand_pos[i] = cands[i].p;
        PointGrid cg;
        cg.init(d->min_distance_m, (uint32_t)cands.size());
        for (size_t i = 0; i < cands.size(); ++i) cg.insert((int32_t)i, cands[i].p);
        lloyd(cands, cand_pos, cg, d->min_distance_m, own_cell, 2u, acc, acc_pos);

        grid.init(d->min_distance_m, d->count);
        for (size_t i = 0; i < acc_pos.size(); ++i) grid.insert((int32_t)i, acc_pos[i]);
    }

    if (acc.size() < d->count) {
        // Geometrically possible, practically not: the request sits so close to
        // the packing bound that random placement cannot reach it. Say so with
        // the size that would work, rather than shipping fewer drones than the
        // storyboard asked for.
        double grow = (d->mode == DAI_SHOW_SAMPLE_VOLUME)
            ? std::cbrt((double)d->count / (DART_EFFICIENCY * (double)f.max_points))
            : std::sqrt((double)d->count / (DART_EFFICIENCY * (double)f.max_points));
        if (grow < 1.0) grow = 1.0;
        fail(err, err_len,
             "only %u of %u points could be placed at %.2f m - the request is within %u%% of the "
             "theoretical packing limit; make the figure about %.1f m across (scale it by %.2fx)",
             (uint32_t)acc.size(), d->count, (double)d->min_distance_m,
             (uint32_t)(100.0 * (double)d->count / (double)(f.max_points ? f.max_points : 1u)),
             (double)f.required_size_m * grow, grow);
        return 0;
    }

    // ---- Lloyd relaxation --------------------------------------------------
    if (d->relax_iterations > 0 && acc.size() == d->count) {
        cand_pos.resize(cands.size());
        for (size_t i = 0; i < cands.size(); ++i) cand_pos[i] = cands[i].p;
        PointGrid cgrid;
        cgrid.init(d->min_distance_m, (uint32_t)cands.size());
        for (size_t i = 0; i < cands.size(); ++i) cgrid.insert((int32_t)i, cands[i].p);
        lloyd(cands, cand_pos, cgrid, d->min_distance_m, own_cell, d->relax_iterations,
              acc, acc_pos);
    }

    // ---- the proof, measured rather than assumed --------------------------
    {
        PointGrid check;
        check.init(d->min_distance_m, d->count * 2u);
        for (uint32_t i = 0; i < d->count; ++i) check.insert((int32_t)i, acc_pos[i]);
        // A hair under the limit, so a pair that landed exactly on it - which
        // the constraint allows - is not reported as breaking it.
        float floor_m = d->min_distance_m * (1.0f - 1e-6f);
        for (uint32_t i = 0; i < d->count; ++i) {
            if (check.crowded(acc_pos, acc_pos[i], floor_m, (int32_t)i)) {
                fail(err, err_len, "internal: drone %u ended up closer than %.3f m to a neighbour",
                     i, (double)d->min_distance_m);
                return 0;
            }
        }
    }

    for (uint32_t i = 0; i < d->count; ++i) {
        out[i].x = acc_pos[i].x; out[i].y = acc_pos[i].y; out[i].z = acc_pos[i].z;
        out[i].r = acc[i].r; out[i].g = acc[i].g; out[i].b = acc[i].b; out[i].w = acc[i].w;
    }
    return d->count;
}
