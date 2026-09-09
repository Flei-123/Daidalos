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

/* Where solidify pushes ONE corner, and how far.
 *
 * The direction is the average of the normals of the distinct PLANES that
 * meet at that corner - pushing each FACE along its own normal would tear the
 * surface apart at every edge it has, and averaging over the FACES weights
 * the answer by how a modeller happened to split a surface up.
 *
 * "Distinct planes" is not fussiness, it is the difference between a wall
 * that works and one that does not. A 4 x 3 m wall with a doorway cut in it
 * has its front split into three polygons by the boolean; a corner of the
 * reveal touches two of them, both facing +Z, plus the jamb and the head. Add
 * the four face normals up and +Z outvotes the other two 2:1, the corner
 * leans out of the front, and a 10 mm skin on that wall comes out holding
 * twice the material it should. Count the +Z plane ONCE and the corner points
 * down (1, -1, 1) - which is exactly where the three offset planes cross,
 * i.e. the right answer rather than a good one. Area weighting has the same
 * fault for a worse reason: it makes a 12 m2 face outvote a 0.6 m2 one.
 *
 * The distance is NOT the thickness. A cube's corner points down its body
 * diagonal, so moving it by t moves each of the three faces that own it by
 * only t / sqrt(3), and a "0.05 m wall" would come out 29 mm thick. `scale`
 * is the correction: the reciprocal of the mean of dot(corner direction,
 * plane normal) over those same distinct planes, which is exactly sqrt(3) at
 * a cube corner, exactly sqrt(2) at the fold of an L, and exactly 1 on a flat
 * sheet. With it EVERY face of the offset surface stands the thickness away
 * from the face it came from, and the volume of the result is arithmetic a
 * test can do on paper.
 *
 * Exact for one plane, for two, and for three at right angles - which is the
 * whole of a blockout and all of a boolean between two of them. Approximate,
 * and clamped at 4, where more than three planes meet at one point or where
 * two of them fold back to within 29 degrees of each other: a corner that
 * runs away to infinity is a worse answer than a slightly thin wall. */
struct Push {
    V3     dir = { 0, 1, 0 };
    double scale = 1.0;
};

inline std::map<Key, Push> vertex_pushes(const Solid &s) {
    /* corner -> the planes there, one entry per distinct normal. Both levels
     * are a std::map keyed by the snapped value, so the sums below are
     * accumulated in sorted order and the answer is bit identical every run. */
    std::map<Key, std::map<Key, V3> > planes;
    for (size_t f = 0; f < s.polys.size(); ++f) {
        const Poly &p = s.polys[f];
        Key n = daiblock::detail::key_of(p.normal);
        for (size_t i = 0; i < p.pts.size(); ++i)
            planes[daiblock::detail::key_of(p.pts[i])][n] = p.normal;
    }

    std::map<Key, Push> out;
    for (std::map<Key, std::map<Key, V3> >::const_iterator it = planes.begin();
         it != planes.end(); ++it) {
        const std::map<Key, V3> &here = it->second;
        V3 sum = v3(0, 0, 0);
        for (std::map<Key, V3>::const_iterator n = here.begin(); n != here.end(); ++n)
            sum = add(sum, n->second);
        Push q;
        q.dir = length(sum) > 1e-12 ? normalise(sum) : v3(0, 1, 0);
        double d = 0;
        for (std::map<Key, V3>::const_iterator n = here.begin(); n != here.end(); ++n)
            d += dot(q.dir, n->second);
        d = here.empty() ? 1.0 : d / (double)here.size();
        double sc = d > 0.25 ? 1.0 / d : 4.0;
        q.scale = sc < 1.0 ? 1.0 : sc;
        out[it->first] = q;
    }
    return out;
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
 * material inward), +1 the inner skin, 0 the middle. Every face of the result
 * stands `thickness` away from the face it came from - see dup::Push for why
 * that needs more than an averaged normal - so:
 *
 *   a flat sheet of area A          ->  A * thickness, exactly;
 *   a closed box of size s          ->  (s + 2*out)^3 - (s - 2*in)^3, exactly,
 *                                       with out + in = thickness;
 *   a sheet with a HOLE in it       ->  closed, both rims walled, and the hole
 *                                       still a hole (its rim is a border edge
 *                                       like any other);
 *   a CONCAVE fold                  ->  closed, and the wall the same thickness
 *                                       through the fold as beside it.
 *
 * Two limits, both deliberate and both measured in the suite rather than
 * assumed away:
 *
 *   A wall THICKER THAN THE BODY it is given. On a closed body, an inward
 *   offset of half the smallest bounding extent or more would push the inner
 *   skin out through the far side and hand back a solid turned inside out.
 *   The answer for "the wall is thicker than the part" is that the part is
 *   SOLID, so the inner skin is dropped and the outer one returned on its own:
 *   still closed, still positive, and the volume of the body it describes.
 *
 *   Self intersection on a strongly concave OPEN surface whose fold is
 *   sharper than the thickness is wide. Nothing here detects it; the operator
 *   is an offset, not a boolean, and the same is true of the tool this one is
 *   named after. */
inline Solid solidify(const Solid &in, const Solidify &p) {
    if (in.polys.empty() || p.thickness <= 0) return in;
    std::map<Key, dup::Push> vp = dup::vertex_pushes(in);
    /* The sign is the one include/dai_modifier.h documents and the one the
     * inspector shows: shift -1 puts ALL the material behind the surface
     * (inward, against the normal), +1 puts all of it in front. */
    double outer = p.thickness * (1.0 + p.shift) * 0.5;
    double inner = p.thickness * (1.0 - p.shift) * 0.5;

    Topology t = topology_of(in);
    bool border = false;
    for (size_t e = 0; e < t.edges.size(); ++e)
        if (t.edges[e].count == 1) { border = true; break; }
    /* A closed body eaten through by its own wall is a solid part - see the
     * comment above. Judged on the smallest bounding extent, which is the
     * thinnest the body can possibly be. */
    bool solid_through = false;
    if (!border) {
        V3 lo, hi;
        bounds(in, &lo, &hi);
        double mn = hi.x - lo.x;
        if (hi.y - lo.y < mn) mn = hi.y - lo.y;
        if (hi.z - lo.z < mn) mn = hi.z - lo.z;
        if (inner >= mn * 0.5) solid_through = true;
    }

    auto moved = [&](const V3 &v, double d) -> V3 {
        std::map<Key, dup::Push>::const_iterator it = vp.find(daiblock::detail::key_of(v));
        if (it == vp.end()) return v;
        return add(v, mul(it->second.dir, d * it->second.scale));
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
    for (size_t f = 0; !solid_through && f < in.polys.size(); ++f) {
        std::vector<V3> pts;
        pts.reserve(in.polys[f].pts.size());
        for (size_t i = in.polys[f].pts.size(); i > 0; --i)
            pts.push_back(moved(in.polys[f].pts[i - 1], -inner));
        out.polys.push_back(daiblock::make_poly(pts));
    }
    /* The rim: one quad per border edge, walked so it agrees with the outer
     * skin's direction along that edge. Without it the two skins are two
     * surfaces and every border edge is open. */
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
 * `count` copies in total, copy i TURNED by i * rotation degrees about
 * `axis` - about the shape's own origin, not about its centre - and then
 * moved by i * offset. A relative offset is a multiple of the shape's own
 * bounding size per axis, so "one step forward" stays one step forward when
 * the step gets deeper.
 *
 * Nothing is welded between copies: the array of a closed shape is `count`
 * closed shapes, which is why the triangle count comes out as exactly count
 * times the original's and the bounding box grows by (count - 1) * offset.
 *
 * WHAT HAPPENS WHEN TWO COPIES TOUCH, because a stair is built out of exactly
 * that and somebody will read the edge counts:
 *
 *   touching along an EDGE - the stair, offset (0, height, depth) on a step
 *   of exactly that height and depth - is the good case. The copies share a
 *   line and nothing else, every triangle keeps its area, no edge is left
 *   with one face, and the volume is exactly count times the step's. The
 *   shared line is an edge with FOUR faces on it, which is what two solids
 *   touching along a line is; open_edges stays 0 and nonmanifold_edges
 *   reports the touches, one per join. That is the case the stair example in
 *   examples/scripts/ builds and the case the suite measures.
 *
 *   touching along a FACE - offset exactly the bounding size on ONE axis -
 *   leaves two coincident faces buried inside the run. The volume is still
 *   count times the original, but the two faces are there and a renderer will
 *   show them fighting. Array does not weld and does not boolean; a run of
 *   boxes that is meant to be one box is one box with a longer size.
 *
 *   OVERLAPPING copies double count the volume they share, exactly the way
 *   two overlapping solids in the same mesh always have.
 *
 * `count` is clamped to 512. A stack is a modelling tool, and a typed 100000
 * is a mistake that should cost a wide stair rather than the editor. */
inline Solid array(const Solid &in, const Array &p) {
    int n = p.count;
    if (n < 1) n = 1;
    if (n > 512) n = 512;
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
 * interior face is a surface that is closed on paper and hollow on screen.
 *
 * The weld is the whole difference between a mirror and two objects. Half a
 * box that stops 0.5 mm short of the plane, mirrored with weld 1 mm, is ONE
 * closed box: the near faces are pulled onto x = 0, recognised as lying on
 * the plane, and dropped, so the two halves share their corners. The same
 * half with weld 0.1 mm is two boxes 1 mm apart, with both near faces still
 * in the mesh - four more triangles, and two of them where nobody can see
 * them. The suite counts those triangles rather than looking at a picture.
 *
 * Geometry that CROSSES the plane is reflected as it stands, so the part that
 * crossed ends up inside the other half. Mirror does not clip and does not
 * boolean: the half you hand it is the half you get back, twice. */
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
