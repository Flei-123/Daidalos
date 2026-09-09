/*
 * The three modifiers that make MORE surface out of the surface they are
 * given: solidify (a plane becomes a wall), array (one step becomes a stair)
 * and mirror (half a thing becomes the thing).
 *
 * Included at the end of include/dai_modifier.h - never included on its own,
 * because everything it speaks (Solid, Topology, the parameter structs) is
 * declared there. Read that file first; the three promises at the top of it -
 * deterministic, closed, measurable - are promises this file has to keep.
 *
 * MODULE B2 OWNS THIS FILE. Nothing else in the tree may define these three
 * functions, and this file defines nothing else: a helper that only bevel
 * needs belongs in include/dai_modifier_edge.h, and a helper both need
 * belongs in include/dai_modifier.h.
 */
#ifndef DAI_MODIFIER_DUP_H
#define DAI_MODIFIER_DUP_H

#ifndef DAI_MODIFIER_H
#error "include dai_modifier.h, not dai_modifier_dup.h"
#endif

namespace daimod {
namespace dup {

/* Averaged, area weighted vertex normals of a solid, by welded position. The
 * direction solidify pushes a corner: pushing each FACE along its own normal
 * would tear the surface apart at every edge. */
inline std::map<Key, V3> vertex_normals(const Solid &s) {
    std::map<Key, V3> acc;
    for (size_t f = 0; f < s.polys.size(); ++f) {
        const Poly &p = s.polys[f];
        double a = 0;
        for (size_t i = 1; i + 1 < p.pts.size(); ++i)
            a += length(cross(sub(p.pts[i], p.pts[0]), sub(p.pts[i + 1], p.pts[0]))) * 0.5;
        V3 w = mul(p.normal, a);
        for (size_t i = 0; i < p.pts.size(); ++i) {
            Key k = daiblock::detail::key_of(p.pts[i]);
            std::map<Key, V3>::iterator it = acc.find(k);
            if (it == acc.end()) acc[k] = w;
            else it->second = add(it->second, w);
        }
    }
    for (std::map<Key, V3>::iterator it = acc.begin(); it != acc.end(); ++it)
        it->second = length(it->second) > 1e-12 ? normalise(it->second) : v3(0, 1, 0);
    return acc;
}

inline V3 rotate_axis(V3 p, int axis, double deg) {
    if (deg == 0) return p;
    double a = deg * 3.14159265358979323846 / 180.0;
    double c = std::cos(a), s = std::sin(a);
    if (axis == 0) return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
    if (axis == 2) return v3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
    return v3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

inline double axis_of(V3 p, int a) { return a == 0 ? p.x : (a == 1 ? p.y : p.z); }
inline V3 set_axis(V3 p, int a, double v) {
    if (a == 0) p.x = v; else if (a == 1) p.y = v; else p.z = v;
    return p;
}

} /* namespace dup */

/* ---- solidify ----------------------------------------------------------
 *
 * The surface, a copy of it moved along the averaged vertex normals, and a rim
 * quad on every BORDER edge - the edges only one face owns. Feed it an open
 * plane and the result is a closed slab; feed it a closed solid and the result
 * is a shell with an inner surface, which is what a wall with a thickness is.
 *
 * `shift` says where the original surface ends up: -1 the outer skin (all the
 * material inward), +1 the inner skin, 0 the middle. */
inline Solid solidify(const Solid &in, const Solidify &p) {
    if (in.polys.empty() || p.thickness <= 0) return in;
    std::map<Key, V3> vn = dup::vertex_normals(in);
    double outer = p.thickness * (1.0 - p.shift) * 0.5;   /* shift -1 -> all outward */
    double inner = p.thickness * (1.0 + p.shift) * 0.5;

    auto moved = [&](const V3 &v, double d) -> V3 {
        std::map<Key, V3>::const_iterator it = vn.find(daiblock::detail::key_of(v));
        if (it == vn.end()) return v;
        return add(v, mul(it->second, d));
    };

    Solid out;
    out.polys.reserve(in.polys.size() * 2 + 8);
    /* The outward skin, walked the way it already is. */
    for (size_t f = 0; f < in.polys.size(); ++f) {
        std::vector<V3> pts;
        pts.reserve(in.polys[f].pts.size());
        for (size_t i = 0; i < in.polys[f].pts.size(); ++i)
            pts.push_back(moved(in.polys[f].pts[i], outer));
        out.polys.push_back(daiblock::make_poly(pts));
    }
    /* The inward skin, walked backwards so it faces the other way. */
    for (size_t f = 0; f < in.polys.size(); ++f) {
        std::vector<V3> pts;
        pts.reserve(in.polys[f].pts.size());
        for (size_t i = in.polys[f].pts.size(); i > 0; --i)
            pts.push_back(moved(in.polys[f].pts[i - 1], -inner));
        out.polys.push_back(daiblock::make_poly(pts));
    }
    /* The rim: one quad per border edge, walked so it agrees with the outer
     * skin's direction along that edge. Without it the two skins are two
     * surfaces and every border edge is open. */
    Topology t = topology_of(in);
    for (size_t e = 0; e < t.edges.size(); ++e) {
        const Edge &ed = t.edges[e];
        if (ed.count != 1 || ed.face[0] < 0) continue;
        const Poly &p0 = in.polys[(size_t)ed.face[0]];
        size_t c = p0.pts.size();
        size_t i = (size_t)ed.corner[0];
        V3 a = p0.pts[i], b = p0.pts[(i + 1) % c];
        V3 ao = moved(a, outer), bo = moved(b, outer);
        V3 ai = moved(a, -inner), bi = moved(b, -inner);
        out.polys.push_back(daiblock::make_poly({ ao, ai, bi, bo }));
    }
    settle(out);
    return out;
}

/* ---- array -------------------------------------------------------------
 *
 * `count` copies in total, copy i moved by i * offset and turned by
 * i * rotation degrees about `axis`. A relative offset is a multiple of the
 * shape's own bounding size per axis, so "one step forward" stays one step
 * forward when the step gets deeper.
 *
 * Nothing is welded between copies: the array of a closed shape is `count`
 * closed shapes, which is why the triangle count comes out as exactly count
 * times the original's and the bounding box grows by (count - 1) * offset. */
inline Solid array(const Solid &in, const Array &p) {
    int n = p.count;
    if (n < 1) n = 1;
    if (in.polys.empty() || n == 1) return in;

    V3 lo, hi;
    bounds(in, &lo, &hi);
    V3 step = v3(p.offset[0], p.offset[1], p.offset[2]);
    if (p.relative)
        step = v3(step.x * (hi.x - lo.x), step.y * (hi.y - lo.y), step.z * (hi.z - lo.z));

    Solid out;
    out.polys.reserve(in.polys.size() * (size_t)n);
    for (int c = 0; c < n; ++c) {
        V3 shift = mul(step, (double)c);
        double turn = p.rotation * (double)c;
        for (size_t f = 0; f < in.polys.size(); ++f) {
            std::vector<V3> pts;
            pts.reserve(in.polys[f].pts.size());
            for (size_t i = 0; i < in.polys[f].pts.size(); ++i)
                pts.push_back(add(dup::rotate_axis(in.polys[f].pts[i], p.axis, turn), shift));
            out.polys.push_back(daiblock::make_poly(pts));
        }
    }
    settle(out);
    return out;
}

/* ---- mirror ------------------------------------------------------------
 *
 * The shape and its reflection in the local X, Y or Z plane, walked backwards
 * so the copy faces outward too. Points within `weld` of the plane are moved
 * ONTO it first - that is what joins the two halves instead of leaving a seam
 * a micrometre wide - and a face that lies ENTIRELY on the plane is dropped
 * from both halves, because after the join it is inside the solid and an
 * interior face is a surface that is closed on paper and hollow on screen. */
inline Solid mirror(const Solid &in, const Mirror &p) {
    if (in.polys.empty()) return in;
    int a = p.axis;
    double w = p.weld > 0 ? p.weld : 0.0;

    Solid snapped;
    snapped.polys.reserve(in.polys.size());
    std::vector<char> on_plane(in.polys.size(), 0);
    for (size_t f = 0; f < in.polys.size(); ++f) {
        std::vector<V3> pts;
        pts.reserve(in.polys[f].pts.size());
        bool all = true;
        for (size_t i = 0; i < in.polys[f].pts.size(); ++i) {
            V3 v = in.polys[f].pts[i];
            if (std::fabs(dup::axis_of(v, a)) <= w) v = dup::set_axis(v, a, 0.0);
            else all = false;
            pts.push_back(v);
        }
        on_plane[f] = all ? 1 : 0;
        snapped.polys.push_back(daiblock::make_poly(pts));
    }

    Solid out;
    out.polys.reserve(snapped.polys.size() * 2);
    for (size_t f = 0; f < snapped.polys.size(); ++f) {
        if (on_plane[f]) continue;
        out.polys.push_back(snapped.polys[f]);
    }
    for (size_t f = 0; f < snapped.polys.size(); ++f) {
        if (on_plane[f]) continue;
        const std::vector<V3> &src = snapped.polys[f].pts;
        std::vector<V3> pts;
        pts.reserve(src.size());
        for (size_t i = src.size(); i > 0; --i) {
            V3 v = src[i - 1];
            pts.push_back(dup::set_axis(v, a, -dup::axis_of(v, a)));
        }
        out.polys.push_back(daiblock::make_poly(pts));
    }
    settle(out);
    return out;
}

} /* namespace daimod */

#endif /* DAI_MODIFIER_DUP_H */
