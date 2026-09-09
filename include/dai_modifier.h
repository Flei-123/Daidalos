/*
 * The modifier stack - detail that comes from a RULE, not from a mouse.
 *
 * A blockout is a rough shape: six faces, eight corners, and every one of them
 * typed rather than sculpted. What turns that into something worth looking at
 * is not a hundred hand placed vertices, it is a short list of operations that
 * are re-run whenever the rough shape moves: break the edges, divide the
 * faces, give the plane a thickness, repeat it, mirror it. Blender calls that
 * list the modifier stack and so does this file.
 *
 * The stack is DATA on the node (dai_modifier[] in dai_node_desc), so it is
 * serialised with the scene, it rides the same undo step as every other field,
 * and the mesh it produces is derived - never written back. Same three
 * promises as include/dai_blockout.h, and for the same reasons:
 *
 *   DETERMINISTIC   the same stack on the same solid gives the same triangles
 *                   in the same order, byte for byte. Every container that
 *                   decides an order here is a std::map or a sorted vector;
 *                   nothing iterates a hash and nothing compares a pointer.
 *   CLOSED          a modifier that is handed a closed surface returns a
 *                   closed surface. Solidify is the one that is allowed to
 *                   turn an OPEN surface into a closed one - that is its job.
 *   MEASURABLE      the result goes back through daiblock::volume(),
 *                   open_edges(), degenerate_triangles() and digest(), so
 *                   "the bevel took the right amount off" is arithmetic a
 *                   test does, not a screenshot somebody looked at.
 *
 * A header rather than a source file under src/ for the reason
 * include/dai_blockout.h gives at the top: build.sh names every translation
 * unit it compiles and build.sh is frozen. Everything here is `inline`.
 *
 * WHERE THE CODE LIVES - three files, so three people can work at once:
 *
 *   include/dai_modifier.h        THIS file: the parameter structs, the
 *                                 topology the geometry operators read, the
 *                                 stack, apply(), and the two ways a result
 *                                 becomes triangles (flat and smooth).
 *   include/dai_modifier_edge.h   bevel() and subdivide().
 *   include/dai_modifier_dup.h    solidify(), array() and mirror().
 *
 * Both of those are included at the END of this file - one
 * `#include "dai_modifier.h"` still gets everything, exactly the way
 * dai_blockout.h pulls in dai_blockout_csg.h.
 *
 * Units are metres and angles are DEGREES at the edge of this API, because
 * that is what the inspector shows and what a script types. Radians exist
 * inside a function and never leave it.
 */
#ifndef DAI_MODIFIER_H
#define DAI_MODIFIER_H

#include "dai_blockout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

namespace daimod {

using daiblock::Mesh;
using daiblock::Poly;
using daiblock::Solid;
using daiblock::V3;
using daiblock::add;
using daiblock::cross;
using daiblock::dot;
using daiblock::length;
using daiblock::lerp;
using daiblock::mul;
using daiblock::normalise;
using daiblock::sub;
using daiblock::v3;

/* Which modifier. Mirrors dai_modifier_type in dai_doc.h - the document stores
 * the number, this file is what the number means. The numbers are the file
 * format, so they never move. */
enum Type {
    MOD_NONE = 0,
    MOD_BEVEL,
    MOD_SUBDIVIDE,
    MOD_SOLIDIFY,
    MOD_ARRAY,
    MOD_MIRROR
};

/* Why an operator handed its input straight back.
 *
 * A modifier that changes nothing is not a broken modifier - a bevel finds no
 * edge sharp enough, or no room for the chamfer it was asked for, and the
 * honest answer to both is the shape it was given. Honest, but INVISIBLE: the
 * entry sits in the list with its numbers and the viewport looks exactly as it
 * did, which reads as "the stack is broken" to everyone who did not write it.
 * So the reason travels out with the result and the inspector says it. */
enum Inert {
    INERT_NONE     = 0,
    INERT_NO_EDGES = 1,   /* nothing disagreed by more than the angle threshold */
    INERT_NO_ROOM  = 2    /* every width down to 1/128th folded a face          */
};

/* The one sentence each reason is worth, for the inspector and the console. */
inline const char *inert_text(int why) {
    switch (why) {
    case INERT_NO_EDGES: return "no edge over the angle";
    case INERT_NO_ROOM:  return "no room for this width";
    default:             return "";
    }
}

/* The flag bits a modifier carries. Mirrors DAI_MODF_* in dai_doc.h. */
enum Flags {
    FLAG_SMOOTH   = 0x1u,   /* subdivide: average the normals at the corners  */
    FLAG_RELATIVE = 0x2u    /* array: the offset is a MULTIPLE of the shape's
                               own bounding size per axis, not metres         */
};

/* ---- what each modifier is made of ------------------------------------
 *
 * Five small structs rather than one struct with a mode: a bevel has a width
 * and a subdivide has a level, and a single struct where half the fields are
 * meaningless is how a test ends up asserting a number nobody reads. The
 * document's dai_modifier IS the flat one - it has to be, it is a file format
 * - and mod_of() below is the single place that unpacks it. */

struct Bevel {
    double width = 0.02;    /* how far back from the edge, in metres, per face */
    int    segments = 1;    /* 1 = a flat chamfer, 2..4 = a rounded edge       */
    double angle = 30.0;    /* only edges whose faces disagree by MORE than
                               this many degrees are broken. A cube's edges
                               are 90, a subdivided sphere's are 5.           */
};

struct Subdiv {
    int  level = 1;         /* 1..3 - each level quarters every face          */
    bool smooth = false;    /* true: the corners of the new faces are pulled
                               towards the average of their neighbours and
                               the normals are averaged, so the surface reads
                               as curved. false: the division is exact and the
                               shape does not move at all.                    */
};

struct Solidify {
    double thickness = 0.05; /* the wall this gives the surface, in metres    */
    double shift = 0.0;      /* -1 all of it INWARD (against the normal),
                                0 half each way, +1 all of it OUTWARD         */
};

struct Array {
    int    count = 2;              /* how many copies IN TOTAL, 1 = unchanged */
    double offset[3] = { 1, 0, 0 };/* step between copies                     */
    bool   relative = false;       /* offset is a multiple of the bounding
                                      size per axis rather than metres        */
    double rotation = 0.0;         /* degrees added per copy                  */
    int    axis = 1;               /* 0 X, 1 Y, 2 Z - which axis it turns on  */
};

struct Mirror {
    int    axis = 0;        /* 0 X, 1 Y, 2 Z - the LOCAL axis it flips on     */
    double weld = 0.001;    /* points closer than this to the plane are moved
                               ONTO it, so the two halves join instead of
                               leaving a seam a micrometre wide              */
};

/* One entry of the stack, in the shape the document holds it. `off` is the
 * tick box in the inspector: an entry that is off is skipped and changes
 * nothing at all - which is a test, not a hope. */
struct Mod {
    int      type = MOD_NONE;
    int      off = 0;
    double   amount = 0;     /* bevel width | solidify thickness | mirror weld */
    int      count = 0;      /* bevel segments | subdiv level | array copies   */
    double   angle = 0;      /* bevel threshold | array degrees per copy       */
    int      axis = 0;       /* mirror axis | array rotation axis              */
    uint32_t flags = 0;      /* FLAG_*                                         */
    double   offset[3] = { 0, 0, 0 };   /* array step                          */
    double   param = 0;      /* solidify shift -1..1                           */
};

struct Stack {
    std::vector<Mod> mods;
};

/* ---- the defaults, in ONE place ---------------------------------------
 *
 * Zero means "the default" everywhere in this codebase (see dai_doc.h), and a
 * modifier read out of a scene file written before a field existed arrives
 * zeroed. These five readers are what a zero means, and the inspector, the
 * bridge, the host and the tests all go through them - two readings of "what
 * does width 0 mean" is two different bevels for the same file. */

inline Bevel bevel_of(const Mod &m) {
    Bevel b;
    b.width = m.amount > 0 ? m.amount : 0.02;
    b.segments = m.count > 0 ? m.count : 1;
    if (b.segments > 4) b.segments = 4;
    b.angle = m.angle > 0 ? m.angle : 30.0;
    return b;
}

inline Subdiv subdiv_of(const Mod &m) {
    Subdiv s;
    s.level = m.count > 0 ? m.count : 1;
    if (s.level > 3) s.level = 3;
    s.smooth = (m.flags & FLAG_SMOOTH) != 0;
    return s;
}

inline Solidify solidify_of(const Mod &m) {
    Solidify s;
    s.thickness = m.amount > 0 ? m.amount : 0.05;
    s.shift = m.param < -1 ? -1 : (m.param > 1 ? 1 : m.param);
    return s;
}

inline Array array_of(const Mod &m) {
    Array a;
    a.count = m.count > 0 ? m.count : 2;
    bool any = m.offset[0] != 0 || m.offset[1] != 0 || m.offset[2] != 0;
    a.offset[0] = any ? m.offset[0] : 1.0;
    a.offset[1] = any ? m.offset[1] : 0.0;
    a.offset[2] = any ? m.offset[2] : 0.0;
    a.relative = (m.flags & FLAG_RELATIVE) != 0;
    a.rotation = m.angle;
    a.axis = (m.axis >= 0 && m.axis <= 2) ? m.axis : 1;
    return a;
}

inline Mirror mirror_of(const Mod &m) {
    Mirror r;
    r.axis = (m.axis >= 0 && m.axis <= 2) ? m.axis : 0;
    r.weld = m.amount > 0 ? m.amount : 0.001;
    return r;
}

/* ---- the topology the geometry operators read --------------------------
 *
 * A Solid is a bag of convex faces that happen to touch. A bevel needs to know
 * WHICH faces touch along which edge and by how much they disagree; a solidify
 * needs to know which edges have only one face (the border of an open surface)
 * and a mirror needs to know nothing but is welcome to look. Rebuilding that
 * by hand in three files is three chances to build it differently, so it is
 * built once, here.
 *
 * Keyed by the SNAPPED position, like every other identity question in
 * dai_blockout.h: the same corner reached from two faces is the same corner
 * only if the two floats agree to the micrometre, and after a rotation they
 * do not. Edges come out sorted by their key pair, which is what makes the
 * order - and therefore every mesh built from it - deterministic. */

typedef daiblock::detail::Key Key;

struct Edge {
    Key a, b;                 /* a < b, always: the edge has no direction here */
    int face[2] = { -1, -1 }; /* which polygons own it, -1 = nobody (yet)      */
    int corner[2] = { -1, -1 };/* the index IN that polygon of the first point
                                  of this edge, so the loop can be walked back */
    int count = 0;            /* how many faces claimed it: 1 = a border edge,
                                 2 = interior, 3+ = the solid is broken        */
};

struct Topology {
    std::vector<Edge>            edges;   /* sorted by (a, b)                  */
    std::map<std::pair<Key, Key>, int> index;   /* (a,b) -> position in edges  */
    /* Every distinct corner position, sorted, and which edges meet there. */
    std::vector<V3>              points;
    std::map<Key, int>           point_index;

    int find(Key a, Key b) const {
        if (b < a) { Key t = a; a = b; b = t; }
        std::map<std::pair<Key, Key>, int>::const_iterator it = index.find(std::make_pair(a, b));
        return it == index.end() ? -1 : it->second;
    }
    int point(Key k) const {
        std::map<Key, int>::const_iterator it = point_index.find(k);
        return it == point_index.end() ? -1 : it->second;
    }
};

inline Topology topology_of(const Solid &s) {
    /* Two passes over a std::map, then one copy out: the map decides the
     * order, and a map is sorted, so the answer does not depend on the order
     * the faces happened to be built in. */
    std::map<std::pair<Key, Key>, Edge> bykey;
    std::map<Key, V3> pts;
    for (size_t f = 0; f < s.polys.size(); ++f) {
        const Poly &p = s.polys[f];
        size_t c = p.pts.size();
        for (size_t i = 0; i < c; ++i) {
            Key ka = daiblock::detail::key_of(p.pts[i]);
            Key kb = daiblock::detail::key_of(p.pts[(i + 1) % c]);
            pts[ka] = p.pts[i];
            if (!(ka < kb) && !(kb < ka)) continue;         /* zero length */
            Key a = ka, b = kb;
            if (b < a) { Key t = a; a = b; b = t; }
            Edge &e = bykey[std::make_pair(a, b)];
            e.a = a; e.b = b;
            if (e.count < 2) { e.face[e.count] = (int)f; e.corner[e.count] = (int)i; }
            ++e.count;
        }
    }
    Topology t;
    t.edges.reserve(bykey.size());
    for (std::map<std::pair<Key, Key>, Edge>::const_iterator it = bykey.begin();
         it != bykey.end(); ++it) {
        t.index[it->first] = (int)t.edges.size();
        t.edges.push_back(it->second);
    }
    t.points.reserve(pts.size());
    for (std::map<Key, V3>::const_iterator it = pts.begin(); it != pts.end(); ++it) {
        t.point_index[it->first] = (int)t.points.size();
        t.points.push_back(it->second);
    }
    return t;
}

/* How far the two faces of an edge disagree, in degrees: 0 for two faces in
 * one plane, 90 for the edge of a cube, 180 for a fold back on itself. A
 * border edge (one face) answers 180 - it is as sharp as an edge gets, and a
 * bevel that is asked to break sharp edges should break it. */
inline double dihedral(const Solid &s, const Edge &e) {
    if (e.count < 2 || e.face[0] < 0 || e.face[1] < 0) return 180.0;
    V3 n0 = s.polys[(size_t)e.face[0]].normal;
    V3 n1 = s.polys[(size_t)e.face[1]].normal;
    double d = dot(n0, n1);
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return std::acos(d) * 180.0 / 3.14159265358979323846;
}

/* The axis aligned box a SOLID fills - daiblock::bounds() answers the same
 * question about a Mesh, and the array needs it before there are triangles. */
inline void bounds(const Solid &s, V3 *lo, V3 *hi) {
    V3 a = v3(0, 0, 0), b = v3(0, 0, 0);
    bool first = true;
    for (size_t i = 0; i < s.polys.size(); ++i)
        for (size_t j = 0; j < s.polys[i].pts.size(); ++j) {
            const V3 &p = s.polys[i].pts[j];
            if (first) { a = p; b = p; first = false; continue; }
            if (p.x < a.x) a.x = p.x;
            if (p.y < a.y) a.y = p.y;
            if (p.z < a.z) a.z = p.z;
            if (p.x > b.x) b.x = p.x;
            if (p.y > b.y) b.y = p.y;
            if (p.z > b.z) b.z = p.z;
        }
    if (lo) *lo = a;
    if (hi) *hi = b;
}

/* Snaps every point of a solid onto the micrometre grid and recomputes the
 * face normals - what daiblock::build() does at the end of a generator, and
 * what every modifier does at the end of itself. Two operators that snap
 * differently weld differently, and a surface welded two ways is open. */
inline void settle(Solid &s) {
    for (size_t i = 0; i < s.polys.size(); ++i) {
        for (size_t j = 0; j < s.polys[i].pts.size(); ++j)
            s.polys[i].pts[j] = daiblock::snap(s.polys[i].pts[j]);
        s.polys[i].normal = daiblock::poly_normal(s.polys[i]);
    }
}

/* ---- the geometry operators -------------------------------------------
 *
 * Declared here, defined in the two headers included at the bottom. Each takes
 * a solid and returns a new one; none of them touches a document, a renderer
 * or a clock. */
/* `why`, when it is given, comes back as one of the Inert values above: 0 when
 * the bevel really ran. An old caller that passes nothing is unchanged. */
inline Solid bevel(const Solid &in, const Bevel &p, int *why);
inline Solid bevel(const Solid &in, const Bevel &p) { return bevel(in, p, 0); }
inline Solid subdivide(const Solid &in, const Subdiv &p);
inline Solid solidify(const Solid &in, const Solidify &p);
inline Solid array(const Solid &in, const Array &p);
inline Solid mirror(const Solid &in, const Mirror &p);

/* ---- running the stack -------------------------------------------------
 *
 * In order, first entry first, each one fed the output of the one before. That
 * ORDER is the whole point of a stack and it is visible in the result: an
 * array of a bevelled step is nine bevelled steps, a bevel of an arrayed step
 * is one long bevel down the joins. The suite checks both and they differ. */

struct Result {
    Solid solid;
    bool  smooth = false;   /* a subdivide asked for averaged normals */
    /* One entry per entry of the stack, in stack order: INERT_NONE when the
     * entry ran (or was switched off and never asked to), one of the other
     * Inert values when it gave the shape back untouched. An empty vector is
     * "nothing to report", which is what an old caller sees. */
    std::vector<int> inert;

    int  inert_at(size_t i) const { return i < inert.size() ? inert[i] : (int)INERT_NONE; }
    bool any_inert() const {
        for (size_t i = 0; i < inert.size(); ++i) if (inert[i]) return true;
        return false;
    }
};

inline Result apply(const Stack &st, const Solid &base) {
    Result out;
    out.solid = base;
    out.inert.assign(st.mods.size(), (int)INERT_NONE);
    for (size_t i = 0; i < st.mods.size(); ++i) {
        const Mod &m = st.mods[i];
        if (m.off || m.type == MOD_NONE) continue;
        if (out.solid.polys.empty()) break;
        switch (m.type) {
        case MOD_BEVEL: {
            int why = INERT_NONE;
            out.solid = bevel(out.solid, bevel_of(m), &why);
            out.inert[i] = why;
            break;
        }
        case MOD_SUBDIVIDE: {
            Subdiv s = subdiv_of(m);
            out.solid = subdivide(out.solid, s);
            if (s.smooth) out.smooth = true;
            break;
        }
        case MOD_SOLIDIFY:  out.solid = solidify(out.solid, solidify_of(m)); break;
        case MOD_ARRAY:     out.solid = array(out.solid, array_of(m)); break;
        case MOD_MIRROR:    out.solid = mirror(out.solid, mirror_of(m)); break;
        default: break;
        }
    }
    return out;
}

/* ---- from a result to triangles ----------------------------------------
 *
 * Flat is daiblock::finalise() and nothing else - a blockout is read by its
 * edges. Smooth is the same triangles with the normals averaged over the faces
 * that meet at each welded corner: the positions are IDENTICAL, only the
 * normals move, so a smooth mesh is still exactly as closed as the flat one
 * and the volume is the same number. */

inline Mesh finalise_smooth(const Solid &sol) {
    Mesh m = daiblock::finalise(sol);
    /* Sum the face normals per welded position, weighted by the triangle's
     * area - the standard weighting, and the one that stops a corner with
     * three tiny triangles on one side from leaning that way. std::map, so
     * the accumulation order is the sorted order and not the vertex order. */
    std::map<Key, V3> acc;
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3) {
        const dai_vec3 &pa = m.verts[m.idx[i]].position;
        const dai_vec3 &pb = m.verts[m.idx[i + 1]].position;
        const dai_vec3 &pc = m.verts[m.idx[i + 2]].position;
        V3 a = v3(pa.x, pa.y, pa.z), b = v3(pb.x, pb.y, pb.z), c = v3(pc.x, pc.y, pc.z);
        V3 g = cross(sub(b, a), sub(c, a));          /* length = 2 * area */
        for (int j = 0; j < 3; ++j) {
            const dai_vec3 &p = m.verts[m.idx[i + (size_t)j]].position;
            Key k = daiblock::detail::key_of(v3(p.x, p.y, p.z));
            std::map<Key, V3>::iterator it = acc.find(k);
            if (it == acc.end()) acc[k] = g;
            else it->second = add(it->second, g);
        }
    }
    for (size_t i = 0; i < m.verts.size(); ++i) {
        const dai_vec3 &p = m.verts[i].position;
        Key k = daiblock::detail::key_of(v3(p.x, p.y, p.z));
        std::map<Key, V3>::const_iterator it = acc.find(k);
        if (it == acc.end()) continue;
        if (length(it->second) < 1e-12) continue;    /* a fold: keep the flat one */
        V3 n = normalise(it->second);
        m.verts[i].normal = dai_vec3{ (float)n.x, (float)n.y, (float)n.z };
    }
    return m;
}

inline Mesh finalise(const Result &r) {
    return r.smooth ? finalise_smooth(r.solid) : daiblock::finalise(r.solid);
}

/* The whole pipeline in one call, for the host and for the tests. */
inline Mesh build(const Stack &st, const Solid &base) { return finalise(apply(st, base)); }

/* ---- what the last build made of a node's stack -------------------------
 *
 * A modifier that ran and handed its input straight back (Inert above) is a
 * correct answer that looks exactly like a broken one: the entry sits in the
 * list with its numbers and the viewport does not move. So the host that
 * rebuilds the mesh writes what came back here, two bits per entry - the
 * Inert value of entry i in bits 2*i, 2*i+1 - and the inspector reads it while
 * it draws the list.
 *
 * A map behind an inline function rather than an API on the editor: the host
 * seam (include/dai_blockout_host.inl) is compiled into programs that do NOT
 * link the editor UI - tests/test_doc.cpp is one - and a report is not worth a
 * link error. One inline function is one map per program, which is what both
 * sides need, and a headless host simply writes into a map nobody reads.
 *
 * The reason is also printed ONCE per change, to stdout, which is the editor's
 * Console panel. Printed every frame it would be the loudest thing in the
 * program and the first thing anybody turned off. */
inline std::map<uint32_t, uint32_t> &inert_report() {
    static std::map<uint32_t, uint32_t> m;
    return m;
}

inline uint32_t inert_of(uint32_t node) {
    std::map<uint32_t, uint32_t>::const_iterator it = inert_report().find(node);
    return it == inert_report().end() ? 0u : it->second;
}

inline void report_inert(uint32_t node, uint32_t mask) {
    if (!node) return;
    std::map<uint32_t, uint32_t> &m = inert_report();
    std::map<uint32_t, uint32_t>::iterator it = m.find(node);
    uint32_t was = it == m.end() ? 0u : it->second;
    if (was == mask) return;
    if (!mask) { if (it != m.end()) m.erase(it); return; }
    m[node] = mask;
    for (int i = 0; i < 16; ++i) {
        uint32_t why = (mask >> (i * 2)) & 3u;
        if (!why || why == ((was >> (i * 2)) & 3u)) continue;
        std::printf("modifier %d on node %u changed nothing: %s\n", i + 1,
                    (unsigned)node, inert_text((int)why));
    }
    std::fflush(stdout);
}

/* A fingerprint of the STACK, for the host's rebuild cache: the same number
 * means the same modifiers and therefore the same mesh, so nothing is rebuilt.
 * FNV-1a over the fields, in list order - the order IS part of the identity. */
inline uint64_t digest(const Stack &st) {
    uint64_t h = 1469598103934665603ull;
    struct E {
        static void eat(uint64_t &x, const void *p, size_t n) {
            const unsigned char *b = (const unsigned char *)p;
            for (size_t i = 0; i < n; ++i) { x ^= b[i]; x *= 1099511628211ull; }
        }
    };
    for (size_t i = 0; i < st.mods.size(); ++i) {
        const Mod &m = st.mods[i];
        E::eat(h, &m.type, sizeof(m.type));
        E::eat(h, &m.off, sizeof(m.off));
        E::eat(h, &m.amount, sizeof(m.amount));
        E::eat(h, &m.count, sizeof(m.count));
        E::eat(h, &m.angle, sizeof(m.angle));
        E::eat(h, &m.axis, sizeof(m.axis));
        E::eat(h, &m.flags, sizeof(m.flags));
        E::eat(h, m.offset, sizeof(m.offset));
        E::eat(h, &m.param, sizeof(m.param));
    }
    return h;
}

} /* namespace daimod */

#include "dai_modifier_dup.h"
#include "dai_modifier_edge.h"

#endif /* DAI_MODIFIER_H */
