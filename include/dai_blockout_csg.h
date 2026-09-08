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
        int type = (t < -EPS) ? BACK : (t > EPS) ? FRONT : COPLANAR;
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
                V3 v = snap(lerp(vi, vj, t));
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
        if (!n.pl.ok) n.pl = plane_of(in[0]);
        Plane pl = n.pl;
        std::vector<Poly> f, b;
        for (const Poly &p : in) {
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
    return s;
}

} /* namespace daiblock */

#endif /* DAI_BLOCKOUT_CSG_H */
