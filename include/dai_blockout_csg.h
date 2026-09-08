/*
 * Blockout - the boolean: union, subtract and intersect on two closed solids.
 *
 * Split out of dai_blockout.h so that the shapes, the welding and the
 * measurements (that file) and the BSP boolean (this file) can be worked on
 * side by side without two people in one header. It is NOT included on its
 * own: dai_blockout.h includes it at its end, after everything it needs -
 * V3, Poly, Solid, EPS, SNAP, snap(), make_poly(), flip() - is defined, so
 * every caller keeps writing `#include "dai_blockout.h"` and gets both.
 *
 * The promises are the ones at the top of dai_blockout.h, restated for the
 * boolean because it is where they are hardest to keep:
 *
 *   DETERMINISTIC   same two solids in, same polygons out, in the same order.
 *   CLOSED          both inputs closed -> the result closed: every edge on
 *                   exactly two faces, every face wound outwards.
 *   FINITE          a BSP built from snapped points must never recurse on a
 *                   polygon that sits on its own plane. The classification
 *                   tolerance therefore has to be COARSER than the snap
 *                   grid, or a point moved 5e-7 by snapping is "in front" of
 *                   its own plane for ever.
 *
 * Checked in tests/blockout_csg_cases.hpp (volumes, edge counts, degenerate
 * triangles, the arch and the cylinder as cutters) and read back from glTF in
 * tests/blockout_gltf_cases.hpp.
 */
#ifndef DAI_BLOCKOUT_CSG_H
#define DAI_BLOCKOUT_CSG_H

#ifndef DAI_BLOCKOUT_H
#error "dai_blockout_csg.h is included by dai_blockout.h - include that one"
#endif

namespace daiblock {
/* ---- the boolean ------------------------------------------------------ */

/* The tolerance of the plane test, and why it is not EPS.
 *
 * EPS (1e-9, dai_blockout.h) is the right answer to "are these two doubles
 * the same number". The boolean asks a different question: "is this SNAPPED
 * point on that plane". Every corner the boolean is handed is on the SNAP
 * grid (1e-6) - build() and transform() round, and so does seal() below
 * when a result becomes the input of the next boolean - and rounding moves
 * a point by up to SNAP/2 per axis, that is sqrt(3)/2 * SNAP ~ 0.87e-6 off
 * the plane it was computed on. Two points of one polygon can therefore
 * disagree about their common plane by sqrt(3) * SNAP ~ 1.7e-6, which is
 * more than a thousand EPS. Measured against EPS, a split piece that keeps
 * its parent's normal but carries snapped corners is "in front of" its own
 * plane, the child node picks that plane again, and Bsp::build recurses
 * until the stack is gone (commit e6902bf: the wall minus the arch, ~21000
 * frames deep).
 *
 * So the plane test is coarser than the grid: 4 * SNAP, twice the worst
 * case of one split, so a piece that is split a second time still lands
 * where it belongs. Anything closer than 4 micrometres to a plane IS on that
 * plane for the boolean - which is still three orders of magnitude below
 * the 1e-4 the volumes are checked to. Derived from SNAP, not typed, so the
 * two can never drift apart.
 *
 * The same number is the boolean's weld distance (seal(), below): two
 * corners closer than PLANE_EPS are one corner, a corner closer than
 * PLANE_EPS to an edge sits on that edge. One tolerance for "on the plane",
 * "the same point" and "on the edge", so the boolean cannot contradict
 * itself between the three. */
const double PLANE_EPS = 4.0 * SNAP;

namespace detail {

struct Plane {
    V3     n;
    double w = 0;
    bool   ok = false;
};

inline Plane plane_of(const Poly &p) {
    Plane pl;
    pl.n = p.normal;
    if (p.pts.empty()) return pl;
    pl.w = dot(pl.n, p.pts[0]);
    pl.ok = length(pl.n) > 0.5;
    return pl;
}

/* Splits `poly` by `pl` into the four buckets the BSP needs. Straight out of
 * the standard constructive-solid-geometry recipe, kept in one place so the
 * classification rule exists exactly once. */
inline void split_poly(const Plane &pl, const Poly &poly,
                       std::vector<Poly> &co_front, std::vector<Poly> &co_back,
                       std::vector<Poly> &front, std::vector<Poly> &back) {
    enum { COPLANAR = 0, FRONT = 1, BACK = 2, SPANNING = 3 };
    int poly_type = 0;
    std::vector<int> types;
    types.reserve(poly.pts.size());
    for (const V3 &v : poly.pts) {
        double t = dot(pl.n, v) - pl.w;
        int type = (t < -PLANE_EPS) ? BACK : (t > PLANE_EPS) ? FRONT : COPLANAR;
        poly_type |= type;
        types.push_back(type);
    }
    switch (poly_type) {
    case COPLANAR:
        (dot(pl.n, poly.normal) > 0 ? co_front : co_back).push_back(poly);
        break;
    case FRONT: front.push_back(poly); break;
    case BACK:  back.push_back(poly); break;
    default: {
        std::vector<V3> f, b;
        size_t c = poly.pts.size();
        for (size_t i = 0; i < c; ++i) {
            size_t j = (i + 1) % c;
            int ti = types[i], tj = types[j];
            const V3 &vi = poly.pts[i], &vj = poly.pts[j];
            if (ti != BACK)  f.push_back(vi);
            if (ti != FRONT) b.push_back(vi);
            if ((ti | tj) == SPANNING) {
                double t = (pl.w - dot(pl.n, vi)) / dot(pl.n, sub(vj, vi));
                /* NOT snapped here. A split point that is rounded onto the
                 * grid is the end of the next edge to be split, and the line
                 * through two rounded points is not the line the planes
                 * meet in: after two or three cuts the same corner, reached
                 * through two different edges, came out a whole grid cell
                 * apart (1.3, 0, 0 against 1.3, 0, -1e-6 on the bore test)
                 * and never welded. Kept exact, the two routes agree to
                 * 1e-15; seal() rounds once, at the end, when nothing is
                 * cut any more. */
                V3 v = lerp(vi, vj, t);
                f.push_back(v);
                b.push_back(v);
            }
        }
        if (f.size() >= 3) { Poly p = make_poly(f); p.normal = poly.normal; front.push_back(p); }
        if (b.size() >= 3) { Poly p = make_poly(b); p.normal = poly.normal; back.push_back(p); }
        break;
    }
    }
}

/* A BSP node. Built from a polygon list, then used to clip another list. */
struct BspNode {
    Plane             pl;
    std::vector<Poly> polys;
    int               front = -1;   /* indices into the arena, not pointers:
                                       a vector of nodes moves, and a pointer
                                       into it is a crash waiting for a resize */
    int               back = -1;
};

struct Bsp {
    std::vector<BspNode> nodes;

    int make() { nodes.push_back(BspNode()); return (int)nodes.size() - 1; }

    void build(int at, const std::vector<Poly> &in) {
        if (in.empty()) return;
        BspNode &n = nodes[(size_t)at];
        size_t first = 0;
        if (!n.pl.ok) {
            /* The polygon this node takes its plane from goes INTO this node,
             * unclassified. That is the guarantee that ends the recursion:
             * every call consumes at least one polygon, so the tree is never
             * deeper than the list is long - no matter how far a snapped
             * corner sits from the plane its own normal describes. Without
             * it a polygon judged "in front" of itself starts a child with
             * the same plane, which judges the same, for ever. */
            n.pl = plane_of(in[0]);
            n.polys.push_back(in[0]);
            first = 1;
        }
        Plane pl = n.pl;
        std::vector<Poly> f, b;
        for (size_t i = first; i < in.size(); ++i) {
            const Poly &p = in[i];
            std::vector<Poly> cf, cb;
            split_poly(pl, p, cf, cb, f, b);
            BspNode &me = nodes[(size_t)at];
            for (const Poly &q : cf) me.polys.push_back(q);
            for (const Poly &q : cb) me.polys.push_back(q);
        }
        if (!f.empty()) {
            if (nodes[(size_t)at].front < 0) nodes[(size_t)at].front = make();
            build(nodes[(size_t)at].front, f);
        }
        if (!b.empty()) {
            if (nodes[(size_t)at].back < 0) nodes[(size_t)at].back = make();
            build(nodes[(size_t)at].back, b);
        }
    }

    std::vector<Poly> clip(int at, const std::vector<Poly> &in) const {
        const BspNode &n = nodes[(size_t)at];
        if (!n.pl.ok) return in;
        std::vector<Poly> f, b;
        for (const Poly &p : in) split_poly(n.pl, p, f, b, f, b);
        if (n.front >= 0) f = clip(n.front, f);
        if (n.back >= 0)  b = clip(n.back, b);
        else              b.clear();          /* inside the other solid */
        f.insert(f.end(), b.begin(), b.end());
        return f;
    }

    void clip_to(int at, const Bsp &other, int other_root) {
        BspNode &n = nodes[(size_t)at];
        n.polys = other.clip(other_root, n.polys);
        int f = n.front, b = n.back;
        if (f >= 0) clip_to(f, other, other_root);
        if (b >= 0) clip_to(b, other, other_root);
    }

    void invert(int at) {
        BspNode &n = nodes[(size_t)at];
        for (Poly &p : n.polys) {
            std::vector<V3> r(p.pts.rbegin(), p.pts.rend());
            p.pts = r;
            p.normal = mul(p.normal, -1.0);
        }
        n.pl.n = mul(n.pl.n, -1.0);
        n.pl.w = -n.pl.w;
        int f = n.front, b = n.back;
        n.front = b;
        n.back = f;
        if (n.front >= 0) invert(n.front);
        if (n.back >= 0)  invert(n.back);
    }

    void all(int at, std::vector<Poly> &out) const {
        const BspNode &n = nodes[(size_t)at];
        out.insert(out.end(), n.polys.begin(), n.polys.end());
        if (n.front >= 0) all(n.front, out);
        if (n.back >= 0)  all(n.back, out);
    }
};

/* Makes the boolean's polygon soup one closed surface, then puts it on the
 * grid. Three steps, in this order, all deterministic:
 *
 *   1. weld   Every corner closer than PLANE_EPS to an earlier corner
 *             becomes that corner. The corners are visited in grid order
 *             (a std::map over their keys), so which of two near twins
 *             survives never depends on the order the tree was walked in.
 *   2. seam   A corner that sits on the inside of another polygon's edge -
 *             which the BSP makes wherever one side was split by a plane the
 *             other side never met - is inserted into that edge. Done here on
 *             the exact points rather than left to finalise(): its repair
 *             looks half a grid cell around a ROUNDED edge, and a rounded
 *             point on a sloped edge is up to 0.87 cells off, so the arch
 *             came out with open edges even though every point was right.
 *   3. snap   Only now is everything rounded. The same corner is the same
 *             double in every polygon that has it, so finalise() welds by
 *             key and finds nothing left to repair.
 *
 * The polygons keep their normals: a corner moved by a few micrometres does
 * not change which way a face looks. */
inline void seal(std::vector<Poly> &polys) {
    /* 1. weld */
    std::map<Key, V3> seen;
    for (const Poly &p : polys)
        for (const V3 &v : p.pts)
            if (seen.find(key_of(v)) == seen.end()) seen[key_of(v)] = v;

    /* Representatives, found through a coarse grid one PLANE_EPS wide: a
     * corner's twin is in its own or a neighbouring coarse cell. */
    struct Coarse {
        long long x, y, z;
        bool operator<(const Coarse &o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    auto coarse_of = [](V3 v) {
        Coarse c;
        c.x = (long long)std::floor(v.x / PLANE_EPS);
        c.y = (long long)std::floor(v.y / PLANE_EPS);
        c.z = (long long)std::floor(v.z / PLANE_EPS);
        return c;
    };
    std::map<Coarse, std::vector<V3> > cells;
    std::map<Key, V3> rep;
    std::vector<V3> reps;
    reps.reserve(seen.size());
    for (const auto &kv : seen) {
        const V3 &v = kv.second;
        Coarse c = coarse_of(v);
        V3 best = v;
        double best_d = PLANE_EPS;
        bool found = false;
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    Coarse q; q.x = c.x + dx; q.y = c.y + dy; q.z = c.z + dz;
                    auto it = cells.find(q);
                    if (it == cells.end()) continue;
                    for (const V3 &r : it->second) {
                        double d = length(sub(r, v));
                        if (d < best_d) { best_d = d; best = r; found = true; }
                    }
                }
        if (!found) {
            cells[c].push_back(v);
            reps.push_back(v);
        }
        rep[kv.first] = best;
    }
    for (Poly &p : polys)
        for (V3 &v : p.pts) v = rep[key_of(v)];

    /* 2. seam. Candidates sorted by x so an edge only looks at the corners
     * inside its own x range - the rest cannot be within PLANE_EPS of it. */
    std::sort(reps.begin(), reps.end(), [](const V3 &a, const V3 &b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    for (Poly &p : polys) {
        std::vector<V3> out;
        size_t c = p.pts.size();
        for (size_t i = 0; i < c; ++i) {
            const V3 &a = p.pts[i], &b = p.pts[(i + 1) % c];
            out.push_back(a);
            V3 ab = sub(b, a);
            double len2 = dot(ab, ab);
            if (len2 <= 0) continue;
            double len = std::sqrt(len2);
            double x0 = (a.x < b.x ? a.x : b.x) - PLANE_EPS;
            double x1 = (a.x < b.x ? b.x : a.x) + PLANE_EPS;
            auto lo = std::lower_bound(reps.begin(), reps.end(), x0,
                                       [](const V3 &r, double x) { return r.x < x; });
            std::vector<std::pair<double, V3> > hits;
            for (auto it = lo; it != reps.end() && it->x <= x1; ++it) {
                const V3 &v = *it;
                double t = dot(sub(v, a), ab) / len2;
                /* strictly inside the edge: a corner on an end IS that end */
                if (t * len <= PLANE_EPS || (1.0 - t) * len <= PLANE_EPS) continue;
                V3 on = add(a, mul(ab, t));
                if (length(sub(on, v)) > PLANE_EPS) continue;
                hits.push_back(std::make_pair(t, v));
            }
            if (hits.empty()) continue;
            std::sort(hits.begin(), hits.end(),
                      [](const std::pair<double, V3> &x, const std::pair<double, V3> &y) {
                          return x.first < y.first;
                      });
            for (const auto &h : hits) out.push_back(h.second);
        }
        p.pts = out;
    }

    /* 3. snap */
    for (Poly &p : polys)
        for (V3 &v : p.pts) v = snap(v);
}

} /* namespace detail */

/* union / subtract / intersect, in the node's own space. Both inputs must be
 * closed solids; the result is one too. */
inline Solid csg(const Solid &a_in, const Solid &b_in, int op) {
    if (op == OP_NONE || b_in.polys.empty()) return a_in;
    if (a_in.polys.empty()) return (op == OP_UNION) ? b_in : Solid();

    detail::Bsp a, b;
    int ar = a.make(), br = b.make();
    a.build(ar, a_in.polys);
    b.build(br, b_in.polys);

    switch (op) {
    case OP_UNION:
        a.clip_to(ar, b, br);
        b.clip_to(br, a, ar);
        b.invert(br);
        b.clip_to(br, a, ar);
        b.invert(br);
        break;
    case OP_SUBTRACT:
        a.invert(ar);
        a.clip_to(ar, b, br);
        b.clip_to(br, a, ar);
        b.invert(br);
        b.clip_to(br, a, ar);
        b.invert(br);
        break;
    case OP_INTERSECT:
        a.invert(ar);
        b.clip_to(br, a, ar);
        b.invert(br);
        a.clip_to(ar, b, br);
        b.clip_to(br, a, ar);
        break;
    default:
        return a_in;
    }

    /* What is left of A plus what is left of B is the surface of the result.
     * The recipe's last step builds B's polygons into A's tree, which splits
     * them again; splitting changes no geometry, so the two lists are simply
     * concatenated here - fewer polygons, fewer T junctions to repair, and
     * the same surface. */
    Solid s;
    a.all(ar, s.polys);
    b.all(br, s.polys);

    /* Subtract and intersect leave A inverted: turn the whole thing back the
     * right way out, or every normal points into the wall and the room
     * renders as a black box lit from inside. */
    if (op == OP_SUBTRACT || op == OP_INTERSECT) flip(s);

    /* One surface, on the grid - see seal(). */
    detail::seal(s.polys);
    return s;
}

} /* namespace daiblock */

#endif /* DAI_BLOCKOUT_CSG_H */
