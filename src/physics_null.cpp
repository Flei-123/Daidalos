// Daidalos - null physics backend.
//
// Gravity, velocity integration, a floor plane at y = 0 and nothing else. No
// collision detection between bodies, no constraints, no solver.
//
// Its job is not to be useful. Its job is to fail loudly if a single Jolt
// concept ever leaks out of physics_jolt.cpp: the engine, the tests and the
// renderer must build and run against this file. Keep it that way.

#include "dai_physics.hpp"
#include <cstring>
#include <cmath>

namespace dai {
namespace {

struct NSlot {
    bool     alive = false;
    bool     dynamic = false;
    dai_vec3 pos{}, vel{}, ang{};
    dai_quat rot{ 0, 0, 0, 1 };
    dai_vec3 half{ 0.5f, 0.5f, 0.5f };
    float    restitution = 0.0f;
    float    damping = 0.0f;
};

class NullBackend final : public IPhysicsBackend {
public:
    const char *name() const override { return "null"; }

    bool init(const dai_config &cfg, char *, size_t) override {
        slots.resize(cfg.max_bodies ? cfg.max_bodies : 8192);
        gravity = dai_vec3{ 0.0f, -9.81f, 0.0f };
        return true;
    }

    bool create_body(uint32_t slot, const dai_body_desc &d, const std::vector<dai_compound_part> &) override {
        if (slot >= slots.size()) return false;
        NSlot &s = slots[slot];
        s = NSlot{};
        s.alive = true;
        s.dynamic = (d.motion == DAI_DYNAMIC);
        s.pos = d.position;
        s.rot = d.rotation;
        s.vel = d.linear_velocity;
        s.ang = d.angular_velocity;
        s.half = d.half_extent;
        s.restitution = d.restitution;
        s.damping = d.linear_damping;
        return true;
    }
    void destroy_body(uint32_t slot) override { if (slot < slots.size()) slots[slot].alive = false; }

    void add_impulse(uint32_t slot, dai_vec3 i) override {
        if (slot >= slots.size() || !slots[slot].dynamic) return;
        NSlot &s = slots[slot];
        s.vel.x += i.x; s.vel.y += i.y; s.vel.z += i.z;   // unit mass, it is a stub
    }
    void set_velocity(uint32_t slot, dai_vec3 l, dai_vec3 a) override {
        if (slot >= slots.size()) return;
        slots[slot].vel = l; slots[slot].ang = a;
    }
    void set_gravity(dai_vec3 g) override { gravity = g; }

    void step(float dt) override {
        contacts.clear();
        for (uint32_t i = 0; i < slots.size(); ++i) {
            NSlot &s = slots[i];
            if (!s.alive || !s.dynamic) continue;
            s.vel.x += gravity.x * dt; s.vel.y += gravity.y * dt; s.vel.z += gravity.z * dt;
            if (s.damping > 0.0f) {
                float k = 1.0f - s.damping * dt; if (k < 0.0f) k = 0.0f;
                s.vel.x *= k; s.vel.y *= k; s.vel.z *= k;
            }
            s.pos.x += s.vel.x * dt; s.pos.y += s.vel.y * dt; s.pos.z += s.vel.z * dt;
            // one implicit floor at y = 0 so bodies do not fall forever
            float bottom = s.pos.y - (s.half.y > 0 ? s.half.y : 0.0f);
            if (bottom < 0.0f) {
                s.pos.y -= bottom;
                if (s.vel.y < 0.0f) {
                    ContactEvent e;
                    e.slot_a = i; e.slot_b = UINT32_MAX;
                    e.point = dai_vec3{ s.pos.x, 0.0f, s.pos.z };
                    e.normal = dai_vec3{ 0.0f, 1.0f, 0.0f };
                    e.impulse = -s.vel.y;
                    if (contacts.size() < 8192) contacts.push_back(e);
                    s.vel.y = -s.vel.y * s.restitution;
                    s.vel.x *= 0.7f; s.vel.z *= 0.7f;
                }
            }
        }
    }

    void get_transform(uint32_t slot, dai_vec3 &p, dai_quat &r) const override {
        if (slot >= slots.size() || !slots[slot].alive) { p = {}; r = { 0,0,0,1 }; return; }
        p = slots[slot].pos; r = slots[slot].rot;
    }
    void get_velocity(uint32_t slot, dai_vec3 &l, dai_vec3 &a) const override {
        if (slot >= slots.size() || !slots[slot].alive) { l = {}; a = {}; return; }
        l = slots[slot].vel; a = slots[slot].ang;
    }
    bool is_sliding(uint32_t) const override { return false; }

    bool raycast(dai_vec3 from, dai_vec3 dir, float max_distance, RayHit &out) const override {
        // Only the implicit floor. Enough to keep gameplay code compiling and
        // to prove the query path is backend agnostic.
        if (dir.y >= 0.0f || from.y <= 0.0f) return false;
        float t = from.y / -dir.y;
        if (t > max_distance) return false;
        out.slot = UINT32_MAX - 1;
        out.distance = t;
        out.point = dai_vec3{ from.x + dir.x * t, 0.0f, from.z + dir.z * t };
        out.normal = dai_vec3{ 0, 1, 0 };
        return true;
    }

    uint32_t poll_contacts(ContactEvent *out, uint32_t max) override {
        uint32_t n = (uint32_t)contacts.size();
        if (out) for (uint32_t i = 0; i < n && i < max; ++i) out[i] = contacts[i];
        return n;
    }

    bool save_state(std::string &out) const override {
        out.assign((const char *)slots.data(), slots.size() * sizeof(NSlot));
        return true;
    }
    bool restore_state(const std::string &in) override {
        if (in.size() != slots.size() * sizeof(NSlot)) return false;
        std::memcpy(slots.data(), in.data(), in.size());
        return true;
    }
    uint32_t active_bodies() const override {
        uint32_t n = 0;
        for (const NSlot &s : slots) if (s.alive && s.dynamic) n++;
        return n;
    }

private:
    std::vector<NSlot> slots;
    std::vector<ContactEvent> contacts;
    dai_vec3 gravity{};
};

} // namespace

IPhysicsBackend *create_null_backend() { return new NullBackend(); }

} // namespace dai
