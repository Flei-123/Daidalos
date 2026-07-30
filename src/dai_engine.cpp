// Daidalos - deterministic engine core.
//
// Reading order:
//   1. dai_world          - everything the simulation consists of
//   2. dai_step           - the only way time moves
//   3. dai_rollback_to    - undo the world, restore physics, re-simulate
//
// Rules enforced here that must not be softened later:
//   * the sim never reads wall clock time, only tick numbers
//   * the sim never reads the audio or render system
//   * every mutation is recorded as a tick stamped command
//   * this file contains NO physics library type. Look for a Jolt include
//     here: there is none, and there must never be one.

#include "dai_internal.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>

using namespace dai;

struct dai_world {
    dai_config cfg{};
    float      dt   = 1.0f / 60.0f;
    double     dt_d = 1.0 / 60.0;      // accumulator maths; float would lose a tick
    dai_tick   tick = 0;
    Rng        rng;

    IPhysicsBackend *phys = nullptr;

    std::vector<BodySlot> slots;
    uint32_t              live_bodies = 0;

    std::vector<JointSlot> jslots;
    uint32_t               live_joints = 0;

    std::vector<Command>       cmds;          // append only, sorted by tick
    std::vector<dai_body_desc> cmd_descs;
    std::vector<std::vector<dai_compound_part>> cmd_parts;
    std::vector<dai_joint_desc> cmd_jdescs;

    struct InputRec { dai_input in{}; bool set = false; dai_tick tick = UINT64_MAX; };
    std::vector<InputRec> inputs;
    uint32_t              input_ring = 256;

    std::vector<Snapshot> snaps;
    uint32_t              snap_ring = 64;

    std::vector<PendingAudio> audio;
    dai_audio_backend        *audio_be = nullptr;

    std::vector<ContactEvent> contact_scratch;

    double   accumulator = 0.0;
    uint64_t ticks_sim = 0, ticks_resim = 0;
    uint32_t rollbacks = 0;
    double   last_ms = 0.0, sum_ms = 0.0;
    bool     replaying = false;
    bool     in_callback = false;

    dai_tick_fn tick_fn  = nullptr;
    void       *tick_usr = nullptr;

    char err[256] = {0};
};

namespace {

void set_err(dai_world *w, const char *msg) {
    if (w) std::snprintf(w->err, sizeof(w->err), "%s", msg);
}

inline dai_body pack(uint32_t slot, uint32_t gen) { return (slot + 1) | (gen << 24); }
inline uint32_t slot_of(dai_body b) { return (b & 0x00ffffffu) - 1; }
inline uint32_t gen_of(dai_body b) { return b >> 24; }

JointSlot *resolve_joint(dai_world *w, dai_joint j);

BodySlot *resolve(dai_world *w, dai_body b) {
    if (b == DAI_INVALID_BODY) return nullptr;
    uint32_t s = slot_of(b);
    if (s >= w->slots.size()) return nullptr;
    BodySlot &sl = w->slots[s];
    if (!sl.alive || sl.generation != gen_of(b)) return nullptr;
    return &sl;
}

JointSlot *resolve_joint(dai_world *w, dai_joint j) {
    if (j == DAI_INVALID_JOINT) return nullptr;
    uint32_t s = (j & 0x00ffffffu) - 1;
    if (s >= w->jslots.size()) return nullptr;
    JointSlot &sl = w->jslots[s];
    if (!sl.alive || sl.generation != (j >> 24)) return nullptr;
    return &sl;
}

bool spawn_joint(dai_world *w, uint32_t slot) {
    JointSlot &j = w->jslots[slot];
    if (!w->phys->create_joint(slot, j.desc, j.slot_a, j.slot_b)) return false;
    j.alive = true;
    return true;
}

void despawn_joint(dai_world *w, uint32_t slot) {
    JointSlot &j = w->jslots[slot];
    if (!j.alive) return;
    w->phys->destroy_joint(slot);
    j.alive = false;
    j.generation++;
}

void refresh_transform(dai_world *w, uint32_t i) {
    BodySlot &s = w->slots[i];
    s.prev_pos = s.cur_pos;
    s.prev_rot = s.cur_rot;
    w->phys->get_transform(i, s.cur_pos, s.cur_rot);
}

bool spawn(dai_world *w, uint32_t slot) {
    BodySlot &s = w->slots[slot];
    if (!w->phys->create_body(slot, s.desc, s.parts)) return false;
    s.alive = true;
    w->phys->get_transform(slot, s.cur_pos, s.cur_rot);
    s.prev_pos = s.cur_pos;
    s.prev_rot = s.cur_rot;
    return true;
}

void despawn(dai_world *w, uint32_t slot) {
    BodySlot &s = w->slots[slot];
    if (!s.alive) return;
    w->phys->destroy_body(slot);
    s.alive = false;
    s.generation++;
}

// ---- command log ----------------------------------------------------------

void log_cmd(dai_world *w, const Command &c) {
    if (w->replaying) return;    // replay reads the log, it never writes it
    if (w->in_callback) return;  // the callback re-issues its own commands on replay
    w->cmds.push_back(c);
}

void trim_log(dai_world *w) {
    dai_tick oldest = w->tick > w->snap_ring ? w->tick - w->snap_ring : 0;
    if (w->cmds.empty() || w->cmds.front().tick >= oldest) return;
    size_t keep = 0;
    while (keep < w->cmds.size() && w->cmds[keep].tick < oldest) ++keep;
    if (keep) w->cmds.erase(w->cmds.begin(), w->cmds.begin() + keep);
}

void apply_cmds_for_tick(dai_world *w, dai_tick t) {
    auto lo = std::lower_bound(w->cmds.begin(), w->cmds.end(), t,
        [](const Command &c, dai_tick v) { return c.tick < v; });
    for (auto it = lo; it != w->cmds.end() && it->tick == t; ++it) {
        const Command &c = *it;
        switch (c.type) {
        case CmdType::Create:
            if (c.slot < w->slots.size() && !w->slots[c.slot].alive) {
                BodySlot &s = w->slots[c.slot];
                if (c.desc_index >= 0) {
                    s.desc  = w->cmd_descs[(size_t)c.desc_index];
                    s.parts = w->cmd_parts[(size_t)c.desc_index];
                    s.desc.parts      = s.parts.empty() ? nullptr : s.parts.data();
                    s.desc.part_count = (uint32_t)s.parts.size();
                }
                s.created_tick = t;
                s.destroyed_tick = UINT64_MAX;
                if (spawn(w, c.slot)) w->live_bodies++;
            }
            break;
        case CmdType::Destroy:
            if (c.slot < w->slots.size() && w->slots[c.slot].alive) {
                w->slots[c.slot].destroyed_tick = t;
                despawn(w, c.slot);
                w->live_bodies--;
            }
            break;
        case CmdType::Impulse:
            if (c.slot < w->slots.size() && w->slots[c.slot].alive) w->phys->add_impulse(c.slot, c.a);
            break;
        case CmdType::SetVelocity:
            if (c.slot < w->slots.size() && w->slots[c.slot].alive) w->phys->set_velocity(c.slot, c.a, c.b);
            break;
        case CmdType::Gravity:
            w->phys->set_gravity(c.a);
            break;
        case CmdType::CreateJoint:
            if (c.slot < w->jslots.size() && !w->jslots[c.slot].alive && c.desc_index >= 0) {
                JointSlot &j = w->jslots[c.slot];
                j.desc   = w->cmd_jdescs[(size_t)c.desc_index];
                j.slot_a = (uint32_t)c.a.x;
                j.slot_b = (uint32_t)c.a.y;
                j.created_tick = t;
                j.destroyed_tick = UINT64_MAX;
                if (spawn_joint(w, c.slot)) w->live_joints++;
            }
            break;
        case CmdType::DestroyJoint:
            if (c.slot < w->jslots.size() && w->jslots[c.slot].alive) {
                w->jslots[c.slot].destroyed_tick = t;
                despawn_joint(w, c.slot);
                w->live_joints--;
            }
            break;
        case CmdType::Motor:
            if (c.slot < w->jslots.size() && w->jslots[c.slot].alive)
                w->phys->set_motor(c.slot, (int)c.a.x, c.a.y);
            break;
        }
    }
}

void save_snapshot(dai_world *w) {
    Snapshot &s = w->snaps[w->tick % w->snap_ring];
    w->phys->save_state(s.physics);
    s.rng   = w->rng;
    s.tick  = w->tick;
    s.valid = true;
}

} // namespace

// ---------------------------------------------------------------------------

extern "C" {

const char *dai_version(void) { return "daidalos 0.2.0 (backends: jolt, null)"; }

dai_result dai_create(const dai_config *cfg_in, dai_world **out) {
    if (!out) return DAI_ERR_INVALID_ARG;
    *out = nullptr;

    dai_world *w = new dai_world();
    if (cfg_in) w->cfg = *cfg_in;
    if (w->cfg.tick_hz == 0)        w->cfg.tick_hz = 60;
    if (w->cfg.max_bodies == 0)     w->cfg.max_bodies = 8192;
    if (w->cfg.snapshot_ring == 0)  w->cfg.snapshot_ring = 64;
    if (w->cfg.velocity_steps == 0) w->cfg.velocity_steps = 10;
    if (w->cfg.position_steps == 0) w->cfg.position_steps = 2;
    w->dt         = 1.0f / (float)w->cfg.tick_hz;
    w->dt_d       = 1.0  / (double)w->cfg.tick_hz;
    w->snap_ring  = w->cfg.snapshot_ring;
    w->input_ring = std::max(256u, w->snap_ring * 4);
    w->rng.seed(w->cfg.seed ? w->cfg.seed : 0x9e3779b97f4a7c15ULL);

    w->phys = (w->cfg.backend == DAI_PHYSICS_NULL) ? create_null_backend() : create_jolt_backend();
    if (!w->phys) { delete w; return DAI_ERR_OUT_OF_MEMORY; }
    char perr[192] = {0};
    if (!w->phys->init(w->cfg, perr, sizeof(perr))) {
        set_err(w, perr[0] ? perr : "physics backend init failed");
        delete w->phys; delete w;
        return DAI_ERR_STATE;
    }

    w->slots.resize(w->cfg.max_bodies);
    w->jslots.resize(512);
    w->snaps.resize(w->snap_ring);
    w->inputs.resize((size_t)w->input_ring * DAI_MAX_PLAYERS);

    if (w->cfg.audio_bank && w->cfg.audio_bank[0]) {
        char err[192] = {0};
        w->audio_be = dai_audio_open(w->cfg.audio_bank, w->cfg.asset_root,
                                     w->cfg.enable_audio_device, err, sizeof(err));
        if (!w->audio_be) set_err(w, err[0] ? err : "audio bank could not be opened");
    }

    save_snapshot(w);   // snapshot for tick 0
    *out = w;
    return DAI_OK;
}

void dai_destroy(dai_world *w) {
    if (!w) return;
    for (uint32_t i = 0; i < w->jslots.size(); ++i) if (w->jslots[i].alive) despawn_joint(w, i);
    for (uint32_t i = 0; i < w->slots.size(); ++i) if (w->slots[i].alive) despawn(w, i);
    if (w->audio_be) dai_audio_close(w->audio_be);
    delete w->phys;
    delete w;
}

const char *dai_last_error(dai_world *w)   { return w ? w->err : "no world"; }
const char *dai_backend_name(dai_world *w) { return (w && w->phys) ? w->phys->name() : "none"; }

// ---- bodies ---------------------------------------------------------------

dai_body dai_body_create(dai_world *w, const dai_body_desc *desc_in) {
    if (!w || !desc_in) return DAI_INVALID_BODY;

    // Lowest free slot: deterministic. But a slot whose body was destroyed
    // INSIDE the snapshot ring may not be reused yet - a rollback has to be
    // able to resurrect that body, and a slot only remembers one lifetime.
    // Reusing it produced a "backend restore_state failed" the moment merging
    // (destroy many, create one) landed on a just freed slot.
    uint32_t slot = UINT32_MAX, fallback = UINT32_MAX;
    dai_tick oldest = dai_oldest_snapshot(w);
    for (uint32_t i = 0; i < w->slots.size(); ++i) {
        if (w->slots[i].alive) continue;
        if (fallback == UINT32_MAX) fallback = i;
        if (w->slots[i].destroyed_tick == UINT64_MAX || w->slots[i].destroyed_tick < oldest) { slot = i; break; }
    }
    if (slot == UINT32_MAX) slot = fallback;             // out of fresh slots: reuse and accept the risk
    if (slot == UINT32_MAX) { set_err(w, "max_bodies reached"); return DAI_INVALID_BODY; }

    dai_body_desc d = *desc_in;
    if (d.density == 0.0f)          d.density = 1000.0f;
    if (d.friction_static == 0.0f)  d.friction_static = 0.6f;
    if (d.friction_kinetic == 0.0f) d.friction_kinetic = d.friction_static;
    if (d.rotation.x == 0 && d.rotation.y == 0 && d.rotation.z == 0 && d.rotation.w == 0)
        d.rotation.w = 1.0f;

    std::vector<dai_compound_part> parts;
    if (d.shape == DAI_SHAPE_COMPOUND && d.parts && d.part_count)
        parts.assign(d.parts, d.parts + std::min<uint32_t>(d.part_count, DAI_MAX_PARTS));

    BodySlot &s = w->slots[slot];
    s.parts = parts;
    s.desc  = d;
    s.desc.parts      = s.parts.empty() ? nullptr : s.parts.data();
    s.desc.part_count = (uint32_t)s.parts.size();
    s.created_tick    = w->tick;
    s.destroyed_tick  = UINT64_MAX;

    if (!spawn(w, slot)) { set_err(w, "body could not be created (invalid shape?)"); return DAI_INVALID_BODY; }
    w->live_bodies++;

    if (!w->replaying && !w->in_callback) {
        Command c; c.tick = w->tick; c.type = CmdType::Create; c.slot = slot;
        w->cmd_descs.push_back(s.desc);
        w->cmd_parts.push_back(s.parts);
        c.desc_index = (int32_t)w->cmd_descs.size() - 1;
        log_cmd(w, c);
    }
    return pack(slot, s.generation);
}

// ---------------------------------------------------------------- merging

namespace {

dai_vec3 q_rotate(dai_quat q, dai_vec3 v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 uv{ u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    dai_vec3 uu{ u.y*uv.z - u.z*uv.y, u.z*uv.x - u.x*uv.z, u.x*uv.y - u.y*uv.x };
    return { v.x + 2.0f*(q.w*uv.x + uu.x), v.y + 2.0f*(q.w*uv.y + uu.y), v.z + 2.0f*(q.w*uv.z + uu.z) };
}
dai_quat q_mul(dai_quat a, dai_quat b) {
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
dai_quat q_conj(dai_quat q) { return { -q.x, -q.y, -q.z, q.w }; }

// Rough volume of a shape, used only to weight the merge: exact inertia is
// Jolt's job, this just decides where the compound's origin sits and how the
// group's momentum is shared out.
float shape_volume(int shape, dai_vec3 he) {
    switch (shape) {
    case DAI_SHAPE_SPHERE:  return 4.18879f * he.x * he.x * he.x;
    case DAI_SHAPE_CAPSULE: return 3.14159f * he.x * he.x * (2.0f * he.y) + 4.18879f * he.x * he.x * he.x;
    default:                return 8.0f * he.x * he.y * he.z;
    }
}

} // namespace

dai_body dai_body_merge(dai_world *w, const dai_body *bodies, uint32_t count, uint32_t user_data) {
    if (!w || !bodies || count == 0) return DAI_INVALID_BODY;
    if (count == 1) return bodies[0];

    struct Src { uint32_t slot; dai_vec3 pos; dai_quat rot; dai_vec3 lv, av; float mass; };
    std::vector<Src> src;
    src.reserve(count);
    uint32_t total_parts = 0;
    bool any_static = false;

    for (uint32_t i = 0; i < count; ++i) {
        BodySlot *s = resolve(w, bodies[i]);
        if (!s) { set_err(w, "dai_body_merge: stale body handle"); return DAI_INVALID_BODY; }
        uint32_t slot = slot_of(bodies[i]);
        Src e{};
        e.slot = slot;
        w->phys->get_transform(slot, e.pos, e.rot);
        w->phys->get_velocity(slot, e.lv, e.av);
        float vol = 0.0f;
        if (s->desc.shape == DAI_SHAPE_COMPOUND && !s->parts.empty()) {
            for (const dai_compound_part &p : s->parts) vol += shape_volume(p.shape, p.half_extent);
            total_parts += (uint32_t)s->parts.size();
        } else {
            vol = shape_volume(s->desc.shape, s->desc.half_extent);
            total_parts += 1;
        }
        e.mass = vol * (s->desc.density > 0.0f ? s->desc.density : 1000.0f);
        if (s->desc.motion == DAI_STATIC) any_static = true;
        src.push_back(e);
    }
    if (total_parts > DAI_MAX_PARTS) { set_err(w, "dai_body_merge: too many parts"); return DAI_INVALID_BODY; }

    // origin = centre of mass, orientation = that of the first body
    float total_mass = 0.0f;
    dai_vec3 centre{ 0, 0, 0 }, momentum{ 0, 0, 0 };
    for (const Src &e : src) {
        total_mass += e.mass;
        centre.x += e.pos.x * e.mass; centre.y += e.pos.y * e.mass; centre.z += e.pos.z * e.mass;
        momentum.x += e.lv.x * e.mass; momentum.y += e.lv.y * e.mass; momentum.z += e.lv.z * e.mass;
    }
    if (total_mass <= 0.0f) total_mass = 1.0f;
    centre = { centre.x / total_mass, centre.y / total_mass, centre.z / total_mass };
    dai_vec3 vel{ momentum.x / total_mass, momentum.y / total_mass, momentum.z / total_mass };
    dai_quat rot = src[0].rot;
    dai_quat inv = q_conj(rot);

    std::vector<dai_compound_part> parts;
    parts.reserve(total_parts);
    for (const Src &e : src) {
        BodySlot &s = w->slots[e.slot];
        dai_vec3 rel{ e.pos.x - centre.x, e.pos.y - centre.y, e.pos.z - centre.z };
        dai_vec3 local_off = q_rotate(inv, rel);
        dai_quat local_rot = q_mul(inv, e.rot);
        if (s.desc.shape == DAI_SHAPE_COMPOUND && !s.parts.empty()) {
            // flatten: a merged block group merged again stays one level deep
            for (const dai_compound_part &p : s.parts) {
                dai_compound_part np = p;
                np.offset = q_rotate(local_rot, p.offset);
                np.offset = { np.offset.x + local_off.x, np.offset.y + local_off.y, np.offset.z + local_off.z };
                np.rotation = q_mul(local_rot, p.rotation);
                parts.push_back(np);
            }
        } else {
            dai_compound_part np{};
            np.shape = s.desc.shape;
            np.half_extent = s.desc.half_extent;
            np.offset = local_off;
            np.rotation = local_rot;
            parts.push_back(np);
        }
    }

    const dai_body_desc &first = w->slots[src[0].slot].desc;
    dai_body_desc d{};
    d.shape = DAI_SHAPE_COMPOUND;
    d.motion = any_static ? DAI_STATIC : DAI_DYNAMIC;
    d.position = centre;
    d.rotation = rot;
    d.density = first.density;
    d.friction_static = first.friction_static;
    d.friction_kinetic = first.friction_kinetic;
    d.restitution = first.restitution;
    d.linear_damping = first.linear_damping;
    d.angular_damping = first.angular_damping;
    d.no_sleeping = first.no_sleeping;
    d.user_data = user_data;
    d.parts = parts.data();
    d.part_count = (uint32_t)parts.size();

    // destroy first, so the new body can reuse a freed slot deterministically
    for (const Src &e : src) dai_body_destroy(w, pack(e.slot, w->slots[e.slot].generation));

    dai_body merged = dai_body_create(w, &d);
    if (merged == DAI_INVALID_BODY) return DAI_INVALID_BODY;
    if (d.motion != DAI_STATIC) dai_body_set_velocity(w, merged, vel, src[0].av);
    return merged;
}

uint32_t dai_body_split(dai_world *w, dai_body body, dai_body *out, uint32_t max) {
    if (!w) return 0;
    BodySlot *s = resolve(w, body);
    if (!s) return 0;
    uint32_t slot = slot_of(body);

    dai_vec3 pos, lv, av; dai_quat rot;
    w->phys->get_transform(slot, pos, rot);
    w->phys->get_velocity(slot, lv, av);

    std::vector<dai_compound_part> parts = s->parts;
    dai_body_desc base = s->desc;
    if (parts.empty()) {                       // a simple shape splits into itself
        if (out && max) out[0] = body;
        return 1;
    }

    dai_body_destroy(w, body);

    uint32_t n = 0;
    for (const dai_compound_part &p : parts) {
        dai_vec3 world_off = q_rotate(rot, p.offset);
        dai_body_desc d = base;
        d.shape = p.shape;
        d.half_extent = p.half_extent;
        d.position = { pos.x + world_off.x, pos.y + world_off.y, pos.z + world_off.z };
        d.rotation = q_mul(rot, p.rotation);
        d.parts = nullptr;
        d.part_count = 0;
        dai_body b = dai_body_create(w, &d);
        if (b == DAI_INVALID_BODY) break;
        // rigid body velocity at that offset: v + omega x r
        dai_vec3 v{ lv.x + av.y * world_off.z - av.z * world_off.y,
                    lv.y + av.z * world_off.x - av.x * world_off.z,
                    lv.z + av.x * world_off.y - av.y * world_off.x };
        if (d.motion != DAI_STATIC) dai_body_set_velocity(w, b, v, av);
        if (out && n < max) out[n] = b;
        ++n;
    }
    return n;
}

uint32_t dai_body_part_count(dai_world *w, dai_body b) {
    if (!w) return 0;
    BodySlot *s = resolve(w, b);
    if (!s) return 0;
    return s->parts.empty() ? 1u : (uint32_t)s->parts.size();
}

dai_result dai_body_get_velocity(dai_world *w, dai_body b, dai_vec3 *linear, dai_vec3 *angular) {
    if (!w) return DAI_ERR_INVALID_ARG;
    BodySlot *s = resolve(w, b);
    if (!s) return DAI_ERR_NOT_FOUND;
    dai_vec3 lv, av;
    w->phys->get_velocity(slot_of(b), lv, av);
    if (linear) *linear = lv;
    if (angular) *angular = av;
    return DAI_OK;
}

dai_result dai_body_destroy(dai_world *w, dai_body b) {
    if (!w) return DAI_ERR_INVALID_ARG;
    BodySlot *s = resolve(w, b);
    if (!s) return DAI_ERR_NOT_FOUND;
    uint32_t slot = slot_of(b);
    s->destroyed_tick = w->tick;
    despawn(w, slot);
    w->live_bodies--;
    Command c; c.tick = w->tick; c.type = CmdType::Destroy; c.slot = slot;
    log_cmd(w, c);
    return DAI_OK;
}

dai_result dai_body_add_impulse(dai_world *w, dai_body b, dai_vec3 imp) {
    if (!w) return DAI_ERR_INVALID_ARG;
    if (!resolve(w, b)) return DAI_ERR_NOT_FOUND;
    w->phys->add_impulse(slot_of(b), imp);
    Command c; c.tick = w->tick; c.type = CmdType::Impulse; c.slot = slot_of(b); c.a = imp;
    log_cmd(w, c);
    return DAI_OK;
}

dai_result dai_body_set_velocity(dai_world *w, dai_body b, dai_vec3 lin, dai_vec3 ang) {
    if (!w) return DAI_ERR_INVALID_ARG;
    if (!resolve(w, b)) return DAI_ERR_NOT_FOUND;
    w->phys->set_velocity(slot_of(b), lin, ang);
    Command c; c.tick = w->tick; c.type = CmdType::SetVelocity; c.slot = slot_of(b); c.a = lin; c.b = ang;
    log_cmd(w, c);
    return DAI_OK;
}

dai_result dai_set_gravity(dai_world *w, dai_vec3 g) {
    if (!w) return DAI_ERR_INVALID_ARG;
    w->phys->set_gravity(g);
    Command c; c.tick = w->tick; c.type = CmdType::Gravity; c.a = g;
    log_cmd(w, c);
    return DAI_OK;
}

int dai_body_valid(dai_world *w, dai_body b) { return (w && resolve(w, b)) ? 1 : 0; }

dai_result dai_body_get(dai_world *w, dai_body b, dai_transform *out) {
    if (!w || !out) return DAI_ERR_INVALID_ARG;
    BodySlot *s = resolve(w, b);
    if (!s) return DAI_ERR_NOT_FOUND;
    out->body = b; out->position = s->cur_pos; out->rotation = s->cur_rot;
    out->user_data = s->desc.user_data;
    return DAI_OK;
}

// ---- joints ---------------------------------------------------------------

dai_joint dai_joint_create(dai_world *w, const dai_joint_desc *desc_in) {
    if (!w || !desc_in) return DAI_INVALID_JOINT;
    BodySlot *ba = resolve(w, desc_in->a);
    if (!ba) { set_err(w, "joint body a is invalid"); return DAI_INVALID_JOINT; }
    uint32_t sa = slot_of(desc_in->a);
    uint32_t sb = UINT32_MAX;
    if (desc_in->b != DAI_INVALID_BODY) {
        if (!resolve(w, desc_in->b)) { set_err(w, "joint body b is invalid"); return DAI_INVALID_JOINT; }
        sb = slot_of(desc_in->b);
    }

    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < w->jslots.size(); ++i)
        if (!w->jslots[i].alive) { slot = i; break; }
    if (slot == UINT32_MAX) { set_err(w, "joint limit reached"); return DAI_INVALID_JOINT; }

    JointSlot &j = w->jslots[slot];
    j.desc   = *desc_in;
    j.slot_a = sa;
    j.slot_b = sb;
    j.created_tick = w->tick;
    j.destroyed_tick = UINT64_MAX;
    if (!spawn_joint(w, slot)) { set_err(w, "joint could not be created"); return DAI_INVALID_JOINT; }
    w->live_joints++;

    if (!w->replaying && !w->in_callback) {
        Command c; c.tick = w->tick; c.type = CmdType::CreateJoint; c.slot = slot;
        c.a = dai_vec3{ (float)sa, (float)sb, 0.0f };
        w->cmd_jdescs.push_back(j.desc);
        c.desc_index = (int32_t)w->cmd_jdescs.size() - 1;
        log_cmd(w, c);
    }
    return (slot + 1) | (j.generation << 24);
}

dai_result dai_joint_destroy(dai_world *w, dai_joint jh) {
    if (!w) return DAI_ERR_INVALID_ARG;
    JointSlot *j = resolve_joint(w, jh);
    if (!j) return DAI_ERR_NOT_FOUND;
    uint32_t slot = (jh & 0x00ffffffu) - 1;
    j->destroyed_tick = w->tick;
    despawn_joint(w, slot);
    w->live_joints--;
    Command c; c.tick = w->tick; c.type = CmdType::DestroyJoint; c.slot = slot;
    log_cmd(w, c);
    return DAI_OK;
}

dai_result dai_joint_set_motor(dai_world *w, dai_joint jh, int motor_state, float target) {
    if (!w) return DAI_ERR_INVALID_ARG;
    if (!resolve_joint(w, jh)) return DAI_ERR_NOT_FOUND;
    uint32_t slot = (jh & 0x00ffffffu) - 1;
    w->phys->set_motor(slot, motor_state, target);
    Command c; c.tick = w->tick; c.type = CmdType::Motor; c.slot = slot;
    c.a = dai_vec3{ (float)motor_state, target, 0.0f };
    log_cmd(w, c);
    return DAI_OK;
}

dai_result dai_joint_get(dai_world *w, dai_joint jh, dai_joint_state *out) {
    if (!w || !out) return DAI_ERR_INVALID_ARG;
    if (!resolve_joint(w, jh)) return DAI_ERR_NOT_FOUND;
    uint32_t slot = (jh & 0x00ffffffu) - 1;
    if (!w->phys->get_joint_state(slot, out->position, out->speed)) return DAI_ERR_NOT_FOUND;
    return DAI_OK;
}

int      dai_joint_valid(dai_world *w, dai_joint jh) { return (w && resolve_joint(w, jh)) ? 1 : 0; }
uint32_t dai_joint_count(dai_world *w) { return w ? w->live_joints : 0; }

// ---- queries --------------------------------------------------------------

int dai_raycast(dai_world *w, dai_vec3 from, dai_vec3 dir, float max_distance, dai_ray_hit *out) {
    if (!w || !out) return 0;
    RayHit h;
    if (!w->phys->raycast(from, dir, max_distance, h)) return 0;
    out->distance = h.distance; out->point = h.point; out->normal = h.normal;
    out->body = (h.slot < w->slots.size() && w->slots[h.slot].alive)
              ? pack(h.slot, w->slots[h.slot].generation) : DAI_INVALID_BODY;
    return 1;
}

uint32_t dai_poll_contacts(dai_world *w, dai_contact *out, uint32_t max) {
    if (!w) return 0;
    uint32_t n = w->phys->poll_contacts(nullptr, 0);
    if (!out || max == 0) return n;
    w->contact_scratch.resize(n);
    w->phys->poll_contacts(w->contact_scratch.data(), n);
    uint32_t written = 0;
    for (uint32_t i = 0; i < n && written < max; ++i) {
        const ContactEvent &e = w->contact_scratch[i];
        dai_contact c{};
        c.a = (e.slot_a < w->slots.size() && w->slots[e.slot_a].alive)
            ? pack(e.slot_a, w->slots[e.slot_a].generation) : DAI_INVALID_BODY;
        c.b = (e.slot_b < w->slots.size() && w->slots[e.slot_b].alive)
            ? pack(e.slot_b, w->slots[e.slot_b].generation) : DAI_INVALID_BODY;
        c.point = e.point; c.normal = e.normal; c.impulse = e.impulse;
        out[written++] = c;
    }
    return n;
}

// ---- input ----------------------------------------------------------------

dai_result dai_set_input(dai_world *w, uint32_t player, dai_tick tick, const dai_input *in) {
    if (!w || !in || player >= DAI_MAX_PLAYERS) return DAI_ERR_INVALID_ARG;
    size_t idx = (size_t)(tick % w->input_ring) * DAI_MAX_PLAYERS + player;
    w->inputs[idx].in = *in; w->inputs[idx].set = true; w->inputs[idx].tick = tick;
    return DAI_OK;
}

dai_result dai_get_input(dai_world *w, uint32_t player, dai_tick tick, dai_input *out) {
    if (!w || !out || player >= DAI_MAX_PLAYERS) return DAI_ERR_INVALID_ARG;
    size_t idx = (size_t)(tick % w->input_ring) * DAI_MAX_PLAYERS + player;
    if (!w->inputs[idx].set || w->inputs[idx].tick != tick) { *out = dai_input{}; return DAI_ERR_NOT_FOUND; }
    *out = w->inputs[idx].in;
    return DAI_OK;
}

// ---- stepping -------------------------------------------------------------

dai_result dai_step(dai_world *w) {
    if (!w) return DAI_ERR_INVALID_ARG;
    auto t0 = std::chrono::high_resolution_clock::now();

    // During a replay the host is not running, so this tick's commands have to
    // come from the log. In normal operation the host already applied them.
    if (w->replaying) apply_cmds_for_tick(w, w->tick);

    if (w->tick_fn) {
        w->in_callback = true;
        w->tick_fn(w, w->tick, w->tick_usr);
        w->in_callback = false;
    }

    w->phys->step(w->dt);

    for (uint32_t i = 0; i < w->slots.size(); ++i)
        if (w->slots[i].alive) refresh_transform(w, i);

    w->tick++;
    save_snapshot(w);
    trim_log(w);

    w->last_ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();
    w->sum_ms += w->last_ms;
    if (w->replaying) w->ticks_resim++; else w->ticks_sim++;
    return DAI_OK;
}

uint32_t dai_advance(dai_world *w, double real_seconds, float *out_alpha) {
    if (!w) return 0;
    w->accumulator += real_seconds;
    const double step = w->dt_d;
    uint32_t n = 0;
    const uint32_t max_catch_up = 8;    // a hitch must not become a death spiral
    while (w->accumulator >= step && n < max_catch_up) { dai_step(w); w->accumulator -= step; n++; }
    if (w->accumulator >= step) w->accumulator = 0.0;
    if (out_alpha) *out_alpha = (float)(w->accumulator / step);
    return n;
}

void dai_set_tick_callback(dai_world *w, dai_tick_fn fn, void *user) {
    if (!w) return;
    w->tick_fn = fn; w->tick_usr = user;
}

uint32_t dai_random(dai_world *w)       { return w ? w->rng.next() : 0; }
float    dai_random_float(dai_world *w) { return w ? w->rng.next_float() : 0.0f; }

dai_tick dai_current_tick(const dai_world *w) { return w ? w->tick : 0; }
double   dai_tick_seconds(const dai_world *w) { return w ? (double)w->dt : 0.0; }

uint64_t dai_checksum(dai_world *w) {
    if (!w) return 0;
    Checksum h;
    h.val(w->tick);
    h.val(w->rng.state); h.val(w->rng.inc);
    for (uint32_t i = 0; i < w->slots.size(); ++i) {
        const BodySlot &s = w->slots[i];
        if (!s.alive) continue;
        dai_vec3 p, lv, av; dai_quat q;
        w->phys->get_transform(i, p, q);
        w->phys->get_velocity(i, lv, av);
        h.val(i);
        h.f32(p.x); h.f32(p.y); h.f32(p.z);
        h.f32(q.x); h.f32(q.y); h.f32(q.z); h.f32(q.w);
        h.f32(lv.x); h.f32(lv.y); h.f32(lv.z);
        h.f32(av.x); h.f32(av.y); h.f32(av.z);
        uint8_t sl = w->phys->is_sliding(i) ? 1 : 0; h.val(sl);
    }
    for (uint32_t i = 0; i < w->jslots.size(); ++i) {
        if (!w->jslots[i].alive) continue;
        float pos = 0.0f, spd = 0.0f;
        w->phys->get_joint_state(i, pos, spd);
        h.val(i); h.f32(pos); h.f32(spd);
    }
    return h.h;
}

// ---- rollback -------------------------------------------------------------

dai_tick dai_oldest_snapshot(const dai_world *w) {
    if (!w) return 0;
    return w->tick > w->snap_ring - 1 ? w->tick - (w->snap_ring - 1) : 0;
}

dai_result dai_rollback_to(dai_world *w, dai_tick target) {
    if (!w) return DAI_ERR_INVALID_ARG;
    if (target > w->tick) return DAI_ERR_INVALID_ARG;
    if (target == w->tick) return DAI_OK;
    if (target < dai_oldest_snapshot(w)) { set_err(w, "rollback target outside the snapshot ring"); return DAI_ERR_TOO_OLD; }

    Snapshot &s = w->snaps[target % w->snap_ring];
    if (!s.valid || s.tick != target) { set_err(w, "snapshot slot was overwritten"); return DAI_ERR_TOO_OLD; }

    const dai_tick resume_at = w->tick;

    // 1a. joints created inside the window go first - a constraint must never
    //     outlive a body it references.
    for (uint32_t i = 0; i < w->jslots.size(); ++i)
        if (w->jslots[i].alive && w->jslots[i].created_tick >= target) { despawn_joint(w, i); w->live_joints--; }

    // 1b. restore the SET of bodies to what it was at the start of `target`.
    //    Undo destroys first, then undo creates, so a body that was created
    //    and destroyed inside the window ends up gone.
    for (uint32_t i = 0; i < w->slots.size(); ++i) {
        BodySlot &sl = w->slots[i];
        if (!sl.alive && sl.destroyed_tick != UINT64_MAX && sl.destroyed_tick >= target && sl.created_tick < target) {
            sl.destroyed_tick = UINT64_MAX;
            sl.generation--;                  // undo the bump from despawn
            if (spawn(w, i)) w->live_bodies++;
        }
    }
    for (uint32_t i = 0; i < w->slots.size(); ++i) {
        BodySlot &sl = w->slots[i];
        if (sl.alive && sl.created_tick >= target) { despawn(w, i); w->live_bodies--; }
    }

    // 1c. joints destroyed inside the window come back, now that their bodies exist
    for (uint32_t i = 0; i < w->jslots.size(); ++i) {
        JointSlot &j = w->jslots[i];
        if (!j.alive && j.destroyed_tick != UINT64_MAX && j.destroyed_tick >= target && j.created_tick < target) {
            j.destroyed_tick = UINT64_MAX;
            j.generation--;
            if (spawn_joint(w, i)) w->live_joints++;
        }
    }

    // 2. restore the backend state
    if (!w->phys->restore_state(s.physics)) {
        set_err(w, "backend restore_state failed (body set mismatch)");
        return DAI_ERR_STATE;
    }
    w->rng  = s.rng;
    w->tick = target;

    for (uint32_t i = 0; i < w->slots.size(); ++i)
        if (w->slots[i].alive) {
            w->phys->get_transform(i, w->slots[i].cur_pos, w->slots[i].cur_rot);
            w->slots[i].prev_pos = w->slots[i].cur_pos;
            w->slots[i].prev_rot = w->slots[i].cur_rot;
        }

    // 3. sounds from ticks that never happened must not be heard
    w->audio.erase(std::remove_if(w->audio.begin(), w->audio.end(),
        [&](const PendingAudio &p) { return !p.played && p.ev.tick >= target; }), w->audio.end());

    // 4. re-simulate
    w->rollbacks++;
    w->replaying = true;
    while (w->tick < resume_at) dai_step(w);
    w->replaying = false;
    return DAI_OK;
}

int dai_apply_remote_input(dai_world *w, uint32_t player, dai_tick tick, const dai_input *in) {
    if (!w || !in || player >= DAI_MAX_PLAYERS) return -1;
    dai_input predicted{};                    // an unset tick predicts "no input"
    dai_get_input(w, player, tick, &predicted);
    dai_set_input(w, player, tick, in);
    if (tick >= w->tick) return 0;
    if (std::memcmp(&predicted, in, sizeof(dai_input)) == 0) return 0;

    dai_tick resume = w->tick;
    if (dai_rollback_to(w, tick) != DAI_OK) return -1;
    return (int)(resume - tick);
}

// ---- presentation ---------------------------------------------------------

uint32_t dai_get_transforms(dai_world *w, dai_transform *out, uint32_t max, float alpha) {
    if (!w || !out) return 0;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->slots.size() && n < max; ++i) {
        const BodySlot &s = w->slots[i];
        if (!s.alive) continue;
        out[n].body = pack(i, s.generation);
        out[n].position = dai_vec3{
            s.prev_pos.x + (s.cur_pos.x - s.prev_pos.x) * alpha,
            s.prev_pos.y + (s.cur_pos.y - s.prev_pos.y) * alpha,
            s.prev_pos.z + (s.cur_pos.z - s.prev_pos.z) * alpha };
        // nlerp, shortest arc. Good enough for a frame of visual smoothing and
        // it never touches the simulation.
        float d = s.prev_rot.x * s.cur_rot.x + s.prev_rot.y * s.cur_rot.y
                + s.prev_rot.z * s.cur_rot.z + s.prev_rot.w * s.cur_rot.w;
        float sg = d < 0.0f ? -1.0f : 1.0f;
        dai_quat q{
            s.prev_rot.x + (s.cur_rot.x * sg - s.prev_rot.x) * alpha,
            s.prev_rot.y + (s.cur_rot.y * sg - s.prev_rot.y) * alpha,
            s.prev_rot.z + (s.cur_rot.z * sg - s.prev_rot.z) * alpha,
            s.prev_rot.w + (s.cur_rot.w * sg - s.prev_rot.w) * alpha };
        float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (l > 1e-8f) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
        out[n].rotation = q;
        out[n].user_data = s.desc.user_data;
        n++;
    }
    return n;
}

dai_result dai_play(dai_world *w, const char *event, dai_vec3 pos, int is_3d) {
    if (!w || !event) return DAI_ERR_INVALID_ARG;
    PendingAudio p;
    p.ev.tick = w->tick;
    std::snprintf(p.ev.name, sizeof(p.ev.name), "%s", event);
    p.ev.position = pos; p.ev.volume = 1.0f; p.ev.pitch = 1.0f; p.ev.is_3d = is_3d;
    if (w->audio.size() < 4096) w->audio.push_back(p);
    return DAI_OK;
}

uint32_t dai_poll_audio(dai_world *w, dai_audio_event *out, uint32_t max) {
    if (!w) return 0;
    uint32_t n = 0;
    for (PendingAudio &p : w->audio) {
        if (p.played) continue;
        if (out && n < max) { out[n] = p.ev; p.played = true; }   // out == NULL is a peek
        n++;
    }
    w->audio.erase(std::remove_if(w->audio.begin(), w->audio.end(),
        [](const PendingAudio &p) { return p.played; }), w->audio.end());
    return n;
}

void dai_set_listener(dai_world *w, dai_vec3 pos, dai_vec3 fwd, dai_vec3 up, dai_vec3 vel) {
    if (w && w->audio_be) dai_audio_listener(w->audio_be, pos, fwd, up, vel);
}

void dai_present(dai_world *w) {
    if (!w || !w->audio_be) return;
    for (PendingAudio &p : w->audio) {
        if (p.played) continue;
        dai_audio_play(w->audio_be, &p.ev);
        p.played = true;
    }
    w->audio.erase(std::remove_if(w->audio.begin(), w->audio.end(),
        [](const PendingAudio &p) { return p.played; }), w->audio.end());
    dai_audio_update(w->audio_be);
}

uint32_t dai_render_audio(dai_world *w, float *out_stereo, uint32_t frames) {
    if (!w || !w->audio_be || !out_stereo) return 0;
    return dai_audio_render(w->audio_be, out_stereo, frames);
}

void dai_get_stats(dai_world *w, dai_stats *out) {
    if (!w || !out) return;
    out->bodies            = w->live_bodies;
    out->active_bodies     = w->phys->active_bodies();
    out->ticks_simulated   = w->ticks_sim;
    out->ticks_resimulated = w->ticks_resim;
    out->rollbacks         = w->rollbacks;
    out->last_step_ms      = w->last_ms;
    out->avg_step_ms       = (w->ticks_sim + w->ticks_resim)
                           ? w->sum_ms / (double)(w->ticks_sim + w->ticks_resim) : 0.0;
    out->audio_voices      = w->audio_be ? dai_audio_voices(w->audio_be) : 0;
}

} // extern "C"
