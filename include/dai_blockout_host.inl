// MODULE 1 (blockout) OWNS THIS FILE. File scope seam, see include/dai_ext.h.
//
// Included once by every host that draws a scene: examples/editor_demo.cpp and
// tools/modeling_shot.cpp. It is where a blockout node (Box, Cylinder, Stairs,
// Arch, Wedge) and a CSG node turn into an actual mesh the renderer can draw.
//
// The contract this file has to keep:
//
//   * Build the mesh with daimesh/dai_blockout - deterministic, the same
//     fields in give the same triangles out, on every machine.
//   * Cache by CONTENT, not by frame: rebuilding a wall every frame would cost
//     a mesh upload per frame per wall. Rebuild when the fields change.
//   * Attach the result WITHOUT touching the document. The mesh is derived
//     data; writing it back would put a mesh id in the undo stack and in the
//     scene file, and the scene file already has the fields it was built from.
//     dai_scene_set_render() on the entity behind the node is the way in
//     (dai_doc_sync_entity gives the entity).
//   * Release the mesh of a node that stopped being a blockout node, or the
//     renderer leaks one mesh per edit.
//
// Called once per frame, AFTER dai_doc_sync_apply().

#include "dai_blockout.h"
#include "dai_modifier.h"

#include <map>
#include <vector>

namespace daiblockhost {

/* What is on the renderer for one node, and what it was built from. The KEY is
 * a digest of the fields the mesh came out of - the node's own, plus every
 * child that took part in its boolean. Rebuild when that number moves and
 * never otherwise: a wall rebuilt every frame is a mesh upload per frame per
 * wall, and forty walls is the editor going quiet for a second at a time. */
struct Built {
    uint64_t   key = 0;
    dai_mesh   mesh = 0;
    dai_entity entity = 0;
    bool       has_mesh = false;
};

static std::map<dai_node, Built> g_built;

/* The one place the document's fields become a shape. Same rules as the
 * inspector shows and the same as tests/blockout_cases.hpp checks: a zero is
 * "the default", never a zero sized wall. */
static daiblock::Shape shape_of(const dai_node_desc &r) {
    daiblock::Shape s;
    s.kind = r.blockout;
    s.size[0] = r.blockout_size.x > 0 ? r.blockout_size.x : 1.0f;
    s.size[1] = r.blockout_size.y > 0 ? r.blockout_size.y : 1.0f;
    s.size[2] = r.blockout_size.z > 0 ? r.blockout_size.z : 1.0f;
    s.segments = r.blockout_segments > 0 ? r.blockout_segments : 16;
    s.steps = r.blockout_steps > 0 ? r.blockout_steps : 8;
    s.thickness = r.blockout_thickness;
    s.pivot[0] = r.blockout_pivot.x;
    s.pivot[1] = r.blockout_pivot.y;
    s.pivot[2] = r.blockout_pivot.z;
    return s;
}

/* The document's stack, in the shape the geometry reads it. The ONE
 * conversion in the tree, next to shape_of() and for the same reason: two
 * readings of "what does a zero width mean" are two different bevels for one
 * file. What a zero means itself is not decided here - that is bevel_of() and
 * its four siblings in include/dai_modifier.h, and this only carries the
 * numbers across.
 *
 * Entries past modifier_count are not in the stack, whatever they hold: the
 * count is what the inspector's list length is, and a slot below it that was
 * deleted keeps its old numbers until it is filled again. */
static daimod::Mod mod_of(const dai_modifier &m) {
    daimod::Mod o;
    o.type = m.type;
    o.off = m.off;
    o.amount = (double)m.amount;
    o.count = m.count;
    o.angle = (double)m.angle;
    o.axis = m.axis;
    o.flags = (uint32_t)((m.smooth ? DAI_MODF_SMOOTH : 0u) |
                         (m.relative ? DAI_MODF_RELATIVE : 0u));
    o.offset[0] = (double)m.offset.x;
    o.offset[1] = (double)m.offset.y;
    o.offset[2] = (double)m.offset.z;
    o.param = (double)m.param;
    return o;
}

static daimod::Stack stack_of(const dai_node_desc &r) {
    daimod::Stack st;
    int n = r.modifier_count;
    if (n < 0) n = 0;
    if (n > DAI_MODIFIER_MAX) n = DAI_MODIFIER_MAX;
    for (int i = 0; i < n; ++i) {
        if (r.modifiers[i].type == DAI_MOD_NONE) continue;
        st.mods.push_back(mod_of(r.modifiers[i]));
    }
    return st;
}

static void eat(uint64_t &h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}

/* Everything the mesh of `n` depends on, hashed. The child transforms are in
 * here too: moving a doorway is a different wall, and a cache that misses that
 * is a hole that will not move. */
static void hash_node(dai_doc *d, dai_node n, uint64_t &h, int depth) {
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK || depth > 8) return;
    eat(h, &r.blockout, sizeof(r.blockout));
    eat(h, &r.blockout_size, sizeof(r.blockout_size));
    eat(h, &r.blockout_segments, sizeof(r.blockout_segments));
    eat(h, &r.blockout_steps, sizeof(r.blockout_steps));
    eat(h, &r.blockout_thickness, sizeof(r.blockout_thickness));
    eat(h, &r.blockout_pivot, sizeof(r.blockout_pivot));
    eat(h, &r.csg, sizeof(r.csg));
    /* The stack is part of the mesh, so it is part of the key. Through
     * daimod::digest() rather than over the raw array: the digest hashes the
     * entries that actually RUN, in the order they run, so a slot that sits
     * below modifier_count with old numbers in it does not force a rebuild.
     * Only at depth 0: the stack runs on the finished shape, and a CUTTER's
     * own stack is not part of the wall it cuts - a hole is a tool, not a
     * shape (see the mesh build below). */
    if (depth == 0) {
        uint64_t sd = daimod::digest(stack_of(r));
        eat(h, &sd, sizeof(sd));
    }
    if (depth > 0) {
        eat(h, &r.position, sizeof(r.position));
        eat(h, &r.rotation, sizeof(r.rotation));
        eat(h, &r.scale, sizeof(r.scale));
    }
    if (!r.csg) return;
    std::vector<dai_node> kids((size_t)dai_doc_children(d, n, nullptr, 0));
    if (kids.empty()) return;
    dai_doc_children(d, n, kids.data(), (uint32_t)kids.size());
    for (dai_node k : kids) hash_node(d, k, h, depth + 1);
}

/* The solid a node stands for, in ITS OWN space: its shape, with its blockout
 * children folded in by its operation, first child first. A child that is a
 * CSG node itself is resolved the same way, so a wall with a door and that
 * door with a letterbox is one expression and not a special case. */
static daiblock::Solid solid_of(dai_doc *d, dai_node n, int depth) {
    daiblock::Solid out;
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK || depth > 8) return out;
    if (r.blockout) out = daiblock::build(shape_of(r));
    if (!r.csg) return out;

    std::vector<dai_node> kids((size_t)dai_doc_children(d, n, nullptr, 0));
    if (kids.empty()) return out;
    dai_doc_children(d, n, kids.data(), (uint32_t)kids.size());
    for (dai_node k : kids) {
        dai_node_desc c{};
        if (dai_doc_get(d, k, &c) != DAI_OK) continue;
        if (!c.blockout && !c.csg) continue;
        daiblock::Solid cs = solid_of(d, k, depth + 1);
        if (cs.polys.empty()) continue;
        const float pos[3] = { c.position.x, c.position.y, c.position.z };
        const float rot[4] = { c.rotation.x, c.rotation.y, c.rotation.z, c.rotation.w };
        const float scl[3] = { c.scale.x, c.scale.y, c.scale.z };
        cs = daiblock::transform(cs, pos, rot, scl);
        /* The first child of a CSG node with no shape of its own IS the base:
         * subtracting from nothing is nothing, and a node that draws nothing
         * looks exactly like a node that is broken. */
        if (out.polys.empty()) out = cs;
        else                   out = daiblock::csg(out, cs, r.csg);
    }
    return out;
}

/* Which nodes are somebody else's cutters. They stop drawing themselves the
 * moment their parent has an operation - the hole belongs to the wall now. */
static void collect_consumed(dai_doc *d, dai_node n, std::map<dai_node, char> &out, int depth) {
    dai_node_desc r{};
    if (dai_doc_get(d, n, &r) != DAI_OK || !r.csg || depth > 8) return;
    std::vector<dai_node> kids((size_t)dai_doc_children(d, n, nullptr, 0));
    if (kids.empty()) return;
    dai_doc_children(d, n, kids.data(), (uint32_t)kids.size());
    for (dai_node k : kids) {
        dai_node_desc c{};
        if (dai_doc_get(d, k, &c) != DAI_OK) continue;
        if (!c.blockout && !c.csg) continue;
        out[k] = 1;
        collect_consumed(d, k, out, depth + 1);
    }
}

/* What dai_doc_sync gave the entity as its render scale - mirrored here, one
 * for one, from render_scale_of() in src/dai_doc_sync.cpp. A blockout mesh is
 * built in metres, so it must be divided by that scale or a 4 m wall on a node
 * with a half metre collider is drawn 2 m long. A node added through Add
 * Component carries render_extent 1,1,1 and the division is exactly 1. */
static dai_vec3 entity_render_scale(const dai_node_desc &r, dai_vec3 ws) {
    dai_vec3 he = r.render_extent;
    if (he.x == 0.0f && he.y == 0.0f && he.z == 0.0f) he = r.half_extent;
    int shape = r.shape;
    switch (r.mesh) {
    case DAI_MESH_SPHERE:   shape = DAI_SHAPE_SPHERE; break;
    case DAI_MESH_CAPSULE:  shape = DAI_SHAPE_CAPSULE; break;
    case DAI_MESH_CYLINDER: shape = DAI_SHAPE_CYLINDER; break;
    case DAI_MESH_BOX:      shape = DAI_SHAPE_BOX; break;
    default: break;
    }
    float ax = std::fabs(ws.x), ay = std::fabs(ws.y), az = std::fabs(ws.z);
    switch (shape) {
    case DAI_SHAPE_SPHERE:
    case DAI_SHAPE_CAPSULE:  return dai_vec3{ he.x * ax, he.x * ax, he.x * ax };
    case DAI_SHAPE_CYLINDER: return dai_vec3{ he.x * ax, he.y * ay, he.x * ax };
    default:                 return dai_vec3{ he.x * ax, he.y * ay, he.z * az };
    }
}

} /* namespace daiblockhost */

static void dai_blockout_host_sync(const dai_ext_host *h) {
    if (!h || !h->doc || !h->sync || !h->scene || !h->renderer) return;
    dai_doc *d = h->doc;

    uint32_t count = dai_doc_count(d);
    std::vector<dai_node> ids((size_t)count);
    if (count) dai_doc_nodes(d, ids.data(), count);

    std::map<dai_node, char> consumed;
    for (dai_node n : ids) daiblockhost::collect_consumed(d, n, consumed, 0);

    std::map<dai_node, char> alive;

    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(d, n, &r) != DAI_OK) continue;
        /* A cutter draws nothing of its own; its parent draws the result. The
         * document is not touched for this - visibility of a derived shape is
         * not an edit, and an undo step per frame would be. */
        if (consumed.count(n)) {
            dai_entity ce = dai_doc_sync_entity(h->sync, n);
            if (ce) dai_scene_set_visible(h->scene, ce, 0);
            continue;
        }
        if (!r.blockout && !r.csg) continue;

        alive[n] = 1;
        daiblockhost::Built &b = daiblockhost::g_built[n];
        dai_entity e = dai_doc_sync_entity(h->sync, n);
        if (!e) continue;

        uint64_t key = 1469598103934665603ull;
        daiblockhost::hash_node(d, n, key, 0);
        /* The world scale is in the key as well: the mesh is divided by the
         * entity's render scale, so a scaled node is a different upload. */
        dai_vec3 wp{}, ws{ 1, 1, 1 };
        dai_quat wr{ 0, 0, 0, 1 };
        dai_doc_world_transform(d, n, &wp, &wr, &ws);
        daiblockhost::eat(key, &ws, sizeof(ws));
        daiblockhost::eat(key, &r.render_extent, sizeof(r.render_extent));
        daiblockhost::eat(key, &r.half_extent, sizeof(r.half_extent));
        daiblockhost::eat(key, &r.shape, sizeof(r.shape));

        if (!b.has_mesh || b.key != key) {
            daiblock::Solid sol = daiblockhost::solid_of(d, n, 0);
            /* The stack, on the solid the CSG produced and before anything is
             * turned into triangles: a bevel has to see the door reveal the
             * boolean cut, not the triangles it was drawn with. finalise()
             * through daimod so a subdivide that asked for smooth gets its
             * normals averaged - the positions are the same either way. */
            daimod::Result res = daimod::apply(daiblockhost::stack_of(r), sol);
            daiblock::Mesh m = daimod::finalise(res);
            if (!m.idx.empty()) {
                dai_vec3 es = daiblockhost::entity_render_scale(r, ws);
                float sx = es.x != 0.0f ? ws.x / es.x : 1.0f;
                float sy = es.y != 0.0f ? ws.y / es.y : 1.0f;
                float sz = es.z != 0.0f ? ws.z / es.z : 1.0f;
                if (sx != 1.0f || sy != 1.0f || sz != 1.0f)
                    for (dai_vertex &v : m.verts)
                        v.position = dai_vec3{ v.position.x * sx, v.position.y * sy,
                                               v.position.z * sz };
                dai_mesh fresh = dai_render_mesh_create(h->renderer, m.verts.data(),
                                                        (uint32_t)m.verts.size(),
                                                        m.idx.data(), (uint32_t)m.idx.size());
                if (fresh >= DAI_MESH_BUILTIN_COUNT) {
                    if (b.has_mesh && b.mesh >= DAI_MESH_BUILTIN_COUNT)
                        dai_render_mesh_destroy(h->renderer, b.mesh);
                    b.mesh = fresh;
                    b.has_mesh = true;
                    b.key = key;
                    b.entity = 0;         /* force the attach below */
                }
            }
        }

        /* Attached through set_render, not through parts: parts carry their
         * own material and would cut the material seam out of the picture.
         * The document's own mesh field stays 0xFFFFFFFF, which is what makes
         * dai_doc_sync's own set_render leave this mesh alone. */
        if (b.has_mesh && (b.entity != e)) {
            dai_scene_set_render(h->scene, e, b.mesh, r.roughness, r.emissive, r.render_flags);
            b.entity = e;
        } else if (b.has_mesh) {
            dai_scene_set_render(h->scene, e, b.mesh, r.roughness, r.emissive, r.render_flags);
        }
        dai_scene_set_visible(h->scene, e, !(r.hidden || r.disabled));
    }

    /* A node that stopped being a blockout node gives its mesh back. Without
     * this the renderer keeps one mesh per edit for the rest of the session. */
    for (auto it = daiblockhost::g_built.begin(); it != daiblockhost::g_built.end(); ) {
        if (alive.count(it->first)) { ++it; continue; }
        if (it->second.has_mesh && it->second.mesh >= DAI_MESH_BUILTIN_COUNT)
            dai_render_mesh_destroy(h->renderer, it->second.mesh);
        it = daiblockhost::g_built.erase(it);
    }
}
