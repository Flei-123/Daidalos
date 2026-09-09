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

/* ---- subdivide ---------------------------------------------------------
 *
 * One level replaces every n-gon with n quads: corner, edge midpoint, face
 * centre, the other edge midpoint. The midpoints are shared with the
 * neighbouring face because they are computed from the same two corners and
 * snapped onto the same micrometre grid - which is what keeps a closed
 * surface closed, and it is checked rather than assumed.
 *
 * SKELETON: this is the FLAT division - the surface does not move, so the
 * volume is unchanged and only the face count grows. `p.smooth` is read by
 * daimod::apply() (it decides how the normals are averaged at the end) but
 * the Catmull-Clark point rules that pull the cage towards the limit surface
 * are B1's work and belong exactly here. Until they land, smooth means
 * "smooth shaded", not "smooth shaped". */
inline Solid subdivide(const Solid &in, const Subdiv &p) {
    int level = p.level < 1 ? 1 : (p.level > 3 ? 3 : p.level);
    Solid cur = in;
    for (int l = 0; l < level; ++l) {
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
 * THE ONE HOLE IN THE SKELETON, and it is B1's job. It returns the shape
 * unchanged today, which is honest - a bevel that half works is worse than
 * one that says it is not there yet, because the first kind ships.
 *
 * What it has to do, and what the suite will ask of it:
 *
 *   - Break only the edges whose two faces disagree by MORE than p.angle
 *     degrees (daimod::dihedral() answers that), so a subdivided cylinder
 *     keeps its round side and loses only its rim.
 *   - Every face shrinks back by p.width along its own plane; every broken
 *     edge becomes p.segments quads across; every corner where broken edges
 *     meet becomes a face that closes the gap those quads leave.
 *   - On a cube with one segment that is 12 edge faces and 8 corner faces on
 *     top of the 6 shrunken originals, and a volume smaller by an amount the
 *     test works out on paper rather than reads off the result.
 *   - It has to survive a CSG result: the door reveal of a wall with a hole
 *     in it is a concave edge loop, and a chamfer that runs off the end of a
 *     face makes a self intersection nobody sees until the exporter does.
 *     Degenerate triangles: zero. Open edges: zero, where there were none.
 *
 * The topology it needs is in include/dai_modifier.h - topology_of(),
 * dihedral(), settle(). Everything private to the bevel goes in a
 * `namespace edge { }` block in THIS file. */
inline Solid bevel(const Solid &in, const Bevel &p) {
    (void)p;
    return in;
}

} /* namespace daimod */

#endif /* DAI_MODIFIER_EDGE_H */
