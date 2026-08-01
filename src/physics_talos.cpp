// Daidalos - Talos physics backend. THE ONLY FILE IN THE ENGINE THAT INCLUDES
// talos.h. Same rule as physics_jolt.cpp: if a tal_* type ever appears outside
// this file, dai_physics.hpp has leaked.
//
// Talos speaks a C API (TalC/talos.h), so this is a thinner layer than the Jolt
// one - no layer interfaces, no job system, no ref counted shape settings. What
// it is NOT is a copy of the Jolt backend with the names changed; three things
// genuinely work differently and are handled here rather than pretended away:
//
//  1. Body ids. Jolt lets us create a body WITH a chosen id, so the engine's
//     slot index and the backend id are the same number. Talos hands out its
//     own ids, so this file keeps both directions of the mapping: slot -> id in
//     the slot table, id -> slot in the body's user_data. Reproducibility still
//     holds, because two peers that issue the same create calls in the same
//     order get the same ids out of Talos.
//
//  2. Contact impulse. Jolt calls a listener DURING the step, so the incoming
//     velocities are still there to compute the impact from. Talos reports its
//     contacts AFTER the step, by which time the solver has already changed
//     them. So the velocities are cached before the step and the same textbook
//     formula is applied afterwards - which measures the same instant Jolt's
//     listener does, rather than a post solve number that reads near zero for
//     every impact.
//
//  3. Stiction. The Jolt backend swaps the friction coefficient inside the
//     contact callback. Talos has no such hook, so the coefficient is written
//     onto the body itself when it starts or stops sliding. The effect is the
//     same one tick later, which is what "static friction until it breaks
//     loose" means anyway.

#include "dai_physics.hpp"

#include "talos.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace dai {
namespace {

inline tal_vec3 T(const dai_vec3 &v) { return tal_vec3{ v.x, v.y, v.z }; }
inline dai_vec3 D(const tal_vec3 &v) { return dai_vec3{ v.x, v.y, v.z }; }
inline tal_quat TQ(const dai_quat &q) {
    float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (n < 1e-8f) return tal_quat{ 0, 0, 0, 1 };
    float inv = 1.0f / std::sqrt(n);
    return tal_quat{ q.x*inv, q.y*inv, q.z*inv, q.w*inv };
}
inline dai_quat DQ(const tal_quat &q) { return dai_quat{ q.x, q.y, q.z, q.w }; }

inline dai_vec3 cross(const dai_vec3 &a, const dai_vec3 &b) {
    return dai_vec3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
inline float dot(const dai_vec3 &a, const dai_vec3 &b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline dai_vec3 sub(const dai_vec3 &a, const dai_vec3 &b) { return dai_vec3{ a.x-b.x, a.y-b.y, a.z-b.z }; }
inline float length(const dai_vec3 &a) { return std::sqrt(dot(a, a)); }
inline dai_vec3 normalize(const dai_vec3 &a, const dai_vec3 &fallback) {
    float l = length(a);
    if (l < 1e-6f) return fallback;
    return dai_vec3{ a.x/l, a.y/l, a.z/l };
}
inline dai_vec3 scale(const dai_vec3 &a, float s) { return dai_vec3{ a.x*s, a.y*s, a.z*s }; }

// Any unit vector perpendicular to n. Picking the smallest component to cross
// against keeps it well conditioned for every input.
inline dai_vec3 perpendicular(const dai_vec3 &n) {
    dai_vec3 a = (std::fabs(n.x) <= std::fabs(n.y) && std::fabs(n.x) <= std::fabs(n.z))
               ? dai_vec3{ 1, 0, 0 }
               : (std::fabs(n.y) <= std::fabs(n.z) ? dai_vec3{ 0, 1, 0 } : dai_vec3{ 0, 0, 1 });
    return normalize(cross(n, a), dai_vec3{ 0, 1, 0 });
}

inline dai_quat q_mul(const dai_quat &a, const dai_quat &b) {
    return dai_quat{ a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                     a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                     a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                     a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
inline dai_quat q_conj(const dai_quat &q) { return dai_quat{ -q.x, -q.y, -q.z, q.w }; }
inline dai_vec3 q_rot(const dai_quat &q, const dai_vec3 &v) {
    dai_vec3 u{ q.x, q.y, q.z };
    dai_vec3 t = scale(cross(u, v), 2.0f);
    dai_vec3 r{ v.x + q.w*t.x, v.y + q.w*t.y, v.z + q.w*t.z };
    dai_vec3 c = cross(u, t);
    return dai_vec3{ r.x + c.x, r.y + c.y, r.z + c.z };
}

struct Slot {
    bool        alive = false;
    tal_body_id id = TAL_INVALID_BODY_ID;
    float       mu_static = 0.6f, mu_kinetic = 0.6f;
    bool        sliding = false;
    bool        dynamic = false;
    // Pre step state, for the contact impulse. See note 2 at the top.
    dai_vec3 prev_pos{}, prev_lin{}, prev_ang{};
    float    inv_mass = 0.0f;
    float    restitution = 0.0f;
};

struct JointSlot {
    bool            alive = false;
    int             type = 0;
    tal_constraint *c = nullptr;
    uint32_t        slot_a = UINT32_MAX, slot_b = UINT32_MAX;
    // Reference frame captured at creation, so "current angle" and "current
    // position" mean the same thing they do in the Jolt backend: zero at the
    // pose the joint was built in.
    dai_vec3        axis_local1{ 0, 1, 0 };
    dai_quat        rel_ref{ 0, 0, 0, 1 };   // conj(q1) * q2 at creation
    float           slide_ref = 0.0f;        // dot(p2 - p1, axis) at creation
    tal_motor_settings motor{};
};

class TalosBackend final : public IPhysicsBackend {
public:
    ~TalosBackend() override {
        for (uint32_t i = 0; i < (uint32_t)joints.size(); ++i) if (joints[i].alive) destroy_joint(i);
        for (uint32_t i = 0; i < (uint32_t)slots.size(); ++i) if (slots[i].alive) destroy_body(i);
        if (world && anchor != TAL_INVALID_BODY_ID) tal_body_destroy(world, anchor);
        if (world) tal_world_destroy(world);
    }

    const char *name() const override { return "talos"; }

    bool init(const dai_config &cfg, char *err, size_t err_len) override {
        if (!tal_init(TAL_VERSION)) {
            std::snprintf(err, err_len, "talos version mismatch: header %s, library %s",
                          "0.3.0", tal_get_version_string() ? tal_get_version_string() : "?");
            return false;
        }
        tal_world_settings ws{};
        tal_world_settings_init(&ws);
        ws.max_bodies = cfg.max_bodies ? cfg.max_bodies : 8192;
        // dai_config semantics: 0 = hardware_concurrency - 1, 1 = single
        // threaded. Talos counts WORKER threads, where 0 means "do it on the
        // calling thread".
        if (cfg.physics_threads == 0) {
            unsigned hw = std::thread::hardware_concurrency();
            ws.num_threads = hw > 1 ? hw - 1 : 0;
        } else {
            ws.num_threads = cfg.physics_threads - 1;
        }
        ws.num_position_steps = cfg.position_steps;
        // cfg.velocity_steps has no counterpart: Talos fixes the velocity
        // iterations in its solver settings. Saying so beats silently
        // accepting a number that does nothing.
        ws.gravity = tal_vec3{ 0.0f, -9.81f, 0.0f };
        // Both kinds: "it just hit" AND "something is resting on me". The
        // engine's contact API promises the second one - Jolt reports added
        // and persisted contacts, so this backend has to as well.
        ws.collect_contact_events = TAL_CONTACTS_ALL;
        world = tal_world_create(&ws);
        if (!world) {
            const char *e = tal_get_last_error();
            std::snprintf(err, err_len, "tal_world_create failed: %s", e && *e ? e : "unknown");
            return false;
        }
        slots.resize(ws.max_bodies);
        joints.resize(512);
        by_id.clear();

        // A joint anchored to the world. Jolt has a built in fixed-to-world
        // body; Talos does not, and its own error message says what to do
        // instead: a static sensor. It is created HERE rather than on first
        // use, so every world - on every peer - hands out the same body ids in
        // the same order no matter which joints the game happens to build.
        // Sensor, so it collides with nothing: it is a frame of reference, not
        // an obstacle. Its contact events carry no slot and are filtered out.
        {
            tal_shape *dot = tal_shape_box(tal_vec3{ 0.01f, 0.01f, 0.01f }, 0.0f);
            if (dot) {
                tal_body_settings bs{};
                tal_body_settings_init(&bs, dot, tal_vec3{ 0, 0, 0 }, TAL_MOTION_STATIC, 0);
                bs.is_sensor = 1;
                bs.user_data = (uint64_t)UINT32_MAX;      // belongs to no slot
                anchor = tal_body_create(world, &bs, 0);
                tal_shape_release(dot);
            }
        }
        return true;
    }

    // ------------------------------------------------------------- bodies

    bool create_body(uint32_t slot, const dai_body_desc &d,
                     const std::vector<dai_compound_part> &parts) override {
        if (!world || slot >= slots.size() || slots[slot].alive) return false;
        tal_shape *shape = make_shape(d, parts);
        if (!shape) return false;

        uint32_t mt = d.motion == DAI_DYNAMIC ? TAL_MOTION_DYNAMIC
                    : d.motion == DAI_KINEMATIC ? TAL_MOTION_KINEMATIC : TAL_MOTION_STATIC;
        uint32_t layer = (mt == TAL_MOTION_STATIC) ? 0u : 1u;

        tal_body_settings bs{};
        tal_body_settings_init(&bs, shape, T(d.position), mt, layer);
        bs.rotation         = TQ(d.rotation);
        bs.linear_velocity  = T(d.linear_velocity);
        bs.angular_velocity = T(d.angular_velocity);
        bs.friction         = d.friction_static;
        bs.restitution      = d.restitution;
        bs.linear_damping   = d.linear_damping;
        bs.angular_damping  = d.angular_damping;
        bs.allow_sleeping   = d.no_sleeping ? 0 : 1;
        bs.is_sensor        = d.sensor != 0;   // reports overlaps, blocks nothing
        bs.user_data        = slot;
        if (d.density > 0.0f && mt == TAL_MOTION_DYNAMIC) {
            float m = tal_shape_get_mass(shape, d.density);
            if (m > 0.0f) bs.override_mass = m;
        }

        tal_body_id id = tal_body_create(world, &bs, mt == TAL_MOTION_STATIC ? 0 : 1);
        // The body took its own reference; ours is done either way.
        tal_shape_release(shape);
        if (id == TAL_INVALID_BODY_ID) return false;

        Slot &s = slots[slot];
        s.alive = true; s.id = id;
        s.mu_static = d.friction_static;
        s.mu_kinetic = d.friction_kinetic > 0.0f ? d.friction_kinetic : d.friction_static;
        s.sliding = false;
        s.dynamic = (mt == TAL_MOTION_DYNAMIC);
        s.restitution = d.restitution;
        s.inv_mass = 0.0f;
        if (s.dynamic) {
            float m = tal_body_get_mass(world, id);
            s.inv_mass = m > 0.0f ? 1.0f / m : 0.0f;
        }
        s.prev_pos = d.position;
        s.prev_lin = d.linear_velocity;
        s.prev_ang = d.angular_velocity;
        remember(id, slot);
        return true;
    }

    void destroy_body(uint32_t slot) override {
        if (!world || slot >= slots.size() || !slots[slot].alive) return;
        forget(slots[slot].id);
        tal_body_destroy(world, slots[slot].id);
        slots[slot].alive = false;
        slots[slot].id = TAL_INVALID_BODY_ID;
    }

    void add_impulse(uint32_t slot, dai_vec3 imp) override {
        if (live(slot)) tal_body_add_impulse(world, slots[slot].id, T(imp));
    }
    void set_velocity(uint32_t slot, dai_vec3 lin, dai_vec3 ang) override {
        if (!live(slot)) return;
        tal_body_set_linear_velocity(world, slots[slot].id, T(lin));
        tal_body_set_angular_velocity(world, slots[slot].id, T(ang));
    }
    void set_gravity(dai_vec3 g) override { if (world) tal_world_set_gravity(world, T(g)); }

    void step(float dt) override {
        if (!world) return;
        contacts.clear();

        // Snapshot what the solver is about to change. Only dynamic bodies
        // matter: a static one contributes neither velocity nor inverse mass
        // to the impulse.
        for (Slot &s : slots) {
            if (!s.alive || !s.dynamic) continue;
            s.prev_pos = D(tal_body_get_position(world, s.id));
            s.prev_lin = D(tal_body_get_linear_velocity(world, s.id));
            s.prev_ang = D(tal_body_get_angular_velocity(world, s.id));
        }

        tal_world_step(world, dt, 1);

        uint32_t ne = tal_world_get_num_contact_events(world);
        const tal_contact_event *ev = tal_world_get_contact_events(world);
        if (ev && ne) {
            contacts.reserve(ne);
            for (uint32_t i = 0; i < ne && contacts.size() < 8192; ++i) {
                if (ev[i].type == TAL_CONTACT_REMOVED) continue;   // Jolt reports added/persisted
                uint32_t sa = slot_of(ev[i].body1);
                uint32_t sb = slot_of(ev[i].body2);
                if (sa == UINT32_MAX || sb == UINT32_MAX) continue;
                ContactEvent e;
                e.slot_a = sa;
                e.slot_b = sb;
                e.point  = D(ev[i].position);
                e.normal = D(ev[i].normal);
                e.impulse = impact_impulse(slots[sa], slots[sb], e.point, e.normal);
                contacts.push_back(e);
            }
            // The order Talos reports in is its own; downstream must not be
            // able to observe it.
            std::sort(contacts.begin(), contacts.end(), [](const ContactEvent &a, const ContactEvent &b) {
                if (a.slot_a != b.slot_a) return a.slot_a < b.slot_a;
                return a.slot_b < b.slot_b;
            });
        }

        // Stiction: same hysteresis thresholds as the Jolt backend, written
        // onto the body because there is no contact hook to write it into.
        for (Slot &s : slots) {
            if (!s.alive || !s.dynamic || s.mu_static == s.mu_kinetic) continue;
            float v = length(D(tal_body_get_linear_velocity(world, s.id)));
            bool now = s.sliding ? (v > 0.05f) : (v > 0.20f);
            if (now != s.sliding) {
                s.sliding = now;
                tal_body_set_friction(world, s.id, now ? s.mu_kinetic : s.mu_static);
            }
        }
    }

    void get_transform(uint32_t slot, dai_vec3 &p, dai_quat &r) const override {
        if (!live(slot)) { p = {}; r = { 0, 0, 0, 1 }; return; }
        p = D(tal_body_get_position(world, slots[slot].id));
        r = DQ(tal_body_get_rotation(world, slots[slot].id));
    }
    void set_transform(uint32_t slot, dai_vec3 p, dai_quat r) override {
        if (!live(slot)) return;
        // A static body must not be asked to activate - same trap as Jolt.
        int activate = slots[slot].dynamic ? 1 : 0;
        tal_body_set_transform(world, slots[slot].id, T(p), TQ(r), activate);
    }
    void get_velocity(uint32_t slot, dai_vec3 &l, dai_vec3 &a) const override {
        if (!live(slot)) { l = {}; a = {}; return; }
        l = D(tal_body_get_linear_velocity(world, slots[slot].id));
        a = D(tal_body_get_angular_velocity(world, slots[slot].id));
    }
    bool is_sliding(uint32_t slot) const override {
        return live(slot) && slots[slot].sliding;
    }

    bool raycast(dai_vec3 from, dai_vec3 dir, float max_distance, RayHit &out) const override {
        if (!world) return false;
        float l = length(dir);
        if (l < 1e-6f || max_distance <= 0.0f) return false;
        dai_vec3 d = scale(dir, max_distance / l);      // length IS the max distance
        tal_ray_hit hit{};
        if (!tal_query_raycast_closest(world, T(from), T(d), &hit)) return false;
        uint32_t slot = slot_of(hit.body);
        if (slot == UINT32_MAX) return false;
        out.slot     = slot;
        out.distance = hit.fraction * max_distance;
        out.point    = D(hit.position);
        out.normal   = D(hit.normal);
        return true;
    }

    uint32_t poll_contacts(ContactEvent *out, uint32_t max) override {
        uint32_t n = (uint32_t)contacts.size();
        if (out) for (uint32_t i = 0; i < n && i < max; ++i) out[i] = contacts[i];
        return n;
    }

    // ------------------------------------------------------------- joints

    bool create_joint(uint32_t slot, const dai_joint_desc &d, uint32_t sa, uint32_t sb) override {
        if (!world) return false;
        if (slot >= joints.size()) joints.resize(slot + 64);
        if (joints[slot].alive) return false;
        if (sa >= slots.size() || !slots[sa].alive) return false;
        bool anchored = !(sb < slots.size() && slots[sb].alive);
        tal_body_id a = slots[sa].id;
        tal_body_id b = anchored ? anchor : slots[sb].id;
        if (b == TAL_INVALID_BODY_ID) return false;      // no anchor, no world joint

        dai_vec3 axis = normalize(d.axis, dai_vec3{ 0, 1, 0 });
        dai_vec3 normal = d.normal_axis;
        if (length(normal) < 1e-6f || std::fabs(dot(normalize(normal, axis), axis)) > 0.99f) {
            normal = perpendicular(axis);
        } else {
            normal = normalize(sub(normal, scale(axis, dot(normal, axis))), perpendicular(axis));
        }

        uint32_t type = d.type == DAI_JOINT_HINGE    ? TAL_CONSTRAINT_HINGE
                      : d.type == DAI_JOINT_SLIDER   ? TAL_CONSTRAINT_SLIDER
                      : d.type == DAI_JOINT_POINT    ? TAL_CONSTRAINT_POINT
                      : d.type == DAI_JOINT_DISTANCE ? TAL_CONSTRAINT_DISTANCE
                                                     : TAL_CONSTRAINT_FIXED;
        tal_constraint_settings cs{};
        tal_constraint_settings_init(&cs, type);
        cs.point1 = cs.point2 = T(d.anchor);
        cs.axis1 = cs.axis2 = T(axis);
        cs.normal1 = cs.normal2 = T(normal);
        cs.max_friction = d.max_friction;
        cs.user_data = d.user_data;
        if (d.type == DAI_JOINT_DISTANCE) {
            // The second anchor rides in normal_axis, exactly like the Jolt
            // backend - the engine's joint desc has one anchor field.
            cs.point2 = T(d.normal_axis);
            cs.min_distance = d.min_distance;
            cs.max_distance = d.max_distance;
        } else if (d.enable_limits) {
            cs.limits_min = d.limit_min;
            cs.limits_max = d.limit_max;
        }
        if (d.max_motor_force > 0.0f) {
            cs.motor.state = 0;                     // built, but off until set_motor
            cs.motor.max_force_limit  =  d.max_motor_force;
            cs.motor.min_force_limit  = -d.max_motor_force;
            cs.motor.max_torque_limit =  d.max_motor_force;
            cs.motor.min_torque_limit = -d.max_motor_force;
        }

        tal_constraint *c = tal_constraint_create(world, a, b, &cs);
        if (!c) return false;

        JointSlot &j = joints[slot];
        j.alive = true;
        j.type = d.type;
        j.c = c;
        j.slot_a = sa;
        j.slot_b = anchored ? UINT32_MAX : sb;
        j.motor = cs.motor;

        dai_quat q1 = DQ(tal_body_get_rotation(world, a));
        dai_quat q2 = anchored ? dai_quat{ 0, 0, 0, 1 } : DQ(tal_body_get_rotation(world, b));
        j.axis_local1 = q_rot(q_conj(q1), axis);
        j.rel_ref = q_mul(q_conj(q1), q2);
        dai_vec3 p1 = D(tal_body_get_position(world, a));
        dai_vec3 p2 = anchored ? d.anchor : D(tal_body_get_position(world, b));
        j.slide_ref = dot(sub(p2, p1), axis);
        return true;
    }

    void destroy_joint(uint32_t slot) override {
        if (!world || slot >= joints.size() || !joints[slot].alive) return;
        tal_constraint_destroy(world, joints[slot].c);
        joints[slot].c = nullptr;
        joints[slot].alive = false;
    }

    void set_motor(uint32_t slot, int state, float target) override {
        if (!world || slot >= joints.size() || !joints[slot].alive) return;
        JointSlot &j = joints[slot];
        if (j.type != DAI_JOINT_HINGE && j.type != DAI_JOINT_SLIDER) return;
        // A motor with no force limit would be built but never move anything;
        // refusing here matches the Jolt backend, which checks IsValid().
        if (state != DAI_MOTOR_OFF && j.motor.max_force_limit <= 0.0f &&
            j.motor.max_torque_limit <= 0.0f) return;
        j.motor.state = state == DAI_MOTOR_VELOCITY ? 1u
                      : state == DAI_MOTOR_POSITION ? 2u : 0u;
        if (state == DAI_MOTOR_VELOCITY) j.motor.target_velocity = target;
        else if (state == DAI_MOTOR_POSITION) j.motor.target_position = target;
        tal_constraint_set_motor(j.c, &j.motor);
    }

    bool get_joint_state(uint32_t slot, float &position, float &speed) const override {
        position = 0.0f; speed = 0.0f;
        if (!world || slot >= joints.size() || !joints[slot].alive) return false;
        const JointSlot &j = joints[slot];
        if (j.type != DAI_JOINT_HINGE && j.type != DAI_JOINT_SLIDER) return true;
        if (j.slot_a >= slots.size() || !slots[j.slot_a].alive) return false;

        tal_body_id a = slots[j.slot_a].id;
        bool anchored = (j.slot_b == UINT32_MAX);
        dai_quat q1 = DQ(tal_body_get_rotation(world, a));
        dai_quat q2 = anchored ? dai_quat{ 0, 0, 0, 1 } : DQ(tal_body_get_rotation(world, slots[j.slot_b].id));
        dai_vec3 axis = q_rot(q1, j.axis_local1);      // the axis turns with body 1

        dai_vec3 v1 = D(tal_body_get_linear_velocity(world, a));
        dai_vec3 w1 = D(tal_body_get_angular_velocity(world, a));
        dai_vec3 v2{}, w2{};
        if (!anchored) {
            v2 = D(tal_body_get_linear_velocity(world, slots[j.slot_b].id));
            w2 = D(tal_body_get_angular_velocity(world, slots[j.slot_b].id));
        }

        if (j.type == DAI_JOINT_HINGE) {
            // How far the relative rotation has turned about the axis since
            // the joint was built. Signed, and continuous through zero.
            dai_quat rel = q_mul(q_conj(q1), q2);
            dai_quat delta = q_mul(rel, q_conj(j.rel_ref));
            float s = dot(dai_vec3{ delta.x, delta.y, delta.z }, j.axis_local1);
            position = 2.0f * std::atan2(s, delta.w);
            if (position > 3.14159265f) position -= 6.28318531f;
            if (position < -3.14159265f) position += 6.28318531f;
            speed = dot(sub(w1, w2), axis);
            return true;
        }
        dai_vec3 p1 = D(tal_body_get_position(world, a));
        dai_vec3 p2 = anchored ? p1 : D(tal_body_get_position(world, slots[j.slot_b].id));
        position = dot(sub(p2, p1), axis) - j.slide_ref;
        speed = dot(sub(v2, v1), axis);
        return true;
    }

    // ------------------------------------------------------------- state

    bool save_state(std::string &out) const override {
        if (!world) return false;
        size_t need = tal_world_save_state(world, nullptr, 0);
        out.assign(need, '\0');
        if (need == 0) return true;
        return tal_world_save_state(world, &out[0], need) == need;
    }
    bool restore_state(const std::string &in) override {
        if (!world) return false;
        if (in.empty()) return false;
        return tal_world_restore_state(world, in.data(), in.size()) != 0;
    }
    uint32_t active_bodies() const override {
        return world ? tal_world_get_num_active_bodies(world) : 0;
    }

private:
    bool live(uint32_t slot) const {
        return world && slot < slots.size() && slots[slot].alive;
    }

    // id -> slot. Talos already stores it in user_data; the table is the fast
    // path for the raycast and the contact loop, which run per hit and per
    // contact rather than once.
    void remember(tal_body_id id, uint32_t slot) {
        if (by_id.size() <= id) by_id.resize((size_t)id + 64, UINT32_MAX);
        by_id[id] = slot;
    }
    void forget(tal_body_id id) {
        if (id < by_id.size()) by_id[id] = UINT32_MAX;
    }
    uint32_t slot_of(tal_body_id id) const {
        if (id == TAL_INVALID_BODY_ID) return UINT32_MAX;
        if (id < by_id.size() && by_id[id] != UINT32_MAX) return by_id[id];
        // Fall back to the body itself: a restore_state can bring back ids the
        // table never saw.
        if (!tal_body_is_valid(world, id)) return UINT32_MAX;
        uint64_t u = tal_body_get_user_data(world, id);
        return u < slots.size() ? (uint32_t)u : UINT32_MAX;
    }

    // See note 2 at the top. Same formula as the Jolt backend, same omission
    // of the inertia terms (so this is an upper bound), evaluated on the
    // velocities from before the step.
    float impact_impulse(const Slot &a, const Slot &b, const dai_vec3 &point,
                         const dai_vec3 &normal) const {
        float inv_mass = a.inv_mass + b.inv_mass;
        if (inv_mass <= 0.0f) return 0.0f;
        auto point_vel = [&](const Slot &s) {
            if (!s.dynamic) return dai_vec3{ 0, 0, 0 };
            dai_vec3 r = sub(point, s.prev_pos);
            dai_vec3 c = cross(s.prev_ang, r);
            return dai_vec3{ s.prev_lin.x + c.x, s.prev_lin.y + c.y, s.prev_lin.z + c.z };
        };
        dai_vec3 vrel = sub(point_vel(a), point_vel(b));
        float closing = dot(vrel, normal);
        if (closing <= 0.0f) return 0.0f;
        float e = std::max(a.restitution, b.restitution);
        return (1.0f + e) * closing / inv_mass;
    }

    static tal_shape *one_shape(int shape, const dai_vec3 &he) {
        switch (shape) {
        case DAI_SHAPE_SPHERE:
            return tal_shape_sphere(he.x > 0 ? he.x : 0.5f);
        case DAI_SHAPE_CAPSULE:
            return tal_shape_capsule(he.y > 0 ? he.y : 0.5f, he.x > 0 ? he.x : 0.5f);
        default: {
            float ex = he.x > 0 ? he.x : 0.5f, ey = he.y > 0 ? he.y : 0.5f, ez = he.z > 0 ? he.z : 0.5f;
            float cr = std::min(std::min(ex, ey), std::min(ez, 0.05f));
            return tal_shape_box(tal_vec3{ ex, ey, ez }, cr);
        }
        }
    }
    static tal_shape *make_shape(const dai_body_desc &d, const std::vector<dai_compound_part> &parts) {
        if (d.shape != DAI_SHAPE_COMPOUND || parts.empty()) return one_shape(d.shape, d.half_extent);
        std::vector<tal_compound_child> kids;
        kids.reserve(parts.size());
        for (const dai_compound_part &p : parts) {
            tal_shape *s = one_shape(p.shape, p.half_extent);
            if (!s) continue;
            tal_compound_child c{};
            c.shape = s;
            c.position = T(p.offset);
            c.rotation = TQ(p.rotation);
            kids.push_back(c);
        }
        tal_shape *compound = kids.empty() ? nullptr
                            : tal_shape_compound(kids.data(), (uint32_t)kids.size());
        // The compound took its own references; drop ours either way, or every
        // merge would leak one shape per part.
        for (tal_compound_child &c : kids) tal_shape_release(c.shape);
        if (compound) return compound;
        return one_shape(DAI_SHAPE_BOX, d.half_extent);
    }

    tal_world *world = nullptr;
    tal_body_id anchor = TAL_INVALID_BODY_ID;   // static sensor, see init()
    std::vector<Slot> slots;
    std::vector<JointSlot> joints;
    std::vector<uint32_t> by_id;
    std::vector<ContactEvent> contacts;
};

} // namespace

IPhysicsBackend *create_talos_backend() { return new TalosBackend(); }

} // namespace dai
