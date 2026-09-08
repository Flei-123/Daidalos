/*
 * Blockout - the shapes a room is made of, and the boolean that puts a door
 * through a wall.
 *
 * Pure arithmetic on triangles: no Vulkan, no document, no engine. That is
 * deliberate and it is what makes the CSG testable - a suite can build a wall,
 * subtract a doorway and CHECK the volume it got against the volume it can
 * work out on paper, on a machine with no GPU.
 *
 * A header rather than a source file under src/ because `build.sh` names every
 * translation unit it compiles and `build.sh` is frozen - see include/dai_ext.h
 * for the same reasoning one level up. Everything here is `inline` and depends
 * on nothing but dai_render.h's vertex layout, so the editor host, the tests
 * and the exporter all include the same code.
 *
 * The three promises this file makes:
 *
 *   DETERMINISTIC   the same fields in give the same triangles out, in the
 *                   same order, byte for byte. No clock, no random, no
 *                   hash iteration order, no pointer compared for order.
 *   CLOSED          every generator and every boolean returns a surface where
 *                   each edge is shared by exactly two triangles. That is not
 *                   decoration: an open edge is a hole light leaks through,
 *                   and a glTF exported from it is a solid nothing can print.
 *   MEASURABLE      volume(), open_edges(), nonmanifold_edges(),
 *                   degenerate_triangles() and normal_mismatches() are part
 *                   of the API, so the claim above is a check and not a
 *                   comment.
 *
 * The boolean itself is in include/dai_blockout_csg.h, included at the end of
 * this file - one `#include "dai_blockout.h"` still gets everything.
 *
 * Units are metres and sizes are FULL sizes, not half extents: a door is 0.9 m
 * wide, and an artist who has to type 0.45 will type 0.9 anyway.
 */
#ifndef DAI_BLOCKOUT_H
#define DAI_BLOCKOUT_H

#include "dai_render.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace daiblock {

/* Which shape. Mirrors dai_blockout_kind in dai_doc.h - the document stores
 * the number, this file is what the number means. */
enum Kind {
    KIND_NONE = 0,
    KIND_BOX,
    KIND_CYLINDER,
    KIND_STAIRS,
    KIND_ARCH,
    KIND_WEDGE
};

/* Which boolean. Mirrors dai_csg_op in dai_doc.h. */
enum Op {
    OP_NONE = 0,
    OP_UNION,
    OP_SUBTRACT,
    OP_INTERSECT
};

/* Everything a shape is made of. All of it is data on the node - nothing here
 * is hard coded, which is the rule the whole round follows. */
struct Shape {
    int   kind = KIND_BOX;
    float size[3] = { 1, 1, 1 };   /* FULL size in metres, per axis        */
    int   segments = 16;           /* round shapes: sides around           */
    int   steps = 8;               /* stairs: how many                     */
    float thickness = 0;           /* arch: ring thickness, 0 -> a fifth   */
    float pivot[3] = { 0, 0, 0 };  /* -1..1: where the origin sits in the
                                      shape's own box. 0 centre, -1 the min
                                      face - a wall is built from the floor
                                      up, so its pivot Y is -1.            */
};

/* ---- the double precision soup the generators and the boolean speak ----
 *
 * Doubles, not floats, and only for the intermediate steps: a plane test at
 * 1e-7 on a 30 m wall is below float's resolution there, and a boolean that
 * misclassifies one polygon returns a solid with a hole in it. The result is
 * converted to dai_vertex at the end, once. */

struct V3 {
    double x = 0, y = 0, z = 0;
};

inline V3 v3(double x, double y, double z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
inline V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 mul(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline double length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalise(V3 a) {
    double l = length(a);
    return l > 0 ? mul(a, 1.0 / l) : v3(0, 1, 0);
}
inline V3 lerp(V3 a, V3 b, double t) { return add(a, mul(sub(b, a), t)); }

/* One convex, planar face. Convex because every generator emits quads,
 * triangles or fans of them, and the BSP below only ever cuts a convex
 * polygon with a plane - which leaves two convex polygons. A fan
 * triangulation is therefore always correct, and no ear clipper is needed. */
struct Poly {
    std::vector<V3> pts;
    V3              normal;
};

struct Solid {
    std::vector<Poly> polys;
};

/* The triangles a renderer wants. Same layout as dai_render_mesh_create takes,
 * so attaching one is two lines in the host. */
struct Mesh {
    std::vector<dai_vertex> verts;
    std::vector<uint32_t>   idx;
};

/* The one tolerance in this file. Everything that asks "is this point on that
 * plane" asks it here, and nothing anywhere defines a second one - two
 * epsilons in one boolean is how a solid grows a hole nobody can reproduce. */
const double EPS = 1e-9;

/* The grid positions are snapped to before anything is welded or compared.
 * 1 micrometre: finer than any blockout an artist types, coarser than the
 * noise a rotation leaves behind. Snapping is what turns "these two corners
 * are nearly the same" into "these two corners ARE the same", and without it
 * the closed surface is closed only to within a rounding error - which is not
 * closed. */
const double SNAP = 1e-6;

inline double snap1(double v) {
    /* std::floor rather than a cast: a cast truncates towards zero, so -0.5
     * and +0.5 would round in opposite directions and a mirrored wall would
     * weld differently from the original. */
    return std::floor(v / SNAP + 0.5) * SNAP;
}
inline V3 snap(V3 v) { return v3(snap1(v.x), snap1(v.y), snap1(v.z)); }

inline V3 poly_normal(const Poly &p) {
    /* Newell: correct for any planar polygon, and unlike "cross the first two
     * edges" it does not return garbage when the first three points happen to
     * be collinear - which they routinely are once a T junction has been
     * repaired into the loop. */
    V3 n = v3(0, 0, 0);
    size_t c = p.pts.size();
    for (size_t i = 0; i < c; ++i) {
        const V3 &a = p.pts[i], &b = p.pts[(i + 1) % c];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return normalise(n);
}

inline Poly make_poly(const std::vector<V3> &pts) {
    Poly p;
    p.pts = pts;
    p.normal = poly_normal(p);
    return p;
}

inline void push_quad(Solid &s, V3 a, V3 b, V3 c, V3 d) {
    s.polys.push_back(make_poly({ a, b, c, d }));
}
inline void push_tri(Solid &s, V3 a, V3 b, V3 c) {
    s.polys.push_back(make_poly({ a, b, c }));
}

/* Turns a solid inside out: every face walked the other way round, every
 * normal the other way. */
inline void flip(Solid &s) {
    for (Poly &p : s.polys) {
        std::vector<V3> r(p.pts.rbegin(), p.pts.rend());
        p.pts = r;
        p.normal = mul(p.normal, -1.0);
    }
}

/* Defined below - build() uses them to settle which way "out" is. */
inline Mesh finalise(const Solid &sol);
inline double volume(const Mesh &m);

/* ---- the shapes ------------------------------------------------------- */

namespace detail {

inline double pi() { return 3.14159265358979323846; }

/* Where the shape sits relative to the node origin. pivot -1 puts the min
 * face on the origin, +1 the max face, 0 the centre - the same convention
 * the inspector's three sliders show. */
inline V3 pivot_offset(const Shape &s) {
    return v3(-s.pivot[0] * (double)s.size[0] * 0.5,
              -s.pivot[1] * (double)s.size[1] * 0.5,
              -s.pivot[2] * (double)s.size[2] * 0.5);
}

inline void offset_solid(Solid &sol, V3 off) {
    if (off.x == 0 && off.y == 0 && off.z == 0) return;
    for (Poly &p : sol.polys)
        for (V3 &v : p.pts) v = add(v, off);
}

inline Solid box_solid(double sx, double sy, double sz) {
    double hx = sx * 0.5, hy = sy * 0.5, hz = sz * 0.5;
    Solid s;
    /* Counter clockwise seen from outside, the same winding daimesh uses:
     * +X, -X, +Y, -Y, +Z, -Z. Checked, not believed - the suite asks
     * inward_faces() of the raw generator in every pivot position. */
    push_quad(s, v3(+hx, -hy, -hz), v3(+hx, +hy, -hz), v3(+hx, +hy, +hz), v3(+hx, -hy, +hz));
    push_quad(s, v3(-hx, -hy, +hz), v3(-hx, +hy, +hz), v3(-hx, +hy, -hz), v3(-hx, -hy, -hz));
    push_quad(s, v3(-hx, +hy, -hz), v3(-hx, +hy, +hz), v3(+hx, +hy, +hz), v3(+hx, +hy, -hz));
    push_quad(s, v3(-hx, -hy, +hz), v3(-hx, -hy, -hz), v3(+hx, -hy, -hz), v3(+hx, -hy, +hz));
    push_quad(s, v3(-hx, -hy, +hz), v3(+hx, -hy, +hz), v3(+hx, +hy, +hz), v3(-hx, +hy, +hz));
    push_quad(s, v3(+hx, -hy, -hz), v3(-hx, -hy, -hz), v3(-hx, +hy, -hz), v3(+hx, +hy, -hz));
    return s;
}

inline Solid cylinder_solid(double sx, double sy, double sz, int segments) {
    if (segments < 3) segments = 3;
    double rx = sx * 0.5, rz = sz * 0.5, hy = sy * 0.5;
    std::vector<V3> lo, hi;
    lo.reserve((size_t)segments);
    hi.reserve((size_t)segments);
    for (int i = 0; i < segments; ++i) {
        double a = 2.0 * pi() * (double)i / (double)segments;
        double x = rx * std::cos(a), z = -rz * std::sin(a);
        lo.push_back(v3(x, -hy, z));
        hi.push_back(v3(x, +hy, z));
    }
    Solid s;
    for (int i = 0; i < segments; ++i) {
        int j = (i + 1) % segments;
        push_quad(s, lo[(size_t)i], lo[(size_t)j], hi[(size_t)j], hi[(size_t)i]);
    }
    /* The caps are one n-gon each, not a fan around a centre point: a fan
     * would put a vertex in the middle of the lid that no side face has, and
     * that vertex is a T junction waiting to happen. A convex n-gon is fanned
     * once, at the end, where the winding is already settled. The ring runs
     * with z = -sin, so `hi` is already counter clockwise seen from +Y. */
    s.polys.push_back(make_poly(hi));
    std::vector<V3> bottom(lo.rbegin(), lo.rend());
    s.polys.push_back(make_poly(bottom));
    return s;
}

inline Solid stairs_solid(double sx, double sy, double sz, int steps) {
    if (steps < 1) steps = 1;
    double hx = sx * 0.5;
    double h = sy / (double)steps, d = sz / (double)steps;
    Solid s;
    /* The profile runs in +Z and rises in +Y: step i is the block from
     * z = i*d to (i+1)*d, standing on the floor and (i+1) risers high. The
     * solid is that profile extruded along X, built face by face rather than
     * as a union of boxes - a union would leave the internal faces where the
     * boxes touch inside the solid, and a solid with internal faces is not
     * closed, it is two surfaces in a trench coat. */
    for (int i = 0; i < steps; ++i) {
        double z0 = (double)i * d, z1 = z0 + d;
        double y1 = (double)(i + 1) * h;
        /* tread */
        push_quad(s, v3(-hx, y1, z1), v3(+hx, y1, z1), v3(+hx, y1, z0), v3(-hx, y1, z0));
        /* riser, facing -Z */
        push_quad(s, v3(-hx, y1 - h, z0), v3(-hx, y1, z0), v3(+hx, y1, z0), v3(+hx, y1 - h, z0));
        /* the two sides of this column */
        push_quad(s, v3(+hx, 0, z1), v3(+hx, 0, z0), v3(+hx, y1, z0), v3(+hx, y1, z1));
        push_quad(s, v3(-hx, 0, z0), v3(-hx, 0, z1), v3(-hx, y1, z1), v3(-hx, y1, z0));
    }
    /* the back, the floor, and nothing else */
    push_quad(s, v3(-hx, 0, sz), v3(+hx, 0, sz), v3(+hx, sy, sz), v3(-hx, sy, sz));
    push_quad(s, v3(-hx, 0, 0), v3(+hx, 0, 0), v3(+hx, 0, sz), v3(-hx, 0, sz));
    /* Built with its foot at the origin; the pivot moves it from there, so
     * shift into the same centred box every other shape uses first. */
    offset_solid(s, v3(0, -sy * 0.5, -sz * 0.5));
    return s;
}

inline Solid arch_solid(double sx, double sy, double sz, int segments, double thickness) {
    if (segments < 2) segments = 2;
    double span = sx * 0.5, rise = sy, depth = sz * 0.5;
    double t = thickness > 0 ? thickness : (rise < span ? rise : span) * 0.2;
    double maxt = (rise < span ? rise : span) * 0.9;
    if (t > maxt) t = maxt;
    if (t < SNAP) t = SNAP;

    std::vector<V3> outer, inner;
    outer.reserve((size_t)segments + 1);
    inner.reserve((size_t)segments + 1);
    for (int i = 0; i <= segments; ++i) {
        double a = pi() * (double)i / (double)segments;   /* 0 = +X leg, pi = -X leg */
        double c = std::cos(a), s2 = std::sin(a);
        outer.push_back(v3(span * c, rise * s2, 0));
        inner.push_back(v3((span - t) * c, (rise - t) * s2, 0));
    }
    Solid s;
    for (int i = 0; i < segments; ++i) {
        V3 o0 = outer[(size_t)i], o1 = outer[(size_t)i + 1];
        V3 i0 = inner[(size_t)i], i1 = inner[(size_t)i + 1];
        V3 fo0 = v3(o0.x, o0.y, +depth), fo1 = v3(o1.x, o1.y, +depth);
        V3 fi0 = v3(i0.x, i0.y, +depth), fi1 = v3(i1.x, i1.y, +depth);
        V3 bo0 = v3(o0.x, o0.y, -depth), bo1 = v3(o1.x, o1.y, -depth);
        V3 bi0 = v3(i0.x, i0.y, -depth), bi1 = v3(i1.x, i1.y, -depth);
        push_quad(s, fi0, fo0, fo1, fi1);      /* front face of the ring   */
        push_quad(s, bi1, bo1, bo0, bi0);      /* back face                */
        push_quad(s, bo0, bo1, fo1, fo0);      /* the outside of the arc   */
        push_quad(s, fi0, fi1, bi1, bi0);      /* the soffit               */
    }
    /* The two feet the arch stands on. */
    V3 o0 = outer.front(), i0 = inner.front();
    push_quad(s, v3(i0.x, i0.y, +depth), v3(i0.x, i0.y, -depth),
                 v3(o0.x, o0.y, -depth), v3(o0.x, o0.y, +depth));
    V3 on = outer.back(), in = inner.back();
    push_quad(s, v3(on.x, on.y, +depth), v3(on.x, on.y, -depth),
                 v3(in.x, in.y, -depth), v3(in.x, in.y, +depth));
    /* Built standing on y = 0 like the stairs; centre it. */
    offset_solid(s, v3(0, -sy * 0.5, 0));
    return s;
}

inline Solid wedge_solid(double sx, double sy, double sz) {
    double hx = sx * 0.5, hy = sy * 0.5, hz = sz * 0.5;
    /* A ramp: full height at -Z, nothing at +Z. Five faces, all convex, all
     * counter clockwise seen from outside. */
    V3 a = v3(-hx, -hy, -hz), b = v3(+hx, -hy, -hz);
    V3 c = v3(+hx, -hy, +hz), d = v3(-hx, -hy, +hz);
    V3 e = v3(-hx, +hy, -hz), f = v3(+hx, +hy, -hz);
    Solid s;
    push_quad(s, a, b, c, d);        /* floor, seen from -Y      */
    push_quad(s, a, e, f, b);        /* the back wall, from -Z   */
    push_quad(s, d, c, f, e);        /* the slope, from +Y+Z     */
    push_tri(s, a, d, e);            /* -X side                  */
    push_tri(s, b, f, c);            /* +X side                  */
    return s;
}

} /* namespace detail */

/* The shape a node's fields describe, in the node's own space. */
inline Solid build(const Shape &shape) {
    double sx = shape.size[0] > 0 ? (double)shape.size[0] : 1.0;
    double sy = shape.size[1] > 0 ? (double)shape.size[1] : 1.0;
    double sz = shape.size[2] > 0 ? (double)shape.size[2] : 1.0;
    int segments = shape.segments > 0 ? shape.segments : 16;
    int steps = shape.steps > 0 ? shape.steps : 8;
    Solid s;
    switch (shape.kind) {
    case KIND_CYLINDER: s = detail::cylinder_solid(sx, sy, sz, segments); break;
    case KIND_STAIRS:   s = detail::stairs_solid(sx, sy, sz, steps); break;
    case KIND_ARCH:     s = detail::arch_solid(sx, sy, sz, segments, (double)shape.thickness); break;
    case KIND_WEDGE:    s = detail::wedge_solid(sx, sy, sz); break;
    default:            s = detail::box_solid(sx, sy, sz); break;
    }
    detail::offset_solid(s, detail::pivot_offset(shape));
    for (Poly &p : s.polys)
        for (V3 &v : p.pts) v = snap(v);
    for (Poly &p : s.polys) p.normal = poly_normal(p);
    /* Every generator above winds its faces counter clockwise seen from
     * OUTSIDE, on its own - the suite proves it on the raw solids, before this
     * line: a positive signed volume for all five, inward_faces() == 0 for the
     * convex three, in every pivot position. The flip below therefore never
     * runs; it stays as a safety net so that a generator typed in backwards
     * one day would come out lit and exported right while the suite goes red
     * on it, rather than inside out in the viewport. */
    if (volume(finalise(s)) < 0) flip(s);
    return s;
}

/* Moves a solid into another node's space. Rotation is a quaternion in the
 * document's own layout (x, y, z, w). */
inline Solid transform(const Solid &in, const float pos[3], const float rot[4],
                       const float scale[3]) {
    Solid out;
    out.polys.reserve(in.polys.size());
    double qx = rot[0], qy = rot[1], qz = rot[2], qw = rot[3];
    for (const Poly &p : in.polys) {
        std::vector<V3> pts;
        pts.reserve(p.pts.size());
        for (const V3 &v : p.pts) {
            V3 s = v3(v.x * (double)scale[0], v.y * (double)scale[1], v.z * (double)scale[2]);
            /* q * v * conj(q), written out - a matrix here would be a second
             * definition of what a quaternion means in this codebase. */
            V3 u = v3(qx, qy, qz);
            V3 t = mul(cross(u, s), 2.0);
            V3 r = add(add(s, mul(t, qw)), cross(u, t));
            pts.push_back(snap(v3(r.x + (double)pos[0], r.y + (double)pos[1],
                                  r.z + (double)pos[2])));
        }
        out.polys.push_back(make_poly(pts));
    }
    return out;
}

/* ---- the boolean ------------------------------------------------------ */

/* csg(a, b, op) lives in include/dai_blockout_csg.h, included at the END of
 * this file: it needs everything above and nothing below, and keeping the
 * boolean in its own file lets the shapes and the boolean be worked on side
 * by side. Callers keep including this header and get both. */

/* ---- from polygons to triangles --------------------------------------- */

namespace detail {

struct Key {
    long long x, y, z;
    bool operator<(const Key &o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

inline Key key_of(V3 v) {
    Key k;
    k.x = (long long)std::floor(v.x / SNAP + 0.5);
    k.y = (long long)std::floor(v.y / SNAP + 0.5);
    k.z = (long long)std::floor(v.z / SNAP + 0.5);
    return k;
}

/* Drops the runs of repeated points a split leaves behind, and the polygons
 * that are nothing but such a run. */
inline bool clean_poly(Poly &p) {
    std::vector<V3> out;
    size_t c = p.pts.size();
    for (size_t i = 0; i < c; ++i) {
        V3 v = snap(p.pts[i]);
        if (!out.empty()) {
            Key a = key_of(out.back()), b = key_of(v);
            if (a.x == b.x && a.y == b.y && a.z == b.z) continue;
        }
        out.push_back(v);
    }
    while (out.size() > 1) {
        Key a = key_of(out.front()), b = key_of(out.back());
        if (a.x == b.x && a.y == b.y && a.z == b.z) out.pop_back();
        else break;
    }
    if (out.size() < 3) return false;
    p.pts = out;
    V3 n = poly_normal(p);
    if (length(n) < 0.5) return false;         /* zero area */
    /* Area, as an absolute check: a sliver a millionth of a square millimetre
     * across is a rounding artefact, not a face. */
    double area = 0;
    for (size_t i = 1; i + 1 < out.size(); ++i)
        area += length(cross(sub(out[i], out[0]), sub(out[i + 1], out[0]))) * 0.5;
    if (area < 1e-12) return false;
    p.normal = n;
    return true;
}

/* How far off an edge a snapped vertex may sit and still be ON it. Not SNAP
 * itself: the edge's two ends and the vertex were each moved up to half a
 * grid diagonal (0.87 SNAP) by snapping, in directions that need not agree,
 * so a point that was exactly on the edge before a rotation can be 1.7 SNAP
 * off it afterwards - and a stair turned 45 degrees came out with three open
 * edges for exactly that reason. Two grid cells covers it; anything that
 * close to an edge and not on it is below what an artist typed anyway. The
 * triangulation below judges "collinear" by the same number, so a point the
 * repair inserted is never mistaken for a corner. */
const double WELD = SNAP * 2.0;

/* T junction repair. A vertex that sits in the middle of a neighbour's edge
 * leaves that edge with one face on one side and two on the other - the seam
 * you can see daylight through, and the reason a "closed" mesh fails an edge
 * count. Inserting the vertex into the longer edge costs one point and makes
 * the surface actually closed. */
inline void repair_t_junctions(std::vector<Poly> &polys) {
    std::map<Key, V3> verts;
    for (const Poly &p : polys)
        for (const V3 &v : p.pts) verts[key_of(v)] = v;

    std::vector<V3> all;
    all.reserve(verts.size());
    for (const auto &kv : verts) all.push_back(kv.second);   /* map = sorted = deterministic */

    for (Poly &p : polys) {
        std::vector<V3> out;
        size_t c = p.pts.size();
        for (size_t i = 0; i < c; ++i) {
            const V3 &a = p.pts[i], &b = p.pts[(i + 1) % c];
            out.push_back(a);
            V3 ab = sub(b, a);
            double len2 = dot(ab, ab);
            if (len2 <= 0) continue;
            std::vector<std::pair<double, V3> > hits;
            for (const V3 &v : all) {
                double t = dot(sub(v, a), ab) / len2;
                if (t <= 1e-9 || t >= 1.0 - 1e-9) continue;
                V3 on = add(a, mul(ab, t));
                if (length(sub(on, v)) > WELD) continue;
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
}

/* Triangulates one convex polygon whose loop may carry collinear points - the
 * T junction repair above puts them there. A fan from vertex 0 is wrong on
 * such a loop: whenever vertex 0 and two inserted points share a line the fan
 * emits a triangle with no area, and that triangle is the only thing keeping
 * the position based edge count at "exactly two". So: ear clipping, in a fixed
 * order, that never clips a collinear ear.
 *
 * The rules, in the order they are asked:
 *   - an ear is the corner (prev, i, next) with i strictly convex, i.e. a
 *     positive turn about the face normal and more than WELD off the chord;
 *     a T point sits within WELD of its edge and is therefore never one;
 *   - the chord prev->next must carry no OTHER vertex of the loop. On a convex
 *     loop that only happens when the loop IS a triangle with T points along
 *     one side, and clipping across them would leave a zero area remainder
 *     and a boundary edge that no triangle owns;
 *   - the first ear in loop order is taken, starting at vertex 1 - so a loop
 *     without collinear points comes out as exactly the fan it did before,
 *     and the index order the digest sees is unchanged there;
 *   - if no vertex passes (a loop thinner than the snap grid - clean_poly
 *     keeps those when their area is still above 1e-12) the corner with the
 *     largest area is taken, first one on a tie, so the loop always closes.
 * Every boundary edge lands in exactly one triangle; no chord is a boundary
 * edge; nothing here looks at a pointer or a hash. `ring` holds indices into
 * `pts`, and `out` receives triples of them. */
inline void clip_convex(const std::vector<V3> &pts, V3 normal, std::vector<uint32_t> &out) {
    size_t n = pts.size();
    if (n < 3) return;
    std::vector<uint32_t> ring;
    ring.reserve(n);
    for (size_t i = 0; i < n; ++i) ring.push_back((uint32_t)i);

    while (ring.size() > 3) {
        size_t r = ring.size();
        size_t pick = r;             /* r = nothing yet */
        size_t best = r;
        double best_area = -1.0;
        for (size_t k = 0; k < r; ++k) {
            size_t i = (k + 1) % r;  /* start at vertex 1: the fan's first ear */
            size_t ip = (i + r - 1) % r, in = (i + 1) % r;
            const V3 &a = pts[ring[ip]], &b = pts[ring[i]], &c = pts[ring[in]];
            V3 chord = sub(c, a);
            double clen = length(chord);
            if (clen <= 0) continue;
            V3 g = cross(sub(b, a), sub(c, a));
            double area2 = length(g);
            double turn = dot(g, normal);
            if (area2 * 0.5 > best_area && turn > 0) { best_area = area2 * 0.5; best = k; }
            if (turn <= 0) continue;                    /* reflex, or backwards */
            if (area2 / clen <= WELD) continue;         /* height off the chord: a T point */
            bool on_chord = false;
            for (size_t j = 0; j < r && !on_chord; ++j) {
                if (j == ip || j == i || j == in) continue;
                const V3 &v = pts[ring[j]];
                double t = dot(sub(v, a), chord) / (clen * clen);
                if (t <= 0 || t >= 1) continue;
                if (length(sub(v, add(a, mul(chord, t)))) <= WELD) on_chord = true;
            }
            if (on_chord) continue;
            pick = k;
            break;
        }
        if (pick == r) pick = best;
        if (pick == r) pick = 0;     /* every corner is flat: the loop has no area left */
        size_t i = (pick + 1) % r, ip = (i + r - 1) % r, in = (i + 1) % r;
        out.push_back(ring[ip]);
        out.push_back(ring[i]);
        out.push_back(ring[in]);
        ring.erase(ring.begin() + (long)i);
    }
    out.push_back(ring[0]);
    out.push_back(ring[1]);
    out.push_back(ring[2]);
}

inline void axis_uv(V3 n, V3 p, float *u, float *v) {
    /* Box projection in metres: the same texel size on every face, no unwrap,
     * and it lines up with the triplanar material because both read world
     * axes rather than a chart. */
    double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az)      { *u = (float)p.z; *v = (float)p.y; }
    else if (ay >= ax && ay >= az) { *u = (float)p.x; *v = (float)p.z; }
    else                           { *u = (float)p.x; *v = (float)p.y; }
}

} /* namespace detail */

/* Welds, repairs and triangulates a solid into the triangles the renderer and
 * the glTF exporter take. Flat shaded on purpose: a blockout is read by its
 * edges, and a smoothed cube is a lie about where the corner is. */
inline Mesh finalise(const Solid &sol) {
    std::vector<Poly> polys;
    polys.reserve(sol.polys.size());
    for (const Poly &p : sol.polys) {
        Poly q = p;
        if (detail::clean_poly(q)) polys.push_back(q);
    }
    detail::repair_t_junctions(polys);

    Mesh m;
    for (const Poly &p : polys) {
        if (p.pts.size() < 3) continue;
        uint32_t base = (uint32_t)m.verts.size();
        for (const V3 &v : p.pts) {
            dai_vertex x{};
            x.position = { (float)v.x, (float)v.y, (float)v.z };
            x.normal = { (float)p.normal.x, (float)p.normal.y, (float)p.normal.z };
            detail::axis_uv(p.normal, v, &x.u, &x.v);
            m.verts.push_back(x);
        }
        /* Ear clipped, not fanned: see clip_convex for why the fan was wrong
         * once the T junction repair had put collinear points into the loop.
         * A loop without them still comes out as the same fan. */
        std::vector<uint32_t> tris;
        detail::clip_convex(p.pts, p.normal, tris);
        for (uint32_t t : tris) m.idx.push_back(base + t);
    }
    return m;
}

/* ---- the measurements the tests are written against -------------------- */

/* Signed volume in cubic metres, by the divergence theorem. Positive for a
 * solid whose faces wind counter clockwise seen from outside - so a negative
 * answer is not a small error, it is a solid that is inside out. */
inline double volume(const Mesh &m) {
    double v = 0;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        const dai_vec3 &a = m.verts[m.idx[i]].position;
        const dai_vec3 &b = m.verts[m.idx[i + 1]].position;
        const dai_vec3 &c = m.verts[m.idx[i + 2]].position;
        v += ((double)a.x * ((double)b.y * c.z - (double)b.z * c.y) -
              (double)a.y * ((double)b.x * c.z - (double)b.z * c.x) +
              (double)a.z * ((double)b.x * c.y - (double)b.y * c.x)) / 6.0;
    }
    return v;
}

inline double volume(const Solid &s) { return volume(finalise(s)); }

/* Total surface area - the second number a wall can be checked with, and the
 * one that catches a face that survived the boolean twice. */
inline double area(const Mesh &m) {
    double a = 0;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        V3 p0 = v3(m.verts[m.idx[i]].position.x, m.verts[m.idx[i]].position.y, m.verts[m.idx[i]].position.z);
        V3 p1 = v3(m.verts[m.idx[i + 1]].position.x, m.verts[m.idx[i + 1]].position.y, m.verts[m.idx[i + 1]].position.z);
        V3 p2 = v3(m.verts[m.idx[i + 2]].position.x, m.verts[m.idx[i + 2]].position.y, m.verts[m.idx[i + 2]].position.z);
        a += length(cross(sub(p1, p0), sub(p2, p0))) * 0.5;
    }
    return a;
}

/* How many edges are NOT shared by exactly two triangles. Counted by POSITION,
 * not by index: the mesh is flat shaded, so the same corner is several
 * vertices, and an index comparison would call every closed solid open. */
inline void edge_report(const Mesh &m, int *open_edges, int *nonmanifold_edges) {
    std::map<std::pair<detail::Key, detail::Key>, int> edges;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        detail::Key k[3];
        for (int j = 0; j < 3; ++j) {
            const dai_vec3 &p = m.verts[m.idx[i + (size_t)j]].position;
            k[j] = detail::key_of(v3(p.x, p.y, p.z));
        }
        for (int j = 0; j < 3; ++j) {
            detail::Key a = k[j], b = k[(j + 1) % 3];
            if (a.x == b.x && a.y == b.y && a.z == b.z) continue;   /* degenerate */
            if (b < a) { detail::Key t = a; a = b; b = t; }
            edges[std::make_pair(a, b)] += 1;
        }
    }
    int open_count = 0, bad = 0;
    for (const auto &kv : edges) {
        if (kv.second < 2) ++open_count;
        else if (kv.second > 2) ++bad;
    }
    if (open_edges) *open_edges = open_count;
    if (nonmanifold_edges) *nonmanifold_edges = bad;
}

/* How many DIRECTED edges are used twice. On a closed surface whose faces all
 * wind the same way round, every edge is walked once in each direction - so
 * this is zero, and a face that was typed in backwards is one of the few
 * modelling mistakes that has an exact test rather than an opinion. */
inline int inconsistent_edges(const Mesh &m) {
    std::map<std::pair<detail::Key, detail::Key>, int> dir;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        detail::Key k[3];
        for (int j = 0; j < 3; ++j) {
            const dai_vec3 &p = m.verts[m.idx[i + (size_t)j]].position;
            k[j] = detail::key_of(v3(p.x, p.y, p.z));
        }
        for (int j = 0; j < 3; ++j) {
            detail::Key a = k[j], b = k[(j + 1) % 3];
            if (!(a < b) && !(b < a)) continue;      /* degenerate */
            dir[std::make_pair(a, b)] += 1;
        }
    }
    int bad = 0;
    for (const auto &kv : dir) if (kv.second > 1) ++bad;
    return bad;
}

inline int open_edges(const Mesh &m) {
    int o = 0, b = 0;
    edge_report(m, &o, &b);
    return o;
}
inline int nonmanifold_edges(const Mesh &m) {
    int o = 0, b = 0;
    edge_report(m, &o, &b);
    return b;
}

/* How many triangles have no area: two corners on the same snapped position,
 * or three corners on one line. A degenerate triangle is a rendering artefact
 * at best and a glTF validator error at worst - and on a mesh that is welded
 * by position it is also how an edge count can come out "right" while the
 * surface is really open. Zero on every generator and every boolean. */
inline int degenerate_triangles(const Mesh &m) {
    int bad = 0;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        const dai_vec3 &a = m.verts[m.idx[i]].position;
        const dai_vec3 &b = m.verts[m.idx[i + 1]].position;
        const dai_vec3 &c = m.verts[m.idx[i + 2]].position;
        V3 p0 = v3(a.x, a.y, a.z), p1 = v3(b.x, b.y, b.z), p2 = v3(c.x, c.y, c.z);
        detail::Key k0 = detail::key_of(p0), k1 = detail::key_of(p1), k2 = detail::key_of(p2);
        bool same = (!(k0 < k1) && !(k1 < k0)) || (!(k1 < k2) && !(k2 < k1)) ||
                    (!(k0 < k2) && !(k2 < k0));
        double area = length(cross(sub(p1, p0), sub(p2, p0))) * 0.5;
        if (same || area < 1e-12) ++bad;
    }
    return bad;
}

/* How many triangles carry a normal that disagrees with their own winding.
 * The stored normal is what the renderer lights with, the winding is what the
 * volume and the edge counts are computed from; the two must agree on every
 * face or the surface is closed on paper and lit inside out on screen. Valid
 * for ANY solid, convex or not. */
inline int normal_mismatches(const Mesh &m) {
    int bad = 0;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        const dai_vertex &a = m.verts[m.idx[i]];
        const dai_vertex &b = m.verts[m.idx[i + 1]];
        const dai_vertex &c = m.verts[m.idx[i + 2]];
        V3 p0 = v3(a.position.x, a.position.y, a.position.z);
        V3 p1 = v3(b.position.x, b.position.y, b.position.z);
        V3 p2 = v3(c.position.x, c.position.y, c.position.z);
        V3 g = cross(sub(p1, p0), sub(p2, p0));
        if (length(g) < 1e-12) continue;             /* degenerate: counted above */
        g = normalise(g);
        V3 n = v3(a.normal.x, a.normal.y, a.normal.z);
        if (dot(g, n) < 0.999) ++bad;
    }
    return bad;
}

/* How many faces point INTO the solid, judged from its centroid: for a CONVEX
 * body every outward normal makes a positive dot with the vector from the
 * centroid to the face's middle. Meaningless on a stair or an arch - the
 * soffit of an arch looks at the centroid on purpose - so the suite asks it
 * of the box, the cylinder, the wedge and the convex booleans only. */
inline int inward_faces(const Mesh &m) {
    if (m.verts.empty()) return 0;
    V3 c = v3(0, 0, 0);
    for (const dai_vertex &v : m.verts) c = add(c, v3(v.position.x, v.position.y, v.position.z));
    c = mul(c, 1.0 / (double)m.verts.size());
    int bad = 0;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        const dai_vertex &a = m.verts[m.idx[i]];
        const dai_vertex &b = m.verts[m.idx[i + 1]];
        const dai_vertex &d = m.verts[m.idx[i + 2]];
        V3 mid = v3((a.position.x + b.position.x + d.position.x) / 3.0,
                    (a.position.y + b.position.y + d.position.y) / 3.0,
                    (a.position.z + b.position.z + d.position.z) / 3.0);
        V3 n = v3(a.normal.x, a.normal.y, a.normal.z);
        if (dot(n, sub(mid, c)) <= 0) ++bad;
    }
    return bad;
}

/* The axis aligned box the mesh fills - what the inspector shows and what the
 * host uses to keep the collider the size of the thing you can see. */
inline void bounds(const Mesh &m, float *min3, float *max3) {
    float lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const dai_vec3 &p = m.verts[i].position;
        float c[3] = { p.x, p.y, p.z };
        for (int j = 0; j < 3; ++j) {
            if (i == 0 || c[j] < lo[j]) lo[j] = c[j];
            if (i == 0 || c[j] > hi[j]) hi[j] = c[j];
        }
    }
    if (min3) { min3[0] = lo[0]; min3[1] = lo[1]; min3[2] = lo[2]; }
    if (max3) { max3[0] = hi[0]; max3[1] = hi[1]; max3[2] = hi[2]; }
}

/* A stable fingerprint of the triangles, for the determinism proof and for the
 * host's rebuild cache. FNV-1a over the raw positions and indices: two builds
 * that differ in one bit differ here. */
inline uint64_t digest(const Mesh &m) {
    uint64_t h = 1469598103934665603ull;
    auto eat = [&h](const void *p, size_t n) {
        const unsigned char *b = (const unsigned char *)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    for (const dai_vertex &v : m.verts) {
        eat(&v.position, sizeof(v.position));
        eat(&v.normal, sizeof(v.normal));
        eat(&v.u, sizeof(v.u));
        eat(&v.v, sizeof(v.v));
    }
    for (uint32_t i : m.idx) eat(&i, sizeof(i));
    return h;
}

} /* namespace daiblock */

#include "dai_blockout_csg.h"

#endif /* DAI_BLOCKOUT_H */
