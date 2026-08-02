// Reconciles a live scene against the document. See include/dai_doc.h.
//
// The document is the truth; this file makes the running world agree with it
// again, touching as little as possible. "As little as possible" is the whole
// point: a full rebuild every frame would reset physics constantly and make
// play mode useless, so each node carries a revision and only nodes whose
// revision moved are re-applied.

#include "dai_doc.h"
#include "dai_doc_internal.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace daidoc;

namespace {

struct Live {
    dai_entity    entity = DAI_INVALID_ENTITY;
    dai_body      body = DAI_INVALID_BODY;
    uint64_t      rev = 0;
    dai_node_desc built{};      // the record the body was created from
    // What the resolver answered when this entity was built. The SAME path can
    // answer differently later - an asset loads asynchronously, so the first
    // answer is usually "not yet". Keeping it is what lets apply() notice.
    std::vector<dai_render_part> res_parts;
};

// Changing any of these means the rigid body itself is wrong, not just its
// pose - Jolt shapes are immutable once created, so the body is rebuilt.
bool needs_rebuild(const dai_node_desc &a, const dai_node_desc &b) {
    if (std::strcmp(a.asset, b.asset) != 0) return true;   // different model entirely
    // The mesh size and the collider offset are baked into the entity at spawn
    // time (render_scale / render_offset), so they need the same rebuild the
    // collision shape does.
    if (std::memcmp(&a.render_extent, &b.render_extent, sizeof(dai_vec3)) != 0) return true;
    if (std::memcmp(&a.collider_center, &b.collider_center, sizeof(dai_vec3)) != 0) return true;
    return a.shape != b.shape || a.motion != b.motion || a.no_body != b.no_body ||
           a.no_collider != b.no_collider || a.no_rigidbody != b.no_rigidbody ||
           a.trigger != b.trigger ||
           a.no_sleeping != b.no_sleeping ||
           std::memcmp(&a.half_extent, &b.half_extent, sizeof(dai_vec3)) != 0 ||
           std::memcmp(&a.scale, &b.scale, sizeof(dai_vec3)) != 0 ||
           a.density != b.density || a.friction != b.friction ||
           a.restitution != b.restitution;
}

} // namespace

struct dai_doc_sync {
    dai_doc   *doc = nullptr;
    dai_scene *scene = nullptr;
    dai_world *world = nullptr;
    dai_asset_resolve_fn resolve_asset = nullptr;
    void                *resolve_user = nullptr;
    std::unordered_map<dai_node, Live>   live;
    std::unordered_map<dai_entity, dai_node> by_entity;
    std::unordered_map<dai_body, dai_node>   by_body;
    bool zero_velocities = false;
};

namespace {

void forget(dai_doc_sync *s, dai_node n) {
    auto it = s->live.find(n);
    if (it == s->live.end()) return;
    if (it->second.entity) {
        s->by_entity.erase(it->second.entity);
        s->by_body.erase(it->second.body);
        dai_scene_remove(s->scene, it->second.entity);
    }
    s->live.erase(it);
}

// World scale multiplies the collision shape, so a box scaled in the editor
// collides the way it looks. A sphere has one radius, so a non uniform scale
// on it can only follow one axis - x wins, and that is documented rather than
// silently producing an ellipsoid the physics does not have.
dai_vec3 scaled_he(int shape, dai_vec3 he, dai_vec3 ws) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:  return { he.x * std::fabs(ws.x), he.y, he.z };
    // A cylinder scales like a capsule: one radius, one height. Squashing it
    // on Z alone would be an ellipse, and neither backend has one.
    case DAI_SHAPE_CAPSULE:
    case DAI_SHAPE_CYLINDER: return { he.x * std::fabs(ws.x), he.y * std::fabs(ws.y), he.z };
    default:                return { he.x * std::fabs(ws.x), he.y * std::fabs(ws.y),
                                     he.z * std::fabs(ws.z) };
    }
}

dai_vec3 scaled_extent(const dai_node_desc &r, dai_vec3 ws) {
    return scaled_he(r.shape, r.half_extent, ws);
}

bool nonzero3(dai_vec3 v) { return v.x != 0.0f || v.y != 0.0f || v.z != 0.0f; }

// The size the MESH is drawn at. Zero means "the same as the collider", which
// is how every scene behaved before the two could differ - so old files look
// exactly as they did, and a node that says otherwise is honoured.
dai_vec3 render_scale_of(const dai_node_desc &r, dai_vec3 ws) {
    dai_vec3 he = scaled_he(r.shape, r.render_extent, ws);
    switch (r.shape) {
    case DAI_SHAPE_SPHERE:
    case DAI_SHAPE_CAPSULE: return { he.x, he.x, he.x };
    // The cylinder mesh takes its half height from the scale, so it is the one
    // round shape whose Y is not the radius - see shape_to_mesh.
    case DAI_SHAPE_CYLINDER: return { he.x, he.y, he.x };
    default:                return he;
    }
}

} // namespace

// Declared after the struct so it can see the resolver fields.
static void resolve_asset_of(dai_doc_sync *s, const dai_node_desc &r,
                             std::vector<dai_render_part> *out);

namespace {

// What the components add up to. The collider checkbox, the rigidbody
// checkbox and the group flag are three different answers, and none of them is
// "stop drawing the model":
//
//   no_body                     -> no physics at all, still rendered
//   no_collider + no_rigidbody  -> the same thing, reached the other way
//   no_collider                 -> a sensor: it moves, nothing bounces off it
//   trigger                     -> a sensor that still has a collider
//   no_rigidbody                -> static, whatever Motion says
bool physicsless(const dai_node_desc &r) {
    return r.no_body || (r.no_collider && r.no_rigidbody);
}

bool spawn(dai_doc_sync *s, dai_node n, const dai_node_desc &r) {
    dai_vec3 wp{}, ws{ 1, 1, 1 };
    dai_quat wr{ 0, 0, 0, 1 };
    dai_doc_world_transform(s->doc, n, &wp, &wr, &ws);

    dai_entity_desc d = dai_entity_desc_default();
    d.body.shape = r.shape;
    d.body.motion = r.no_rigidbody ? DAI_STATIC : r.motion;
    d.body.sensor = (r.trigger || r.no_collider) && !physicsless(r);
    d.body.half_extent = scaled_extent(r, ws);
    d.body.position = wp;
    d.body.rotation = wr;
    // Unity's "Center": the collider moves, the model does not. The body is
    // what carries the transform here, so the body takes the offset and the
    // mesh takes it back - which is the same picture with a different
    // collision volume, exactly as intended.
    if (nonzero3(r.collider_center)) {
        dai_vec3 off{ r.collider_center.x * ws.x, r.collider_center.y * ws.y,
                      r.collider_center.z * ws.z };
        dai_vec3 world = qrot(wr, off);
        d.body.position = { wp.x + world.x, wp.y + world.y, wp.z + world.z };
        d.render_offset = { -off.x, -off.y, -off.z };
    }
    if (nonzero3(r.render_extent)) d.render_scale = render_scale_of(r, ws);
    d.body.density = r.density;
    d.body.friction_static = r.friction;
    d.body.restitution = r.restitution;
    d.body.no_sleeping = r.no_sleeping;
    d.body.user_data = r.user_data;
    d.mesh = r.mesh;
    // An asset path wins over the mesh index. Failing to resolve is deliberately
    // NOT fatal: the node keeps its collision shape and draws as that shape, so
    // a missing file looks obviously wrong instead of silently vanishing.
    std::vector<dai_render_part> parts;
    resolve_asset_of(s, r, &parts);
    if (!parts.empty()) {
        // The pieces carry their own place inside the model, so the entity only
        // has to carry the document's scale. One piece still goes through the
        // parts path - one code path, and a model that grows a second object
        // does not change how it is drawn.
        d.mesh = parts[0].mesh;
        d.material = parts[0].material;
        d.render_scale = ws;
    }
    d.color = r.color;
    d.roughness = r.roughness;
    d.emissive = r.emissive;
    d.render_flags = r.render_flags;
    d.invisible = r.hidden;
    d.name = r.name[0] ? r.name : nullptr;

    dai_entity e = physicsless(r) ? dai_scene_spawn_render(s->scene, &d)
                                  : dai_scene_spawn(s->scene, &d);
    if (e == DAI_INVALID_ENTITY) return false;

    if (!parts.empty())
        dai_scene_set_parts(s->scene, e, parts.data(), (uint32_t)parts.size());

    Live l;
    l.entity = e;
    l.body = dai_scene_body(s->scene, e);
    l.built = r;
    l.res_parts = parts;
    s->live[n] = l;
    s->by_entity[e] = n;
    s->by_body[l.body] = n;
    return true;
}

} // namespace

// One place where a path becomes render data, used by both the spawn and the
// update path. It used to exist only inside spawn(), which meant an asset that
// finished loading AFTER its node was built never reached the screen - the
// node was already live, so nothing asked again.
static void resolve_asset_of(dai_doc_sync *s, const dai_node_desc &r,
                             std::vector<dai_render_part> *out) {
    out->clear();
    if (!r.asset[0] || !s->resolve_asset) return;
    // Ask once to size it, once to fill it. An asset with no parts and an
    // asset that failed are the same thing here: nothing to draw.
    uint32_t have = s->resolve_asset(r.asset, nullptr, 0, s->resolve_user);
    if (!have) return;
    out->resize(have);
    uint32_t got = s->resolve_asset(r.asset, out->data(), have, s->resolve_user);
    out->resize(got < have ? got : have);
}

extern "C" {

dai_doc_sync *dai_doc_sync_create(dai_doc *d, dai_scene *scene) {
    if (!d || !scene) return nullptr;
    dai_doc_sync *s = new dai_doc_sync();
    s->doc = d;
    s->scene = scene;
    s->world = dai_scene_world(scene);
    return s;
}

void dai_doc_sync_destroy(dai_doc_sync *s) { delete s; }


void dai_doc_sync_resolver(dai_doc_sync *s, dai_asset_resolve_fn fn, void *user) {
    if (!s) return;
    s->resolve_asset = fn;
    s->resolve_user = user;
    // Everything already built may have been drawn with the wrong (or no) asset,
    // so force a full pass rather than leaving stale geometry on screen.
    // UINT64_MAX rather than 0: a document revision of 0 is a real value for a
    // node nobody has touched, and "equal" would skip exactly those.
    for (auto &kv : s->live) kv.second.rev = UINT64_MAX;
}

dai_scene *dai_doc_sync_scene(const dai_doc_sync *s) { return s ? s->scene : nullptr; }
dai_doc   *dai_doc_sync_doc(const dai_doc_sync *s) { return s ? s->doc : nullptr; }

dai_entity dai_doc_sync_entity(const dai_doc_sync *s, dai_node n) {
    if (!s) return DAI_INVALID_ENTITY;
    auto it = s->live.find(n);
    return it == s->live.end() ? DAI_INVALID_ENTITY : it->second.entity;
}

dai_node dai_doc_sync_node(const dai_doc_sync *s, dai_entity e) {
    if (!s) return DAI_INVALID_NODE;
    auto it = s->by_entity.find(e);
    return it == s->by_entity.end() ? DAI_INVALID_NODE : it->second;
}

dai_node dai_doc_sync_node_of_body(const dai_doc_sync *s, dai_body b) {
    if (!s) return DAI_INVALID_NODE;
    auto it = s->by_body.find(b);
    return it == s->by_body.end() ? DAI_INVALID_NODE : it->second;
}

uint32_t dai_doc_sync_apply(dai_doc_sync *s) {
    if (!s) return 0;
    uint32_t changed = 0;

    uint32_t count = dai_doc_count(s->doc);
    std::vector<dai_node> ids(count);
    if (count) dai_doc_nodes(s->doc, ids.data(), count);

    // Parents first, so a child's world transform is computed against a parent
    // that already exists in this pass.
    for (dai_node n : ids) {
        dai_node_desc r{};
        if (dai_doc_get(s->doc, n, &r) != DAI_OK) continue;
        const Node *doc_node = find(s->doc, n);
        uint64_t rev = doc_node ? doc_node->rev : 0;

        auto it = s->live.find(n);
        if (it == s->live.end()) {
            if (spawn(s, n, r)) { s->live[n].rev = rev; ++changed; }
            continue;
        }
        Live &l = it->second;
        if (l.rev == rev) continue;                 // untouched: leave physics alone

        // The path did not change, but the answer might have: the file has
        // finished loading, or a different resolver is in place. Anything that
        // moves mesh, material or scale needs the entity rebuilt - the scene
        // can set a mesh in place, but not a material or a render scale.
        std::vector<dai_render_part> parts;
        resolve_asset_of(s, r, &parts);
        bool asset_moved = parts.size() != l.res_parts.size() ||
                           (!parts.empty() &&
                            std::memcmp(parts.data(), l.res_parts.data(),
                                        parts.size() * sizeof(dai_render_part)) != 0);

        if (asset_moved || needs_rebuild(l.built, r)) {
            // The body is thrown away and recreated - handles change, the node
            // id does not. Nothing outside this file stores a body handle, so
            // this is invisible to the editor and to undo. Rebuilding for a
            // resolver change costs one entity respawn per node, once, when
            // the asset arrives.
            forget(s, n);
            if (spawn(s, n, r)) s->live[n].rev = rev;
            ++changed;
            continue;
        }

        if (l.entity) {
            dai_vec3 wp{}, ws{ 1, 1, 1 };
            dai_quat wr{ 0, 0, 0, 1 };
            dai_doc_world_transform(s->doc, n, &wp, &wr, &ws);
            if (nonzero3(r.collider_center)) {
                dai_vec3 off = qrot(wr, dai_vec3{ r.collider_center.x * ws.x,
                                                  r.collider_center.y * ws.y,
                                                  r.collider_center.z * ws.z });
                wp = { wp.x + off.x, wp.y + off.y, wp.z + off.z };
            }
            dai_body_set_transform(s->world, l.body, wp, wr);
            if (s->zero_velocities && l.body != DAI_INVALID_BODY)
                dai_body_set_velocity(s->world, l.body, dai_vec3{ 0,0,0 }, dai_vec3{ 0,0,0 });
            // A zero colour in the document means "no colour was chosen", and
            // the scene already picked one from the palette when this entity
            // was spawned. Pushing the zero back would paint it black - which
            // is what happened on every edit: move a crate, watch it go dark.
            if (r.color.x != 0.0f || r.color.y != 0.0f || r.color.z != 0.0f)
                dai_scene_set_color(s->scene, l.entity, r.color);
            dai_scene_set_visible(s->scene, l.entity, !r.hidden);
            dai_scene_set_render(s->scene, l.entity, r.mesh, r.roughness, r.emissive, r.render_flags);
            dai_scene_set_name(s->scene, l.entity, r.name);
        }
        l.built = r;
        l.rev = rev;
        ++changed;
    }

    // Anything live that the document no longer has.
    std::vector<dai_node> dead;
    for (const auto &kv : s->live)
        if (!dai_doc_valid(s->doc, kv.first)) dead.push_back(kv.first);
    for (dai_node n : dead) { forget(s, n); ++changed; }

    s->zero_velocities = false;
    return changed;
}

void dai_doc_sync_reset(dai_doc_sync *s) {
    if (!s) return;
    for (auto &kv : s->live) kv.second.rev = 0;   // 0 never matches a real revision
    s->zero_velocities = true;
}

uint32_t dai_doc_sync_pull(dai_doc_sync *s, const char *undo_name) {
    if (!s) return 0;
    uint32_t written = 0;
    dai_doc_begin(s->doc, undo_name ? undo_name : "Apply simulation");
    // Parents before children: a child's local transform is derived from its
    // parent's world transform, so the parent has to be settled first.
    uint32_t count = dai_doc_count(s->doc);
    std::vector<dai_node> ids(count);
    if (count) dai_doc_nodes(s->doc, ids.data(), count);

    for (dai_node n : ids) {
        auto it = s->live.find(n);
        if (it == s->live.end() || !it->second.entity) continue;
        if (it->second.body == DAI_INVALID_BODY) continue;   // render-only: nothing simulated
        dai_transform t{};
        if (dai_body_get(s->world, it->second.body, &t) != DAI_OK) continue;
        // The body sits at the COLLIDER, which a centre offset moved away from
        // the node. Writing that back unchanged would walk the object off by
        // one offset every time play mode was kept.
        dai_node_desc rec{};
        if (dai_doc_get(s->doc, n, &rec) == DAI_OK && nonzero3(rec.collider_center)) {
            dai_vec3 wp{}, ws{ 1, 1, 1 };
            dai_quat wr{ 0, 0, 0, 1 };
            dai_doc_world_transform(s->doc, n, &wp, &wr, &ws);
            dai_vec3 off = qrot(t.rotation, dai_vec3{ rec.collider_center.x * ws.x,
                                                      rec.collider_center.y * ws.y,
                                                      rec.collider_center.z * ws.z });
            t.position = { t.position.x - off.x, t.position.y - off.y, t.position.z - off.z };
        }
        dai_doc_set_world_position(s->doc, n, t.position);
        dai_doc_set_world_rotation(s->doc, n, t.rotation);
        ++written;
    }
    dai_doc_commit(s->doc);

    // The world already looks like this, so do not schedule a write back.
    for (auto &kv : s->live) {
        const Node *node = find(s->doc, kv.first);
        if (node) kv.second.rev = node->rev;
    }
    return written;
}

} // extern "C"
