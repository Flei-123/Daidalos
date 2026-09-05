// Daidalos scene layer. See include/dai_scene.h for what it is for.
//
// No Vulkan, no Talos: this file only knows the public engine API and the
// render instance struct, so it links into the core library and stays
// backend agnostic.

#include "dai_scene.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Renderable {
    dai_body body = DAI_INVALID_BODY;
    // Where a body-less renderable stands. Bodies own their transform; for an
    // entity that deliberately has none (a model, not a physics object) the
    // scene keeps it instead.
    dai_vec3 tpos{ 0, 0, 0 };
    dai_quat trot{ 0, 0, 0, 1 };
    uint32_t mesh = DAI_MESH_BOX;
    dai_vec3 offset{ 0, 0, 0 };   // mesh relative to the body, body space
    dai_vec3 scale{ 1, 1, 1 };
    dai_vec3 color{ 0.8f, 0.8f, 0.8f };
    float    param = 0.0f;
    float    roughness = 1.0f;
    float    emissive = 0.0f;
    uint32_t flags = 0;
    uint32_t material = 0;
    bool     visible = true;
    bool     alive = false;
    std::string name;
    // compound parts, empty for simple shapes
    std::vector<dai_compound_part> parts;
    // drawable pieces of an imported model, empty unless an asset resolved to
    // more than one. Takes priority over both of the above.
    std::vector<dai_render_part> render_parts;
};

// A pleasant default palette so a scene never comes out uniformly grey.
dai_vec3 auto_color(uint32_t id) {
    static const float table[8][3] = {
        { 0.85f, 0.35f, 0.25f }, { 0.30f, 0.55f, 0.80f }, { 0.45f, 0.70f, 0.35f },
        { 0.88f, 0.72f, 0.28f }, { 0.62f, 0.42f, 0.78f }, { 0.30f, 0.72f, 0.70f },
        { 0.80f, 0.50f, 0.65f }, { 0.62f, 0.62f, 0.66f },
    };
    const float *c = table[id % 8];
    return dai_vec3{ c[0], c[1], c[2] };
}

dai_vec3 mul(dai_vec3 a, dai_vec3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
bool is_zero(dai_vec3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

dai_vec3 rotate(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    dai_vec3 uuv{ u.y * uv.z - u.z * uv.y, u.z * uv.x - u.x * uv.z, u.x * uv.y - u.y * uv.x };
    return { v.x + 2.0f * (q.w * uv.x + uuv.x),
             v.y + 2.0f * (q.w * uv.y + uuv.y),
             v.z + 2.0f * (q.w * uv.z + uuv.z) };
}

dai_quat qmul(dai_quat a, dai_quat b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}

// collision shape -> mesh, scale and capsule parameter
void shape_to_mesh(int shape, dai_vec3 he, uint32_t *mesh, dai_vec3 *scale, float *param) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:
        *mesh = DAI_MESH_SPHERE; *scale = { he.x, he.x, he.x }; *param = 0.0f; break;
    case DAI_SHAPE_CAPSULE:
        // half_extent.x = radius, .y = half height of the shaft
        *mesh = DAI_MESH_CAPSULE; *scale = { he.x, he.x, he.x }; *param = he.y; break;
    case DAI_SHAPE_CYLINDER:
        // DAI_MESH_CYLINDER is radius 1 and y +-1, so the half height goes into
        // the scale rather than into param the way the capsule needs - the
        // capsule's caps have to stay round, a cylinder's ends do not exist.
        *mesh = DAI_MESH_CYLINDER; *scale = { he.x, he.y, he.x }; *param = 0.0f; break;
    case DAI_SHAPE_BOX:
    default:
        *mesh = DAI_MESH_BOX; *scale = he; *param = 0.0f; break;
    }
}

} // namespace

struct dai_scene {
    dai_world *world = nullptr;
    std::vector<Renderable> ents;                       // index 0 unused
    std::unordered_map<uint32_t, uint32_t> by_body;     // body handle -> index
    std::vector<dai_transform> scratch;
    // Slots of removed entities, reused by the next attach. Without this an
    // editor session that rebuilds a body on every drag frame grows this
    // vector without bound. Entity ids are therefore NOT unique over time -
    // only the scene document's node ids are.
    std::vector<uint32_t> free_slots;
};

extern "C" {

// Lives here rather than in the renderer backend: it is plain data, and the
// scene layer must link without pulling in Vulkan.
dai_render_instance dai_render_instance_default(void) {
    dai_render_instance i{};
    i.rotation = { 0, 0, 0, 1 };
    i.scale = { 1, 1, 1 };
    i.color = { 0.8f, 0.8f, 0.8f };
    i.mesh = DAI_MESH_BOX;
    i.roughness = 1.0f;
    return i;
}

dai_entity_desc dai_entity_desc_default(void) {
    dai_entity_desc d{};
    d.body.rotation = { 0, 0, 0, 1 };
    d.body.half_extent = { 0.5f, 0.5f, 0.5f };
    d.mesh = 0xFFFFFFFFu;
    d.roughness = 1.0f;
    return d;
}

dai_scene *dai_scene_create(dai_world *w) {
    if (!w) return nullptr;
    dai_scene *s = new dai_scene();
    s->world = w;
    s->ents.push_back(Renderable{});   // slot 0 = invalid
    return s;
}

void dai_scene_destroy(dai_scene *s) { delete s; }
dai_world *dai_scene_world(dai_scene *s) { return s ? s->world : nullptr; }
uint32_t dai_scene_count(dai_scene *s) {
    if (!s) return 0;
    uint32_t n = 0;
    for (size_t i = 1; i < s->ents.size(); ++i) if (s->ents[i].alive) ++n;
    return n;
}

dai_entity dai_scene_attach(dai_scene *s, dai_body b, const dai_entity_desc *desc) {
    if (!s || !desc) return DAI_INVALID_ENTITY;
    Renderable r;
    r.body = b;
    r.tpos = desc->body.position;
    r.trot = desc->body.rotation;
    r.alive = true;
    r.visible = !desc->invisible;
    r.roughness = desc->roughness > 0.0f ? desc->roughness : 1.0f;
    r.emissive = desc->emissive;
    r.flags = desc->render_flags;
    r.material = desc->material;
    if (desc->name) r.name = desc->name;

    uint32_t mesh; dai_vec3 scale; float param;
    shape_to_mesh(desc->body.shape, desc->body.half_extent, &mesh, &scale, &param);
    r.mesh = (desc->mesh == 0xFFFFFFFFu) ? mesh : desc->mesh;
    r.scale = is_zero(desc->render_scale) ? scale : desc->render_scale;
    r.offset = desc->render_offset;
    r.param = param;
    r.color = is_zero(desc->color) ? auto_color((uint32_t)s->ents.size()) : desc->color;

    if (desc->body.shape == DAI_SHAPE_COMPOUND && desc->body.parts && desc->body.part_count)
        r.parts.assign(desc->body.parts, desc->body.parts + desc->body.part_count);

    uint32_t idx;
    if (!s->free_slots.empty()) {
        idx = s->free_slots.back();
        s->free_slots.pop_back();
        if (is_zero(desc->color)) r.color = auto_color(idx);
        s->ents[idx] = r;
    } else {
        s->ents.push_back(r);
        idx = (uint32_t)s->ents.size() - 1;
    }
    if (b != DAI_INVALID_BODY) s->by_body[b] = idx;
    return idx;
}

dai_entity dai_scene_spawn_render(dai_scene *s, const dai_entity_desc *desc) {
    // No body at all - the entity is a picture with a transform.
    return dai_scene_attach(s, DAI_INVALID_BODY, desc);
}

dai_result dai_scene_set_transform(dai_scene *s, dai_entity e, dai_vec3 pos, dai_quat rot) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    Renderable &r = s->ents[e];
    if (r.body != DAI_INVALID_BODY) {
        // A body's transform belongs to the physics - this is the same call
        // with the authority in the right place.
        return dai_body_set_transform(s->world, r.body, pos, rot);
    }
    r.tpos = pos;
    r.trot = rot;
    return DAI_OK;
}

dai_entity dai_scene_spawn(dai_scene *s, const dai_entity_desc *desc) {
    if (!s || !desc) return DAI_INVALID_ENTITY;
    dai_body b = dai_body_create(s->world, &desc->body);
    if (b == DAI_INVALID_BODY) return DAI_INVALID_ENTITY;
    return dai_scene_attach(s, b, desc);
}

dai_result dai_scene_remove(dai_scene *s, dai_entity e) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    Renderable &r = s->ents[e];
    if (r.body != DAI_INVALID_BODY) {
        dai_body_destroy(s->world, r.body);
        s->by_body.erase(r.body);
    }
    r.alive = false;
    r.parts.clear();
    r.name.clear();
    s->free_slots.push_back(e);
    return DAI_OK;
}

dai_body dai_scene_body(dai_scene *s, dai_entity e) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_INVALID_BODY;
    return s->ents[e].body;
}

dai_entity dai_scene_find(dai_scene *s, const char *name) {
    if (!s || !name) return DAI_INVALID_ENTITY;
    for (size_t i = 1; i < s->ents.size(); ++i)
        if (s->ents[i].alive && s->ents[i].name == name) return (dai_entity)i;
    return DAI_INVALID_ENTITY;
}

dai_result dai_scene_set_color(dai_scene *s, dai_entity e, dai_vec3 c) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    s->ents[e].color = c;
    return DAI_OK;
}

dai_result dai_scene_color(const dai_scene *s, dai_entity e, dai_vec3 *out) {
    if (!s || !out || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    *out = s->ents[e].color;
    return DAI_OK;
}

dai_result dai_scene_set_visible(dai_scene *s, dai_entity e, int visible) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    s->ents[e].visible = visible != 0;
    return DAI_OK;
}

dai_result dai_scene_set_render(dai_scene *s, dai_entity e, uint32_t mesh,
                                float roughness, float emissive, uint32_t flags) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    Renderable &r = s->ents[e];
    if (mesh != 0xFFFFFFFFu) r.mesh = mesh;
    r.roughness = roughness > 0.0f ? roughness : 1.0f;
    r.emissive = emissive;
    r.flags = flags;
    return DAI_OK;
}

dai_result dai_scene_set_parts(dai_scene *s, dai_entity e,
                               const dai_render_part *parts, uint32_t count) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    if (count && !parts) return DAI_ERR_INVALID_ARG;
    Renderable &r = s->ents[e];
    r.render_parts.assign(parts, parts + count);
    return DAI_OK;
}

uint32_t dai_scene_part_count(const dai_scene *s, dai_entity e) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return 0;
    return (uint32_t)s->ents[e].render_parts.size();
}

dai_result dai_scene_set_material(dai_scene *s, dai_entity e, uint32_t material) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    s->ents[e].material = material;
    return DAI_OK;
}

dai_result dai_scene_set_name(dai_scene *s, dai_entity e, const char *name) {
    if (!s || e == 0 || e >= s->ents.size() || !s->ents[e].alive) return DAI_ERR_NOT_FOUND;
    s->ents[e].name = name ? name : "";
    return DAI_OK;
}

uint32_t dai_scene_instances(dai_scene *s, dai_render_instance *out, uint32_t max, float alpha) {
    if (!s || !out || !max) return 0;
    uint32_t cap = dai_scene_count(s) + 16;
    if (s->scratch.size() < cap) s->scratch.resize(cap);
    uint32_t n = dai_get_transforms(s->world, s->scratch.data(), (uint32_t)s->scratch.size(), alpha);

    uint32_t w = 0;
    // Body-less renderables first: their transform is their own.
    for (size_t i = 1; i < s->ents.size() && w < max; ++i) {
        const Renderable &r = s->ents[i];
        if (!r.alive || !r.visible || r.body != DAI_INVALID_BODY) continue;
        dai_render_instance &o = out[w++];
        o = dai_render_instance_default();
        o.position = r.tpos;
        o.rotation = r.trot;
        o.scale = r.scale;
        o.color = r.color;
        o.mesh = r.mesh;
        o.param = r.param;
        o.roughness = r.roughness;
        o.emissive = r.emissive;
        o.flags = r.flags;
        o.material = r.material;
    }
    for (uint32_t i = 0; i < n && w < max; ++i) {
        const dai_transform &t = s->scratch[i];
        auto it = s->by_body.find(t.body);
        if (it == s->by_body.end()) continue;
        const Renderable &r = s->ents[it->second];
        if (!r.alive || !r.visible) continue;

        if (!r.render_parts.empty()) {
            // An imported model: every piece carries its own mesh, material and
            // place inside the model, and the entity's transform puts the whole
            // thing in the world.
            for (const dai_render_part &p : r.render_parts) {
                if (w >= max) break;
                dai_vec3 local = mul(p.position, r.scale);
                local = { local.x + r.offset.x, local.y + r.offset.y, local.z + r.offset.z };
                dai_render_instance &o = out[w++];
                o = dai_render_instance_default();
                o.position = { t.position.x + rotate(t.rotation, local).x,
                               t.position.y + rotate(t.rotation, local).y,
                               t.position.z + rotate(t.rotation, local).z };
                o.rotation = qmul(t.rotation, p.rotation);
                o.scale = mul(p.scale, r.scale);
                o.color = r.color;
                o.mesh = p.mesh;
                o.roughness = r.roughness;
                o.emissive = r.emissive;
                o.flags = r.flags;
                o.material = p.material;
            }
        } else if (r.parts.empty()) {
            dai_render_instance &o = out[w++];
            o = dai_render_instance_default();
            o.position = t.position;
            if (!is_zero(r.offset)) {
                dai_vec3 off = rotate(t.rotation, r.offset);
                o.position = { t.position.x + off.x, t.position.y + off.y,
                               t.position.z + off.z };
            }
            o.rotation = t.rotation;
            o.scale = r.scale;
            o.color = r.color;
            o.mesh = r.mesh;
            o.param = r.param;
            o.roughness = r.roughness;
            o.emissive = r.emissive;
            o.flags = r.flags;
            o.material = r.material;
        } else {
            for (const dai_compound_part &p : r.parts) {
                if (w >= max) break;
                uint32_t mesh; dai_vec3 scale; float param;
                shape_to_mesh(p.shape, p.half_extent, &mesh, &scale, &param);
                dai_render_instance &o = out[w++];
                o = dai_render_instance_default();
                o.position = { t.position.x + rotate(t.rotation, p.offset).x,
                               t.position.y + rotate(t.rotation, p.offset).y,
                               t.position.z + rotate(t.rotation, p.offset).z };
                o.rotation = qmul(t.rotation, p.rotation);
                o.scale = mul(scale, r.scale);
                o.color = r.color;
                o.mesh = mesh;
                o.param = param;
                o.roughness = r.roughness;
                o.emissive = r.emissive;
                o.flags = r.flags;
                o.material = r.material;
            }
        }
    }
    return w;
}

// ---------------------------------------------------------------- camera

dai_camera dai_camera_default(void) {
    dai_camera c{};
    c.target = { 0, 1, 0 };
    c.distance = 14.0f;
    c.yaw = 0.6f;
    c.pitch = 0.35f;
    c.fov = 55.0f;
    c.znear = 0.1f;
    c.zfar = 600.0f;
    return c;
}

dai_vec3 dai_camera_eye(const dai_camera *c) {
    if (!c) return dai_vec3{ 0, 0, 0 };
    float cp = cosf(c->pitch), sp = sinf(c->pitch);
    return dai_vec3{ c->target.x + c->distance * cp * sinf(c->yaw),
                     c->target.y + c->distance * sp,
                     c->target.z + c->distance * cp * cosf(c->yaw) };
}

void dai_camera_follow(dai_camera *c, dai_vec3 p, float smoothing, float dt) {
    if (!c) return;
    // framerate independent exponential smoothing
    float k = (smoothing <= 0.0f) ? 1.0f : 1.0f - expf(-dt / smoothing);
    c->target.x += (p.x - c->target.x) * k;
    c->target.y += (p.y - c->target.y) * k;
    c->target.z += (p.z - c->target.z) * k;
}

void dai_camera_frame(dai_camera *c, dai_vec3 center, float radius) {
    if (!c || radius <= 0.0f) return;
    c->target = center;
    float half = c->fov * 3.14159265f / 360.0f;
    c->distance = radius / fmaxf(0.05f, sinf(half)) * 1.05f;
}

} // extern "C"
