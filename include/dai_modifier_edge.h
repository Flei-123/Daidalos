/*
 * The two modifiers that work on the edges and faces a shape already has:
 * bevel (break the sharp edges) and subdivide (four faces where there was
 * one, optionally pulled towards the smooth surface the cage describes).
 *
 * Included at the end of include/dai_modifier.h - never included on its own.
 * Read that file first; deterministic, closed, measurable are promises this
 * file has to keep, and each of them is a test and not a comment.
 *
 * MODULE B1 OWNS THIS FILE. Nothing else in the tree may define bevel() or
 * subdivide(), and this file defines nothing else: a helper both this and
 * dai_modifier_dup.h want belongs in include/dai_modifier.h.
 */
#ifndef DAI_MODIFIER_EDGE_H
#define DAI_MODIFIER_EDGE_H

#ifndef DAI_MODIFIER_H
#error "include dai_modifier.h, not dai_modifier_edge.h"
#endif

namespace daimod {

/* Everything private to the two operators below. Nothing outside this file
 * may reach in here, and nothing in here reaches out: the only inputs are a
 * Solid, the parameter struct and the topology dai_modifier.h builds. */
namespace edge {

typedef daiblock::detail::Key EKey;

inline EKey key_of(const V3 &v) { return daiblock::detail::key_of(v); }
inline bool key_eq(const EKey &a, const EKey &b) { return !(a < b) && !(b < a); }

/* ---- Catmull-Clark, the point rules ------------------------------------
 *
 * One level, on the cage it is handed. The topology it produces is exactly
 * the topology the flat division produces - n quads per n-gon, in the same
 * order, wound the same way - so the two paths differ in WHERE the points
 * are and in nothing else. That is what lets `smooth` be a tick box rather
 * than a second modifier.
 *
 * The three rules, in Catmull and Clark's own order:
 *
 *   face point    F = the centroid of the face.
 *   edge point    E = (v0 + v1 + F0 + F1) / 4 for an edge with two faces,
 *                 and the plain midpoint for a border edge - a border has no
 *                 second face to average with, and pulling it inward would
 *                 shrink an open sheet away from its own outline.
 *   vertex point  V' = (F + 2R + (n-3)V) / n, with n the valence, F the mean
 *                 of the face points around the vertex and R the mean of the
 *                 incident edge MIDPOINTS. A vertex with fewer than three
 *                 edges has no surface around it to average, so it stays.
 *
 * Every container that decides an order here is a std::map keyed by the
 * snapped position, so the sums are accumulated in sorted order and the
 * result is bit identical from one run to the next. */
inline Solid catmull(const Solid &cur) {
    size_t nf = cur.polys.size();

    std::vector<V3> fp(nf, v3(0, 0, 0));
    for (size_t f = 0; f < nf; ++f) {
        const std::vector<V3> &q = cur.polys[f].pts;
        if (q.empty()) continue;
        V3 c = v3(0, 0, 0);
        for (size_t i = 0; i < q.size(); ++i) c = add(c, q[i]);
        fp[f] = mul(c, 1.0 / (double)q.size());
    }

    struct EdgeAcc { V3 mid; V3 fsum; int n; };
    std::map<std::pair<EKey, EKey>, EdgeAcc> em;
    for (size_t f = 0; f < nf; ++f) {
        const std::vector<V3> &q = cur.polys[f].pts;
        size_t c = q.size();
        for (size_t i = 0; i < c; ++i) {
            EKey ka = key_of(q[i]), kb = key_of(q[(i + 1) % c]);
            if (key_eq(ka, kb)) continue;
            EKey a = ka, b = kb;
            if (b < a) { EKey t = a; a = b; b = t; }
            std::pair<EKey, EKey> k = std::make_pair(a, b);
            std::map<std::pair<EKey, EKey>, EdgeAcc>::iterator it = em.find(k);
            if (it == em.end()) {
                EdgeAcc acc;
                acc.mid = mul(add(q[i], q[(i + 1) % c]), 0.5);
                acc.fsum = fp[f];
                acc.n = 1;
                em[k] = acc;
            } else {
                it->second.fsum = add(it->second.fsum, fp[f]);
                it->second.n += 1;
            }
        }
    }

    struct VertAcc { V3 pos; V3 fsum; int fn; V3 msum; int mn; };
    std::map<EKey, VertAcc> vm;
    for (size_t f = 0; f < nf; ++f) {
        const std::vector<V3> &q = cur.polys[f].pts;
        for (size_t i = 0; i < q.size(); ++i) {
            EKey k = key_of(q[i]);
            std::map<EKey, VertAcc>::iterator it = vm.find(k);
            if (it == vm.end()) {
                VertAcc a;
                a.pos = q[i]; a.fsum = fp[f]; a.fn = 1;
                a.msum = v3(0, 0, 0); a.mn = 0;
                vm[k] = a;
            } else {
                it->second.fsum = add(it->second.fsum, fp[f]);
                it->second.fn += 1;
            }
        }
    }
    for (std::map<std::pair<EKey, EKey>, EdgeAcc>::const_iterator it = em.begin();
         it != em.end(); ++it) {
        for (int s = 0; s < 2; ++s) {
            EKey k = s == 0 ? it->first.first : it->first.second;
            std::map<EKey, VertAcc>::iterator v = vm.find(k);
            if (v == vm.end()) continue;
            v->second.msum = add(v->second.msum, it->second.mid);
            v->second.mn += 1;
        }
    }

    std::map<EKey, V3> moved;
    for (std::map<EKey, VertAcc>::const_iterator it = vm.begin(); it != vm.end(); ++it) {
        const VertAcc &a = it->second;
        double n = (double)a.mn;
        if (a.mn < 3 || a.fn < 1) { moved[it->first] = daiblock::snap(a.pos); continue; }
        V3 F = mul(a.fsum, 1.0 / (double)a.fn);
        V3 R = mul(a.msum, 1.0 / n);
        V3 out = mul(add(add(F, mul(R, 2.0)), mul(a.pos, n - 3.0)), 1.0 / n);
        moved[it->first] = daiblock::snap(out);
    }

    std::map<std::pair<EKey, EKey>, V3> epoint;
    for (std::map<std::pair<EKey, EKey>, EdgeAcc>::const_iterator it = em.begin();
         it != em.end(); ++it) {
        const EdgeAcc &a = it->second;
        V3 e = a.n == 2 ? mul(add(mul(a.mid, 2.0), a.fsum), 0.25) : a.mid;
        epoint[it->first] = daiblock::snap(e);
    }

    Solid out;
    out.polys.reserve(nf * 4);
    for (size_t f = 0; f < nf; ++f) {
        const std::vector<V3> &q = cur.polys[f].pts;
        size_t c = q.size();
        if (c < 3) continue;
        V3 centre = daiblock::snap(fp[f]);
        for (size_t i = 0; i < c; ++i) {
            EKey kp = key_of(q[(i + c - 1) % c]), ki = key_of(q[i]), kn = key_of(q[(i + 1) % c]);
            V3 prev = daiblock::snap(mul(add(q[(i + c - 1) % c], q[i]), 0.5));
            V3 next = daiblock::snap(mul(add(q[i], q[(i + 1) % c]), 0.5));
            V3 here = q[i];
            std::map<EKey, V3>::const_iterator mv = moved.find(ki);
            if (mv != moved.end()) here = mv->second;
            std::pair<EKey, EKey> ep = kp < ki ? std::make_pair(kp, ki) : std::make_pair(ki, kp);
            std::map<std::pair<EKey, EKey>, V3>::const_iterator pe = epoint.find(ep);
            if (pe != epoint.end()) prev = pe->second;
            std::pair<EKey, EKey> en = ki < kn ? std::make_pair(ki, kn) : std::make_pair(kn, ki);
            std::map<std::pair<EKey, EKey>, V3>::const_iterator ne = epoint.find(en);
            if (ne != epoint.end()) next = ne->second;
            out.polys.push_back(daiblock::make_poly({ prev, here, next, centre }));
        }
    }
    settle(out);
    return out;
}

/* ---- what the bevel needs before it can start --------------------------
 *
 * A boolean hands back a surface with T junctions in it: the door reveal ends
 * in the middle of the wall's front face, so that face's loop has a corner
 * sitting on an edge whose two neighbours never heard of it. daiblock's
 * finalise() repairs those on the way to triangles, which is why the wall
 * comes out closed - but the bevel works on the POLYGONS, and a loop with a
 * hidden T junction has a vertex whose faces do not form one ring, so the
 * corner face at that vertex cannot be walked. Repairing first costs one
 * point per T and turns every vertex into a proper umbrella. */
inline Solid prepare(const Solid &in) {
    std::vector<Poly> polys;
    polys.reserve(in.polys.size());
    for (size_t i = 0; i < in.polys.size(); ++i) {
        Poly q = in.polys[i];
        if (daiblock::detail::clean_poly(q)) polys.push_back(q);
    }
    daiblock::detail::repair_t_junctions(polys);
    Solid s;
    s.polys.swap(polys);
    settle(s);
    return s;
}

/* One step of the ring of faces around a corner: the face, which of its
 * corners sits on the point, and the edge the walk leaves it by. */
struct Step {
    int f, i, e;
};

/* The ring of faces around one corner, walked in the loop direction: stand on
 * face f at its corner i, leave by the edge that starts there, land on the
 * face on the other side of it, repeat. Answers false - and leaves whatever
 * it managed to collect - when the surface is open or non manifold at that
 * point, because then there is no ring and nothing sensible to build.
 *
 * The walk is the whole geometry of the bevel: WHERE the retreated corners
 * are is decided by it, and so is the patch that closes the corner, and the
 * two agree because they are the same list. */
inline bool umbrella(const Solid &src, const Topology &topo,
                     const std::vector<std::vector<int> > &eof,
                     size_t f0, size_t i0, std::vector<Step> &ring) {
    ring.clear();
    size_t f = f0, i = i0;
    EKey kx = key_of(src.polys[f0].pts[i0]);
    for (size_t guard = 0; guard <= topo.edges.size() + 4; ++guard) {
        Step st;
        st.f = (int)f; st.i = (int)i; st.e = eof[f][i];
        ring.push_back(st);
        if (st.e < 0) return false;
        const Edge &e = topo.edges[(size_t)st.e];
        if (e.count != 2 || e.face[0] < 0 || e.face[1] < 0) return false;
        size_t g = (size_t)(e.face[0] == (int)f ? e.face[1] : e.face[0]);
        if (g == f) return false;
        size_t ng = src.polys[g].pts.size();
        size_t cg = (size_t)(e.face[0] == (int)g ? e.corner[0] : e.corner[1]);
        if (cg >= ng) return false;
        size_t jv = key_eq(key_of(src.polys[g].pts[cg]), kx) ? cg : (cg + 1) % ng;
        if (!key_eq(key_of(src.polys[g].pts[jv]), kx)) return false;
        f = g; i = jv;
        if (f == f0 && i == i0) return true;
    }
    return false;
}

/* Where three planes meet. Two of them are the retreated edges the corner
 * lies between, the third is the surface the corner is on - which is the only
 * way to place the corner of a face that has been CUT IN TWO by a boolean and
 * therefore only knows about one of its two constraints. False when the three
 * do not meet in a point: two nearly parallel retreats, and the caller has a
 * fallback for that. */
inline bool meet3(V3 na, double da, V3 nb, double db, V3 nc, double dc, V3 &out) {
    V3 bc = cross(nb, nc);
    double det = dot(na, bc);
    if (std::fabs(det) < 1e-6) return false;
    V3 r = add(add(mul(bc, da), mul(cross(nc, na), db)), mul(cross(na, nb), dc));
    out = mul(r, 1.0 / det);
    return true;
}

/* How far this face may pull back before it eats itself. The limit is the
 * distance from a broken edge's LINE to the nearest corner of the face that
 * is not on it - a face cannot retreat past the far side of itself - taken at
 * 45%, so two opposite edges both retreating still leave a tenth of the face.
 * Corners that sit ON the line are the collinear points the T junction repair
 * inserted and say nothing about how wide the face is. */
inline double face_width(const Poly &pf, const std::vector<int> &eidx,
                         const std::vector<char> &brk, double want) {
    const std::vector<V3> &q = pf.pts;
    size_t c = q.size();
    double w = want;
    for (size_t i = 0; i < c; ++i) {
        if (eidx[i] < 0 || !brk[(size_t)eidx[i]]) continue;
        V3 d = sub(q[(i + 1) % c], q[i]);
        double l = length(d);
        if (l <= 0) continue;
        d = mul(d, 1.0 / l);
        for (size_t j = 0; j < c; ++j) {
            if (j == i || j == (i + 1) % c) continue;
            V3 r = sub(q[j], q[i]);
            double h = length(sub(r, mul(d, dot(r, d))));
            if (h <= 1e-5) continue;                   /* collinear: not a far side */
            if (0.45 * h < w) w = 0.45 * h;
        }
    }
    return w;
}

/* The direction a face retreats in along one of its edges: into the face, in
 * the face's own plane, at right angles to the edge. `d` is the edge as that
 * face walks it - cross(normal, edge) points inward for a loop wound counter
 * clockwise seen from outside, which every face in this codebase is. */
inline V3 retreat_dir(const Poly &pf, V3 from, V3 to) {
    V3 d = sub(to, from);
    double l = length(d);
    if (l <= 0) return v3(0, 0, 0);
    return normalise(cross(pf.normal, mul(d, 1.0 / l)));
}

/* Did the pull back leave a face, or did it turn the loop inside out? A
 * polygon that flipped its normal or lost its area has folded through itself,
 * and the width that did it is halved and tried again. */
inline bool inset_ok(const Poly &pf, const std::vector<V3> &got) {
    Poly t;
    t.pts = got;
    V3 n = daiblock::poly_normal(t);
    if (dot(n, pf.normal) < 0.9) return false;
    double area = 0;
    for (size_t i = 1; i + 1 < got.size(); ++i)
        area += length(cross(sub(got[i], got[0]), sub(got[i + 1], got[0]))) * 0.5;
    return area > 1e-12;
}

/* Is every turn of this loop the same way round about its own normal? Faces
 * go into daiblock::finalise() convex or not at all - its triangulator is
 * allowed to assume it, and says so - and a bevel is the one operation that
 * can hand back a face that is not. The wall's back face is cut in two by the
 * doorway; the half that keeps the corner of the opening gets a REFLEX corner
 * the moment that corner retreats, because the material wraps around the
 * opening there. Collinear points - the T junction repair's - are neither
 * way and are let through. */
inline bool convex_loop(const std::vector<V3> &q, V3 n) {
    size_t c = q.size();
    for (size_t i = 0; i < c; ++i) {
        V3 a = sub(q[i], q[(i + c - 1) % c]), b = sub(q[(i + 1) % c], q[i]);
        double la = length(a), lb = length(b);
        if (la <= 0 || lb <= 0) continue;
        if (dot(cross(a, b), n) / (la * lb) < -1e-6) return false;
    }
    return true;
}

/* A face for the result: as it is when it is convex, ear clipped into
 * triangles when it is not. The ear taken is the first in loop order whose
 * corner turns the right way, whose triangle has area, and that no other
 * corner of the loop stands inside - which is ear clipping's own rule and the
 * only one that is correct on a reflex loop. Every clipped diagonal is shared
 * by the two triangles either side of it and every boundary edge stays where
 * it was, so a closed surface stays closed. */
inline void emit_face(const std::vector<V3> &q, Solid &out) {
    if (q.size() < 3) return;
    Poly probe;
    probe.pts = q;
    V3 n = daiblock::poly_normal(probe);
    if (q.size() == 3) {
        out.polys.push_back(daiblock::make_poly(q));
        return;
    }
    /* Flat as well as convex: a strip that crosses a rounded corner is a
     * quad whose four points are not quite in one plane, and a face carries
     * ONE normal for the renderer to light it with. Two triangles each with
     * their own is not a workaround, it is the truth about that quad. */
    double bend = 0;
    for (size_t i = 0; i < q.size(); ++i) {
        double d = std::fabs(dot(sub(q[i], q[0]), n));
        if (d > bend) bend = d;
    }
    if (bend <= daiblock::detail::WELD && convex_loop(q, n)) {
        out.polys.push_back(daiblock::make_poly(q));
        return;
    }
    std::vector<size_t> ring;
    ring.reserve(q.size());
    for (size_t i = 0; i < q.size(); ++i) ring.push_back(i);
    while (ring.size() > 3) {
        size_t r = ring.size(), pick = r;
        for (size_t k = 0; k < r && pick == r; ++k) {
            size_t ip = (k + r - 1) % r, in = (k + 1) % r;
            const V3 &a = q[ring[ip]], &b = q[ring[k]], &c = q[ring[in]];
            V3 g = cross(sub(b, a), sub(c, a));
            if (dot(g, n) <= 0) continue;                       /* reflex or flat */
            double clen = length(sub(c, a));
            if (clen <= 0 || length(g) / clen <= daiblock::detail::WELD) continue;
            bool blocked = false;
            for (size_t j = 0; j < r && !blocked; ++j) {
                if (j == k || j == ip || j == in) continue;
                const V3 &v = q[ring[j]];
                double s0 = dot(cross(sub(b, a), sub(v, a)), n) / (length(sub(b, a)) + 1e-30);
                double s1 = dot(cross(sub(c, b), sub(v, b)), n) / (length(sub(c, b)) + 1e-30);
                double s2 = dot(cross(sub(a, c), sub(v, c)), n) / (clen + 1e-30);
                double w = daiblock::detail::WELD;
                if (s0 > w && s1 > w && s2 > w) blocked = true;
            }
            if (!blocked) pick = k;
        }
        if (pick == r) break;
        size_t ip = (pick + ring.size() - 1) % ring.size(), in = (pick + 1) % ring.size();
        out.polys.push_back(daiblock::make_poly({ q[ring[ip]], q[ring[pick]], q[ring[in]] }));
        ring.erase(ring.begin() + (long)pick);
    }
    if (ring.size() >= 3) {
        std::vector<V3> rest;
        rest.reserve(ring.size());
        for (size_t i = 0; i < ring.size(); ++i) rest.push_back(q[ring[i]]);
        out.polys.push_back(daiblock::make_poly(rest));
    }
}

/* The corner patch, from the loop the umbrella walk collected - and the two
 * things that loop can be besides a simple ring:
 *
 *   A SPIKE. Two broken edges whose faces retreat onto the same point leave
 *   the loop going A -> T -> A: the strip either side of T already meets
 *   itself there, so the edge A-T has its two faces and a patch on top of it
 *   would be a third. Out it comes, tip first, until nothing goes out and
 *   comes straight back.
 *   A PINCH. The same point twice in a loop that is otherwise fine - the
 *   corner of a step where two coplanar side faces touch at one point. That
 *   is two patches meeting at a point, not one patch, and it is cut into the
 *   two loops it really is.
 *
 * Then: three points is a triangle, more is a fan from the loop's own centre.
 * At two segments and up the patch is not flat, and a fan is planar per
 * triangle, star shaped whatever the corner does, and puts no point on
 * anybody else's edge. */
inline void corner_faces(std::vector<V3> loop, Solid &out) {
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<V3> s;
        s.reserve(loop.size());
        for (size_t i = 0; i < loop.size(); ++i) {
            if (s.size() >= 2 && key_eq(key_of(s[s.size() - 2]), key_of(loop[i]))) {
                s.pop_back();
                continue;
            }
            if (!s.empty() && key_eq(key_of(s.back()), key_of(loop[i]))) continue;
            s.push_back(loop[i]);
        }
        while (s.size() >= 2 && key_eq(key_of(s.front()), key_of(s.back()))) s.pop_back();
        bool done = s.size() == loop.size();
        loop.swap(s);
        if (loop.size() < 3) return;
        if (done) break;
        /* The seam the pass could not see is in the middle of the next one. */
        std::rotate(loop.begin(), loop.begin() + 1, loop.end());
    }
    for (size_t i = 0; i + 1 < loop.size(); ++i)
        for (size_t j = i + 1; j < loop.size(); ++j)
            if (key_eq(key_of(loop[i]), key_of(loop[j]))) {
                std::vector<V3> a(loop.begin() + (long)i, loop.begin() + (long)j);
                std::vector<V3> b(loop.begin(), loop.begin() + (long)i);
                b.insert(b.end(), loop.begin() + (long)j, loop.end());
                corner_faces(a, out);
                corner_faces(b, out);
                return;
            }
    if (loop.size() == 3) {
        out.polys.push_back(daiblock::make_poly(loop));
        return;
    }
    V3 mid = v3(0, 0, 0);
    for (size_t k = 0; k < loop.size(); ++k) mid = add(mid, loop[k]);
    mid = daiblock::snap(mul(mid, 1.0 / (double)loop.size()));
    for (size_t k = 0; k < loop.size(); ++k)
        out.polys.push_back(daiblock::make_poly({ mid, loop[k], loop[(k + 1) % loop.size()] }));
}

/* The points that cross one broken edge at one of its ends: P0 on the first
 * face's side, P1 on the second's, and segments-1 points on the arc between
 * them. The arc turns about O, the point the two retreated faces are both
 * `their own offset` away from - for a cube's edge that is the axis of the
 * quarter round an artist expects, and for any other angle it is still the
 * circle through both ends that meets each face tangentially. One segment
 * asks no question: a chamfer is the two ends and nothing between. */
inline void arc_ring(V3 X, V3 P0, V3 P1, V3 n0, V3 n1, V3 d, int seg,
                     std::vector<V3> &out) {
    out.clear();
    out.reserve((size_t)seg + 1);
    out.push_back(P0);
    if (seg > 1) {
        V3 t0 = normalise(cross(n0, d));
        V3 t1 = mul(normalise(cross(n1, d)), -1.0);
        V3 e0 = sub(P0, X), e1 = sub(P1, X);
        double s = 0.5 * (dot(e0, d) + dot(e1, d));
        V3 O = add(add(X, mul(d, s)), add(mul(t0, dot(e0, t0)), mul(t1, dot(e1, t1))));
        V3 r0 = sub(P0, O), r1 = sub(P1, O);
        double l0 = length(r0), l1 = length(r1);
        bool round = l0 > 1e-9 && l1 > 1e-9;
        V3 u0 = round ? mul(r0, 1.0 / l0) : v3(0, 0, 0);
        V3 u1 = round ? mul(r1, 1.0 / l1) : v3(0, 0, 0);
        double cs = round ? dot(u0, u1) : 1.0;
        if (cs > 1.0) cs = 1.0;
        if (cs < -1.0) cs = -1.0;
        double ang = std::acos(cs);
        for (int k = 1; k < seg; ++k) {
            double t = (double)k / (double)seg;
            if (round && ang > 1e-6 && ang < 3.14159) {
                double sa = std::sin(ang);
                V3 dirv = add(mul(u0, std::sin((1.0 - t) * ang) / sa),
                              mul(u1, std::sin(t * ang) / sa));
                out.push_back(add(O, mul(dirv, l0 + (l1 - l0) * t)));
            } else {
                out.push_back(lerp(P0, P1, t));
            }
        }
    }
    out.push_back(P1);
    for (size_t i = 0; i < out.size(); ++i) out[i] = daiblock::snap(out[i]);
}

} /* namespace edge */

/* ---- subdivide ---------------------------------------------------------
 *
 * One level replaces every n-gon with n quads: corner, edge midpoint, face
 * centre, the other edge midpoint. The midpoints are shared with the
 * neighbouring face because they are computed from the same two corners and
 * snapped onto the same micrometre grid - which is what keeps a closed
 * surface closed, and it is checked rather than assumed.
 *
 * FLAT is that division and nothing else: the surface does not move, so the
 * volume is unchanged and only the face count grows. SMOOTH is the same
 * division with Catmull-Clark's three point rules (edge::catmull above), so
 * the cage is pulled towards the limit surface it describes - a cube at level
 * 2 comes out between the cube it was and the sphere it is heading for, which
 * is the shape of the check in the suite. Both paths produce the same faces
 * in the same order; only the points differ. */
inline Solid subdivide(const Solid &in, const Subdiv &p) {
    int level = p.level < 1 ? 1 : (p.level > 3 ? 3 : p.level);
    Solid cur = in;
    for (int l = 0; l < level; ++l) {
        if (p.smooth) { cur = edge::catmull(cur); continue; }
        Solid out;
        out.polys.reserve(cur.polys.size() * 4);
        for (size_t f = 0; f < cur.polys.size(); ++f) {
            const std::vector<V3> &q = cur.polys[f].pts;
            size_t c = q.size();
            if (c < 3) continue;
            V3 centre = v3(0, 0, 0);
            for (size_t i = 0; i < c; ++i) centre = add(centre, q[i]);
            centre = daiblock::snap(mul(centre, 1.0 / (double)c));
            for (size_t i = 0; i < c; ++i) {
                V3 prev = daiblock::snap(mul(add(q[(i + c - 1) % c], q[i]), 0.5));
                V3 next = daiblock::snap(mul(add(q[i], q[(i + 1) % c]), 0.5));
                out.polys.push_back(daiblock::make_poly({ prev, q[i], next, centre }));
            }
        }
        settle(out);
        cur = out;
    }
    return cur;
}

/* ---- bevel -------------------------------------------------------------
 *
 * Three kinds of face come out, and every one of them is placed by TOPOLOGY
 * rather than by a guess about which way is out:
 *
 *   the original faces, each pulled back by p.width along the edges that are
 *   broken and left where it was along the edges that are not;
 *   one strip of p.segments quads across every broken edge, joining the two
 *   retreated faces;
 *   one closing face at every corner where broken edges meet, walked round
 *   the umbrella of faces at that corner.
 *
 * Which edges are broken: those whose two faces disagree by MORE than p.angle
 * degrees (daimod::dihedral()), so a 16 sided cylinder - 22.5 degrees between
 * neighbouring side faces - keeps its round wall at the default 30 and loses
 * only its two rims. An edge with one face, or three, is never broken: there
 * is no second face to bridge to.
 *
 * The one thing that has to be got right for a CSG result is where the
 * retreated corner goes when the surface around it is not one face. A boolean
 * cuts the wall's back face in two along the top of the doorway, so the face
 * that owns the corner beside the reveal knows about ONE of the two edges
 * that corner sits between - and a corner placed from one constraint lands
 * a centimetre off the other one, which is a fold, a warped strip and a
 * chamfer that visibly kinks at the door head.
 *
 * So the corner is not placed per face at all. Every corner belongs to a RUN:
 * the faces met between one broken edge and the next while walking the ring
 * around that point. Every face in a run gets ONE position, and that position
 * is where three planes meet - the two retreated edges that bound the run and
 * the surface the run lies on. Faces inside a run share an unbroken edge, so
 * one point for the run is also what keeps that edge from tearing open.
 *
 * On a cube of side s with one segment that is 6 + 12 + 8 = 26 faces and a
 * volume of s^3 - 6*b^2*s + (16/3)*b^3, both checked in
 * tests/modifier_bevel_cases.hpp against the arithmetic rather than against
 * a previous run. */
inline Solid bevel(const Solid &in, const Bevel &p, int *why) {
    if (why) *why = INERT_NONE;
    if (in.polys.empty() || !(p.width > 0)) return in;
    int seg = p.segments < 1 ? 1 : (p.segments > 4 ? 4 : p.segments);

    Solid src = edge::prepare(in);
    Topology topo = topology_of(src);
    size_t nf = src.polys.size();

    /* --- which edges break ------------------------------------------- */
    std::vector<char> brk(topo.edges.size(), 0);
    size_t nbrk = 0;
    for (size_t i = 0; i < topo.edges.size(); ++i) {
        const Edge &e = topo.edges[i];
        if (e.count != 2 || e.face[0] < 0 || e.face[1] < 0 || e.face[0] == e.face[1]) continue;
        if (dihedral(src, e) > p.angle) { brk[i] = 1; ++nbrk; }
    }
    if (nbrk == 0) {
        /* Not a failure and not silence either: the caller says so in the
         * inspector, so that an entry that changes nothing does not read as
         * an entry that is broken. */
        if (why) *why = INERT_NO_EDGES;
        return in;
    }

    /* --- every loop edge, and where it sits in the topology ----------- */
    std::vector<std::vector<int> > eof(nf);
    std::vector<int> base(nf + 1, 0);
    for (size_t f = 0; f < nf; ++f) {
        const std::vector<V3> &q = src.polys[f].pts;
        size_t c = q.size();
        eof[f].assign(c, -1);
        for (size_t i = 0; i < c; ++i)
            eof[f][i] = topo.find(edge::key_of(q[i]), edge::key_of(q[(i + 1) % c]));
        base[f + 1] = base[f] + (int)c;
    }

    /* --- how far each face may retreat -------------------------------- */
    std::vector<double> wf(nf, 0.0);
    for (size_t f = 0; f < nf; ++f) {
        bool any = false;
        for (size_t i = 0; i < src.polys[f].pts.size(); ++i)
            if (eof[f][i] >= 0 && brk[(size_t)eof[f][i]]) any = true;
        if (any) wf[f] = edge::face_width(src.polys[f], eof[f], brk, p.width);
    }

    /* --- the retreated corners, one position per run --------------------
     *
     * Tried at the full width first. A face that folds through itself at that
     * width - a sliver a boolean left behind, narrower than the chamfer asked
     * for - takes the whole solid back for another go at half of it, rather
     * than that one face retreating less than its neighbours and tearing the
     * strip between them. Eight halvings and the bevel gives up and hands
     * back the shape it was given, which is the honest answer to "there is no
     * room for this chamfer here". */
    std::vector<V3> fin((size_t)base[nf], v3(0, 0, 0));
    bool placed = false;
    for (int attempt = 0; attempt < 8 && !placed; ++attempt) {
        double k = 1.0;
        for (int h = 0; h < attempt; ++h) k *= 0.5;
        std::vector<char> done((size_t)base[nf], 0);
        for (size_t f0 = 0; f0 < nf; ++f0) {
            for (size_t i0 = 0; i0 < src.polys[f0].pts.size(); ++i0) {
                if (done[(size_t)(base[f0] + (int)i0)]) continue;
                V3 X = src.polys[f0].pts[i0];
                std::vector<edge::Step> ring;
                bool ok = edge::umbrella(src, topo, eof, f0, i0, ring);
                for (size_t r = 0; r < ring.size(); ++r) {
                    done[(size_t)(base[(size_t)ring[r].f] + ring[r].i)] = 1;
                    fin[(size_t)(base[(size_t)ring[r].f] + ring[r].i)] = X;
                }
                if (!ok) continue;
                size_t n = ring.size();
                std::vector<size_t> cut;
                for (size_t r = 0; r < n; ++r)
                    if (ring[r].e >= 0 && brk[(size_t)ring[r].e]) cut.push_back(r);
                if (cut.empty()) continue;
                for (size_t t = 0; t < cut.size(); ++t) {
                    size_t a = cut[t], b = cut[(t + 1) % cut.size()];
                    size_t first = (a + 1) % n, last = b;
                    /* The edge that ENDS at this point in the run's first
                     * face, and the one that STARTS at it in its last. */
                    const Poly &pa = src.polys[(size_t)ring[first].f];
                    size_t ia = (size_t)ring[first].i, na = pa.pts.size();
                    V3 ma = edge::retreat_dir(pa, pa.pts[(ia + na - 1) % na], pa.pts[ia]);
                    const Poly &pb = src.polys[(size_t)ring[last].f];
                    size_t ib = (size_t)ring[last].i, nb = pb.pts.size();
                    V3 mb = edge::retreat_dir(pb, pb.pts[ib], pb.pts[(ib + 1) % nb]);
                    double wa = wf[(size_t)ring[first].f] * k;
                    double wb = wf[(size_t)ring[last].f] * k;
                    V3 sn = v3(0, 0, 0);
                    for (size_t r = first;; r = (r + 1) % n) {
                        sn = add(sn, src.polys[(size_t)ring[r].f].normal);
                        if (r == last) break;
                    }
                    V3 nc = length(sn) > 1e-9 ? normalise(sn) : pa.normal;
                    V3 pt;
                    if (!edge::meet3(ma, dot(ma, X) + wa, mb, dot(mb, X) + wb,
                                     nc, dot(nc, X), pt) ||
                        length(sub(pt, X)) > 8.0 * (wa + wb))
                        pt = add(X, mul(add(mul(ma, wa), mul(mb, wb)), 0.5));
                    pt = daiblock::snap(pt);
                    for (size_t r = first;; r = (r + 1) % n) {
                        fin[(size_t)(base[(size_t)ring[r].f] + ring[r].i)] = pt;
                        if (r == last) break;
                    }
                }
            }
        }
        placed = true;
        for (size_t f = 0; f < nf && placed; ++f) {
            std::vector<V3> q(src.polys[f].pts.size());
            for (size_t i = 0; i < q.size(); ++i) q[i] = fin[(size_t)(base[f] + (int)i)];
            if (!edge::inset_ok(src.polys[f], q)) placed = false;
        }
    }
    if (!placed) {
        if (why) *why = INERT_NO_ROOM;
        return in;
    }

    Solid out;
    out.polys.reserve(nf + topo.edges.size() * (size_t)seg + topo.points.size());

    /* --- the retreated faces ------------------------------------------ */
    for (size_t f = 0; f < nf; ++f) {
        std::vector<V3> q(src.polys[f].pts.size());
        for (size_t i = 0; i < q.size(); ++i) q[i] = fin[(size_t)(base[f] + (int)i)];
        edge::emit_face(q, out);
    }

    /* --- one strip of quads across every broken edge -------------------
     *
     * The strip is wound off the FIRST face's own walk of the edge: that face
     * runs u -> v, so whatever is on the other side of it has to run v -> u,
     * and the quad below starts with exactly that. No normal is consulted,
     * which is why a concave edge - the inside of a door reveal - comes out
     * as right as a convex one. */
    std::vector<std::vector<V3> > ring_a(topo.edges.size()), ring_b(topo.edges.size());
    for (size_t i = 0; i < topo.edges.size(); ++i) {
        if (!brk[i]) continue;
        const Edge &e = topo.edges[i];
        size_t f0 = (size_t)e.face[0], f1 = (size_t)e.face[1];
        size_t n0 = src.polys[f0].pts.size(), n1 = src.polys[f1].pts.size();
        size_t c0 = (size_t)e.corner[0], c1 = (size_t)e.corner[1];
        if (c0 >= n0 || c1 >= n1) continue;
        size_t d0 = (c0 + 1) % n0, d1 = (c1 + 1) % n1;
        V3 U = src.polys[f0].pts[c0], V = src.polys[f0].pts[d0];
        bool aligned = edge::key_eq(edge::key_of(src.polys[f1].pts[c1]), edge::key_of(U));
        size_t u1 = aligned ? c1 : d1, v1 = aligned ? d1 : c1;

        V3 dir = sub(V, U);
        double l = length(dir);
        if (l <= 0) continue;
        dir = mul(dir, 1.0 / l);
        V3 n0v = src.polys[f0].normal, n1v = src.polys[f1].normal;
        std::vector<V3> ru, rv;
        edge::arc_ring(U, fin[(size_t)(base[f0] + (int)c0)], fin[(size_t)(base[f1] + (int)u1)],
                       n0v, n1v, dir, seg, ru);
        edge::arc_ring(V, fin[(size_t)(base[f0] + (int)d0)], fin[(size_t)(base[f1] + (int)v1)],
                       n0v, n1v, dir, seg, rv);
        for (int k = 0; k < seg; ++k)
            edge::emit_face({ rv[(size_t)k], ru[(size_t)k],
                              ru[(size_t)k + 1], rv[(size_t)k + 1] }, out);
        /* Keep the rings: the corner faces below are made of them. */
        if (edge::key_eq(edge::key_of(U), e.a)) { ring_a[i] = ru; ring_b[i] = rv; }
        else                                    { ring_a[i] = rv; ring_b[i] = ru; }
    }

    /* --- the face that closes every corner -----------------------------
     *
     * Walk the umbrella: stand on a face at the vertex, step across the edge
     * that leaves it, land on the face on the other side, repeat until back
     * where it started. Collect the face's own corner point and, whenever the
     * edge crossed was broken, the arc points on the way over. The walk runs
     * the same way round as the quads it borders, so the loop is reversed
     * once at the end - that, and not a normal, is what makes the corner face
     * point outward on a concave corner too. */
    std::vector<char> seen((size_t)base[nf], 0);
    for (size_t f0 = 0; f0 < nf; ++f0) {
        for (size_t i0 = 0; i0 < src.polys[f0].pts.size(); ++i0) {
            if (seen[(size_t)(base[f0] + (int)i0)]) continue;
            V3 X = src.polys[f0].pts[i0];
            edge::EKey kx = edge::key_of(X);
            std::vector<V3> loop;
            std::vector<int> touched;
            size_t f = f0, i = i0;
            bool closed = false, anybreak = false;
            for (size_t step = 0; step <= topo.edges.size() + 4; ++step) {
                touched.push_back(base[f] + (int)i);
                loop.push_back(fin[(size_t)(base[f] + (int)i)]);
                size_t c = src.polys[f].pts.size();
                int ei = eof[f][i];
                if (ei < 0) break;
                const Edge &e = topo.edges[(size_t)ei];
                if (e.count != 2 || e.face[0] < 0 || e.face[1] < 0) break;
                size_t g = (size_t)(e.face[0] == (int)f ? e.face[1] : e.face[0]);
                if (g == f) break;
                if (brk[(size_t)ei]) {
                    anybreak = true;
                    const std::vector<V3> &ring =
                        edge::key_eq(kx, e.a) ? ring_a[(size_t)ei] : ring_b[(size_t)ei];
                    if ((int)ring.size() == seg + 1) {
                        if (e.face[0] == (int)f)
                            for (int k = 1; k < seg; ++k) loop.push_back(ring[(size_t)k]);
                        else
                            for (int k = seg - 1; k >= 1; --k) loop.push_back(ring[(size_t)k]);
                    }
                }
                size_t ng = src.polys[g].pts.size();
                size_t cg = (size_t)(e.face[0] == (int)g ? e.corner[0] : e.corner[1]);
                if (cg >= ng) break;
                size_t jv = edge::key_eq(edge::key_of(src.polys[g].pts[cg]), kx)
                                ? cg : (cg + 1) % ng;
                if (!edge::key_eq(edge::key_of(src.polys[g].pts[jv]), kx)) break;
                f = g; i = jv;
                (void)c;
                if (f == f0 && i == i0) { closed = true; break; }
            }
            for (size_t t = 0; t < touched.size(); ++t) seen[(size_t)touched[t]] = 1;
            if (!closed || !anybreak) continue;
            /* The walk runs the same way round as the strips it borders, so
             * the patch that closes it is that loop reversed - that, and not
             * a normal, is what makes the corner face point outward on a
             * concave corner too. */
            std::vector<V3> rev(loop.rbegin(), loop.rend());
            edge::corner_faces(rev, out);
        }
    }

    settle(out);
    return out;
}

} /* namespace daimod */

#endif /* DAI_MODIFIER_EDGE_H */
